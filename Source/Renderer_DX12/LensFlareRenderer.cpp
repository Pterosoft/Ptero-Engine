#include "pch.h"
#include "LensFlareRenderer.h"

#include "LensDiffraction.h"
#include "System/PteroLog.h"

#include <DirectXPackedVector.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
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
    constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr float kPi = 3.14159265358979f;

    // Wavelength range the ghosts are traced over, as in the framework.
    constexpr float kMinLambda = 475.0f;
    constexpr float kMaxLambda = 650.0f;

    // Half-angle the sky pass draws the sun disc with (SkyRenderer.cpp); the occlusion
    // test stays inside it so the disc's soft edge does not read as partial cover.
    constexpr float kSunDiscSampleRadians = 0.6f * 0.8f * (kPi / 180.0f);

    // How far past the screen edge (in NDC) a light may be and still flare; it fades out
    // over the last stretch. Real flares do not stop at the frame edge, but past it the
    // depth buffer can no longer say whether the light is hidden.
    constexpr float kOffscreenLimit = 1.25f;
    constexpr float kOffscreenFade = 0.25f;

    struct GridEntryGpu
    {
        float Ndc[2];
        float Aperture[2];
        float Intensity;
        float RelRadius;
    };

    float Luminance(const XMFLOAT3& c)
    {
        return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
    }

    ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* device, UINT64 bytes, D3D12_HEAP_TYPE heapType,
        D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = heapType;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes, flags);
        ComPtr<ID3D12Resource> resource;
        DX12_THROW_IF_FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)));
        return resource;
    }

    ComPtr<ID3D12Resource> MakeColorTarget(ID3D12Device* device, UINT width, UINT height, D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = kColorFormat;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clear{};
        clear.Format = kColorFormat;

        ComPtr<ID3D12Resource> resource;
        DX12_THROW_IF_FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, state, &clear, IID_PPV_ARGS(&resource)));
        return resource;
    }

    bool CompileShader(DX12Shader& shader, const wchar_t* file, const wchar_t* entry, const wchar_t* profile,
        ShaderStage stage, std::string& error)
    {
        const ShaderCompileRequest request{ file, entry, profile, stage };
        if (shader.Compile(request))
            return true;
        char entryName[64] = {};
        std::snprintf(entryName, sizeof(entryName), "%ls", entry);
        error = std::string("LensFlareRenderer: ") + entryName + " failed: "
            + (shader.GetLastErrorMessage() ? shader.GetLastErrorMessage() : "unknown");
        return false;
    }
}

// -------------------------------------------------------------------------
// Initialize / Shutdown
// -------------------------------------------------------------------------

bool LensFlareRenderer::Initialize(UINT width, UINT height)
{
    if (width == 0 || height == 0)
        return false;
    if (mIsInitialized && width == mWidth && height == mHeight)
        return true;

    mLastError.clear();
    try
    {
        if (!mIsInitialized && !CreatePipelines())
            return false;

        if (mIsInitialized)
            DX12Context_WaitForGPU();
        if (!CreateTargets(width, height))
            return false;

        mWidth = width;
        mHeight = height;
        mIsInitialized = true;
        WriteStaticDescriptors();
        return true;
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("LensFlareRenderer::Initialize: ") + ex.what();
        return false;
    }
}

void LensFlareRenderer::Shutdown()
{
    if (mConstantBuffer && mMappedCb)
        mConstantBuffer->Unmap(0, nullptr);
    mMappedCb = nullptr;
    mConstantBuffer.Reset();
    mHeap.Reset();
    mRtvHeap.Reset();
    mGridBuffer.Reset();
    mGridEntries = 0;
    mBoundsBuffer.Reset();
    mVisibilityBuffer.Reset();
    mOutputTexture.Reset();
    mGhostTarget.Reset();
    mStarburstTexture.Reset();
    mStarburstUpload.Reset();
    mStarburstBlades = -1;
    mOcclusionPso.Reset(); mBoundsPso.Reset(); mTracePso.Reset();
    mGhostPso.Reset(); mCompositePso.Reset(); mStarburstPso.Reset();
    mRootSignature.Reset();
    mSlotKeys = {};
    if (mPendingLens.valid())
        mPendingLens.wait();
    mPendingLens = {};
    mLoadedLensName.clear();
    mLensValid = false;
    mIsInitialized = false;
    // The UI slot is kept: the shared heap never frees, so it is reused on re-init.
}

// -------------------------------------------------------------------------
// Pipelines
// -------------------------------------------------------------------------

