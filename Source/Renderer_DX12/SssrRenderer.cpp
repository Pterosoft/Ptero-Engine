#include "pch.h"
#include "SssrRenderer.h"

#include <algorithm>
#include <cstring>
#include <iterator>

using Microsoft::WRL::ComPtr;

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
    // Eric Heitz's blue-noise sampler tables (sobol_256spp_256d, rankingTile, scramblingTile),
    // as shipped with the SSSR 1.3 sample. The 1 spp variant is the one the sample uses.
#include "../SDKs/FidelityFX-SSSR-1.3/sample/libs/samplerCPP/samplerBlueNoiseErrorDistribution_128x128_OptimizedFor_2d2d2d2d_1spp.cpp"

    // Must match the scene colour target: the result is copied straight back over it.
    constexpr DXGI_FORMAT OutputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    // Denoiser formats are the sample's, except the normal, which the sample inherited from
    // Cauldron's G-Buffer and which here is the pass's own decoded copy.
    constexpr DXGI_FORMAT DepthFormat           = DXGI_FORMAT_R32_FLOAT;
    constexpr DXGI_FORMAT NormalFormat          = DXGI_FORMAT_R10G10B10A2_UNORM;
    constexpr DXGI_FORMAT RoughnessFormat       = DXGI_FORMAT_R8_UNORM;
    constexpr DXGI_FORMAT RadianceFormat        = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr DXGI_FORMAT AverageRadianceFormat = DXGI_FORMAT_R11G11B10_FLOAT;
    constexpr DXGI_FORMAT VarianceFormat        = DXGI_FORMAT_R16_FLOAT;
    constexpr DXGI_FORMAT SampleCountFormat     = DXGI_FORMAT_R16_FLOAT;
    constexpr DXGI_FORMAT BlueNoiseFormat       = DXGI_FORMAT_R8G8_UNORM;
    constexpr UINT        BlueNoiseSize         = 128;

    // Sssr_Common.hlsli: [0..2] intersection dispatch, [3..5] denoiser dispatch,
    // [6] ray count, [7] tile count.
    constexpr UINT IndirectArgsCount           = 8;
    constexpr UINT IntersectArgsOffsetBytes    = 0;
    constexpr UINT DenoiserArgsOffsetBytes     = 3 * sizeof(UINT);

    constexpr D3D12_RESOURCE_STATES SrvState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr D3D12_RESOURCE_STATES UavState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // The indirect passes read the counts out of the same buffer they are launched from.
    constexpr D3D12_RESOURCE_STATES ArgsReadState =
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    UINT DivideRoundingUp(UINT value, UINT divisor)
    {
        return (value + divisor - 1) / divisor;
    }

    UINT MipCount(UINT width, UINT height)
    {
        UINT levels = 1;
        for (UINT size = (std::max)(width, height); size > 1; size >>= 1)
            ++levels;
        return levels;
    }
}

bool SssrRenderer::Initialize(UINT width, UINT height)
{
    if (width == 0 || height == 0)
        return false;

    mLastError.clear();

    try
    {
        const bool pipelineNeeded = !mIsInitialized;
        const bool texturesNeeded = pipelineNeeded || width != mWidth || height != mHeight;

        if (pipelineNeeded)
        {
            if (!CreatePipelines() || !CreateBlueNoiseBuffers() || !AllocateDescriptors())
                return false;
        }

        if (texturesNeeded && !CreateSizeDependentResources(width, height))
            return false;

        mWidth = width;
        mHeight = height;
        mIsInitialized = true;
        return true;
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("SssrRenderer::Initialize: ") + ex.what();
        return false;
    }
}

