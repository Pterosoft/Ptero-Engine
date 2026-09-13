#include "pch.h"
#include "SMAARenderer.h"
#include "AreaTex.h"
#include "SearchTex.h"

#include <algorithm>
#include <cstring>
#include <vector>

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
    constexpr DXGI_FORMAT ColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr DXGI_FORMAT EdgesFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr DXGI_FORMAT BlendFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr D3D12_RESOURCE_STATES ScratchSrvState = static_cast<D3D12_RESOURCE_STATES>(
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    ComPtr<ID3D12Resource> MakeTex2D(
        ID3D12Device* device,
        UINT width,
        UINT height,
        DXGI_FORMAT format,
        D3D12_RESOURCE_FLAGS flags,
        D3D12_RESOURCE_STATES initialState)
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
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = flags;

        ComPtr<ID3D12Resource> resource;
        DX12_THROW_IF_FAILED(device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            initialState,
            nullptr,
            IID_PPV_ARGS(&resource)));
        return resource;
    }
    bool CreateUploadedLookupTexture(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* uploadCommandList,
        const void* sourceBytes,
        UINT sourcePitch,
        UINT width,
        UINT height,
        DXGI_FORMAT format,
        ComPtr<ID3D12Resource>& texture,
        ComPtr<ID3D12Resource>& upload,
        std::string& error)
    {
        texture = MakeTex2D(
            device,
            width,
            height,
            format,
            D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST);

        const UINT64 uploadSize = GetRequiredIntermediateSize(texture.Get(), 0, 1);

        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        uploadHeap.CreationNodeMask = 1;
        uploadHeap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC uploadDesc{};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Width = uploadSize;
        uploadDesc.Height = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels = 1;
        uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
        uploadDesc.SampleDesc.Count = 1;
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        DX12_THROW_IF_FAILED(device->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&upload)));

        D3D12_SUBRESOURCE_DATA subresource{};
        subresource.pData = sourceBytes;
        subresource.RowPitch = sourcePitch;
        subresource.SlicePitch = static_cast<LONG_PTR>(sourcePitch) * static_cast<LONG_PTR>(height);

        if (UpdateSubresources(uploadCommandList, texture.Get(), upload.Get(), 0, 0, 1, &subresource) == 0)
        {
            error = "SMAARenderer: failed to upload embedded SMAA lookup texture data.";
            return false;
        }

        const auto toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            texture.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        uploadCommandList->ResourceBarrier(1, &toSrv);
        return true;
    }
}

bool SMAARenderer::Initialize(UINT width, UINT height, ID3D12GraphicsCommandList* uploadCommandList)
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
            if (!CreatePipeline())
                return false;
            if (!CreateLookupTextures(uploadCommandList))
                return false;
        }

        if (texturesNeeded && !CreateTextures(width, height))
            return false;

        mWidth = width;
        mHeight = height;
        mIsInitialized = true;
        return true;
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("SMAARenderer::Initialize: ") + ex.what();
        return false;
    }
}

