#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "DeferredLightingPass.h"
#include "LightClusterRenderer.h"
#include "VolumetricFogSettings.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <cstddef>
#include <cstdint>

class VolumetricFogRenderer
{
public:
    // The fog lights the medium from the same array the deferred pass shades
    // surfaces from, rather than a reduced copy of it. The reduced copy is what
    // let the two disagree: it had no falloff exponent, no source radius, no
    // rect frame and no shadow index, so a light that fell off with distance on
    // the walls stayed flat out to its radius in the air, and none of them cast
    // a shadow through the fog. It also capped out at four lights, chosen by
    // entity order, and silently dropped the fifth.
    using FogPointLight = DeferredLightingPass::PointLightGpu;
    static constexpr uint32_t kMaxPointLights =
        static_cast<uint32_t>(DeferredLightingPass::kMaxPointLights);
    static constexpr uint32_t kMaxShadowFaces =
        static_cast<uint32_t>(DeferredLightingPass::kMaxShadowCastingPointLights
            * DeferredLightingPass::kPointShadowFacesPerLight);

    // Everything the injection pass needs beyond the fog's own settings. Passed
    // as one struct because the argument list had already grown past the point
    // where a caller could see which float went where.
    struct FrameInputs
    {
        const float* ViewProjInv = nullptr;   // 16 floats, row-major pre-transposed
        const float* CurrViewProj = nullptr;  // 16 floats
        const float* CameraPos = nullptr;     // 3 floats

        float NearPlane = 0.1f;
        float FarPlane = 100.0f;

        // Travel direction, from the sun toward the scene - the same convention
        // DeferredLighting.hlsl's gSunDirection uses.
        float SunDir[3]{ 0.0f, 0.0f, -1.0f };
        float SunColor[3]{};
        float SkyColor[3]{};

        const FogPointLight* PointLights = nullptr;
        uint32_t NumPointLights = 0;

        // Indirect light, from whichever GI is running: the radiance probe grid when
        // the probes are the GI, otherwise the screen-space GI buffer (RTGI's or voxel
        // GI's diffuse output, irradiance / pi at the visible surface), read behind
        // each froxel. Leave both null to inject without indirect light. The SRV
        // must be readable from compute.
        D3D12_GPU_DESCRIPTOR_HANDLE ProbeSrv{};
        RadianceProbeFieldGpu ProbeField{};
        D3D12_GPU_DESCRIPTOR_HANDLE ScreenGiSrv{};
        // The G-Buffer normal (xy: oct normal), readable from compute. With it and
        // ScreenGiSrv, the screen-space GI is gathered into a world-space volume
        // the fog samples instead (VolumetricFogSettings::GiVolume).
        D3D12_GPU_DESCRIPTOR_HANDLE GBufferNormalSrv{};
        // Both hold pre-exposed light (see HosekWilkieResult::PreExposure); this is
        // the inverse, folded into the GI intensity.
        float ProbeInvPreExposure = 1.0f;

        // Point shadow cubemap atlas. Leave the SRV null for unshadowed fog.
        D3D12_GPU_DESCRIPTOR_HANDLE PointShadowSrv{};
        const DirectX::XMFLOAT4X4* PointShadowFaceViewProj = nullptr;
        int PointShadowLightCount = 0;
        float PointShadowMapSize = 0.0f;
        float PointShadowBias = 0.0f;

        // Virtual shadow map. When set with LocalEnabled, local lights' shadows come
        // from it instead of the cubemap atlas; the pool must be readable from compute.
        const VsmGpuConstants* Vsm = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS VsmPageTable = 0;
        D3D12_GPU_DESCRIPTOR_HANDLE VsmPoolSrv{};

        // Every scene light and its clustered lists (LightClusterRenderer); the
        // injection reads these, not PointLights, whose 16 slots only fill the
        // constant buffer's layout. Required: without them nothing is dispatched.
        LightClusterBindings LightClusters{};
    };

    VolumetricFogRenderer() = default;
    ~VolumetricFogRenderer() { Shutdown(); }

    bool Initialize(UINT sceneWidth, UINT sceneHeight);
    bool EnsureSize(UINT sceneWidth, UINT sceneHeight);
    bool EnsureSize(UINT sceneWidth, UINT sceneHeight, const VolumetricFogSettings& settings);
    void Shutdown();

    void Dispatch(
        ID3D12GraphicsCommandList* cmdList,
        D3D12_GPU_DESCRIPTOR_HANDLE sceneDepthSrv,
        const VolumetricFogSettings& settings,
        const FrameInputs& inputs);

