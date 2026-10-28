#include "pch.h"
#include "TAARenderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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
    constexpr DXGI_FORMAT TaaFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    // Staging heap layout (CPU only).
    constexpr UINT kStagingHistorySrv = 0; // + index
    constexpr UINT kStagingHistoryUav = 2; // + index
    constexpr UINT kStagingNullTexture = 4;
    constexpr UINT kStagingNullBuffer = 5;
    constexpr UINT kStagingCount = 6;

    // Below this many pixels of disagreement between a motion vector and the
    // camera's own motion, a pixel is treated as static.
    constexpr float kDynamicThresholdPixels = 0.1f;

    ComPtr<ID3D12Resource> MakeTex2D(
        ID3D12Device* device,
        UINT width, UINT height,
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
        desc.Format = TaaFormat;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = flags;

        ComPtr<ID3D12Resource> resource;
        DX12_THROW_IF_FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, initialState,
            nullptr, IID_PPV_ARGS(&resource)));
        return resource;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Offset(D3D12_CPU_DESCRIPTOR_HANDLE handle, UINT index, UINT stride)
    {
        handle.ptr += static_cast<SIZE_T>(index) * stride;
        return handle;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE Offset(D3D12_GPU_DESCRIPTOR_HANDLE handle, UINT index, UINT stride)
    {
        handle.ptr += static_cast<UINT64>(index) * stride;
        return handle;
    }

    float Halton(UINT index, UINT base)
    {
        float fraction = 1.0f;
        float result = 0.0f;
        for (; index > 0; index /= base)
        {
            fraction /= static_cast<float>(base);
            result += fraction * static_cast<float>(index % base);
        }
        return result;
    }

    float CatmullRom(float x)
    {
        const float ax = std::fabs(x);
        if (ax > 1.0f)
            return ((-0.5f * ax + 2.5f) * ax - 4.0f) * ax + 2.0f;
        return (1.5f * ax - 2.5f) * ax * ax + 1.0f;
    }
}

// ---------------------------------------------------------------------------
void TAARenderer::GetJitterOffset(UINT frameIndex, int samples, float screenPercentage, float& outX, float& outY)
{
    // Unreal (TemporalAA.cpp): when upsampling, scale the number of samples so
    // every output pixel converges as fast as it would at 100%.
    const float fraction = std::clamp(screenPercentage / 100.0f, 0.01f, 1.0f);
    const int count = std::clamp(static_cast<int>(std::ceil(static_cast<float>((std::max)(samples, 1)) / (fraction * fraction))), 1, 255);
    const UINT index = (frameIndex % static_cast<UINT>(count)) + 1; // Halton index 0 is (0,0)
    outX = Halton(index, 2) - 0.5f;
    outY = Halton(index, 3) - 0.5f;
}

// ---------------------------------------------------------------------------
bool TAARenderer::Initialize(UINT inputWidth, UINT inputHeight, UINT outputWidth, UINT outputHeight)
{
    if (inputWidth == 0 || inputHeight == 0 || outputWidth == 0 || outputHeight == 0)
        return false;

    mLastError.clear();

    try
    {
        if (!mRootSignature && !CreateRootSignature())
            return false;

        // The default permutation is compiled up front so a broken shader is
        // reported at startup rather than when TAA is first switched on.
        if (!EnsurePipeline(false, 2))
            return false;

        const bool texturesNeeded = !mIsInitialized
            || outputWidth != mOutputWidth || outputHeight != mOutputHeight;
        if (texturesNeeded && !CreateTextures(outputWidth, outputHeight))
            return false;

        if (inputWidth != mInputWidth || inputHeight != mInputHeight)
            mHistoryValid = false;

        mInputWidth = inputWidth;
        mInputHeight = inputHeight;
        mOutputWidth = outputWidth;
        mOutputHeight = outputHeight;
        mIsInitialized = true;
        return true;
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("TAARenderer::Initialize: ") + ex.what();
        return false;
    }
}