void SssrRenderer::Shutdown()
{
    if (mConstantBuffer && mMappedCb)
    {
        mConstantBuffer->Unmap(0, nullptr);
        mMappedCb = nullptr;
    }
    mConstantBuffer.Reset();

    for (Pass* pass : { &mDepthDownsamplePass, &mClassifyTilesPass, &mBlueNoisePass, &mPrepareArgsPass,
                        &mIntersectPass, &mReprojectPass, &mPrefilterPass, &mResolveTemporalPass, &mApplyPass })
    {
        pass->Pipeline.Reset();
        pass->RootSignature.Reset();
    }
    mDispatchSignature.Reset();

    mSobolBuffer.Resource.Reset();
    mRankingTileBuffer.Resource.Reset();
    mScramblingTileBuffer.Resource.Reset();
    mBlueNoiseUpload.Reset();
    mBlueNoiseUploaded = false;

    mBlueNoiseTexture.Resource.Reset();
    mRayCounter.Resource.Reset();
    mIndirectArgs.Resource.Reset();
    mSpdAtomicCounter.Resource.Reset();
    mDepthHierarchy.Resource.Reset();
    mRayList.Resource.Reset();
    mDenoiserTileList.Resource.Reset();
    mReprojectedRadiance.Resource.Reset();
    mOutput.Resource.Reset();
    for (UINT i = 0; i < 2; ++i)
    {
        for (Texture* texture : { &mDepthCopy[i], &mNormal[i], &mRoughness[i], &mRadiance[i],
                                  &mAverageRadiance[i], &mVariance[i], &mSampleCount[i] })
        {
            texture->Resource.Reset();
        }
    }

    // Descriptor slots stay allocated: the shared heap never frees, so a later Initialize
    // rewrites the same slots rather than leaking new ones.
    mPendingBarriers.clear();
    mHistoryValid = false;
    mIsInitialized = false;
}

bool SssrRenderer::CreatePass(
    Pass& pass,
    const wchar_t* shaderFile,
    UINT srvTables,
    std::initializer_list<UINT> uavTableSizes,
    UINT rootSrvs,
    UINT rootUavs)
{
    ID3D12Device* device = DX12Context_GetDevice();

    // Every pass uses Wave intrinsics or shares an include with ones that do, and the
    // startup precompiler files Sssr_* under the same profile.
    const ShaderCompileRequest request
    {
        std::wstring(L"Shaders\\") + shaderFile,
        L"CSMain",
        L"cs_6_5",
        ShaderStage::Compute
    };

    if (!pass.Shader.Compile(request))
    {
        mLastError = "SssrRenderer: failed to compile ";
        for (const wchar_t* c = shaderFile; *c; ++c)
            mLastError += static_cast<char>(*c);
        mLastError += ": ";
        mLastError += pass.Shader.GetLastErrorMessage() ? pass.Shader.GetLastErrorMessage() : "unknown";
        return false;
    }

    const UINT uavTables = static_cast<UINT>(uavTableSizes.size());
    std::vector<D3D12_DESCRIPTOR_RANGE> ranges(srvTables + uavTables);
    std::vector<D3D12_ROOT_PARAMETER> params;
    params.reserve(1 + srvTables + uavTables + rootSrvs + rootUavs);

    D3D12_ROOT_PARAMETER cbv{};
    cbv.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    cbv.Descriptor.ShaderRegister = 0;
    cbv.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params.push_back(cbv);

    for (UINT i = 0; i < srvTables; ++i)
    {
        D3D12_DESCRIPTOR_RANGE& range = ranges[i];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = i;
        range.OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 1;
        param.DescriptorTable.pDescriptorRanges = &range;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(param);
    }

    UINT nextUavRegister = 0;
    UINT tableIndex = 0;
    for (UINT size : uavTableSizes)
    {
        D3D12_DESCRIPTOR_RANGE& range = ranges[srvTables + tableIndex++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = size;
        range.BaseShaderRegister = nextUavRegister;
        range.OffsetInDescriptorsFromTableStart = 0;
        nextUavRegister += size;

        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 1;
        param.DescriptorTable.pDescriptorRanges = &range;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(param);
    }

    for (UINT i = 0; i < rootSrvs; ++i)
    {
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        param.Descriptor.ShaderRegister = srvTables + i;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(param);
    }

    for (UINT i = 0; i < rootUavs; ++i)
    {
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        param.Descriptor.ShaderRegister = nextUavRegister + i;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(param);
    }

    // Clamp matters for the history taps: a reprojection just off screen must not wrap round
    // and fetch the opposite edge.
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
    rsDesc.NumParameters = static_cast<UINT>(params.size());
    rsDesc.pParameters = params.data();
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> serialized, errors;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
    {
        mLastError = "SssrRenderer: root signature serialization failed";
        if (errors)
            mLastError += std::string(": ") + static_cast<const char*>(errors->GetBufferPointer());
        return false;
    }
    DX12_THROW_IF_FAILED(device->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&pass.RootSignature)));

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = pass.RootSignature.Get();
    psoDesc.CS = pass.Shader.GetBytecode();
    DX12_THROW_IF_FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pass.Pipeline)));

    pass.SrvTables = srvTables;
    pass.UavTables = uavTables;
    pass.RootSrvs = rootSrvs;
    pass.RootUavs = rootUavs;
    return true;
}

