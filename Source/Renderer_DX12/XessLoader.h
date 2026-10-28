#pragma once

// Runtime access to Intel XeSS 3: super resolution (libxess.dll), frame
// generation (libxess_fg.dll) and Xe Low Latency (libxell.dll).
//
// Loaded with LoadLibrary rather than linked, like the FidelityFX loader, so
// the editor still starts on a machine without the DLLs - each XeSS feature
// then reports itself unavailable. The headers are only used for their types
// and function signatures; nothing is called through an import library.

#include "xess-3.0.2/inc/xess/xess_d3d12.h"
#include "xess-3.0.2/inc/xess_fg/xefg_swapchain_d3d12.h"
#include "xess-3.0.2/inc/xell/xell_d3d12.h"

namespace XessLoader
{
    // libxess.dll
    struct SuperResolutionFunctions
    {
        decltype(&xessGetVersion)                  GetVersion = nullptr;
        decltype(&xessD3D12CreateContext)          D3D12CreateContext = nullptr;
        decltype(&xessD3D12BuildPipelines)         D3D12BuildPipelines = nullptr;
        decltype(&xessD3D12Init)                   D3D12Init = nullptr;
        decltype(&xessD3D12Execute)                D3D12Execute = nullptr;
        decltype(&xessDestroyContext)              DestroyContext = nullptr;
        decltype(&xessGetOptimalInputResolution)   GetOptimalInputResolution = nullptr;
        decltype(&xessSetVelocityScale)            SetVelocityScale = nullptr;
        decltype(&xessSetJitterScale)              SetJitterScale = nullptr;
        decltype(&xessSetLoggingCallback)          SetLoggingCallback = nullptr;
        decltype(&xessIsOptimalDriver)             IsOptimalDriver = nullptr;
    };

    // libxess_fg.dll
    struct FrameGenerationFunctions
    {
        decltype(&xefgSwapChainGetVersion)                  GetVersion = nullptr;
        decltype(&xefgSwapChainD3D12CreateContext)          D3D12CreateContext = nullptr;
        decltype(&xefgSwapChainD3D12InitFromSwapChainDesc)  D3D12InitFromSwapChainDesc = nullptr;
        decltype(&xefgSwapChainD3D12GetSwapChainPtr)        D3D12GetSwapChainPtr = nullptr;
        decltype(&xefgSwapChainD3D12TagFrameResource)       D3D12TagFrameResource = nullptr;
        decltype(&xefgSwapChainD3D12GetInitializationParameters) D3D12GetInitializationParameters = nullptr;
        decltype(&xefgSwapChainTagFrameConstants)           TagFrameConstants = nullptr;
        decltype(&xefgSwapChainSetEnabled)                  SetEnabled = nullptr;
        decltype(&xefgSwapChainSetPresentId)                SetPresentId = nullptr;
        decltype(&xefgSwapChainGetLastPresentStatus)        GetLastPresentStatus = nullptr;
        decltype(&xefgSwapChainSetLoggingCallback)          SetLoggingCallback = nullptr;
        decltype(&xefgSwapChainDestroy)                     Destroy = nullptr;
        decltype(&xefgSwapChainSetLatencyReduction)         SetLatencyReduction = nullptr;
        decltype(&xefgSwapChainSetNumInterpolatedFrames)    SetNumInterpolatedFrames = nullptr;
        decltype(&xefgSwapChainSetUiCompositionState)       SetUiCompositionState = nullptr;
    };

    // libxell.dll
    struct LowLatencyFunctions
    {
        decltype(&xellD3D12CreateContext) D3D12CreateContext = nullptr;
        decltype(&xellDestroyContext)     DestroyContext = nullptr;
        decltype(&xellSetSleepMode)       SetSleepMode = nullptr;
        decltype(&xellSleep)              Sleep = nullptr;
        decltype(&xellAddMarkerData)      AddMarkerData = nullptr;
        decltype(&xellGetVersion)         GetVersion = nullptr;
        decltype(&xellSetLoggingCallback) SetLoggingCallback = nullptr;
    };

    // Null when the DLL is missing or lacks an export. Each loads on its first
    // call; later calls are free.
    const SuperResolutionFunctions* SuperResolution();
    const FrameGenerationFunctions* FrameGeneration();
    const LowLatencyFunctions*      LowLatency();

    // Why the last load failed, for the settings panel and the log.
    const char* GetLastError();

    // Routes the libraries' messages into the session log.
    void SuperResolutionLog(const char* message, xess_logging_level_t level);
    void FrameGenerationLog(const char* message, xefg_swapchain_logging_level_t level, void* userData);
    void LowLatencyLog(const char* message, xell_logging_level_t level);
}
