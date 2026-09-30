#include "pch.h"
#include "VirtualShadowMapRenderer.h"

#include "Components.h"
#include "EntityMeshRenderer.h"
#include "VegetationRenderer.h"
#include "VegetationScatter.h"
#include "VirtualGeometryRenderer.h"
#include "System/PteroLog.h"

#include "d3dx12.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <intrin.h>
#include <iterator>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
    ID3D12DescriptorHeap* __stdcall DX12Context_GetSrvDescriptorHeap();
    bool __stdcall DX12Context_AllocateSrvDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
    bool __stdcall DX12Context_WaitForGPU();
}

namespace
{
    using VSM = VirtualShadowMapRenderer;

    constexpr std::uint32_t kValidBit = 0x80000000u;
    constexpr UINT64 kPageTableBytes = UINT64(VSM::kTotalPages) * 4;
    // One request bit per page, rounded up to whole words.
    constexpr std::size_t kRequestWords = (std::size_t(VSM::kTotalPages) + 31) / 32;
    constexpr UINT64 kRequestBytes = UINT64(kRequestWords) * 4;
    constexpr std::size_t kRequestWordsPerLevel = VSM::kPagesPerLevel / 32;
    static_assert(VSM::kDirectionalPages % 32 == 0, "local request bits must start on a word");

    constexpr int kLocalMipOffsets[VSM::kLocalMips] = { 0, 1024, 1280, 1344, 1360, 1364 };

    // Cube faces: forward and up. Must match PteroVsmSelectFace in VirtualShadowMap.hlsli.
    struct CubeFace { XMFLOAT3 Forward; XMFLOAT3 Up; };
    constexpr CubeFace kLocalFaces[6] =
    {
        { {  1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } },
        { { -1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } },
        { { 0.0f,  1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } },
        { { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } },
        { { 0.0f, 0.0f,  1.0f }, { 0.0f, 1.0f, 0.0f } },
        { { 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f, 0.0f } },
    };

    // A page requested within this many frames is in use: it is not evicted, and a
    // dirty one is re-rendered. Covers the readback latency with room to spare.
    constexpr std::uint64_t kInUseFrames = 8;

    // Beyond this distance from the anchor the camera re-anchors the sun's light frame,
    // which drops the sun's cache; within it a small sun rotation keeps the pages (see
    // UpdateLightFrame).
    constexpr float kReanchorDistance = 1000.0f;
    constexpr float kSoftRotationDistance = 64.0f;

    // Sun culling views extend this far toward the sun past the near plane, because
    // casters there are clamped onto it (depth clamp) and still cast.
    constexpr float kCullNearExtension = 100000.0f;

    // A light that moved less than this keeps its pages as they are.
    constexpr float kLightMoveEpsilon = 1e-4f;

    // Always-resident fallbacks (RequestResidentFallbacks): the sun's levels whose pages
    // span at least this many metres, and a light's second-coarsest mip while the camera
    // is within this distance of its reach.
    constexpr float kSunFallbackPageSpan = 16.0f;
    constexpr float kLocalFallbackDistance = 20.0f;

    struct alignas(256) MarkConstants
    {
        XMFLOAT4X4      InvViewProj;
        XMFLOAT3        CameraPos;
        float           Pad0;
        VsmGpuConstants Vsm;
    };
    static_assert(offsetof(MarkConstants, Vsm) == 80);
    constexpr UINT64 kMarkConstantStride = (sizeof(MarkConstants) + 255) & ~UINT64(255);

    // Root parameters of the marking pass.
    enum MarkRoot : UINT { MarkRootConstants = 0, MarkRootDepth, MarkRootNormal, MarkRootRequests, MarkRootCount };

    std::uint64_t HashBytes(std::uint64_t hash, const void* data, std::size_t size)
    {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < size; ++i)
        {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
        return hash;
    }

    template <typename T>
    std::uint64_t HashValue(std::uint64_t hash, const T& value)
    {
        return HashBytes(hash, &value, sizeof(value));
    }

    bool CreateBuffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags,
                      D3D12_RESOURCE_STATES initialState, ComPtr<ID3D12Resource>& out, const wchar_t* name)
    {
        const CD3DX12_HEAP_PROPERTIES heap(heapType);
        const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(size, flags);
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(&out))))
            return false;
        out->SetName(name);
        return true;
    }

    // Light-space xy footprint of a world AABB.
    void ProjectBox(const XMFLOAT3& boxMin, const XMFLOAT3& boxMax, const XMFLOAT3& anchor,
                    const XMFLOAT3& axisX, const XMFLOAT3& axisY,
                    float& outMinX, float& outMinY, float& outMaxX, float& outMaxY)
    {
        // Centre/extent form: the footprint's half-size on an axis is the extent
        // projected with absolute values.
        const XMFLOAT3 centre((boxMin.x + boxMax.x) * 0.5f - anchor.x,
                              (boxMin.y + boxMax.y) * 0.5f - anchor.y,
                              (boxMin.z + boxMax.z) * 0.5f - anchor.z);
        const XMFLOAT3 extent((boxMax.x - boxMin.x) * 0.5f, (boxMax.y - boxMin.y) * 0.5f, (boxMax.z - boxMin.z) * 0.5f);
        const float cx = centre.x * axisX.x + centre.y * axisX.y + centre.z * axisX.z;
        const float cy = centre.x * axisY.x + centre.y * axisY.y + centre.z * axisY.z;
        const float ex = extent.x * std::fabs(axisX.x) + extent.y * std::fabs(axisX.y) + extent.z * std::fabs(axisX.z);
        const float ey = extent.x * std::fabs(axisY.x) + extent.y * std::fabs(axisY.y) + extent.z * std::fabs(axisY.z);
        outMinX = cx - ex; outMaxX = cx + ex;
        outMinY = cy - ey; outMaxY = cy + ey;
    }

    void TransformBox(const XMFLOAT3& localMin, const XMFLOAT3& localMax, const XMMATRIX& world,
                      XMFLOAT3& outMin, XMFLOAT3& outMax)
    {
        XMVECTOR lo = XMVectorReplicate(FLT_MAX);
        XMVECTOR hi = XMVectorReplicate(-FLT_MAX);
        for (int corner = 0; corner < 8; ++corner)
        {
            const XMVECTOR p = XMVector3TransformCoord(XMVectorSet(
                (corner & 1) ? localMax.x : localMin.x,
                (corner & 2) ? localMax.y : localMin.y,
                (corner & 4) ? localMax.z : localMin.z, 1.0f), world);
            lo = XMVectorMin(lo, p);
            hi = XMVectorMax(hi, p);
        }
        XMStoreFloat3(&outMin, lo);
        XMStoreFloat3(&outMax, hi);
    }

    bool SphereIntersectsBox(const XMFLOAT3& centre, float radius, const XMFLOAT3& boxMin, const XMFLOAT3& boxMax)
    {
        const float dx = (std::max)((std::max)(boxMin.x - centre.x, 0.0f), centre.x - boxMax.x);
        const float dy = (std::max)((std::max)(boxMin.y - centre.y, 0.0f), centre.y - boxMax.y);
        const float dz = (std::max)((std::max)(boxMin.z - centre.z, 0.0f), centre.z - boxMax.z);
        return dx * dx + dy * dy + dz * dz <= radius * radius;
    }

    // Whether any part of a world AABB can land inside a view (row-major view-projection).
    bool BoxIntersectsView(const XMFLOAT3& boxMin, const XMFLOAT3& boxMax, const XMMATRIX& viewProjection)
    {
        int outside[6] = {};
        for (int corner = 0; corner < 8; ++corner)
        {
            const XMVECTOR p = XMVector4Transform(XMVectorSet(
                (corner & 1) ? boxMax.x : boxMin.x,
                (corner & 2) ? boxMax.y : boxMin.y,
                (corner & 4) ? boxMax.z : boxMin.z, 1.0f), viewProjection);
            XMFLOAT4 c;
            XMStoreFloat4(&c, p);
            outside[0] += c.x < -c.w;
            outside[1] += c.x > c.w;
            outside[2] += c.y < -c.w;
            outside[3] += c.y > c.w;
            outside[4] += c.z < 0.0f;
            outside[5] += c.z > c.w;
        }
        for (int plane = 0; plane < 6; ++plane)
        {
            if (outside[plane] == 8)
                return false;
        }
        return true;
    }

    int FloorDiv(float value, float divisor)
    {
        return static_cast<int>(std::floor(value / divisor));
    }

    D3D12_BLEND_DESC OpaqueBlend()
    {
        D3D12_BLEND_DESC blend{};
        for (D3D12_RENDER_TARGET_BLEND_DESC& target : blend.RenderTarget)
        {
            target.SrcBlend = D3D12_BLEND_ONE;
            target.DestBlend = D3D12_BLEND_ZERO;
            target.BlendOp = D3D12_BLEND_OP_ADD;
            target.SrcBlendAlpha = D3D12_BLEND_ONE;
            target.DestBlendAlpha = D3D12_BLEND_ZERO;
            target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            target.LogicOp = D3D12_LOGIC_OP_NOOP;
            target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }
        return blend;
    }

    D3D12_RASTERIZER_DESC SolidRasterizer(D3D12_CULL_MODE cull)
    {
        D3D12_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
        rasterizer.CullMode = cull;
        rasterizer.DepthClipEnable = TRUE;
        rasterizer.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
        return rasterizer;
    }
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