bool LensFlareRenderer::CreatePipelines()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "LensFlareRenderer: null device.";
        return false;
    }

    const wchar_t* traceFile = L"Shaders\\LensFlare_Trace.hlsl";
    const wchar_t* drawFile = L"Shaders\\LensFlare_Draw.hlsl";
    if (!CompileShader(mOcclusionShader, traceFile, L"CSOcclusion", L"cs_5_0", ShaderStage::Compute, mLastError)
        || !CompileShader(mBoundsShader, traceFile, L"CSBounds", L"cs_5_0", ShaderStage::Compute, mLastError)
        || !CompileShader(mTraceShader, traceFile, L"CSTrace", L"cs_5_0", ShaderStage::Compute, mLastError)
        || !CompileShader(mGhostVs, drawFile, L"VSGhost", L"vs_5_0", ShaderStage::Vertex, mLastError)
        || !CompileShader(mGhostPs, drawFile, L"PSGhost", L"ps_5_0", ShaderStage::Pixel, mLastError)
        || !CompileShader(mFullscreenVs, drawFile, L"VSFullscreen", L"vs_5_0", ShaderStage::Vertex, mLastError)
        || !CompileShader(mCompositePs, drawFile, L"PSComposite", L"ps_5_0", ShaderStage::Pixel, mLastError)
        || !CompileShader(mStarburstVs, drawFile, L"VSStarburst", L"vs_5_0", ShaderStage::Vertex, mLastError)
        || !CompileShader(mStarburstPs, drawFile, L"PSStarburst", L"ps_5_0", ShaderStage::Pixel, mLastError))
        return false;

    // One root signature for every pass:
    //   [0] CBV b0
    //   [1] SRV t0..t6   (input, depth, grid, bounds, visibility, ghost target, starburst)
    //   [2] UAV u0..u2   (grid, bounds, visibility)
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = UavGrid - SrvInput;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = DescriptorsPerSlot - UavGrid;
    uavRange.BaseShaderRegister = 0;
    uavRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &uavRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = static_cast<UINT>(std::size(params));
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> serialized, errors;
    DX12_THROW_IF_FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
    DX12_THROW_IF_FAILED(device->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&mRootSignature)));

    auto makeCompute = [&](DX12Shader& shader, ComPtr<ID3D12PipelineState>& pso)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = mRootSignature.Get();
        desc.CS = shader.GetBytecode();
        DX12_THROW_IF_FAILED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
    };
    makeCompute(mOcclusionShader, mOcclusionPso);
    makeCompute(mBoundsShader, mBoundsPso);
    makeCompute(mTraceShader, mTracePso);

    auto makeGraphics = [&](DX12Shader& vs, DX12Shader& ps, bool additive, ComPtr<ID3D12PipelineState>& pso)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = mRootSignature.Get();
        desc.VS = vs.GetBytecode();
        desc.PS = ps.GetBytecode();
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = kColorFormat;
        desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        // Ghost grids fold over themselves; both windings are real light.
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

        D3D12_RENDER_TARGET_BLEND_DESC blend{};
        blend.BlendEnable = additive ? TRUE : FALSE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_ONE;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
        blend.DestBlendAlpha = D3D12_BLEND_ONE;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.BlendState.RenderTarget[0] = blend;

        DX12_THROW_IF_FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
    };
    makeGraphics(mGhostVs, mGhostPs, true, mGhostPso);
    makeGraphics(mFullscreenVs, mCompositePs, false, mCompositePso);
    makeGraphics(mStarburstVs, mStarburstPs, true, mStarburstPso);

    // Ringed constants: one copy per frame in flight.
    mCbStride = (sizeof(LensFlareCb) + 255ull) & ~255ull;
    mConstantBuffer = MakeBuffer(device, mCbStride * kFrameSlots, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    DX12_THROW_IF_FAILED(mConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedCb)));

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = DescriptorsPerSlot * kFrameSlots;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    DX12_THROW_IF_FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mHeap)));
    mDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = 2;
    DX12_THROW_IF_FAILED(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&mRtvHeap)));

    mBoundsBuffer = MakeBuffer(device, sizeof(XMFLOAT4) * kMaxLights * kMaxGhosts, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    mVisibilityBuffer = MakeBuffer(device, sizeof(float) * kMaxLights, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

    return true;
}

bool LensFlareRenderer::CreateTargets(UINT width, UINT height)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    if (!mUiSlotAllocated)
    {
        if (!DX12Context_AllocateSrvDescriptor(&mOutputUiSrvCpu, &mOutputUiSrvGpu))
        {
            mLastError = "LensFlareRenderer: failed to allocate a Ui SRV slot.";
            return false;
        }
        mUiSlotAllocated = true;
        mOutputTextureId = static_cast<UiTextureID>(mOutputUiSrvGpu.ptr);
    }

    mOutputTexture = MakeColorTarget(device, width, height, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // Ghosts are soft; half resolution quarters their fill cost.
    mGhostWidth = (std::max)(1u, width / 2);
    mGhostHeight = (std::max)(1u, height / 2);
    mGhostTarget = MakeColorTarget(device, mGhostWidth, mGhostHeight, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
    const UINT rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    device->CreateRenderTargetView(mOutputTexture.Get(), nullptr, rtv);
    rtv.ptr += rtvSize;
    device->CreateRenderTargetView(mGhostTarget.Get(), nullptr, rtv);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = kColorFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(mOutputTexture.Get(), &srv, mOutputUiSrvCpu);
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE LensFlareRenderer::HeapCpu(UINT slot, UINT descriptor) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = mHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(slot * DescriptorsPerSlot + descriptor) * mDescriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE LensFlareRenderer::HeapGpu(UINT slot, UINT descriptor) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle = mHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(slot * DescriptorsPerSlot + descriptor) * mDescriptorSize;
    return handle;
}

// Everything but the per-frame input and depth views. Only called with the GPU idle (at
// init, or after a rebuild waited for it), so no in-flight frame can be reading a slot.
void LensFlareRenderer::WriteStaticDescriptors()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr || !mHeap)
        return;

    for (UINT slot = 0; slot < kFrameSlots; ++slot)
    {
        auto bufferSrv = [&](ID3D12Resource* resource, UINT count, UINT stride, UINT descriptor)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.Buffer.NumElements = count;
            desc.Buffer.StructureByteStride = stride;
            device->CreateShaderResourceView(resource, &desc, HeapCpu(slot, descriptor));
        };
        auto bufferUav = [&](ID3D12Resource* resource, UINT count, UINT stride, UINT descriptor)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            desc.Buffer.NumElements = count;
            desc.Buffer.StructureByteStride = stride;
            device->CreateUnorderedAccessView(resource, nullptr, &desc, HeapCpu(slot, descriptor));
        };
        auto textureSrv = [&](ID3D12Resource* resource, UINT descriptor)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Format = kColorFormat;
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(resource, &desc, HeapCpu(slot, descriptor));
        };

        // Null views keep the table valid before a buffer exists; nothing reads them then.
        // The input and depth views are overwritten every frame before any dispatch.
        textureSrv(nullptr, SrvInput);
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Format = DXGI_FORMAT_R32_FLOAT;
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(nullptr, &desc, HeapCpu(slot, SrvDepth));
        }
        if (mGridBuffer)
        {
            bufferSrv(mGridBuffer.Get(), mGridEntries, sizeof(GridEntryGpu), SrvGrid);
            bufferUav(mGridBuffer.Get(), mGridEntries, sizeof(GridEntryGpu), UavGrid);
        }
        else
        {
            bufferSrv(nullptr, 1, sizeof(GridEntryGpu), SrvGrid);
            bufferUav(nullptr, 1, sizeof(GridEntryGpu), UavGrid);
        }
        bufferSrv(mBoundsBuffer.Get(), kMaxLights * kMaxGhosts, sizeof(XMFLOAT4), SrvBounds);
        bufferUav(mBoundsBuffer.Get(), kMaxLights * kMaxGhosts, sizeof(XMFLOAT4), UavBounds);
        bufferSrv(mVisibilityBuffer.Get(), kMaxLights, sizeof(float), SrvVisibility);
        bufferUav(mVisibilityBuffer.Get(), kMaxLights, sizeof(float), UavVisibility);
        textureSrv(mGhostTarget.Get(), SrvGhostTarget);
        textureSrv(mStarburstTexture ? mStarburstTexture.Get() : nullptr, SrvStarburst);
    }
}

