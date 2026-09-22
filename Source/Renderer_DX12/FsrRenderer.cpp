#include "pch.h"
#include "FsrRenderer.h"

#include "FfxLoader.h"
#include "FidelityFX-SDK-2.3.0/Kits/FidelityFX/upscalers/include/ffx_upscale.h"
#include "System/PteroLog.h"

#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
    bool __stdcall DX12Context_AllocateSrvDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
    bool __stdcall DX12Context_WaitForGPU();
}

namespace
{
    // Same HDR format as the DLSS output, so everything downstream of the upscaler
    // sees one kind of image whichever upscaler produced it.
    constexpr DXGI_FORMAT FsrOutputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    ffxContext* AsContext(void*& context)
    {
        return reinterpret_cast<ffxContext*>(&context);
    }

    // Per-dimension ratios FSR documents for each quality mode. Only used when the
    // runtime query is unavailable.
    float RatioForMode(int mode)
    {
        switch (mode)
        {
        case FFX_UPSCALE_QUALITY_MODE_NATIVEAA:          return 1.0f;
        case FFX_UPSCALE_QUALITY_MODE_QUALITY:           return 1.5f;
        case FFX_UPSCALE_QUALITY_MODE_BALANCED:          return 1.7f;
        case FFX_UPSCALE_QUALITY_MODE_PERFORMANCE:       return 2.0f;
        case FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE: return 3.0f;
        default:                                         return 1.5f;
        }
    }

    float Halton(int index, int base)
    {
        float f = 1.0f;
        float result = 0.0f;
        for (int i = index; i > 0; i /= base)
        {
            f /= static_cast<float>(base);
            result += f * static_cast<float>(i % base);
        }
        return result;
    }
}

bool FsrRenderer::Initialize()
{
    mLastError.clear();
    mAvailable = false;

    const ffxFunctions* ffx = FfxLoader::Get();
    ID3D12Device* device = DX12Context_GetDevice();
    if (ffx == nullptr || device == nullptr)
    {
        mLastError = FfxLoader::GetLastError() ? FfxLoader::GetLastError() : "FSR: the FidelityFX API is unavailable.";
        return false;
    }

    // Asking for the provider count is the cheapest way to learn whether an
    // upscaler DLL was found and supports this device, without creating anything.
    uint64_t providerCount = 0;
    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    versions.device = device;
    versions.outputCount = &providerCount;
    if (ffx->Query(nullptr, &versions.header) != FFX_API_RETURN_OK || providerCount == 0)
    {
        mLastError = "FSR: no upscaler provider is available (amd_fidelityfx_upscaler_dx12.dll missing or unsupported GPU).";
        PteroLog::Write(PteroLog::Level::Warning, "FSR", mLastError.c_str());
        return false;
    }

    mAvailable = true;
    return true;
}

void FsrRenderer::Shutdown()
{
    ReleaseResources();
    mAvailable = false;
}

void FsrRenderer::ReleaseResources()
{
    if (mContext == nullptr && mOutputTexture == nullptr)
        return;

    DX12Context_WaitForGPU();
    DestroyContext();
    mOutputTexture.Reset();
    mOutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    mRenderWidth = mRenderHeight = mOutputWidth = mOutputHeight = 0;
}

bool FsrRenderer::CreateContext(UINT outputWidth, UINT outputHeight)
{
    const ffxFunctions* ffx = FfxLoader::Get();
    if (ffx == nullptr)
        return false;

    ffxCreateBackendDX12Desc backend = FfxLoader::MakeBackendDesc(DX12Context_GetDevice());

    ffxCreateContextDescUpscaleVersion version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version.version = FFX_UPSCALER_VERSION;
    version.header.pNext = &backend.header;

    // The render size can change with the quality mode without recreating the
    // context, so it is sized for the largest input it can receive - native.
    ffxCreateContextDescUpscale create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    create.header.pNext = &version.header;
    // Scene colour is linear HDR, before bloom and tonemapping. Depth is standard
    // [0 near, 1 far] with a finite far plane, so neither depth flag applies.
    create.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE
                 | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    create.maxRenderSize = { outputWidth, outputHeight };
    create.maxUpscaleSize = { outputWidth, outputHeight };
    create.fpMessage = &FfxLoader::LogMessage;

    const ffxReturnCode_t result = ffx->CreateContext(AsContext(mContext), &create.header, nullptr);
    if (result != FFX_API_RETURN_OK)
    {
        mContext = nullptr;
        mLastError = "FSR: ffxCreateContext(upscale) failed with code " + std::to_string(result) + ".";
        PteroLog::Write(PteroLog::Level::Error, "FSR", mLastError.c_str());
        return false;
    }

    ffxQueryGetProviderVersion providerVersion{};
    providerVersion.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (ffx->Query(AsContext(mContext), &providerVersion.header) == FFX_API_RETURN_OK && providerVersion.versionName)
        mVersionName = providerVersion.versionName;
    else
        mVersionName.clear();

    PteroLog::Writef(PteroLog::Level::Info, "FSR", "Upscaler context created (%s) for %ux%u output.",
                     mVersionName.empty() ? "unknown version" : mVersionName.c_str(), outputWidth, outputHeight);
    return true;
}

