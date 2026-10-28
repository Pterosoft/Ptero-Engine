#pragma once

// RtGlobalIllumination.h
// Custom Ray Traced Global Illumination renderer for Ptero-Engine.
//
// Four compute passes using DXR inline ray-tracing (cs_6_5 RayQuery):
//   Pass 0 - RayGen    : fire GI rays against the TLAS, write initial reservoirs + radiance
//   Pass 1 - Temporal  : ReSTIR temporal resampling against previous frame
//   Pass 2 - Spatial   : ReSTIR spatial resampling from screen neighbours
//   Pass 3 - Composite : temporal accumulation + final colour output

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "RtGISettings.h"
#include "DeferredLightingPass.h"
#include "LightClusterRenderer.h"
#include "Components.h"         // Entity, Mesh
#include "NrdDenoiser.h"        // NVIDIA NRD RELAX_DIFFUSE wrapper
#include "TextureManager.h"
#include "VegetationRenderer.h" // RayTracingBatch
#include "VirtualShadowMapConstants.h"
#include "GpuSkyModel.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <cstdint>
#include <array>
#include <cstddef>
#include <memory>
#include <vector>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

class VirtualGeometryRenderer;
namespace VirtualGeometry { struct BuiltMesh; }

// -----------------------------------------------------------------------
// Per-vertex data uploaded into the global geometry SRV buffer.
// Stride = 24 bytes.
// -----------------------------------------------------------------------
struct GpuPackedVertex
{
    float px, py, pz;  // object-space position
    float nx, ny, nz;  // object-space normal
    float u, v;        // object-space UV0
};

// Per-instance lookup record that maps a TLAS instance index to its slice
// of the global vertex and index buffers.  Indexed by CommittedInstanceIndex().
struct GpuInstanceInfo
{
    uint32_t vertexOffset;  // first vertex index inside mVertexBuffer
    uint32_t indexOffset;   // first index inside mIndexBuffer
    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t materialRangeOffset;
    uint32_t materialRangeCount;
    uint32_t _pad0;
    uint32_t _pad1;
};

struct GpuMaterialRange
{
    uint32_t startPrimitive;
    uint32_t primitiveCount;
    float    baseColorR;
    float    baseColorG;
    float    baseColorB;
    float    baseColorA;
    float    opacityFactor;
    float    alphaCutoff;
    uint32_t baseColorTextureIndex;
    uint32_t opacityTextureIndex;
    uint32_t flags;
    // Terrain ranges: how far tessellation can move the drawn surface off the traced
    // mesh, in world units, so passes matching a G-Buffer point to a traced hit
    // allow for it. 0 for everything else.
    float    terrainReach;
};

// The surface response at a hit, one per GpuMaterialRange and at the same index,
// in its own buffer so the passes that only need base colour keep their layout.
// The first 48 bytes are what RTGI and the probes read (tint and averages); the
// path tracer also reads the factors and samples the maps themselves through the
// surface texture table (GetSurfaceTextureTableSrv). The averaged values stand in
// wherever a map is not in the table. Mirrors RtMaterialSurface.hlsli.
struct GpuMaterialSurface
{
    float tintR, tintG, tintB;   // baseColorTint alone, without any texture average
    float roughness;             // factor x the roughness map's average
    float emissiveR, emissiveG, emissiveB;   // colour x the emissive map's average
    float metallic;              // factor x the metallic map's average
    float specular;              // specularFactor; 0.5 is the engine's neutral F0 of 0.04
    float baseColorAverageR;     // the base texture's average, for when it is not in the table
    float baseColorAverageG;
    float baseColorAverageB;

    float emissiveColorR, emissiveColorG, emissiveColorB;   // the factors alone, for sampled maps
    float roughnessFactor;
    float metallicFactor;
    float normalScale;
    uint32_t packedLayout;       // 0 separate maps, 1 metallicRoughness (R rough, G metal), 2 ORM (G, B)
    uint32_t flags;              // kRtSurfaceFlag*

    // Indices into the surface texture table, or 0xffffffff.
    uint32_t baseColorTexture;
    uint32_t normalTexture;
    uint32_t roughnessTexture;   // the packed map when packedLayout != 0
    uint32_t metallicTexture;
    uint32_t emissiveTexture;
    uint32_t opacityTexture;
    uint32_t terrainTrace;       // terrain: its GpuTerrainTrace record; otherwise 0xffffffff
    float sssRadius = 0.0f;      // subsurface: the profile's range in metres

    // Disney principled lobes (PrincipledBrdf.hlsli), the material's values; all off at 0.
    float principledSubsurface     = 0.0f;
    float principledSpecularTint   = 0.0f;
    float principledSheen          = 0.0f;
    float principledSheenTint      = 0.5f;
    float principledClearcoat      = 0.0f;
    float principledClearcoatGloss = 1.0f;
    float principledAnisotropic    = 0.0f;
    uint32_t principledMaskTexture      = 0xffffffffu;   // surface texture table slot, or 0xffffffff
    uint32_t anisotropyDirectionTexture = 0xffffffffu;

    // The separable-SSS profile of a material with kRtSurfaceFlagSubsurface, as
    // SubsurfaceScattering.cpp hands it to the raster passes (ColorRadius, FalloffTranslucency).
    float sssColorR = 0.0f, sssColorG = 0.0f, sssColorB = 0.0f;
    float sssFalloffR = 0.0f, sssFalloffG = 0.0f, sssFalloffB = 0.0f;
    float sssTranslucency = 0.0f;
};
static_assert(sizeof(GpuMaterialSurface) == 176, "GpuMaterialSurface must match RtMaterialSurface.hlsli");

// What a tracer needs to rebuild a terrain's tessellated displacement at any point
// (Terrain.hlsl's DisplaceLocalPosition), for shadow rays to march it: the traced
// copy is the undisplaced mesh. One per terrain instance, indexed by
// GpuMaterialSurface::terrainTrace. Mirrors GpuTerrainTrace in PathTracer_Common.hlsli.
struct GpuTerrainTraceLayer
{
    uint32_t heightTexture;      // surface texture table slot, or 0xffffffff
    float    tileSize;
    float    rotationSin, rotationCos;
    float    uvTilingX, uvTilingY;
    float    uvOffsetX, uvOffsetY;
    float    displacementScale;  // metres
    float    midLevel;
    float    _pad0, _pad1;
};
static_assert(sizeof(GpuTerrainTraceLayer) == 48, "GpuTerrainTraceLayer must match PathTracer_Common.hlsli");