// -------------------------------------------------------------------------
// Lens, buffers, starburst
// -------------------------------------------------------------------------

LensFlareRenderer::LoadedLens LensFlareRenderer::LoadAndRankLens(const std::string& name)
{
    LoadedLens result;
    const auto start = std::chrono::steady_clock::now();

    if (!LensFlareOptics::LoadLens(name, result.Lens, result.Error))
        return result;
    if (result.Lens.Elements.size() > kMaxSurfaces)
    {
        result.Error = "Lens has more than 48 surfaces: " + name;
        return result;
    }

    // Ghosts are ranked once per lens, with a round iris, so dragging the aperture sliders
    // never re-ranks. The ranking runs wide open (f/5.6 at most): stopping down only
    // shrinks a ghost, so the set stays right at any f-number, while some of the bundled
    // zooms pass next to no light off axis at their stock f/22 and would rank nothing.
    // The screen corner of a 21:9 display bounds the angles a light can reach.
    const LensFlareOptics::Lens& lens = result.Lens;
    LensFlareOptics::ApertureShape round;
    round.Blades = 8;
    round.Roundness = 1.0f;
    const float tanHalfY = 0.5f * lens.FilmHeight / lens.FocalLength;
    const float maxTan = tanHalfY * std::sqrt(1.0f + 2.4f * 2.4f) * kOffscreenLimit;
    const float rankFNumber = (std::min)(lens.FNumber, 5.6f);
    for (const LensFlareOptics::RankedGhost& ranked : LensFlareOptics::RankGhosts(lens, rankFNumber, maxTan, round))
        result.Ghosts.push_back(ranked.Surfaces);
    result.ImageScale = LensFlareOptics::ImageScale(lens, lens.FNumber, round);

    result.Milliseconds = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
    result.Ok = true;
    return result;
}