bool VirtualShadowMapRenderer::Initialize(const VirtualShadowMapSettings& settings)
{
    mLastError.clear();
    mInitialized = false;

    mPages.assign(std::size_t(kTotalPages), PageEntry{});
    mPageTableCpu.assign(std::size_t(kTotalPages), 0u);

    if (!CreateBuffers() || !CreatePool(settings.PhysicalPages) || !CreatePipelines(settings.SlopeScaledDepthBias))
        return false;

    mInvalidateAllRequested = true;
    mInitialized = true;
    return true;
}

void VirtualShadowMapRenderer::Shutdown()
{
    if (mPageTableUpload && mPageTableUploadMapped) mPageTableUpload->Unmap(0, nullptr);
    if (mRequestsReadback && mRequestsReadbackMapped) mRequestsReadback->Unmap(0, nullptr);
    if (mMarkConstants && mMarkConstantsMapped) mMarkConstants->Unmap(0, nullptr);
    mPageTableUploadMapped = nullptr;
    mRequestsReadbackMapped = nullptr;
    mMarkConstantsMapped = nullptr;

    mPool.Reset();
    mDsvHeap.Reset();
    mPageTable.Reset();
    mPageTableUpload.Reset();
    mRequests.Reset();
    mRequestsZero.Reset();
    mRequestsReadback.Reset();
    mMarkConstants.Reset();
    mMarkRootSignature.Reset();
    mMarkPipeline.Reset();
    mDepthRootSignature.Reset();
    mDepthPipeline.Reset();
    mPipelineSlopeBias = -1.0f;
    // The pool's shared-heap SRV slot is never freed; a later Initialize reuses it.

    mPages.clear();
    mPageTableCpu.clear();
    mFreeTiles.clear();
    mTileOwner.clear();
    mTrackedCasters.clear();
    mMeshBounds.clear();
    mPageViews.clear();
    for (LocalSlot& slot : mLocalSlots)
        slot = LocalSlot{};
    mInitialized = false;
    mActiveThisFrame = false;
}

bool VirtualShadowMapRenderer::CreateBuffers()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "VirtualShadowMap: DX12 device is null.";
        return false;
    }

    const bool ok =
           CreateBuffer(device, kPageTableBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_COMMON, mPageTable, L"VSM_PageTable")
        && CreateBuffer(device, kPageTableBytes * kFramesInFlight, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_GENERIC_READ, mPageTableUpload, L"VSM_PageTableUpload")
        && CreateBuffer(device, kRequestBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COMMON, mRequests, L"VSM_Requests")
        && CreateBuffer(device, kRequestBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_GENERIC_READ, mRequestsZero, L"VSM_RequestsZero")
        && CreateBuffer(device, kRequestBytes * kFramesInFlight, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_COPY_DEST, mRequestsReadback, L"VSM_RequestsReadback")
        && CreateBuffer(device, kMarkConstantStride * kFramesInFlight, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_GENERIC_READ, mMarkConstants, L"VSM_MarkConstants");
    if (!ok)
    {
        mLastError = "VirtualShadowMap: buffer creation failed.";
        return false;
    }

    const D3D12_RANGE noRead{ 0, 0 };
    void* mapped = nullptr;
    if (FAILED(mPageTableUpload->Map(0, &noRead, &mapped)))
        return false;
    mPageTableUploadMapped = static_cast<std::byte*>(mapped);

    if (FAILED(mMarkConstants->Map(0, &noRead, &mapped)))
        return false;
    mMarkConstantsMapped = static_cast<std::byte*>(mapped);

    // Written once, then only ever copied from.
    if (FAILED(mRequestsZero->Map(0, &noRead, &mapped)))
        return false;
    std::memset(mapped, 0, static_cast<std::size_t>(kRequestBytes));
    mRequestsZero->Unmap(0, nullptr);

    // Persistently mapped; each slot is read only after the frame that wrote it retired.
    const D3D12_RANGE readAll{ 0, static_cast<SIZE_T>(kRequestBytes * kFramesInFlight) };
    if (FAILED(mRequestsReadback->Map(0, &readAll, &mapped)))
        return false;
    mRequestsReadbackMapped = static_cast<const std::uint32_t*>(mapped);

    for (ReadbackSlot& slot : mReadbackSlots)
        slot = ReadbackSlot{};
    return true;
}

bool VirtualShadowMapRenderer::CreatePool(int physicalPages)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    const int pages = std::clamp(physicalPages, 256, 4096);
    mPoolPagesX = 64;
    mPoolPagesY = (pages + mPoolPagesX - 1) / mPoolPagesX;
    mPoolPageCount = mPoolPagesX * mPoolPagesY;

    const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = UINT64(mPoolPagesX) * kPageTexels;
    desc.Height = UINT(mPoolPagesY) * kPageTexels;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;

    mPool.Reset();
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&mPool))))
    {
        mLastError = "VirtualShadowMap: failed to create the physical page pool.";
        return false;
    }
    mPool->SetName(L"VSM_PhysicalPool");
    mPoolState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    if (!mDsvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
        dsvHeapDesc.NumDescriptors = 1;
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (FAILED(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&mDsvHeap))))
        {
            mLastError = "VirtualShadowMap: failed to create the DSV heap.";
            return false;
        }
        mPoolDsv = mDsvHeap->GetCPUDescriptorHandleForHeapStart();
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(mPool.Get(), &dsvDesc, mPoolDsv);

    // The shared heap never frees, so the slot is allocated once and rewritten when the
    // pool is rebuilt (always behind a GPU wait).
    if (mPoolSrvGpu.ptr == 0 && !DX12Context_AllocateSrvDescriptor(&mPoolSrvCpu, &mPoolSrvGpu))
    {
        mLastError = "VirtualShadowMap: failed to allocate the pool SRV.";
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(mPool.Get(), &srvDesc, mPoolSrvCpu);

    mFreeTiles.clear();
    mTileOwner.assign(std::size_t(mPoolPageCount), -1);
    for (int tile = mPoolPageCount - 1; tile >= 0; --tile)
        mFreeTiles.push_back(tile);

    PTERO_LOG_INFO("Shadows", "Virtual shadow map pool: %d pages (%d x %d texels, %.0f MB).",
                   mPoolPageCount, mPoolPagesX * int(kPageTexels), mPoolPagesY * int(kPageTexels),
                   double(mPoolPageCount) * kPageTexels * kPageTexels * 4.0 / (1024.0 * 1024.0));
    return true;
}

