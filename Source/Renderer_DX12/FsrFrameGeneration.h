#pragma once

#include "DX12Helper.h"
#include "FsrSettings.h"

#include <DirectXMath.h>
#include <string>

// AMD FSR frame generation. Interpolates one frame between every pair the
// renderer presents, inside the FSR proxy swap chain that DX12Context installs
// in place of the plain one.
//
// A frame goes through it in three steps, all recorded on the frame's own
// command list before Present:
//   1. Update        - once, before recording: creates or drops the context to
//                      match the settings, the swap chain and its size.
//   2. Prepare       - from the scene renderer, once depth and motion vectors
//                      exist: hands them over for this frame.
//   3. FinishFrame   - after the scene is in the back buffer and before the game
//                      UI goes over it: keeps a HUD-less copy so the UI is not
//                      smeared across the interpolated frames.
class FsrFrameGeneration
{
public:
    struct PrepareData
    {
        UINT  RenderWidth = 0;
        UINT  RenderHeight = 0;
        // Pixel jitter in FSR's convention: +X right, +Y down.
        float JitterX = 0.0f;
        float JitterY = 0.0f;
        float FrameTimeDeltaMs = 16.6f;
        float NearPlane = 0.1f;
        float FarPlane = 1000.0f;
        float FovY = 0.785398f;
        bool  Reset = false;
        DirectX::XMFLOAT3 CameraPosition{};
        DirectX::XMFLOAT3 CameraUp{};
        DirectX::XMFLOAT3 CameraRight{};
        DirectX::XMFLOAT3 CameraForward{};
    };

    // True when the loader found a frame generation provider for this device.
    bool IsApiAvailable();

    // swapChain is the FSR proxy (null when the plain chain is installed).
    void Update(IDXGISwapChain* swapChain, UINT displayWidth, UINT displayHeight,
                UINT renderWidth, UINT renderHeight, const FsrSettings& settings);

    // Inputs must be in D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE.
    void Prepare(ID3D12GraphicsCommandList* commandList, ID3D12Resource* depth,
                 ID3D12Resource* motionVectors, const PrepareData& data, const FsrSettings& settings);

    // backBuffer must be in RENDER_TARGET state and is left in it. Call every
    // frame the context exists, prepared or not: an unprepared frame switches
    // interpolation off for that present instead of interpolating stale inputs.
    void FinishFrame(ID3D12GraphicsCommandList* commandList, ID3D12Resource* backBuffer, const FsrSettings& settings);

    // Must run while the swap chain it was configured with is still alive and
    // unchanged: before it is resized, replaced or parked.
    void Release();

    bool IsActive() const { return mContext != nullptr; }
    const std::string& GetVersionName() const { return mVersionName; }
    const char* GetLastErrorMessage() const { return mLastError.empty() ? nullptr : mLastError.c_str(); }

private:
    bool Create(IDXGISwapChain* swapChain, UINT displayWidth, UINT displayHeight, UINT maxRenderWidth, UINT maxRenderHeight);
    bool CreateHudlessTextures(UINT width, UINT height);
    void Configure(bool enabled, const FsrSettings& settings);
    uint32_t DebugFlags(const FsrSettings& settings) const;

    void*           mContext = nullptr; // ffxContext
    IDXGISwapChain* mSwapChain = nullptr;
    UINT            mDisplayWidth = 0;
    UINT            mDisplayHeight = 0;
    UINT            mMaxRenderWidth = 0;
    UINT            mMaxRenderHeight = 0;

    // Must advance by exactly one per presented frame: any other step makes FSR
    // treat the frame as a discontinuity and reset.
    uint64_t mFrameId = 0;
    bool     mPreparedThisFrame = false;

    // Double-buffered because the copy made for frame N is still read when the
    // proxy interpolates at present time, while frame N+1 is already recording.
    Microsoft::WRL::ComPtr<ID3D12Resource> mHudless[2];
    UINT mHudlessIndex = 0;

    int         mApiAvailable = -1; // -1 not checked yet
    std::string mVersionName;
    std::string mLastError;
};
