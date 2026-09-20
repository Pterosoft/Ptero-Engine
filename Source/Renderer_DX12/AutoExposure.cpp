#include "pch.h"
#include "AutoExposure.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

using Microsoft::WRL::ComPtr;

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
    bool __stdcall DX12Context_AllocateSrvDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu);
}

namespace
{
    // Raw (ByteAddress) buffers are typeless with the RAW view flag; both the
    // histogram and the exposure value are addressed by byte offset.
    constexpr DXGI_FORMAT kRawViewFormat = DXGI_FORMAT_R32_TYPELESS;

    ComPtr<ID3D12Resource> MakeRawBuffer(
        ID3D12Device*         device,
        UINT64                sizeBytes,
        D3D12_RESOURCE_STATES initialState)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask  = 1;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width            = sizeBytes;
        desc.Height           = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        ComPtr<ID3D12Resource> resource;
        DX12_THROW_IF_FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, initialState,
            nullptr, IID_PPV_ARGS(&resource)));
        return resource;
    }

    void CreateRawUav(
        ID3D12Device*               device,
        ID3D12Resource*             resource,
        UINT                        numDwords,
        D3D12_CPU_DESCRIPTOR_HANDLE destination)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.Format              = kRawViewFormat;
        desc.ViewDimension       = D3D12_UAV_DIMENSION_BUFFER;
        desc.Buffer.FirstElement = 0;
        desc.Buffer.NumElements  = numDwords;
        desc.Buffer.Flags        = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(resource, nullptr, &desc, destination);
    }
}

// ---------------------------------------------------------------------------
bool AutoExposure::Initialize()
{
    if (mIsInitialized)
        return true;

    mLastError.clear();

    try
    {
        if (!CreatePipelines())
            return false;
        if (!CreateBuffers())
            return false;

        mResetHistory = true;
        mIsInitialized = true;
        return true;
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("AutoExposure::Initialize: ") + ex.what();
        return false;
    }
}

// ---------------------------------------------------------------------------
void AutoExposure::Shutdown()
{
    if (mConstantBuffer && mMappedCb)
    {
        mConstantBuffer->Unmap(0, nullptr);
        mMappedCb = nullptr;
    }
    mConstantBuffer.Reset();
    mHistogramBuffer.Reset();
    mExposureBuffer.Reset();
    mComputeHeap.Reset();
    mHistogramPipeline.Reset();
    mAdaptPipeline.Reset();
    mHistogramRootSignature.Reset();
    mAdaptRootSignature.Reset();
    mIsInitialized = false;
}

// ---------------------------------------------------------------------------
bool AutoExposure::CreatePipelines()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
    {
        mLastError = "AutoExposure: null device.";
        return false;
    }

    const ShaderCompileRequest histogramReq
    {
        L"Shaders\\AutoExposureHistogram.hlsl",
        L"CSMain",
        L"cs_5_0",
        ShaderStage::Compute
    };
    if (!mHistogramShader.Compile(histogramReq))
    {
        mLastError = std::string("AutoExposure: histogram shader compile failed: ")
            + (mHistogramShader.GetLastErrorMessage() ? mHistogramShader.GetLastErrorMessage() : "unknown");
        return false;
    }

    const ShaderCompileRequest adaptReq
    {
        L"Shaders\\AutoExposureAdapt.hlsl",
        L"CSMain",
        L"cs_5_0",
        ShaderStage::Compute
    };
    if (!mAdaptShader.Compile(adaptReq))
    {
        mLastError = std::string("AutoExposure: adapt shader compile failed: ")
            + (mAdaptShader.GetLastErrorMessage() ? mAdaptShader.GetLastErrorMessage() : "unknown");
        return false;
    }

    // --- Histogram root signature -----------------------------------------
    //   param 0 - inline CBV b0
    //   param 1 - table: t0 (scene colour)
    //   param 2 - table: u0 (histogram)
    {
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors     = 1;
        srvRange.BaseShaderRegister = 0;
        srvRange.OffsetInDescriptorsFromTableStart = 0;

        D3D12_DESCRIPTOR_RANGE uavRange{};
        uavRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors     = 1;
        uavRange.BaseShaderRegister = 0;
        uavRange.OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges   = &srvRange;
        params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges   = &uavRange;
        params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 3;
        rsDesc.pParameters   = params;
        rsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> serialized, errors;
        DX12_THROW_IF_FAILED(D3D12SerializeRootSignature(
            &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
        DX12_THROW_IF_FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&mHistogramRootSignature)));

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = mHistogramRootSignature.Get();
        psoDesc.CS             = mHistogramShader.GetBytecode();
        DX12_THROW_IF_FAILED(device->CreateComputePipelineState(
            &psoDesc, IID_PPV_ARGS(&mHistogramPipeline)));
    }

    // --- Adapt root signature ---------------------------------------------
    //   param 0 - inline CBV b0
    //   param 1 - table: u0 (histogram) and u1 (exposure), contiguous
    {
        D3D12_DESCRIPTOR_RANGE uavRange{};
        uavRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors     = 2; // u0, u1
        uavRange.BaseShaderRegister = 0;
        uavRange.OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges   = &uavRange;
        params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 2;
        rsDesc.pParameters   = params;
        rsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> serialized, errors;
        DX12_THROW_IF_FAILED(D3D12SerializeRootSignature(
            &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
        DX12_THROW_IF_FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&mAdaptRootSignature)));

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = mAdaptRootSignature.Get();
        psoDesc.CS             = mAdaptShader.GetBytecode();
        DX12_THROW_IF_FAILED(device->CreateComputePipelineState(
            &psoDesc, IID_PPV_ARGS(&mAdaptPipeline)));
    }

    return true;
}

