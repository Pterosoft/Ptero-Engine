#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "VolumetricCloudSettings.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

// VolumetricCloudRenderer
// Raymarched volumetric cloud layer.
//
// Frame flow:
//   1. BakeNoise (first frame only, plus whenever the weather seed changes)
//      generates the Perlin-Worley base shape volume, the Worley detail volume,
//      the curl field and the weather map entirely on the GPU.
//   2. Dispatch() marches the layer at 1/ResolutionDivisor resolution against
//      the scene depth buffer, then reconstructs a full resolution result by
//      blending with the reprojected previous frame.
//   3. Composite() blends that result over the lit scene colour target with
//      (ONE, SRC_ALPHA), which evaluates scene * transmittance + luminance.
//
// Dispatch() must run while the scene depth buffer is readable by compute
// (ALL_SHADER_RESOURCE); Composite() must run while the scene colour target is
// bound as a render target.
class VolumetricCloudRenderer
{
public:
    VolumetricCloudRenderer() = default;
    ~VolumetricCloudRenderer() { Shutdown(); }

    VolumetricCloudRenderer(const VolumetricCloudRenderer&) = delete;
    VolumetricCloudRenderer& operator=(const VolumetricCloudRenderer&) = delete;

    bool Initialize(UINT sceneWidth, UINT sceneHeight, DXGI_FORMAT sceneColorFormat);
    bool EnsureSize(UINT sceneWidth, UINT sceneHeight, const VolumetricCloudSettings& settings);
    void Shutdown();

    void Dispatch(
        ID3D12GraphicsCommandList* commandList,
        D3D12_GPU_DESCRIPTOR_HANDLE sceneDepthSrv,
        const VolumetricCloudSettings& settings,
        const float viewProjInv[16],
        const float prevViewProj[16],
        const float cameraPos[3],
        const float sunDirection[3],
        const float sunColor[3],
        const float skyColor[3],
        float deltaTimeSeconds,
        bool resetHistory);

    void Composite(
        ID3D12GraphicsCommandList* commandList,
        D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv,
        UINT viewportWidth,
        UINT viewportHeight);

    // Full resolution resolved cloud buffer: rgb = in-scattered luminance,
    // a = transmittance.  Valid after Dispatch(); empty before the first frame.
    D3D12_GPU_DESCRIPTOR_HANDLE GetResolvedCloudSrv() const;

    bool IsInitialized() const { return mIsInitialized; }
    bool DidDispatchThisFrame() const { return mDispatchedThisFrame; }
    void ResetHistory() { mHistoryValid = false; }

    const char* GetLastError() const { return mLastError.empty() ? nullptr : mLastError.c_str(); }

private:
    static constexpr UINT kBaseShapeSize = 128;
    static constexpr UINT kDetailSize = 32;
    static constexpr UINT kCurlSize = 128;
    static constexpr UINT kWeatherSize = 512;
    static constexpr UINT kHistoryBufferCount = 2;

    // Must stay byte-for-byte identical to the cbuffer in
    // Data/Shaders/VolumetricCloudConstants.hlsli.
    struct alignas(256) CloudConstants
    {
        uint32_t TraceResolution[2]{};
        uint32_t FullResolution[2]{};

        float InvTraceResolution[2]{};
        float InvFullResolution[2]{};

        uint32_t FrameIndex = 0;
        uint32_t ResolutionDivisor = 2;
        uint32_t TemporalEnabled = 1;
        uint32_t DebugView = 0;

        float CameraPos[3]{};
        float TimeSeconds = 0.0f;

        float SunDirection[3]{ 0.0f, 0.0f, -1.0f };
        float SunIntensityScale = 1.0f;

        float SunColor[3]{ 1.0f, 1.0f, 1.0f };
        float AmbientIntensityScale = 1.0f;

        float SkyColor[3]{ 0.3f, 0.5f, 0.8f };
        float GroundBounceScale = 0.3f;

