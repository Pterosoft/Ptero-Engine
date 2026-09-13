#pragma once

// ParticleRenderer.h
// GPU particle systems: simulation, sprite rendering, and the analytic light
// each emissive system contributes back to the scene.
//
// One ParticleRenderer owns every particle system in the level. Each system
// gets its own particle buffer and its own pair of constant buffers, and is
// simulated by a single compute dispatch and drawn by a single instanced draw.
// Systems are independent, so nothing here needs a global sort or a shared
// free-list.
//
// The emissive contribution is the part worth understanding. Particle sprites
// are not in the ray tracing acceleration structure, so RTGI cannot see them by
// tracing. Instead an emissive system registers an analytic proxy light derived
// from the same emissive colour and intensity the sprites are drawn with, and
// the scene renderer folds that light into the per-frame point-light array. One
// array feeds the deferred shading, the RTGI secondary-bounce evaluation and
// the volumetric fog injection, so a fire lights the room, bounces off its
// walls and glows through smoke from a single source of truth - and flickers in
// all three at once, because the light style is applied before the split.

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "Components.h"
#include "TextureManager.h"
#include "..\System\MaterialEditor.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// One particle system placed in the level, resolved to world space. Built by
// the scene renderer from the entity list once per frame.
struct ParticleSystemInstance
{
    ParticleSystemComponent Settings;

    // Emitter local space -> world. Rotation and translation from the entity
    // transform; the global mesh scale is deliberately not applied, so a
    // particle system's sizes stay in metres regardless of entity scale.
    DirectX::XMMATRIX EmitterToWorld = DirectX::XMMatrixIdentity();
    DirectX::XMFLOAT3 EmitterPosition{ 0.0f, 0.0f, 0.0f };

    // Entity name, used only for resource debug names.
    std::string DebugName;
};

// A light a particle system asks the renderer to add to the scene this frame.
// Position, radius and colour are already final: the style curve, the lumen
// normalisation and the particle colour gradient have all been folded in.
struct ParticleProxyLight
{
    DirectX::XMFLOAT3 Position{ 0.0f, 0.0f, 0.0f };
    float             Radius = 1.0f;
    DirectX::XMFLOAT3 Color{ 0.0f, 0.0f, 0.0f };
    float             InvRadiusSq = 1.0f;
    bool              CastShadows = false;
    bool              AffectVolumetricFog = true;

    // Multiplier applied to this light's colour for indirect lighting only, so
    // a fire can be dialled back in the GI without dimming the direct pool of
    // light it casts on the floor around it.
    float             GiContribution = 1.0f;
};