// ---------------------------------------------------------------------------
bool TAARenderer::CreateRootSignature()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
    {
        mLastError = "TAARenderer: null device.";
        return false;
    }

    // ---- root signature ----
    //   param 0  CBV  b0      (inline root descriptor, one ring entry per frame)
    //   param 1  table t0-t4, u0
    //   s0       static linear clamp sampler (history)
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 5;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;

    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 5;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
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

    // ---- constant buffer ring (persistently mapped upload heap) ----
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeap.CreationNodeMask = 1;
    uploadHeap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC cbDesc{};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = kCbStride * kFramesInFlight;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.Format = DXGI_FORMAT_UNKNOWN;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    DX12_THROW_IF_FAILED(device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&mConstantBuffer)));
    void* mapped = nullptr;
    DX12_THROW_IF_FAILED(mConstantBuffer->Map(0, nullptr, &mapped));
    mMappedCb = static_cast<std::uint8_t*>(mapped);

    // ---- descriptor heaps ----
    D3D12_DESCRIPTOR_HEAP_DESC ringDesc{};
    ringDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    ringDesc.NumDescriptors = kDescriptorsPerSet * kFramesInFlight;
    ringDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    DX12_THROW_IF_FAILED(device->CreateDescriptorHeap(&ringDesc, IID_PPV_ARGS(&mComputeHeap)));
    mComputeHeapStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC stagingDesc{};
    stagingDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    stagingDesc.NumDescriptors = kStagingCount;
    stagingDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    DX12_THROW_IF_FAILED(device->CreateDescriptorHeap(&stagingDesc, IID_PPV_ARGS(&mStagingHeap)));

    // Null views stand in for motion vectors and the exposure buffer when there are none.
    const D3D12_CPU_DESCRIPTOR_HANDLE staging = mStagingHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC nullTexture{};
    nullTexture.Format = DXGI_FORMAT_R16G16_FLOAT;
    nullTexture.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullTexture.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullTexture.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(nullptr, &nullTexture, Offset(staging, kStagingNullTexture, mComputeHeapStride));

    D3D12_SHADER_RESOURCE_VIEW_DESC nullBuffer{};
    nullBuffer.Format = DXGI_FORMAT_R32_TYPELESS;
    nullBuffer.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    nullBuffer.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullBuffer.Buffer.NumElements = 4;
    nullBuffer.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device->CreateShaderResourceView(nullptr, &nullBuffer, Offset(staging, kStagingNullBuffer, mComputeHeapStride));

    return true;
}