    D3D12_GPU_DESCRIPTOR_HANDLE GetIntegratedFogSrv() const { return mIntegratedSrvGpu; }
    bool IsInitialized() const { return mIsInitialized; }
    const char* GetLastError() const { return mLastError.empty() ? nullptr : mLastError.c_str(); }

private:
    struct alignas(256) FogConstants
    {
        uint32_t FrameWidth = 0;
        uint32_t FrameHeight = 0;
        uint32_t FroxelWidth = 0;
        uint32_t FroxelHeight = 0;

        uint32_t DepthSlices = 0;
        uint32_t DebugView = 0;
        float NearPlane = 0.1f;
        float FarPlane = 100.0f;

        float StartDistance = 0.1f;
        float MaxDistance = 100.0f;
        float Density = 0.02f;
        float Anisotropy = 0.5f;

        float BaseHeight = 0.0f;
        float HeightFalloff = 0.0f;
        float ScatteringAlbedo = 0.9f;
        float GiIntensity = 1.0f;

        float FogColor[3] = { 1.0f, 1.0f, 1.0f };
        float _Pad0 = 0.0f;

        float EmissiveColor[3]{};
        float EmissiveIntensity = 0.0f;

        float CameraPos[3]{};
        float _Pad1 = 0.0f;

        float SunDir[3]{};
        float _Pad2 = 0.0f;

        float SunColor[3]{};
        float _Pad3 = 0.0f;

        float SkyColor[3]{};
        float _Pad4 = 0.0f;

        uint32_t NumPointLights = 0;
        uint32_t ScreenGiEnabled = 0;   // 1: indirect light from ScreenGiSrv (t5)
        uint32_t _ProbePad[2]{};

        RadianceProbeFieldGpu ProbeField{};

        float PointShadowMapSize = 0.0f;
        float PointShadowBias = 0.0f;
        int32_t PointShadowLightCount = 0;
        float _Pad5 = 0.0f;

        FogPointLight PointLights[kMaxPointLights]{};
        DirectX::XMFLOAT4X4 PointShadowFaceViewProj[kMaxShadowFaces]{};

        float ViewProjInv[16]{};
        float CurrViewProj[16]{};

        VsmGpuConstants Vsm{};

        // World-space GI volume (VolumetricFogGiVolume.hlsl).
        int32_t  GiVolumeOrigin[4]{};      // xyz: world cell at the window's min corner; w: 1 = in use
        int32_t  GiVolumePrevOrigin[4]{};  // xyz: last frame's; w: 1 = last frame's volume is history
        float    GiVolumeCellSize = 0.0f;
        float    GiVolumeInvCellSize = 0.0f;
        uint32_t GiVolumeFrame = 0;
        uint32_t GiVolumeSplatStride = 1;
    };

    // Pinned against the offsets FogConstants has in VolumetricFogCommon.hlsli.
    // A field inserted on one side and not the other shifts everything after it
    // and shows up as fog lit by whatever happened to land where a light's
    // colour used to be, which reads as a shader bug rather than a layout one.
    static_assert(offsetof(FogConstants, PointLights) == 272,
        "FogConstants header must stay 272 bytes; see cbuffer FogConstants in VolumetricFogCommon.hlsli.");
    static_assert(offsetof(FogConstants, PointShadowFaceViewProj) == 1808,
        "FogConstants light array must stay at offset 272, 16 x 96 bytes.");
    static_assert(offsetof(FogConstants, ViewProjInv) == 3344,
        "FogConstants shadow matrix array must stay at offset 1808, 24 x 64 bytes.");
    static_assert(offsetof(FogConstants, CurrViewProj) == 3408,
        "FogConstants matrices must stay packed at the end of the buffer.");
    static_assert(offsetof(FogConstants, Vsm) == 3472,
        "FogConstants virtual shadow map block must follow the matrices.");
    static_assert(offsetof(FogConstants, GiVolumeOrigin) == offsetof(FogConstants, Vsm) + sizeof(VsmGpuConstants)
                  && offsetof(FogConstants, GiVolumeOrigin) % 16 == 0,
        "FogConstants GI volume block must follow the virtual shadow map block on a 16-byte register.");