// ---------------------------------------------------------------------------
bool AutoExposure::CreateBuffers()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
    {
        mLastError = "AutoExposure: null device.";
        return false;
    }

    // Constant buffer: one 256-byte-aligned copy per frame in flight. The
    // contents change every frame (delta time at minimum), and the CPU runs up
    // to three frames ahead of the GPU, so a single copy would be overwritten
    // while an earlier frame's dispatch was still reading it.
    mCbStride = (sizeof(AutoExposureCbData) + 255ull) & ~255ull;

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeap.CreationNodeMask = 1;
    uploadHeap.VisibleNodeMask  = 1;

    D3D12_RESOURCE_DESC cbDesc{};
    cbDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width            = mCbStride * kFramesInFlight;
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
    std::memset(mMappedCb, 0, static_cast<size_t>(mCbStride) * kFramesInFlight);

    // The histogram starts zeroed and is re-zeroed by the adapt pass at the end
    // of every frame, so it never needs an explicit clear.
    mHistogramBuffer = MakeRawBuffer(
        device, kHistogramBins * sizeof(UINT), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // [0] smoothed EV100, [1] target EV100, [2] average luminance.
    // Created in NON_PIXEL_SHADER_RESOURCE because that is the state the
    // tonemapper reads it in, and Apply() returns it to that state each frame.
    mExposureBuffer = MakeRawBuffer(
        device, 4 * sizeof(float), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Private heap: t0 scene SRV, u0 histogram UAV, u1 exposure UAV.
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 3;
    heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    DX12_THROW_IF_FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mComputeHeap)));
    mComputeHeapStride = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_CPU_DESCRIPTOR_HANDLE heapStart = mComputeHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_CPU_DESCRIPTOR_HANDLE histogramUav = heapStart;
    histogramUav.ptr += static_cast<SIZE_T>(mComputeHeapStride);
    CreateRawUav(device, mHistogramBuffer.Get(), kHistogramBins, histogramUav);

    D3D12_CPU_DESCRIPTOR_HANDLE exposureUav = heapStart;
    exposureUav.ptr += static_cast<SIZE_T>(mComputeHeapStride) * 2;
    CreateRawUav(device, mExposureBuffer.Get(), 4, exposureUav);

    // SRV of the exposure buffer on the shared heap, for the tonemapper.
    if (!mExposureSrvAllocated)
    {
        if (!DX12Context_AllocateSrvDescriptor(&mExposureSrvCpu, &mExposureSrvGpu))
        {
            mLastError = "AutoExposure: could not allocate an exposure SRV descriptor.";
            return false;
        }
        mExposureSrvAllocated = true;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format                  = kRawViewFormat;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement     = 0;
    srvDesc.Buffer.NumElements      = 4;
    srvDesc.Buffer.Flags            = D3D12_BUFFER_SRV_FLAG_RAW;
    device->CreateShaderResourceView(mExposureBuffer.Get(), &srvDesc, mExposureSrvCpu);

    return true;
}