struct GpuTerrainTrace
{
    float    worldToLocal[12];   // 3x4, rows: local = row . float4(world, 1)
    uint32_t layerCount;
    uint32_t useSplat;
    uint32_t weightsTexture;     // surface texture table slot of the baked splat weights
    uint32_t flags;              // 1 break-up tiling, 2 height blend
    float    heightBlendSharpness;
    float    reach;              // metres the surface can stand off the traced mesh
    float    worldSize;          // patch edge, local units
    float    _pad0;
    GpuTerrainTraceLayer layers[4];
};
static_assert(sizeof(GpuTerrainTrace) == 272, "GpuTerrainTrace must match PathTracer_Common.hlsli");
inline constexpr uint32_t kRtSurfaceFlagFlipNormalGreen = 1u;
inline constexpr uint32_t kRtSurfaceFlagSubsurface      = 2u;   // PTERO_RT_SURFACE_SUBSURFACE

struct RtMaterialSlotInfo
{
    std::string BaseColorPath;
    std::string OpacityPath;
    float       BaseColorTint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float       OpacityFactor    = 1.0f;
    float       AlphaCutoff      = 0.5f;
    bool        UseAlphaCutout   = false;
    bool        DoubleSided      = false;

    // Surface response, read the way EntityMeshRenderer reads it.
    float       RoughnessFactor  = 1.0f;
    float       MetallicFactor   = 1.0f;
    float       SpecularFactor   = 0.5f;
    float       EmissiveColor[3] = { 0.0f, 0.0f, 0.0f };
    std::string RoughnessPath;     // the packed texture when PackedLayout != 0
    std::string MetallicPath;
    std::string EmissivePath;
    int         PackedLayout     = 0;   // 0 separate maps, 1 metallicRoughness (R rough, G metal), 2 ORM
    std::string NormalPath;
    float       NormalScale      = 1.0f;
    bool        FlipNormalGreen  = false;

    // Disney principled lobes, read the way EntityMeshRenderer reads them.
    float       PrincipledSubsurface     = 0.0f;
    float       PrincipledSpecularTint   = 0.0f;
    float       PrincipledSheen          = 0.0f;
    float       PrincipledSheenTint      = 0.5f;
    float       PrincipledClearcoat      = 0.0f;
    float       PrincipledClearcoatGloss = 1.0f;
    float       PrincipledAnisotropic    = 0.0f;
    std::string PrincipledMaskPath;
    std::string AnisotropyDirectionPath;

    // Separable subsurface scattering, read the way EntityMeshRenderer reads it (only
    // opaque surfaces scatter there, so a blended one does not here either).
    bool        UseSubsurface            = false;
    float       SubsurfaceColor[3]       = { 0.48f, 0.41f, 0.28f };
    float       SubsurfaceFalloff[3]     = { 1.0f, 0.37f, 0.3f };
    float       SubsurfaceRadiusMm       = 3.0f;
    float       SubsurfaceTranslucency   = 0.8f;
};

// -----------------------------------------------------------------------
// GPU constant buffer shared across all four RTGI shaders (256 bytes).
// -----------------------------------------------------------------------
struct alignas(256) RtGIConstants
{
    uint32_t FrameWidth;
    uint32_t FrameHeight;
    uint32_t FrameIndex;
    uint32_t RaysPerPixel;

    uint32_t MaxHistoryLength;
    uint32_t SpatialSamples;
    float    SpatialRadius;
    float    DepthThreshold;

    float    NormalThreshold;
    float    RadianceClamp;
    float    AccumulationBlend;
    int      MaxBounces;

    int      DebugView;
    int      TemporalReuseEnabled;
    int      SpatialReuseEnabled;
    int      NextEventEstimation;

    float    NrdSharpenAmount;
    float    ColorLeakIntensity;
    float    SpecularRoughnessThreshold;
    uint32_t TraceScale;        // diffuse grid divisor: 1, 2 or 4 (RtGI_Common.hlsli, "Reduced-resolution grids")

    float    ViewProjInv[16];   // row-major inverse view-projection (world-pos reconstruction)
    float    CurrViewProj[16];  // row-major current non-jittered view-projection (NRD motion vectors)
    float    PrevViewProj[16];  // row-major previous-frame view-projection (temporal reprojection)
    // Current non-jittered world-to-view matrix in the same row-major convention.
    // Used by the NRD prepare pass to compute true camera-space viewZ.
    float    WorldToView[16];

    float    CameraPos[3];
    uint32_t SpecularScale;     // specular grid divisor: 1 or 2

    float    SunDirX, SunDirY, SunDirZ;    // world-space direction FROM sun TOWARD scene (Z-up)
    float    _pad2;

    float    SunColorR, SunColorG, SunColorB;  // scaled Hosek-Wilkie sun colour
    float    _pad3;

    float    SkyColorR, SkyColorG, SkyColorB;  // scaled Hosek-Wilkie sky colour
    float    _pad4;

    int      NumPointLights;
    float    _pad5[3]{};
    DeferredLightingPass::PointLightGpu PointLights[DeferredLightingPass::kMaxPointLights]{};

    // Next-event visibility from the virtual shadow map (RtGI_Vsm.hlsli).
    float    VsmMaxTexelSize;
    int      VsmVisibility;     // 1 = ask the map first, trace only where it cannot answer
    int      LightSamples;      // > 0: lights whose visibility a bounce hit tests, picked stochastically
    float    _pad6{};
    VsmGpuConstants Vsm{};      // Active = 0 when the map is not bound this frame

    // The sky escaped rays see (SkyModel.hlsli), pre-exposed like the colours above.
    GpuSkyModel Sky{};

