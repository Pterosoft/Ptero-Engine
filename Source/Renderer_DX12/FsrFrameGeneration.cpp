#include "pch.h"
#include "FsrFrameGeneration.h"

#include "FfxLoader.h"
#include "FidelityFX-SDK-2.3.0/Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h"
#include "FidelityFX-SDK-2.3.0/Kits/FidelityFX/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"
#include "System/PteroLog.h"

#include <algorithm>

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
    bool __stdcall DX12Context_WaitForGPU();
}

namespace
{
    // Matches the swap chain DX12Context creates.
    constexpr DXGI_FORMAT BackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    ffxContext* AsContext(void*& context)
    {
        return reinterpret_cast<ffxContext*>(&context);
    }

    // Called by the proxy swap chain when it is time to generate a frame. The user
    // context is the address of the owning object's context handle.
    ffxReturnCode_t DispatchFrameGeneration(ffxDispatchDescFrameGeneration* params, void* userContext)
    {
        const ffxFunctions* ffx = FfxLoader::Get();
        if (ffx == nullptr || userContext == nullptr)
            return FFX_API_RETURN_ERROR;
        return ffx->Dispatch(reinterpret_cast<ffxContext*>(userContext), &params->header);
    }
}

bool FsrFrameGeneration::IsApiAvailable()
{
    if (mApiAvailable >= 0)
        return mApiAvailable != 0;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false; // too early to tell; ask again once the device exists

    mApiAvailable = 0;
    const ffxFunctions* ffx = FfxLoader::Get();
    if (ffx == nullptr)
    {
        mLastError = FfxLoader::GetLastError() ? FfxLoader::GetLastError() : "FSR: the FidelityFX API is unavailable.";
        return false;
    }

    uint64_t providerCount = 0;
    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    versions.device = device;
    versions.outputCount = &providerCount;
    if (ffx->Query(nullptr, &versions.header) != FFX_API_RETURN_OK || providerCount == 0)
    {
        mLastError = "FSR: no frame generation provider is available (amd_fidelityfx_framegeneration_dx12.dll missing or unsupported GPU).";
        PteroLog::Write(PteroLog::Level::Warning, "FSR", mLastError.c_str());
        return false;
    }

    mApiAvailable = 1;
    return true;
}

void FsrFrameGeneration::Update(IDXGISwapChain* swapChain, UINT displayWidth, UINT displayHeight,
                                UINT renderWidth, UINT renderHeight, const FsrSettings& settings)
{
    mPreparedThisFrame = false;

    const bool wanted = settings.FrameGeneration && swapChain != nullptr
        && displayWidth > 0 && displayHeight > 0 && IsApiAvailable();
    if (!wanted)
    {
        Release();
        return;
    }

    // A fixed viewport resolution can render larger than the window, and the
    // context must be able to accept whatever the scene renders at.
    const UINT maxRenderWidth = (std::max)(displayWidth, renderWidth);
    const UINT maxRenderHeight = (std::max)(displayHeight, renderHeight);
    if (mContext != nullptr
        && (swapChain != mSwapChain
            || displayWidth != mDisplayWidth || displayHeight != mDisplayHeight
            || renderWidth > mMaxRenderWidth || renderHeight > mMaxRenderHeight))
    {
        Release();
    }

    if (mContext == nullptr && !Create(swapChain, displayWidth, displayHeight, maxRenderWidth, maxRenderHeight))
        Release();
}

bool FsrFrameGeneration::CreateHudlessTextures(UINT width, UINT height)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = BackBufferFormat;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    for (auto& texture : mHudless)
    {
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&texture))))
        {
            return false;
        }
        texture->SetName(L"FSR Frame Generation HUD-less Color");
    }
    mHudlessIndex = 0;
    return true;
}