// ---------------------------------------------------------------------------
bool AutoExposure::Apply(
    ID3D12GraphicsCommandList*  commandList,
    ID3D12Resource*             inputResource,
    D3D12_CPU_DESCRIPTOR_HANDLE inputCpuSrv,
    const AgxTonemapSettings&   settings,
    float                       deltaTimeSeconds)
{
    if (!mIsInitialized || !commandList || !inputResource || !mMappedCb)
        return false;

    const D3D12_RESOURCE_DESC inputDesc = inputResource->GetDesc();
    const UINT inputWidth  = static_cast<UINT>(inputDesc.Width);
    const UINT inputHeight = inputDesc.Height;
    if (inputWidth == 0 || inputHeight == 0)
        return false;

    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
        return false;

    mFrameSlot = (mFrameSlot + 1) % kFramesInFlight;

    // Metering at full resolution buys nothing: the histogram is a 64-bin
    // summary of the whole frame. Stride so roughly a quarter-million pixels
    // are sampled regardless of resolution.
    constexpr UINT kTargetSamples = 256u * 1024u;
    UINT stride = 1;
    while (stride < 8 && (inputWidth / stride) * (inputHeight / stride) > kTargetSamples)
        ++stride;

    const float logMin = (std::min)(settings.AutoExposureHistogramLogMin,
                                    settings.AutoExposureHistogramLogMax);
    const float logMax = (std::max)(settings.AutoExposureHistogramLogMin,
                                    settings.AutoExposureHistogramLogMax);

    {
        AutoExposureCbData cb{};
        cb.InputWidth   = inputWidth;
        cb.InputHeight  = inputHeight;
        cb.MeterStride  = stride;
        cb.ResetHistory = mResetHistory ? 1u : 0u;

        cb.LogMin      = logMin;
        cb.LogRange    = (std::max)(logMax - logMin, 0.1f);
        cb.LowPercent  = std::clamp(settings.AutoExposureLowPercent, 0.0f, 1.0f);
        cb.HighPercent = std::clamp(settings.AutoExposureHighPercent, cb.LowPercent, 1.0f);

        cb.MinEv100  = settings.Ev100Min;
        cb.MaxEv100  = settings.Ev100Max;
        cb.SpeedUp   = (std::max)(settings.AutoExposureSpeedUp, 0.0f);
        cb.SpeedDown = (std::max)(settings.AutoExposureSpeedDown, 0.0f);

        // A long hitch must not let the exposure jump: clamping the step keeps
        // adaptation smooth across a stall the same way the eye would not snap.
        cb.DeltaTimeSeconds = std::clamp(deltaTimeSeconds, 0.0f, 0.1f);
        cb.GreyPoint        = (std::max)(settings.AutoExposureGreyPoint, 1e-4f);
        cb.MeteringMask     = std::clamp(settings.AutoExposureMeteringMask, 0.0f, 1.0f);

        std::memcpy(static_cast<std::byte*>(mMappedCb) + mCbStride * mFrameSlot,
                    &cb, sizeof(cb));
    }

    mResetHistory = false;

    const D3D12_GPU_VIRTUAL_ADDRESS cbAddress =
        mConstantBuffer->GetGPUVirtualAddress() + mCbStride * mFrameSlot;

    // Scene colour into slot 0 of the private heap.
    D3D12_CPU_DESCRIPTOR_HANDLE heapStart = mComputeHeap->GetCPUDescriptorHandleForHeapStart();
    device->CopyDescriptorsSimple(1, heapStart, inputCpuSrv,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = mComputeHeap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE histogramUavGpu = gpuBase;
    histogramUavGpu.ptr += mComputeHeapStride;

    // The tonemapper reads the exposure buffer as an SRV, so it has to come
    // back to UNORDERED_ACCESS for the adapt pass to update it.
    D3D12_RESOURCE_BARRIER preBarriers[2]{};
    preBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        inputResource,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    preBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
        mExposureBuffer.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commandList->ResourceBarrier(2, preBarriers);

    ID3D12DescriptorHeap* heaps[] = { mComputeHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);

    // --- Pass 1: build the histogram --------------------------------------
    commandList->SetComputeRootSignature(mHistogramRootSignature.Get());
    commandList->SetPipelineState(mHistogramPipeline.Get());
    commandList->SetComputeRootConstantBufferView(0, cbAddress);
    commandList->SetComputeRootDescriptorTable(1, gpuBase);          // t0
    commandList->SetComputeRootDescriptorTable(2, histogramUavGpu);  // u0

    const UINT meteredWidth  = (inputWidth  + stride - 1) / stride;
    const UINT meteredHeight = (inputHeight + stride - 1) / stride;
    const UINT groupsX = (meteredWidth  + kThreadsX - 1) / kThreadsX;
    const UINT groupsY = (meteredHeight + kThreadsY - 1) / kThreadsY;
    commandList->Dispatch(groupsX, groupsY, 1);

    // The adapt pass reads every bin the histogram pass wrote.
    auto histogramBarrier = CD3DX12_RESOURCE_BARRIER::UAV(mHistogramBuffer.Get());
    commandList->ResourceBarrier(1, &histogramBarrier);

    // --- Pass 2: reduce to one adapted EV ---------------------------------
    commandList->SetComputeRootSignature(mAdaptRootSignature.Get());
    commandList->SetPipelineState(mAdaptPipeline.Get());
    commandList->SetComputeRootConstantBufferView(0, cbAddress);
    commandList->SetComputeRootDescriptorTable(1, histogramUavGpu);  // u0, u1
    commandList->Dispatch(1, 1, 1);

    D3D12_RESOURCE_BARRIER postBarriers[2]{};
    postBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        mExposureBuffer.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    postBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
        inputResource,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(2, postBarriers);

    return true;
}