    // Ray visualisation capture (RtGI_RayDebug.hlsli); see RtGlobalIllumination::PrepareRayDebug.
    uint32_t RayDebugWrite;
    uint32_t RayDebugPattern;
    uint32_t RayDebugSpacing;
    uint32_t RayDebugGridWidth;
    uint32_t RayDebugSlotBase;
    uint32_t RayDebugRays;
    uint32_t RayDebugSlotsPerRay;
    uint32_t RayDebugSlotsPerPixel;
};
// HLSL starts a struct member of a cbuffer on a 16-byte register; the C++ side has
// to land it there too or every field of g_Vsm reads shifted.
static_assert(offsetof(RtGIConstants, Vsm) % 16 == 0, "RtGIConstants::Vsm must be 16-byte aligned to match g_Vsm");
static_assert(offsetof(RtGIConstants, Sky) % 16 == 0, "RtGIConstants::Sky must be 16-byte aligned to match g_Sky");
static_assert(offsetof(RtGIConstants, RayDebugWrite) == offsetof(RtGIConstants, Sky) + sizeof(GpuSkyModel),
    "RtGIConstants::RayDebugWrite must follow g_Sky directly, as g_RayDebugWrite does");

// One recorded ray segment of the RTGI ray visualisation. Mirrors RtGIDebugRaySegment
// in RtGI_RayDebug.hlsli.
struct RtGIDebugRaySegment
{
    float    start[3];
    uint32_t kind;
    float    end[3];
    uint32_t bounce;
    float    light[3];
    float    _pad0;
    float    albedo[3];
    float    _pad1;
};
static_assert(sizeof(RtGIDebugRaySegment) == 64, "RtGIDebugRaySegment must match RtGI_RayDebug.hlsli");

// -----------------------------------------------------------------------
// One element in the per-pixel GI reservoir structured buffer.
// -----------------------------------------------------------------------
struct RtGIPackedReservoir
{
    float    posX, posY, posZ;
    uint32_t packedNormal;      // oct-encoded 2x snorm16

    float    radR, radG, radB;
    float    weight;            // W = w_sum / (M * p_hat)

    uint32_t M;                 // candidate count
    uint32_t age;               // temporal age of the chosen sample
    uint32_t _pad0, _pad1;
};

class RtGlobalIllumination
{
public:
    static constexpr uint32_t kMaxRtMaterialTextures = 32;
    // Every map a ray can hit - base colour, normal, roughness, metallic, emissive,
    // opacity - for the path tracer, which binds them as one array (register space 1).
    static constexpr uint32_t kMaxRtSurfaceTextures = 1024;

    // Load material textures through the raster renderer's cache, so a texture both
    // draw and trace use exists once in VRAM. Null uses this pass's own cache.
    void SetSharedTextureManager(TextureManager* manager) { mSharedTextureManager = manager; }

    RtGlobalIllumination()  = default;
    ~RtGlobalIllumination() { Shutdown(); }

    // Initialize (or re-initialize) for the given render resolution.
    bool Initialize(UINT width, UINT height);

    // Resize resolution-dependent resources (no-op if size unchanged).
    bool EnsureSize(UINT width, UINT height);

    // Build / update the TLAS from the current entity list.  Call once per frame before Dispatch.
    // Build / update the TLAS from the current entity list, plus any vegetation
    // instances whose layer opted into ray tracing (see
    // VegetationRenderer::CollectRayTracingBatches).  Vegetation is passed in
    // rather than pulled from the entity list because its instances are
    // procedural and have no entity of their own.
    //
    // `detail`, when given, lets meshes with a virtualized-geometry cluster DAG
    // be traced against a static cut through it rather than every source
    // triangle, with the cut chosen per instance from its distance to the camera.
    struct BlasDetail
    {
        DirectX::XMFLOAT3 CameraPosition{};
        // Source of the cluster DAGs; null traces source triangles only.
        const VirtualGeometryRenderer* VirtualGeometry = nullptr;
        float LodError     = 0.005f;   // metres, nearest tier
        float TierDistance = 15.0f;    // metres, where the second tier starts
        int   TierCount    = 3;
    };

    // `terrainPatches` (TerrainRenderer::CollectRayTracingPatches) enter the scene
    // as their raster mesh, at most kTerrainRtGridQuads quads a side, with a baked
    // splat-blended albedo; see BuildTerrainRt in the .cpp.
    void BuildTlas(
        ID3D12GraphicsCommandList4* cmdList,
        const std::vector<Entity>&  entities,
        const std::vector<VegetationRenderer::RayTracingBatch>* vegetationBatches = nullptr,
        const BlasDetail* detail = nullptr,
        const std::vector<TerrainRenderer::RayTracingPatch>* terrainPatches = nullptr);

    // The virtual shadow map this frame's RayGen and specular passes may answer
    // next-event visibility from, or null when it is not active. The pool must be
    // readable by compute (VirtualShadowMapRenderer::RenderPages leaves it so).
    // Call before Dispatch / DispatchSpecularOnly.
    void SetVirtualShadowMap(
        const VsmGpuConstants*      constants,
        D3D12_GPU_VIRTUAL_ADDRESS   pageTable,
        D3D12_GPU_DESCRIPTOR_HANDLE poolSrv);

    // The sky rays that escape the scene see, pre-exposed like the sun and sky
    // colours passed to Dispatch. Call before Dispatch / DispatchSpecularOnly.
    void SetSkyModel(const GpuSkyModel& sky) { mSkyModel = sky; }

    // Every scene light and its clustered lists (LightClusterRenderer): what RayGen and
    // the specular pass light bounce hits from. Neither runs until this has been set.
    // Call before Dispatch / DispatchSpecularOnly.
    void SetLightClusters(const LightClusterBindings& bindings) { mLightClusters = bindings; }

    // Resolution divisors for the diffuse passes (1, 2 or 4) and the specular pass
    // (1 or 2), from RtGISettings::DiffuseResolution / SpecularResolution. Other
    // values round down to the nearest of those. Takes effect at the next
    // EnsureSize, which rebuilds the per-resolution buffers when they change.
    void SetTraceResolution(int diffuseDivisor, int specularDivisor);

    // Release all GPU resources.
    void Shutdown();