bool LensFlareRenderer::EnsureLens(const LensFlareSettings& settings)
{
    if (settings.Lens != mLoadedLensName)
    {
        // A load still running for the previous choice is waited out (the future's
        // destructor would block anyway); lens changes are rare, clicks in a combo.
        if (mPendingLens.valid())
            mPendingLens.wait();

        mLoadedLensName = settings.Lens;
        mLensValid = false;
        mLensError.clear();
        mRankedGhosts.clear();
        mSurfacesFNumber = -1.0f;
        mPendingLens = std::async(std::launch::async, &LensFlareRenderer::LoadAndRankLens, settings.Lens);
    }

    if (mPendingLens.valid()
        && mPendingLens.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        LoadedLens loaded = mPendingLens.get();
        if (!loaded.Ok)
        {
            mLensError = loaded.Error;
            PTERO_LOG_ERROR("Renderer", "Lens flare: %s", loaded.Error.c_str());
        }
        else
        {
            PTERO_LOG_INFO("Renderer", "Lens flare: loaded %s (%zu surfaces, %zu visible ghosts, image scale %.1f mm, %.0f ms)",
                mLoadedLensName.c_str(), loaded.Lens.Elements.size(), loaded.Ghosts.size(), loaded.ImageScale, loaded.Milliseconds);
            mLens = std::move(loaded.Lens);
            mRankedGhosts = std::move(loaded.Ghosts);
            mImageScale = loaded.ImageScale;
            mSurfacesFNumber = -1.0f;
            mLensValid = true;
        }
    }

    return mLensValid;
}

bool LensFlareRenderer::EnsureGridBuffer(UINT entries)
{
    if (mGridBuffer && entries <= mGridEntries)
        return true;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    // Replaced, not written: wait so no frame in flight still names the old buffer or
    // reads a descriptor rewritten below.
    DX12Context_WaitForGPU();
    mGridBuffer.Reset();
    try
    {
        mGridBuffer = MakeBuffer(device, static_cast<UINT64>(entries) * sizeof(GridEntryGpu), D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("LensFlareRenderer: grid buffer: ") + ex.what();
        mGridEntries = 0;
        WriteStaticDescriptors();
        return false;
    }
    mGridEntries = entries;
    WriteStaticDescriptors();
    return true;
}

bool LensFlareRenderer::EnsureStarburst(ID3D12GraphicsCommandList* commandList, const LensFlareSettings& settings)
{
    const int blades = std::clamp(settings.ApertureBlades, 3, 16);
    const float rotation = settings.ApertureRotation;
    const float roundness = std::clamp(settings.ApertureRoundness, 0.0f, 1.0f);
    if (mStarburstTexture && blades == mStarburstBlades && rotation == mStarburstRotation
        && roundness == mStarburstRoundness)
        return true;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    LensDiffraction::ApertureParams aperture;
    aperture.Blades = blades;
    aperture.RotationRadians = rotation * (kPi / 180.0f);
    aperture.Roundness = roundness;
    const std::vector<float> pattern = LensDiffraction::BuildStarburst(static_cast<int>(kStarburstSize), aperture, 1.0f);

    DX12Context_WaitForGPU();
    mStarburstTexture.Reset();
    mStarburstUpload.Reset();

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kStarburstSize;
    desc.Height = kStarburstSize;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = kColorFormat;
    desc.SampleDesc.Count = 1;
    DX12_THROW_IF_FAILED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&mStarburstTexture)));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 uploadBytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &uploadBytes);
    mStarburstUpload = MakeBuffer(device, uploadBytes, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);

    std::uint8_t* mapped = nullptr;
    DX12_THROW_IF_FAILED(mStarburstUpload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    const float half = 0.5f * static_cast<float>(kStarburstSize);
    for (UINT y = 0; y < kStarburstSize; ++y)
    {
        auto* row = reinterpret_cast<PackedVector::HALF*>(mapped + footprint.Offset + static_cast<UINT64>(y) * footprint.Footprint.RowPitch);
        for (UINT x = 0; x < kStarburstSize; ++x)
        {
            // The true pattern falls off as 1/r^3; the square root keeps the spikes
            // readable against the core, and a radial window takes the sprite's edge to 0.
            const float dx = (x + 0.5f - half) / half, dy = (y + 0.5f - half) / half;
            const float r = std::sqrt(dx * dx + dy * dy);
            const float window = std::clamp(1.0f - r, 0.0f, 1.0f);
            const float* texel = pattern.data() + (static_cast<std::size_t>(y) * kStarburstSize + x) * 3;
            for (int c = 0; c < 3; ++c)
                row[x * 4 + c] = PackedVector::XMConvertFloatToHalf(std::sqrt((std::max)(texel[c], 0.0f)) * window * window);
            row[x * 4 + 3] = PackedVector::XMConvertFloatToHalf(1.0f);
        }
    }
    mStarburstUpload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = mStarburstTexture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = mStarburstUpload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;
    commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const auto toShader = CD3DX12_RESOURCE_BARRIER::Transition(
        mStarburstTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &toShader);

    mStarburstBlades = blades;
    mStarburstRotation = rotation;
    mStarburstRoundness = roundness;
    WriteStaticDescriptors();
    return true;
}

// -------------------------------------------------------------------------
// Apply
// -------------------------------------------------------------------------

