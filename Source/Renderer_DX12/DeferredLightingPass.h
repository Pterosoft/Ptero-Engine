#pragma once

// DeferredLightingPass
// Owns the three G-Buffer render targets (albedo, normal, material) and the
// fullscreen lighting-resolve pipeline.  The flow each frame is:
//
//   1. BeginGeometryPass()  – transition G-Buffer RTs to RenderTarget state,
//                             bind them as MRT output together with the scene
//                             depth buffer, and clear them.
//   2. <geometry draw calls via EntityMeshRenderer>
//   3. EndGeometryPass()    – transition G-Buffer RTs to PixelShaderResource.
//   4. ResolveLight()       – run the fullscreen deferred-lighting quad.
//                             Writes lit colour on top of the scene colour
//                             target (sky was already rendered into it).

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "RadianceProbeSettings.h"
#include "MsaaSettings.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <vector>

struct VolumetricFogSettings;

// Forward declare to avoid circular includes.
struct Entity;

// The G-Buffer SRV handles are exposed so EntityMeshRenderer's geometry pass
// can verify it is writing the correct targets and for debug visualisation later.
struct GBufferSrvs
{
    D3D12_GPU_DESCRIPTOR_HANDLE Albedo{};
    D3D12_GPU_DESCRIPTOR_HANDLE Normal{};
    D3D12_GPU_DESCRIPTOR_HANDLE Material{};
};

class DeferredLightingPass
{
public:
    // --- GPU-facing point light record; mirrors PointLightGpu in EntityMeshRenderer. ---
    struct PointLightGpu
    {
        DirectX::XMFLOAT3 Position;
        float             Radius;
        DirectX::XMFLOAT3 Color;
        float             InvRadiusSq;
        float             FalloffExponent;
        float             SourceRadius;
        float             CastShadows;
        // Index into the point shadow atlas, or -1. Named _Pad0 from when it
        // was unused; the scene renderer writes it after the shadow pass picks
        // which lights get a cubemap.
        float             _Pad0;

        // --- Spot and rect ---------------------------------------------------
        // Emission axis in world space, from the entity's local -Z. Unused by
        // LightType::Point.
        DirectX::XMFLOAT3 Direction;
        float             LightType;      // matches ::LightType in Components.h

        // Cosines of the half-angles, so the shader compares against a dot
        // product directly instead of taking an acos per pixel per light.
        float             SpotCosInner;
        float             SpotCosOuter;
        float             RectHalfWidth;
        float             RectHalfHeight;

        // The rectangle's local X axis in world space. Its local Y is
        // cross(Direction, Right), so the frame costs one vector rather than two.
        DirectX::XMFLOAT3 RectRight;
        float             RectTwoSided;
    };
    static_assert(sizeof(PointLightGpu) == 96,
        "PointLightGpu must match PointLightData in DeferredLighting.hlsl and "
        "RtgiPointLightData in RtGI_Common.hlsli.");

    // Maximum point lights the lighting shader supports.
    // Must match MAX_POINT_LIGHTS in DeferredLighting.hlsl.
    static constexpr int kMaxPointLights = 16;
    static constexpr int kMaxShadowCastingPointLights = 4;
    static constexpr int kPointShadowFacesPerLight = 6;

    // Called once after the DX12 device is ready.
    // sceneDepthTarget – the same D32_FLOAT resource used by the geometry pass.
    // width/height     – initial scene resolution; re-created automatically when
    //                    EnsureSize() is called with different dimensions.
    // msaaSettings     – MSAA configuration (sample count, quality)
    bool Initialize(
        UINT               width,
        UINT               height,
        DXGI_FORMAT        depthFormat,
        const MsaaSettings& msaaSettings = MsaaSettings{});

    // Resize G-Buffer RTs if the resolution has changed.
    // If MSAA settings change, resources are recreated.
    bool EnsureSize(UINT width, UINT height, const MsaaSettings& msaaSettings = MsaaSettings{});