bool SssrRenderer::CreatePipelines()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "SssrRenderer: null device.";
        return false;
    }

    // Binding counts must match the register declarations in each shader.
    //                 pass                  shader                              SRVs  UAV tables     root SRV/UAV
    if (!CreatePass(mDepthDownsamplePass, L"Sssr_DepthDownsample.hlsl",     1, { kMaxDepthMips }, 0, 1)) return false;
    if (!CreatePass(mClassifyTilesPass,   L"Sssr_ClassifyTiles.hlsl",       4, { 1, 1, 1, 1 },    0, 3)) return false;
    if (!CreatePass(mBlueNoisePass,       L"Sssr_BlueNoise.hlsl",           0, { 1 },             3, 0)) return false;
    if (!CreatePass(mPrepareArgsPass,     L"Sssr_PrepareIndirectArgs.hlsl", 0, {},                0, 2)) return false;
    if (!CreatePass(mIntersectPass,       L"Sssr_Intersect.hlsl",           5, { 1 },             2, 0)) return false;
    if (!CreatePass(mReprojectPass,       L"Sssr_Reproject.hlsl",          10, { 1, 1, 1, 1 },    2, 0)) return false;
    if (!CreatePass(mPrefilterPass,       L"Sssr_Prefilter.hlsl",           7, { 1, 1, 1 },       2, 0)) return false;
    if (!CreatePass(mResolveTemporalPass, L"Sssr_ResolveTemporal.hlsl",     6, { 1, 1 },          2, 0)) return false;
    if (!CreatePass(mApplyPass,           L"Sssr_Apply.hlsl",               6, { 1 },             0, 0)) return false;

    D3D12_INDIRECT_ARGUMENT_DESC dispatchArg{};
    dispatchArg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

    D3D12_COMMAND_SIGNATURE_DESC signatureDesc{};
    signatureDesc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    signatureDesc.NumArgumentDescs = 1;
    signatureDesc.pArgumentDescs = &dispatchArg;
    DX12_THROW_IF_FAILED(device->CreateCommandSignature(&signatureDesc, nullptr, IID_PPV_ARGS(&mDispatchSignature)));

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    const CD3DX12_RESOURCE_DESC cbDesc = CD3DX12_RESOURCE_DESC::Buffer(static_cast<UINT64>(kCbStride) * kFramesInFlight);
    DX12_THROW_IF_FAILED(device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&mConstantBuffer)));
    DX12_THROW_IF_FAILED(mConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedCb)));

    // Size-independent pass resources. Default-heap committed resources start zeroed, which
    // is the state both counters need: the passes reset them themselves after that.
    CreateBuffer(mRayCounter, 4 * sizeof(UINT), L"SSSR Ray Counter");
    CreateBuffer(mIndirectArgs, IndirectArgsCount * sizeof(UINT), L"SSSR Indirect Args");
    CreateBuffer(mSpdAtomicCounter, sizeof(UINT), L"SSSR SPD Atomic Counter");
    return true;
}

bool SssrRenderer::CreateBlueNoiseBuffers()
{
    ID3D12Device* device = DX12Context_GetDevice();

    const UINT64 sobolBytes      = sizeof(sobol_256spp_256d);
    const UINT64 rankingBytes    = sizeof(rankingTile);
    const UINT64 scramblingBytes = sizeof(scramblingTile);
    static_assert(sizeof(sobol_256spp_256d[0]) == sizeof(UINT), "the shader reads the tables as uint");

    CreateBuffer(mSobolBuffer, sobolBytes, L"SSSR Sobol");
    CreateBuffer(mRankingTileBuffer, rankingBytes, L"SSSR Ranking Tile");
    CreateBuffer(mScramblingTileBuffer, scramblingBytes, L"SSSR Scrambling Tile");

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    const CD3DX12_RESOURCE_DESC uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(sobolBytes + rankingBytes + scramblingBytes);
    DX12_THROW_IF_FAILED(device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&mBlueNoiseUpload)));

    std::uint8_t* mapped = nullptr;
    DX12_THROW_IF_FAILED(mBlueNoiseUpload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    std::memcpy(mapped, sobol_256spp_256d, sobolBytes);
    std::memcpy(mapped + sobolBytes, rankingTile, rankingBytes);
    std::memcpy(mapped + sobolBytes + rankingBytes, scramblingTile, scramblingBytes);
    mBlueNoiseUpload->Unmap(0, nullptr);

    mBlueNoiseUploaded = false;
    return true;
}