// ---------------------------------------------------------------------------
bool TAARenderer::EnsurePipeline(bool upsample, int quality)
{
    quality = std::clamp(quality, 0, 3);
    const int u = upsample ? 1 : 0;
    if (mPipelineStates[u][quality])
        return true;

    ID3D12Device* device = DX12Context_GetDevice();
    if (!device || !mRootSignature)
    {
        mLastError = "TAARenderer: the pipeline was requested before the root signature exists.";
        return false;
    }

    ShaderCompileRequest request
    {
        L"Shaders\\TAAResolve.hlsl",
        L"CSMain",
        L"cs_6_0",
        ShaderStage::Compute
    };
    request.Defines.push_back(std::wstring(L"TAA_UPSAMPLE=") + (upsample ? L"1" : L"0"));
    request.Defines.push_back(L"TAA_QUALITY=" + std::to_wstring(quality));

    DX12Shader& shader = mComputeShaders[u][quality];
    if (!shader.Compile(request))
    {
        mLastError = std::string("TAARenderer: shader compile failed: ")
            + (shader.GetLastErrorMessage() ? shader.GetLastErrorMessage() : "unknown error");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = mRootSignature.Get();
    psoDesc.CS = shader.GetBytecode();
    if (FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mPipelineStates[u][quality]))))
    {
        mLastError = "TAARenderer: CreateComputePipelineState failed.";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
bool TAARenderer::CreateTextures(UINT width, UINT height)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
        return false;

    // Allocate the shared-context Ui SRV slots once.
    if (!mUiSlotsAllocated)
    {
        for (UINT i = 0; i < 2; ++i)
        {
            if (!DX12Context_AllocateSrvDescriptor(&mOutputUiSrvCpu[i], &mOutputUiSrvGpu[i]))
            {
                mLastError = "TAARenderer: failed to allocate Ui output SRV slots.";
                return false;
            }
            mOutputTextureId[i] = static_cast<UiTextureID>(mOutputUiSrvGpu[i].ptr);
        }
        mUiSlotsAllocated = true;
    }

    // Flush the GPU before releasing the old textures: a resolve still in flight
    // may be reading or writing them.
    DX12Context_WaitForGPU();
    for (auto& texture : mHistory)
        texture.Reset();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = TaaFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = TaaFormat;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    const D3D12_CPU_DESCRIPTOR_HANDLE staging = mStagingHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < 2; ++i)
    {
        // Rests in PIXEL_SHADER_RESOURCE between frames: that is what every pass
        // reading the TAA output expects.
        mHistory[i] = MakeTex2D(device, width, height,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        mHistory[i]->SetName(i == 0 ? L"TAA History 0" : L"TAA History 1");

        device->CreateShaderResourceView(mHistory[i].Get(), &srvDesc, Offset(staging, kStagingHistorySrv + i, mComputeHeapStride));
        device->CreateUnorderedAccessView(mHistory[i].Get(), nullptr, &uavDesc, Offset(staging, kStagingHistoryUav + i, mComputeHeapStride));
        device->CreateShaderResourceView(mHistory[i].Get(), &srvDesc, mOutputUiSrvCpu[i]);
    }

    mCurrent = 0;
    mHistoryValid = false;
    return true;
}

