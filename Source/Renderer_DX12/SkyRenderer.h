#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "TimeOfDaySettings.h"
#include "HosekWilkieSky.h"
#include "TextureManager.h"

#include <memory>

#include <DirectXMath.h>
#include <wrl/client.h>
#include <string>

// SkyRenderer
// Renders a fullscreen sky background - sky gradient, sun disc, the moon with
// its phase, and a star field that wheels with the hour - from the evaluated
// time of day. Must be drawn BEFORE the scene geometry so the depth test hides it.
class SkyRenderer
{
public:
    bool Initialize(DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat);
    void Shutdown();

    // Draw the sky.  Call this while the scene RTV+DSV are bound, BEFORE
    // opaque geometry, so the sky only shows through untouched pixels.
    void Render(
        ID3D12GraphicsCommandList* commandList,
        const HosekWilkieResult&   hosekResult,
        const TimeOfDaySettings&   settings,
        const DirectX::XMMATRIX&   projectionMatrix,
        const DirectX::XMMATRIX&   viewMatrix,
        UINT                       viewportWidth,
        UINT                       viewportHeight,
        float                      deltaSeconds);

    bool IsInitialized() const { return mIsInitialized; }

    const char* GetLastError() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    // Constant buffer layout — must match SkyPass.hlsl. Directions are world
    // space and point from the scene toward the body.
    struct alignas(256) SkyConstants
    {
        DirectX::XMFLOAT4X4 InvProj;
        DirectX::XMFLOAT4X4 InvView;
        DirectX::XMFLOAT3 SkyZenithColor;    float PixelAngle;      // radians per pixel
        DirectX::XMFLOAT3 SkyHorizonColor;   float TimeSeconds;
        DirectX::XMFLOAT3 SunDirection;      float SunDiscHalfAngleCos;
        DirectX::XMFLOAT3 SunColor;          float MoonDiscSin;     // sin of the moon's angular radius
        DirectX::XMFLOAT3 MoonDirection;     float StarVisibility;
        DirectX::XMFLOAT3 MoonColor;         float StarRadiance;
        DirectX::XMFLOAT3 TwilightGlowColor; float MoonHalo;
        DirectX::XMFLOAT4 StarRotation[3];  // rows, xyz used
        // 1 when the texture is bound, else the procedural fallback is drawn.
        float MoonTextureOn;
        float SunTextureOn;
        // Gains on the star map and on the procedural stars / Milky Way; 0 = off.
        float StarMapGain;
        float ProceduralStarGain;
        float ProceduralMilkyWayGain;
        float Pad3[3];
    };

    // The sky textures, loaded on first use. A texture that fails to load is
    // simply left out: the shader falls back to the procedural moon, sun disc or
    // stars. They live under Data/Textures/Sky; see CREDITS.txt there.
    enum SkyTexture { kMoonTexture, kSunTexture, kStarTexture, kSkyTextureCount };
    void LoadTextures();

    TextureManager              mTextureManager;
    std::shared_ptr<GpuTexture> mTextures[kSkyTextureCount];
    bool                        mTexturesRequested = false;

    // The constants change every frame (time, the camera), so they are ringed
    // one copy per frame in flight.
    static constexpr UINT kFramesInFlight = 3;

    DX12Shader  mVertexShader;
    DX12Shader  mPixelShader;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPipelineState;
    Microsoft::WRL::ComPtr<ID3D12Resource>      mConstantBuffer;
    void*  mMappedCb = nullptr;
    UINT64 mCbStride = 0;
    UINT   mFrameSlot = 0;
    // Drives the stars' twinkle; wraps so float precision never degrades it.
    float  mTimeSeconds = 0.0f;

    bool        mIsInitialized = false;
    std::string mLastError;
};