    // Dispatch all RTGI compute passes for this frame.
    // When settings.UseNrdDenoiser is true the custom temporal/spatial/composite
    // passes are replaced by NVIDIA NRD RELAX_DIFFUSE denoising.
    //
    // worldToViewMatrix / viewToClipMatrix: column-major, NON-jittered, for NRD.
    // prevWorldToViewMatrix / prevViewToClipMatrix: same for the previous frame.
    void Dispatch(
        ID3D12GraphicsCommandList4*  cmdList,
        D3D12_GPU_DESCRIPTOR_HANDLE  gbufferAlbedoSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE  gbufferNormalDepthSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE  gbufferMaterialSrv,
        const RtGISettings&          settings,
        const float                  viewProjInv[16],
        const float                  currViewProj[16],
        const float                  prevViewProj[16],
        const float                  cameraPos[3],
        float sunDirX, float sunDirY, float sunDirZ,
        float sunR, float sunG, float sunB,
        float skyR, float skyG, float skyB,
        const DeferredLightingPass::PointLightGpu* pointLights,
        uint32_t                     numPointLights,
        // NRD-specific matrices (non-jittered, column-major XMFLOAT4X4 layout).
        // Pass nullptr to disable NRD even when settings.UseNrdDenoiser is true.
        const float worldToViewMatrix[16]     = nullptr,
        const float viewToClipMatrix[16]      = nullptr,
        const float prevWorldToViewMatrix[16] = nullptr,
        const float prevViewToClipMatrix[16]  = nullptr,
        const float cameraJitter[2]           = nullptr,
        const float prevCameraJitter[2]       = nullptr,
        float       timeDeltaMs               = 0.0f);

    // Runs only the specular reflection pass, for frames where the radiance
    // probes supply diffuse GI and Dispatch() is skipped. Requires the TLAS
    // to have been built this frame, exactly as Dispatch() does.
    void DispatchSpecularOnly(
        ID3D12GraphicsCommandList4*  cmdList,
        D3D12_GPU_DESCRIPTOR_HANDLE  gbufferAlbedoSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE  gbufferNormalDepthSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE  gbufferMaterialSrv,
        const RtGISettings&          settings,
        const float                  viewProjInv[16],
        const float                  currViewProj[16],
        const float                  worldToViewMatrix[16],
        const float                  cameraPos[3],
        float sunDirX, float sunDirY, float sunDirZ,
        float sunR, float sunG, float sunB,
        float skyR, float skyG, float skyB,
        const DeferredLightingPass::PointLightGpu* pointLights,
        uint32_t                     numPointLights);

    // Draws the paths the ray visualisation captured (RtGISettings::RayDebug*) as
    // lines into the scene colour target, depth tested against the scene depth
    // and without writing it. viewProj is the transposed jittered view-projection
    // the depth was rasterised with. Does nothing until Dispatch() has captured
    // something with the overlay on.
    void DrawDebugRays(
        ID3D12GraphicsCommandList*   cmdList,
        D3D12_CPU_DESCRIPTOR_HANDLE  sceneRtv,
        D3D12_CPU_DESCRIPTOR_HANDLE  sceneDsv,
        DXGI_FORMAT                  sceneColorFormat,
        DXGI_FORMAT                  sceneDepthFormat,
        UINT                         width,
        UINT                         height,
        const float                  viewProj[16],
        const RtGISettings&          settings,
        float                        invPreExposure);

    // Shader-visible SRV GPU handle for the final accumulated GI output (RGBA16F).
    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputSrv()   const { return mOutputSrvGpu; }

    // Shader-visible SRV GPU handle for the specular reflections output (RGBA16F).
    D3D12_GPU_DESCRIPTOR_HANDLE GetSpecularSrv() const { return mSpecularSrvGpu; }