bool SMAARenderer::CreatePipeline()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
    {
        mLastError = "SMAARenderer: null device.";
        return false;
    }

    const ShaderCompileRequest edgeReq{ L"Shaders\\SMAA_Ptero.hlsl", L"CSEdgeDetection", L"cs_6_0", ShaderStage::Compute };
    const ShaderCompileRequest weightReq{ L"Shaders\\SMAA_Ptero.hlsl", L"CSBlendWeights", L"cs_6_0", ShaderStage::Compute };
    const ShaderCompileRequest resolveReq{ L"Shaders\\SMAA_Ptero.hlsl", L"CSNeighborhoodResolve", L"cs_6_0", ShaderStage::Compute };
    if (!mEdgesShader.Compile(edgeReq) || !mWeightsShader.Compile(weightReq) || !mResolveShader.Compile(resolveReq))
    {
        const char* edgeError = mEdgesShader.GetLastErrorMessage();
        const char* weightError = mWeightsShader.GetLastErrorMessage();
        const char* resolveError = mResolveShader.GetLastErrorMessage();
        mLastError = std::string("SMAARenderer: shader compile failed: ")
            + (edgeError ? edgeError : (weightError ? weightError : (resolveError ? resolveError : "unknown")));
        return false;
    }

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 5;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 3;
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

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 3;
    rsDesc.pParameters = params;
    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].MaxAnisotropy = 1;
    samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    samplers[0].MinLOD = 0.0f;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    samplers[1] = samplers[0];
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[1].ShaderRegister = 1;
    rsDesc.NumStaticSamplers = 2;
    rsDesc.pStaticSamplers = samplers;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    DX12_THROW_IF_FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
    DX12_THROW_IF_FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&mRootSignature)));

    auto createPso = [&](DX12Shader& shader, ComPtr<ID3D12PipelineState>& pso)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = mRootSignature.Get();
        psoDesc.CS = shader.GetBytecode();
        DX12_THROW_IF_FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso)));
    };
    createPso(mEdgesShader, mEdgesPso);
    createPso(mWeightsShader, mWeightsPso);
    createPso(mResolveShader, mResolvePso);

    const UINT64 cbSize = (sizeof(SMAACbData) + 255ull) & ~255ull;
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeap.CreationNodeMask = 1;
    uploadHeap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC cbDesc{};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = cbSize;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.Format = DXGI_FORMAT_UNKNOWN;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    DX12_THROW_IF_FAILED(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mConstantBuffer)));
    DX12_THROW_IF_FAILED(mConstantBuffer->Map(0, nullptr, &mMappedCb));

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = SlotCount;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    DX12_THROW_IF_FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mComputeHeap)));
    mComputeHeapStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    return true;
}

bool SMAARenderer::CreateTextures(UINT width, UINT height)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
        return false;

    if (!mUiSlotAllocated)
    {
        if (!DX12Context_AllocateSrvDescriptor(&mOutputUiSrvCpu, &mOutputUiSrvGpu))
        {
            mLastError = "SMAARenderer: failed to allocate output SRV slot.";
            return false;
        }
        if (!DX12Context_AllocateSrvDescriptor(&mEdgesDebugSrvCpu, &mEdgesDebugSrvGpu))
        {
            mLastError = "SMAARenderer: failed to allocate edge debug SRV slot.";
            return false;
        }
        if (!DX12Context_AllocateSrvDescriptor(&mBlendDebugSrvCpu, &mBlendDebugSrvGpu))
        {
            mLastError = "SMAARenderer: failed to allocate blend debug SRV slot.";
            return false;
        }
        mUiSlotAllocated = true;
        mOutputTextureId = static_cast<UiTextureID>(mOutputUiSrvGpu.ptr);
    }

    DX12Context_WaitForGPU();
    mEdgesTexture.Reset();
    mBlendTexture.Reset();
    mOutputTexture.Reset();

    mEdgesTexture = MakeTex2D(device, width, height, EdgesFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, ScratchSrvState);
    mBlendTexture = MakeTex2D(device, width, height, BlendFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, ScratchSrvState);
    mOutputTexture = MakeTex2D(device, width, height, ColorFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    auto heapCpu = [&](UINT slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = mComputeHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(slot) * mComputeHeapStride;
        return handle;
    };

    D3D12_SHADER_RESOURCE_VIEW_DESC colorSrv{};
    colorSrv.Format = ColorFormat;
    colorSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    colorSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    colorSrv.Texture2D.MipLevels = 1;

    D3D12_SHADER_RESOURCE_VIEW_DESC edgeSrv = colorSrv;
    edgeSrv.Format = EdgesFormat;
    D3D12_SHADER_RESOURCE_VIEW_DESC blendSrv = colorSrv;
    blendSrv.Format = BlendFormat;

    device->CreateShaderResourceView(mEdgesTexture.Get(), &edgeSrv, heapCpu(SlotEdgesSrv));
    device->CreateShaderResourceView(mBlendTexture.Get(), &blendSrv, heapCpu(SlotBlendSrv));

    D3D12_SHADER_RESOURCE_VIEW_DESC areaSrv = colorSrv;
    areaSrv.Format = DXGI_FORMAT_R8G8_UNORM;
    D3D12_SHADER_RESOURCE_VIEW_DESC searchSrv = colorSrv;
    searchSrv.Format = DXGI_FORMAT_R8_UNORM;
    device->CreateShaderResourceView(mAreaTexture.Get(), &areaSrv, heapCpu(SlotAreaSrv));
    device->CreateShaderResourceView(mSearchTexture.Get(), &searchSrv, heapCpu(SlotSearchSrv));

    D3D12_UNORDERED_ACCESS_VIEW_DESC edgeUav{};
    edgeUav.Format = EdgesFormat;
    edgeUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_UNORDERED_ACCESS_VIEW_DESC blendUav = edgeUav;
    blendUav.Format = BlendFormat;
    D3D12_UNORDERED_ACCESS_VIEW_DESC outputUav = edgeUav;
    outputUav.Format = ColorFormat;

    device->CreateUnorderedAccessView(mEdgesTexture.Get(), nullptr, &edgeUav, heapCpu(SlotEdgesUav));
    device->CreateUnorderedAccessView(mBlendTexture.Get(), nullptr, &blendUav, heapCpu(SlotBlendUav));
    device->CreateUnorderedAccessView(mOutputTexture.Get(), nullptr, &outputUav, heapCpu(SlotOutputUav));
    device->CreateShaderResourceView(mOutputTexture.Get(), &colorSrv, mOutputUiSrvCpu);
    device->CreateShaderResourceView(mEdgesTexture.Get(), &edgeSrv, mEdgesDebugSrvCpu);
    device->CreateShaderResourceView(mBlendTexture.Get(), &blendSrv, mBlendDebugSrvCpu);

    return true;
}