bool SssrRenderer::AllocateDescriptors()
{
    if (mDescriptorsAllocated)
        return true;

    ID3D12Device* device = DX12Context_GetDevice();
    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    auto allocate = [this](D3D12_CPU_DESCRIPTOR_HANDLE& cpu, D3D12_GPU_DESCRIPTOR_HANDLE& gpu)
    {
        if (!DX12Context_AllocateSrvDescriptor(&cpu, &gpu))
        {
            mLastError = "SssrRenderer: the shared descriptor heap is full.";
            return false;
        }
        return true;
    };

    // The downsample pass binds every pyramid level as one table, so those slots have to be
    // adjacent. The heap is a bump allocator and initialization is single-threaded, so
    // consecutive allocations are - but check rather than assume.
    for (UINT mip = 0; mip < kMaxDepthMips; ++mip)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
        if (!allocate(mDepthHierarchyMipUavCpu[mip], gpu))
            return false;
        if (mip == 0)
        {
            mDepthHierarchyMipUavTable = gpu;
        }
        else if (mDepthHierarchyMipUavCpu[mip].ptr != mDepthHierarchyMipUavCpu[0].ptr + static_cast<SIZE_T>(mip) * increment)
        {
            mLastError = "SssrRenderer: depth pyramid descriptors are not contiguous.";
            return false;
        }
    }
    if (!allocate(mDepthHierarchy.SrvCpu, mDepthHierarchy.Srv))
        return false;

    auto allocateTexture = [&](Texture& texture, bool needsSrv)
    {
        if (needsSrv && !allocate(texture.SrvCpu, texture.Srv))
            return false;
        return allocate(texture.UavCpu, texture.Uav);
    };

    if (!allocateTexture(mBlueNoiseTexture, true) || !allocateTexture(mReprojectedRadiance, true)
        || !allocateTexture(mOutput, false))
        return false;

    for (UINT i = 0; i < 2; ++i)
    {
        for (Texture* texture : { &mDepthCopy[i], &mNormal[i], &mRoughness[i], &mRadiance[i],
                                  &mAverageRadiance[i], &mVariance[i], &mSampleCount[i] })
        {
            if (!allocateTexture(*texture, true))
                return false;
        }
    }

    mDescriptorsAllocated = true;
    return true;
}

void SssrRenderer::CreateTexture(
    Texture& texture, UINT width, UINT height, DXGI_FORMAT format, const wchar_t* name, UINT mipLevels)
{
    ID3D12Device* device = DX12Context_GetDevice();

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = static_cast<UINT16>(mipLevels);
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    texture.Resource.Reset();
    texture.State = SrvState;
    DX12_THROW_IF_FAILED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, texture.State, nullptr, IID_PPV_ARGS(&texture.Resource)));
    texture.Resource->SetName(name);

    if (texture.SrvCpu.ptr != 0)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = mipLevels;
        device->CreateShaderResourceView(texture.Resource.Get(), &srvDesc, texture.SrvCpu);
    }

    if (texture.UavCpu.ptr != 0)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = format;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(texture.Resource.Get(), nullptr, &uavDesc, texture.UavCpu);
    }
}

void SssrRenderer::CreateBuffer(Buffer& buffer, UINT64 sizeInBytes, const wchar_t* name)
{
    ID3D12Device* device = DX12Context_GetDevice();

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(
        (std::max)(sizeInBytes, static_cast<UINT64>(sizeof(UINT))), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    buffer.Resource.Reset();
    buffer.State = D3D12_RESOURCE_STATE_COMMON;
    DX12_THROW_IF_FAILED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, buffer.State, nullptr, IID_PPV_ARGS(&buffer.Resource)));
    buffer.Resource->SetName(name);
}