bool VirtualShadowMapRenderer::CreatePipelines(float slopeScaledDepthBias)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    auto serialize = [&](const D3D12_ROOT_SIGNATURE_DESC& desc, ComPtr<ID3D12RootSignature>& out, const char* what)
    {
        ComPtr<ID3DBlob> blob, errors;
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors))
            || FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&out))))
        {
            mLastError = std::string("VirtualShadowMap: ") + what + " root signature failed.";
            if (errors)
                mLastError += std::string(" ") + static_cast<const char*>(errors->GetBufferPointer());
            return false;
        }
        return true;
    };

    // --- Marking pass --------------------------------------------------------------
    if (!mMarkPipeline)
    {
        const ShaderCompileRequest vsRequest{ L"Shaders\\VirtualShadowMapMark.hlsl", L"VSMain", L"vs_5_0", ShaderStage::Vertex };
        const ShaderCompileRequest psRequest{ L"Shaders\\VirtualShadowMapMark.hlsl", L"PSMain", L"ps_5_0", ShaderStage::Pixel };
        if (!mMarkVertexShader.Compile(vsRequest) || !mMarkPixelShader.Compile(psRequest))
        {
            const char* error = mMarkVertexShader.GetLastErrorMessage() ? mMarkVertexShader.GetLastErrorMessage()
                              : mMarkPixelShader.GetLastErrorMessage();
            mLastError = std::string("VirtualShadowMap: marking shader compile failed: ") + (error ? error : "unknown");
            return false;
        }

        const D3D12_DESCRIPTOR_RANGE depthRange{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        const D3D12_DESCRIPTOR_RANGE normalRange{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0 };
        D3D12_ROOT_PARAMETER params[MarkRootCount]{};
        params[MarkRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[MarkRootConstants].Descriptor.ShaderRegister = 0;
        params[MarkRootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[MarkRootDepth].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[MarkRootDepth].DescriptorTable = { 1, &depthRange };
        params[MarkRootDepth].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[MarkRootNormal].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[MarkRootNormal].DescriptorTable = { 1, &normalRange };
        params[MarkRootNormal].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[MarkRootRequests].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[MarkRootRequests].Descriptor.ShaderRegister = 0;
        params[MarkRootRequests].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.NumParameters = MarkRootCount;
        rootDesc.pParameters = params;
        rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        if (!serialize(rootDesc, mMarkRootSignature, "marking"))
            return false;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = mMarkRootSignature.Get();
        pso.VS = mMarkVertexShader.GetBytecode();
        pso.PS = mMarkPixelShader.GetBytecode();
        pso.BlendState = OpaqueBlend();
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState = SolidRasterizer(D3D12_CULL_MODE_NONE);
        pso.DepthStencilState.DepthEnable = FALSE;
        pso.DepthStencilState.StencilEnable = FALSE;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 0;
        pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
        pso.SampleDesc.Count = 1;
        if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&mMarkPipeline))))
        {
            mLastError = "VirtualShadowMap: marking pipeline creation failed.";
            return false;
        }
    }

    // --- Entity page depth pass ----------------------------------------------------
    if (!mDepthRootSignature)
    {
        if (!mDepthVertexShader.Compile({ L"Shaders\\ShadowDepth.hlsl", L"VSMain", L"vs_5_0", ShaderStage::Vertex }))
        {
            mLastError = std::string("VirtualShadowMap: depth shader compile failed: ")
                + (mDepthVertexShader.GetLastErrorMessage() ? mDepthVertexShader.GetLastErrorMessage() : "unknown");
            return false;
        }

        // ShadowDepth.hlsl's b0 (gLightMVP) as root constants: pages times casters is
        // far more draws than a constant buffer ring wants to hold.
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants.ShaderRegister = 0;
        param.Constants.Num32BitValues = 16;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.NumParameters = 1;
        rootDesc.pParameters = &param;
        rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        if (!serialize(rootDesc, mDepthRootSignature, "depth"))
            return false;
    }

    if (!mDepthPipeline || mPipelineSlopeBias != slopeScaledDepthBias)
    {
        // Same layout and cull mode as ShadowMapRenderer's pipeline, with depth clamping
        // instead of clipping so casters beyond the near plane still cast.
        const D3D12_INPUT_ELEMENT_DESC inputLayout[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = mDepthRootSignature.Get();
        pso.VS = mDepthVertexShader.GetBytecode();
        pso.BlendState = OpaqueBlend();
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState = SolidRasterizer(D3D12_CULL_MODE_BACK);
        pso.RasterizerState.SlopeScaledDepthBias = (std::max)(slopeScaledDepthBias, 0.0f);
        pso.RasterizerState.DepthClipEnable = FALSE;
        pso.DepthStencilState.DepthEnable = TRUE;
        pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        pso.DepthStencilState.StencilEnable = FALSE;
        pso.InputLayout = { inputLayout, static_cast<UINT>(std::size(inputLayout)) };
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 0;
        pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pso.SampleDesc.Count = 1;

        ComPtr<ID3D12PipelineState> pipeline;
        if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline))))
        {
            mLastError = "VirtualShadowMap: depth pipeline creation failed.";
            return false;
        }
        // Only ever replaced behind a GPU wait (see BeginFrame), since recorded frames
        // still name the old one.
        mDepthPipeline = pipeline;
        mPipelineSlopeBias = slopeScaledDepthBias;
    }
    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS VirtualShadowMapRenderer::GetPageTableAddress() const
{
    return mPageTable ? mPageTable->GetGPUVirtualAddress() : 0;
}

// ---------------------------------------------------------------------------
// Page bookkeeping
// ---------------------------------------------------------------------------

float VirtualShadowMapRenderer::TexelSize(int level) const
{
    return mFirstTexel * static_cast<float>(1u << level);
}

XMFLOAT3 VirtualShadowMapRenderer::ToLightSpace(const XMFLOAT3& world) const
{
    const XMFLOAT3 r(world.x - mAnchor.x, world.y - mAnchor.y, world.z - mAnchor.z);
    return XMFLOAT3(
        r.x * mAxisX.x + r.y * mAxisX.y + r.z * mAxisX.z,
        r.x * mAxisY.x + r.y * mAxisY.y + r.z * mAxisY.z,
        r.x * mAxisZ.x + r.y * mAxisZ.y + r.z * mAxisZ.z);
}

int VirtualShadowMapRenderer::LocalPageIndex(int slot, int face, int mip, int x, int y)
{
    return kDirectionalPages + slot * kLocalLightPages + face * kLocalFacePages
         + kLocalMipOffsets[mip] + y * (32 >> mip) + x;
}

VirtualShadowMapRenderer::LocalPageAddress VirtualShadowMapRenderer::DecodeLocalPage(int pageIndex)
{
    LocalPageAddress address;
    int rest = pageIndex - kDirectionalPages;
    address.Slot = rest / kLocalLightPages;
    rest %= kLocalLightPages;
    address.Face = rest / kLocalFacePages;
    rest %= kLocalFacePages;
    int mip = kLocalMips - 1;
    while (mip > 0 && rest < kLocalMipOffsets[mip])
        --mip;
    rest -= kLocalMipOffsets[mip];
    const int pagesPerAxis = 32 >> mip;
    address.Mip = mip;
    address.X = rest % pagesPerAxis;
    address.Y = rest / pagesPerAxis;
    return address;
}

int VirtualShadowMapRenderer::CoarseRank(int pageIndex)
{
    if (!IsLocalPage(pageIndex))
        return pageIndex / kPagesPerLevel;
    return DecodeLocalPage(pageIndex).Mip * 2 + 4;
}

void VirtualShadowMapRenderer::WritePageTableEntry(int pageIndex)
{
    const PageEntry& page = mPages[std::size_t(pageIndex)];
    const std::uint32_t value = (page.Tile >= 0 && page.Rendered) ? (kValidBit | std::uint32_t(page.Tile)) : 0u;
    if (mPageTableCpu[std::size_t(pageIndex)] != value)
    {
        mPageTableCpu[std::size_t(pageIndex)] = value;
        mPageTableDirty = true;
    }
}

void VirtualShadowMapRenderer::FreePage(PageEntry& page)
{
    if (page.Tile >= 0)
    {
        mTileOwner[std::size_t(page.Tile)] = -1;
        mFreeTiles.push_back(page.Tile);
    }
    page.Tile = -1;
    page.Rendered = false;
    page.Dirty = false;
    WritePageTableEntry(static_cast<int>(&page - mPages.data()));
}

void VirtualShadowMapRenderer::ResetAllPages()
{
    for (std::size_t tile = 0; tile < mTileOwner.size(); ++tile)
    {
        const int owner = mTileOwner[tile];
        if (owner >= 0)
            FreePage(mPages[std::size_t(owner)]);
    }
    for (PageEntry& page : mPages)
        page = PageEntry{};
    std::fill(mPageTableCpu.begin(), mPageTableCpu.end(), 0u);
    mPageTableDirty = true;
    for (LevelWindow& window : mWindows)
        window.Valid = false;
    for (LocalSlot& slot : mLocalSlots)
        slot = LocalSlot{};
    // Requests in flight were made against the old state.
    for (ReadbackSlot& slot : mReadbackSlots)
        slot.Pending = false;
    for (TrackedCaster& caster : mTrackedCasters)
        caster.Valid = false;
    mCastersNotReady.clear();
    mLightValid = false;
}

void VirtualShadowMapRenderer::ResetSunPages()
{
    for (std::size_t tile = 0; tile < mTileOwner.size(); ++tile)
    {
        const int owner = mTileOwner[tile];
        if (owner >= 0 && !IsLocalPage(owner))
            FreePage(mPages[std::size_t(owner)]);
    }
    for (int page = 0; page < kDirectionalPages; ++page)
        mPages[std::size_t(page)].LastRequested = 0;
    for (LevelWindow& window : mWindows)
        window.Valid = false;
    for (ReadbackSlot& slot : mReadbackSlots)
        slot.SunMarked = false;
}

