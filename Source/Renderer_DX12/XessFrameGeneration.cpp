#include "pch.h"
#include "XessFrameGeneration.h"

#include "XessLoader.h"
#include "System/PteroLog.h"

#include <algorithm>
#include <cstring>

extern "C"
{
    void* __stdcall DX12Context_GetXessFrameGenerationContext();
    UINT __stdcall DX12Context_GetXessFrameId();
}

namespace
{
    xefg_swapchain_handle_t AsHandle(void* context)
    {
        return static_cast<xefg_swapchain_handle_t>(context);
    }

    bool Succeeded(xefg_swapchain_result_t result)
    {
        return static_cast<int>(result) >= 0;
    }
}

bool XessFrameGeneration::IsApiAvailable()
{
    if (mApiAvailable >= 0)
        return mApiAvailable != 0;

    // Both are needed: the proxy will not initialise without a XeLL context.
    mApiAvailable = (XessLoader::FrameGeneration() != nullptr && XessLoader::LowLatency() != nullptr) ? 1 : 0;
    if (mApiAvailable == 0)
        mLastError = XessLoader::GetLastError() ? XessLoader::GetLastError() : "XeSS frame generation libraries are unavailable.";
    return mApiAvailable != 0;
}

const std::string& XessFrameGeneration::GetVersionName()
{
    if (mVersionName.empty())
    {
        if (const XessLoader::FrameGenerationFunctions* xefg = XessLoader::FrameGeneration())
        {
            xefg_swapchain_version_t version{};
            if (Succeeded(xefg->GetVersion(&version)))
                mVersionName = "XeSS-FG " + std::to_string(version.major) + "." + std::to_string(version.minor) + "." + std::to_string(version.patch);
        }
    }
    return mVersionName;
}

void XessFrameGeneration::Release()
{
    mContext = nullptr;
    mEnabled = false;
    mAppliedInterpolatedFrames = -1;
    mAppliedUiComposition = -1;
    mMaxInterpolatedFrames = 1;
    mPreparedThisFrame = false;
}

void XessFrameGeneration::Update(const XessSettings& settings)
{
    mPreparedThisFrame = false;

    void* handle = DX12Context_GetXessFrameGenerationContext();
    if (handle != mContext)
    {
        // A new proxy (or none): everything applied to the old one is gone.
        Release();
        mContext = handle;
        if (mContext != nullptr)
        {
            if (const XessLoader::FrameGenerationFunctions* xefg = XessLoader::FrameGeneration())
            {
                xefg_swapchain_d3d12_init_params_t init{};
                if (Succeeded(xefg->D3D12GetInitializationParameters(AsHandle(mContext), &init)) && init.maxInterpolatedFrames > 0)
                    mMaxInterpolatedFrames = init.maxInterpolatedFrames;
            }
            mLastError.clear();
        }
    }

    const XessLoader::FrameGenerationFunctions* xefg = XessLoader::FrameGeneration();
    if (mContext == nullptr || xefg == nullptr)
        return;

    const xefg_swapchain_handle_t context = AsHandle(mContext);

    const int frames = std::clamp(settings.InterpolatedFrames, 1, static_cast<int>(mMaxInterpolatedFrames));
    if (frames != mAppliedInterpolatedFrames)
    {
        const xefg_swapchain_result_t result = xefg->SetNumInterpolatedFrames(context, static_cast<uint32_t>(frames));
        if (!Succeeded(result))
            mLastError = "XeSS-FG: could not set " + std::to_string(frames) + " generated frames (" + std::to_string(static_cast<int>(result)) + ").";
        mAppliedInterpolatedFrames = frames;
    }

    const int uiComposition = settings.UiComposition ? 1 : 0;
    if (uiComposition != mAppliedUiComposition)
    {
        xefg->SetUiCompositionState(context, uiComposition
            ? XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED
            : XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_DISABLED);
        mAppliedUiComposition = uiComposition;
    }

    if (settings.FrameGeneration != mEnabled)
    {
        const xefg_swapchain_result_t result = xefg->SetEnabled(context, settings.FrameGeneration ? 1u : 0u);
        if (Succeeded(result))
        {
            mEnabled = settings.FrameGeneration;
            PteroLog::Writef(PteroLog::Level::Info, "XeSS-FG", "Frame generation %s (%d generated frame%s).",
                mEnabled ? "enabled" : "disabled", frames, frames == 1 ? "" : "s");
        }
        else
        {
            mLastError = "XeSS-FG: could not enable frame generation (" + std::to_string(static_cast<int>(result)) + ").";
        }
    }
}