bool SssrRenderer::CreateSizeDependentResources(UINT width, UINT height)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    // Earlier frames may still be reading the resources being replaced.
    DX12Context_WaitForGPU();

    const UINT depthMips = (std::min)(MipCount(width, height), kMaxDepthMips);
    mDepthHierarchy.UavCpu = {};   // the per-level UAVs are written below
    CreateTexture(mDepthHierarchy, width, height, DepthFormat, L"SSSR Depth Hierarchy", depthMips);
    for (UINT mip = 0; mip < kMaxDepthMips; ++mip)
    {
        // Levels the texture does not have get a null view, which SPD's stores hit harmlessly.
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DepthFormat;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = (mip < depthMips) ? mip : 0;
        device->CreateUnorderedAccessView(
            (mip < depthMips) ? mDepthHierarchy.Resource.Get() : nullptr, nullptr, &uavDesc, mDepthHierarchyMipUavCpu[mip]);
    }

    const UINT tilesX = DivideRoundingUp(width, 8);
    const UINT tilesY = DivideRoundingUp(height, 8);
    CreateBuffer(mRayList, static_cast<UINT64>(width) * height * sizeof(UINT), L"SSSR Ray List");
    CreateBuffer(mDenoiserTileList, static_cast<UINT64>(tilesX) * tilesY * sizeof(UINT), L"SSSR Denoiser Tile List");

    CreateTexture(mBlueNoiseTexture, BlueNoiseSize, BlueNoiseSize, BlueNoiseFormat, L"SSSR Blue Noise");
    CreateTexture(mReprojectedRadiance, width, height, RadianceFormat, L"SSSR Reprojected Radiance");
    CreateTexture(mOutput, width, height, OutputFormat, L"SSSR Output");

    for (UINT i = 0; i < 2; ++i)
    {
        CreateTexture(mDepthCopy[i], width, height, DepthFormat, L"SSSR Depth");
        CreateTexture(mNormal[i], width, height, NormalFormat, L"SSSR Normal");
        CreateTexture(mRoughness[i], width, height, RoughnessFormat, L"SSSR Roughness");
        CreateTexture(mRadiance[i], width, height, RadianceFormat, L"SSSR Radiance");
        CreateTexture(mAverageRadiance[i], tilesX, tilesY, AverageRadianceFormat, L"SSSR Average Radiance");
        CreateTexture(mVariance[i], width, height, VarianceFormat, L"SSSR Variance");
        CreateTexture(mSampleCount[i], width, height, SampleCountFormat, L"SSSR Sample Count");
    }

    mPendingBarriers.clear();
    mHistoryValid = false;
    return true;
}

void SssrRenderer::Require(Texture& texture, D3D12_RESOURCE_STATES state)
{
    if (texture.State == state)
        return;
    mPendingBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(texture.Resource.Get(), texture.State, state));
    texture.State = state;
}

void SssrRenderer::Require(Buffer& buffer, D3D12_RESOURCE_STATES state)
{
    if (buffer.State == state)
        return;
    mPendingBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(buffer.Resource.Get(), buffer.State, state));
    buffer.State = state;
}

void SssrRenderer::FlushBarriers(ID3D12GraphicsCommandList* commandList)
{
    // Every flush sits between two passes, and consecutive passes often keep writing the
    // same UAV (the ray counter, this frame's radiance) - one global UAV barrier covers all.
    mPendingBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(nullptr));
    commandList->ResourceBarrier(static_cast<UINT>(mPendingBarriers.size()), mPendingBarriers.data());
    mPendingBarriers.clear();
}

void SssrRenderer::Bind(
    ID3D12GraphicsCommandList* commandList,
    const Pass& pass,
    D3D12_GPU_VIRTUAL_ADDRESS constants,
    std::initializer_list<D3D12_GPU_DESCRIPTOR_HANDLE> srvs,
    std::initializer_list<D3D12_GPU_DESCRIPTOR_HANDLE> uavs,
    std::initializer_list<D3D12_GPU_VIRTUAL_ADDRESS> rootSrvs,
    std::initializer_list<D3D12_GPU_VIRTUAL_ADDRESS> rootUavs)
{
    commandList->SetComputeRootSignature(pass.RootSignature.Get());
    commandList->SetPipelineState(pass.Pipeline.Get());

    UINT parameter = 0;
    commandList->SetComputeRootConstantBufferView(parameter++, constants);
    for (const D3D12_GPU_DESCRIPTOR_HANDLE& srv : srvs)
        commandList->SetComputeRootDescriptorTable(parameter++, srv);
    for (const D3D12_GPU_DESCRIPTOR_HANDLE& uav : uavs)
        commandList->SetComputeRootDescriptorTable(parameter++, uav);
    for (const D3D12_GPU_VIRTUAL_ADDRESS address : rootSrvs)
        commandList->SetComputeRootShaderResourceView(parameter++, address);
    for (const D3D12_GPU_VIRTUAL_ADDRESS address : rootUavs)
        commandList->SetComputeRootUnorderedAccessView(parameter++, address);
}