bool FsrFrameGeneration::Create(IDXGISwapChain* swapChain, UINT displayWidth, UINT displayHeight,
                                UINT maxRenderWidth, UINT maxRenderHeight)
{
    const ffxFunctions* ffx = FfxLoader::Get();
    if (ffx == nullptr)
        return false;

    if (!CreateHudlessTextures(displayWidth, displayHeight))
    {
        mLastError = "FSR: failed to create the frame generation HUD-less textures.";
        return false;
    }

    ffxCreateBackendDX12Desc backend = FfxLoader::MakeBackendDesc(DX12Context_GetDevice());

    ffxCreateContextDescFrameGenerationHudless hudless{};
    hudless.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
    hudless.header.pNext = &backend.header;
    hudless.hudlessBackBufferFormat = ffxApiGetSurfaceFormatDX12(BackBufferFormat);

    ffxCreateContextDescFrameGenerationVersion version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    version.header.pNext = &hudless.header;
    version.version = FFX_FRAMEGENERATION_VERSION;

    // The back buffer holds the tonemapped 8-bit image, so no HDR flag, and the
    // depth handed to Prepare is standard [0 near, 1 far] with a finite far plane.
    ffxCreateContextDescFrameGeneration create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    create.header.pNext = &version.header;
    create.flags = 0;
    create.displaySize = { displayWidth, displayHeight };
    create.maxRenderSize = { maxRenderWidth, maxRenderHeight };
    create.backBufferFormat = ffxApiGetSurfaceFormatDX12(BackBufferFormat);

    const ffxReturnCode_t result = ffx->CreateContext(AsContext(mContext), &create.header, nullptr);
    if (result != FFX_API_RETURN_OK)
    {
        mContext = nullptr;
        mLastError = "FSR: ffxCreateContext(frame generation) failed with code " + std::to_string(result) + ".";
        PteroLog::Write(PteroLog::Level::Error, "FSR", mLastError.c_str());
        return false;
    }

    mSwapChain = swapChain;
    mDisplayWidth = displayWidth;
    mDisplayHeight = displayHeight;
    mMaxRenderWidth = maxRenderWidth;
    mMaxRenderHeight = maxRenderHeight;
    mPreparedThisFrame = false;

    ffxQueryGetProviderVersion providerVersion{};
    providerVersion.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (ffx->Query(AsContext(mContext), &providerVersion.header) == FFX_API_RETURN_OK && providerVersion.versionName)
        mVersionName = providerVersion.versionName;
    else
        mVersionName.clear();

    mLastError.clear();
    PteroLog::Writef(PteroLog::Level::Info, "FSR", "Frame generation context created (%s) for %ux%u.",
                     mVersionName.empty() ? "unknown version" : mVersionName.c_str(), displayWidth, displayHeight);
    return true;
}

uint32_t FsrFrameGeneration::DebugFlags(const FsrSettings& settings) const
{
    uint32_t flags = 0;
    if (settings.FrameGenerationDebugTearLines)
        flags |= FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES;
    if (settings.FrameGenerationDebugResetIndicators)
        flags |= FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_RESET_INDICATORS;
    if (settings.FrameGenerationDebugView)
        flags |= FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW;
    return flags;
}

void FsrFrameGeneration::Configure(bool enabled, const FsrSettings& settings)
{
    const ffxFunctions* ffx = FfxLoader::Get();
    if (ffx == nullptr || mContext == nullptr)
        return;

    ffxConfigureDescFrameGeneration config{};
    config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    config.swapChain = mSwapChain;
    config.presentCallback = nullptr;
    config.presentCallbackUserContext = nullptr;
    config.frameGenerationCallback = &DispatchFrameGeneration;
    config.frameGenerationCallbackUserContext = AsContext(mContext);
    config.frameGenerationEnabled = enabled;
    config.allowAsyncWorkloads = false;
    config.HUDLessColor = enabled
        ? ffxApiGetResourceDX12(mHudless[mHudlessIndex].Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ)
        : ffxApiGetResourceDX12(nullptr);
    config.flags = DebugFlags(settings);
    config.onlyPresentGenerated = false;
    config.generationRect = { 0, 0, static_cast<int32_t>(mDisplayWidth), static_cast<int32_t>(mDisplayHeight) };
    config.frameID = mFrameId;

    const ffxReturnCode_t result = ffx->Configure(AsContext(mContext), &config.header);
    if (result != FFX_API_RETURN_OK)
        mLastError = "FSR: frame generation configure failed with code " + std::to_string(result) + ".";
}

