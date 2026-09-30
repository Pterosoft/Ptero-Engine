#include "pch.h"
#include "VirtualGeometryRenderer.h"
#include "EntityMeshRenderer.h"

#include "System/PteroLog.h"

#include "d3dx12.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <thread>
#include <tuple>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

static_assert(VirtualGeometryRenderer::kDrawnInGBuffer == EntityMeshRenderer::kVirtualizedInGBuffer
    && VirtualGeometryRenderer::kDrawnInShadows == EntityMeshRenderer::kVirtualizedInShadows,
    "EntityMeshRenderer reads the entity mask with its own copy of these bits.");

namespace
{
    constexpr const char* kLogCategory = "VirtualGeometry";

    bool CreateBuffer(
        ID3D12Device* device,
        UINT64 size,
        D3D12_HEAP_TYPE heapType,
        D3D12_RESOURCE_STATES initialState,
        D3D12_RESOURCE_FLAGS flags,
        ComPtr<ID3D12Resource>& outResource,
        const wchar_t* name)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = heapType;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = (std::max)(size, UINT64(4));
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = flags;

        outResource.Reset();
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initialState,
                nullptr, IID_PPV_ARGS(&outResource))))
        {
            return false;
        }
        if (name != nullptr)
            outResource->SetName(name);
        return true;
    }

    // Same planes, same convention as VegetationRenderer: inward-facing,
    // normalised, from a row-major (not transposed) view-projection.
    void ExtractFrustumPlanes(const XMFLOAT4X4& m, XMFLOAT4 outPlanes[6])
    {
        const XMFLOAT4 raw[6] =
        {
            XMFLOAT4(m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41),
            XMFLOAT4(m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41),
            XMFLOAT4(m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42),
            XMFLOAT4(m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42),
            XMFLOAT4(m._13,         m._23,         m._33,         m._43),
            XMFLOAT4(m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43),
        };

        for (int i = 0; i < 6; ++i)
        {
            const float length = std::sqrt(raw[i].x * raw[i].x + raw[i].y * raw[i].y + raw[i].z * raw[i].z);
            const float inverse = (length > 1e-6f) ? (1.0f / length) : 1.0f;
            outPlanes[i] = XMFLOAT4(raw[i].x * inverse, raw[i].y * inverse, raw[i].z * inverse, raw[i].w * inverse);
        }
    }

    XMFLOAT4X4 Transposed(const XMFLOAT4X4& m)
    {
        XMFLOAT4X4 result;
        XMStoreFloat4x4(&result, XMMatrixTranspose(XMLoadFloat4x4(&m)));
        return result;
    }

    UINT PreviousPowerOfTwo(UINT value)
    {
        UINT result = 1;
        while (result * 2 <= value)
            result *= 2;
        return result;
    }

    // One subobject of a pipeline state stream, laid out as d3dx12's
    // CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT is (the trimmed d3dx12.h in this
    // project does not carry the stream helpers).
    template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename Value>
    struct alignas(void*) StreamSubobject
    {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE SubobjectType = Type;
        Value Inner{};
    };

    struct MeshPipelineStream
    {
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> RootSignature;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> MeshShader;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> PixelShader;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> Blend;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> SampleMask;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> Rasterizer;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> DepthStencil;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> RenderTargets;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> DepthFormat;
        StreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> SampleDesc;
    };

    D3D12_BLEND_DESC OpaqueBlend()
    {
        D3D12_BLEND_DESC blend{};
        for (D3D12_RENDER_TARGET_BLEND_DESC& target : blend.RenderTarget)
        {
            target.BlendEnable = FALSE;
            target.LogicOpEnable = FALSE;
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

    D3D12_RASTERIZER_DESC Rasterizer(D3D12_CULL_MODE cull, bool wireframe = false,
                                     float slopeScaledBias = 0.0f, float biasClamp = 0.0f)
    {
        D3D12_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
        rasterizer.CullMode = cull;
        rasterizer.FrontCounterClockwise = FALSE;
        rasterizer.DepthBias = 0;
        rasterizer.DepthBiasClamp = biasClamp;
        rasterizer.SlopeScaledDepthBias = slopeScaledBias;
        rasterizer.DepthClipEnable = TRUE;
        rasterizer.MultisampleEnable = FALSE;
        rasterizer.AntialiasedLineEnable = FALSE;
        rasterizer.ForcedSampleCount = 0;
        rasterizer.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
        return rasterizer;
    }

    D3D12_DEPTH_STENCIL_DESC DepthTest(bool enabled)
    {
        D3D12_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = enabled ? TRUE : FALSE;
        depth.DepthWriteMask = enabled ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        depth.StencilEnable = FALSE;
        depth.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        depth.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        const D3D12_DEPTH_STENCILOP_DESC keep{ D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS };
        depth.FrontFace = keep;
        depth.BackFace = keep;
        return depth;
    }

    std::string FormatCount(std::uint64_t value)
    {
        std::string digits = std::to_string(value);
        for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3)
            digits.insert(static_cast<std::size_t>(i), ",");
        return digits;
    }

    // Root parameter indices of the raster root signature. 1-10 are the
    // G-Buffer's own layout, so EntityMeshRenderer's material binding writes
    // into them unchanged.
    enum RasterRoot : UINT
    {
        RasterDrawConstants = 0,   // b3
        RasterMaterial      = 1,   // b1, EntityMeshRenderer
        RasterRain          = 2,   // b2
        RasterTextures      = 3,   // t0-t7, one table each, 3..10
        RasterBin           = 11,  // b4 root constant
        RasterClusters      = 12,  // t0, space1
        RasterInstances,
        RasterClusterVertices,
        RasterClusterTriangles,
        RasterVertices,
        RasterVisible,
        RasterBinRanges,
        RasterRootCount
    };

    // Root parameter indices of the cull root signature.
    enum CullRoot : UINT
    {
        CullConstantsRoot = 0,
        CullClusters, CullAssets, CullInstances, CullViewsRoot, CullBinTable,
        CullCounters, CullChunks, CullCandidates, CullOccluded, CullBinCounts,
        CullBinRanges, CullDrawArgs, CullDispatchArgs, CullVisible,
        CullHzbTable, CullDepthTable, CullHzbSrcTable, CullHzbDstTable,
        CullRootCount
    };
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

bool VirtualGeometryRenderer::Initialize()
{
    mLastError.clear();
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "VirtualGeometryRenderer: no device.";
        return false;
    }

    // Mesh shaders need the hardware tier, shader model 6.5 and the
    // pipeline-stream entry point; any one missing leaves the vertex path.
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
        D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{ D3D_SHADER_MODEL_6_5 };
        ComPtr<ID3D12Device2> device2;
        mMeshShaderSupported =
            SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7)))
            && options7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED
            && SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel)))
            && shaderModel.HighestShaderModel >= D3D_SHADER_MODEL_6_5
            && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device2)));
    }

    if (!CreateWorkBuffers() || !CreateCullPipelines() || !CreateCommandSignatures())
        return false;

    const unsigned hardwareThreads = (std::max)(std::thread::hardware_concurrency(), 2u);
    mMaxConcurrentBuilds = (std::clamp)(static_cast<std::size_t>(hardwareThreads / 2), std::size_t(1), std::size_t(4));

    mInitialized = true;
    PTERO_LOG_INFO(kLogCategory, "Virtualized geometry ready (%s rasterisation, up to %zu concurrent builds).",
        mMeshShaderSupported ? "mesh shader" : "vertex shader", mMaxConcurrentBuilds);
    return true;
}

