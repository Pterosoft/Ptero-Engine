#pragma once

// SubsurfaceScattering
// Screen-space and ray-traced subsurface scattering for the deferred renderer.
//
// Materials opt in with MaterialDefinition::UseSubsurfaceScattering and describe their
// medium (scatter strength, falloff, radius, translucency). While the G-Buffer is drawn,
// EntityMeshRenderer registers each such material with SubsurfaceProfiles and writes the
// returned slot into the normal target's W channel. The frame's profile table and the
// separable kernels built from it (a port of the SDK's SeparableSSS::calculateKernel,
// Source/SDKs/separable-sss-1.0) are uploaded once per frame and read by:
//
//   - the deferred lighting resolve, which writes each subsurface pixel's diffuse
//     lighting to this pass's diffuse target and, in screen-space mode, adds light
//     transmitted through thin parts using shadow-map thickness;
//   - screen space: a horizontal compute blur and a vertical blur that composites
//     straight into the scene colour (SubsurfaceScattering.hlsl);
//   - ray traced: one inline-ray-tracing compute pass that scatters over the real
//     surface and traces transmission thickness (SubsurfaceScattering_RT.hlsl),
//     composited the same way.
//
// The composite adds (scattered - original) diffuse, so specular, fog and everything else
// the lighting pass wrote stays exactly as it was.

#include "DX12Helper.h"
#include "DeferredLightingPass.h"
#include "SubsurfaceSettings.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

// A material's scattering medium, as the registry sees it.
struct SubsurfaceProfileDesc
{
    DirectX::XMFLOAT3 Color{ 0.48f, 0.41f, 0.28f };  // scatter strength per channel
    DirectX::XMFLOAT3 Falloff{ 1.0f, 0.37f, 0.3f };  // per-channel width
    float RadiusMeters = 0.003f;                      // world span of the kernel
    float Translucency = 0.8f;                        // transmission, 0..1
};

// Hands out the profile slots written into the G-Buffer. Slot 0 means "no scattering";
// slots 1..kMaxProfiles-1 are shared by every material with an identical profile. A slot
// is only ever reassigned in a later frame than the one that last used it, and every frame
// uploads its own copy of the table, so a frame in flight never sees its slots change.
namespace SubsurfaceProfiles
{
    constexpr int kMaxProfiles = 16;

    // Start a new frame: slots not used since the previous one become reusable.
    void BeginFrame();

    // Returns the slot for this profile (1..15), or 0 when every slot is already taken by
    // a different profile this frame - the surface then renders without scattering.
    int Acquire(const SubsurfaceProfileDesc& desc);

    // True when at least one slot was acquired since BeginFrame().
    bool AnyActiveThisFrame();
}

// Must match the SubsurfaceConstants cbuffer in Data/Shaders/Subsurface.hlsli.
struct SubsurfaceGpuConstants
{
    static constexpr int kMaxProfiles = SubsurfaceProfiles::kMaxProfiles;
    static constexpr int kMaxKernelSamples = 25;
    static constexpr int kMaxLights = DeferredLightingPass::kMaxPointLights;

    struct Profile
    {
        DirectX::XMFLOAT4 ColorRadius;          // rgb = strength, a = radius (m); a = 0 = unused
        DirectX::XMFLOAT4 FalloffTranslucency;  // rgb = falloff, a = translucency
    };

    Profile             Profiles[kMaxProfiles];
    DirectX::XMFLOAT4   Kernel[kMaxProfiles * kMaxKernelSamples];

    DirectX::XMFLOAT4X4 ViewProj;       // transposed for mul(row, M)
    DirectX::XMFLOAT4X4 InvViewProj;    // transposed

    DirectX::XMFLOAT3   CameraPos;
    uint32_t            KernelSamples;

    DirectX::XMFLOAT2   RenderSize;
    DirectX::XMFLOAT2   InvRenderSize;

    float               ProjScaleX;
    float               ProjScaleY;
    float               DepthA;
    float               DepthB;

    uint32_t            Enabled;
    uint32_t            Mode;
    uint32_t            FollowSurface;
    uint32_t            Transmission;

