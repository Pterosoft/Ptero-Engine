#include "pch.h"
#include "ImageSharpenRenderer.h"

#include <array>
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
    constexpr DXGI_FORMAT ImageSharpenFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    ComPtr<ID3D12Resource> MakeTex2D(
        ID3D12Device* device,
        UINT width,
        UINT height,
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
        desc.Format = ImageSharpenFormat;
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
}

bool ImageSharpenRenderer::Initialize(UINT width, UINT height)
{
    if (width == 0 || height == 0)
        return false;

    mLastError.clear();

    try
    {
        const bool pipelineNeeded = !mIsInitialized;
        const bool texturesNeeded = pipelineNeeded || width != mWidth || height != mHeight;

        if (pipelineNeeded && !CreatePipeline())
            return false;

        if (texturesNeeded && !CreateTextures(width, height))
            return false;

        mWidth = width;
        mHeight = height;
        mIsInitialized = true;
        return true;
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("ImageSharpenRenderer::Initialize: ") + ex.what();
        return false;
    }
}

void ImageSharpenRenderer::Shutdown()
{
    if (mConstantBuffer && mMappedCb)
    {
        mConstantBuffer->Unmap(0, nullptr);
        mMappedCb = nullptr;
    }

    mConstantBuffer.Reset();
    mOutputTexture.Reset();
    mComputeHeap.Reset();
    mPipelineState.Reset();
    mRootSignature.Reset();
    mIsInitialized = false;
}

bool ImageSharpenRenderer::CreatePipeline()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
    {
        mLastError = "ImageSharpenRenderer: null device.";
        return false;
    }

    const ShaderCompileRequest csReq
    {
        L"Shaders\\ImageSharpen.hlsl",
        L"CSMain",
        L"cs_5_0",
        ShaderStage::Compute
    };

    if (!mComputeShader.Compile(csReq))
    {
        mLastError = std::string("ImageSharpenRenderer: shader compile failed: ")
            + (mComputeShader.GetLastErrorMessage() ? mComputeShader.GetLastErrorMessage() : "unknown");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
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
    rsDesc.NumParameters = static_cast<UINT>(std::size(params));
    rsDesc.pParameters = params;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> serialized, errors;
    DX12_THROW_IF_FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
    DX12_THROW_IF_FAILED(device->CreateRootSignature(
        0,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&mRootSignature)));

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = mRootSignature.Get();
    psoDesc.CS = mComputeShader.GetBytecode();
    DX12_THROW_IF_FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mPipelineState)));

    const UINT64 cbSize = (sizeof(SharpenCbData) + 255ull) & ~255ull;
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
    heapDesc.NumDescriptors = 2;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    DX12_THROW_IF_FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mComputeHeap)));
    mComputeHeapStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    return true;
}

bool ImageSharpenRenderer::CreateTextures(UINT width, UINT height)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
        return false;

    if (!mUiSlotAllocated)
    {
        if (!DX12Context_AllocateSrvDescriptor(&mOutputUiSrvCpu, &mOutputUiSrvGpu))
        {
            mLastError = "ImageSharpenRenderer: failed to allocate Ui SRV slot.";
            return false;
        }

        mUiSlotAllocated = true;
        mOutputTextureId = static_cast<UiTextureID>(mOutputUiSrvGpu.ptr);
    }

    DX12Context_WaitForGPU();
    mOutputTexture.Reset();
    mOutputTexture = MakeTex2D(
        device,
        width,
        height,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = ImageSharpenFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = ImageSharpenFormat;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    D3D12_CPU_DESCRIPTOR_HANDLE slot0 = mComputeHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateShaderResourceView(mOutputTexture.Get(), &srvDesc, slot0);

    D3D12_CPU_DESCRIPTOR_HANDLE slot1 = slot0;
    slot1.ptr += mComputeHeapStride;
    device->CreateUnorderedAccessView(mOutputTexture.Get(), nullptr, &uavDesc, slot1);

    device->CreateShaderResourceView(mOutputTexture.Get(), &srvDesc, mOutputUiSrvCpu);

    return true;
}

void ImageSharpenRenderer::Apply(
    ID3D12GraphicsCommandList*  commandList,
    ID3D12Resource*             inputResource,
    D3D12_CPU_DESCRIPTOR_HANDLE inputCpuSrv,
    const SharpenSettings&      settings)
{
    if (!mIsInitialized || !commandList || !inputResource)
        return;

    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
        return;

    if (mMappedCb)
    {
        SharpenCbData cb{};
        cb.Strength = settings.ImageSharpeningStrength;
        cb.FrameWidth = mWidth;
        cb.FrameHeight = mHeight;
        std::memcpy(mMappedCb, &cb, sizeof(cb));
    }

    D3D12_CPU_DESCRIPTOR_HANDLE slot0 = mComputeHeap->GetCPUDescriptorHandleForHeapStart();
    device->CopyDescriptorsSimple(1, slot0, inputCpuSrv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_RESOURCE_BARRIER preBarriers[2]{};
    preBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        inputResource,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    preBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
        mOutputTexture.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commandList->ResourceBarrier(2, preBarriers);

    ID3D12DescriptorHeap* heaps[] = { mComputeHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(mRootSignature.Get());
    commandList->SetPipelineState(mPipelineState.Get());
    commandList->SetComputeRootConstantBufferView(0, mConstantBuffer->GetGPUVirtualAddress());

    D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = mComputeHeap->GetGPUDescriptorHandleForHeapStart();
    commandList->SetComputeRootDescriptorTable(1, gpuBase);

    D3D12_GPU_DESCRIPTOR_HANDLE uavGpu = gpuBase;
    uavGpu.ptr += mComputeHeapStride;
    commandList->SetComputeRootDescriptorTable(2, uavGpu);

    const UINT groupsX = (mWidth + 7) / 8;
    const UINT groupsY = (mHeight + 7) / 8;
    commandList->Dispatch(groupsX, groupsY, 1);

    auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(mOutputTexture.Get());
    commandList->ResourceBarrier(1, &uavBarrier);

    D3D12_RESOURCE_BARRIER postBarriers[2]{};
    postBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        mOutputTexture.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    postBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
        inputResource,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(2, postBarriers);

    ID3D12DescriptorHeap* sharedHeap[] = { DX12Context_GetSrvDescriptorHeap() };
    commandList->SetDescriptorHeaps(1, sharedHeap);
}