void VirtualGeometryRenderer::Shutdown()
{
    // A std::future from std::async joins in its destructor; builds finish
    // (they only ever read the mesh they own) before the entries go away.
    mAssets.clear();
    mActiveBuilds = 0;
    mPoolAssets.clear();

    auto unmap = [](ComPtr<ID3D12Resource>& resource, std::byte*& mapped)
    {
        if (resource && mapped != nullptr)
            resource->Unmap(0, nullptr);
        mapped = nullptr;
        resource.Reset();
    };
    unmap(mInstanceUpload, mInstanceUploadMapped);
    unmap(mBinTableUpload, mBinTableUploadMapped);
    unmap(mViewUpload, mViewUploadMapped);
    unmap(mCullConstantUpload, mCullConstantMapped);
    unmap(mDrawConstantUpload, mDrawConstantMapped);
    unmap(mStatsReadback, mStatsReadbackMapped);
    mInstanceCapacity = 0;
    mBinTableCapacity = 0;

    // std::addressof: ComPtr's own operator& yields a ComPtrRef, not a pointer.
    for (ComPtr<ID3D12Resource>* resource : {
             std::addressof(mClusterPool),
             std::addressof(mClusterVertexPool),
             std::addressof(mClusterTrianglePool),
             std::addressof(mVertexPool),
             std::addressof(mAssetPool),
             std::addressof(mCounters),
             std::addressof(mChunks),
             std::addressof(mCandidates),
             std::addressof(mOccluded),
             std::addressof(mBinCounts),
             std::addressof(mBinRanges),
             std::addressof(mDrawArgs),
             std::addressof(mDispatchArgs),
             std::addressof(mVisible),
             std::addressof(mZeroUpload),
             std::addressof(mHzb) })
    {
        resource->Reset();
    }

    mRetired.clear();
    mCullRootSignature.Reset();
    mRasterRootSignature.Reset();
    for (ComPtr<ID3D12PipelineState>& pipeline : mCullPipelines)
        pipeline.Reset();
    mGBufferPipeline.Reset();
    mDebugPipeline.Reset();
    mSunShadowPipeline.Reset();
    mShadowPagePipeline.Reset();
    mPointShadowPipeline.Reset();
    mMotionPipeline.Reset();
    mDispatchSignature.Reset();
    mDispatchMeshSignature.Reset();
    mDrawSignature.Reset();
    mGBufferKey = {};
    mHzbWidth = mHzbHeight = mHzbMipCount = 0;
    mHzbValid = false;
    // The HZB descriptor slots are kept: the shared heap never frees, so a
    // re-initialise must rewrite them rather than take new ones.
    mInitialized = false;
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

bool VirtualGeometryRenderer::CreateWorkBuffers()
{
    ID3D12Device* device = DX12Context_GetDevice();
    constexpr D3D12_RESOURCE_FLAGS kUav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    // Buffers are always created in COMMON whatever state is asked for, and
    // decay back to it at the end of every ExecuteCommandLists; BeginFrame
    // resets the tracked states accordingly. The chunk, candidate and occluded
    // buffers are only ever bound as UAVs and rely on implicit promotion.
    constexpr D3D12_RESOURCE_STATES kCommon = D3D12_RESOURCE_STATE_COMMON;

    bool ok =
        CreateBuffer(device, kCounterBytes, D3D12_HEAP_TYPE_DEFAULT, kCommon, kUav, mCounters, L"VG_Counters")
        && CreateBuffer(device, UINT64(kChunkCapacity) * 8, D3D12_HEAP_TYPE_DEFAULT, kCommon, kUav, mChunks, L"VG_Chunks")
        && CreateBuffer(device, UINT64(kCandidateCapacity) * 16, D3D12_HEAP_TYPE_DEFAULT, kCommon, kUav, mCandidates, L"VG_Candidates")
        && CreateBuffer(device, UINT64(kOccludedCapacity) * 8, D3D12_HEAP_TYPE_DEFAULT, kCommon, kUav, mOccluded, L"VG_Occluded")
        && CreateBuffer(device, UINT64(kMaxBins) * 2 * 4, D3D12_HEAP_TYPE_DEFAULT, kCommon, kUav, mBinCounts, L"VG_BinCounts")
        && CreateBuffer(device, UINT64(kMaxBins) * 2 * 8, D3D12_HEAP_TYPE_DEFAULT, kCommon, kUav, mBinRanges, L"VG_BinRanges")
        && CreateBuffer(device, UINT64(kMaxBins) * 2 * 16, D3D12_HEAP_TYPE_DEFAULT, kCommon, kUav, mDrawArgs, L"VG_DrawArgs")
        && CreateBuffer(device, 64, D3D12_HEAP_TYPE_DEFAULT, kCommon, kUav, mDispatchArgs, L"VG_DispatchArgs")
        && CreateBuffer(device, UINT64(kCandidateCapacity) * 8, D3D12_HEAP_TYPE_DEFAULT, kCommon, kUav, mVisible, L"VG_Visible");
    if (!ok)
    {
        mLastError = "VirtualGeometryRenderer: failed to create the GPU work buffers.";
        return false;
    }

    const UINT64 zeroBytes = (std::max)(UINT64(kCounterBytes), UINT64(kMaxBins) * 2 * 4);
    if (!CreateBuffer(device, zeroBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
            D3D12_RESOURCE_FLAG_NONE, mZeroUpload, L"VG_Zero"))
    {
        mLastError = "VirtualGeometryRenderer: failed to create the zero buffer.";
        return false;
    }
    {
        void* mapped = nullptr;
        if (FAILED(mZeroUpload->Map(0, nullptr, &mapped)))
            return false;
        std::memset(mapped, 0, static_cast<std::size_t>(zeroBytes));
        mZeroUpload->Unmap(0, nullptr);
    }

    auto createMapped = [&](UINT64 size, D3D12_HEAP_TYPE heap, ComPtr<ID3D12Resource>& resource,
                            std::byte*& mapped, const wchar_t* name)
    {
        const D3D12_RESOURCE_STATES state = heap == D3D12_HEAP_TYPE_READBACK
            ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ;
        if (!CreateBuffer(device, size, heap, state, D3D12_RESOURCE_FLAG_NONE, resource, name))
            return false;
        return SUCCEEDED(resource->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    };

    ok = createMapped(UINT64(kFramesInFlight) * kMaxViews * sizeof(GpuView), D3D12_HEAP_TYPE_UPLOAD, mViewUpload, mViewUploadMapped, L"VG_Views")
        && createMapped(UINT64(kFramesInFlight) * kCullConstantSlots * sizeof(CullConstants), D3D12_HEAP_TYPE_UPLOAD, mCullConstantUpload, mCullConstantMapped, L"VG_CullConstants")
        && createMapped(UINT64(kFramesInFlight) * kDrawConstantSlots * sizeof(DrawConstants), D3D12_HEAP_TYPE_UPLOAD, mDrawConstantUpload, mDrawConstantMapped, L"VG_DrawConstants")
        && createMapped(UINT64(kFramesInFlight) * kCounterBytes, D3D12_HEAP_TYPE_READBACK, mStatsReadback, mStatsReadbackMapped, L"VG_StatsReadback");
    if (!ok)
    {
        mLastError = "VirtualGeometryRenderer: failed to create the per-frame upload buffers.";
        return false;
    }
    std::memset(mStatsReadbackMapped, 0, kFramesInFlight * kCounterBytes);

    return EnsureUploadCapacity(256, 1024);
}

bool VirtualGeometryRenderer::EnsureUploadCapacity(std::size_t instanceCount, std::size_t binTableEntries)
{
    ID3D12Device* device = DX12Context_GetDevice();

    auto grow = [&](std::size_t required, std::size_t& capacity, std::size_t elementSize,
                    ComPtr<ID3D12Resource>& resource, std::byte*& mapped, const wchar_t* name)
    {
        if (required <= capacity && resource)
            return true;

        const std::size_t newCapacity = (std::max)(required + required / 2, std::size_t(64));
        if (resource)
        {
            resource->Unmap(0, nullptr);
            mapped = nullptr;
            // Earlier frames may still be reading their ring slot of it.
            RetireResource(std::move(resource));
        }

        if (!CreateBuffer(device, UINT64(newCapacity) * elementSize * kFramesInFlight, D3D12_HEAP_TYPE_UPLOAD,
                D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, resource, name)
            || FAILED(resource->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
        {
            capacity = 0;
            return false;
        }
        capacity = newCapacity;
        return true;
    };

    return grow(instanceCount, mInstanceCapacity, sizeof(GpuInstance), mInstanceUpload, mInstanceUploadMapped, L"VG_Instances")
        && grow(binTableEntries, mBinTableCapacity, sizeof(std::uint32_t), mBinTableUpload, mBinTableUploadMapped, L"VG_BinTable");
}

bool VirtualGeometryRenderer::EnsureHzb(UINT sceneWidth, UINT sceneHeight)
{
    if (sceneWidth == 0 || sceneHeight == 0)
        return false;

    const UINT width = PreviousPowerOfTwo(sceneWidth);
    const UINT height = PreviousPowerOfTwo(sceneHeight);
    if (mHzb && width == mHzbWidth && height == mHzbHeight
        && sceneWidth == mHzbSourceWidth && sceneHeight == mHzbSourceHeight)
    {
        return true;
    }

    ID3D12Device* device = DX12Context_GetDevice();

    UINT mips = 1;
    while (mips < kMaxHzbMips && ((std::max)(width, height) >> mips) > 0)
        ++mips;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = static_cast<UINT16>(mips);
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    ComPtr<ID3D12Resource> hzb;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&hzb))))
    {
        mLastError = "VirtualGeometryRenderer: failed to create the HZB.";
        return false;
    }
    hzb->SetName(L"VG_HZB");

    if (!mHzbDescriptorsAllocated)
    {
        if (!DX12Context_AllocateSrvDescriptor(&mHzbSrvCpu, &mHzbSrvGpu))
            return false;
        for (UINT mip = 0; mip < kMaxHzbMips; ++mip)
        {
            if (!DX12Context_AllocateSrvDescriptor(&mHzbUavCpu[mip], &mHzbUavGpu[mip]))
                return false;
        }
        mHzbDescriptorsAllocated = true;
    }

    // Replacing it (a resize) rewrites descriptors that earlier in-flight
    // frames still execute through, and a descriptor is read when the GPU runs
    // the work, not when it is recorded. Resizes are rare, so wait for those
    // frames rather than let them write into a texture their barriers never
    // named. This frame has not touched the HZB yet.
    if (mHzb)
    {
        DX12Context_WaitForGPU();
        mHzb.Reset();
    }
    mHzb = std::move(hzb);
    mHzbState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = mips;
    device->CreateShaderResourceView(mHzb.Get(), &srv, mHzbSrvCpu);

    for (UINT mip = 0; mip < kMaxHzbMips; ++mip)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        // Slots past the chain get the last level, so every table stays valid.
        uav.Texture2D.MipSlice = (std::min)(mip, mips - 1);
        device->CreateUnorderedAccessView(mHzb.Get(), nullptr, &uav, mHzbUavCpu[mip]);
    }

    mHzbWidth = width;
    mHzbHeight = height;
    mHzbMipCount = mips;
    mHzbSourceWidth = sceneWidth;
    mHzbSourceHeight = sceneHeight;
    mHzbValid = false;
    return true;
}

void VirtualGeometryRenderer::RetireResource(ComPtr<ID3D12Resource> resource)
{
    if (!resource)
        return;
    // One frame of slack over the number in flight: the resource may be
    // retired part-way through a frame that already recorded work using it.
    mRetired.push_back({ std::move(resource), static_cast<int>(kFramesInFlight) + 1 });
}

void VirtualGeometryRenderer::RetireExpiredResources()
{
    mRetired.erase(std::remove_if(mRetired.begin(), mRetired.end(),
        [](RetiredResource& retired) { return --retired.FramesRemaining <= 0; }), mRetired.end());
}

// ---------------------------------------------------------------------------
// Pipelines
// ---------------------------------------------------------------------------

bool VirtualGeometryRenderer::CreateCullPipelines()
{
    ID3D12Device* device = DX12Context_GetDevice();

    static const wchar_t* const kEntryPoints[PassCount] =
    {
        L"VgInstanceCull", L"VgChunkArgs", L"VgClusterCull", L"VgOcclusionArgs", L"VgOcclusionRecull",
        L"VgBuildBins", L"VgScatter", L"VgHzbFromDepth", L"VgHzbDownsample",
    };

    for (int pass = 0; pass < PassCount; ++pass)
    {
        // cs_5_0, matching the startup shader cache's guess for this file.
        const ShaderCompileRequest request{ L"Shaders\\VirtualGeometry_Cull.hlsl", kEntryPoints[pass], L"cs_5_0", ShaderStage::Compute };
        if (!mCullShaders[pass].Compile(request))
        {
            mLastError = std::string("VirtualGeometry cull shader compile failed: ")
                + (mCullShaders[pass].GetLastErrorMessage() ? mCullShaders[pass].GetLastErrorMessage() : "unknown");
            PTERO_LOG_ERROR(kLogCategory, "%s", mLastError.c_str());
            return false;
        }
    }

    D3D12_ROOT_PARAMETER params[CullRootCount]{};
    auto rootDescriptor = [&](UINT index, D3D12_ROOT_PARAMETER_TYPE type, UINT reg)
    {
        params[index].ParameterType = type;
        params[index].Descriptor.ShaderRegister = reg;
        params[index].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    };
    rootDescriptor(CullConstantsRoot, D3D12_ROOT_PARAMETER_TYPE_CBV, 0);
    rootDescriptor(CullClusters,  D3D12_ROOT_PARAMETER_TYPE_SRV, 0);
    rootDescriptor(CullAssets,    D3D12_ROOT_PARAMETER_TYPE_SRV, 1);
    rootDescriptor(CullInstances, D3D12_ROOT_PARAMETER_TYPE_SRV, 2);
    rootDescriptor(CullViewsRoot, D3D12_ROOT_PARAMETER_TYPE_SRV, 3);
    rootDescriptor(CullBinTable,  D3D12_ROOT_PARAMETER_TYPE_SRV, 4);
    for (UINT uav = 0; uav <= 8; ++uav)
        rootDescriptor(CullCounters + uav, D3D12_ROOT_PARAMETER_TYPE_UAV, uav);

    D3D12_DESCRIPTOR_RANGE ranges[4]{};
    const UINT tableRoots[4] = { CullHzbTable, CullDepthTable, CullHzbSrcTable, CullHzbDstTable };
    const D3D12_DESCRIPTOR_RANGE_TYPE tableTypes[4] =
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, D3D12_DESCRIPTOR_RANGE_TYPE_UAV };
    const UINT tableRegisters[4] = { 5, 6, 9, 10 };
    for (int i = 0; i < 4; ++i)
    {
        ranges[i].RangeType = tableTypes[i];
        ranges[i].NumDescriptors = 1;
        ranges[i].BaseShaderRegister = tableRegisters[i];
        ranges[i].OffsetInDescriptorsFromTableStart = 0;
        params[tableRoots[i]].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[tableRoots[i]].DescriptorTable.NumDescriptorRanges = 1;
        params[tableRoots[i]].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[tableRoots[i]].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = CullRootCount;
    desc.pParameters = params;

    ComPtr<ID3DBlob> serialized, errors;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors))
        || FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&mCullRootSignature))))
    {
        mLastError = "VirtualGeometryRenderer: failed to create the cull root signature.";
        return false;
    }

    for (int pass = 0; pass < PassCount; ++pass)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = mCullRootSignature.Get();
        pso.CS = mCullShaders[pass].GetBytecode();
        if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&mCullPipelines[pass]))))
        {
            mLastError = "VirtualGeometryRenderer: failed to create a cull pipeline.";
            return false;
        }
    }
    return true;
}

