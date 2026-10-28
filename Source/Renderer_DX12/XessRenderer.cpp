#include "pch.h"
#include "XessRenderer.h"

#include "XessLoader.h"
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
    // Same HDR format as the DLSS and FSR outputs, so everything downstream of the
    // upscaler sees one kind of image whichever upscaler produced it.
    constexpr DXGI_FORMAT XessOutputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    xess_context_handle_t AsContext(void* context)
    {
        return static_cast<xess_context_handle_t>(context);
    }

    // Per-dimension ratios Intel documents for each preset. Only used when the
    // runtime query is unavailable.
    float RatioForQuality(int quality)
    {
        switch (quality)
        {
        case XESS_QUALITY_SETTING_AA:                  return 1.0f;
        case XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS:  return 1.3f;
        case XESS_QUALITY_SETTING_ULTRA_QUALITY:       return 1.5f;
        case XESS_QUALITY_SETTING_QUALITY:             return 1.7f;
        case XESS_QUALITY_SETTING_BALANCED:            return 2.0f;
        case XESS_QUALITY_SETTING_PERFORMANCE:         return 2.3f;
        case XESS_QUALITY_SETTING_ULTRA_PERFORMANCE:   return 3.0f;
        default:                                       return 1.7f;
        }
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

    std::string ResultText(xess_result_t result)
    {
        switch (result)
        {
        case XESS_RESULT_ERROR_UNSUPPORTED_DEVICE:  return "the GPU does not support XeSS (Shader Model 6.4 is required)";
        case XESS_RESULT_ERROR_UNSUPPORTED_DRIVER:  return "the graphics driver is too old for XeSS";
        case XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY: return "out of GPU memory";
        case XESS_RESULT_ERROR_CANT_LOAD_LIBRARY:   return "a XeSS runtime library could not be loaded";
        default:                                    return "error code " + std::to_string(static_cast<int>(result));
        }
    }
}

int XessRenderer::QualityForMode(int mode)
{
    static constexpr int kQualities[] =
    {
        XESS_QUALITY_SETTING_AA,
        XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS,
        XESS_QUALITY_SETTING_ULTRA_QUALITY,
        XESS_QUALITY_SETTING_QUALITY,
        XESS_QUALITY_SETTING_BALANCED,
        XESS_QUALITY_SETTING_PERFORMANCE,
        XESS_QUALITY_SETTING_ULTRA_PERFORMANCE,
    };
    return kQualities[std::clamp(mode, 0, static_cast<int>(std::size(kQualities)) - 1)];
}

bool XessRenderer::Initialize()
{
    mLastError.clear();
    if (mContext != nullptr)
        return true;

    const XessLoader::SuperResolutionFunctions* xess = XessLoader::SuperResolution();
    ID3D12Device* device = DX12Context_GetDevice();
    if (xess == nullptr || device == nullptr)
    {
        mLastError = XessLoader::GetLastError() ? XessLoader::GetLastError() : "XeSS: libxess.dll is unavailable.";
        return false;
    }

    xess_context_handle_t context = nullptr;
    const xess_result_t result = xess->D3D12CreateContext(device, &context);
    if (result != XESS_RESULT_SUCCESS || context == nullptr)
    {
        mLastError = "XeSS super resolution is unavailable: " + ResultText(result) + ".";
        PteroLog::Write(PteroLog::Level::Warning, "XeSS", mLastError.c_str());
        return false;
    }
    mContext = context;

    xess->SetLoggingCallback(context, XESS_LOGGING_LEVEL_WARNING, &XessLoader::SuperResolutionLog);
    if (xess->IsOptimalDriver(context) == XESS_RESULT_WARNING_OLD_DRIVER)
        PteroLog::Write(PteroLog::Level::Warning, "XeSS", "The installed driver may give degraded XeSS quality or performance.");

    xess_version_t version{};
    if (xess->GetVersion(&version) == XESS_RESULT_SUCCESS)
        mVersionName = "XeSS-SR " + std::to_string(version.major) + "." + std::to_string(version.minor) + "." + std::to_string(version.patch);

    // Start compiling the kernels now, in the background, so switching XeSS on
    // later does not stall on it. Init waits for the build if it is still going.
    xess->D3D12BuildPipelines(context, nullptr, false, XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE);

    PteroLog::Writef(PteroLog::Level::Info, "XeSS", "Super resolution context created (%s).",
                     mVersionName.empty() ? "unknown version" : mVersionName.c_str());
    return true;
}