    float               TransmissionIntensity;
    uint32_t            RtSamples;
    uint32_t            FrameIndex;
    int32_t             DebugView;

    DirectX::XMFLOAT3   SunDirection;   float Pad0;
    DirectX::XMFLOAT3   SunColor;       float Pad1;
    DirectX::XMFLOAT3   SkyAmbient;     int32_t NumLights;

    DeferredLightingPass::PointLightGpu Lights[kMaxLights];

    // Shadow maps, as the deferred lighting pass samples them: the ray-traced mode shadows
    // its scatter samples with these rather than with rays (see SubsurfaceScattering_RT.hlsl).
    DirectX::XMFLOAT4X4 LightViewProj;  // as uploaded to the lighting pass
    float               ShadowMapSize;
    float               ShadowBias;
    float               PointShadowMapSize;
    float               PointShadowBias;
    DirectX::XMFLOAT4X4 PointFaceViewProj[DeferredLightingPass::kMaxShadowCastingPointLights
                                          * DeferredLightingPass::kPointShadowFacesPerLight];
    uint32_t            HasSunShadow;
    uint32_t            HasPointShadows;
    float               ShadowPad[2];
};
static_assert(offsetof(SubsurfaceGpuConstants, Kernel) == 512);
static_assert(offsetof(SubsurfaceGpuConstants, ViewProj) == 6912);
static_assert(offsetof(SubsurfaceGpuConstants, CameraPos) == 7040);
static_assert(offsetof(SubsurfaceGpuConstants, Enabled) == 7088);
static_assert(offsetof(SubsurfaceGpuConstants, SunDirection) == 7120);
static_assert(offsetof(SubsurfaceGpuConstants, Lights) == 7168);
static_assert(offsetof(SubsurfaceGpuConstants, LightViewProj) == 8704);
static_assert(offsetof(SubsurfaceGpuConstants, PointFaceViewProj) == 8784);
static_assert(offsetof(SubsurfaceGpuConstants, HasSunShadow) == 10320);
static_assert(sizeof(SubsurfaceGpuConstants) == 10336, "Must match Subsurface.hlsli");

// Builds one profile's separable kernel exactly as the SDK does: taps spread over
// [-range, range] with a quadratic distribution, weighted by the sum-of-Gaussians skin
// profile stretched per channel by `falloff`, normalised, then blended toward identity by
// `strength`. Tap 0 is the centre. Exposed for the shader's cbuffer and for tests.
void BuildSeparableSssKernel(
    const DirectX::XMFLOAT3& strength,
    const DirectX::XMFLOAT3& falloff,
    int sampleCount,
    DirectX::XMFLOAT4* outKernel);

class SubsurfaceScatteringRenderer
{
public:
    struct FrameInputs
    {
        const SubsurfaceSettings* Settings = nullptr;
        DirectX::XMFLOAT4X4 ViewProjection{};   // jittered, as the G-Buffer was drawn; not transposed
        DirectX::XMFLOAT4X4 Projection{};       // jittered; not transposed
        DirectX::XMFLOAT3   CameraPosition{};
        DirectX::XMFLOAT3   SunDirection{};     // from the sun toward the scene
        DirectX::XMFLOAT3   SunColor{};
        DirectX::XMFLOAT3   SkyAmbient{};
        const DeferredLightingPass::PointLightGpu* Lights = nullptr;
        int                 NumLights = 0;
        bool                RayTracingAvailable = false;  // a TLAS was built this frame
        const DeferredLightingPass::ShadowSnapshot* Shadows = nullptr;
    };

    SubsurfaceScatteringRenderer() = default;
    ~SubsurfaceScatteringRenderer() { Shutdown(); }

    bool Initialize(UINT width, UINT height, DXGI_FORMAT sceneColorFormat);
    bool EnsureSize(UINT width, UINT height);
    void Shutdown();

    bool IsInitialized() const { return mIsInitialized; }
    bool HasInitFailed() const { return mInitFailed; }
    bool SupportsRayTracing() const { return mPsoRayTraced != nullptr; }
    const char* GetLastError() const { return mLastError.empty() ? nullptr : mLastError.c_str(); }