void SssrRenderer::Dispatch(
    ID3D12GraphicsCommandList*   commandList,
    ID3D12Resource*              sceneColorResource,
    D3D12_GPU_DESCRIPTOR_HANDLE  sceneColorSrv,
    D3D12_GPU_DESCRIPTOR_HANDLE  depthSrv,
    D3D12_GPU_DESCRIPTOR_HANDLE  normalSrv,
    D3D12_GPU_DESCRIPTOR_HANDLE  materialSrv,
    D3D12_GPU_DESCRIPTOR_HANDLE  albedoSrv,
    const SsrSettings&           settings,
    const DirectX::XMFLOAT4X4&   viewProjection,
    const DirectX::XMFLOAT4X4&   inverseViewProjection,
    const DirectX::XMFLOAT3&     cameraPosition,
    const DirectX::XMFLOAT3&     cameraForward)
{
    if (!mIsInitialized || commandList == nullptr || sceneColorResource == nullptr || mMappedCb == nullptr)
        return;

    // Every input must be bound; a missing one would leave a stale descriptor in the table.
    if (sceneColorSrv.ptr == 0 || depthSrv.ptr == 0 || normalSrv.ptr == 0
        || materialSrv.ptr == 0 || albedoSrv.ptr == 0)
        return;

    const UINT cur = mBufferIndex;
    const UINT prev = 1 - mBufferIndex;
    // Debug view 2 shows the raw traced result, so the denoiser does not run at all.
    const bool denoise = settings.DebugView != 2;

    // ---------------------------------------------------------------- constants
    mCbSlot = (mCbSlot + 1) % kFramesInFlight;
    {
        SssrCbData cb{};
        cb.ViewProj = viewProjection;
        cb.InvViewProj = inverseViewProjection;
        cb.PrevViewProj = mHistoryValid ? mPrevViewProjection : viewProjection;
        cb.CameraPos = cameraPosition;
        cb.CameraForward = cameraForward;
        cb.TemporalStabilityFactor = std::clamp(settings.SssrTemporalStability, 0.0f, 1.0f);
        cb.DepthBufferThickness = (std::max)(settings.SssrDepthThickness, 1.0e-4f);
        cb.BufferWidth = mWidth;
        cb.BufferHeight = mHeight;
        cb.InvBufferWidth = 1.0f / static_cast<float>(mWidth);
        cb.InvBufferHeight = 1.0f / static_cast<float>(mHeight);
        cb.RoughnessThreshold = std::clamp(settings.MaxRoughness, 0.0f, 1.0f);
        cb.TemporalVarianceThreshold = (std::max)(settings.SssrTemporalVarianceThreshold, 0.0f);
        cb.FrameIndex = mFrameIndex;
        cb.MaxTraversalIntersections = static_cast<UINT>(std::clamp(settings.SssrMaxTraversalIntersections, 1, 1024));
        cb.MinTraversalOccupancy = static_cast<UINT>(std::clamp(settings.SssrMinTraversalOccupancy, 0, 64));
        cb.MostDetailedMip = static_cast<UINT>(std::clamp(settings.SssrMostDetailedMip, 0, 4));
        cb.SamplesPerQuad = (settings.SssrSamplesPerQuad >= 4) ? 4u : (settings.SssrSamplesPerQuad >= 2 ? 2u : 1u);
        cb.TemporalVarianceGuidedTracingEnabled = settings.SssrTemporalVarianceGuidedTracing ? 1u : 0u;
        cb.HistoryValid = mHistoryValid ? 1u : 0u;
        cb.Intensity = (std::max)(settings.Intensity, 0.0f);
        cb.DebugView = static_cast<UINT>(std::clamp(settings.DebugView, 0, 2));
        std::memcpy(mMappedCb + static_cast<size_t>(mCbSlot) * kCbStride, &cb, sizeof(cb));
    }
    const D3D12_GPU_VIRTUAL_ADDRESS constants =
        mConstantBuffer->GetGPUVirtualAddress() + static_cast<UINT64>(mCbSlot) * kCbStride;

    ID3D12DescriptorHeap* heaps[] = { DX12Context_GetSrvDescriptorHeap() };
    commandList->SetDescriptorHeaps(1, heaps);

    // ------------------------------------------------ one-time blue-noise upload
    if (!mBlueNoiseUploaded)
    {
        Require(mSobolBuffer, D3D12_RESOURCE_STATE_COPY_DEST);
        Require(mRankingTileBuffer, D3D12_RESOURCE_STATE_COPY_DEST);
        Require(mScramblingTileBuffer, D3D12_RESOURCE_STATE_COPY_DEST);
        FlushBarriers(commandList);

        const UINT64 sobolBytes = sizeof(sobol_256spp_256d);
        const UINT64 rankingBytes = sizeof(rankingTile);
        commandList->CopyBufferRegion(mSobolBuffer.Resource.Get(), 0, mBlueNoiseUpload.Get(), 0, sobolBytes);
        commandList->CopyBufferRegion(mRankingTileBuffer.Resource.Get(), 0, mBlueNoiseUpload.Get(), sobolBytes, rankingBytes);
        commandList->CopyBufferRegion(mScramblingTileBuffer.Resource.Get(), 0, mBlueNoiseUpload.Get(),
                                      sobolBytes + rankingBytes, sizeof(scramblingTile));

        Require(mSobolBuffer, SrvState);
        Require(mRankingTileBuffer, SrvState);
        Require(mScramblingTileBuffer, SrvState);
        // The upload buffer stays alive with the renderer: frames still in flight read it.
        mBlueNoiseUploaded = true;
    }

    // The scene colour is only read from compute until the copy-back at the end.
    mPendingBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
        sceneColorResource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

    // ---------------------------------------------------------------- 1. depth pyramid
    Require(mDepthHierarchy, UavState);
    Require(mSpdAtomicCounter, UavState);
    FlushBarriers(commandList);

    Bind(commandList, mDepthDownsamplePass, constants,
         { depthSrv }, { mDepthHierarchyMipUavTable }, {}, { mSpdAtomicCounter.Address() });
    commandList->Dispatch(DivideRoundingUp(mWidth, 64), DivideRoundingUp(mHeight, 64), 1);

    // ---------------------------------------------------------------- 2. classify + blue noise
    Require(mVariance[prev], SrvState);
    Require(mRadiance[cur], UavState);
    Require(mRoughness[cur], UavState);
    Require(mNormal[cur], UavState);
    Require(mDepthCopy[cur], UavState);
    Require(mRayList, UavState);
    Require(mRayCounter, UavState);
    Require(mDenoiserTileList, UavState);
    Require(mBlueNoiseTexture, UavState);
    FlushBarriers(commandList);

    Bind(commandList, mClassifyTilesPass, constants,
         { normalSrv, materialSrv, depthSrv, mVariance[prev].Srv },
         { mRadiance[cur].Uav, mRoughness[cur].Uav, mNormal[cur].Uav, mDepthCopy[cur].Uav },
         {},
         { mRayList.Address(), mRayCounter.Address(), mDenoiserTileList.Address() });
    commandList->Dispatch(DivideRoundingUp(mWidth, 8), DivideRoundingUp(mHeight, 8), 1);

    Bind(commandList, mBlueNoisePass, constants,
         {}, { mBlueNoiseTexture.Uav },
         { mSobolBuffer.Address(), mRankingTileBuffer.Address(), mScramblingTileBuffer.Address() }, {});
    commandList->Dispatch(BlueNoiseSize / 8, BlueNoiseSize / 8, 1);

    // ---------------------------------------------------------------- 3. indirect arguments
    Require(mIndirectArgs, UavState);
    FlushBarriers(commandList);

    Bind(commandList, mPrepareArgsPass, constants, {}, {}, {}, { mRayCounter.Address(), mIndirectArgs.Address() });
    commandList->Dispatch(1, 1, 1);

    // ---------------------------------------------------------------- 4. intersection
    Require(mIndirectArgs, ArgsReadState);
    Require(mRayList, SrvState);
    Require(mDenoiserTileList, SrvState);
    Require(mDepthHierarchy, SrvState);
    Require(mNormal[cur], SrvState);
    Require(mRoughness[cur], SrvState);
    Require(mBlueNoiseTexture, SrvState);
    FlushBarriers(commandList);

    Bind(commandList, mIntersectPass, constants,
         { sceneColorSrv, mDepthHierarchy.Srv, mNormal[cur].Srv, mRoughness[cur].Srv, mBlueNoiseTexture.Srv },
         { mRadiance[cur].Uav },
         { mRayList.Address(), mIndirectArgs.Address() }, {});
    commandList->ExecuteIndirect(mDispatchSignature.Get(), 1, mIndirectArgs.Resource.Get(), IntersectArgsOffsetBytes, nullptr, 0);

    // ---------------------------------------------------------------- 5. denoiser
    if (denoise)
    {
        // Reproject: this frame's trace against last frame's resolved result.
        Require(mDepthCopy[cur], SrvState);
        Require(mDepthCopy[prev], SrvState);
        Require(mRoughness[prev], SrvState);
        Require(mNormal[prev], SrvState);
        Require(mRadiance[cur], SrvState);
        Require(mRadiance[prev], SrvState);
        Require(mSampleCount[prev], SrvState);
        Require(mReprojectedRadiance, UavState);
        Require(mAverageRadiance[cur], UavState);
        Require(mVariance[cur], UavState);
        Require(mSampleCount[cur], UavState);
        FlushBarriers(commandList);

        Bind(commandList, mReprojectPass, constants,
             { mDepthCopy[cur].Srv, mRoughness[cur].Srv, mNormal[cur].Srv,
               mDepthCopy[prev].Srv, mRoughness[prev].Srv, mNormal[prev].Srv,
               mRadiance[cur].Srv, mRadiance[prev].Srv, mVariance[prev].Srv, mSampleCount[prev].Srv },
             { mReprojectedRadiance.Uav, mAverageRadiance[cur].Uav, mVariance[cur].Uav, mSampleCount[cur].Uav },
             { mDenoiserTileList.Address(), mIndirectArgs.Address() }, {});
        commandList->ExecuteIndirect(mDispatchSignature.Get(), 1, mIndirectArgs.Resource.Get(), DenoiserArgsOffsetBytes, nullptr, 0);

        // Prefilter: writes into the history half, whose contents Reproject just consumed.
        Require(mAverageRadiance[cur], SrvState);
        Require(mVariance[cur], SrvState);
        Require(mSampleCount[cur], SrvState);
        Require(mRadiance[prev], UavState);
        Require(mVariance[prev], UavState);
        Require(mSampleCount[prev], UavState);
        FlushBarriers(commandList);

        Bind(commandList, mPrefilterPass, constants,
             { mDepthCopy[cur].Srv, mRoughness[cur].Srv, mNormal[cur].Srv, mAverageRadiance[cur].Srv,
               mRadiance[cur].Srv, mVariance[cur].Srv, mSampleCount[cur].Srv },
             { mRadiance[prev].Uav, mVariance[prev].Uav, mSampleCount[prev].Uav },
             { mDenoiserTileList.Address(), mIndirectArgs.Address() }, {});
        commandList->ExecuteIndirect(mDispatchSignature.Get(), 1, mIndirectArgs.Resource.Get(), DenoiserArgsOffsetBytes, nullptr, 0);

        // Resolve temporal: the result lands back in this frame's radiance, which is both
        // what gets composited and next frame's history.
        Require(mRadiance[prev], SrvState);
        Require(mVariance[prev], SrvState);
        Require(mSampleCount[prev], SrvState);
        Require(mReprojectedRadiance, SrvState);
        Require(mRadiance[cur], UavState);
        Require(mVariance[cur], UavState);
        FlushBarriers(commandList);

        Bind(commandList, mResolveTemporalPass, constants,
             { mRoughness[cur].Srv, mAverageRadiance[cur].Srv, mRadiance[prev].Srv,
               mReprojectedRadiance.Srv, mVariance[prev].Srv, mSampleCount[prev].Srv },
             { mRadiance[cur].Uav, mVariance[cur].Uav },
             { mDenoiserTileList.Address(), mIndirectArgs.Address() }, {});
        commandList->ExecuteIndirect(mDispatchSignature.Get(), 1, mIndirectArgs.Resource.Get(), DenoiserArgsOffsetBytes, nullptr, 0);
    }

    // ---------------------------------------------------------------- 6. composite
    Require(mRadiance[cur], SrvState);
    Require(mOutput, UavState);
    FlushBarriers(commandList);

    Bind(commandList, mApplyPass, constants,
         { sceneColorSrv, mRadiance[cur].Srv, normalSrv, materialSrv, albedoSrv, depthSrv },
         { mOutput.Uav }, {}, {});
    commandList->Dispatch(DivideRoundingUp(mWidth, 8), DivideRoundingUp(mHeight, 8), 1);

    // Copy the composited result back over the scene colour so downstream passes carry on
    // reading the texture they always have.
    Require(mOutput, D3D12_RESOURCE_STATE_COPY_SOURCE);
    mPendingBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
        sceneColorResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
    FlushBarriers(commandList);

    commandList->CopyResource(sceneColorResource, mOutput.Resource.Get());

    const auto restoreSceneColor = CD3DX12_RESOURCE_BARRIER::Transition(
        sceneColorResource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &restoreSceneColor);

    // The raw view leaves no resolved result behind, so the next denoised frame starts over.
    mHistoryValid = denoise;
    mPrevViewProjection = viewProjection;
    mBufferIndex = prev;
    ++mFrameIndex;
}