bool SMAARenderer::CreateLookupTextures(ID3D12GraphicsCommandList* uploadCommandList)
{
    if (mAreaTexture && mSearchTexture)
        return true;

    if (uploadCommandList == nullptr)
    {
        mLastError = "SMAARenderer: lookup texture upload requires an initialization command list.";
        return false;
    }

    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
        return false;

    static_assert(AREATEX_SIZE == AREATEX_HEIGHT * AREATEX_PITCH, "Unexpected SMAA area texture layout.");
    static_assert(SEARCHTEX_SIZE == SEARCHTEX_HEIGHT * SEARCHTEX_PITCH, "Unexpected SMAA search texture layout.");

    return CreateUploadedLookupTexture(
            device,
            uploadCommandList,
            areaTexBytes,
            AREATEX_PITCH,
            AREATEX_WIDTH,
            AREATEX_HEIGHT,
            DXGI_FORMAT_R8G8_UNORM,
            mAreaTexture,
            mAreaTextureUpload,
            mLastError)
        && CreateUploadedLookupTexture(
            device,
            uploadCommandList,
            searchTexBytes,
            SEARCHTEX_PITCH,
            SEARCHTEX_WIDTH,
            SEARCHTEX_HEIGHT,
            DXGI_FORMAT_R8_UNORM,
            mSearchTexture,
            mSearchTextureUpload,
            mLastError);
}