void FsrFrameGeneration::Prepare(ID3D12GraphicsCommandList* commandList, ID3D12Resource* depth,
                                 ID3D12Resource* motionVectors, const PrepareData& data, const FsrSettings& settings)
{
    const ffxFunctions* ffx = FfxLoader::Get();
    if (!(ffx && mContext && commandList && depth && motionVectors) || mPreparedThisFrame)
        return;

    // Configured before the prepare dispatch, as the SDK sample does, so both
    // describe the same frame ID.
    Configure(true, settings);

    ffxDispatchDescFrameGenerationPrepareV2 prepare{};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    prepare.frameID = mFrameId;
    prepare.flags = DebugFlags(settings);
    prepare.commandList = commandList;
    prepare.renderSize = { data.RenderWidth, data.RenderHeight };
    prepare.jitterOffset = { data.JitterX, data.JitterY };
    // Same conversion as the upscaler: engine vectors are (current - previous) in UV.
    prepare.motionVectorScale = { -static_cast<float>(data.RenderWidth), -static_cast<float>(data.RenderHeight) };
    prepare.frameTimeDelta = data.FrameTimeDeltaMs;
    prepare.reset = data.Reset;
    prepare.cameraNear = data.NearPlane;
    prepare.cameraFar = data.FarPlane;
    prepare.cameraFovAngleVertical = data.FovY;
    prepare.viewSpaceToMetersFactor = 1.0f;
    prepare.depth = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    prepare.motionVectors = ffxApiGetResourceDX12(motionVectors, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    const auto copy3 = [](float* out, const DirectX::XMFLOAT3& v) { out[0] = v.x; out[1] = v.y; out[2] = v.z; };
    copy3(prepare.cameraPosition, data.CameraPosition);
    copy3(prepare.cameraUp, data.CameraUp);
    copy3(prepare.cameraRight, data.CameraRight);
    copy3(prepare.cameraForward, data.CameraForward);

    const ffxReturnCode_t result = ffx->Dispatch(AsContext(mContext), &prepare.header);
    if (result != FFX_API_RETURN_OK)
    {
        mLastError = "FSR: frame generation prepare failed with code " + std::to_string(result) + ".";
        Configure(false, settings);
        return;
    }

    mPreparedThisFrame = true;
}

void FsrFrameGeneration::FinishFrame(ID3D12GraphicsCommandList* commandList, ID3D12Resource* backBuffer,
                                     const FsrSettings& settings)
{
    if (mContext == nullptr)
        return;

    if (!mPreparedThisFrame || commandList == nullptr || backBuffer == nullptr)
    {
        // Nothing valid to interpolate from: present this frame as it is.
        Configure(false, settings);
    }
    else
    {
        ID3D12Resource* hudless = mHudless[mHudlessIndex].Get();
        D3D12_RESOURCE_BARRIER before[] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(hudless, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        commandList->ResourceBarrier(static_cast<UINT>(std::size(before)), before);
        commandList->CopyResource(hudless, backBuffer);
        D3D12_RESOURCE_BARRIER after[] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(hudless, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
        };
        commandList->ResourceBarrier(static_cast<UINT>(std::size(after)), after);
        mHudlessIndex = (mHudlessIndex + 1) % static_cast<UINT>(std::size(mHudless));
    }

    ++mFrameId;
    mPreparedThisFrame = false;
}

void FsrFrameGeneration::Release()
{
    mPreparedThisFrame = false;
    if (mContext == nullptr)
    {
        mHudless[0].Reset();
        mHudless[1].Reset();
        return;
    }

    if (const ffxFunctions* ffx = FfxLoader::Get())
    {
        // Switching generation off first makes the proxy wait for its queued
        // presents, so nothing it is still interpolating is destroyed under it.
        ffxConfigureDescFrameGeneration config{};
        config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        config.swapChain = mSwapChain;
        config.frameGenerationEnabled = false;
        config.HUDLessColor = ffxApiGetResourceDX12(nullptr);
        config.frameID = mFrameId;
        ffx->Configure(AsContext(mContext), &config.header);
        ffx->DestroyContext(AsContext(mContext), nullptr);
    }
    mContext = nullptr;
    mSwapChain = nullptr;

    // The HUD-less copies may still be referenced by work already queued.
    DX12Context_WaitForGPU();
    mHudless[0].Reset();
    mHudless[1].Reset();
    mDisplayWidth = mDisplayHeight = 0;
    mMaxRenderWidth = mMaxRenderHeight = 0;
    PteroLog::Write(PteroLog::Level::Info, "FSR", "Frame generation context released.");
}