    bool CreateRootSignatures();
    bool CreatePipelines();
    bool CreateConstantBuffer();
    bool CreateResolutionResources(const VolumetricFogSettings& settings);
    bool CreateDescriptors();
    bool CreateNullDescriptors();
    void UploadConstants(const VolumetricFogSettings& settings, const FrameInputs& inputs);
    // Lazily creates the GI volume's textures, accumulation buffer and views.
    bool EnsureGiVolume();
    // Splats this frame's screen-space GI into the volume and resolves it. Uses the
    // constants already uploaded for this frame.
    void UpdateGiVolume(ID3D12GraphicsCommandList* cmdList, D3D12_GPU_VIRTUAL_ADDRESS constantsAddress,
        D3D12_GPU_DESCRIPTOR_HANDLE sceneDepthSrv, const FrameInputs& inputs);

    bool mIsInitialized = false;
    UINT mSceneWidth = 0;
    UINT mSceneHeight = 0;
    UINT mFroxelWidth = 0;
    UINT mFroxelHeight = 0;
    UINT mDepthSlices = 0;
    std::string mLastError;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mInjectRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mAccumulateRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mInjectPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mAccumulatePipelineState;

    // The CPU runs up to three frames ahead of the GPU (DX12Context's FrameCount,
    // whose per-frame wait only blocks on the fence from three frames back), so a
    // single mapped copy of this buffer is rewritten while the injection dispatch
    // of the two previous frames is still reading it. FogConstants is ~3.4 KB, and
    // the memcpy that fills it is not atomic, so what the shader sees is a blend:
    // this frame's camera matrices against the previous frame's light array, or a
    // half-written light record. It is invisible with a still camera and static
    // lights, because consecutive frames then write identical bytes, and it is at
    // its worst with a moving camera and a styled light whose colour changes every
    // frame - which is exactly when the fog looked unstable.
    //
    // One copy per in-flight frame, selected by mFrameSlot, so no frame writes over
    // bytes another frame can still be reading. Same fix as EntityMeshRenderer's
    // constant-buffer slots.
    static constexpr UINT kFramesInFlight = 3;

    Microsoft::WRL::ComPtr<ID3D12Resource> mConstantBuffer;
    FogConstants* mMappedConstants = nullptr;
    UINT mFrameSlot = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> mLightingVolume;
    Microsoft::WRL::ComPtr<ID3D12Resource> mIntegratedFog;

    D3D12_CPU_DESCRIPTOR_HANDLE mLightingSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mLightingSrvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE mLightingUavCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mLightingUavGpu{};

    D3D12_CPU_DESCRIPTOR_HANDLE mIntegratedSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mIntegratedSrvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE mIntegratedUavCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mIntegratedUavGpu{};

    // Stand-ins bound when a scene has no probe grid or no shadow-casting
    // lights. A descriptor table left unbound is undefined behaviour even for a
    // shader that never reads it, and binding the depth SRV in its place would
    // hand the shader a Texture2D where it declared a StructuredBuffer.
    D3D12_CPU_DESCRIPTOR_HANDLE mNullProbeSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mNullProbeSrvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE mNullShadowSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mNullShadowSrvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE mNullVolumeSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mNullVolumeSrvGpu{};

    // ---- World-space GI volume (VolumetricFogGiVolume.hlsl) ----------------
    // Two volumes ping-pong: last frame's is the history this frame's is resolved
    // from. Both rest in NON_PIXEL_SHADER_RESOURCE; the splat buffer rests in
    // UNORDERED_ACCESS and is left zeroed by the resolve.
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mGiVolumeRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mGiSplatPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mGiResolvePipelineState;
    Microsoft::WRL::ComPtr<ID3D12Resource> mGiVolume[2];
    Microsoft::WRL::ComPtr<ID3D12Resource> mGiAccum;
    D3D12_CPU_DESCRIPTOR_HANDLE mGiVolumeSrvCpu[2]{};
    D3D12_GPU_DESCRIPTOR_HANDLE mGiVolumeSrvGpu[2]{};
    D3D12_CPU_DESCRIPTOR_HANDLE mGiVolumeUavCpu[2]{};
    D3D12_GPU_DESCRIPTOR_HANDLE mGiVolumeUavGpu[2]{};
    bool     mGiVolumeFailed = false;
    // This frame's state, decided in Dispatch before the constants go up.
    bool     mGiVolumeActive = false;     // the volume is updated and sampled this frame
    UINT     mGiVolumeWrite = 0;          // the volume this frame writes
    bool     mGiVolumeHistoryValid = false;
    int32_t  mGiVolumeOrigin[3]{};
    int32_t  mGiVolumePrevOrigin[3]{};
    float    mGiVolumeCellSize = 0.0f;
    uint32_t mGiVolumeFrame = 0;

    bool mDescriptorsAllocated = false;
    bool mNullDescriptorsCreated = false;
};