bool VirtualGeometryRenderer::CreateCommandSignatures()
{
    ID3D12Device* device = DX12Context_GetDevice();

    auto create = [&](D3D12_INDIRECT_ARGUMENT_TYPE type, UINT stride, ComPtr<ID3D12CommandSignature>& out)
    {
        D3D12_INDIRECT_ARGUMENT_DESC argument{};
        argument.Type = type;
        D3D12_COMMAND_SIGNATURE_DESC desc{};
        desc.ByteStride = stride;
        desc.NumArgumentDescs = 1;
        desc.pArgumentDescs = &argument;
        return SUCCEEDED(device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&out)));
    };

    if (!create(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH, 16, mDispatchSignature)
        || !create(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW, 16, mDrawSignature))
    {
        mLastError = "VirtualGeometryRenderer: failed to create command signatures.";
        return false;
    }

    if (mMeshShaderSupported && !create(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH, 16, mDispatchMeshSignature))
    {
        PTERO_LOG_WARNING(kLogCategory, "DispatchMesh command signature unavailable; using the vertex shader path.");
        mMeshShaderSupported = false;
    }
    return true;
}

bool VirtualGeometryRenderer::CreateRasterRootSignature(float mipLodBias)
{
    if (mRasterRootSignature && mRasterMipLodBias == mipLodBias)
        return true;

    ID3D12Device* device = DX12Context_GetDevice();

    D3D12_ROOT_PARAMETER params[RasterRootCount]{};
    auto rootDescriptor = [&](UINT index, D3D12_ROOT_PARAMETER_TYPE type, UINT reg, UINT space, D3D12_SHADER_VISIBILITY visibility)
    {
        params[index].ParameterType = type;
        params[index].Descriptor.ShaderRegister = reg;
        params[index].Descriptor.RegisterSpace = space;
        params[index].ShaderVisibility = visibility;
    };

    rootDescriptor(RasterDrawConstants, D3D12_ROOT_PARAMETER_TYPE_CBV, 3, 0, D3D12_SHADER_VISIBILITY_ALL);
    rootDescriptor(RasterMaterial, D3D12_ROOT_PARAMETER_TYPE_CBV, 1, 0, D3D12_SHADER_VISIBILITY_ALL);
    rootDescriptor(RasterRain, D3D12_ROOT_PARAMETER_TYPE_CBV, 2, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    D3D12_DESCRIPTOR_RANGE textureRanges[8]{};
    for (UINT i = 0; i < 8; ++i)
    {
        textureRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        textureRanges[i].NumDescriptors = 1;
        textureRanges[i].BaseShaderRegister = i;
        textureRanges[i].RegisterSpace = 0;
        textureRanges[i].OffsetInDescriptorsFromTableStart = 0;
        params[RasterTextures + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[RasterTextures + i].DescriptorTable.NumDescriptorRanges = 1;
        params[RasterTextures + i].DescriptorTable.pDescriptorRanges = &textureRanges[i];
        params[RasterTextures + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    params[RasterBin].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[RasterBin].Constants.ShaderRegister = 4;
    params[RasterBin].Constants.RegisterSpace = 0;
    params[RasterBin].Constants.Num32BitValues = 1;
    params[RasterBin].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    for (UINT i = 0; i < 7; ++i)
        rootDescriptor(RasterClusters + i, D3D12_ROOT_PARAMETER_TYPE_SRV, i, 1, D3D12_SHADER_VISIBILITY_ALL);

    // The G-Buffer's own sampler, bias included: the pixel shader is shared
    // with EntityMeshRenderer, and so must be everything it samples through.
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_ANISOTROPIC;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MipLODBias = mipLodBias;
    sampler.MaxAnisotropy = 16;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = RasterRootCount;
    desc.pParameters = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;

    ComPtr<ID3DBlob> serialized, errors;
    ComPtr<ID3D12RootSignature> rootSignature;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors))
        || FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature))))
    {
        mLastError = "VirtualGeometryRenderer: failed to create the raster root signature.";
        return false;
    }

    mRasterRootSignature = std::move(rootSignature);
    mRasterMipLodBias = mipLodBias;

    // Every pipeline was built against the old root signature.
    mGBufferPipeline.Reset();
    mDebugPipeline.Reset();
    mSunShadowPipeline.Reset();
    mShadowPagePipeline.Reset();
    mPointShadowPipeline.Reset();
    mMotionPipeline.Reset();
    mGBufferKey = {};
    return true;
}

bool VirtualGeometryRenderer::CreateRasterPipeline(
    const RasterShaders& shaders,
    const D3D12_RASTERIZER_DESC& rasterizer,
    const D3D12_DEPTH_STENCIL_DESC& depthStencil,
    UINT renderTargetCount,
    const DXGI_FORMAT* renderTargetFormats,
    DXGI_FORMAT depthFormat,
    UINT sampleCount,
    ComPtr<ID3D12PipelineState>& outPipeline,
    const char* what)
{
    ID3D12Device* device = DX12Context_GetDevice();
    outPipeline.Reset();

    HRESULT result = E_FAIL;
    if (UseMeshShaders())
    {
        ComPtr<ID3D12Device2> device2;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device2))))
            return false;

        MeshPipelineStream stream{};
        stream.RootSignature.Inner = mRasterRootSignature.Get();
        stream.MeshShader.Inner = shaders.Front;
        stream.PixelShader.Inner = shaders.Pixel;
        stream.Blend.Inner = OpaqueBlend();
        stream.SampleMask.Inner = UINT_MAX;
        stream.Rasterizer.Inner = rasterizer;
        stream.DepthStencil.Inner = depthStencil;
        stream.RenderTargets.Inner.NumRenderTargets = renderTargetCount;
        for (UINT i = 0; i < renderTargetCount; ++i)
            stream.RenderTargets.Inner.RTFormats[i] = renderTargetFormats[i];
        stream.DepthFormat.Inner = depthFormat;
        stream.SampleDesc.Inner = DXGI_SAMPLE_DESC{ sampleCount, 0 };

        const D3D12_PIPELINE_STATE_STREAM_DESC streamDesc{ sizeof(stream), &stream };
        result = device2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&outPipeline));
    }
    else
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = mRasterRootSignature.Get();
        desc.VS = shaders.Front;
        desc.PS = shaders.Pixel;
        desc.BlendState = OpaqueBlend();
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState = rasterizer;
        desc.DepthStencilState = depthStencil;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = renderTargetCount;
        for (UINT i = 0; i < renderTargetCount; ++i)
            desc.RTVFormats[i] = renderTargetFormats[i];
        desc.DSVFormat = depthFormat;
        desc.SampleDesc = DXGI_SAMPLE_DESC{ sampleCount, 0 };
        result = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&outPipeline));
    }

    if (FAILED(result))
    {
        mLastError = std::string("VirtualGeometryRenderer: failed to create the ") + what + " pipeline.";
        PTERO_LOG_ERROR(kLogCategory, "%s (HRESULT 0x%08X)", mLastError.c_str(), static_cast<unsigned>(result));
        outPipeline.Reset();
        return false;
    }
    return true;
}

bool VirtualGeometryRenderer::CompileRasterShaders()
{
    if (mRasterShadersCompiled)
        return true;

    const wchar_t* file = L"Shaders\\VirtualGeometry_Raster.hlsl";
    struct Job { DX12Shader* Shader; ShaderCompileRequest Request; };
    const Job jobs[] =
    {
        { &mVertexShader,           { file, L"VgVertexMain", L"vs_6_0", ShaderStage::Vertex } },
        { &mVertexMotionShader,     { file, L"VgVertexMotion", L"vs_6_0", ShaderStage::Vertex } },
        { &mDebugPixelShader,       { file, L"VgPixelDebug", L"ps_6_0", ShaderStage::Pixel } },
        { &mPointShadowPixelShader, { file, L"VgPixelPointShadow", L"ps_6_0", ShaderStage::Pixel } },
        { &mMotionPixelShader,      { file, L"VgPixelMotion", L"ps_6_0", ShaderStage::Pixel } },
        // The G-Buffer pixel shader itself, the same request EntityMeshRenderer makes.
        { &mGBufferPixelShader,     { L"Shaders\\GBuffer.hlsl", L"PSMain", L"ps_5_0", ShaderStage::Pixel } },
    };
    for (const Job& job : jobs)
    {
        if (!job.Shader->Compile(job.Request))
        {
            mLastError = std::string("VirtualGeometry raster shader compile failed: ")
                + (job.Shader->GetLastErrorMessage() ? job.Shader->GetLastErrorMessage() : "unknown");
            PTERO_LOG_ERROR(kLogCategory, "%s", mLastError.c_str());
            return false;
        }
    }
    mRasterShadersCompiled = true;

    if (mMeshShaderSupported)
    {
        const ShaderCompileRequest meshRequest{ file, L"VgMeshMain", L"ms_6_5", ShaderStage::Mesh };
        const ShaderCompileRequest motionRequest{ file, L"VgMeshMotion", L"ms_6_5", ShaderStage::Mesh };
        mMeshShadersCompiled = mMeshShader.Compile(meshRequest) && mMeshMotionShader.Compile(motionRequest);
        if (!mMeshShadersCompiled)
        {
            PTERO_LOG_WARNING(kLogCategory, "Mesh shaders failed to compile (%s); using the vertex shader path.",
                mMeshShader.GetLastErrorMessage() ? mMeshShader.GetLastErrorMessage()
                    : (mMeshMotionShader.GetLastErrorMessage() ? mMeshMotionShader.GetLastErrorMessage() : "unknown"));
            mMeshShaderSupported = false;
        }
    }
    return true;
}

