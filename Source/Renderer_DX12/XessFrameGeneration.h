#pragma once

#include "DX12Helper.h"
#include "XessSettings.h"

#include <DirectXMath.h>
#include <string>

// Intel XeSS frame generation (XeSS-FG 3). Generates frames between the ones the
// renderer presents, inside the XeSS-FG proxy swap chain that DX12Context
// installs in place of the plain one (DX12Context_SetFrameGenerationProxy(2)).
// The proxy needs Xe Low Latency, which DX12Context switches on with it.
//
// A frame goes through it in three steps, all recorded on the frame's own
// command list before Present:
//   1. Update        - once, before recording: picks up the proxy chain and
//                      applies the settings (on/off, frame count, UI mode).
//   2. Prepare       - from the scene renderer, once depth and motion vectors
//                      exist: tags them and the frame's camera constants.
//   3. FinishFrame   - after the scene is in the back buffer and before the game
//                      UI goes over it: tags that UI-less image.
class XessFrameGeneration
{
public:
    struct PrepareData
    {
        UINT  RenderWidth = 0;
        UINT  RenderHeight = 0;
        // Pixel jitter in XeSS's convention: +X right, +Y down.
        float JitterX = 0.0f;
        float JitterY = 0.0f;
        float FrameTimeDeltaMs = 16.6f;
        bool  Reset = false;
        // Without jitter, row-vector (DirectXMath) convention.
        DirectX::XMFLOAT4X4 View{};
        DirectX::XMFLOAT4X4 Projection{};
    };

    // True when libxess_fg.dll and libxell.dll could be loaded. Whether the GPU
    // actually supports it is only known once the proxy chain is created.
    bool IsApiAvailable();

    // Called once per frame after the swap chain is settled.
    void Update(const XessSettings& settings);

    // Inputs must be in D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE; XeSS-FG copies
    // them on this command list, so they may be overwritten afterwards.
    void Prepare(ID3D12GraphicsCommandList* commandList, ID3D12Resource* depth,
                 ID3D12Resource* motionVectors, const PrepareData& data);

    // backBuffer must be in RENDER_TARGET state and is left in it.
    void FinishFrame(ID3D12GraphicsCommandList* commandList, ID3D12Resource* backBuffer,
                     UINT width, UINT height);

    // Forgets the proxy chain's state; the next Update picks it up afresh.
    void Release();

    bool IsActive() const { return mContext != nullptr && mEnabled; }
    UINT GetMaxInterpolatedFrames() const { return mMaxInterpolatedFrames; }
    const std::string& GetVersionName();
    const char* GetLastErrorMessage() const { return mLastError.empty() ? nullptr : mLastError.c_str(); }

private:
    void* mContext = nullptr; // xefg_swapchain_handle_t of the active proxy
    bool  mEnabled = false;
    int   mAppliedInterpolatedFrames = -1;
    int   mAppliedUiComposition = -1;
    UINT  mMaxInterpolatedFrames = 1;
    bool  mPreparedThisFrame = false;

    int         mApiAvailable = -1; // -1 not checked yet
    std::string mVersionName;
    std::string mLastError;
};