    // Shader-visible SRV GPU handle for the TLAS built by BuildTlas().
    // Used by the RTAO pass so it can share the same acceleration structure without rebuilding it.
    D3D12_GPU_DESCRIPTOR_HANDLE GetTlasSrv()        const { return mTlasSrvGpu; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetVertexSrv()      const { return mVertexSrvGpu; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetIndexSrv()       const { return mIndexSrvGpu; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetInstanceInfoSrv() const { return mInstanceInfoSrvGpu; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetMaterialRangeSrv() const { return mMaterialRangeSrvGpu; }
    // GpuMaterialSurface per material range, at the same indices.
    D3D12_GPU_DESCRIPTOR_HANDLE GetMaterialSurfaceSrv() const { return mMaterialSurfaceSrvGpu; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetBaseTextureTableSrv() const { return mBaseTextureTableGpu; }
    // kMaxRtSurfaceTextures contiguous SRVs, indexed by GpuMaterialSurface's *Texture fields.
    D3D12_GPU_DESCRIPTOR_HANDLE GetSurfaceTextureTableSrv() const { return mSurfaceTextureTableGpu; }
    // StructuredBuffer<GpuTerrainTrace>, indexed by GpuMaterialSurface::terrainTrace.
    D3D12_GPU_DESCRIPTOR_HANDLE GetTerrainTraceSrv() const { return mTerrainTraceSrvGpu; }
    bool                        IsTlasReady()  const { return mTlasReady; }
    // Changes whenever the TLAS is rebuilt from a different scene, so a consumer
    // that accumulates over frames knows its history no longer describes it.
    std::uint64_t               GetSceneSignature() const { return mSceneSignature; }
    // Whether any material range in the TLAS is alpha-cut. Every instance is built
    // opaque, so a pass that wants cutouts to let light through has to force
    // non-opaque traversal and test them itself - which is only worth paying for
    // when there is something to test.
    bool                        HasAlphaCutoutMaterials() const { return mHasAlphaCutoutMaterials; }

    bool        IsInitialized() const { return mIsInitialized; }
    bool        HasInitFailed() const { return mInitFailed; }
    const char* GetLastError()  const { return mLastError.empty() ? nullptr : mLastError.c_str(); }

private:
    bool CreateRootSignature();
    bool CreateSpecularRootSignature();
    // Decides what RayGen records this frame and fills mRayDebugConstants.
    void PrepareRayDebug(const RtGISettings& settings, bool rayGenWillRun);
    bool CreateRayDebugPipeline(DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat);
    bool CreatePipelines();
    bool CreateResolutionBuffers();
    bool CreateDescriptors();
    void RefreshMaterialTextureDescriptors();
    // Create/re-create NRD resources and (re-)initialize NrdDenoiser.
    bool InitNrd();

    void DispatchSpecular(
        ID3D12GraphicsCommandList4*  cmdList,
        D3D12_GPU_DESCRIPTOR_HANDLE  gbufferAlbedoSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE  gbufferNormalDepthSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE  gbufferMaterialSrv,
        const RtGISettings&          settings);

    void UploadConstants(
        const RtGISettings& s,
        const float viewProjInv[16],
        const float currViewProj[16],
        const float prevViewProj[16],
        const float worldToView[16],
        const float cameraPos[3],
        float sunDirX, float sunDirY, float sunDirZ,
        float sunR, float sunG, float sunB,
        float skyR, float skyG, float skyB,
        const DeferredLightingPass::PointLightGpu* pointLights,
        uint32_t numPointLights);

    // ---- GPU helpers shared with TLAS build ----
    //
    // `debugName` is what DRED prints when a page fault lands in or near the
    // allocation. Without it the crash log says "(unnamed)" and names nothing,
    // which is worth very little when the question is which buffer was freed
    // too early. Pass one for anything the GPU reads across frames.
    static bool CreateGpuBuffer(ID3D12Device* dev, UINT64 size,
        D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
        ComPtr<ID3D12Resource>& out, const wchar_t* debugName = nullptr);
    static bool CreateUploadBuf(ID3D12Device* dev, UINT64 size,
        ComPtr<ID3D12Resource>& out, const wchar_t* debugName = nullptr);

    bool     mIsInitialized         = false;
    bool     mInitFailed            = false;
    UINT     mWidth                 = 0;
    UINT     mHeight                = 0;
    // The reduced grids (RtGI_Common.hlsli): the diffuse passes run on the trace
    // grid, the specular pass on its own. The buffers are built for mTraceScale /
    // mSpecularScale; SetTraceResolution asks for new ones through the Requested pair.
    UINT     mTraceScale            = 1;
    UINT     mSpecularScale         = 1;
    UINT     mRequestedTraceScale   = 1;
    UINT     mRequestedSpecularScale = 1;
    UINT     mTraceWidth            = 0;
    UINT     mTraceHeight           = 0;
    UINT     mSpecularWidth         = 0;
    UINT     mSpecularHeight        = 0;
    // Fills the frame back in from a reduced grid (RtGI_Upsample.hlsl).
    void UpsampleToFrame(ID3D12GraphicsCommandList4* cmdList, ID3D12PipelineState* pso,
        ID3D12Resource* reduced, D3D12_GPU_DESCRIPTOR_HANDLE reducedSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE gbufferNormalDepthSrv, D3D12_GPU_DESCRIPTOR_HANDLE gbufferMaterialSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE frameUav);
    uint32_t mFrameIndex            = 0;
    std::string mLastError;

    ComPtr<ID3D12RootSignature>  mRootSignature;

    // One PSO per compute pass.
    ComPtr<ID3D12PipelineState>  mPSO_RayGen;
    ComPtr<ID3D12PipelineState>  mPSO_Temporal;
    ComPtr<ID3D12PipelineState>  mPSO_Spatial;
    ComPtr<ID3D12PipelineState>  mPSO_Composite;
    // NRD path: prepare-inputs and final-composite shaders.
    ComPtr<ID3D12PipelineState>  mPSO_NrdPrepare;
    ComPtr<ID3D12PipelineState>  mPSO_NrdComposite;
    // Extended root signature for NrdPrepare (4 UAV outputs at u0-u3).
    ComPtr<ID3D12RootSignature>  mRootSignatureNrd;

    // Specular reflection pass (GGX ray-traced, separate PSO + root sig).
    ComPtr<ID3D12PipelineState>  mPSO_Specular;
    ComPtr<ID3D12RootSignature>  mRootSignatureSpecular;

    // Persistently-mapped 256-byte upload constant buffer.
    // One RtGIConstants block per frame in flight, in a single upload buffer.
    //
    // A single block was being overwritten by the CPU while the GPU was still
    // executing an earlier frame's RTGI passes, so the shaders could read a
    // mixture of two frames' camera matrices. Standing still that is harmless -
    // consecutive frames hold identical matrices - but under camera motion the
    // inconsistent ViewProjInv and CameraPos put reconstructed world positions
    // off the surface, and the GI rays then either self-intersect or escape.
    // That showed up as black and bright patches that flickered only while the
    // camera moved, and only once the CPU was fast enough to run ahead of the
    // GPU; before that, CPU stalls had been hiding it by acting as a
    // synchroniser.
    ComPtr<ID3D12Resource>  mConstantBuffer;
    uint8_t*                mMappedCb = nullptr;
    uint32_t                mConstantFrameSlot = 0;
    // Offset between consecutive blocks; constant buffers need 256-byte alignment.
    static constexpr UINT64 kConstantStride = (sizeof(RtGIConstants) + 255ull) & ~255ull;

    // Byte offset of the block this frame's passes should bind.
    UINT64 CurrentConstantOffset() const
    {
        return static_cast<UINT64>(mConstantFrameSlot) * kConstantStride;
    }

    // GPU address of that block, for SetComputeRootConstantBufferView.
    D3D12_GPU_VIRTUAL_ADDRESS CurrentConstantAddress() const
    {
        return mConstantBuffer->GetGPUVirtualAddress() + CurrentConstantOffset();
    }

    // Ping-pong reservoir structured buffers (read/write swap each frame).
    ComPtr<ID3D12Resource>  mReservoirBuffer[2];

    // Intermediate GI radiance buffer (RGBA16F), written by RayGen, read by Temporal/Spatial.
    ComPtr<ID3D12Resource>  mRadianceBuffer;

    // Temporal accumulation history (RGBA16F), read and written by Composite each frame.
    ComPtr<ID3D12Resource>  mAccumBuffer;

    // Final composited GI output (RGBA16F), read by the deferred lighting pass.
    ComPtr<ID3D12Resource>  mOutputBuffer;

    // Specular reflection output (RGBA16F), read by the deferred lighting pass.
    ComPtr<ID3D12Resource>  mSpecularBuffer;

    // Reduced-grid results the upsample reads: the denoised diffuse GI on the trace
    // grid, and the reflections on the specular grid. 1x1 placeholders at full
    // resolution, where the passes write the frame-sized buffers directly. Both
    // rest in UNORDERED_ACCESS.
    ComPtr<ID3D12Resource>  mTraceResultBuffer;
    ComPtr<ID3D12Resource>  mSpecularReducedBuffer;
    D3D12_CPU_DESCRIPTOR_HANDLE mTraceResultSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mTraceResultSrvGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mTraceResultUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mTraceResultUavGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mSpecularReducedSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mSpecularReducedSrvGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mSpecularReducedUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mSpecularReducedUavGpu = {};
    ComPtr<ID3D12PipelineState>  mPSO_UpsampleDiffuse;
    ComPtr<ID3D12PipelineState>  mPSO_UpsampleSpecular;

    // Non-shader-visible CPU heap for internal UAV operations.
    ComPtr<ID3D12DescriptorHeap>  mCpuUavHeap;

    // ---- NRD RELAX_DIFFUSE resources ----------------------------------------
    // These textures carry the NRD inputs (prepared by NrdPrepare shader) and
    // the NRD output (consumed by NrdComposite shader).
    ComPtr<ID3D12Resource>  mNrdDiffRadianceHitDist; // IN_DIFF_RADIANCE_HITDIST (RGBA16F)
    ComPtr<ID3D12Resource>  mNrdMotionVectors;        // IN_MV                    (RG16F)
    ComPtr<ID3D12Resource>  mNrdNormalRoughness;      // IN_NORMAL_ROUGHNESS      (RGBA8)
    ComPtr<ID3D12Resource>  mNrdViewZ;                // IN_VIEWZ                 (R32F)
    ComPtr<ID3D12Resource>  mNrdOutDiff;              // OUT_DIFF_RADIANCE_HITDIST (R11G11B10F)

    // SRV/UAV descriptors for the NRD input textures (written by NrdPrepare).
    D3D12_CPU_DESCRIPTOR_HANDLE mNrdDiffRadianceUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mNrdDiffRadianceUavGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mNrdMotionVectorsUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mNrdMotionVectorsUavGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mNrdNormalRoughnessUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mNrdNormalRoughnessUavGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mNrdViewZUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mNrdViewZUavGpu = {};

    // SRV descriptor for the denoised NRD output (read by NrdComposite).
    D3D12_CPU_DESCRIPTOR_HANDLE mNrdOutDiffSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mNrdOutDiffSrvGpu = {};
    // SRV descriptor for the NRD radiance input (read by NrdComposite as SRV).
    D3D12_CPU_DESCRIPTOR_HANDLE mNrdDiffRadianceSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mNrdDiffRadianceSrvGpu = {};

    // The NRD denoiser instance — created/destroyed with the RTGI renderer.
    NrdDenoiser  mNrdDenoiser;
    bool         mNrdDescriptorsAllocated = false;

    // Shader-visible SRV/UAV descriptors allocated from the shared heap.
    D3D12_CPU_DESCRIPTOR_HANDLE mReservoirSrvCpu[2] = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mReservoirSrvGpu[2] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mReservoirUavCpu[2] = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mReservoirUavGpu[2] = {};

    D3D12_CPU_DESCRIPTOR_HANDLE mRadianceSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mRadianceSrvGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mRadianceUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mRadianceUavGpu = {};

    D3D12_CPU_DESCRIPTOR_HANDLE mAccumSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mAccumSrvGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mAccumUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mAccumUavGpu = {};

    D3D12_CPU_DESCRIPTOR_HANDLE mOutputSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputSrvGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mOutputUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputUavGpu = {};

    // Specular reflection buffer descriptors.
    D3D12_CPU_DESCRIPTOR_HANDLE mSpecularSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mSpecularSrvGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mSpecularUavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mSpecularUavGpu = {};
    bool                        mSpecularBufferInSrvState = false;

    // TLAS SRV (bound at t2 in RayGen shader).
    D3D12_CPU_DESCRIPTOR_HANDLE mTlasSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mTlasSrvGpu = {};

    // Global geometry SRVs (bound at t3/t4/t5 in RayGen shader).
    // These allow the shader to interpolate true per-vertex normals at secondary hits.
    D3D12_CPU_DESCRIPTOR_HANDLE mVertexSrvCpu       = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mVertexSrvGpu       = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mIndexSrvCpu        = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mIndexSrvGpu        = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mInstanceInfoSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mInstanceInfoSrvGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mMaterialRangeSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mMaterialRangeSrvGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mMaterialSurfaceSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mMaterialSurfaceSrvGpu = {};

    bool     mDescriptorsAllocated = false;
    uint32_t mReservoirWriteIdx    = 0;
    bool     mOutputBufferInSrvState = false;  // true after first Dispatch; false until then

    // CPU-side geometry pools – grown as new BLASes are built, never shrunk.
    std::vector<GpuPackedVertex> mCpuVertices;
    std::vector<uint32_t>        mCpuIndices;
    bool                         mGeometryDirty = false;  // true when the pools grew this frame

    // Folds everything that affects the acceleration structure's contents into
    // one value, so an unchanged scene can be detected before any work is done.
    // `cutSteps` is the BLAS cut each entity uses this frame (BlasKey::CutStep).
    std::uint64_t ComputeSceneSignature(
        const std::vector<Entity>& entities,
        const std::vector<VegetationRenderer::RayTracingBatch>* vegetationBatches,
        const std::vector<std::int16_t>& cutSteps) const;

    // Signature of the scene the current TLAS was built from: entity count,
    // mesh identity, world transforms and material assignment, plus the same
    // for any ray-traced vegetation.
    //
    // A top-level acceleration structure only describes where instances are, so
    // rebuilding it when nothing has moved produces a bit-identical result at
    // full cost. An editor scene is static most of the time - the camera moves,
    // the geometry does not - so comparing this signature first turns the
    // common case into no work at all. Zero means "nothing built yet".
    std::uint64_t                mSceneSignature = 0;

    // Per-frame instance info rebuilt every BuildTlas call.
    std::vector<GpuInstanceInfo> mCpuInstanceInfo;
    std::vector<GpuMaterialRange> mCpuMaterialRanges;
    std::vector<GpuMaterialSurface> mCpuMaterialSurfaces;   // parallel to mCpuMaterialRanges
    bool                          mHasAlphaCutoutMaterials = false;

    // GPU buffers for the geometry pools (UPLOAD heap, persistently mapped, recreated on growth).
    ComPtr<ID3D12Resource> mVertexBuffer;        // StructuredBuffer<GpuPackedVertex>  t3
    ComPtr<ID3D12Resource> mIndexBuffer;         // StructuredBuffer<uint>              t4
    ComPtr<ID3D12Resource> mInstanceInfoBuffer;  // StructuredBuffer<GpuInstanceInfo>   t5
    ComPtr<ID3D12Resource> mMaterialRangeBuffer; // StructuredBuffer<GpuMaterialRange>
    ComPtr<ID3D12Resource> mMaterialSurfaceBuffer; // StructuredBuffer<GpuMaterialSurface>
    void* mMappedVertices     = nullptr;
    void* mMappedIndices      = nullptr;
    void* mMappedInstanceInfo = nullptr;
    void* mMappedMaterialRanges = nullptr;
    void* mMappedMaterialSurfaces = nullptr;

    // ---- Acceleration structures ----
    // One material slot's triangles in a cut BLAS: the cut regroups triangles by
    // slot, so the source mesh's sub-mesh index ranges no longer describe it.
    struct CutRange
    {
        uint32_t startPrimitive = 0;
        uint32_t primitiveCount = 0;
        uint32_t materialId     = 0;
        uint32_t udimTile       = 0;   // 0 = not a UDIM tile
    };

    struct BlasEntry
    {
        // Null when the build failed; the entry then only remembers not to retry.
        ComPtr<ID3D12Resource> Result;
        // Empty for a BLAS of the source triangles, whose material ranges come
        // from the mesh's sub-meshes.
        std::vector<CutRange> CutRanges;
        // Keeps the asset the key addresses alive, so a freed mesh's address can
        // never be handed to a new mesh that then matches this entry and raytraces
        // the wrong geometry at the wrong pool offsets. A level swap - which is
        // what Play is - frees and reallocates meshes back to back, which is
        // exactly when an allocator recycles addresses.
        std::shared_ptr<const Mesh> MeshOwner;
        uint32_t vertexOffset = 0;  // base index into the global vertex buffer
        uint32_t indexOffset  = 0;  // base index into the global index buffer
        uint32_t vertexCount  = 0;
        uint32_t indexCount   = 0;
    };
    struct BlasKey
    {
        const void*  Mesh = nullptr;
        bool         NonOpaque = false;
        // -1: the source triangles. Otherwise a static cut through the mesh's
        // cluster DAG at CutStepError(CutStep), in mesh units: instances whose
        // scale and distance ask for about the same error share one BLAS.
        std::int16_t CutStep = -1;

        bool operator==(const BlasKey& other) const noexcept
        {
            return Mesh == other.Mesh && NonOpaque == other.NonOpaque && CutStep == other.CutStep;
        }
    };

    struct BlasKeyHasher
    {
        size_t operator()(const BlasKey& key) const noexcept
        {
            return (reinterpret_cast<size_t>(key.Mesh) >> 4)
                ^ (key.NonOpaque ? 0x9e3779b9u : 0u)
                ^ (static_cast<size_t>(static_cast<std::uint16_t>(key.CutStep)) * 0x85ebca6bu);
        }
    };

    std::unordered_map<BlasKey, BlasEntry, BlasKeyHasher> mBlasCache;

    // ---- Terrain ----
    // One per terrain entity: its own BLAS (not in mBlasCache, which never evicts -
    // a sculpted terrain would leave a BLAS behind per stroke), a fixed slice of the
    // geometry pools that a same-sized rebuild overwrites in place, and its material,
    // pushed again on every TLAS build like any other range.
    static constexpr int kTerrainRtGridQuads = 512;   // per axis; the raster mesh is decimated to it
    static constexpr int kTerrainRtTextureRing = 4;   // surface-table slots each terrain cycles through
    struct TerrainRtEntry
    {
        bool          Valid = false;
        std::uint64_t MeshRevision = 0;   // what the BLAS, pools and albedo were built from
        std::uint64_t MaterialHash = 0;
        double        LastBuildSeconds = -1.0e9;
        ComPtr<ID3D12Resource> Blas;
        uint32_t      vertexOffset = 0, indexOffset = 0, vertexCount = 0, indexCount = 0;
        GpuMaterialRange   Range{};
        GpuMaterialSurface Surface{};
        uint32_t      TextureRingSlot = 0;
        // Its displacement for shadow rays (worldToLocal is filled per TLAS build,
        // from the transform it is drawn with); HasTrace when it displaces at all.
        GpuTerrainTrace Trace{};
        bool          HasTrace = false;
    };
    std::unordered_map<std::size_t, TerrainRtEntry> mTerrainRt;   // by entity index

    // This build's GpuTerrainTrace records, and the buffer they reach the GPU in:
    // one slice per frame in flight, like the other pools; never empty, so the
    // descriptor always names something.
    std::vector<GpuTerrainTrace> mCpuTerrainTraces;
    ComPtr<ID3D12Resource>       mTerrainTraceBuffer;
    void*                        mMappedTerrainTraces = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE  mTerrainTraceSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE  mTerrainTraceSrvGpu{};
    bool                         mTerrainTraceSrvAllocated = false;

    // Puts `texture` in the surface texture table under `key` (a slot of its own,
    // reused when the key comes back) and returns the slot. The texture it
    // replaces there is retired through mPendingReleases.
    uint32_t SetSurfaceTexture(const std::string& key, std::shared_ptr<GpuTexture> texture, DXGI_FORMAT format);

    // Distance tier each entity used last frame, by entity index, so a tier
    // boundary is crossed with some hysteresis rather than flickering between
    // two BLASes (and rebuilding the TLAS) while the camera hovers on it.
    std::vector<std::uint8_t> mEntityBlasTier;

    // The escaped rays' sky (SetSkyModel).
    GpuSkyModel                 mSkyModel{};

    // Virtual shadow map for next-event visibility (SetVirtualShadowMap).
    VsmGpuConstants             mVsmConstants{};
    LightClusterBindings        mLightClusters{};
    D3D12_GPU_VIRTUAL_ADDRESS   mVsmPageTable = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE mVsmPoolSrv{};
    bool                        mVsmBound = false;

    // Binds the map, or harmless stand-ins when it is not bound, at the given
    // root parameters of whichever root signature is set.
    void BindVirtualShadowMap(ID3D12GraphicsCommandList4* cmdList, UINT poolParam, UINT pageTableParam,
                              D3D12_GPU_DESCRIPTOR_HANDLE fallbackTextureSrv);
    std::unordered_map<std::string, std::vector<RtMaterialSlotInfo>> mMaterialSlotCache;
    std::unordered_map<std::string, std::array<float, 3>> mTextureAverageColorCache;
    TextureManager mTextureManager;
    TextureManager* mSharedTextureManager = nullptr;
    TextureManager& Textures() { return mSharedTextureManager ? *mSharedTextureManager : mTextureManager; }

    // The surface texture table. A slot, once given to a path, keeps it, so the
    // descriptors frames in flight read are only ever written once - except when the
    // cache hot-reloads a file, and then the replaced texture is retired through
    // mPendingReleases rather than freed under them.
    uint32_t RegisterSurfaceTexture(const std::string& path, TextureSemantic semantic);
    void     RefreshSurfaceTextureDescriptors();
    D3D12_CPU_DESCRIPTOR_HANDLE mSurfaceTextureTableCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mSurfaceTextureTableGpu = {};
    bool                        mSurfaceTextureTableAllocated = false;
    std::unordered_map<std::string, uint32_t> mSurfaceTextureIndices;
    bool mSurfaceTextureTableFullWarned = false;
    std::vector<std::string>                  mSurfaceTexturePaths;    // by slot
    std::vector<std::shared_ptr<GpuTexture>>  mSurfaceTextures;        // by slot, what the descriptor names

    D3D12_CPU_DESCRIPTOR_HANDLE mBaseTextureTableCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mBaseTextureTableGpu = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mOpacityTextureTableCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mOpacityTextureTableGpu = {};
    bool                        mMaterialTextureTableAllocated = false;
    uint32_t                    mMaterialTextureCount = 0;
    std::unordered_map<std::string, uint32_t> mMaterialTextureIndices;

    ComPtr<ID3D12Resource>  mTlas;
    ComPtr<ID3D12Resource>  mTlasScratch;
    ComPtr<ID3D12Resource>  mInstanceDescBuffer;
    UINT                    mTlasMaxInstances = 0;

    // Whether a valid TLAS has been built at least once.
    // RayGen is skipped (outputs zeros) until this is true.
    bool mTlasReady = false;

    // ---- Ray visualisation overlay (RtGI_RayDebug.hlsli) --------------------
    // Capacity of the capture buffer; the grid spacing and history are fitted to it.
    static constexpr uint32_t kMaxDebugRaySegments = 1u << 17;   // 8 MB

    // How the capture buffer's slots are laid out. Whatever was recorded under
    // one layout means nothing under another, so a change starts the history over.
    struct RayDebugLayout
    {
        uint32_t Pattern       = 0;
        uint32_t Spacing       = 0;
        uint32_t GridWidth     = 0;
        uint32_t GridHeight    = 0;
        uint32_t Rays          = 0;
        uint32_t SlotsPerRay   = 0;
        uint32_t SlotsPerPixel = 0;
        uint32_t SlotsPerFrame = 0;
        uint32_t HistoryFrames = 0;
        bool operator==(const RayDebugLayout&) const = default;
    };

    // Created the first time the overlay is switched on; bound at u2 for RayGen
    // and read by the draw's vertex shader as a UAV too, so it never changes
    // state: it is promoted from COMMON to UNORDERED_ACCESS on first use every
    // frame and needs only a UAV barrier between the two.
    ComPtr<ID3D12Resource>      mDebugRayBuffer;
    // Stands in at u2 until mDebugRayBuffer exists; never accessed.
    ComPtr<ID3D12Resource>      mDebugRayPlaceholder;
    ComPtr<ID3D12RootSignature> mRootSignatureRayDebug;
    ComPtr<ID3D12PipelineState> mPSO_RayDebug;
    DXGI_FORMAT                 mRayDebugRtvFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT                 mRayDebugDsvFormat = DXGI_FORMAT_UNKNOWN;
    // The layout of what the buffer holds now, how many history frames hold a
    // capture under it (the first ones, from slot 0), and the frame written next.
    RayDebugLayout              mRayDebugLayout{};
    uint32_t                    mRayDebugFramesRecorded = 0;
    uint32_t                    mRayDebugRingIndex = 0;
    // Whether this frame's RayGen records, and from which slot.
    bool                        mRayDebugRecording = false;
    uint32_t                    mRayDebugSlotBase = 0;
    // Luminance of the pre-exposed sun plus sky, for the overlay's fixed colours.
    float                       mRayDebugReferenceLuminance = 1.0f;

    // Upload buffers that must stay alive until the GPU finishes the frame.
    // Deferred release: every GPU resource this pass replaces goes here instead of
    // being freed where it is replaced, and dies when the ring next reaches its
    // slot. That covers the BLAS geometry uploads, the instance-descriptor buffer,
    // the geometry pools and the TLAS and its scratch - all of which are named by
    // command lists that were recorded in earlier frames and have not executed yet.
    //
    // Depth must match the engine's frames in flight (DX12Context's FrameCount),
    // not two: with three frames queued the CPU can be three frames ahead of the
    // GPU, and a two-slot ring would free a BLAS's vertex or index upload while
    // an earlier frame's acceleration-structure build was still reading it.
    static constexpr uint32_t kFramesInFlight = 3;
    std::vector<ComPtr<ID3D12Resource>> mPendingReleases[kFramesInFlight];
    uint32_t mUploadRingIdx = 0;
};
