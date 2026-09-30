#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "LensFlareOptics.h"
#include "LensFlareSettings.h"

#include "../QtUi/UiTypes.h"

#include <DirectXMath.h>

#include <array>
#include <cstdint>
#include <future>
#include <string>
#include <vector>

// LensFlareRenderer - physically based lens flares for the sun and the level's lights.
//
// Each flaring light gets two things:
//   * Ghosts: light that reflects off two lens surfaces on its way to the sensor. They are
//     ray traced every frame through a real lens prescription (Data/LensFlares/Lenses,
//     LensFlareOptics), on the GPU, one ray grid per light x ghost x wavelength, and the
//     grids are rasterised with energy conservation - Hullin et al. 2011, ported from
//     Source/SDKs/LensFlareFramework-master. They move, stretch, fringe and take the shape
//     of the iris as the light moves across the frame, exactly as the lens would make them.
//   * A starburst: the aperture's diffraction pattern (LensDiffraction), centred on the light.
// Each light's visibility is measured on the GPU against the depth buffer (and, for the
// sun, against how bright its disc actually is, so clouds dim it), smoothed over time.
//
// Runs on linear HDR after bloom and before exposure / tonemapping, at output resolution.
// Like the other post passes it reads one image and writes its own output texture.
//
// The lens is placed so that its film covers the screen: a light at the screen's edge is
// traced at the angle the lens's own film edge sees. That keeps any lens usable with any
// game field of view.
class LensFlareRenderer
{
public:
    struct LightSource
    {
        bool IsSun = false;
        DirectX::XMFLOAT3 Position{};    // world position (local lights)
        DirectX::XMFLOAT3 Direction{};   // world direction towards the light (sun)
        DirectX::XMFLOAT3 Illuminance{}; // light arriving at the camera, engine light units
        float SourceRadius = 0.0f;       // world size of the emitter (local lights)
        std::uint32_t Key = 0;           // stable identity across frames
    };

    struct FrameInputs
    {
        ID3D12Resource* Input = nullptr;              // PIXEL_SHADER_RESOURCE on entry and exit
        // Depth, readable from compute (the caller transitions it). The views are created
        // from the resources: the engine's own SRVs sit in a shader-visible heap, which
        // is not a valid descriptor copy source.
        ID3D12Resource* Depth = nullptr;
        DXGI_FORMAT DepthSrvFormat = DXGI_FORMAT_R32_FLOAT;
        DirectX::XMFLOAT4X4 ViewProjection{};         // row-vector, no jitter
        DirectX::XMFLOAT4X4 Projection{};
        std::vector<LightSource> Lights;
        float DeltaSeconds = 0.016f;
    };

    bool Initialize(UINT width, UINT height);
    void Shutdown();

    void Apply(ID3D12GraphicsCommandList* commandList, const LensFlareSettings& settings, const FrameInputs& inputs);