// ---------------------------------------------------------------------------
void TAARenderer::Resolve(ID3D12GraphicsCommandList* commandList, const FrameInputs& inputs, TaaSettings& settings)
{
    if (!mIsInitialized || !commandList || !inputs.Color || inputs.ColorSrv.ptr == 0 || inputs.DepthSrv.ptr == 0)
        return;

    settings.Validate();
    const bool upsample = IsUpsampling();
    const int quality = settings.Quality;
    if (!EnsurePipeline(upsample, quality))
        return;

    ID3D12Device* device = DX12Context_GetDevice();

    const UINT history = mCurrent;
    const UINT output = 1 - mCurrent;
    const bool cameraCut = settings.ResetHistory || !mHistoryValid;

    // ---- constants ----
    TaaCbData cb{};
    {
        XMStoreFloat4x4(&cb.ClipToPrevClip, XMMatrixTranspose(XMLoadFloat4x4(&inputs.ClipToPrevClip)));

        const float iw = static_cast<float>(mInputWidth);
        const float ih = static_cast<float>(mInputHeight);
        const float ow = static_cast<float>(mOutputWidth);
        const float oh = static_cast<float>(mOutputHeight);

        const float inputSize[4] = { iw, ih, 1.0f / iw, 1.0f / ih };
        const float outputSize[4] = { ow, oh, 1.0f / ow, 1.0f / oh };
        std::memcpy(cb.InputSceneColorSize, inputSize, sizeof(inputSize));
        std::memcpy(cb.InputViewSize, inputSize, sizeof(inputSize));
        std::memcpy(cb.OutputViewportSize, outputSize, sizeof(outputSize));
        std::memcpy(cb.HistoryBufferSize, outputSize, sizeof(outputSize));

        cb.InputMinPixelCoord[0] = 0;
        cb.InputMinPixelCoord[1] = 0;
        cb.InputMaxPixelCoord[0] = static_cast<int>(mInputWidth) - 1;
        cb.InputMaxPixelCoord[1] = static_cast<int>(mInputHeight) - 1;

        // The history covers exactly the viewport, so it is clamped half a texel in.
        cb.HistoryBufferUVMinMax[0] = 0.5f / ow;
        cb.HistoryBufferUVMinMax[1] = 0.5f / oh;
        cb.HistoryBufferUVMinMax[2] = 1.0f - 0.5f / ow;
        cb.HistoryBufferUVMinMax[3] = 1.0f - 0.5f / oh;
        cb.ScreenPosToHistoryBufferUV[0] = 0.5f;
        cb.ScreenPosToHistoryBufferUV[1] = -0.5f;
        cb.ScreenPosToHistoryBufferUV[2] = 0.5f;
        cb.ScreenPosToHistoryBufferUV[3] = 0.5f;
        cb.ScreenPosAbsMax[0] = 1.0f - 1.0f / ow;
        cb.ScreenPosAbsMax[1] = 1.0f - 1.0f / oh;

        const float jitterX = inputs.JitterX;
        const float jitterY = inputs.JitterY;
        cb.TemporalJitterPixels[0] = jitterX;
        cb.TemporalJitterPixels[1] = jitterY;

        cb.InputViewMin[0] = 0.0f;
        cb.InputViewMin[1] = 0.0f;
        cb.ScreenPercentage = iw / ow;
        cb.UpscaleFactor = ow / iw;

        // Reconstruction filter weights for the 3x3 neighbourhood (and its plus
        // subset), centred on where this output pixel lies among the jittered
        // input samples. As Unreal's TemporalAA.cpp computes them.
        static const float kSampleOffsets[9][2] =
        {
            { -1.0f, -1.0f }, { 0.0f, -1.0f }, { 1.0f, -1.0f },
            { -1.0f,  0.0f }, { 0.0f,  0.0f }, { 1.0f,  0.0f },
            { -1.0f,  1.0f }, { 0.0f,  1.0f }, { 1.0f,  1.0f },
        };
        float weights[9] = {};
        float totalWeight = 0.0f;
        for (int i = 0; i < 9; ++i)
        {
            const float pixelOffsetX = (kSampleOffsets[i][0] - jitterX) / settings.FilterSize;
            const float pixelOffsetY = (kSampleOffsets[i][1] - jitterY) / settings.FilterSize;
            if (settings.CatmullRomFilter)
                weights[i] = CatmullRom(pixelOffsetX) * CatmullRom(pixelOffsetY);
            else // Normal distribution, Sigma = 0.47
                weights[i] = std::exp(-2.29f * (pixelOffsetX * pixelOffsetX + pixelOffsetY * pixelOffsetY));
            totalWeight += weights[i];
        }
        for (int i = 0; i < 9; ++i)
            cb.SampleWeights[i] = weights[i] / totalWeight;

        static const int kPlusIndexes[5] = { 1, 3, 4, 5, 7 };
        float totalPlusWeight = 0.0f;
        for (int i = 0; i < 5; ++i)
            totalPlusWeight += weights[kPlusIndexes[i]];
        for (int i = 0; i < 5; ++i)
            cb.PlusWeights[i] = weights[kPlusIndexes[i]] / totalPlusWeight;

        cb.CurrentFrameWeight = settings.CurrentFrameWeight;
        cb.bCameraCut = cameraCut ? 1 : 0;
        cb.bVelocityAvailable = (inputs.VelocityAvailable && inputs.VelocitySrv.ptr != 0) ? 1u : 0u;
        cb.HistoryPreExposureCorrection = 1.0f;

        cb.Ev100 = inputs.Ev100;
        cb.Ev100Min = inputs.Ev100Min;
        cb.Ev100Max = inputs.Ev100Max;
        cb.ExposureTrim = inputs.ExposureTrim;
        cb.UseAutoExposure = inputs.AutoExposureSrv.ptr != 0 ? 1u : 0u;
        cb.DynamicThresholdPixels = kDynamicThresholdPixels;
    }
    std::memcpy(mMappedCb + kCbStride * mFrameSlot, &cb, sizeof(cb));
    const D3D12_GPU_VIRTUAL_ADDRESS cbAddress = mConstantBuffer->GetGPUVirtualAddress() + kCbStride * mFrameSlot;

    // Consume the reset flag now that it has been applied to the constant buffer.
    settings.ResetHistory = false;

    // ---- descriptors: this frame's set in the ring ----
    const D3D12_CPU_DESCRIPTOR_HANDLE staging = mStagingHeap->GetCPUDescriptorHandleForHeapStart();
    const UINT setBase = mFrameSlot * kDescriptorsPerSet;
    const D3D12_CPU_DESCRIPTOR_HANDLE set = Offset(mComputeHeap->GetCPUDescriptorHandleForHeapStart(), setBase, mComputeHeapStride);
    const auto copy = [&](UINT slot, D3D12_CPU_DESCRIPTOR_HANDLE source)
    {
        device->CopyDescriptorsSimple(1, Offset(set, slot, mComputeHeapStride), source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    };
    copy(0, inputs.ColorSrv);
    copy(1, inputs.DepthSrv);
    copy(2, cb.bVelocityAvailable ? inputs.VelocitySrv : Offset(staging, kStagingNullTexture, mComputeHeapStride));
    copy(3, Offset(staging, kStagingHistorySrv + history, mComputeHeapStride));
    copy(4, inputs.AutoExposureSrv.ptr != 0 ? inputs.AutoExposureSrv : Offset(staging, kStagingNullBuffer, mComputeHeapStride));
    copy(5, Offset(staging, kStagingHistoryUav + output, mComputeHeapStride));

    // ---- barriers ----
    //   current frame: PIXEL_SHADER_RESOURCE -> NON_PIXEL_SHADER_RESOURCE (compute read)
    //   history:       PIXEL_SHADER_RESOURCE -> NON_PIXEL_SHADER_RESOURCE (compute read)
    //   output:        PIXEL_SHADER_RESOURCE -> UNORDERED_ACCESS
    D3D12_RESOURCE_BARRIER preBarriers[3] =
    {
        CD3DX12_RESOURCE_BARRIER::Transition(inputs.Color,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(mHistory[history].Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(mHistory[output].Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    commandList->ResourceBarrier(static_cast<UINT>(std::size(preBarriers)), preBarriers);

    ID3D12DescriptorHeap* heaps[] = { mComputeHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(mRootSignature.Get());
    commandList->SetPipelineState(mPipelineStates[upsample ? 1 : 0][quality].Get());
    commandList->SetComputeRootConstantBufferView(0, cbAddress);
    commandList->SetComputeRootDescriptorTable(1, Offset(mComputeHeap->GetGPUDescriptorHandleForHeapStart(), setBase, mComputeHeapStride));
    commandList->Dispatch((mOutputWidth + 7) / 8, (mOutputHeight + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER postBarriers[3] =
    {
        CD3DX12_RESOURCE_BARRIER::Transition(mHistory[output].Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(mHistory[history].Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(inputs.Color,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
    };
    commandList->ResourceBarrier(static_cast<UINT>(std::size(postBarriers)), postBarriers);

    mCurrent = output;
    mHistoryValid = true;
    mFrameSlot = (mFrameSlot + 1) % kFramesInFlight;

    // Re-bind the shared SRV heap so Ui and other passes see it.
    ID3D12DescriptorHeap* sharedHeap[] = { DX12Context_GetSrvDescriptorHeap() };
    commandList->SetDescriptorHeaps(1, sharedHeap);
}

// ---------------------------------------------------------------------------
void TAARenderer::Shutdown()
{
    if (mConstantBuffer && mMappedCb)
    {
        mConstantBuffer->Unmap(0, nullptr);
        mMappedCb = nullptr;
    }
    mConstantBuffer.Reset();
    for (auto& texture : mHistory)
        texture.Reset();
    mComputeHeap.Reset();
    mStagingHeap.Reset();
    for (auto& perUpsample : mPipelineStates)
        for (auto& pso : perUpsample)
            pso.Reset();
    mRootSignature.Reset();
    mIsInitialized = false;
    mHistoryValid = false;
    mInputWidth = mInputHeight = mOutputWidth = mOutputHeight = 0;
    // The shared heap slots stay allocated (mUiSlotsAllocated) so a re-init does
    // not allocate them again; the shared heap never frees.
}