bool VirtualGeometryRenderer::EnsureRasterPipelines(
    DXGI_FORMAT albedoFormat, DXGI_FORMAT normalFormat, DXGI_FORMAT materialFormat,
    DXGI_FORMAT depthFormat, UINT sampleCount)
{
    if (!CreateRasterRootSignature(mFrame.TextureMipLodBias) || !CompileRasterShaders())
        return false;

    const RasterKey key{ albedoFormat, normalFormat, materialFormat, depthFormat, sampleCount,
                         mFrame.WireframeEnabled, UseMeshShaders() };
    if (key == mGBufferKey && mGBufferPipeline && mDebugPipeline)
        return true;

    const D3D12_SHADER_BYTECODE front = UseMeshShaders() ? mMeshShader.GetBytecode() : mVertexShader.GetBytecode();
    const DXGI_FORMAT targets[3] = { albedoFormat, normalFormat, materialFormat };
    const D3D12_RASTERIZER_DESC rasterizer = Rasterizer(D3D12_CULL_MODE_BACK, mFrame.WireframeEnabled);

    if (!CreateRasterPipeline({ front, mGBufferPixelShader.GetBytecode() }, rasterizer, DepthTest(true),
            3, targets, depthFormat, sampleCount, mGBufferPipeline, "G-Buffer")
        || !CreateRasterPipeline({ front, mDebugPixelShader.GetBytecode() }, rasterizer, DepthTest(true),
            3, targets, depthFormat, sampleCount, mDebugPipeline, "debug view"))
    {
        mGBufferKey = {};
        return false;
    }

    mGBufferKey = key;
    return true;
}

bool VirtualGeometryRenderer::EnsureShadowPipelines(float pointSlopeScaledDepthBias)
{
    // The shadow passes run before the G-Buffer pass, so on the first frame
    // they are the ones that find nothing compiled yet.
    if (!CreateRasterRootSignature(mFrame.TextureMipLodBias) || !CompileRasterShaders())
        return false;

    const bool meshShaders = UseMeshShaders();
    if (mShadowPipelinesMeshShaders != meshShaders)
    {
        mSunShadowPipeline.Reset();
        mShadowPagePipeline.Reset();
        mPointShadowPipeline.Reset();
        mShadowPipelinesMeshShaders = meshShaders;
    }

    const D3D12_SHADER_BYTECODE front = meshShaders ? mMeshShader.GetBytecode() : mVertexShader.GetBytecode();

    // Same raster state ShadowMapRenderer's own depth pipeline uses.
    if (!mSunShadowPipeline
        && !CreateRasterPipeline({ front, {} }, Rasterizer(D3D12_CULL_MODE_BACK, false, 1.0f, 0.01f),
            DepthTest(true), 0, nullptr, DXGI_FORMAT_D32_FLOAT, 1, mSunShadowPipeline, "sun shadow"))
    {
        return false;
    }

    // A negative bias means the caller only needs the sun pipeline.
    if (pointSlopeScaledDepthBias >= 0.0f
        && (!mPointShadowPipeline || mPointShadowSlopeBias != pointSlopeScaledDepthBias))
    {
        // Same raster state as PointShadowMapRenderer: two-sided, its slope bias.
        if (!CreateRasterPipeline({ front, mPointShadowPixelShader.GetBytecode() },
                Rasterizer(D3D12_CULL_MODE_NONE, false, pointSlopeScaledDepthBias, 0.0f),
                DepthTest(true), 0, nullptr, DXGI_FORMAT_D32_FLOAT, 1, mPointShadowPipeline, "point shadow"))
        {
            return false;
        }
        mPointShadowSlopeBias = pointSlopeScaledDepthBias;
    }
    return true;
}

bool VirtualGeometryRenderer::EnsureMotionPipeline(DXGI_FORMAT targetFormat)
{
    if (!mRasterRootSignature || !mRasterShadersCompiled)
        return false;

    const bool meshShaders = UseMeshShaders();
    if (mMotionPipeline && mMotionTargetFormat == targetFormat && mMotionPipelineMeshShaders == meshShaders)
        return true;

    const D3D12_SHADER_BYTECODE front = meshShaders ? mMeshMotionShader.GetBytecode() : mVertexMotionShader.GetBytecode();
    if (!CreateRasterPipeline({ front, mMotionPixelShader.GetBytecode() }, Rasterizer(D3D12_CULL_MODE_BACK),
            DepthTest(false), 1, &targetFormat, DXGI_FORMAT_UNKNOWN, 1, mMotionPipeline, "motion vector"))
    {
        return false;
    }
    mMotionTargetFormat = targetFormat;
    mMotionPipelineMeshShaders = meshShaders;
    return true;
}

// ---------------------------------------------------------------------------
// Assets
// ---------------------------------------------------------------------------

VirtualGeometryRenderer::AssetEntry& VirtualGeometryRenderer::RequestAsset(const std::shared_ptr<Mesh>& mesh)
{
    auto it = mAssets.find(mesh.get());
    if (it != mAssets.end())
        return it->second;

    AssetEntry& entry = mAssets[mesh.get()];
    entry.Owner = mesh;
    entry.State = AssetState::Queued;

    // The same slots, in the same order, as the builder numbers clusters by.
    for (const VirtualGeometry::MaterialSlot& slot : VirtualGeometry::CollectMaterialSlots(mesh->GetLod(0)))
    {
        entry.SlotMaterialIds.push_back(slot.MaterialId);
        entry.SlotUdimTiles.push_back(slot.UdimTile);
    }
    return entry;
}