    ID3D12Resource*             GetOutputResource()  const { return mOutputTexture.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetOutputCpuSrv()    const { return mOutputUiSrvCpu; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputGpuSrv()    const { return mOutputUiSrvGpu; }
    UiTextureID                 GetOutputTextureId() const { return mOutputTextureId; }

    bool IsInitialized() const { return mIsInitialized; }

    // Number of lights that flared last frame, and the ghosts drawn per light (for the UI).
    int GetActiveLightCount() const { return mActiveLights; }
    int GetGhostCount() const { return mGhostCount; }
    const char* GetLensError() const { return mLensError.empty() ? nullptr : mLensError.c_str(); }

    const char* GetLastErrorMessage() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    static constexpr UINT kFrameSlots = 3;
    static constexpr UINT kMaxChannels = 6;
    static constexpr UINT kMaxSurfaces = 48;
    static constexpr UINT kMaxLights = 8;
    static constexpr UINT kMaxGhosts = 256;
    static constexpr UINT kStarburstSize = 256;
    // Upper bound on grid entries (24 bytes each): 192 MB.
    static constexpr UINT64 kMaxGridEntries = 8ull * 1024ull * 1024ull;

    // Descriptor layout of one frame slot in mHeap. Must match the registers in
    // LensFlare_Trace.hlsl / LensFlare_Draw.hlsl.
    enum Descriptor : UINT
    {
        SrvInput = 0, SrvDepth, SrvGrid, SrvBounds, SrvVisibility, SrvGhostTarget, SrvStarburst,
        UavGrid, UavBounds, UavVisibility,
        DescriptorsPerSlot
    };

    struct LightCb
    {
        DirectX::XMFLOAT4 DirectionLocal;
        DirectX::XMFLOAT4 Rotation;
        DirectX::XMFLOAT4 Color;
        DirectX::XMFLOAT4 Screen;
        DirectX::XMFLOAT4 Extra;
        DirectX::XMFLOAT4 Pupil;
    };

    // Mirrors LensFlareConstants in LensFlare_Common.hlsli.
    struct LensFlareCb
    {
        DirectX::XMFLOAT4 Lens;
        DirectX::XMFLOAT4 TanHalf;
        DirectX::XMFLOAT4 Aperture;
        UINT              Counts[4];
        UINT              Counts2[4];
        DirectX::XMFLOAT4 Target;
        DirectX::XMFLOAT4 Occlusion;
        DirectX::XMFLOAT4 Starburst;
        DirectX::XMFLOAT4 Channel[kMaxChannels];
        UINT              Ghosts[kMaxGhosts / 2][4];
        LightCb           Lights[kMaxLights];
        DirectX::XMFLOAT4 Surfaces[kMaxChannels * kMaxSurfaces * 2];
    };

    bool CreatePipelines();
    bool CreateTargets(UINT width, UINT height);
    bool EnsureLens(const LensFlareSettings& settings);
    bool EnsureGridBuffer(UINT entries);
    bool EnsureStarburst(ID3D12GraphicsCommandList* commandList, const LensFlareSettings& settings);
    void WriteStaticDescriptors();
    D3D12_CPU_DESCRIPTOR_HANDLE HeapCpu(UINT slot, UINT descriptor) const;
    D3D12_GPU_DESCRIPTOR_HANDLE HeapGpu(UINT slot, UINT descriptor) const;

    // ---- pipelines ---------------------------------------------------------
    DX12Shader mOcclusionShader, mBoundsShader, mTraceShader;
    DX12Shader mGhostVs, mGhostPs, mFullscreenVs, mCompositePs, mStarburstVs, mStarburstPs;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mOcclusionPso, mBoundsPso, mTracePso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mGhostPso, mCompositePso, mStarburstPso;

    // ---- per-frame data (ringed: three frames in flight) -------------------
    Microsoft::WRL::ComPtr<ID3D12Resource> mConstantBuffer;
    std::uint8_t* mMappedCb = nullptr;
    UINT64 mCbStride = 0;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mHeap;
    UINT mDescriptorSize = 0;
    UINT64 mFrameCounter = 0;

    // ---- GPU buffers -------------------------------------------------------
    Microsoft::WRL::ComPtr<ID3D12Resource> mGridBuffer;
    UINT mGridEntries = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> mBoundsBuffer;     // kMaxLights * kMaxGhosts float4
    Microsoft::WRL::ComPtr<ID3D12Resource> mVisibilityBuffer; // kMaxLights floats, persists across frames

    // ---- textures ----------------------------------------------------------
    Microsoft::WRL::ComPtr<ID3D12Resource> mOutputTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> mGhostTarget;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mRtvHeap;    // [0] output, [1] ghost target
    UINT mGhostWidth = 0, mGhostHeight = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> mStarburstTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> mStarburstUpload;  // kept until the next rebuild (which waits for the GPU)
    int   mStarburstBlades = -1;
    float mStarburstRotation = -1.0f;
    float mStarburstRoundness = -1.0f;

    D3D12_CPU_DESCRIPTOR_HANDLE mOutputUiSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputUiSrvGpu{};
    UiTextureID mOutputTextureId = UiTextureID_Invalid;
    bool mUiSlotAllocated = false;

    // ---- lens --------------------------------------------------------------
    // Loading and ranking a lens takes up to a few hundred milliseconds (seconds in a
    // Debug build), so it runs on a worker; until it lands the flare draws starbursts only.
    struct LoadedLens
    {
        bool Ok = false;
        std::string Error;
        LensFlareOptics::Lens Lens;
        std::vector<LensFlareOptics::Ghost> Ghosts;
        float ImageScale = 50.0f;
        float Milliseconds = 0.0f;
    };
    static LoadedLens LoadAndRankLens(const std::string& name);

    std::string mLoadedLensName;
    std::future<LoadedLens> mPendingLens;
    bool mLensValid = false;
    std::string mLensError;
    LensFlareOptics::Lens mLens;
    std::vector<LensFlareOptics::Ghost> mRankedGhosts;
    float mImageScale = 50.0f;
    // Surfaces for the current f-number and wavelength count, cached.
    float mSurfacesFNumber = -1.0f;
    int   mSurfacesChannels = -1;
    std::vector<std::vector<LensFlareOptics::Surface>> mChannelSurfaces;
    float mApertureHeight = 1.0f;
    // The direct path's pupil on the entrance plane at evenly spaced tan(angle) up to
    // mPupilMaxTan, rebuilt with the surfaces. The bounds pass samples it per light.
    static constexpr int kPupilSamples = 16;
    std::array<LensFlareOptics::PupilRegion, kPupilSamples> mPupilTable{};
    float mPupilMaxTan = 1.0f;

    // Stable light -> visibility slot mapping, so the temporal smoothing follows a light.
    std::array<std::uint32_t, kMaxLights> mSlotKeys{};

    int mActiveLights = 0;
    int mGhostCount = 0;

    UINT mWidth = 0;
    UINT mHeight = 0;
    bool mIsInitialized = false;
    std::string mLastError;
};