        float ScatteringAlbedo[3]{ 1.0f, 1.0f, 1.0f };
        float ExtinctionScale = 0.05f;

        float ViewProjInv[16]{};
        float PrevViewProj[16]{};

        float PlanetRadius = 6360000.0f;
        float LayerBottomRadius = 6361200.0f;
        float LayerTopRadius = 6365200.0f;
        float LayerThickness = 4000.0f;

        float Coverage = 0.55f;
        float CloudType = 0.5f;
        float DensityScale = 0.6f;
        float BaseNoiseFrequency = 1.0f / 24000.0f;

        float DetailNoiseFrequency = 1.0f / 1600.0f;
        float DetailStrength = 0.35f;
        float CurlStrength = 0.6f;
        float AnvilBias = 0.25f;

        float WeatherFrequency = 1.0f / 90000.0f;
        float WeatherOffsetX = 0.0f;
        float WeatherOffsetY = 0.0f;
        float CloudTopOffset = 350.0f;

        float WindOffset[3]{};
        float WindSkew = 0.35f;

        float DetailWindOffset[3]{};
        float WindDirectionX = 0.7071f;

        float WindDirectionY = 0.7071f;
        float PhaseG0 = 0.8f;
        float PhaseG1 = -0.25f;
        float PhaseBlend = 0.28f;

        float PowderStrength = 0.35f;
        uint32_t MsOctaves = 3;
        float MsScatterFalloff = 0.55f;
        float MsExtinctionFalloff = 0.55f;

        float MsPhaseFalloff = 0.5f;
        uint32_t MaxSteps = 96;
        uint32_t LightSteps = 6;
        float LightMarchDistance = 1600.0f;

        float MaxTraceDistance = 120000.0f;
        float DistanceFadeStart = 80000.0f;
        float DetailFadeDistance = 25000.0f;
        float TemporalBlend = 0.92f;

        float ShadowStepGrowth = 1.35f;
        float HistoryValid = 0.0f;
        float ConeSpread = 0.12f;
        float _Pad0 = 0.0f;
    };

    // Must stay byte-for-byte identical to the cbuffer in
    // Data/Shaders/VolumetricCloudNoise.hlsl.
    struct alignas(256) NoiseConstants
    {
        uint32_t TargetSize = 0;
        uint32_t Seed = 0;
        float WeatherCellSize = 1.0f;
        float WeatherCoverageBias = 0.0f;

        float WeatherTypeBias = 0.0f;
        float _Pad0[3]{};
    };

    enum NoiseBakeSlot : uint32_t
    {
        NoiseBakeSlot_BaseShape = 0,
        NoiseBakeSlot_Detail,
        NoiseBakeSlot_Curl,
        NoiseBakeSlot_Weather,
        NoiseBakeSlot_Count
    };

    bool CreateRootSignatures();
    bool CreatePipelines(DXGI_FORMAT sceneColorFormat);
    bool CreateConstantBuffers();
    bool CreateNoiseResources();
    bool CreateResolutionResources(const VolumetricCloudSettings& settings);
    bool CreateNoiseDescriptors();
    bool CreateResolutionDescriptors();
    void ReleaseResolutionResources();

    void BakeNoise(ID3D12GraphicsCommandList* commandList, const VolumetricCloudSettings& settings);
    void UploadConstants(
        const VolumetricCloudSettings& settings,
        const float viewProjInv[16],
        const float prevViewProj[16],
        const float cameraPos[3],
        const float sunDirection[3],
        const float sunColor[3],
        const float skyColor[3]);

    static UINT ComputeTraceExtent(UINT sceneExtent, int resolutionDivisor);

    bool mIsInitialized = false;
    bool mNoiseBaked = false;
    bool mHistoryValid = false;
    bool mDispatchedThisFrame = false;
    std::string mLastError;