void VirtualShadowMapRenderer::ResetLocalSlot(int slot)
{
    const int first = LocalPageIndex(slot, 0, 0, 0, 0);
    for (int page = first; page < first + kLocalLightPages; ++page)
    {
        FreePage(mPages[std::size_t(page)]);
        mPages[std::size_t(page)].LastRequested = 0;
    }
    mLocalSlots[slot] = LocalSlot{};
}

void VirtualShadowMapRenderer::DirtyLocalSlot(int slot)
{
    const int first = LocalPageIndex(slot, 0, 0, 0, 0);
    for (int page = first; page < first + kLocalLightPages; ++page)
    {
        if (mPages[std::size_t(page)].Tile >= 0)
            mPages[std::size_t(page)].Dirty = true;
    }
}

void VirtualShadowMapRenderer::DirtySunPages()
{
    for (std::size_t tile = 0; tile < mTileOwner.size(); ++tile)
    {
        const int owner = mTileOwner[tile];
        if (owner >= 0 && !IsLocalPage(owner))
            mPages[std::size_t(owner)].Dirty = true;
    }
}

void VirtualShadowMapRenderer::UpdateLightFrame(const FrameInputs& inputs, const VirtualShadowMapSettings& settings)
{
    XMVECTOR direction = XMLoadFloat3(&inputs.SunDirection);
    if (XMVectorGetX(XMVector3LengthSq(direction)) < 1e-12f)
        direction = XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f);
    direction = XMVector3Normalize(direction);

    const float depthRange = (std::max)(settings.DepthRange, 10.0f);
    const XMVECTOR camera = XMLoadFloat3(&inputs.CameraPosition);

    bool rotate = !mLightValid;
    bool reanchor = !mLightValid;
    if (mLightValid)
    {
        const float cosAngle = XMVectorGetX(XMVector3Dot(direction, XMLoadFloat3(&mLightDirection)));
        const float threshold = std::cos(XMConvertToRadians((std::max)(settings.LightRotationThreshold, 0.0f)));
        rotate = cosAngle < threshold;

        const float anchorDistance = XMVectorGetX(XMVector3Length(XMVectorSubtract(camera, XMLoadFloat3(&mAnchor))));
        const XMFLOAT3 cameraLs = ToLightSpace(inputs.CameraPosition);
        reanchor = anchorDistance > kReanchorDistance
                || std::fabs(cameraLs.z) > depthRange * 0.5f
                || (rotate && anchorDistance > kSoftRotationDistance);
    }

    if (!rotate && !reanchor)
        return;

    XMStoreFloat3(&mLightDirection, direction);
    XMVECTOR up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
    if (std::fabs(XMVectorGetZ(direction)) > 0.99f)
        up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMVECTOR axisX = XMVector3Normalize(XMVector3Cross(up, direction));
    const XMVECTOR axisY = XMVector3Cross(direction, axisX);
    XMStoreFloat3(&mAxisX, axisX);
    XMStoreFloat3(&mAxisY, axisY);
    XMStoreFloat3(&mAxisZ, direction);

    if (reanchor)
    {
        // A new anchor moves every sun page to a different place in the world, so
        // nothing the sun has cached survives it.
        mAnchor = inputs.CameraPosition;
        ResetSunPages();
    }
    else
    {
        // A small rotation about an anchor near the camera shifts the cached depth by a
        // fraction of a texel nearby: keep sampling it while the pages re-render.
        DirtySunPages();
    }

    mDepthNear = -depthRange;
    mDepthFar = depthRange;
    mLightValid = true;
}

void VirtualShadowMapRenderer::UpdateWindows(const FrameInputs& inputs)
{
    const XMFLOAT3 cameraLs = ToLightSpace(inputs.CameraPosition);
    for (int level = 0; level < kMaxLevels; ++level)
    {
        LevelWindow& window = mWindows[level];
        if (level >= mLevelCount)
        {
            window.Valid = false;
            continue;
        }

        const float pageWorld = TexelSize(level) * float(kPageTexels);
        const int originX = FloorDiv(cameraLs.x, pageWorld) - kLevelPages / 2;
        const int originY = FloorDiv(-cameraLs.y, pageWorld) - kLevelPages / 2;
        if (window.Valid && window.OriginX == originX && window.OriginY == originY)
            continue;

        // Pages the window slid off lose their tiles; their slots now belong to the
        // pages sliding in.
        PageEntry* levelPages = mPages.data() + std::size_t(level) * kPagesPerLevel;
        for (int slot = 0; slot < kPagesPerLevel; ++slot)
        {
            PageEntry& page = levelPages[slot];
            if (page.Tile < 0)
                continue;
            const int relX = page.GlobalX - originX;
            const int relY = page.GlobalY - originY;
            if (relX < 0 || relX >= kLevelPages || relY < 0 || relY >= kLevelPages)
                FreePage(page);
        }
        window.OriginX = originX;
        window.OriginY = originY;
        window.Valid = true;
    }
}

void VirtualShadowMapRenderer::UpdateLocalSlots(const FrameInputs& inputs, const VirtualShadowMapSettings& settings)
{
    const std::size_t lightCount = settings.LocalLights ? inputs.LocalLights.size() : 0;
    mLocalSlotOfInput.assign(inputs.LocalLights.size(), -1);
    for (LocalSlot& slot : mLocalSlots)
        slot.Seen = false;

    // Lights that already hold a slot keep it, and their pages.
    for (std::size_t i = 0; i < lightCount; ++i)
    {
        const LocalLight& light = inputs.LocalLights[i];
        for (int s = 0; s < kMaxLocalLights; ++s)
        {
            LocalSlot& slot = mLocalSlots[s];
            if (!slot.Used || slot.Seen || slot.Key != light.Key)
                continue;
            const bool moved = std::fabs(slot.Position.x - light.Position.x) > kLightMoveEpsilon
                            || std::fabs(slot.Position.y - light.Position.y) > kLightMoveEpsilon
                            || std::fabs(slot.Position.z - light.Position.z) > kLightMoveEpsilon
                            || std::fabs(slot.Radius - light.Radius) > kLightMoveEpsilon;
            if (moved)
                DirtyLocalSlot(s);
            slot.Seen = true;
            slot.Position = light.Position;
            slot.Radius = light.Radius;
            slot.Direction = light.Direction;
            slot.EmitCosine = light.EmitCosine;
            mLocalSlotOfInput[i] = s;
            break;
        }
    }

    // Slots whose light is gone free their pages.
    for (int s = 0; s < kMaxLocalLights; ++s)
    {
        if (mLocalSlots[s].Used && !mLocalSlots[s].Seen)
            ResetLocalSlot(s);
    }

    // New lights take a free slot.
    for (std::size_t i = 0; i < lightCount; ++i)
    {
        if (mLocalSlotOfInput[i] >= 0)
            continue;
        for (int s = 0; s < kMaxLocalLights; ++s)
        {
            LocalSlot& slot = mLocalSlots[s];
            if (slot.Used)
                continue;
            const LocalLight& light = inputs.LocalLights[i];
            slot.Used = true;
            slot.Seen = true;
            slot.Key = light.Key;
            slot.Position = light.Position;
            slot.Radius = light.Radius;
            slot.Direction = light.Direction;
            slot.EmitCosine = light.EmitCosine;
            mLocalSlotOfInput[i] = s;
            break;
        }
    }

    std::uint32_t used = 0;
    for (const LocalSlot& slot : mLocalSlots)
        used += slot.Used ? 1u : 0u;
    mStatistics.LocalLights = used;
}

void VirtualShadowMapRenderer::InvalidateBox(const XMFLOAT3& boxMin, const XMFLOAT3& boxMax)
{
    // Local lights: anything moving inside a light's reach re-renders the whole light.
    for (int s = 0; s < kMaxLocalLights; ++s)
    {
        const LocalSlot& slot = mLocalSlots[s];
        if (slot.Used && SphereIntersectsBox(slot.Position, slot.Radius, boxMin, boxMax))
            DirtyLocalSlot(s);
    }

    if (!mLightValid)
        return;

    float minX, minY, maxX, maxY;
    ProjectBox(boxMin, boxMax, mAnchor, mAxisX, mAxisY, minX, minY, maxX, maxY);

    for (int level = 0; level < mLevelCount; ++level)
    {
        const LevelWindow& window = mWindows[level];
        if (!window.Valid)
            continue;
        const float pageWorld = TexelSize(level) * float(kPageTexels);
        // Virtual y runs down, so -ls.y. A little slack either side for the filter.
        const float slack = TexelSize(level) * 2.0f;
        const int x0 = (std::max)(FloorDiv(minX - slack, pageWorld), window.OriginX);
        const int x1 = (std::min)(FloorDiv(maxX + slack, pageWorld), window.OriginX + kLevelPages - 1);
        const int y0 = (std::max)(FloorDiv(-maxY - slack, pageWorld), window.OriginY);
        const int y1 = (std::min)(FloorDiv(-minY + slack, pageWorld), window.OriginY + kLevelPages - 1);
        for (int y = y0; y <= y1; ++y)
        {
            for (int x = x0; x <= x1; ++x)
            {
                PageEntry& page = mPages[std::size_t(PageIndex(level, x, y))];
                if (page.Tile >= 0 && page.GlobalX == x && page.GlobalY == y)
                    page.Dirty = true;
            }
        }
    }
}