void XessRenderer::Shutdown()
{
    ReleaseResources();
    if (mContext != nullptr)
    {
        DX12Context_WaitForGPU();
        if (const XessLoader::SuperResolutionFunctions* xess = XessLoader::SuperResolution())
            xess->DestroyContext(AsContext(mContext));
        mContext = nullptr;
    }
}

void XessRenderer::ReleaseResources()
{
    if (!mInitialized && mOutputTexture == nullptr)
        return;

    // The context stays: it is what answers availability. Only the output-size
    // state goes, and the next EnsureSize initialises again.
    DX12Context_WaitForGPU();
    mOutputTexture.Reset();
    mOutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    mInitialized = false;
    mInitializedQuality = -1;
    mRenderWidth = mRenderHeight = mOutputWidth = mOutputHeight = 0;
}

bool XessRenderer::InitializeForOutput(UINT outputWidth, UINT outputHeight, int quality)
{
    const XessLoader::SuperResolutionFunctions* xess = XessLoader::SuperResolution();
    if (xess == nullptr || mContext == nullptr)
        return false;

    // Scene colour is linear HDR in scene units, before bloom, exposure and
    // tonemapping, so XeSS meters it itself. Motion vectors are at render size
    // and carry no jitter; depth is standard [0 near, 1 far].
    xess_d3d12_init_params_t init{};
    init.outputResolution = { outputWidth, outputHeight };
    init.qualitySetting = static_cast<xess_quality_settings_t>(quality);
    init.initFlags = XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE;
    init.creationNodeMask = 1;
    init.visibleNodeMask = 1;

    const xess_result_t result = xess->D3D12Init(AsContext(mContext), &init);
    if (result != XESS_RESULT_SUCCESS)
    {
        mLastError = "XeSS: initialisation failed: " + ResultText(result) + ".";
        PteroLog::Write(PteroLog::Level::Error, "XeSS", mLastError.c_str());
        return false;
    }

    mInitialized = true;
    mInitializedQuality = quality;
    PteroLog::Writef(PteroLog::Level::Info, "XeSS", "Super resolution initialised for %ux%u output.",
                     outputWidth, outputHeight);
    return true;
}

bool XessRenderer::CreateOutputTexture(UINT outputWidth, UINT outputHeight)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "XeSS: null D3D12 device.";
        return false;
    }

    if (!mOutputSrvAllocated)
    {
        if (!DX12Context_AllocateSrvDescriptor(&mOutputSrvCpu, &mOutputSrvGpu))
        {
            mLastError = "XeSS: failed to allocate the output SRV descriptor.";
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
    desc.Format = XessOutputFormat;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&mOutputTexture))))
    {
        mLastError = "XeSS: failed to create the output texture.";
        return false;
    }
    mOutputTexture->SetName(L"XeSS Output");
    mOutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = XessOutputFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(mOutputTexture.Get(), &srvDesc, mOutputSrvCpu);
    return true;
}

bool XessRenderer::EnsureSize(UINT renderWidth, UINT renderHeight, UINT outputWidth, UINT outputHeight,
                              const XessSettings& settings)
{
    if (mContext == nullptr || renderWidth == 0 || renderHeight == 0 || outputWidth == 0 || outputHeight == 0)
        return false;

    // The render size is a per-dispatch parameter; the output size and the
    // preset are baked into the initialisation.
    mRenderWidth = renderWidth;
    mRenderHeight = renderHeight;
    const int quality = QualityForMode(settings.Mode);
    if (mInitialized && mOutputTexture && mOutputWidth == outputWidth && mOutputHeight == outputHeight
        && mInitializedQuality == quality)
    {
        return true;
    }

    // Reinitialising requires every command list that used XeSS to be done.
    DX12Context_WaitForGPU();
    mInitialized = false;
    if (mOutputWidth != outputWidth || mOutputHeight != outputHeight)
        mOutputTexture.Reset();

    if ((!mOutputTexture && !CreateOutputTexture(outputWidth, outputHeight))
        || !InitializeForOutput(outputWidth, outputHeight, quality))
    {
        mOutputTexture.Reset();
        mInitialized = false;
        mOutputWidth = mOutputHeight = 0;
        return false;
    }

    mOutputWidth = outputWidth;
    mOutputHeight = outputHeight;
    mLastError.clear();
    return true;
}

