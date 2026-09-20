#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "DeferredLightingPass.h"
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

        // Radiance probe grid. Leave the SRV null to inject without indirect
        // light; the shader then skips the grid entirely.
        D3D12_GPU_DESCRIPTOR_HANDLE ProbeSrv{};
        uint32_t ProbeGridX = 0;
        uint32_t ProbeGridY = 0;
        uint32_t ProbeGridZ = 0;
        float ProbeSpacing = 0.0f;
        float ProbeOrigin[3]{};

        // Point shadow cubemap atlas. Leave the SRV null for unshadowed fog.
        D3D12_GPU_DESCRIPTOR_HANDLE PointShadowSrv{};
        const DirectX::XMFLOAT4X4* PointShadowFaceViewProj = nullptr;
        int PointShadowLightCount = 0;
        float PointShadowMapSize = 0.0f;
        float PointShadowBias = 0.0f;
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
        uint32_t ProbeGridX = 0;
        uint32_t ProbeGridY = 0;
        uint32_t ProbeGridZ = 0;

        float ProbeOrigin[3]{};
        float ProbeSpacing = 0.0f;

        float PointShadowMapSize = 0.0f;
        float PointShadowBias = 0.0f;
        int32_t PointShadowLightCount = 0;
        float _Pad5 = 0.0f;

        FogPointLight PointLights[kMaxPointLights]{};
        DirectX::XMFLOAT4X4 PointShadowFaceViewProj[kMaxShadowFaces]{};

        float ViewProjInv[16]{};
        float CurrViewProj[16]{};
    };

    // Pinned against the offsets FogConstants has in VolumetricFogCommon.hlsli.
    // A field inserted on one side and not the other shifts everything after it
    // and shows up as fog lit by whatever happened to land where a light's
    // colour used to be, which reads as a shader bug rather than a layout one.
    static_assert(offsetof(FogConstants, PointLights) == 208,
        "FogConstants header must stay 208 bytes; see cbuffer FogConstants in VolumetricFogCommon.hlsli.");
    static_assert(offsetof(FogConstants, PointShadowFaceViewProj) == 1744,
        "FogConstants light array must stay at offset 208, 16 x 96 bytes.");
    static_assert(offsetof(FogConstants, ViewProjInv) == 3280,
        "FogConstants shadow matrix array must stay at offset 1744, 24 x 64 bytes.");
    static_assert(offsetof(FogConstants, CurrViewProj) == 3344,
        "FogConstants matrices must stay packed at the end of the buffer.");

    bool CreateRootSignatures();
    bool CreatePipelines();
    bool CreateConstantBuffer();
    bool CreateResolutionResources(const VolumetricFogSettings& settings);
    bool CreateDescriptors();
    bool CreateNullDescriptors();
    void UploadConstants(const VolumetricFogSettings& settings, const FrameInputs& inputs);

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

    bool mDescriptorsAllocated = false;
    bool mNullDescriptorsCreated = false;
};