void XessFrameGeneration::Prepare(ID3D12GraphicsCommandList* commandList, ID3D12Resource* depth,
                                  ID3D12Resource* motionVectors, const PrepareData& data)
{
    const XessLoader::FrameGenerationFunctions* xefg = XessLoader::FrameGeneration();
    if (!(xefg && mContext && mEnabled && commandList && depth && motionVectors) || mPreparedThisFrame)
        return;

    const xefg_swapchain_handle_t context = AsHandle(mContext);
    const uint32_t presentId = DX12Context_GetXessFrameId();

    // Copied now, on this command list: depth and motion vectors are rewritten
    // next frame, and their state at present time is not this one.
    const auto tag = [&](xefg_swapchain_resource_type_t type, ID3D12Resource* resource)
    {
        xefg_swapchain_d3d12_resource_data_t resourceData{};
        resourceData.type = type;
        resourceData.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
        resourceData.resourceBase = { 0, 0 };
        resourceData.resourceSize = { data.RenderWidth, data.RenderHeight };
        resourceData.pResource = resource;
        resourceData.incomingState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        return xefg->D3D12TagFrameResource(context, commandList, presentId, &resourceData);
    };

    xefg_swapchain_result_t result = tag(XEFG_SWAPCHAIN_RES_DEPTH, depth);
    if (Succeeded(result))
        result = tag(XEFG_SWAPCHAIN_RES_MOTION_VECTOR, motionVectors);

    if (Succeeded(result))
    {
        xefg_swapchain_frame_constant_data_t constants{};
        std::memcpy(constants.viewMatrix, &data.View.m[0][0], sizeof(constants.viewMatrix));
        std::memcpy(constants.projectionMatrix, &data.Projection.m[0][0], sizeof(constants.projectionMatrix));
        constants.jitterOffsetX = data.JitterX;
        constants.jitterOffsetY = data.JitterY;
        // Engine vectors are (current - previous) in UV; XeSS-FG wants the offset
        // from the current pixel back to the previous one, in render pixels.
        constants.motionVectorScaleX = -static_cast<float>(data.RenderWidth);
        constants.motionVectorScaleY = -static_cast<float>(data.RenderHeight);
        constants.resetHistory = data.Reset ? 1u : 0u;
        constants.frameRenderTime = data.FrameTimeDeltaMs;
        result = xefg->TagFrameConstants(context, presentId, &constants);
    }

    if (!Succeeded(result))
    {
        mLastError = "XeSS-FG: tagging the frame's inputs failed (" + std::to_string(static_cast<int>(result)) + ").";
        return;
    }

    mPreparedThisFrame = true;
}

void XessFrameGeneration::FinishFrame(ID3D12GraphicsCommandList* commandList, ID3D12Resource* backBuffer,
                                      UINT width, UINT height)
{
    const XessLoader::FrameGenerationFunctions* xefg = XessLoader::FrameGeneration();
    const bool prepared = mPreparedThisFrame;
    mPreparedThisFrame = false;
    if (!(xefg && mContext && mEnabled && prepared && commandList && backBuffer) || width == 0 || height == 0)
        return;

    // The back buffer holds the scene and nothing else yet. A copy of it lets
    // XeSS-FG tell the game UI drawn next apart from the scene.
    xefg_swapchain_d3d12_resource_data_t resourceData{};
    resourceData.type = XEFG_SWAPCHAIN_RES_HUDLESS_COLOR;
    resourceData.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
    resourceData.resourceBase = { 0, 0 };
    resourceData.resourceSize = { width, height };
    resourceData.pResource = backBuffer;
    resourceData.incomingState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    const xefg_swapchain_result_t result =
        xefg->D3D12TagFrameResource(AsHandle(mContext), commandList, DX12Context_GetXessFrameId(), &resourceData);
    if (!Succeeded(result))
        mLastError = "XeSS-FG: tagging the HUD-less colour failed (" + std::to_string(static_cast<int>(result)) + ").";
}