bool XessRenderer::QueryRenderSize(UINT outputWidth, UINT outputHeight, const XessSettings& settings,
                                   UINT& outRenderWidth, UINT& outRenderHeight)
{
    outRenderWidth = outputWidth;
    outRenderHeight = outputHeight;
    if (mContext == nullptr || outputWidth == 0 || outputHeight == 0)
        return false;

    const int quality = QualityForMode(settings.Mode);
    xess_2d_t output{ outputWidth, outputHeight };
    xess_2d_t optimal{}, minimum{}, maximum{};
    const XessLoader::SuperResolutionFunctions* xess = XessLoader::SuperResolution();
    if (xess == nullptr
        || xess->GetOptimalInputResolution(AsContext(mContext), &output,
               static_cast<xess_quality_settings_t>(quality), &optimal, &minimum, &maximum) != XESS_RESULT_SUCCESS
        || optimal.x == 0 || optimal.y == 0)
    {
        const float ratio = RatioForQuality(quality);
        optimal.x = static_cast<uint32_t>(static_cast<float>(outputWidth) / ratio);
        optimal.y = static_cast<uint32_t>(static_cast<float>(outputHeight) / ratio);
    }

    outRenderWidth = (std::max)(1u, (std::min)(static_cast<UINT>(optimal.x), outputWidth));
    outRenderHeight = (std::max)(1u, (std::min)(static_cast<UINT>(optimal.y), outputHeight));
    return true;
}

void XessRenderer::GetJitterOffset(UINT frameIndex, UINT renderWidth, UINT outputWidth, float& outX, float& outY)
{
    outX = 0.0f;
    outY = 0.0f;
    if (renderWidth == 0 || outputWidth == 0)
        return;

    // Intel's guidance: a length-8 sequence at native becomes 8 * ratio^2 when
    // upscaling, so each render pixel's footprint in the output is covered.
    const float ratio = (std::max)(1.0f, static_cast<float>(outputWidth) / static_cast<float>(renderWidth));
    const UINT phaseCount = (std::max)(8u, static_cast<UINT>(std::ceil(8.0f * ratio * ratio)));
    const UINT index = (frameIndex % phaseCount) + 1; // Halton index 0 is (0,0)
    outX = Halton(index, 2) - 0.5f;
    outY = Halton(index, 3) - 0.5f;
}

void XessRenderer::Evaluate(
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* inputColor,
    ID3D12Resource* depth,
    ID3D12Resource* motionVectors,
    const FrameData& frameData,
    XessSettings& settings)
{
    const XessLoader::SuperResolutionFunctions* xess = XessLoader::SuperResolution();
    if (!(xess && mContext && mInitialized && mOutputTexture && commandList && inputColor && depth && motionVectors))
        return;

    if (mOutputState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        const auto toUav = CD3DX12_RESOURCE_BARRIER::Transition(
            mOutputTexture.Get(), mOutputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->ResourceBarrier(1, &toUav);
        mOutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    // The engine writes (current - previous) in UV space; XeSS wants the offset
    // from the current pixel back to where it was, in render-resolution pixels.
    xess->SetVelocityScale(AsContext(mContext), -static_cast<float>(mRenderWidth), -static_cast<float>(mRenderHeight));

    xess_d3d12_execute_params_t execute{};
    execute.pColorTexture = inputColor;
    execute.pVelocityTexture = motionVectors;
    execute.pDepthTexture = depth;
    execute.pOutputTexture = mOutputTexture.Get();
    execute.jitterOffsetX = std::clamp(frameData.JitterX, -0.5f, 0.5f);
    execute.jitterOffsetY = std::clamp(frameData.JitterY, -0.5f, 0.5f);
    execute.exposureScale = 1.0f;
    execute.resetHistory = (frameData.Reset || settings.ResetHistory) ? 1u : 0u;
    execute.inputWidth = mRenderWidth;
    execute.inputHeight = mRenderHeight;

    const xess_result_t result = xess->D3D12Execute(AsContext(mContext), commandList, &execute);
    if (result != XESS_RESULT_SUCCESS)
    {
        mLastError = "XeSS: execute failed: " + ResultText(result) + ".";
        return;
    }

    const auto toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
        mOutputTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &toSrv);
    mOutputState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    settings.ResetHistory = false;
    mLastError.clear();
}
