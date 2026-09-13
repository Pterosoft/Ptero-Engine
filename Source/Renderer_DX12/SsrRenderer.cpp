#include "pch.h"
#include "SsrRenderer.h"

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
    // Must match the scene colour target: the result is copied straight back over it,
    // and CopyResource requires identical format, size and sample count.
    constexpr DXGI_FORMAT SsrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
}

bool SsrRenderer::Initialize(UINT width, UINT height)
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
        mLastError = std::string("SsrRenderer::Initialize: ") + ex.what();
        return false;
    }
}

void SsrRenderer::Shutdown()
{
    if (mConstantBuffer && mMappedCb)
    {
        mConstantBuffer->Unmap(0, nullptr);
        mMappedCb = nullptr;
    }

    mConstantBuffer.Reset();
    mOutputTexture.Reset();
    mPipelineState.Reset();
    mRootSignature.Reset();
    mIsInitialized = false;
}

bool SsrRenderer::CreatePipeline()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "SsrRenderer: null device.";
        return false;
    }

    const ShaderCompileRequest csRequest
    {
        L"Shaders\\Ssr.hlsl",
        L"CSMain",
        L"cs_5_0",
        ShaderStage::Compute
    };

    if (!mComputeShader.Compile(csRequest))
    {
        mLastError = std::string("SsrRenderer: shader compile failed: ")
            + (mComputeShader.GetLastErrorMessage() ? mComputeShader.GetLastErrorMessage() : "unknown");
        return false;
    }

    // One single-descriptor table per input, so each SRV can be bound directly from the
    // handle its owner already holds in the shared heap. A single contiguous table would
    // mean copying five descriptors into a private heap every frame to make them adjacent.
    D3D12_DESCRIPTOR_RANGE srvRanges[5]{};
    for (UINT i = 0; i < 5; ++i)
    {
        srvRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[i].NumDescriptors = 1;
        srvRanges[i].BaseShaderRegister = i;   // t0..t4
        srvRanges[i].OffsetInDescriptorsFromTableStart = 0;
    }

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;   // u0
    uavRange.OffsetInDescriptorsFromTableStart = 0;

    // 0 = CBV b0, 1..5 = SRV t0..t4, 6 = UAV u0.
    D3D12_ROOT_PARAMETER params[7]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    for (UINT i = 0; i < 5; ++i)
    {
        params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[1 + i].DescriptorTable.pDescriptorRanges = &srvRanges[i];
        params[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[6].DescriptorTable.NumDescriptorRanges = 1;
    params[6].DescriptorTable.pDescriptorRanges = &uavRange;
    params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Clamping matters here: a ray that runs off the frame must not wrap around and
    // reflect the opposite edge. The shader fades these out, but the sample still happens.
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
        0,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&mRootSignature)));

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = mRootSignature.Get();
    psoDesc.CS = mComputeShader.GetBytecode();
    DX12_THROW_IF_FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mPipelineState)));

    const UINT64 cbSize = (sizeof(SsrCbData) + 255ull) & ~255ull;

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

    return true;
}

bool SsrRenderer::CreateTextures(UINT width, UINT height)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    if (!mUavSlotAllocated)
    {
        if (!DX12Context_AllocateSrvDescriptor(&mOutputUavCpu, &mOutputUavGpu))
        {
            mLastError = "SsrRenderer: failed to allocate a UAV descriptor slot.";
            return false;
        }
        mUavSlotAllocated = true;
    }

    DX12Context_WaitForGPU();
    mOutputTexture.Reset();

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
    desc.Format = SsrFormat;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    DX12_THROW_IF_FAILED(device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COPY_SOURCE,
        nullptr,
        IID_PPV_ARGS(&mOutputTexture)));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = SsrFormat;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(mOutputTexture.Get(), nullptr, &uavDesc, mOutputUavCpu);

    return true;
}

void SsrRenderer::Dispatch(
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
    const DirectX::XMFLOAT3&     cameraPosition)
{
    if (!mIsInitialized || commandList == nullptr || sceneColorResource == nullptr)
        return;

    // Every input must be bound; a missing one would leave a stale descriptor in the table.
    if (sceneColorSrv.ptr == 0 || depthSrv.ptr == 0 || normalSrv.ptr == 0
        || materialSrv.ptr == 0 || albedoSrv.ptr == 0)
        return;

    if (mMappedCb != nullptr)
    {
        SsrCbData cb{};
        cb.ViewProj = viewProjection;
        cb.InvViewProj = inverseViewProjection;
        cb.CameraPos = cameraPosition;
        cb.Intensity = (std::max)(settings.Intensity, 0.0f);
        cb.FrameWidth = mWidth;
        cb.FrameHeight = mHeight;
        cb.MaxSteps = std::clamp(settings.MaxSteps, 1, 512);
        cb.StepSize = (std::max)(settings.StepSize, 1.0e-3f);
        cb.StepGrowth = std::clamp(settings.StepGrowth, 1.0f, 2.0f);
        cb.Thickness = (std::max)(settings.Thickness, 1.0e-3f);
        cb.MaxRoughness = std::clamp(settings.MaxRoughness, 0.0f, 1.0f);
        cb.RefineSteps = std::clamp(settings.RefineSteps, 0, 16);
        cb.MaxDistance = (std::max)(settings.MaxDistance, 1.0e-3f);
        cb.EdgeFadeStart = std::clamp(settings.EdgeFadeStart, 0.0f, 0.999f);
        cb.DebugView = settings.DebugView;
        std::memcpy(mMappedCb, &cb, sizeof(cb));
    }

    D3D12_RESOURCE_BARRIER preBarriers[2]{};
    preBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        sceneColorResource,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    preBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
        mOutputTexture.Get(),
        D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commandList->ResourceBarrier(2, preBarriers);

    ID3D12DescriptorHeap* heaps[] = { DX12Context_GetSrvDescriptorHeap() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(mRootSignature.Get());
    commandList->SetPipelineState(mPipelineState.Get());
    commandList->SetComputeRootConstantBufferView(0, mConstantBuffer->GetGPUVirtualAddress());
    commandList->SetComputeRootDescriptorTable(1, sceneColorSrv);
    commandList->SetComputeRootDescriptorTable(2, depthSrv);
    commandList->SetComputeRootDescriptorTable(3, normalSrv);
    commandList->SetComputeRootDescriptorTable(4, materialSrv);
    commandList->SetComputeRootDescriptorTable(5, albedoSrv);
    commandList->SetComputeRootDescriptorTable(6, mOutputUavGpu);

    const UINT groupsX = (mWidth + 7) / 8;
    const UINT groupsY = (mHeight + 7) / 8;
    commandList->Dispatch(groupsX, groupsY, 1);

    auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(mOutputTexture.Get());
    commandList->ResourceBarrier(1, &uavBarrier);

    // Copy the composited result back over the scene colour so downstream passes carry on
    // reading the texture they always have.
    D3D12_RESOURCE_BARRIER toCopy[2]{};
    toCopy[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        mOutputTexture.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    toCopy[1] = CD3DX12_RESOURCE_BARRIER::Transition(
        sceneColorResource,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->ResourceBarrier(2, toCopy);

    commandList->CopyResource(sceneColorResource, mOutputTexture.Get());

    auto restoreSceneColor = CD3DX12_RESOURCE_BARRIER::Transition(
        sceneColorResource,
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &restoreSceneColor);
}