void FsrRenderer::DestroyContext()
{
    if (mContext == nullptr)
        return;
    if (const ffxFunctions* ffx = FfxLoader::Get())
        ffx->DestroyContext(AsContext(mContext), nullptr);
    mContext = nullptr;
}

bool FsrRenderer::CreateOutputTexture(UINT outputWidth, UINT outputHeight)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "FSR: null D3D12 device.";
        return false;
    }

    if (!mOutputSrvAllocated)
    {
        if (!DX12Context_AllocateSrvDescriptor(&mOutputSrvCpu, &mOutputSrvGpu))
        {
            mLastError = "FSR: failed to allocate the output SRV descriptor.";
            return false;
        }
        mOutputTextureId = static_cast<UiTextureID>(mOutputSrvGpu.ptr);
        mOutputSrvAllocated = true;
    }

    const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = outputWidth;
    desc.Height = outputHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = FsrOutputFormat;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&mOutputTexture))))
    {
        mLastError = "FSR: failed to create the output texture.";
        return false;
    }
    mOutputTexture->SetName(L"FSR Output");
    mOutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = FsrOutputFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(mOutputTexture.Get(), &srvDesc, mOutputSrvCpu);
    return true;
}

bool FsrRenderer::EnsureSize(UINT renderWidth, UINT renderHeight, UINT outputWidth, UINT outputHeight)
{
    if (!mAvailable || renderWidth == 0 || renderHeight == 0 || outputWidth == 0 || outputHeight == 0)
        return false;

    // The render size alone is a per-dispatch parameter; only the output size is
    // baked into the context and the texture.
    mRenderWidth = renderWidth;
    mRenderHeight = renderHeight;
    if (mContext != nullptr && mOutputTexture && mOutputWidth == outputWidth && mOutputHeight == outputHeight)
        return true;

    if (mContext != nullptr || mOutputTexture)
    {
        DX12Context_WaitForGPU();
        DestroyContext();
        mOutputTexture.Reset();
    }

    if (!CreateOutputTexture(outputWidth, outputHeight) || !CreateContext(outputWidth, outputHeight))
    {
        DestroyContext();
        mOutputTexture.Reset();
        mOutputWidth = mOutputHeight = 0;
        return false;
    }

    mOutputWidth = outputWidth;
    mOutputHeight = outputHeight;
    mLastError.clear();
    return true;
}

bool FsrRenderer::QueryRenderSize(UINT outputWidth, UINT outputHeight, const FsrSettings& settings,
                                  UINT& outRenderWidth, UINT& outRenderHeight)
{
    outRenderWidth = outputWidth;
    outRenderHeight = outputHeight;
    if (!mAvailable || outputWidth == 0 || outputHeight == 0)
        return false;

    const int mode = std::clamp(settings.Mode,
        static_cast<int>(FFX_UPSCALE_QUALITY_MODE_NATIVEAA),
        static_cast<int>(FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE));

    uint32_t width = 0;
    uint32_t height = 0;
    if (const ffxFunctions* ffx = FfxLoader::Get())
    {
        // Chaining the backend lets a driver-side provider answer a context-less query.
        ffxCreateBackendDX12Desc backend = FfxLoader::MakeBackendDesc(DX12Context_GetDevice());
        ffxQueryDescUpscaleGetRenderResolutionFromQualityMode query{};
        query.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
        query.header.pNext = &backend.header;
        query.displayWidth = outputWidth;
        query.displayHeight = outputHeight;
        query.qualityMode = static_cast<uint32_t>(mode);
        query.pOutRenderWidth = &width;
        query.pOutRenderHeight = &height;
        if (ffx->Query(mContext ? AsContext(mContext) : nullptr, &query.header) != FFX_API_RETURN_OK)
            width = height = 0;
    }

    if (width == 0 || height == 0)
    {
        const float ratio = RatioForMode(mode);
        width = static_cast<uint32_t>(static_cast<float>(outputWidth) / ratio);
        height = static_cast<uint32_t>(static_cast<float>(outputHeight) / ratio);
    }

    outRenderWidth = (std::max)(1u, (std::min)(static_cast<UINT>(width), outputWidth));
    outRenderHeight = (std::max)(1u, (std::min)(static_cast<UINT>(height), outputHeight));
    return true;
}