    // Resolve MSAA G-Buffer to single-sample textures for lighting.
    // Only performs work if MSAA is enabled. Call after EndGeometryPass().
    // Returns the GPU time in milliseconds (requires query support).
    void ResolveGBuffer(ID3D12GraphicsCommandList* commandList);

    // Get the last measured resolve time in milliseconds (0.0f if not measured).
    float GetLastResolveTimeMs() const { return mLastResolveTimeMs; }

    // Transition G-Buffer RTs to render-target state and bind them as MRT.
    // The existing scene depth buffer is also bound as the shared DSV so the
    // geometry pass writes into the correct depth buffer.
    // Call this before drawing all scene geometry.
    void BeginGeometryPass(
        ID3D12GraphicsCommandList* commandList,
        D3D12_CPU_DESCRIPTOR_HANDLE sceneDsvHandle,
        UINT                        width,
        UINT                        height) const;

    // Transition G-Buffer RTs back to shader-resource state.
    // Call this after all geometry draw calls are finished.
    void EndGeometryPass(ID3D12GraphicsCommandList* commandList) const;

    // Upload per-frame lighting parameters.
    void SetSceneLighting(
        const DirectX::XMFLOAT3& sunDirection,
        const DirectX::XMFLOAT3& sunColor,
        const DirectX::XMFLOAT3& skyAmbient);

    // Upload point light data each frame.
    void SetPointLights(const PointLightGpu* lights, int count);

    // Upload camera/inverse-VP data for world-position reconstruction.
    void SetCameraData(
        const DirectX::XMFLOAT4X4& invViewProj,
        const DirectX::XMFLOAT3&   cameraPosition);

    // Upload shadow map parameters.
    void SetShadowData(
        const DirectX::XMFLOAT4X4& lightViewProjection,
        float                       shadowMapSize,
        D3D12_GPU_DESCRIPTOR_HANDLE shadowSrvHandle);

    // Supply the DXR GI accumulation texture SRV and an intensity multiplier.
    // Call with handle={} and intensity=0.0f to disable GI contribution.
    void SetGiSrv(D3D12_GPU_DESCRIPTOR_HANDLE giSrvHandle, float intensity);

    // Supply the radiance probe SH buffer SRV and matching grid parameters.
    // Call with handle={} to disable probe-driven GI and clear the probe constants.
    void SetProbeSrv(
        D3D12_GPU_DESCRIPTOR_HANDLE   probeSrvHandle,
        const RadianceProbeSettings*  settings,
        const DirectX::XMFLOAT3&      cameraPosition);

    // Supply the ray-traced specular reflections SRV and intensity multiplier.
    // Call with handle={} and intensity=0.0f to disable specular contribution.
    void SetSpecularSrv(D3D12_GPU_DESCRIPTOR_HANDLE specularSrvHandle, float intensity);

    // Forward the RTGI debug mode so deferred lighting can show specular-only output.
    void SetRtgiDebugView(int debugView);

    // Supply the RTAO output texture SRV and an intensity multiplier.
    // Call with handle={} and intensity=0.0f to disable RTAO contribution.
    void SetAoSrv(D3D12_GPU_DESCRIPTOR_HANDLE aoSrvHandle, float intensity, int debugView = 0);

    // Supply the accumulated volumetric fog 3D texture SRV.
    void SetVolumetricFogSrv(D3D12_GPU_DESCRIPTOR_HANDLE fogSrvHandle, const VolumetricFogSettings* settings);
    void SetPointShadowSrv(D3D12_GPU_DESCRIPTOR_HANDLE pointShadowSrvHandle, int activeShadowLightCount, float shadowMapSize, float shadowBias);
    void SetPointShadowMatrices(const DirectX::XMFLOAT4X4* faceViewProjections, int activeShadowLightCount);
    void SetPointShadowDebug(int debugView, int filterRadius, float seamBlendDistance, float normalOffset);