    // Uploads this frame's constants and returns their address for the lighting pass.
    // Call after the G-Buffer pass (which acquires the frame's profiles).
    D3D12_GPU_VIRTUAL_ADDRESS PrepareFrame(const FrameInputs& inputs);

    // True when the last PrepareFrame chose the ray-traced path.
    bool IsRayTracedThisFrame() const { return mRayTracedThisFrame; }

    // Moves the diffuse target into render-target state for the lighting resolve.
    void BeginLightingOutput(ID3D12GraphicsCommandList* commandList);
    D3D12_CPU_DESCRIPTOR_HANDLE GetDiffuseRtv() const { return mDiffuseRtv; }

    // The ray-traced scene shared with RTGI: its TLAS and the geometry pools the world-
    // space mode reads hit normals from. All null when the ray-traced path is not used.
    struct RayTracedSceneSrvs
    {
        D3D12_GPU_DESCRIPTOR_HANDLE Tlas{};
        D3D12_GPU_DESCRIPTOR_HANDLE Vertices{};
        D3D12_GPU_DESCRIPTOR_HANDLE Indices{};
        D3D12_GPU_DESCRIPTOR_HANDLE InstanceInfo{};
        // Shadow maps the scatter samples are shadowed with. The sun map must be readable
        // from compute (ALL_SHADER_RESOURCE) for the duration of Apply().
        D3D12_GPU_DESCRIPTOR_HANDLE SunShadow{};
        D3D12_GPU_DESCRIPTOR_HANDLE PointShadows{};
    };

    // Scatters the diffuse target and composites it into the scene colour, which must be
    // in render-target state. The G-Buffer albedo and normal must be readable from
    // compute (ALL_SHADER_RESOURCE). The scene SRVs are only used in ray-traced mode.
    void Apply(
        ID3D12GraphicsCommandList*  commandList,
        D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv,
        D3D12_GPU_DESCRIPTOR_HANDLE gbufferAlbedoSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE gbufferNormalSrv,
        const RayTracedSceneSrvs&   scene);

private:
    bool CreateRootSignature();
    bool CreatePipelines();
    bool CreateSizeDependentResources();
    void RetireSizeDependentResources();
    void TickRetiredResources();

    static constexpr UINT kFramesInFlight = 3;

    bool        mIsInitialized = false;
    bool        mInitFailed = false;
    std::string mLastError;
    UINT        mWidth = 0;
    UINT        mHeight = 0;
    DXGI_FORMAT mSceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPsoBlurX;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPsoRayTraced;
    // [0] = additive composite, [1] = replace (debug views)
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPsoCompositeBlurY[2];
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPsoCompositeRayTraced[2];

    // Per-frame constants, ringed across the frames in flight.
    Microsoft::WRL::ComPtr<ID3D12Resource> mConstantBuffer;
    std::uint8_t* mMappedConstants = nullptr;
    UINT64        mConstantStride = 0;
    UINT          mFrameSlot = 0;
    uint32_t      mFrameIndex = 0;
    D3D12_GPU_VIRTUAL_ADDRESS mCurrentConstants = 0;
    bool          mRayTracedThisFrame = false;
    int           mDebugView = 0;

    // Diffuse lighting written by the lighting pass (RTV + SRV) and the scratch target the
    // blur / ray-traced pass writes (UAV + SRV).
    Microsoft::WRL::ComPtr<ID3D12Resource> mDiffuseTarget;
    Microsoft::WRL::ComPtr<ID3D12Resource> mScratchTarget;
    D3D12_RESOURCE_STATES mDiffuseState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES mScratchState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mRtvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE mDiffuseRtv{};

    // Allocated once from the shared shader-visible heap, which never frees; views are
    // rewritten in place on resize.
    bool mDescriptorsAllocated = false;
    D3D12_CPU_DESCRIPTOR_HANDLE mDiffuseSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mDiffuseSrvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE mScratchSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mScratchSrvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE mScratchUavCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mScratchUavGpu{};

    // Resources replaced by a resize stay alive until the frames that used them retire.
    struct RetiredResource
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
        UINT FramesRemaining = 0;
    };
    std::vector<RetiredResource> mRetiredResources;
};