void SMAARenderer::Apply(
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* sourceResource,
    D3D12_CPU_DESCRIPTOR_HANDLE sourceCpuSrv,
    const SMAASettings& settings)
{
    if (!mIsInitialized || !commandList || !sourceResource)
        return;

    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
        return;

    const bool edgeDebugOnly = settings.DebugView == 1;

    if (mMappedCb)
    {
        SMAACbData cb{};
        cb.PixelSizeX = 1.0f / static_cast<float>((std::max)(mWidth, 1u));
        cb.PixelSizeY = 1.0f / static_cast<float>((std::max)(mHeight, 1u));
        cb.EdgeThreshold = (std::max)(settings.EdgeThreshold, 0.001f);
        cb.MaxSearchSteps = (std::max)(settings.MaxSearchSteps, 1.0f);
        cb.MaxSearchStepsDiag = (std::max)(settings.MaxSearchStepsDiag, 0.0f);
        cb.CornerRounding = (std::clamp)(settings.CornerRounding, 0.0f, 100.0f);
        cb.FrameWidth = mWidth;
        cb.FrameHeight = mHeight;
        cb.DebugView = static_cast<UINT>((std::max)(settings.DebugView, 0));
        std::memcpy(mMappedCb, &cb, sizeof(cb));
    }

    auto heapCpu = [&](UINT slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = mComputeHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(slot) * mComputeHeapStride;
        return handle;
    };
    auto heapGpu = [&](UINT slot)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = mComputeHeap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(slot) * mComputeHeapStride;
        return handle;
    };

    device->CopyDescriptorsSimple(1, heapCpu(SlotSourceSrv), sourceCpuSrv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    const auto sourceToCompute = CD3DX12_RESOURCE_BARRIER::Transition(
        sourceResource,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &sourceToCompute);

    const auto outputToUav = CD3DX12_RESOURCE_BARRIER::Transition(
        mOutputTexture.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commandList->ResourceBarrier(1, &outputToUav);

    D3D12_RESOURCE_BARRIER scratchToUav[2]{};
    scratchToUav[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        mEdgesTexture.Get(),
        ScratchSrvState,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    UINT scratchBarrierCount = 1;
    if (!edgeDebugOnly)
    {
        scratchToUav[1] = CD3DX12_RESOURCE_BARRIER::Transition(
            mBlendTexture.Get(),
            ScratchSrvState,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        scratchBarrierCount = 2;
    }
    commandList->ResourceBarrier(scratchBarrierCount, scratchToUav);

    ID3D12DescriptorHeap* heaps[] = { mComputeHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(mRootSignature.Get());
    commandList->SetComputeRootConstantBufferView(0, mConstantBuffer->GetGPUVirtualAddress());
    commandList->SetComputeRootDescriptorTable(1, heapGpu(SlotSourceSrv));
    commandList->SetComputeRootDescriptorTable(2, heapGpu(SlotEdgesUav));

    const UINT groupsX = (mWidth + 7) / 8;
    const UINT groupsY = (mHeight + 7) / 8;

    commandList->SetPipelineState(mEdgesPso.Get());
    commandList->Dispatch(groupsX, groupsY, 1);

    D3D12_RESOURCE_BARRIER edgesReady[2]{};
    edgesReady[0] = CD3DX12_RESOURCE_BARRIER::UAV(mEdgesTexture.Get());
    edgesReady[1] = CD3DX12_RESOURCE_BARRIER::Transition(
        mEdgesTexture.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        ScratchSrvState);
    commandList->ResourceBarrier(2, edgesReady);

    if (!edgeDebugOnly)
    {
        commandList->SetPipelineState(mWeightsPso.Get());
        commandList->Dispatch(groupsX, groupsY, 1);

        D3D12_RESOURCE_BARRIER blendReady[2]{};
        blendReady[0] = CD3DX12_RESOURCE_BARRIER::UAV(mBlendTexture.Get());
        blendReady[1] = CD3DX12_RESOURCE_BARRIER::Transition(
            mBlendTexture.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            ScratchSrvState);
        commandList->ResourceBarrier(2, blendReady);
    }

    commandList->SetPipelineState(mResolvePso.Get());
    commandList->Dispatch(groupsX, groupsY, 1);

    auto outputBarrier = CD3DX12_RESOURCE_BARRIER::UAV(mOutputTexture.Get());
    commandList->ResourceBarrier(1, &outputBarrier);

    D3D12_RESOURCE_BARRIER postBarriers[2]{};
    postBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        mOutputTexture.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    postBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
        sourceResource,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(2, postBarriers);

    ID3D12DescriptorHeap* sharedHeap[] = { DX12Context_GetSrvDescriptorHeap() };
    commandList->SetDescriptorHeaps(1, sharedHeap);
}

void SMAARenderer::Shutdown()
{
    if (mConstantBuffer && mMappedCb)
    {
        mConstantBuffer->Unmap(0, nullptr);
        mMappedCb = nullptr;
    }

    mConstantBuffer.Reset();
    mEdgesTexture.Reset();
    mBlendTexture.Reset();
    mOutputTexture.Reset();
    mAreaTexture.Reset();
    mSearchTexture.Reset();
    mAreaTextureUpload.Reset();
    mSearchTextureUpload.Reset();
    mComputeHeap.Reset();
    mEdgesPso.Reset();
    mWeightsPso.Reset();
    mResolvePso.Reset();
    mRootSignature.Reset();
    mOutputTextureId = UiTextureID_Invalid;
    mIsInitialized = false;
}