    // Subsurface scattering for the next ResolveLight(). With a constants address, the
    // resolve switches to the pipeline variant that also writes each subsurface pixel's
    // diffuse lighting to diffuseRtv (see SubsurfaceScattering.h). Pass 0 / {} to use the
    // plain pipeline.
    void SetSubsurface(D3D12_GPU_VIRTUAL_ADDRESS subsurfaceConstants, D3D12_CPU_DESCRIPTOR_HANDLE diffuseRtv)
    {
        mSubsurfaceConstants = subsurfaceConstants;
        mSubsurfaceDiffuseRtv = diffuseRtv;
    }

    // CPU-side copies of the last values handed to SetSceneLighting / SetPointLights, for
    // passes that need the same lights (the ray-traced subsurface transmission). Kept apart
    // from the mapped constant buffer, which is write-combined and must not be read.
    const DirectX::XMFLOAT3& GetSunDirection() const { return mCpuSunDirection; }
    const DirectX::XMFLOAT3& GetSunColor() const { return mCpuSunColor; }
    const DirectX::XMFLOAT3& GetSkyAmbient() const { return mCpuSkyAmbient; }
    const PointLightGpu* GetPointLights() const { return mCpuPointLights; }
    int GetPointLightCount() const { return mCpuPointLightCount; }

    // CPU copies of the shadow data the lighting pass samples, for passes that must shadow
    // exactly as it does (the ray-traced subsurface pass). Handles are null when disabled.
    struct ShadowSnapshot
    {
        DirectX::XMFLOAT4X4         LightViewProj{};
        float                       ShadowMapSize = 0.0f;
        float                       ShadowBias = 0.0f;
        D3D12_GPU_DESCRIPTOR_HANDLE SunShadowSrv{};
        float                       PointShadowMapSize = 0.0f;
        float                       PointShadowBias = 0.0f;
        int                         PointShadowLightCount = 0;
        D3D12_GPU_DESCRIPTOR_HANDLE PointShadowSrv{};
        DirectX::XMFLOAT4X4         PointFaceViewProj[kMaxShadowCastingPointLights * kPointShadowFacesPerLight]{};
    };
    const ShadowSnapshot& GetShadowSnapshot() const { return mCpuShadows; }

    // Run the fullscreen deferred-lighting quad.
    // The scene colour RT must already be in RenderTarget state.
    // The G-Buffer must be in PixelShaderResource state (call EndGeometryPass first).
    // sceneDepthSrvHandle – SRV for the scene depth texture for world-pos reconstruction.
    void ResolveLight(
        ID3D12GraphicsCommandList*  commandList,
        D3D12_CPU_DESCRIPTOR_HANDLE sceneRtvHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE sceneDepthSrvHandle,
        DXGI_FORMAT                 sceneColorFormat,
        UINT                        width,
        UINT                        height) const;

