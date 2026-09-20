#include "pch.h"
#include "AgxTonemapper.h"

#include <cstring>

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
    constexpr DXGI_FORMAT AgxFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    ComPtr<ID3D12Resource> MakeTex2D(
        ID3D12Device* device,
        UINT width, UINT height,
        D3D12_RESOURCE_FLAGS flags,
        D3D12_RESOURCE_STATES initialState)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask  = 1;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width              = width;
        desc.Height             = height;
        desc.DepthOrArraySize   = 1;
        desc.MipLevels          = 1;
        desc.Format             = AgxFormat;
        desc.SampleDesc.Count   = 1;
        desc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags              = flags;

        ComPtr<ID3D12Resource> resource;
        DX12_THROW_IF_FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, initialState,
            nullptr, IID_PPV_ARGS(&resource)));
        return resource;
    }
}

// ---------------------------------------------------------------------------
bool AgxTonemapper::Initialize(UINT width, UINT height)
{
    if (width == 0 || height == 0)
        return false;

    mLastError.clear();

    try
    {
        const bool pipelineNeeded = !mIsInitialized;
        const bool texturesNeeded = pipelineNeeded || (width != mWidth || height != mHeight);

        if (pipelineNeeded && !CreatePipeline())
            return false;

        if (texturesNeeded && !CreateTextures(width, height))
            return false;

        mWidth  = width;
        mHeight = height;
        mIsInitialized = true;
        return true;
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("AgxTonemapper::Initialize: ") + ex.what();
        return false;
    }
}

// ---------------------------------------------------------------------------
void AgxTonemapper::Shutdown()
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

// ---------------------------------------------------------------------------
bool AgxTonemapper::CreatePipeline()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
    {
        mLastError = "AgxTonemapper: null device.";
        return false;
    }

    // Compile compute shader
    const ShaderCompileRequest csReq
    {
        L"Shaders\\AgxTonemap.hlsl",
        L"CSMain",
        L"cs_5_0",
        ShaderStage::Compute
    };
    if (!mComputeShader.Compile(csReq))
    {
        mLastError = std::string("AgxTonemapper: shader compile failed: ")
            + (mComputeShader.GetLastErrorMessage() ? mComputeShader.GetLastErrorMessage() : "unknown");
        return false;
    }

    // Root signature:
    //   param 0 – inline CBV b0
    //   param 1 – descriptor table: t0 (input SRV)
    //   param 2 – descriptor table: u0 (output UAV)
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors     = 1;
    srvRange.BaseShaderRegister = 0; // t0
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors     = 1;
    uavRange.BaseShaderRegister = 0; // u0
    uavRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE exposureRange{};
    exposureRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    exposureRange.NumDescriptors     = 1;
    exposureRange.BaseShaderRegister = 1; // t1
    exposureRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[4]{};

    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace  = 0;
    params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &srvRange;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges   = &uavRange;
    params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    params[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges   = &exposureRange;
    params[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 4;
    rsDesc.pParameters   = params;
    rsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> serialized, errors;
    DX12_THROW_IF_FAILED(D3D12SerializeRootSignature(
        &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
    DX12_THROW_IF_FAILED(device->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&mRootSignature)));

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = mRootSignature.Get();
    psoDesc.CS             = mComputeShader.GetBytecode();
    DX12_THROW_IF_FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mPipelineState)));

    // Constant buffer (persistently mapped)
    const UINT64 cbSize = (sizeof(AgxCbData) + 255ull) & ~255ull;
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeap.CreationNodeMask = 1;
    uploadHeap.VisibleNodeMask  = 1;
    D3D12_RESOURCE_DESC cbDesc{};
    cbDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width            = cbSize;
    cbDesc.Height           = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels        = 1;
    cbDesc.Format           = DXGI_FORMAT_UNKNOWN;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    DX12_THROW_IF_FAILED(device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mConstantBuffer)));
    DX12_THROW_IF_FAILED(mConstantBuffer->Map(0, nullptr, &mMappedCb));

    // Private 3-slot shader-visible descriptor heap
    // (slot 0 = input SRV, slot 1 = output UAV, slot 2 = exposure SRV).
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 3;
    heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    DX12_THROW_IF_FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mComputeHeap)));
    mComputeHeapStride = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    return true;
}

// ---------------------------------------------------------------------------
bool AgxTonemapper::CreateTextures(UINT width, UINT height)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
        return false;

    if (!mUiSlotAllocated)
    {
        if (!DX12Context_AllocateSrvDescriptor(&mOutputUiSrvCpu, &mOutputUiSrvGpu))
        {
            mLastError = "AgxTonemapper: failed to allocate Ui SRV slot.";
            return false;
        }
        mUiSlotAllocated = true;
        mOutputTextureId = static_cast<UiTextureID>(mOutputUiSrvGpu.ptr);
    }

    DX12Context_WaitForGPU();
    mOutputTexture.Reset();

    mOutputTexture = MakeTex2D(device, width, height,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format                        = AgxFormat;
    srvDesc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping       = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2,
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
    srvDesc.Texture2D.MipLevels           = 1;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format        = AgxFormat;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    // slot 0 placeholder (overwritten each frame via CopyDescriptors)
    D3D12_CPU_DESCRIPTOR_HANDLE slot0 = mComputeHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateShaderResourceView(mOutputTexture.Get(), &srvDesc, slot0);

    // slot 1: output UAV
    D3D12_CPU_DESCRIPTOR_HANDLE slot1 = slot0;
    slot1.ptr += mComputeHeapStride;
    device->CreateUnorderedAccessView(mOutputTexture.Get(), nullptr, &uavDesc, slot1);

    // Ui shared-heap SRV
    device->CreateShaderResourceView(mOutputTexture.Get(), &srvDesc, mOutputUiSrvCpu);

    return true;
}