// Everything the draw pass needs that comes from the frame rather than from a
// particle system: camera, targets, and the scene lighting sprites can receive.
struct ParticleDrawContext
{
    DirectX::XMFLOAT4X4 ViewProjection{};   // already transposed for HLSL
    DirectX::XMFLOAT3   CameraPosition{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3   CameraRight{ 1.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3   CameraUp{ 0.0f, 0.0f, 1.0f };
    DirectX::XMFLOAT3   CameraForward{ 0.0f, 1.0f, 0.0f };

    float NearPlane = 0.1f;
    float FarPlane  = 10000.0f;
    float ScreenWidth  = 1.0f;
    float ScreenHeight = 1.0f;

    // Scene depth as an R32_FLOAT SRV, for the soft-particle fade and, under
    // MSAA, for occlusion. A zero handle disables both for the frame.
    D3D12_GPU_DESCRIPTOR_HANDLE SceneDepthSrv{};

    // Test depth in the pixel shader against SceneDepthSrv instead of relying
    // on the hardware depth test.
    //
    // This is what lets particles draw under MSAA. There, the depth-stencil is
    // the multisampled target while the scene colour target stays
    // single-sample, so no pipeline state can be bound against both - and the
    // resolved depth the rest of the frame reads is a plain texture with no
    // depth-stencil view over it (D3D12 forbids combining ALLOW_DEPTH_STENCIL
    // with the ALLOW_UNORDERED_ACCESS the resolve pass needs). Discarding
    // occluded fragments in the shader sidesteps the whole problem, at the cost
    // of the early-Z rejection the hardware path gets for free.
    bool ManualDepthTest = false;

    DirectX::XMFLOAT3 SunDirection{ 0.0f, 0.0f, -1.0f };
    DirectX::XMFLOAT3 SunColor{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 SkyColor{ 0.0f, 0.0f, 0.0f };

    // Scene point lights that may illuminate sprites. Smoke rising off a fire
    // is the case this exists for: without it the plume stays grey while the
    // flame under it is blazing.
    static constexpr int kMaxSceneLights = 8;
    struct SceneLight
    {
        DirectX::XMFLOAT3 Position{};
        float             Radius = 1.0f;
        DirectX::XMFLOAT3 Color{};
        float             InvRadiusSq = 1.0f;
    };
    SceneLight SceneLights[kMaxSceneLights]{};
    int        NumSceneLights = 0;
};

class ParticleRenderer
{
public:
    ParticleRenderer() = default;
    ~ParticleRenderer() { Shutdown(); }

    ParticleRenderer(const ParticleRenderer&) = delete;
    ParticleRenderer& operator=(const ParticleRenderer&) = delete;

    // Compiles shaders and creates the root signatures and pipeline states.
    // Per-system buffers are created lazily as systems appear in the level.
    bool Initialize();

    void Shutdown();

    bool IsInitialized() const { return mIsInitialized; }

    // Replace the active system list. Call once per frame, before Dispatch.
    // Also advances the style clock and recomputes the proxy lights, so the
    // scene renderer can read them straight afterwards while it assembles the
    // frame's light array - well before the simulation itself runs.
    void SetSystems(const std::vector<ParticleSystemInstance>& systems, float deltaSeconds);

    // Proxy lights the active systems contribute this frame, in the order the
    // systems were supplied. Valid after SetSystems.
    const std::vector<ParticleProxyLight>& GetProxyLights() const { return mProxyLights; }

    // Simulate every active system. cmdList must be recording; the shared SRV
    // descriptor heap does not need to be bound (this pass uses a private one).
    void Dispatch(
        ID3D12GraphicsCommandList* cmdList,
        const DirectX::XMFLOAT3&   windVelocity,
        float                      deltaSeconds);

    // Draw every active system. The caller must have set the render target,
    // viewport, scissor and the shared SRV descriptor heap.
    void Draw(ID3D12GraphicsCommandList* cmdList, const ParticleDrawContext& context);

    // Drop all simulation state; every system refills from scratch (honouring
    // Prewarm) on the next Dispatch.
    void ResetSimulation();

    // Total live particle slots across all systems, for the statistics overlay.
    uint32_t GetActiveParticleCount() const;
    uint32_t GetActiveSystemCount() const { return static_cast<uint32_t>(mSystems.size()); }

    const char* GetLastError() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    // GPU layout for one particle. Must match Particle_Update.hlsl and
    // Particle_Draw.hlsl.
    struct GpuParticle
    {
        DirectX::XMFLOAT3 Position;
        float             Age;
        DirectX::XMFLOAT3 Velocity;
        float             Life;
        DirectX::XMFLOAT4 Seed;
        float             Rotation;
        float             RotationRate;
        float             SizeScale;
        float             FrameOffset;
    };
    static_assert(sizeof(GpuParticle) == 64, "GpuParticle must match the HLSL struct layout.");

    struct alignas(256) UpdateConstants
    {
        DirectX::XMFLOAT4X4 EmitterToWorld;

        DirectX::XMFLOAT3 EmitterPosition;
        float             DeltaTime;

        DirectX::XMFLOAT3 Acceleration;
        float             GlobalTime;

        DirectX::XMFLOAT3 WindVelocity;
        float             WindInfluence;

        DirectX::XMFLOAT3 ShapeExtents;
        float             ShapeRadius;

        float    ConeAngleRadians;
        float    ShapeShellBias;
        uint32_t ShapeType;
        uint32_t ParticleCount;

        float Lifetime;
        float LifetimeVariance;
        float InitialSpeed;
        float SpeedVariance;

        float Drag;
        float TurbulenceStrength;
        float TurbulenceFrequency;
        float TurbulenceSpeed;

        float VortexStrength;
        float RotationRateRadians;
        float RotationRateVariance;
        float StartRotationRadians;

        float    RandomStartRotation;
        float    SizeVariance;
        uint32_t SpawnCursor;
        uint32_t SpawnCount;

        uint32_t RandomSeed;
        uint32_t ResetMode;
        uint32_t FlipbookRandomStart;
        float    PrewarmSpan;
    };

    struct GpuParticleLight
    {
        DirectX::XMFLOAT3 Position;
        float             Radius;
        DirectX::XMFLOAT3 Color;
        float             InvRadiusSq;
    };

    struct alignas(256) DrawConstants
    {
        DirectX::XMFLOAT4X4 ViewProj;

        DirectX::XMFLOAT3 CameraPosition;
        float             ParticleCount;

        DirectX::XMFLOAT3 CameraRight;
        float             StartSize;

        DirectX::XMFLOAT3 CameraUp;
        float             EndSize;

        DirectX::XMFLOAT3 CameraForward;
        float             StretchFactor;

        DirectX::XMFLOAT4 ColorStart;
        DirectX::XMFLOAT4 ColorMid;
        DirectX::XMFLOAT4 ColorEnd;

        DirectX::XMFLOAT3 EmissiveColor;
        float             ColorMidPoint;

        DirectX::XMFLOAT4 BaseColorTint;

        float EmissiveIntensity;
        float FlipbookColumns;
        float FlipbookRows;
        float FlipbookFps;

        float FlipbookBlendFrames;
        float SoftFadeDistance;
        float CameraFadeDistance;
        float UseSoftParticles;

        float NearPlane;
        float FarPlane;
        float ScreenWidth;
        float ScreenHeight;

        float    LightingInfluence;
        float    SphericalNormal;
        float    CullDistance;
        uint32_t FacingMode;

        DirectX::XMFLOAT3 SunDirection;
        float             HasSpriteTexture;

        DirectX::XMFLOAT3 SunColor;
        float             NumLights;

        DirectX::XMFLOAT3 SkyColor;
        float             BlendModeIsPremultiplied;

        float AlphaFromLuminance;
        float ManualDepthTest;
        float Pad0;
        float Pad1;

        GpuParticleLight Lights[ParticleDrawContext::kMaxSceneLights];
    };

    // The parts of a particle material the renderer actually consumes, resolved
    // and cached so the JSON is only parsed when the file changes on disk.
    struct ResolvedMaterial
    {
        DirectX::XMFLOAT4 BaseColorTint{ 1.0f, 1.0f, 1.0f, 1.0f };
        DirectX::XMFLOAT3 EmissiveColor{ 1.0f, 0.45f, 0.12f };
        float             EmissiveIntensity = 1.0f;
        ParticleBlendMode Blend = ParticleBlendMode::Additive;
        float             LightingInfluence = 0.0f;
        float             SphericalNormal = 0.6f;
        float             CameraFadeDistance = 0.6f;
        int               FlipbookColumns = 1;
        int               FlipbookRows = 1;
        bool              AlphaFromLuminance = false;
        std::string       SpriteTexturePath;   // absolute, empty if none

        // False when the path was empty or the file could not be read. The
        // system still draws, using these defaults, so a missing material shows
        // up as a plain orange puff rather than as nothing at all.
        bool              Loaded = false;
    };

    struct CachedMaterial
    {
        ResolvedMaterial                Material;
        std::filesystem::file_time_type LastWriteTime{};
        bool                            FileExists = false;
    };

    // A GPU texture plus its descriptor, for sprite sheets loaded outside the
    // DDS path.
    struct SpriteTexture
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
        D3D12_GPU_DESCRIPTOR_HANDLE            GpuHandle{};
        bool                                   Valid = false;
    };

    // Per-system GPU state, kept across frames so a system keeps simulating
    // rather than restarting every time the entity list is re-synced.
    struct SystemState
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> ParticleBuffer;
        D3D12_RESOURCE_STATES                  BufferState = D3D12_RESOURCE_STATE_COMMON;

        // Private (non-shader-visible is not an option for a table) heap slot
        // for the compute UAV, and a shared-heap slot for the draw SRV. Both
        // are allocated once per system slot and reused for the level's life.
        uint32_t                    UavHeapSlot = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE SrvCpuHandle{};
        D3D12_GPU_DESCRIPTOR_HANDLE SrvGpuHandle{};
        bool                        DescriptorsAllocated = false;

        uint32_t Capacity = 0;        // particles the buffer can hold
        uint32_t ActiveCount = 0;     // particles the current settings use

        // Ring cursor and the fractional spawn debt carried between frames, so
        // a rate that is not a whole number of particles per frame still
        // averages out exactly instead of quantising down.
        uint32_t SpawnCursor = 0;
        float    SpawnAccumulator = 0.0f;
        float    BurstTimer = 0.0f;

        float    SimulationTime = 0.0f;
        uint32_t RandomSeed = 0;

        bool     NeedsPrewarm = true;
        bool     NeedsClear = false;

        ResolvedMaterial Material;
        D3D12_GPU_DESCRIPTOR_HANDLE SpriteSrv{};
        bool                        HasSprite = false;

        // Signature of the settings that force a reseed when they change.
        uint64_t ShapeSignature = 0;
    };

    bool CreateComputePipeline();
    bool CreateDrawPipelines();
    bool CreateConstantBuffers();

    // A 1x1 opaque white texture bound to the sprite and depth slots whenever
    // the real resource is missing. Both are gated off in the shader, but a
    // descriptor table still has to point at a resource of the declared type.
    bool CreateFallbackTexture();
    bool EnsureSystemBuffer(SystemState& state, uint32_t capacity, const std::string& debugName);

    // Loads (or returns a cached) particle material. Never fails hard: an
    // unreadable material yields the built-in defaults with Loaded = false.
    const ResolvedMaterial& ResolveMaterial(const std::string& dataRelativePath);

    // Returns a shader-visible SRV for a sprite texture, loading it on first
    // use. Handles DDS through the shared TextureManager and everything else
    // through WIC, so an artist's PNG or TGA works without a conversion step.
    bool ResolveSpriteTexture(const std::string& absolutePath, D3D12_GPU_DESCRIPTOR_HANDLE& outHandle);
    bool LoadTextureWithWic(const std::string& absolutePath, SpriteTexture& outTexture);

    void RecomputeProxyLights();

    // Which PSO a blend mode maps to, for the depth-tested or no-depth set.
    ID3D12PipelineState* GetPipelineForBlend(ParticleBlendMode blend, bool noDepth) const;

    // ---- pipeline state ---------------------------------------------------
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mUpdateRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mUpdatePso;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mDrawRootSig;

    // Two sets of three: one per blend mode, with and without the hardware
    // depth test. The no-depth set is bound with no depth-stencil view at all,
    // which is the only way to draw alongside a multisampled depth buffer from
    // a single-sample pipeline.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mDrawPsoAdditive;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mDrawPsoAlphaBlend;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mDrawPsoPremultiplied;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mDrawPsoAdditiveNoDepth;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mDrawPsoAlphaBlendNoDepth;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mDrawPsoPremultipliedNoDepth;

    // ---- descriptors ------------------------------------------------------
    // Private shader-visible heap holding one UAV per system slot. Kept apart
    // from the shared engine heap because the compute pass binds its own heap.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mUavHeap;
    uint32_t                                     mUavHeapSlotsUsed = 0;
    uint32_t                                     mDescriptorSize = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> mFallbackTexture;
    D3D12_GPU_DESCRIPTOR_HANDLE            mFallbackTextureSrv{};

    // ---- constant buffers -------------------------------------------------
    // One region per system per frame in flight. The engine keeps three frames
    // in flight, so a single mapped block per system would be overwritten while
    // an earlier frame was still reading it.
    static constexpr uint32_t kFramesInFlight = 3;
    Microsoft::WRL::ComPtr<ID3D12Resource> mUpdateCb;
    uint8_t*                               mUpdateCbPtr = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mDrawCb;
    uint8_t*                               mDrawCbPtr = nullptr;
    uint32_t                               mFrameSlot = 0;

    // ---- per-system state -------------------------------------------------
    std::vector<ParticleSystemInstance> mSystems;
    std::vector<SystemState>            mStates;
    std::vector<ParticleProxyLight>     mProxyLights;

    // Draw order: indices into mSystems, far to near, so alpha-blended systems
    // composite correctly against each other. (Particles inside one system are
    // not sorted; for additive fire that is invisible, and for smoke a single
    // emitter's sprites overlap too softly for the order to read.)
    std::vector<int>                    mDrawOrder;

    std::unordered_map<std::string, CachedMaterial> mMaterialCache;
    std::unordered_map<std::string, SpriteTexture>  mSpriteCache;
    TextureManager                                  mTextureManager;
    MaterialEditor                                  mMaterialLoader;

    float       mGlobalTime = 0.0f;
    bool        mIsInitialized = false;
    std::string mLastError;
};