    // Expose G-Buffer RTV handles so BeginGeometryPass can also be called
    // by external code if needed.
    D3D12_CPU_DESCRIPTOR_HANDLE GetAlbedoRtv()   const { return mRtvHandles[0]; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetNormalRtv()   const { return mRtvHandles[1]; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetMaterialRtv() const { return mRtvHandles[2]; }

    // G-Buffer SRV GPU handles for binding in the lighting shader.
    const GBufferSrvs& GetSrvs() const { return mSrvs; }
    // Same targets with alpha forced to 1, for the editor's texture viewer only.
    const GBufferSrvs& GetDebugSrvs() const { return mDebugSrvs; }

    // Raw G-Buffer resource pointers for resource-barrier transitions (e.g. before DXR).
    // Index: 0 = albedo, 1 = normal, 2 = material.
    ID3D12Resource* GetGBufferResource(UINT index) const
    {
        return (index < 3u) ? mGBufferResources[index].Get() : nullptr;
    }

    // Get MSAA G-Buffer resource (source for resolve). Returns nullptr if MSAA disabled.
    ID3D12Resource* GetMsaaGBufferResource(UINT index) const
    {
        return (index < 3u) ? mMsaaGBufferResources[index].Get() : nullptr;
    }

    bool IsMsaaEnabled() const { return mMsaaSettings.Enabled; }
    const MsaaSettings& GetMsaaSettings() const { return mMsaaSettings; }
    // Get the current MSAA sample count for geometry renderers to use in their PSOs.
    UINT GetMsaaSampleCount() const { return mMsaaSettings.GetEffectiveSampleCount(); }
    UINT GetMsaaQuality() const { return mMsaaSettings.GetEffectiveQuality(); }


    bool IsInitialized() const { return mIsInitialized; }
    void Shutdown();

    const char* GetLastError() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    // Constant buffer layouts (must match DeferredLighting.hlsl).
    struct alignas(256) CameraConstants
    {
        DirectX::XMFLOAT4X4 InvViewProj;
        DirectX::XMFLOAT3   CameraPos;
        float               Pad = 0;
        std::byte           Padding[128]{};
    };
    static_assert(sizeof(CameraConstants) == 256);

    struct LightingConstants
    {
        DirectX::XMFLOAT3 SunDirection; float Pad0 = 0;
        DirectX::XMFLOAT3 SunColor;     float Pad1 = 0;
        DirectX::XMFLOAT3 SkyAmbient;   float Pad2 = 0;
        int               NumPointLights    = 0;
        float             GiIntensity       = 0.0f; // 0 = GI disabled / not available
        float             AoIntensity       = 0.0f; // 0 = RTAO disabled
        int               AoDebugView       = 0;    // 0 = normal lit output, >0 = standalone AO debug
        int               VolumetricFogEnabled = 0;
        float             FogStartDistance = 0.1f;
        float             FogMaxDistance   = 100.0f;
        int               FogDebugView     = 0;
        float             SpecularIntensity = 0.0f; // 0 = specular reflections disabled
        float             Pad3[3]{};
        PointLightGpu     PointLights[kMaxPointLights]{};
        int               RtgiDebugView    = 0;
        int               PointShadowDebugView = 0;
        int               PointShadowFilterRadius = 1;
        float             PointShadowSeamBlendDistance = 0.05f;
        float             PointShadowNormalOffset = 1.0f;
    };
    // LightingConstants is ~576 bytes; CB is allocated rounded up to 256 alignment.

    struct alignas(256) ShadowConstants
    {
        DirectX::XMFLOAT4X4 LightViewProj;
        float               ShadowMapSize;
        float               ShadowBias;
        float               PointShadowMapSize;
        float               PointShadowBias;
        DirectX::XMFLOAT4X4 PointShadowFaceViewProj[kMaxShadowCastingPointLights * kPointShadowFacesPerLight]{};
    };

    struct alignas(256) ProbeConstants
    {
        uint32_t ProbeGridX = 0;
        uint32_t ProbeGridY = 0;
        uint32_t ProbeGridZ = 0;
        float    ProbeSpacing = 1.0f;
        float    ProbeOriginX = 0.0f;
        float    ProbeOriginY = 0.0f;
        float    ProbeOriginZ = 0.0f;
        float    Pad0 = 0.0f;
        std::byte Padding[224]{};
    };
    static_assert(sizeof(ProbeConstants) == 256);

    bool CreateGBufferResources(UINT width, UINT height);
    bool CreateLightingPipeline(DXGI_FORMAT sceneColorFormat);
    bool CreateConstantBuffers();

    // MSAA G-Buffer resources (source for resolve, only created if MSAA enabled).
    Microsoft::WRL::ComPtr<ID3D12Resource> mMsaaGBufferResources[3];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mMsaaRtvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE            mMsaaRtvHandles[3]{};

    // Single-sample G-Buffer resources (albedo, normal, material).
    // These are always created and used for lighting (resolve targets if MSAA enabled).
    Microsoft::WRL::ComPtr<ID3D12Resource> mGBufferResources[3];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mRtvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE            mRtvHandles[3]{};
    GBufferSrvs                            mSrvs{};

    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> mRetiredGBufferResources;
    std::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> mRetiredGBufferDescriptorHeaps;

    // CPU handles matching the SRV allocations (kept for re-creating SRVs on resize).
    D3D12_CPU_DESCRIPTOR_HANDLE mSrvCpuHandles[3]{};
    GBufferSrvs                 mDebugSrvs{};
    D3D12_CPU_DESCRIPTOR_HANDLE mDebugSrvCpuHandles[3]{};

    // Current MSAA configuration.
    MsaaSettings mMsaaSettings{};

    // GPU timing for MSAA resolve (in milliseconds).
    float mLastResolveTimeMs = 0.0f;

    // Lighting pipeline.
    DX12Shader                                   mVertexShader;
    DX12Shader                                   mPixelShader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  mPipelineState;
    // Same resolve compiled with PTERO_SSS_OUTPUT=1: a second render target receives the
    // diffuse lighting of subsurface pixels. Null if it failed to build, in which case
    // subsurface scattering is simply skipped.
    DX12Shader                                   mPixelShaderSubsurface;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  mPipelineStateSubsurface;

    D3D12_GPU_VIRTUAL_ADDRESS   mSubsurfaceConstants = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE mSubsurfaceDiffuseRtv{};

    DirectX::XMFLOAT3 mCpuSunDirection{ 0.0f, 0.0f, -1.0f };
    DirectX::XMFLOAT3 mCpuSunColor{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 mCpuSkyAmbient{ 0.0f, 0.0f, 0.0f };
    PointLightGpu     mCpuPointLights[kMaxPointLights]{};
    int               mCpuPointLightCount = 0;
    ShadowSnapshot    mCpuShadows{};

    // Constant buffers.
    Microsoft::WRL::ComPtr<ID3D12Resource> mCameraCB;
    CameraConstants*                        mMappedCameraCB  = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> mLightingCB;
    LightingConstants*                      mMappedLightingCB = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> mShadowCB;
    ShadowConstants*                        mMappedShadowCB   = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> mProbeCB;
    ProbeConstants*                         mMappedProbeCB    = nullptr;

    // Shadow SRV from last SetShadowData() call.
    D3D12_GPU_DESCRIPTOR_HANDLE mShadowSrvHandle{};

    // GI SRV and intensity from last SetGiSrv() call.
    // Handle is zero-initialised; zero ptr means no GI contribution.
    D3D12_GPU_DESCRIPTOR_HANDLE mGiSrvHandle{};
    float                       mGiIntensity = 0.0f;

    // Probe SH SRV from last SetProbeSrv() call.
    D3D12_GPU_DESCRIPTOR_HANDLE mProbeSrvHandle{};

    // Specular reflections SRV from last SetSpecularSrv() call.
    D3D12_GPU_DESCRIPTOR_HANDLE mSpecularSrvHandle{};
    float                       mSpecularIntensity = 0.0f;

    // AO SRV and intensity from last SetAoSrv() call.
    D3D12_GPU_DESCRIPTOR_HANDLE mAoSrvHandle{};
    float                       mAoIntensity = 0.0f;

    // Volumetric fog 3D texture SRV from last SetVolumetricFogSrv() call.
    D3D12_GPU_DESCRIPTOR_HANDLE mFogSrvHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE mPointShadowSrvHandle{};
    int                         mPointShadowLightCount = 0;
    float                       mPointShadowMapSize = 0.0f;
    float                       mPointShadowBias = 0.0f;

    UINT mWidth  = 0;
    UINT mHeight = 0;

    bool mIsInitialized   = false;
    bool mPipelineReady   = false;
    std::string mLastError;
};