void VirtualGeometryRenderer::PollBuilds()
{
    for (auto& [meshKey, entry] : mAssets)
    {
        if (entry.State != AssetState::Building)
            continue;
        if (entry.Job.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            continue;

        VirtualGeometry::BuildResult result = entry.Job.get();
        --mActiveBuilds;
        entry.BuildSeconds = result.Seconds;
        entry.FromCache = result.FromCache;

        if (result.Mesh)
        {
            entry.Data = std::move(result.Mesh);
            entry.State = AssetState::Ready;
            const VirtualGeometry::BuiltMesh& data = *entry.Data;
            PTERO_LOG_INFO(kLogCategory,
                "%s cluster hierarchy for a %s-triangle mesh: %s clusters over %u levels (%s triangles in all levels), %.2f s.",
                result.FromCache ? "Loaded" : "Built",
                FormatCount(data.SourceTriangleCount).c_str(),
                FormatCount(data.Clusters.size()).c_str(),
                data.DagDepth,
                FormatCount(data.ClusterTriangles.size()).c_str(),
                result.Seconds);
        }
        else
        {
            entry.State = AssetState::Failed;
            entry.Error = result.Error;
            PTERO_LOG_INFO(kLogCategory, "Mesh not virtualized (drawn the ordinary way): %s.", result.Error.c_str());
        }
    }
}

void VirtualGeometryRenderer::StartQueuedBuilds()
{
    for (auto& [meshKey, entry] : mAssets)
    {
        if (mActiveBuilds >= mMaxConcurrentBuilds)
            return;
        if (entry.State != AssetState::Queued)
            continue;

        std::shared_ptr<const Mesh> owner = entry.Owner;
        entry.Job = std::async(std::launch::async, [owner]()
        {
            return VirtualGeometry::BuildOrLoad(owner->GetLod(0));
        });
        entry.State = AssetState::Building;
        ++mActiveBuilds;
    }
}

void VirtualGeometryRenderer::EvictUnusedAssets()
{
    if (mFrameCounter % 120 != 0)
        return;

    for (auto it = mAssets.begin(); it != mAssets.end(); )
    {
        AssetEntry& entry = it->second;
        // A running build cannot be abandoned (its future joins on
        // destruction), so it stays until it finishes and ages out normally.
        const bool stale = mFrameCounter - entry.LastUsedFrame > kAssetEvictionFrames;
        if (stale && entry.State != AssetState::Building)
            it = mAssets.erase(it);
        else
            ++it;
    }
}

bool VirtualGeometryRenderer::RebuildPoolsIfNeeded(ID3D12GraphicsCommandList* commandList)
{
    std::vector<const Mesh*> wanted;
    for (const auto& [meshKey, entry] : mAssets)
    {
        if (entry.State == AssetState::Ready && mFrameCounter - entry.LastUsedFrame <= kAssetEvictionFrames)
            wanted.push_back(meshKey);
    }
    std::sort(wanted.begin(), wanted.end());

    if (wanted == mPoolAssets)
        return true;

    // Several builds finishing over a few seconds would otherwise re-upload
    // the whole pool once each. Until they settle, the newcomers keep drawing
    // the ordinary way; an empty pool is filled at once.
    static constexpr std::uint64_t kRebuildInterval = 30;
    const bool newcomersOnly = std::includes(wanted.begin(), wanted.end(), mPoolAssets.begin(), mPoolAssets.end());
    if (!mPoolAssets.empty() && newcomersOnly && mActiveBuilds > 0
        && mFrameCounter - mLastPoolRebuildFrame < kRebuildInterval)
    {
        return true;
    }
    mLastPoolRebuildFrame = mFrameCounter;

    for (auto& [meshKey, entry] : mAssets)
        entry.PoolIndex = -1;

    RetireResource(std::move(mClusterPool));
    RetireResource(std::move(mClusterVertexPool));
    RetireResource(std::move(mClusterTrianglePool));
    RetireResource(std::move(mVertexPool));
    RetireResource(std::move(mAssetPool));
    mPoolAssets.clear();
    mSceneContentChanged = true;
    mStatistics.ResidentMeshes = 0;
    mStatistics.ResidentClusters = 0;
    mStatistics.ResidentTriangles = 0;
    mStatistics.PoolBytes = 0;

    // Sizes, with the vertex data taken straight from each mesh asset.
    std::uint64_t clusterCount = 0, clusterVertexCount = 0, triangleCount = 0, vertexCount = 0;
    std::vector<const Mesh*> resident;
    for (const Mesh* meshKey : wanted)
    {
        AssetEntry& entry = mAssets[meshKey];
        const VirtualGeometry::BuiltMesh& data = *entry.Data;
        if (entry.Owner->GetLod(0).Vertices.size() != data.SourceVertexCount)
        {
            // Assets are replaced, not edited, on reimport; if one ever is
            // edited in place its hierarchy no longer describes it.
            entry.State = AssetState::Failed;
            entry.Error = "the mesh changed after its cluster hierarchy was built";
            continue;
        }
        clusterCount += data.Clusters.size();
        clusterVertexCount += data.ClusterVertices.size();
        triangleCount += data.ClusterTriangles.size();
        vertexCount += data.SourceVertexCount;
        resident.push_back(meshKey);
    }

    if (resident.empty())
        return true;

    if (clusterVertexCount > 0xFFFFFFFFull || triangleCount > 0xFFFFFFFFull || vertexCount > 0xFFFFFFFFull)
    {
        mLastError = "VirtualGeometryRenderer: resident geometry exceeds 32-bit addressing.";
        return false;
    }

    const UINT64 clusterBytes = clusterCount * sizeof(VirtualGeometry::GpuCluster);
    const UINT64 clusterVertexBytes = clusterVertexCount * sizeof(std::uint32_t);
    const UINT64 triangleBytes = triangleCount * sizeof(std::uint32_t);
    const UINT64 vertexBytes = vertexCount * sizeof(Vertex);
    const UINT64 assetBytes = resident.size() * sizeof(GpuAsset);

    // Offsets in the staging buffer; each block is 256-aligned for the copies.
    auto align = [](UINT64 value) { return (value + 255) & ~UINT64(255); };
    const UINT64 clusterAt = 0;
    const UINT64 clusterVertexAt = align(clusterAt + clusterBytes);
    const UINT64 triangleAt = align(clusterVertexAt + clusterVertexBytes);
    const UINT64 vertexAt = align(triangleAt + triangleBytes);
    const UINT64 assetAt = align(vertexAt + vertexBytes);
    const UINT64 stagingBytes = assetAt + assetBytes;

    ID3D12Device* device = DX12Context_GetDevice();
    ComPtr<ID3D12Resource> staging;
    constexpr D3D12_RESOURCE_STATES kCopyDest = D3D12_RESOURCE_STATE_COPY_DEST;
    if (!CreateBuffer(device, stagingBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
            D3D12_RESOURCE_FLAG_NONE, staging, L"VG_PoolStaging")
        || !CreateBuffer(device, clusterBytes, D3D12_HEAP_TYPE_DEFAULT, kCopyDest, D3D12_RESOURCE_FLAG_NONE, mClusterPool, L"VG_ClusterPool")
        || !CreateBuffer(device, clusterVertexBytes, D3D12_HEAP_TYPE_DEFAULT, kCopyDest, D3D12_RESOURCE_FLAG_NONE, mClusterVertexPool, L"VG_ClusterVertexPool")
        || !CreateBuffer(device, triangleBytes, D3D12_HEAP_TYPE_DEFAULT, kCopyDest, D3D12_RESOURCE_FLAG_NONE, mClusterTrianglePool, L"VG_ClusterTrianglePool")
        || !CreateBuffer(device, vertexBytes, D3D12_HEAP_TYPE_DEFAULT, kCopyDest, D3D12_RESOURCE_FLAG_NONE, mVertexPool, L"VG_VertexPool")
        || !CreateBuffer(device, assetBytes, D3D12_HEAP_TYPE_DEFAULT, kCopyDest, D3D12_RESOURCE_FLAG_NONE, mAssetPool, L"VG_AssetPool"))
    {
        mLastError = "VirtualGeometryRenderer: failed to allocate the geometry pools.";
        PTERO_LOG_ERROR(kLogCategory, "%s (%.1f MB requested)", mLastError.c_str(), double(stagingBytes) / (1024.0 * 1024.0));
        mClusterPool.Reset(); mClusterVertexPool.Reset(); mClusterTrianglePool.Reset(); mVertexPool.Reset(); mAssetPool.Reset();
        return false;
    }

    std::byte* mapped = nullptr;
    if (FAILED(staging->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
        return false;

    std::uint32_t clusterBase = 0, clusterVertexBase = 0, triangleBase = 0, vertexBase = 0;
    for (std::size_t poolIndex = 0; poolIndex < resident.size(); ++poolIndex)
    {
        AssetEntry& entry = mAssets[resident[poolIndex]];
        const VirtualGeometry::BuiltMesh& data = *entry.Data;
        const MeshLod& lod = entry.Owner->GetLod(0);

        // Offsets become pool-global here, so the shaders need no per-asset
        // bases: a cluster index alone reaches everything it draws with.
        auto* clusters = reinterpret_cast<VirtualGeometry::GpuCluster*>(mapped + clusterAt) + clusterBase;
        for (std::size_t c = 0; c < data.Clusters.size(); ++c)
        {
            clusters[c] = data.Clusters[c];
            clusters[c].VertexOffset += clusterVertexBase;
            clusters[c].TriangleOffset += triangleBase;
        }

        auto* clusterVertices = reinterpret_cast<std::uint32_t*>(mapped + clusterVertexAt) + clusterVertexBase;
        for (std::size_t v = 0; v < data.ClusterVertices.size(); ++v)
            clusterVertices[v] = data.ClusterVertices[v] + vertexBase;

        std::memcpy(reinterpret_cast<std::uint32_t*>(mapped + triangleAt) + triangleBase,
            data.ClusterTriangles.data(), data.ClusterTriangles.size() * sizeof(std::uint32_t));
        std::memcpy(reinterpret_cast<Vertex*>(mapped + vertexAt) + vertexBase,
            lod.Vertices.data(), lod.Vertices.size() * sizeof(Vertex));

        GpuAsset asset;
        asset.BoundsSphere = data.BoundsSphere;
        asset.ClusterOffset = clusterBase;
        asset.ClusterCount = static_cast<std::uint32_t>(data.Clusters.size());
        reinterpret_cast<GpuAsset*>(mapped + assetAt)[poolIndex] = asset;

        entry.PoolIndex = static_cast<int>(poolIndex);
        clusterBase += static_cast<std::uint32_t>(data.Clusters.size());
        clusterVertexBase += static_cast<std::uint32_t>(data.ClusterVertices.size());
        triangleBase += static_cast<std::uint32_t>(data.ClusterTriangles.size());
        vertexBase += data.SourceVertexCount;
    }
    staging->Unmap(0, nullptr);

    commandList->CopyBufferRegion(mClusterPool.Get(), 0, staging.Get(), clusterAt, clusterBytes);
    commandList->CopyBufferRegion(mClusterVertexPool.Get(), 0, staging.Get(), clusterVertexAt, clusterVertexBytes);
    commandList->CopyBufferRegion(mClusterTrianglePool.Get(), 0, staging.Get(), triangleAt, triangleBytes);
    commandList->CopyBufferRegion(mVertexPool.Get(), 0, staging.Get(), vertexAt, vertexBytes);
    commandList->CopyBufferRegion(mAssetPool.Get(), 0, staging.Get(), assetAt, assetBytes);
    RetireResource(std::move(staging));

    // Read by the cull passes and the mesh / vertex shaders; never written again.
    D3D12_RESOURCE_BARRIER barriers[5];
    ID3D12Resource* pools[5] = { mClusterPool.Get(), mClusterVertexPool.Get(), mClusterTrianglePool.Get(), mVertexPool.Get(), mAssetPool.Get() };
    for (int i = 0; i < 5; ++i)
        barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(pools[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(5, barriers);

    mPoolAssets = std::move(resident);
    mStatistics.ResidentMeshes = static_cast<std::uint32_t>(mPoolAssets.size());
    mStatistics.ResidentClusters = static_cast<std::uint32_t>(clusterCount);
    mStatistics.ResidentTriangles = triangleCount;
    mStatistics.PoolBytes = clusterBytes + clusterVertexBytes + triangleBytes + vertexBytes + assetBytes;

    PTERO_LOG_INFO(kLogCategory, "Geometry pools rebuilt: %u meshes, %s clusters, %.1f MB.",
        mStatistics.ResidentMeshes, FormatCount(clusterCount).c_str(), double(mStatistics.PoolBytes) / (1024.0 * 1024.0));
    return true;
}

std::string VirtualGeometryRenderer::DescribeAsset(const Mesh* mesh) const
{
    const auto it = mAssets.find(mesh);
    if (it == mAssets.end())
        return mSettings.Enabled ? "Waiting for the renderer" : "Virtualized geometry is switched off (vg.enabled)";

    const AssetEntry& entry = it->second;
    switch (entry.State)
    {
    case AssetState::Queued:   return "Queued to build its cluster hierarchy";
    case AssetState::Building: return "Building its cluster hierarchy...";
    case AssetState::Failed:   return "Drawn the ordinary way: " + entry.Error;
    case AssetState::Ready:    break;
    }

    const VirtualGeometry::BuiltMesh& data = *entry.Data;
    std::ostringstream text;
    text << FormatCount(data.Clusters.size()) << " clusters, " << data.DagDepth << " levels, "
         << FormatCount(data.SourceTriangleCount) << " source triangles";
    if (entry.MaterialRejected)
        text << " - drawn the ordinary way: its material uses tessellation";
    else if (entry.PoolIndex < 0)
        text << " (uploading)";
    else if (!entry.FromCache)
        text << " (built in " << std::fixed << std::setprecision(1) << entry.BuildSeconds << " s)";
    return text.str();
}

std::shared_ptr<const VirtualGeometry::BuiltMesh> VirtualGeometryRenderer::FindBuiltMesh(const Mesh* mesh) const
{
    const auto it = mAssets.find(mesh);
    if (it == mAssets.end() || it->second.State != AssetState::Ready)
        return nullptr;
    return it->second.Data;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

std::uint32_t VirtualGeometryRenderer::NextCullSlot()
{
    const std::uint32_t slot = (std::min)(mCullSlotCursor, kCullConstantSlots - 1);
    ++mCullSlotCursor;
    return slot;
}

std::uint32_t VirtualGeometryRenderer::NextDrawSlot()
{
    const std::uint32_t slot = (std::min)(mDrawSlotCursor, kDrawConstantSlots - 1);
    ++mDrawSlotCursor;
    return slot;
}

D3D12_GPU_VIRTUAL_ADDRESS VirtualGeometryRenderer::CullConstantsAddress(std::uint32_t slot) const
{
    return mCullConstantUpload->GetGPUVirtualAddress()
        + (UINT64(mFrameSlot) * kCullConstantSlots + slot) * sizeof(CullConstants);
}

D3D12_GPU_VIRTUAL_ADDRESS VirtualGeometryRenderer::DrawConstantsAddress(std::uint32_t slot) const
{
    return mDrawConstantUpload->GetGPUVirtualAddress()
        + (UINT64(mFrameSlot) * kDrawConstantSlots + slot) * sizeof(DrawConstants);
}

bool VirtualGeometryRenderer::IsReadyToDraw() const
{
    return mInitialized && mClusterPool && mAssetPool && mHzb && !mInstances.empty();
}

void VirtualGeometryRenderer::BeginFrame(ID3D12GraphicsCommandList* commandList, const FrameInputs& inputs, EntityMeshRenderer& materials)
{
    ++mFrameCounter;
    mFrameSlot = (mFrameSlot + 1) % kFramesInFlight;
    mCullSlotCursor = 0;
    mDrawSlotCursor = 0;
    mCulledThisFrame = false;
    mOcclusionThisFrame = false;
    mPhase2Recorded = false;
    RetireExpiredResources();

    // Last frame's command list has been submitted, and every buffer it
    // touched decayed to COMMON when it finished executing.
    mCountersState = mBinCountsState = mBinRangesState = mDrawArgsState = mDispatchArgsState = mVisibleState =
        D3D12_RESOURCE_STATE_COMMON;

    mFrame = inputs;
    mEntityMask.assign(inputs.Entities != nullptr ? inputs.Entities->size() : 0, 0);
    const std::size_t previousInstanceCount = mInstances.size();
    mInstances.clear();
    mBinTable.clear();
    mMaterialBins.clear();

    if (!mInitialized || inputs.Entities == nullptr || commandList == nullptr)
        return;

    // Counters this ring slot copied back kFramesInFlight frames ago; that
    // frame's work has retired by the time its slot comes round again.
    {
        const auto* counters = reinterpret_cast<const std::uint32_t*>(mStatsReadbackMapped + mFrameSlot * kCounterBytes);
        mStatistics.VisibleClusters    = counters[CounterMainClusters / 4];
        mStatistics.VisibleTriangles   = counters[CounterMainTriangles / 4];
        mStatistics.ShadowClusters     = counters[CounterShadowClusters / 4];
        mStatistics.OcclusionRecovered = counters[CounterRecoveredClusters / 4];
        mStatistics.OverflowFlags      = counters[CounterOverflow / 4];
    }

    PollBuilds();

    // Freeze: keep culling from where the camera was when it was switched on.
    if (mSettings.FreezeCulling && !mCullingFrozen)
    {
        mCullingFrozen = true;
        mFrozenCameraPosition = inputs.CameraPosition;
        mFrozenCullViewProjection = inputs.CullViewProjection;
    }
    else if (!mSettings.FreezeCulling)
    {
        mCullingFrozen = false;
    }

    const float threshold = (std::max)(mSettings.ErrorThresholdPixels, 0.05f);
    const float pixelsPerRadian = 0.5f * static_cast<float>((std::max)(inputs.SceneHeight, 1u))
        / std::tan(0.5f * (std::max)(inputs.FovYRadians, 0.01f));
    mLodFactor = pixelsPerRadian / threshold;

    std::vector<Entity>& entities = *inputs.Entities;

    // Which entities want virtualizing and have a hierarchy to draw with.
    std::vector<std::size_t> candidates;
    if (mSettings.Enabled)
    {
        for (std::size_t i = 0; i < entities.size(); ++i)
        {
            const Entity& entity = entities[i];
            if (!entity.HasMeshComponent() || !entity.Mesh.has_value() || !entity.Mesh->MeshAsset
                || !(entity.Mesh->VirtualizedGeometry || mSettings.VirtualizeAllMeshes))
            {
                continue;
            }

            AssetEntry& asset = RequestAsset(entity.Mesh->MeshAsset);
            asset.LastUsedFrame = mFrameCounter;
            if (asset.State != AssetState::Ready)
                continue;
            // Tessellated displacement needs hull and domain stages the
            // cluster pipelines do not have; those materials keep the ordinary path.
            asset.MaterialRejected = !materials.CanDrawVirtualized(entity.Mesh->MaterialPath);
            if (asset.MaterialRejected)
                continue;
            candidates.push_back(i);
        }
    }

    StartQueuedBuilds();
    EvictUnusedAssets();

    if (!RebuildPoolsIfNeeded(commandList))
        return;
    EnsureHzb(inputs.SceneWidth, inputs.SceneHeight);

    // Instances and material bins. A bin is one (material file, sub-material)
    // pair; the camera's clusters are sorted into bins on the GPU and each bin
    // is one indirect draw with its material bound, however many instances
    // and clusters land in it.
    std::map<std::tuple<std::string, std::uint32_t, std::uint32_t>, std::uint32_t> binLookup;
    const std::uint32_t maxMaterialBins = kMaxBins - kMaxViews;

    for (std::size_t entityIndex : candidates)
    {
        const Entity& entity = entities[entityIndex];
        const AssetEntry& asset = mAssets[entity.Mesh->MeshAsset.get()];
        if (asset.PoolIndex < 0)
            continue;   // built, not uploaded yet

        const std::string& materialPath = entity.Mesh->MaterialPath;
        const std::uint32_t tableOffset = static_cast<std::uint32_t>(mBinTable.size());
        bool binsAvailable = true;
        for (std::size_t slot = 0; slot < asset.SlotMaterialIds.size(); ++slot)
        {
            const std::uint32_t materialId = asset.SlotMaterialIds[slot];
            const std::uint32_t udimTile = asset.SlotUdimTiles[slot];
            auto [it, inserted] = binLookup.try_emplace({ materialPath, materialId, udimTile }, static_cast<std::uint32_t>(mMaterialBins.size()));
            if (inserted)
            {
                if (mMaterialBins.size() >= maxMaterialBins)
                {
                    binLookup.erase(it);
                    binsAvailable = false;
                    break;
                }
                mMaterialBins.push_back({ materialPath, materialId, udimTile });
            }
            mBinTable.push_back(it->second);
        }
        if (!binsAvailable)
        {
            mBinTable.resize(tableOffset);
            continue;
        }

        const XMMATRIX model = entity.Transform.GetTransform();
        XMMATRIX previous = model;
        if (inputs.PreviousTransforms != nullptr)
        {
            const auto previousIt = inputs.PreviousTransforms->find(entityIndex);
            if (previousIt != inputs.PreviousTransforms->end())
                previous = XMLoadFloat4x4(&previousIt->second);
        }

        GpuInstance instance;
        XMFLOAT4X4 world, previousWorld;
        XMStoreFloat4x4(&world, XMMatrixTranspose(model));
        XMStoreFloat4x4(&previousWorld, XMMatrixTranspose(previous));
        for (int row = 0; row < 3; ++row)
        {
            instance.World[row] = XMFLOAT4(world.m[row][0], world.m[row][1], world.m[row][2], world.m[row][3]);
            instance.PrevWorld[row] = XMFLOAT4(previousWorld.m[row][0], previousWorld.m[row][1], previousWorld.m[row][2], previousWorld.m[row][3]);
        }

        // Axis scales are the lengths of the model matrix's basis rows.
        float scales[3];
        for (int axis = 0; axis < 3; ++axis)
            scales[axis] = XMVectorGetX(XMVector3Length(model.r[axis]));
        const float maxScale = (std::max)({ scales[0], scales[1], scales[2] });
        const float minScale = (std::min)({ scales[0], scales[1], scales[2] });
        const bool mirrored = XMVectorGetX(XMMatrixDeterminant(model)) < 0.0f;

        instance.AssetIndex = static_cast<std::uint32_t>(asset.PoolIndex);
        instance.BinTableOffset = tableOffset;
        instance.MaxScale = (std::max)(maxScale, 1e-6f);
        // A backface cone only survives a transform that preserves angles and
        // winding.
        instance.Flags = (!mirrored && maxScale - minScale <= 1e-3f * maxScale) ? 1u : 0u;

        mInstances.push_back(instance);
        mEntityMask[entityIndex] = kDrawnInGBuffer | (mSettings.Shadows ? kDrawnInShadows : 0);
    }

    if (mInstances.size() != previousInstanceCount)
        mSceneContentChanged = true;

    mStatistics.Instances = static_cast<std::uint32_t>(mInstances.size());
    mStatistics.MeshShaders = UseMeshShaders();
    mStatistics.Occlusion = false;

    if (mInstances.empty())
        return;

    if (!EnsureUploadCapacity(mInstances.size(), (std::max)(mBinTable.size(), std::size_t(1))))
    {
        std::fill(mEntityMask.begin(), mEntityMask.end(), std::uint8_t(0));
        mInstances.clear();
        return;
    }

    std::memcpy(mInstanceUploadMapped + mFrameSlot * mInstanceCapacity * sizeof(GpuInstance),
        mInstances.data(), mInstances.size() * sizeof(GpuInstance));
    std::memcpy(mBinTableUploadMapped + mFrameSlot * mBinTableCapacity * sizeof(std::uint32_t),
        mBinTable.data(), mBinTable.size() * sizeof(std::uint32_t));
}

VirtualGeometryRenderer::CullConstants VirtualGeometryRenderer::MakeCullConstants(std::uint32_t phase) const
{
    CullConstants constants;
    constants.HzbViewProjection = Transposed(phase == 0 ? mHzbViewProjection : mFrame.RasterViewProjection);
    constants.LodCameraPosition = mCullingFrozen ? mFrozenCameraPosition : mFrame.CameraPosition;
    constants.LodFactor = mLodFactor;
    constants.ConeCameraPosition = constants.LodCameraPosition;
    constants.LodZNear = (std::max)(mFrame.NearPlane, 1e-3f);
    constants.InstanceCount = static_cast<std::uint32_t>(mInstances.size());
    constants.ViewCount = mViewCount;
    constants.Phase = phase;
    constants.Flags = UseMeshShaders() ? kCullMeshShader : 0u;
    constants.HzbSize = XMFLOAT2(static_cast<float>(mHzbWidth), static_cast<float>(mHzbHeight));
    constants.HzbMipCount = mHzbMipCount;
    constants.BinCount = phase == 0
        ? static_cast<std::uint32_t>(mMaterialBins.size()) + (mViewCount - 1)
        : static_cast<std::uint32_t>(mMaterialBins.size());
    constants.ChunkCapacity = kChunkCapacity;
    constants.CandidateCapacity = kCandidateCapacity;
    constants.OccludedCapacity = kOccludedCapacity;
    constants.MaxBins = kMaxBins;
    return constants;
}

void VirtualGeometryRenderer::Transition(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource,
                                         D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES target)
{
    if (state == target)
        return;
    const D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, state, target);
    commandList->ResourceBarrier(1, &barrier);
    state = target;
}

void VirtualGeometryRenderer::UavBarrier(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    commandList->ResourceBarrier(1, &barrier);
}

void VirtualGeometryRenderer::BindCullRoot(ID3D12GraphicsCommandList* commandList, std::uint32_t constantSlot)
{
    commandList->SetComputeRootSignature(mCullRootSignature.Get());
    commandList->SetComputeRootConstantBufferView(CullConstantsRoot, CullConstantsAddress(constantSlot));
    commandList->SetComputeRootShaderResourceView(CullClusters, mClusterPool->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(CullAssets, mAssetPool->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(CullInstances,
        mInstanceUpload->GetGPUVirtualAddress() + UINT64(mFrameSlot) * mInstanceCapacity * sizeof(GpuInstance));
    commandList->SetComputeRootShaderResourceView(CullViewsRoot,
        mViewUpload->GetGPUVirtualAddress() + UINT64(mFrameSlot) * kMaxViews * sizeof(GpuView));
    commandList->SetComputeRootShaderResourceView(CullBinTable,
        mBinTableUpload->GetGPUVirtualAddress() + UINT64(mFrameSlot) * mBinTableCapacity * sizeof(std::uint32_t));
    commandList->SetComputeRootUnorderedAccessView(CullCounters, mCounters->GetGPUVirtualAddress());
    commandList->SetComputeRootUnorderedAccessView(CullChunks, mChunks->GetGPUVirtualAddress());
    commandList->SetComputeRootUnorderedAccessView(CullCandidates, mCandidates->GetGPUVirtualAddress());
    commandList->SetComputeRootUnorderedAccessView(CullOccluded, mOccluded->GetGPUVirtualAddress());
    commandList->SetComputeRootUnorderedAccessView(CullBinCounts, mBinCounts->GetGPUVirtualAddress());
    commandList->SetComputeRootUnorderedAccessView(CullBinRanges, mBinRanges->GetGPUVirtualAddress());
    commandList->SetComputeRootUnorderedAccessView(CullDrawArgs, mDrawArgs->GetGPUVirtualAddress());
    commandList->SetComputeRootUnorderedAccessView(CullDispatchArgs, mDispatchArgs->GetGPUVirtualAddress());
    commandList->SetComputeRootUnorderedAccessView(CullVisible, mVisible->GetGPUVirtualAddress());
    // Every table must name a valid descriptor whether or not the pass reads
    // it; the HZB's own views stand in for the ones a pass does not use.
    commandList->SetComputeRootDescriptorTable(CullHzbTable, mHzbSrvGpu);
    commandList->SetComputeRootDescriptorTable(CullDepthTable, mDepthSrvForCull.ptr != 0 ? mDepthSrvForCull : mHzbSrvGpu);
    commandList->SetComputeRootDescriptorTable(CullHzbSrcTable, mHzbUavGpu[0]);
    commandList->SetComputeRootDescriptorTable(CullHzbDstTable, mHzbUavGpu[0]);
}

void VirtualGeometryRenderer::BeginRasterReads(ID3D12GraphicsCommandList* commandList)
{
    Transition(commandList, mVisible.Get(), mVisibleState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(commandList, mBinRanges.Get(), mBinRangesState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(commandList, mDrawArgs.Get(), mDrawArgsState, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
}

void VirtualGeometryRenderer::CullViews(
    ID3D12GraphicsCommandList* commandList,
    bool sunShadowEnabled,
    const XMFLOAT4X4& sunViewProjection,
    const std::vector<PointShadowLight>& pointLights,
    const std::vector<ShadowPageView>* shadowPages)
{
    mFirstPageView = 0;
    mPageViewCount = 0;
    if (commandList == nullptr || !IsReadyToDraw())
        return;

    // --- Views ---------------------------------------------------------------
    mViewScratch.assign(kMaxViews, GpuView{});
    GpuView* views = mViewScratch.data();
    mViewCount = 0;
    const std::uint32_t materialBinCount = static_cast<std::uint32_t>(mMaterialBins.size());

    const bool occlusionEnabled = mSettings.OcclusionCulling && mFrame.MsaaSampleCount <= 1 && !mCullingFrozen;
    {
        GpuView& camera = views[mViewCount++];
        ExtractFrustumPlanes(mCullingFrozen ? mFrozenCullViewProjection : mFrame.CullViewProjection, camera.Planes);
        camera.Bin = kMaterialBinned;
        camera.Flags = (occlusionEnabled ? kViewOcclusion : 0u) | (mSettings.BackfaceCulling ? kViewCone : 0u);
    }

    mSunView = 0;
    mFirstPointView = 0;
    mPointViewCount = 0;
    if (mSettings.Shadows)
    {
        if (sunShadowEnabled && mViewCount < kMaxViews)
        {
            mSunView = mViewCount;
            GpuView& sun = views[mViewCount++];
            ExtractFrustumPlanes(sunViewProjection, sun.Planes);
            sun.Bin = materialBinCount + (mSunView - 1);
        }

        mFirstPointView = mViewCount;
        for (const PointShadowLight& light : pointLights)
        {
            if (mViewCount >= kMaxViews)
                break;
            const std::uint32_t viewIndex = mViewCount++;
            GpuView& view = views[viewIndex];
            view.LightSphere = XMFLOAT4(light.Position.x, light.Position.y, light.Position.z, light.Radius);
            view.Flags = kViewSphere;
            view.Bin = materialBinCount + (viewIndex - 1);
            ++mPointViewCount;
        }

        // Virtual shadow map pages. The detail is the page's texel size rather than
        // the camera's cut: the page is cached, so what it holds must not depend on
        // where the camera stood when it was drawn.
        mFirstPageView = mViewCount;
        if (shadowPages != nullptr)
        {
            for (const ShadowPageView& page : *shadowPages)
            {
                if (mViewCount >= kMaxViews)
                    break;
                const std::uint32_t viewIndex = mViewCount++;
                GpuView& view = views[viewIndex];
                ExtractFrustumPlanes(page.ViewProjection, view.Planes);
                if (page.Lod == ShadowPageView::LodMode::Perspective)
                {
                    view.Flags = kViewPointLod;
                    view.LightSphere = XMFLOAT4(page.LodOrigin.x, page.LodOrigin.y, page.LodOrigin.z, 0.0f);
                    view.LodTexelSize = page.LodTexelAngle;
                }
                else
                {
                    view.Flags = kViewTexelLod;
                    view.LodTexelSize = page.TexelWorldSize;
                }
                view.Bin = materialBinCount + (viewIndex - 1);
                ++mPageViewCount;
            }
        }
    }
    std::memcpy(mViewUploadMapped + mFrameSlot * kMaxViews * sizeof(GpuView), views, std::size_t(mViewCount) * sizeof(GpuView));

    // --- Constants -------------------------------------------------------------
    mCullSlotPhase1 = NextCullSlot();
    CullConstants constants = MakeCullConstants(0);
    const bool hzbUsable = occlusionEnabled && mHzbValid;
    if (hzbUsable)
        constants.Flags |= kCullHzbValid;
    std::memcpy(mCullConstantMapped + (mFrameSlot * kCullConstantSlots + mCullSlotPhase1) * sizeof(CullConstants),
        &constants, sizeof(constants));

    mOcclusionThisFrame = hzbUsable;
    mStatistics.Occlusion = occlusionEnabled;

    // --- Record ------------------------------------------------------------------
    ID3D12DescriptorHeap* heap = DX12Context_GetSrvDescriptorHeap();
    commandList->SetDescriptorHeaps(1, &heap);

    // Clear the counters and per-bin counts by copying zeros over them.
    Transition(commandList, mCounters.Get(), mCountersState, D3D12_RESOURCE_STATE_COPY_DEST);
    Transition(commandList, mBinCounts.Get(), mBinCountsState, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyBufferRegion(mCounters.Get(), 0, mZeroUpload.Get(), 0, kCounterBytes);
    commandList->CopyBufferRegion(mBinCounts.Get(), 0, mZeroUpload.Get(), 0, UINT64(kMaxBins) * 2 * 4);
    Transition(commandList, mCounters.Get(), mCountersState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(commandList, mBinCounts.Get(), mBinCountsState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(commandList, mBinRanges.Get(), mBinRangesState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(commandList, mDrawArgs.Get(), mDrawArgsState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(commandList, mDispatchArgs.Get(), mDispatchArgsState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(commandList, mVisible.Get(), mVisibleState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(commandList, mHzb.Get(), mHzbState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    mDepthSrvForCull = {};
    BindCullRoot(commandList, mCullSlotPhase1);

    commandList->SetPipelineState(mCullPipelines[PassInstanceCull].Get());
    commandList->Dispatch((static_cast<UINT>(mInstances.size()) + 63) / 64, mViewCount, 1);
    UavBarrier(commandList);

    commandList->SetPipelineState(mCullPipelines[PassChunkArgs].Get());
    commandList->Dispatch(1, 1, 1);
    UavBarrier(commandList);
    Transition(commandList, mDispatchArgs.Get(), mDispatchArgsState, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

    commandList->SetPipelineState(mCullPipelines[PassClusterCull].Get());
    commandList->ExecuteIndirect(mDispatchSignature.Get(), 1, mDispatchArgs.Get(), kDispatchClusterCull, nullptr, 0);
    UavBarrier(commandList);
    Transition(commandList, mDispatchArgs.Get(), mDispatchArgsState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    commandList->SetPipelineState(mCullPipelines[PassBuildBins].Get());
    commandList->Dispatch(1, 1, 1);
    UavBarrier(commandList);
    Transition(commandList, mDispatchArgs.Get(), mDispatchArgsState, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

    commandList->SetPipelineState(mCullPipelines[PassScatter].Get());
    commandList->ExecuteIndirect(mDispatchSignature.Get(), 1, mDispatchArgs.Get(), kDispatchScatter, nullptr, 0);
    UavBarrier(commandList);

    BeginRasterReads(commandList);
    mCulledThisFrame = true;
}

void VirtualGeometryRenderer::BindRasterRoot(ID3D12GraphicsCommandList* commandList, std::uint32_t drawConstantSlot)
{
    commandList->SetGraphicsRootSignature(mRasterRootSignature.Get());
    commandList->SetGraphicsRootConstantBufferView(RasterDrawConstants, DrawConstantsAddress(drawConstantSlot));
    commandList->SetGraphicsRootShaderResourceView(RasterClusters, mClusterPool->GetGPUVirtualAddress());
    commandList->SetGraphicsRootShaderResourceView(RasterInstances,
        mInstanceUpload->GetGPUVirtualAddress() + UINT64(mFrameSlot) * mInstanceCapacity * sizeof(GpuInstance));
    commandList->SetGraphicsRootShaderResourceView(RasterClusterVertices, mClusterVertexPool->GetGPUVirtualAddress());
    commandList->SetGraphicsRootShaderResourceView(RasterClusterTriangles, mClusterTrianglePool->GetGPUVirtualAddress());
    commandList->SetGraphicsRootShaderResourceView(RasterVertices, mVertexPool->GetGPUVirtualAddress());
    commandList->SetGraphicsRootShaderResourceView(RasterVisible, mVisible->GetGPUVirtualAddress());
    commandList->SetGraphicsRootShaderResourceView(RasterBinRanges, mBinRanges->GetGPUVirtualAddress());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void VirtualGeometryRenderer::DrawBins(ID3D12GraphicsCommandList* commandList, std::uint32_t firstSlot, std::uint32_t count)
{
    ID3D12CommandSignature* signature = UseMeshShaders() ? mDispatchMeshSignature.Get() : mDrawSignature.Get();
    for (std::uint32_t slot = firstSlot; slot < firstSlot + count; ++slot)
    {
        commandList->SetGraphicsRoot32BitConstant(RasterBin, slot, 0);
        commandList->ExecuteIndirect(signature, 1, mDrawArgs.Get(), UINT64(slot) * 16, nullptr, 0);
    }
}

void VirtualGeometryRenderer::RenderSunShadow(ID3D12GraphicsCommandList* commandList, const XMFLOAT4X4& lightViewProjection)
{
    if (!mCulledThisFrame || mSunView == 0 || !EnsureShadowPipelines(-1.0f))
        return;

    const std::uint32_t slot = NextDrawSlot();
    DrawConstants constants;
    constants.ViewProjection = lightViewProjection;
    std::memcpy(mDrawConstantMapped + (mFrameSlot * kDrawConstantSlots + slot) * sizeof(DrawConstants), &constants, sizeof(constants));

    BindRasterRoot(commandList, slot);
    commandList->SetPipelineState(mSunShadowPipeline.Get());
    DrawBins(commandList, static_cast<std::uint32_t>(mMaterialBins.size()) + (mSunView - 1), 1);
}

void VirtualGeometryRenderer::RenderShadowPage(
    ID3D12GraphicsCommandList* commandList,
    std::size_t pageIndex,
    const ShadowPageView& page,
    float slopeScaledDepthBias)
{
    if (!mCulledThisFrame || pageIndex >= mPageViewCount || !EnsureShadowPipelines(-1.0f))
        return;

    if (!mShadowPagePipeline || mShadowPageSlopeBias != slopeScaledDepthBias)
    {
        const D3D12_SHADER_BYTECODE front = UseMeshShaders() ? mMeshShader.GetBytecode() : mVertexShader.GetBytecode();
        // The sun pipeline's state, but clamped rather than clipped against the page's
        // near plane: casters nearer the sun than the depth range still cast.
        D3D12_RASTERIZER_DESC rasterizer = Rasterizer(D3D12_CULL_MODE_BACK, false, (std::max)(slopeScaledDepthBias, 0.0f), 0.0f);
        rasterizer.DepthClipEnable = FALSE;
        // A bias change replaces the pipeline in place; VirtualShadowMapRenderer waits
        // for the GPU before it hands out a new bias, so no recorded frame still uses it.
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
        if (!CreateRasterPipeline({ front, {} }, rasterizer, DepthTest(true), 0, nullptr, DXGI_FORMAT_D32_FLOAT, 1,
                                  pipeline, "shadow page"))
        {
            return;
        }
        mShadowPagePipeline = pipeline;
        mShadowPageSlopeBias = slopeScaledDepthBias;
    }

    const std::uint32_t slot = NextDrawSlot();
    DrawConstants constants;
    constants.ViewProjection = page.ViewProjectionTransposed;
    std::memcpy(mDrawConstantMapped + (mFrameSlot * kDrawConstantSlots + slot) * sizeof(DrawConstants), &constants, sizeof(constants));

    BindRasterRoot(commandList, slot);
    commandList->SetPipelineState(mShadowPagePipeline.Get());
    commandList->RSSetViewports(1, &page.Viewport);
    commandList->RSSetScissorRects(1, &page.Scissor);
    const std::uint32_t view = mFirstPageView + static_cast<std::uint32_t>(pageIndex);
    DrawBins(commandList, static_cast<std::uint32_t>(mMaterialBins.size()) + (view - 1), 1);
}

void VirtualGeometryRenderer::RenderPointShadowFace(
    ID3D12GraphicsCommandList* commandList,
    int lightIndex,
    const XMFLOAT4X4& faceViewProjection,
    const XMFLOAT3& lightPosition,
    float farPlane,
    float slopeScaledDepthBias)
{
    if (!mCulledThisFrame || lightIndex < 0 || static_cast<std::uint32_t>(lightIndex) >= mPointViewCount
        || !EnsureShadowPipelines(slopeScaledDepthBias))
    {
        return;
    }

    const std::uint32_t slot = NextDrawSlot();
    DrawConstants constants;
    constants.ViewProjection = faceViewProjection;
    constants.LightPosition = lightPosition;
    constants.LightFarPlane = farPlane;
    std::memcpy(mDrawConstantMapped + (mFrameSlot * kDrawConstantSlots + slot) * sizeof(DrawConstants), &constants, sizeof(constants));

    BindRasterRoot(commandList, slot);
    commandList->SetPipelineState(mPointShadowPipeline.Get());
    const std::uint32_t view = mFirstPointView + static_cast<std::uint32_t>(lightIndex);
    DrawBins(commandList, static_cast<std::uint32_t>(mMaterialBins.size()) + (view - 1), 1);
}

void VirtualGeometryRenderer::RenderGBuffer(
    ID3D12GraphicsCommandList* commandList,
    EntityMeshRenderer& materials,
    int phase,
    DXGI_FORMAT albedoFormat,
    DXGI_FORMAT normalFormat,
    DXGI_FORMAT materialFormat,
    DXGI_FORMAT depthFormat,
    UINT sampleCount)
{
    if (!mCulledThisFrame || (phase != 0 && !mPhase2Recorded) || mMaterialBins.empty())
        return;
    if (!EnsureRasterPipelines(albedoFormat, normalFormat, materialFormat, depthFormat, sampleCount))
        return;

    if (phase == 0)
    {
        mGBufferDrawSlot = NextDrawSlot();
        DrawConstants constants;
        constants.ViewProjection = Transposed(mFrame.RasterViewProjection);
        constants.DebugMode = static_cast<std::uint32_t>((std::clamp)(mSettings.DebugView, 0, 3));
        std::memcpy(mDrawConstantMapped + (mFrameSlot * kDrawConstantSlots + mGBufferDrawSlot) * sizeof(DrawConstants),
            &constants, sizeof(constants));
    }

    const D3D12_GPU_VIRTUAL_ADDRESS rainConstants = materials.GetRainSurfaceConstantsAddress();
    if (rainConstants == 0)
        return;

    const bool debugView = mSettings.DebugView > 0;
    BindRasterRoot(commandList, mGBufferDrawSlot);
    commandList->SetPipelineState(debugView ? mDebugPipeline.Get() : mGBufferPipeline.Get());
    commandList->SetGraphicsRootConstantBufferView(RasterRain, rainConstants);

    const std::uint32_t firstSlot = phase == 0 ? 0u : kMaxBins;
    for (std::uint32_t bin = 0; bin < mMaterialBins.size(); ++bin)
    {
        if (!debugView)
        {
            const MaterialBin& material = mMaterialBins[bin];
            materials.BindVirtualGeometryMaterial(commandList, material.MaterialPath, material.MaterialId, material.UdimTile, mFrame.CameraPosition);
        }
        DrawBins(commandList, firstSlot + bin, 1);
    }
}

void VirtualGeometryRenderer::BuildHzbFromDepth(
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* depthResource,
    D3D12_RESOURCE_STATES& depthState,
    D3D12_GPU_DESCRIPTOR_HANDLE depthSrv)
{
    const D3D12_RESOURCE_STATES originalDepthState = depthState;
    Transition(commandList, depthResource, depthState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(commandList, mHzb.Get(), mHzbState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap* heap = DX12Context_GetSrvDescriptorHeap();
    commandList->SetDescriptorHeaps(1, &heap);

    mDepthSrvForCull = depthSrv;
    UINT sourceWidth = mHzbSourceWidth;
    UINT sourceHeight = mHzbSourceHeight;
    for (UINT mip = 0; mip < mHzbMipCount; ++mip)
    {
        const UINT width = (std::max)(mHzbWidth >> mip, 1u);
        const UINT height = (std::max)(mHzbHeight >> mip, 1u);

        const std::uint32_t slot = NextCullSlot();
        CullConstants constants = MakeCullConstants(0);
        constants.HzbSrcSize = XMUINT2(sourceWidth, sourceHeight);
        constants.HzbDstSize = XMUINT2(width, height);
        std::memcpy(mCullConstantMapped + (mFrameSlot * kCullConstantSlots + slot) * sizeof(CullConstants),
            &constants, sizeof(constants));

        if (mip == 0)
        {
            BindCullRoot(commandList, slot);
            commandList->SetPipelineState(mCullPipelines[PassHzbFromDepth].Get());
            commandList->SetComputeRootDescriptorTable(CullHzbDstTable, mHzbUavGpu[0]);
        }
        else
        {
            commandList->SetComputeRootConstantBufferView(CullConstantsRoot, CullConstantsAddress(slot));
            if (mip == 1)
                commandList->SetPipelineState(mCullPipelines[PassHzbDownsample].Get());
            commandList->SetComputeRootDescriptorTable(CullHzbSrcTable, mHzbUavGpu[mip - 1]);
            commandList->SetComputeRootDescriptorTable(CullHzbDstTable, mHzbUavGpu[mip]);
        }

        commandList->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        UavBarrier(commandList, mHzb.Get());

        sourceWidth = width;
        sourceHeight = height;
    }
    mDepthSrvForCull = {};

    Transition(commandList, mHzb.Get(), mHzbState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(commandList, depthResource, depthState, originalDepthState);
}

bool VirtualGeometryRenderer::RunOcclusionPass(
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* depthResource,
    D3D12_RESOURCE_STATES& depthState,
    D3D12_GPU_DESCRIPTOR_HANDLE depthSrv)
{
    if (!mCulledThisFrame || !mOcclusionThisFrame || depthResource == nullptr || depthSrv.ptr == 0)
        return false;

    BuildHzbFromDepth(commandList, depthResource, depthState, depthSrv);

    const std::uint32_t slot = NextCullSlot();
    CullConstants constants = MakeCullConstants(1);
    constants.Flags |= kCullHzbValid;
    std::memcpy(mCullConstantMapped + (mFrameSlot * kCullConstantSlots + slot) * sizeof(CullConstants),
        &constants, sizeof(constants));

    Transition(commandList, mVisible.Get(), mVisibleState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(commandList, mBinRanges.Get(), mBinRangesState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(commandList, mDrawArgs.Get(), mDrawArgsState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(commandList, mDispatchArgs.Get(), mDispatchArgsState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    BindCullRoot(commandList, slot);

    commandList->SetPipelineState(mCullPipelines[PassOcclusionArgs].Get());
    commandList->Dispatch(1, 1, 1);
    UavBarrier(commandList);
    Transition(commandList, mDispatchArgs.Get(), mDispatchArgsState, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

    commandList->SetPipelineState(mCullPipelines[PassOcclusionRecull].Get());
    commandList->ExecuteIndirect(mDispatchSignature.Get(), 1, mDispatchArgs.Get(), kDispatchRecull, nullptr, 0);
    UavBarrier(commandList);
    Transition(commandList, mDispatchArgs.Get(), mDispatchArgsState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    commandList->SetPipelineState(mCullPipelines[PassBuildBins].Get());
    commandList->Dispatch(1, 1, 1);
    UavBarrier(commandList);
    Transition(commandList, mDispatchArgs.Get(), mDispatchArgsState, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

    commandList->SetPipelineState(mCullPipelines[PassScatter].Get());
    commandList->ExecuteIndirect(mDispatchSignature.Get(), 1, mDispatchArgs.Get(), kDispatchScatter, nullptr, 0);
    UavBarrier(commandList);

    BeginRasterReads(commandList);
    mPhase2Recorded = true;
    return true;
}

void VirtualGeometryRenderer::EndFrame(
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* depthResource,
    D3D12_RESOURCE_STATES& depthState,
    D3D12_GPU_DESCRIPTOR_HANDLE depthSrv)
{
    if (!mInitialized || commandList == nullptr)
        return;

    // Next frame's first pass tests against this frame's finished depth,
    // terrain and ordinary meshes included.
    const bool occlusionWanted = mSettings.OcclusionCulling && mFrame.MsaaSampleCount <= 1 && !mCullingFrozen;
    if (mCulledThisFrame && occlusionWanted && depthResource != nullptr && depthSrv.ptr != 0)
    {
        BuildHzbFromDepth(commandList, depthResource, depthState, depthSrv);
        mHzbViewProjection = mFrame.RasterViewProjection;
        mHzbValid = true;
    }
    else
    {
        mHzbValid = false;
    }

    if (mCulledThisFrame)
    {
        Transition(commandList, mCounters.Get(), mCountersState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        commandList->CopyBufferRegion(mStatsReadback.Get(), UINT64(mFrameSlot) * kCounterBytes,
            mCounters.Get(), 0, kCounterBytes);
    }
    else
    {
        std::memset(mStatsReadbackMapped + mFrameSlot * kCounterBytes, 0, kCounterBytes);
    }
}

void VirtualGeometryRenderer::RenderMotionVectors(
    ID3D12GraphicsCommandList* commandList,
    const XMFLOAT4X4& viewProjection,
    const XMFLOAT4X4& previousViewProjection,
    DXGI_FORMAT targetFormat)
{
    if (!mCulledThisFrame || mMaterialBins.empty() || !EnsureMotionPipeline(targetFormat))
        return;

    const std::uint32_t slot = NextDrawSlot();
    DrawConstants constants;
    constants.ViewProjection = Transposed(viewProjection);
    constants.PreviousViewProjection = Transposed(previousViewProjection);
    std::memcpy(mDrawConstantMapped + (mFrameSlot * kDrawConstantSlots + slot) * sizeof(DrawConstants), &constants, sizeof(constants));

    const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(mFrame.SceneWidth), static_cast<float>(mFrame.SceneHeight), 0.0f, 1.0f };
    const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(mFrame.SceneWidth), static_cast<LONG>(mFrame.SceneHeight) };
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    BindRasterRoot(commandList, slot);
    commandList->SetPipelineState(mMotionPipeline.Get());
    const std::uint32_t binCount = static_cast<std::uint32_t>(mMaterialBins.size());
    DrawBins(commandList, 0, binCount);
    if (mPhase2Recorded)
        DrawBins(commandList, kMaxBins, binCount);
}