    UINT mSceneWidth = 0;
    UINT mSceneHeight = 0;
    UINT mTraceWidth = 0;
    UINT mTraceHeight = 0;
    UINT mResolutionDivisor = 2;
    uint32_t mFrameIndex = 0;
    uint32_t mHistoryReadIndex = 0;
    uint32_t mHistoryWriteIndex = 1;

    int mBakedWeatherSeed = INT32_MIN;
    float mBakedWeatherCellSize = -1.0f;
    float mBakedWeatherCoverageBias = -1.0f;
    float mBakedWeatherTypeBias = -1.0f;

    float mWindOffset[3]{};
    float mDetailWindOffset[3]{};
    float mElapsedSeconds = 0.0f;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mBakeRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRaymarchRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mReconstructRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mCompositeRootSignature;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> mBaseShapePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mDetailPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mCurlPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mWeatherPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mRaymarchPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mReconstructPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mCompositePipeline;

    Microsoft::WRL::ComPtr<ID3D12Resource> mConstantBuffer;
    CloudConstants* mMappedConstants = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mNoiseConstantBuffer;
    uint8_t* mMappedNoiseConstants = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> mBaseShapeNoise;
    Microsoft::WRL::ComPtr<ID3D12Resource> mDetailNoise;
    Microsoft::WRL::ComPtr<ID3D12Resource> mCurlNoise;
    Microsoft::WRL::ComPtr<ID3D12Resource> mWeatherMap;
    D3D12_RESOURCE_STATES mNoiseState = D3D12_RESOURCE_STATE_COMMON;

    Microsoft::WRL::ComPtr<ID3D12Resource> mTraceColor;
    Microsoft::WRL::ComPtr<ID3D12Resource> mTraceDepth;
    Microsoft::WRL::ComPtr<ID3D12Resource> mHistoryColor[kHistoryBufferCount];
    Microsoft::WRL::ComPtr<ID3D12Resource> mHistoryDepth[kHistoryBufferCount];
    D3D12_RESOURCE_STATES mTraceState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES mHistoryState[kHistoryBufferCount]{};

    // Noise bake targets, one single-descriptor table each.
    D3D12_CPU_DESCRIPTOR_HANDLE mBakeUavCpu[NoiseBakeSlot_Count]{};
    D3D12_GPU_DESCRIPTOR_HANDLE mBakeUavGpu[NoiseBakeSlot_Count]{};

    // Contiguous t0..t3 for the raymarch pass.
    D3D12_CPU_DESCRIPTOR_HANDLE mNoiseSrvCpu[4]{};
    D3D12_GPU_DESCRIPTOR_HANDLE mNoiseSrvTableGpu{};

    // Contiguous u0..u1 written by the raymarch pass.
    D3D12_CPU_DESCRIPTOR_HANDLE mTraceUavCpu[2]{};
    D3D12_GPU_DESCRIPTOR_HANDLE mTraceUavTableGpu{};

    // Per history buffer: contiguous t4..t7 (trace colour, trace depth, history
    // colour, history depth), contiguous u0..u1, and contiguous t8..t9 for the
    // composite pass.
    D3D12_CPU_DESCRIPTOR_HANDLE mReconstructSrvCpu[kHistoryBufferCount][4]{};
    D3D12_GPU_DESCRIPTOR_HANDLE mReconstructSrvTableGpu[kHistoryBufferCount]{};
    D3D12_CPU_DESCRIPTOR_HANDLE mReconstructUavCpu[kHistoryBufferCount][2]{};
    D3D12_GPU_DESCRIPTOR_HANDLE mReconstructUavTableGpu[kHistoryBufferCount]{};
    D3D12_CPU_DESCRIPTOR_HANDLE mCompositeSrvCpu[kHistoryBufferCount][2]{};
    D3D12_GPU_DESCRIPTOR_HANDLE mCompositeSrvTableGpu[kHistoryBufferCount]{};

    bool mNoiseDescriptorsAllocated = false;
    bool mResolutionDescriptorsAllocated = false;
};