void FsrRenderer::GetJitterOffset(UINT frameIndex, UINT renderWidth, UINT outputWidth, float& outX, float& outY)
{
    outX = 0.0f;
    outY = 0.0f;
    if (renderWidth == 0 || outputWidth == 0)
        return;

    int32_t phaseCount = 0;
    const ffxFunctions* ffx = FfxLoader::Get();
    if (ffx != nullptr && mContext != nullptr)
    {
        ffxQueryDescUpscaleGetJitterPhaseCount phaseQuery{};
        phaseQuery.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
        phaseQuery.renderWidth = renderWidth;
        phaseQuery.displayWidth = outputWidth;
        phaseQuery.pOutPhaseCount = &phaseCount;
        if (ffx->Query(AsContext(mContext), &phaseQuery.header) == FFX_API_RETURN_OK && phaseCount > 0)
        {
            ffxQueryDescUpscaleGetJitterOffset offsetQuery{};
            offsetQuery.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
            offsetQuery.index = static_cast<int32_t>(frameIndex % static_cast<UINT>(phaseCount));
            offsetQuery.phaseCount = phaseCount;
            offsetQuery.pOutX = &outX;
            offsetQuery.pOutY = &outY;
            if (ffx->Query(AsContext(mContext), &offsetQuery.header) == FFX_API_RETURN_OK)
                return;
        }
    }

    // The sequence FSR documents: Halton(2,3) with 8 samples per output pixel's
    // worth of render pixels. Index is 1-based so the first sample is never (0,0).
    const float ratio = static_cast<float>(outputWidth) / static_cast<float>(renderWidth);
    phaseCount = (std::max)(1, static_cast<int32_t>(8.0f * ratio * ratio));
    const int index = static_cast<int>(frameIndex % static_cast<UINT>(phaseCount)) + 1;
    outX = Halton(index, 2) - 0.5f;
    outY = Halton(index, 3) - 0.5f;
}

void FsrRenderer::Evaluate(
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* inputColor,
    ID3D12Resource* depth,
    ID3D12Resource* motionVectors,
    const FrameData& frameData,
    FsrSettings& settings)
{
    const ffxFunctions* ffx = FfxLoader::Get();
    if (!(ffx && mContext && mOutputTexture && commandList && inputColor && depth && motionVectors))
        return;

    if (mOutputState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        const auto toUav = CD3DX12_RESOURCE_BARRIER::Transition(
            mOutputTexture.Get(), mOutputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->ResourceBarrier(1, &toUav);
        mOutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    ffxDispatchDescUpscale dispatch{};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch.commandList = commandList;
    dispatch.color = ffxApiGetResourceDX12(inputColor, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatch.depth = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatch.motionVectors = ffxApiGetResourceDX12(motionVectors, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatch.exposure = ffxApiGetResourceDX12(nullptr);
    dispatch.reactive = ffxApiGetResourceDX12(nullptr);
    dispatch.transparencyAndComposition = ffxApiGetResourceDX12(nullptr);
    dispatch.output = ffxApiGetResourceDX12(mOutputTexture.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.jitterOffset = { frameData.JitterX, frameData.JitterY };
    // The engine writes (current - previous) in UV space; FSR wants the offset from
    // the current pixel back to where it was, in render-resolution pixels.
    dispatch.motionVectorScale = { -static_cast<float>(mRenderWidth), -static_cast<float>(mRenderHeight) };
    dispatch.renderSize = { mRenderWidth, mRenderHeight };
    dispatch.upscaleSize = { mOutputWidth, mOutputHeight };
    dispatch.enableSharpening = settings.Sharpening;
    dispatch.sharpness = std::clamp(settings.Sharpness, 0.0f, 1.0f);
    dispatch.frameTimeDelta = frameData.FrameTimeDeltaMs;
    dispatch.preExposure = 1.0f;
    dispatch.reset = frameData.Reset;
    dispatch.cameraNear = frameData.NearPlane;
    dispatch.cameraFar = frameData.FarPlane;
    dispatch.cameraFovAngleVertical = frameData.FovY;
    dispatch.viewSpaceToMetersFactor = 1.0f; // one engine unit is one metre
    dispatch.flags = 0;

    const ffxReturnCode_t result = ffx->Dispatch(AsContext(mContext), &dispatch.header);
    if (result != FFX_API_RETURN_OK)
    {
        mLastError = "FSR: upscale dispatch failed with code " + std::to_string(result) + ".";
        return;
    }

    const auto toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
        mOutputTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &toSrv);
    mOutputState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    settings.ResetHistory = false;
    mLastError.clear();
}