bool VirtualShadowMapRenderer::GetMeshBounds(const std::shared_ptr<Mesh>& mesh, XMFLOAT3& outMin, XMFLOAT3& outMax)
{
    if (!mesh)
        return false;

    // Keyed by address, which is only unique while the asset lives, so the entry
    // remembers its owner and is rebuilt when the address has been reused.
    MeshBounds& entry = mMeshBounds[mesh.get()];
    if (entry.Owner.lock() != mesh)
    {
        const std::vector<Vertex>& vertices = mesh->GetVertices();
        if (vertices.empty())
        {
            mMeshBounds.erase(mesh.get());
            return false;
        }
        XMFLOAT3 lo(FLT_MAX, FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (const Vertex& vertex : vertices)
        {
            lo.x = (std::min)(lo.x, vertex.Position.x); hi.x = (std::max)(hi.x, vertex.Position.x);
            lo.y = (std::min)(lo.y, vertex.Position.y); hi.y = (std::max)(hi.y, vertex.Position.y);
            lo.z = (std::min)(lo.z, vertex.Position.z); hi.z = (std::max)(hi.z, vertex.Position.z);
        }
        entry.Owner = mesh;
        entry.Min = lo;
        entry.Max = hi;
    }
    outMin = entry.Min;
    outMax = entry.Max;
    return true;
}

void VirtualShadowMapRenderer::TrackCasters(const FrameInputs& inputs)
{
    mCasters.clear();
    if (inputs.Entities == nullptr)
        return;
    std::vector<Entity>& entities = *inputs.Entities;

    // Entities removed from the end of the list take their shadows with them.
    for (std::size_t i = entities.size(); i < mTrackedCasters.size(); ++i)
    {
        if (mTrackedCasters[i].Valid)
            InvalidateBox(mTrackedCasters[i].Min, mTrackedCasters[i].Max);
    }
    mTrackedCasters.resize(entities.size());

    // Casters a page render had to skip (mesh not uploaded yet) get their pages again.
    for (std::uint32_t index : mCastersNotReady)
    {
        if (index < mTrackedCasters.size() && mTrackedCasters[index].Valid)
            InvalidateBox(mTrackedCasters[index].Min, mTrackedCasters[index].Max);
    }
    mCastersNotReady.clear();

    const bool vegetationChanged = inputs.VegetationSignature != mVegetationSignature;
    mVegetationSignature = inputs.VegetationSignature;

    for (std::size_t i = 0; i < entities.size(); ++i)
    {
        const Entity& entity = entities[i];
        TrackedCaster current;
        current.Hash = 14695981039346656037ull;
        bool isMesh = false;
        bool isVegetation = false;

        if (entity.HasMeshComponent() && entity.Mesh->MeshAsset)
        {
            XMFLOAT3 localMin, localMax;
            if (GetMeshBounds(entity.Mesh->MeshAsset, localMin, localMax))
            {
                TransformBox(localMin, localMax, entity.Transform.GetTransform(), current.Min, current.Max);
                const bool virtualized = inputs.VirtualizedMask != nullptr && i < inputs.VirtualizedMask->size()
                    && ((*inputs.VirtualizedMask)[i] & 2u) != 0;
                current.Hash = HashValue(current.Hash, entity.Transform);
                current.Hash = HashValue(current.Hash, static_cast<const void*>(entity.Mesh->MeshAsset.get()));
                current.Hash = HashValue(current.Hash, virtualized);
                current.Valid = true;
                isMesh = !virtualized;
            }
        }
        else if (entity.HasVegetationAreaComponent())
        {
            const VegetationAreaComponent& area = *entity.VegetationArea;
            VegetationScatter::ComputeWorldBounds(area, entity.Transform.Position, entity.Transform.Rotation,
                                                  current.Min, current.Max);
            // The area bounds hold the plants' roots; leave room for the plants.
            constexpr float kPlantReach = 12.0f, kPlantHeight = 40.0f;
            current.Min.x -= kPlantReach; current.Min.y -= kPlantReach; current.Min.z -= kPlantReach;
            current.Max.x += kPlantReach; current.Max.y += kPlantReach; current.Max.z += kPlantHeight;
            current.Hash = HashValue(current.Hash, entity.Transform);
            current.Hash = HashValue(current.Hash, area.Layers.size());
            for (const VegetationLayer& layer : area.Layers)
                current.Hash = HashValue(current.Hash, layer.CastShadows);
            current.Valid = true;
            isVegetation = true;
        }

        TrackedCaster& tracked = mTrackedCasters[i];
        if (tracked.Valid != current.Valid || tracked.Hash != current.Hash)
        {
            if (tracked.Valid)
                InvalidateBox(tracked.Min, tracked.Max);
            if (current.Valid)
                InvalidateBox(current.Min, current.Max);
            tracked = current;
        }
        else if (isVegetation && vegetationChanged)
        {
            InvalidateBox(current.Min, current.Max);
        }

        if (isMesh)
        {
            Caster caster;
            caster.EntityIndex = static_cast<std::uint32_t>(i);
            caster.Min = current.Min;
            caster.Max = current.Max;
            if (mLightValid)
                ProjectBox(current.Min, current.Max, mAnchor, mAxisX, mAxisY, caster.MinX, caster.MinY, caster.MaxX, caster.MaxY);
            mCasters.push_back(caster);
        }
    }
}

void VirtualShadowMapRenderer::RequestPage(int pageIndex, int globalX, int globalY)
{
    PageEntry& page = mPages[std::size_t(pageIndex)];
    if (page.Tile >= 0 && (page.GlobalX != globalX || page.GlobalY != globalY))
        FreePage(page);
    if (page.Tile < 0)
    {
        page.GlobalX = globalX;
        page.GlobalY = globalY;
        page.Rendered = false;
        page.Dirty = true;
        if (page.LastRequested != mFrame)
            mAllocationRequests.push_back(pageIndex);
    }
    page.LastRequested = mFrame;
}

void VirtualShadowMapRenderer::ConsumeRequests(std::size_t slotIndex)
{
    ReadbackSlot& slot = mReadbackSlots[slotIndex];
    if (!slot.Pending || mRequestsReadbackMapped == nullptr)
        return;
    slot.Pending = false;

    const std::uint32_t* bits = mRequestsReadbackMapped + slotIndex * kRequestWords;
    std::uint32_t requested = 0;

    // --- Sun ---
    if (slot.SunMarked && mSunActive)
    {
        const int levels = (std::min)(slot.LevelCount, mLevelCount);
        for (int level = 0; level < levels; ++level)
        {
            const LevelWindow& window = mWindows[level];
            if (!window.Valid)
                continue;
            const std::uint32_t* levelBits = bits + std::size_t(level) * kRequestWordsPerLevel;
            for (std::size_t word = 0; word < kRequestWordsPerLevel; ++word)
            {
                std::uint32_t mask = levelBits[word];
                while (mask != 0)
                {
                    unsigned long bit = 0;
                    _BitScanForward(&bit, mask);
                    mask &= mask - 1;

                    const int slotIndexInLevel = static_cast<int>(word * 32 + bit);
                    const int slotX = slotIndexInLevel & (kLevelPages - 1);
                    const int slotY = slotIndexInLevel / kLevelPages;
                    // The page that slot held in the window the marking pass saw.
                    const int globalX = slot.OriginX[level] + ((slotX - slot.OriginX[level]) & (kLevelPages - 1));
                    const int globalY = slot.OriginY[level] + ((slotY - slot.OriginY[level]) & (kLevelPages - 1));
                    const int relX = globalX - window.OriginX;
                    const int relY = globalY - window.OriginY;
                    if (relX < 0 || relX >= kLevelPages || relY < 0 || relY >= kLevelPages)
                        continue;

                    RequestPage(PageIndex(level, globalX, globalY), globalX, globalY);
                    ++requested;
                }
            }
        }
    }

    // --- Local lights ---
    const std::size_t firstLocalWord = std::size_t(kDirectionalPages) / 32;
    for (std::size_t word = firstLocalWord; word < kRequestWords; ++word)
    {
        std::uint32_t mask = bits[word];
        while (mask != 0)
        {
            unsigned long bit = 0;
            _BitScanForward(&bit, mask);
            mask &= mask - 1;

            const int pageIndex = static_cast<int>(word * 32 + bit);
            if (pageIndex >= kTotalPages)
                break;
            const LocalPageAddress address = DecodeLocalPage(pageIndex);
            // Only while the slot still holds the light it held when the pixels asked.
            const LocalSlot& light = mLocalSlots[address.Slot];
            if (!light.Used || !slot.LocalUsed[address.Slot] || slot.LocalKeys[address.Slot] != light.Key)
                continue;
            RequestPage(pageIndex, address.X, address.Y);
            ++requested;
        }
    }
    mStatistics.RequestedPages = requested;
}

void VirtualShadowMapRenderer::RequestResidentFallbacks(const FrameInputs& inputs)
{
    // Pages are asked for by the pixels that need them, and the answer is a few frames
    // behind. Whatever the camera turns toward has no fine pages yet, and the lighting
    // falls back to the next coarser level that has one - so there must always be one,
    // or a turn of the camera shows the surfaces unshadowed (indoors: sunlight through
    // the walls) until the requests catch up. These are kept resident whether asked for
    // or not.

    // Sun: a 3 x 3 block of pages around the camera on every level from the first whose
    // pages span kSunFallbackPageSpan. Nothing on screen is closer to the camera in light
    // space than it is in the world, so this covers everything within a page of it.
    if (mSunActive)
    {
        const XMFLOAT3 cameraLs = ToLightSpace(inputs.CameraPosition);
        for (int level = 0; level < mLevelCount; ++level)
        {
            const float pageWorld = TexelSize(level) * float(kPageTexels);
            if (pageWorld < kSunFallbackPageSpan && level != mLevelCount - 1)
                continue;
            const int centreX = FloorDiv(cameraLs.x, pageWorld);
            const int centreY = FloorDiv(-cameraLs.y, pageWorld);
            for (int y = centreY - 1; y <= centreY + 1; ++y)
                for (int x = centreX - 1; x <= centreX + 1; ++x)
                    RequestPage(PageIndex(level, x, y), x, y);
        }
    }

    // Local lights: every light keeps its coarsest mip on all six faces (one page each,
    // also what fog in the air falls back to), and lights near the camera the next one
    // up too (four pages a face).
    for (int s = 0; s < kMaxLocalLights; ++s)
    {
        const LocalSlot& light = mLocalSlots[s];
        if (!light.Used)
            continue;
        const float dx = light.Position.x - inputs.CameraPosition.x;
        const float dy = light.Position.y - inputs.CameraPosition.y;
        const float dz = light.Position.z - inputs.CameraPosition.z;
        const float reach = light.Radius + kLocalFallbackDistance;
        const bool nearCamera = dx * dx + dy * dy + dz * dz <= reach * reach;
        for (int face = 0; face < 6; ++face)
        {
            RequestPage(LocalPageIndex(s, face, kLocalMips - 1, 0, 0), 0, 0);
            if (nearCamera)
                for (int y = 0; y < 2; ++y)
                    for (int x = 0; x < 2; ++x)
                        RequestPage(LocalPageIndex(s, face, kLocalMips - 2, x, y), x, y);
        }
    }
}

void VirtualShadowMapRenderer::AllocateRequested()
{
    if (mAllocationRequests.empty())
        return;

    // Coarse levels first: they are the fallback everything finer leans on.
    std::sort(mAllocationRequests.begin(), mAllocationRequests.end(), [](int a, int b)
    {
        return CoarseRank(a) > CoarseRank(b);
    });

    std::vector<int> evictable;
    bool evictableBuilt = false;
    std::size_t evictCursor = 0;

    for (int pageIndex : mAllocationRequests)
    {
        PageEntry& page = mPages[std::size_t(pageIndex)];
        if (page.Tile >= 0)
            continue;

        int tile = -1;
        if (!mFreeTiles.empty())
        {
            tile = mFreeTiles.back();
            mFreeTiles.pop_back();
        }
        else
        {
            if (!evictableBuilt)
            {
                // Least recently requested first, and never a page still in use.
                for (std::size_t t = 0; t < mTileOwner.size(); ++t)
                {
                    const int owner = mTileOwner[t];
                    if (owner >= 0 && mPages[std::size_t(owner)].LastRequested + kInUseFrames < mFrame)
                        evictable.push_back(owner);
                }
                std::sort(evictable.begin(), evictable.end(), [this](int a, int b)
                {
                    return mPages[std::size_t(a)].LastRequested < mPages[std::size_t(b)].LastRequested;
                });
                evictableBuilt = true;
            }
            while (evictCursor < evictable.size() && tile < 0)
            {
                PageEntry& victim = mPages[std::size_t(evictable[evictCursor++])];
                if (victim.Tile < 0)
                    continue;
                FreePage(victim);
                tile = mFreeTiles.back();
                mFreeTiles.pop_back();
            }
        }

        if (tile < 0)
        {
            // Pool full of pages in use; this one waits and the lighting falls back.
            ++mStatistics.PoolOverflow;
            continue;
        }

        page.Tile = tile;
        page.Rendered = false;
        page.Dirty = true;
        mTileOwner[std::size_t(tile)] = pageIndex;
    }
    mAllocationRequests.clear();
}

void VirtualShadowMapRenderer::ChoosePagesToRender(const VirtualShadowMapSettings& settings)
{
    mPagesToRender.clear();
    std::uint32_t resident = 0;
    for (std::size_t tile = 0; tile < mTileOwner.size(); ++tile)
    {
        const int owner = mTileOwner[tile];
        if (owner < 0)
            continue;
        ++resident;
        PageEntry& page = mPages[std::size_t(owner)];
        const bool inUse = page.LastRequested + kInUseFrames >= mFrame;
        if (settings.DisableCaching && inUse)
            page.Dirty = true;
        if (!page.Dirty)
            continue;
        if (!inUse)
        {
            // Stale and nobody is looking: stop sampling it, and render it again only
            // if it is asked for.
            page.Rendered = false;
            WritePageTableEntry(owner);
            continue;
        }
        mPagesToRender.push_back(owner);
    }
    mStatistics.ResidentPages = resident;

    // Pages with nothing valid yet before stale ones, coarse before fine, then the most
    // recently requested.
    std::sort(mPagesToRender.begin(), mPagesToRender.end(), [this](int a, int b)
    {
        const PageEntry& pa = mPages[std::size_t(a)];
        const PageEntry& pb = mPages[std::size_t(b)];
        if (pa.Rendered != pb.Rendered)
            return !pa.Rendered;
        const int ra = CoarseRank(a), rb = CoarseRank(b);
        if (ra != rb)
            return ra > rb;
        return pa.LastRequested > pb.LastRequested;
    });

    const std::size_t budget = static_cast<std::size_t>(std::clamp(settings.MaxPagesPerFrame, 1, kMaxPagesPerFrameLimit));
    if (mPagesToRender.size() > budget)
    {
        mStatistics.WaitingPages = static_cast<std::uint32_t>(mPagesToRender.size() - budget);
        mPagesToRender.resize(budget);
    }

    for (int pageIndex : mPagesToRender)
    {
        PageEntry& page = mPages[std::size_t(pageIndex)];
        page.Dirty = false;
        page.Rendered = true;   // by the time anything samples it, it will be
        WritePageTableEntry(pageIndex);
    }
    mStatistics.RenderedThisFrame = static_cast<std::uint32_t>(mPagesToRender.size());
}

void VirtualShadowMapRenderer::BuildPageViews()
{
    mPageViews.clear();

    const XMMATRIX sunView = XMMatrixSet(
        mAxisX.x, mAxisY.x, mAxisZ.x, 0.0f,
        mAxisX.y, mAxisY.y, mAxisZ.y, 0.0f,
        mAxisX.z, mAxisY.z, mAxisZ.z, 0.0f,
        -(mAnchor.x * mAxisX.x + mAnchor.y * mAxisX.y + mAnchor.z * mAxisX.z),
        -(mAnchor.x * mAxisY.x + mAnchor.y * mAxisY.y + mAnchor.z * mAxisY.z),
        -(mAnchor.x * mAxisZ.x + mAnchor.y * mAxisZ.y + mAnchor.z * mAxisZ.z),
        1.0f);

    for (int pageIndex : mPagesToRender)
    {
        const PageEntry& page = mPages[std::size_t(pageIndex)];
        ShadowPageView view;

        if (!IsLocalPage(pageIndex))
        {
            const int level = pageIndex / kPagesPerLevel;
            const float texel = TexelSize(level);
            const float pageWorld = texel * float(kPageTexels);

            // Virtual y runs down the page, light-space y up it.
            const float left = float(page.GlobalX) * pageWorld;
            const float right = left + pageWorld;
            const float top = -float(page.GlobalY) * pageWorld;
            const float bottom = top - pageWorld;

            const XMMATRIX raster = sunView * XMMatrixOrthographicOffCenterLH(left, right, bottom, top, mDepthNear, mDepthFar);
            const XMMATRIX cull = sunView * XMMatrixOrthographicOffCenterLH(left, right, bottom, top,
                                                                            mDepthNear - kCullNearExtension, mDepthFar);
            XMStoreFloat4x4(&view.ViewProjection, cull);
            XMStoreFloat4x4(&view.ViewProjectionTransposed, XMMatrixTranspose(raster));
            view.Lod = ShadowPageView::LodMode::Orthographic;
            view.TexelWorldSize = texel;
        }
        else
        {
            const LocalPageAddress address = DecodeLocalPage(pageIndex);
            const LocalSlot& light = mLocalSlots[address.Slot];
            const CubeFace& face = kLocalFaces[address.Face];
            const float pagesPerAxis = float(32 >> address.Mip);

            // The page's rectangle of the face, in units of the unit-distance face plane.
            const float u0 = -1.0f + 2.0f * float(address.X) / pagesPerAxis;
            const float u1 = u0 + 2.0f / pagesPerAxis;
            const float vTop = 1.0f - 2.0f * float(address.Y) / pagesPerAxis;
            const float vBottom = vTop - 2.0f / pagesPerAxis;
            const float n = kLocalNear;
            const float f = (std::max)(light.Radius, n * 2.0f);

            const XMMATRIX faceView = XMMatrixLookToLH(XMLoadFloat3(&light.Position),
                                                       XMLoadFloat3(&face.Forward), XMLoadFloat3(&face.Up));
            const XMMATRIX viewProjection = faceView
                * XMMatrixPerspectiveOffCenterLH(u0 * n, u1 * n, vBottom * n, vTop * n, n, f);
            XMStoreFloat4x4(&view.ViewProjection, viewProjection);
            XMStoreFloat4x4(&view.ViewProjectionTransposed, XMMatrixTranspose(viewProjection));
            view.Lod = ShadowPageView::LodMode::Perspective;
            view.LodOrigin = light.Position;
            view.LodTexelAngle = 2.0f / float(kLocalResolution >> address.Mip);
        }

        const int tileX = page.Tile % mPoolPagesX;
        const int tileY = page.Tile / mPoolPagesX;
        view.Viewport = { float(tileX * int(kPageTexels)), float(tileY * int(kPageTexels)),
                          float(kPageTexels), float(kPageTexels), 0.0f, 1.0f };
        view.Scissor = { LONG(tileX * int(kPageTexels)), LONG(tileY * int(kPageTexels)),
                         LONG((tileX + 1) * int(kPageTexels)), LONG((tileY + 1) * int(kPageTexels)) };
        mPageViews.push_back(view);
    }
}

void VirtualShadowMapRenderer::BuildCasterDraws()
{
    mEntityDrawEntities.clear();
    mEntityDrawOffsets.assign(1, 0u);

    // Per light, the casters its sphere reaches, built only for lights with pages to draw.
    std::vector<std::uint32_t> lightCasters[kMaxLocalLights];
    bool lightCastersBuilt[kMaxLocalLights] = {};

    for (std::size_t viewIndex = 0; viewIndex < mPagesToRender.size(); ++viewIndex)
    {
        const int pageIndex = mPagesToRender[viewIndex];
        if (!IsLocalPage(pageIndex))
        {
            const PageEntry& page = mPages[std::size_t(pageIndex)];
            const int level = pageIndex / kPagesPerLevel;
            const float pageWorld = TexelSize(level) * float(kPageTexels);
            const float minX = float(page.GlobalX) * pageWorld;
            const float maxX = minX + pageWorld;
            const float maxY = -float(page.GlobalY) * pageWorld;
            const float minY = maxY - pageWorld;

            for (const Caster& caster : mCasters)
            {
                if (caster.MaxX < minX || caster.MinX > maxX || caster.MaxY < minY || caster.MinY > maxY)
                    continue;
                mEntityDrawEntities.push_back(caster.EntityIndex);
            }
        }
        else
        {
            const int slot = DecodeLocalPage(pageIndex).Slot;
            const LocalSlot& light = mLocalSlots[slot];
            if (!lightCastersBuilt[slot])
            {
                for (std::uint32_t c = 0; c < mCasters.size(); ++c)
                {
                    if (SphereIntersectsBox(light.Position, light.Radius, mCasters[c].Min, mCasters[c].Max))
                        lightCasters[slot].push_back(c);
                }
                lightCastersBuilt[slot] = true;
            }

            const XMMATRIX viewProjection = XMLoadFloat4x4(&mPageViews[viewIndex].ViewProjection);
            for (std::uint32_t c : lightCasters[slot])
            {
                if (BoxIntersectsView(mCasters[c].Min, mCasters[c].Max, viewProjection))
                    mEntityDrawEntities.push_back(mCasters[c].EntityIndex);
            }
        }
        mEntityDrawOffsets.push_back(static_cast<std::uint32_t>(mEntityDrawEntities.size()));
    }
}

void VirtualShadowMapRenderer::UpdateGpuConstants(const FrameInputs& inputs, const VirtualShadowMapSettings& settings)
{
    VsmGpuConstants& c = mGpuConstants;
    c = VsmGpuConstants{};
    c.Anchor = mAnchor;
    c.AxisX = mAxisX;
    c.AxisY = mAxisY;
    c.AxisZ = mAxisZ;
    c.FirstTexelSize = mFirstTexel;
    c.PixelFootprint = 2.0f * std::tan(inputs.FovYRadians * 0.5f) / float((std::max)(inputs.ViewportHeight, 1u));
    c.LodBias = settings.ResolutionLodBias;
    c.DepthNear = mDepthNear;
    c.DepthRangeInv = 1.0f / (std::max)(mDepthFar - mDepthNear, 1e-3f);
    c.LevelCount = static_cast<std::uint32_t>(mLevelCount);
    c.PoolPagesX = static_cast<std::uint32_t>(mPoolPagesX);
    c.Enabled = mSunActive ? 1u : 0u;
    c.NormalOffset = (std::max)(settings.NormalOffset, 0.0f);
    c.ConstantBias = (std::max)(settings.ConstantBias, 0.0f);
    c.DebugView = settings.DebugView;
    c.Active = 1u;
    for (int level = 0; level < kMaxLevels; ++level)
        c.WindowOrigin[level] = XMINT4(mWindows[level].OriginX, mWindows[level].OriginY, 0, 0);

    c.LocalEnabled = settings.LocalLights ? 1u : 0u;
    c.LocalLodBias = settings.LocalResolutionBias;
    c.LocalPageTableBase = static_cast<std::uint32_t>(kDirectionalPages);
    c.LocalNear = kLocalNear;
    for (int s = 0; s < kMaxLocalLights; ++s)
    {
        const LocalSlot& slot = mLocalSlots[s];
        if (!slot.Used)
            continue;
        c.LocalLights[s] = XMFLOAT4(slot.Position.x, slot.Position.y, slot.Position.z, (std::max)(slot.Radius, kLocalNear * 2.0f));
        c.LocalDirections[s] = XMFLOAT4(slot.Direction.x, slot.Direction.y, slot.Direction.z, slot.EmitCosine);
    }
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

bool VirtualShadowMapRenderer::BeginFrame(const FrameInputs& inputs, const VirtualShadowMapSettings& settings)
{
    mActiveThisFrame = false;
    mPageViews.clear();
    mPagesToRender.clear();
    mGpuConstants.Active = 0;
    mGpuConstants.Enabled = 0;
    mGpuConstants.LocalEnabled = 0;
    if (!mInitialized)
        return false;

    ++mFrame;
    mFrameSlot = static_cast<std::size_t>(mFrame % kFramesInFlight);
    mStatistics = Statistics{};
    mStatistics.PoolPages = static_cast<std::uint32_t>(mPoolPageCount);

    // --- Settings that invalidate the cache or rebuild resources --------------------
    const int desiredPoolPages = std::clamp(settings.PhysicalPages, 256, 4096);
    const bool poolChanged = ((desiredPoolPages + 63) / 64) * 64 != mPoolPageCount;
    const bool pipelineChanged = settings.SlopeScaledDepthBias != mPipelineSlopeBias;
    if (poolChanged || pipelineChanged)
    {
        // Frames in flight still use the old pool and pipeline.
        if (!DX12Context_WaitForGPU())
            return false;
        if (poolChanged)
        {
            ResetAllPages();
            if (!CreatePool(settings.PhysicalPages))
            {
                mInitialized = false;
                return false;
            }
            mPages.assign(std::size_t(kTotalPages), PageEntry{});
        }
        if (!CreatePipelines(settings.SlopeScaledDepthBias))
            return false;
        mInvalidateAllRequested = true;
    }

    if (mInvalidateAllRequested)
    {
        ResetAllPages();
        mInvalidateAllRequested = false;
    }

    mLevelCount = std::clamp(settings.LevelCount, 1, kMaxLevels);
    mFirstTexel = (std::max)(settings.FirstLevelTexelSize, 1e-4f);
    if (mBuiltFirstTexel != mFirstTexel || mBuiltLevelCount != mLevelCount || mBuiltDepthRange != settings.DepthRange)
    {
        mBuiltFirstTexel = mFirstTexel;
        mBuiltLevelCount = mLevelCount;
        mBuiltDepthRange = settings.DepthRange;
        ResetSunPages();
        mLightValid = false;   // re-anchors
    }

    // --- Sun ---
    mSunActive = inputs.SunEnabled;
    if (mSunActive)
    {
        UpdateLightFrame(inputs, settings);
        UpdateWindows(inputs);
    }
    else if (mLightValid)
    {
        ResetSunPages();
        mLightValid = false;
    }

    // --- Local lights ---
    UpdateLocalSlots(inputs, settings);

    TrackCasters(inputs);
    ConsumeRequests(mFrameSlot);
    RequestResidentFallbacks(inputs);
    AllocateRequested();
    ChoosePagesToRender(settings);
    BuildPageViews();
    BuildCasterDraws();
    UpdateGpuConstants(inputs, settings);

    mActiveThisFrame = true;
    return true;
}

void VirtualShadowMapRenderer::RenderPages(
    ID3D12GraphicsCommandList* commandList,
    EntityMeshRenderer& entities,
    VegetationRenderer& vegetation,
    VirtualGeometryRenderer& virtualGeometry)
{
    if (!mActiveThisFrame || commandList == nullptr)
        return;

    ID3D12DescriptorHeap* heap = DX12Context_GetSrvDescriptorHeap();
    commandList->SetDescriptorHeaps(1, &heap);

    // --- Page table --------------------------------------------------------------
    // The DEFAULT copy holds whatever was last uploaded, so only a change needs a copy.
    // Buffers decay to COMMON between frames.
    if (mPageTableDirty)
    {
        std::byte* destination = mPageTableUploadMapped + mFrameSlot * kPageTableBytes;
        std::memcpy(destination, mPageTableCpu.data(), static_cast<std::size_t>(kPageTableBytes));

        const auto toCopy = CD3DX12_RESOURCE_BARRIER::Transition(mPageTable.Get(),
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->ResourceBarrier(1, &toCopy);
        commandList->CopyBufferRegion(mPageTable.Get(), 0, mPageTableUpload.Get(), mFrameSlot * kPageTableBytes, kPageTableBytes);
        const auto toRead = CD3DX12_RESOURCE_BARRIER::Transition(mPageTable.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        commandList->ResourceBarrier(1, &toRead);
        mPageTableDirty = false;
    }

    // --- Pages -------------------------------------------------------------------
    if (!mPageViews.empty())
    {
        if (mPoolState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
        {
            const auto toDepth = CD3DX12_RESOURCE_BARRIER::Transition(mPool.Get(), mPoolState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            commandList->ResourceBarrier(1, &toDepth);
            mPoolState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        }

        // One tile per clear. A single clear with every tile's rect in one list overran
        // a fixed-size array inside the NVIDIA driver (a fail-fast in nvwgf2umx.dll)
        // once a frame rendered more than a handful of pages.
        for (const ShadowPageView& view : mPageViews)
            commandList->ClearDepthStencilView(mPoolDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 1, &view.Scissor);
        commandList->OMSetRenderTargets(0, nullptr, FALSE, &mPoolDsv);

        std::uint32_t drawCount = 0;
        entities.RenderDepthOnlyPages(commandList, mDepthRootSignature.Get(), mDepthPipeline.Get(),
                                      mPageViews, mEntityDrawOffsets, mEntityDrawEntities, mCastersNotReady, drawCount);
        mStatistics.CasterDraws = drawCount;

        // Both of these bind their own root signatures and pipelines, and draw into
        // whatever depth target is bound.
        vegetation.RenderShadowDepthPages(commandList, mPageViews, DXGI_FORMAT_D32_FLOAT);
        for (std::size_t page = 0; page < mPageViews.size(); ++page)
            virtualGeometry.RenderShadowPage(commandList, page, mPageViews[page], mPipelineSlopeBias);
    }

    if (mPoolState != D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)
    {
        const auto toRead = CD3DX12_RESOURCE_BARRIER::Transition(mPool.Get(), mPoolState, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        commandList->ResourceBarrier(1, &toRead);
        mPoolState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    }
}

void VirtualShadowMapRenderer::MarkRequests(
    ID3D12GraphicsCommandList*  commandList,
    D3D12_GPU_DESCRIPTOR_HANDLE depthSrv,
    D3D12_GPU_DESCRIPTOR_HANDLE normalSrv,
    const XMFLOAT4X4&           invViewProjection,
    const XMFLOAT3&             cameraPosition,
    UINT width,
    UINT height)
{
    if (!mActiveThisFrame || commandList == nullptr || depthSrv.ptr == 0 || normalSrv.ptr == 0
        || width == 0 || height == 0)
    {
        return;
    }

    MarkConstants constants{};
    constants.InvViewProj = invViewProjection;
    constants.CameraPos = cameraPosition;
    constants.Vsm = mGpuConstants;
    std::memcpy(mMarkConstantsMapped + mFrameSlot * kMarkConstantStride, &constants, sizeof(constants));

    // Clear the request bits (buffers start every frame in COMMON).
    {
        const auto toCopy = CD3DX12_RESOURCE_BARRIER::Transition(mRequests.Get(),
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->ResourceBarrier(1, &toCopy);
        commandList->CopyBufferRegion(mRequests.Get(), 0, mRequestsZero.Get(), 0, kRequestBytes);
        const auto toUav = CD3DX12_RESOURCE_BARRIER::Transition(mRequests.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->ResourceBarrier(1, &toUav);
    }

    ID3D12DescriptorHeap* heap = DX12Context_GetSrvDescriptorHeap();
    commandList->SetDescriptorHeaps(1, &heap);
    commandList->SetGraphicsRootSignature(mMarkRootSignature.Get());
    commandList->SetPipelineState(mMarkPipeline.Get());
    commandList->SetGraphicsRootConstantBufferView(MarkRootConstants,
        mMarkConstants->GetGPUVirtualAddress() + mFrameSlot * kMarkConstantStride);
    commandList->SetGraphicsRootDescriptorTable(MarkRootDepth, depthSrv);
    commandList->SetGraphicsRootDescriptorTable(MarkRootNormal, normalSrv);
    commandList->SetGraphicsRootUnorderedAccessView(MarkRootRequests, mRequests->GetGPUVirtualAddress());
    commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f };
    const D3D12_RECT scissor{ 0, 0, LONG(width), LONG(height) };
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);

    const auto toCopySource = CD3DX12_RESOURCE_BARRIER::Transition(mRequests.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->ResourceBarrier(1, &toCopySource);
    commandList->CopyBufferRegion(mRequestsReadback.Get(), mFrameSlot * kRequestBytes, mRequests.Get(), 0, kRequestBytes);

    ReadbackSlot& slot = mReadbackSlots[mFrameSlot];
    slot.Pending = true;
    slot.SunMarked = mSunActive;
    slot.LevelCount = mLevelCount;
    for (int level = 0; level < kMaxLevels; ++level)
    {
        slot.OriginX[level] = mWindows[level].OriginX;
        slot.OriginY[level] = mWindows[level].OriginY;
    }
    for (int s = 0; s < kMaxLocalLights; ++s)
    {
        slot.LocalUsed[s] = mLocalSlots[s].Used;
        slot.LocalKeys[s] = mLocalSlots[s].Key;
    }
}