void LensFlareRenderer::Apply(ID3D12GraphicsCommandList* commandList, const LensFlareSettings& settings, const FrameInputs& inputs)
{
    if (!mIsInitialized || commandList == nullptr || inputs.Input == nullptr)
        return;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return;

    try
    {
        EnsureStarburst(commandList, settings);
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("LensFlareRenderer: starburst: ") + ex.what();
    }

    const bool lensReady = EnsureLens(settings);

    const UINT channels = static_cast<UINT>(std::clamp(settings.Wavelengths, 1, static_cast<int>(kMaxChannels)));
    const UINT gridSize = static_cast<UINT>(std::clamp(settings.RayGridSize, 8, 64));
    const UINT maxLights = static_cast<UINT>(std::clamp(settings.MaxLights, 1, static_cast<int>(kMaxLights)));
    UINT ghosts = lensReady
        ? static_cast<UINT>((std::min)(mRankedGhosts.size(), static_cast<std::size_t>(std::clamp(settings.MaxGhosts, 0, static_cast<int>(kMaxGhosts)))))
        : 0u;
    // Keep the grid inside its memory budget by drawing fewer ghosts.
    const UINT64 perGhost = static_cast<UINT64>(maxLights) * channels * gridSize * gridSize;
    ghosts = static_cast<UINT>((std::min)(static_cast<UINT64>(ghosts), kMaxGridEntries / perGhost));
    if (ghosts > 0 && !EnsureGridBuffer(static_cast<UINT>(perGhost * ghosts)))
        ghosts = 0;

    // ---- f-number, iris and per-wavelength surfaces --------------------------------
    const float fNumber = settings.FNumber > 0.0f ? settings.FNumber : (lensReady ? mLens.FNumber : 8.0f);
    if (lensReady && (fNumber != mSurfacesFNumber || static_cast<int>(channels) != mSurfacesChannels))
    {
        mApertureHeight = LensFlareOptics::PhysicalApertureHeight(mLens, fNumber);
        mChannelSurfaces.clear();
        for (UINT c = 0; c < channels; ++c)
        {
            const float lambda = channels == 1 ? 550.0f : kMinLambda + (kMaxLambda - kMinLambda) * c / (channels - 1);
            mChannelSurfaces.push_back(LensFlareOptics::BuildSurfaces(mLens, lambda, mApertureHeight));
        }
        mSurfacesFNumber = fNumber;
        mSurfacesChannels = static_cast<int>(channels);

        // The pupil moves across the front element as the light goes off axis, and its
        // size follows the f-number. A circular iris of the polygon's circumradius covers
        // every blade count, so the table does not depend on the aperture shape.
        LensFlareOptics::ApertureShape round;
        round.Roundness = 1.0f;
        const float tanHalfY = 0.5f * mLens.FilmHeight / mLens.FocalLength;
        mPupilMaxTan = tanHalfY * std::sqrt(1.0f + 2.4f * 2.4f) * kOffscreenLimit;
        const std::vector<LensFlareOptics::Surface>& middle = mChannelSurfaces[channels / 2];
        for (int i = 0; i < kPupilSamples; ++i)
        {
            const float t = mPupilMaxTan * static_cast<float>(i) / (kPupilSamples - 1);
            mPupilTable[i] = LensFlareOptics::DirectPupilRegion(mLens, middle, 550.0f, t, round);
        }
    }

    const UINT slot = static_cast<UINT>(mFrameCounter++ % kFrameSlots);
    LensFlareCb& cb = *reinterpret_cast<LensFlareCb*>(mMappedCb + slot * mCbStride);
    std::memset(&cb, 0, sizeof(cb));

    const float aspect = static_cast<float>(mWidth) / static_cast<float>(mHeight);
    const float focal = lensReady ? mLens.FocalLength : 50.0f;
    const float tanHalfY = lensReady ? 0.5f * mLens.FilmHeight / focal : 0.25f;
    const float tanHalfX = tanHalfY * aspect;

    cb.Lens = XMFLOAT4(
        lensReady ? LensFlareOptics::SensorDistance(mLens) : 0.0f,
        lensReady ? mLens.Elements[0].Height : 1.0f,
        kPi * std::pow(LensFlareOptics::EntrancePupilRadius(lensReady ? mLens : LensFlareOptics::Lens{}, fNumber), 2.0f),
        0.0f);
    cb.TanHalf = XMFLOAT4(tanHalfX, tanHalfY, 1.0f / (mImageScale * tanHalfX), 1.0f / (mImageScale * tanHalfY));
    cb.Aperture = XMFLOAT4(static_cast<float>(std::clamp(settings.ApertureBlades, 3, 16)),
        settings.ApertureRotation * (kPi / 180.0f), std::clamp(settings.ApertureRoundness, 0.0f, 1.0f), 0.0f);
    cb.Target = XMFLOAT4(static_cast<float>(mGhostWidth), static_cast<float>(mGhostHeight),
        static_cast<float>(mWidth), static_cast<float>(mHeight));
    // Visibility settles within about a tenth of a second.
    const float blend = 1.0f - std::exp(-(std::max)(inputs.DeltaSeconds, 0.0f) / 0.05f);
    cb.Occlusion = XMFLOAT4((std::max)(settings.OcclusionDepthTolerance, 0.0f),
        inputs.Projection._33, inputs.Projection._43, blend);
    const float intensity = (std::max)(settings.Intensity, 0.0f);
    cb.Starburst = XMFLOAT4(std::clamp(settings.StarburstSize, 0.01f, 2.0f),
        (std::max)(settings.StarburstIntensity, 0.0f) * 4.0f, 1.0f / aspect, (std::max)(settings.GhostIntensity, 0.0f));

    // Channel colours: each wavelength's colour, balanced so the channels sum to white.
    {
        float rgb[kMaxChannels][3] = {};
        float sum[3] = {};
        for (UINT c = 0; c < channels; ++c)
        {
            const float lambda = channels == 1 ? 550.0f : kMinLambda + (kMaxLambda - kMinLambda) * c / (channels - 1);
            if (channels == 1)
            {
                rgb[c][0] = rgb[c][1] = rgb[c][2] = 1.0f;
            }
            else
            {
                LensDiffraction::WavelengthToRgb(lambda, rgb[c][0], rgb[c][1], rgb[c][2]);
                // Keep every channel contributing a little to every primary so three
                // wavelengths still add up to white rather than a gap in the spectrum.
                for (float& v : rgb[c])
                    v += 0.02f;
            }
            for (int k = 0; k < 3; ++k)
                sum[k] += rgb[c][k];
            cb.Channel[c].w = lambda;
        }
        for (UINT c = 0; c < channels; ++c)
        {
            cb.Channel[c].x = rgb[c][0] / sum[0];
            cb.Channel[c].y = rgb[c][1] / sum[1];
            cb.Channel[c].z = rgb[c][2] / sum[2];
        }
    }

    for (UINT g = 0; g < ghosts; ++g)
    {
        cb.Ghosts[g / 2][(g & 1) * 2 + 0] = static_cast<UINT>(mRankedGhosts[g].First);
        cb.Ghosts[g / 2][(g & 1) * 2 + 1] = static_cast<UINT>(mRankedGhosts[g].Second);
    }

    if (lensReady)
    {
        for (UINT c = 0; c < channels && c < mChannelSurfaces.size(); ++c)
        {
            const std::vector<LensFlareOptics::Surface>& surfaces = mChannelSurfaces[c];
            for (std::size_t s = 0; s < surfaces.size(); ++s)
            {
                const LensFlareOptics::Surface& surface = surfaces[s];
                XMFLOAT4* out = &cb.Surfaces[(c * kMaxSurfaces + s) * 2];
                out[0] = XMFLOAT4(surface.CenterZ, surface.Radius, surface.Height, surface.Aperture);
                out[1] = XMFLOAT4(surface.IorBefore, surface.CoatingIor, surface.IorAfter, surface.CoatingThickness);
            }
        }
    }

    // ---- Light selection -------------------------------------------------------
    struct Candidate
    {
        const LightSource* Source;
        XMFLOAT2 Ndc;
        float ViewDepth;
        float Fade;
        float Score;
    };
    std::vector<Candidate> candidates;
    const XMMATRIX viewProjection = XMLoadFloat4x4(&inputs.ViewProjection);
    for (const LightSource& light : inputs.Lights)
    {
        if (light.IsSun ? !settings.SunFlares : !settings.LocalLightFlares)
            continue;

        const XMVECTOR point = light.IsSun
            ? XMVectorSet(light.Direction.x, light.Direction.y, light.Direction.z, 0.0f)
            : XMVectorSet(light.Position.x, light.Position.y, light.Position.z, 1.0f);
        XMFLOAT4 clip;
        XMStoreFloat4(&clip, XMVector4Transform(point, viewProjection));
        if (clip.w <= 1.0e-4f)
            continue;

        const XMFLOAT2 ndc(clip.x / clip.w, clip.y / clip.w);
        const float edge = (std::max)(std::fabs(ndc.x), std::fabs(ndc.y));
        if (edge >= kOffscreenLimit)
            continue;
        const float fade = std::clamp((kOffscreenLimit - edge) / kOffscreenFade, 0.0f, 1.0f);

        const float brightness = Luminance(light.Illuminance) * fade;
        if (brightness <= 0.0f || (!light.IsSun && brightness < settings.LocalLightThreshold))
            continue;

        // The sun always wins a slot when it is in view.
        candidates.push_back({ &light, ndc, light.IsSun ? 0.0f : clip.w, fade, light.IsSun ? 1.0e30f : brightness });
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) { return a.Score > b.Score; });
    if (candidates.size() > maxLights)
        candidates.resize(maxLights);

    // Slots keep a light's smoothed visibility attached to it from frame to frame.
    std::array<bool, kMaxLights> slotUsed{};
    std::array<int, kMaxLights> lightSlot{};
    lightSlot.fill(-1);
    for (std::size_t i = 0; i < candidates.size(); ++i)
        for (UINT s = 0; s < kMaxLights; ++s)
            if (!slotUsed[s] && mSlotKeys[s] == candidates[i].Source->Key && mSlotKeys[s] != 0)
            {
                slotUsed[s] = true;
                lightSlot[i] = static_cast<int>(s);
                break;
            }

    const float projY = inputs.Projection._22; // 1 / tan(fovY / 2)
    const UINT lightCount = static_cast<UINT>(candidates.size());
    for (UINT i = 0; i < lightCount; ++i)
    {
        const Candidate& candidate = candidates[i];
        const LightSource& source = *candidate.Source;

        bool reset = false;
        if (lightSlot[i] < 0)
        {
            for (UINT s = 0; s < kMaxLights; ++s)
                if (!slotUsed[s])
                {
                    slotUsed[s] = true;
                    lightSlot[i] = static_cast<int>(s);
                    break;
                }
            mSlotKeys[lightSlot[i]] = source.Key;
            reset = true;
        }

        // The lens frame, rotated about the axis so this light lies in the xz plane.
        const float tx = candidate.Ndc.x * tanHalfX;
        const float ty = candidate.Ndc.y * tanHalfY;
        const float t = std::sqrt(tx * tx + ty * ty);
        const float azimuth = t > 1.0e-6f ? std::atan2(ty, tx) : 0.0f;
        const float invLength = 1.0f / std::sqrt(1.0f + t * t);

        // Screen footprint used for the occlusion test, as a UV radius (vertical).
        float occlusionRadius;
        if (source.IsSun)
            occlusionRadius = std::tan(kSunDiscSampleRadians) * projY * 0.5f;
        else
            occlusionRadius = (std::max)(source.SourceRadius, 0.05f) / candidate.ViewDepth * projY * 0.5f;
        occlusionRadius = std::clamp(occlusionRadius, 2.0f / static_cast<float>(mHeight), 0.05f);

        LightCb& light = cb.Lights[i];
        light.DirectionLocal = XMFLOAT4(-t * invLength, 0.0f, -invLength, t);
        light.Rotation = XMFLOAT4(std::cos(azimuth), std::sin(azimuth), 0.0f, 0.0f);
        const float scale = candidate.Fade * intensity;
        light.Color = XMFLOAT4(source.Illuminance.x * scale, source.Illuminance.y * scale, source.Illuminance.z * scale, 0.0f);
        light.Screen = XMFLOAT4(candidate.Ndc.x * 0.5f + 0.5f, 0.5f - candidate.Ndc.y * 0.5f, occlusionRadius, candidate.ViewDepth);
        // The sky draws the sun disc at the sun's own colour, so a disc much darker than
        // that is behind cloud or fog.
        const float expectedDisc = (source.IsSun && settings.SunCloudOcclusion) ? 0.5f * Luminance(source.Illuminance) : 0.0f;
        light.Extra = XMFLOAT4(static_cast<float>(lightSlot[i]), reset ? 1.0f : 0.0f, expectedDisc, 0.0f);

        // The pupil region at this light's angle: the union of the two table entries
        // around it, so it can only be too generous, never too tight.
        light.Pupil = XMFLOAT4(1.0f, 1.0f, -1.0f, -1.0f);
        if (lensReady)
        {
            const float position = std::clamp(t / mPupilMaxTan, 0.0f, 1.0f) * (kPupilSamples - 1);
            const int below = static_cast<int>(position);
            const int above = (std::min)(below + 1, kPupilSamples - 1);
            for (int sample : { below, above })
            {
                const LensFlareOptics::PupilRegion& region = mPupilTable[sample];
                if (region.IsEmpty())
                    continue;
                if (light.Pupil.x > light.Pupil.z)
                    light.Pupil = XMFLOAT4(region.MinX, region.MinY, region.MaxX, region.MaxY);
                else
                    light.Pupil = XMFLOAT4((std::min)(light.Pupil.x, region.MinX), (std::min)(light.Pupil.y, region.MinY),
                        (std::max)(light.Pupil.z, region.MaxX), (std::max)(light.Pupil.w, region.MaxY));
            }
        }
    }

    // Slots whose light went away forget it, so it fades back in from its true value.
    for (UINT s = 0; s < kMaxLights; ++s)
        if (!slotUsed[s])
            mSlotKeys[s] = 0;

    cb.Counts[0] = gridSize;
    cb.Counts[1] = ghosts;
    cb.Counts[2] = channels;
    cb.Counts[3] = lensReady ? static_cast<UINT>(mLens.Elements.size()) : 0u;
    cb.Counts2[0] = lightCount;

    mActiveLights = static_cast<int>(lightCount);
    mGhostCount = static_cast<int>(ghosts);

    // ---- Descriptors for this frame's slot ---------------------------------------
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        view.Format = DX12ShaderReadableFormat(inputs.Input->GetDesc().Format);
        device->CreateShaderResourceView(inputs.Input, &view, HeapCpu(slot, SrvInput));
        view.Format = inputs.DepthSrvFormat;
        device->CreateShaderResourceView(inputs.Depth, &view, HeapCpu(slot, SrvDepth));
    }

    const D3D12_GPU_VIRTUAL_ADDRESS cbAddress = mConstantBuffer->GetGPUVirtualAddress() + slot * mCbStride;

    // ---- Compute: visibility, bounds, ray grids -------------------------------------
    {
        const auto toRead = CD3DX12_RESOURCE_BARRIER::Transition(inputs.Input,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        commandList->ResourceBarrier(1, &toRead);
    }

    ID3D12DescriptorHeap* heaps[] = { mHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);

    const bool traceGhosts = lightCount > 0 && ghosts > 0;
    if (lightCount > 0)
    {
        commandList->SetComputeRootSignature(mRootSignature.Get());
        commandList->SetComputeRootConstantBufferView(0, cbAddress);
        commandList->SetComputeRootDescriptorTable(1, HeapGpu(slot, SrvInput));
        commandList->SetComputeRootDescriptorTable(2, HeapGpu(slot, UavGrid));

        commandList->SetPipelineState(mOcclusionPso.Get());
        commandList->Dispatch(lightCount, 1, 1);

        if (traceGhosts)
        {
            commandList->SetPipelineState(mBoundsPso.Get());
            commandList->Dispatch(1, 1, lightCount * ghosts);
            const auto boundsDone = CD3DX12_RESOURCE_BARRIER::UAV(mBoundsBuffer.Get());
            commandList->ResourceBarrier(1, &boundsDone);

            commandList->SetPipelineState(mTracePso.Get());
            const UINT groups = (gridSize + 7) / 8;
            commandList->Dispatch(groups, groups, lightCount * ghosts * channels);
        }

        // Buffers start every frame in COMMON (they decay after each submission) and were
        // promoted to UNORDERED_ACCESS by the dispatches above - only the ones those
        // dispatches touched.
        D3D12_RESOURCE_BARRIER toDraw[3] = {
            CD3DX12_RESOURCE_BARRIER::Transition(mVisibilityBuffer.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(mBoundsBuffer.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(mGridBuffer.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        commandList->ResourceBarrier(traceGhosts ? 3 : 1, toDraw);
    }

    // ---- Graphics: ghosts, composite, starburst ----------------------------------
    commandList->SetGraphicsRootSignature(mRootSignature.Get());
    commandList->SetGraphicsRootConstantBufferView(0, cbAddress);
    commandList->SetGraphicsRootDescriptorTable(1, HeapGpu(slot, SrvInput));
    commandList->SetGraphicsRootDescriptorTable(2, HeapGpu(slot, UavGrid));
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_CPU_DESCRIPTOR_HANDLE outputRtv = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE ghostRtv = outputRtv;
    ghostRtv.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    {
        const auto toTarget = CD3DX12_RESOURCE_BARRIER::Transition(mGhostTarget.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        commandList->ResourceBarrier(1, &toTarget);
    }
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    commandList->ClearRenderTargetView(ghostRtv, clear, 0, nullptr);

    if (traceGhosts)
    {
        const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(mGhostWidth), static_cast<float>(mGhostHeight), 0.0f, 1.0f };
        const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(mGhostWidth), static_cast<LONG>(mGhostHeight) };
        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissor);
        commandList->OMSetRenderTargets(1, &ghostRtv, FALSE, nullptr);
        commandList->SetPipelineState(mGhostPso.Get());
        commandList->DrawInstanced(6 * (gridSize - 1) * (gridSize - 1), lightCount * ghosts * channels, 0, 0);
    }

    {
        D3D12_RESOURCE_BARRIER barriers[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(mGhostTarget.Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(mOutputTexture.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        commandList->ResourceBarrier(2, barriers);
    }

    {
        const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight), 0.0f, 1.0f };
        const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(mWidth), static_cast<LONG>(mHeight) };
        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissor);
        commandList->OMSetRenderTargets(1, &outputRtv, FALSE, nullptr);

        commandList->SetPipelineState(mCompositePso.Get());
        commandList->DrawInstanced(3, 1, 0, 0);

        if (lightCount > 0 && settings.StarburstIntensity > 0.0f && mStarburstTexture)
        {
            commandList->SetPipelineState(mStarburstPso.Get());
            commandList->DrawInstanced(6, lightCount, 0, 0);
        }
    }

    {
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(mOutputTexture.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
        barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(inputs.Input,
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
        // Hand the buffers back in COMMON, the state they would decay to anyway, so the
        // next frame's promotion starts from a known state whichever passes run.
        if (lightCount > 0)
        {
            barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(mVisibilityBuffer.Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
        }
        if (traceGhosts)
        {
            barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(mBoundsBuffer.Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
            barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(mGridBuffer.Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
        }
        commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    }

    ID3D12DescriptorHeap* sharedHeap[] = { DX12Context_GetSrvDescriptorHeap() };
    commandList->SetDescriptorHeaps(1, sharedHeap);
}