// ---------------------------------------------------------------------------
void AgxTonemapper::Apply(
    ID3D12GraphicsCommandList*  commandList,
    ID3D12Resource*             inputResource,
    D3D12_CPU_DESCRIPTOR_HANDLE inputCpuSrv,
    const AgxTonemapSettings&   settings,
    D3D12_CPU_DESCRIPTOR_HANDLE autoExposureCpuSrv)
{
    if (!mIsInitialized || !commandList || !inputResource)
        return;

    ID3D12Device* device = DX12Context_GetDevice();

    // Update constant buffer.
    if (mMappedCb)
    {
        AgxCbData cb{};
        auto copyGradeControl = [](AgxGradeControlCb& destination, const AgxColorGradeControl& source)
        {
            destination.Total = source.Total;
            destination.Red = source.Red;
            destination.Green = source.Green;
            destination.Blue = source.Blue;
            destination.Yellow = source.Yellow;
        };

        auto copyGradeRegion = [&copyGradeControl](AgxGradeRegionCb& destination, const AgxColorGradeRegion& source)
        {
            copyGradeControl(destination.Contrast, source.Contrast);
            copyGradeControl(destination.Gamma, source.Gamma);
            copyGradeControl(destination.Gain, source.Gain);
            copyGradeControl(destination.Saturation, source.Saturation);
            copyGradeControl(destination.Vibrance, source.Vibrance);
        };

        cb.Exposure = settings.Exposure;
        // Min == Max is legal and useful: it pins the exposure to one value.
        // The shader orders the pair itself, so no guard is needed here.
        cb.Ev100 = settings.Ev100;
        cb.Ev100Min = settings.Ev100Min;
        cb.Ev100Max = settings.Ev100Max;

        // Only claim automatic exposure when the meter actually produced a
        // buffer this frame; otherwise the shader falls back to manual rather
        // than reading a null descriptor as an exposure of zero EV.
        cb.UseAutoExposure =
            (settings.ExposureMode == AgxExposureMode::AutoHistogram && autoExposureCpuSrv.ptr != 0)
                ? 1u : 0u;

        cb.ToeStrength = settings.ToeStrength;
        cb.ShoulderStrength = settings.ShoulderStrength;

        copyGradeRegion(cb.Global, settings.Global);
        copyGradeRegion(cb.Shadows, settings.Shadows);
        copyGradeRegion(cb.Midtones, settings.Midtones);
        copyGradeRegion(cb.Highlights, settings.Highlights);

        cb.FrameWidth = mWidth;
        cb.FrameHeight = mHeight;
        std::memcpy(mMappedCb, &cb, sizeof(cb));
    }

    // Copy input SRV into slot 0 of the private heap.
    D3D12_CPU_DESCRIPTOR_HANDLE slot0 = mComputeHeap->GetCPUDescriptorHandleForHeapStart();
    device->CopyDescriptorsSimple(1, slot0, inputCpuSrv,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Slot 2 – the adapted exposure. When there is none, write a null raw-buffer
    // SRV: it reads as zero, and leaving the previous frame's descriptor there
    // would point at a resource the meter may since have released.
    D3D12_CPU_DESCRIPTOR_HANDLE slot2 = slot0;
    slot2.ptr += static_cast<SIZE_T>(mComputeHeapStride) * 2;
    if (autoExposureCpuSrv.ptr != 0)
    {
        device->CopyDescriptorsSimple(1, slot2, autoExposureCpuSrv,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    else
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
        nullSrv.Format                  = DXGI_FORMAT_R32_TYPELESS;
        nullSrv.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
        nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullSrv.Buffer.NumElements      = 4;
        nullSrv.Buffer.Flags            = D3D12_BUFFER_SRV_FLAG_RAW;
        device->CreateShaderResourceView(nullptr, &nullSrv, slot2);
    }

    // Resource barriers:
    //   input  – PIXEL_SHADER_RESOURCE -> NON_PIXEL_SHADER_RESOURCE (compute read)
    //   output – PIXEL_SHADER_RESOURCE -> UNORDERED_ACCESS           (compute write)
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

    // Bind pipeline and dispatch.
    ID3D12DescriptorHeap* heaps[] = { mComputeHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(mRootSignature.Get());
    commandList->SetPipelineState(mPipelineState.Get());

    commandList->SetComputeRootConstantBufferView(0, mConstantBuffer->GetGPUVirtualAddress());

    D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = mComputeHeap->GetGPUDescriptorHandleForHeapStart();
    commandList->SetComputeRootDescriptorTable(1, gpuBase); // t0

    D3D12_GPU_DESCRIPTOR_HANDLE uavGpu = gpuBase;
    uavGpu.ptr += mComputeHeapStride;
    commandList->SetComputeRootDescriptorTable(2, uavGpu); // u0

    D3D12_GPU_DESCRIPTOR_HANDLE exposureGpu = gpuBase;
    exposureGpu.ptr += static_cast<UINT64>(mComputeHeapStride) * 2;
    commandList->SetComputeRootDescriptorTable(3, exposureGpu); // t1

    const UINT groupsX = (mWidth  + 7) / 8;
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
}
