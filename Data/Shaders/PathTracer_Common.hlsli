// PathTracer_Common.hlsli
// The path tracer's bindings and integrator, shared by its three passes:
//   PathTracer.hlsl           reference: progressive accumulation from the camera
//   PathTracer_RealTime.hlsl  real time: one diffuse and one specular path from the
//                             G-Buffer surface, split and demodulated for NRD
//   PathTracer_Composite.hlsl real time: remodulates NRD's output into the image
// All three bind against PathTracer::CreateRootSignature; a pass leaves unbound what
// it does not use. (DXR inline RayQuery: cs_6_5.)
//
// Units match the deferred resolve: its diffuse BRDF is the albedo without 1/pi,
// so a light's colour is irradiance / pi. Here the BRDF is physical (albedo / pi)
// and every light's irradiance is pi times its colour, which makes diffuse direct
// light identical to the raster pass and the specular term physically scaled.
// Lights, sky and emission arrive pre-exposed (see gi-runs-pre-exposed); the
// resolve divides the exposure back out.

#ifndef PTERO_PATH_TRACER_COMMON_HLSLI
#define PTERO_PATH_TRACER_COMMON_HLSLI

#include "LightShapes.hlsli"
#include "ShaderSafeMath.hlsli"
#include "SkyModel.hlsli"
#include "SurfaceSpecular.hlsli"
#include "RtTransform.hlsli"
#include "RtMaterialSurface.hlsli"
#include "PrincipledBrdf.hlsli"

#define PT_MAX_LIGHTS 16

#define PT_FLAG_ALPHA_TEST   1u
#define PT_FLAG_SOFT_SHADOWS 2u
#define PT_FLAG_TRACE        4u   // reference: 0 = the image is held, only resolve it
#define PT_FLAG_ONE_LIGHT    8u   // one scene light per vertex, chosen by its unshadowed contribution
#define PT_FLAG_DENOISED     16u  // composite: read NRD's output rather than the noisy input
#define PT_FLAG_SCREEN_CACHE 32u  // real time: paths end in last frame's denoised lighting where they can
#define PT_FLAG_TERRAIN_SHADOWS 64u // shadow rays march displaced terrain (TerrainShadowVisible)
#define PT_FLAG_DISNEY_BRDF  128u // shade with the Disney principled BRDF (PrincipledBrdf.hlsli)
#define PT_FLAG_SUBSURFACE   256u // materials with useSubsurfaceScattering scatter their diffuse light
#define PT_FLAG_SSS_MASK     512u // SubsurfaceSettings' "Profile Mask" view: SubsurfaceMaskColor instead of the image
#define PT_FLAG_VOLUMETRIC_FOG 1024u // t_VolumetricFog is bound: lay it over the traced pixels

// ─── Constants (PathTracerConstants in PathTracer.h) ─────────────────────────
cbuffer PathTracerConstants : register(b0)
{
    uint     g_Width;
    uint     g_Height;
    uint     g_FrameSeed;
    uint     g_SampleIndex;        // samples already in the accumulation target

    uint     g_SamplesPerFrame;
    uint     g_MaxBounces;
    uint     g_Flags;
    uint     g_NumLights;

    float    g_IndirectClamp;      // pre-exposed luminance; 0 = off
    float    g_EmissiveScale;      // EmissiveIntensity x pre-exposure
    float    g_SunCosAngularRadius;
    float    g_InvPreExposure;

    float4x4 g_ViewProjInv;        // non-jittered, row-vector convention

    float3   g_CameraPos;
    int      g_DebugView;

    float3   g_SunDir;             // from the sun toward the scene (Z up)
    float    g_GroundAlbedo;

    float3   g_SunColor;           // pre-exposed, raster units (irradiance / pi)
    float    g_ScreenCacheScale;   // this frame's pre-exposure over last frame's

    float3   g_SkyAmbient;         // pre-exposed hemisphere average of the sky
    float    g_SssTransmission;    // subsurface transmission intensity; 0 = off

    PteroLightData g_Lights[PT_MAX_LIGHTS];   // pre-exposed

    PteroSkyModel  g_Sky;          // pre-exposed

    // Real time only: the G-Buffer was drawn with the jittered camera; NRD wants
    // non-jittered motion and view depth.
    float4x4 g_ViewProjInvJittered;
    float4x4 g_CurrViewProj;       // non-jittered
    float4x4 g_PrevViewProj;       // non-jittered, previous frame
    float4x4 g_WorldToView;        // non-jittered

    float    g_FogStartDistance;   // VolumetricFogSettings::StartDistance
    float    g_FogMaxDistance;     // VolumetricFogSettings::MaxDistance
    float2   g_FogPad;
}

// ─── Scene (RtGlobalIllumination's pools) ────────────────────────────────────
struct GpuPackedVertex { float px, py, pz, nx, ny, nz, u, v; };
struct GpuInstanceInfo { uint vertexOffset, indexOffset, vertexCount, indexCount, materialRangeOffset, materialRangeCount, _pad0, _pad1; };
struct GpuMaterialRange
{
    uint  startPrimitive;
    uint  primitiveCount;
    float baseColorR, baseColorG, baseColorB, baseColorA;
    float opacityFactor;
    float alphaCutoff;
    uint  baseColorTextureIndex;
    uint  opacityTextureIndex;
    uint  flags;
    float terrainReach;   // GpuMaterialRange::terrainReach
};

RaytracingAccelerationStructure      t_TLAS             : register(t0);
StructuredBuffer<GpuPackedVertex>    t_Vertices         : register(t1);
StructuredBuffer<uint>               t_Indices          : register(t2);
StructuredBuffer<GpuInstanceInfo>    t_InstanceInfo     : register(t3);
StructuredBuffer<GpuMaterialRange>   t_MaterialRanges   : register(t4);
StructuredBuffer<GpuMaterialSurface> t_MaterialSurfaces : register(t5);
Texture2D<float4>                    t_Background       : register(t6);   // the raster image
// Every map a ray can hit, indexed by GpuMaterialSurface's *Texture fields
// (RtGlobalIllumination::GetSurfaceTextureTableSrv). Its own space, so it can be large.
Texture2D<float4>                    t_Textures[1024]   : register(t0, space1);
SamplerState                         s_LinearWrap       : register(s0);

#ifdef PT_SCREEN_CACHE
// The screen radiance cache (real time): last frame's denoised diffuse lighting and
// view depth. Bound as the same frame's NRD output before NRD overwrites it. Only
// the real-time pass declares it, so the reference pass never needs it bound.
Texture2D<float4>                    t_PrevDiffuseLighting : register(t45);
Texture2D<float>                     t_PrevViewZ           : register(t46);
#endif

// VolumetricFogRenderer's integrated volume: rgb in-scattered light, a transmittance,
// in screen uv x log depth. Bound when PT_FLAG_VOLUMETRIC_FOG is set.
Texture3D<float4>                    t_VolumetricFog    : register(t49);
SamplerState                         s_LinearClamp      : register(s1);

// The deferred resolve's fog, over a traced pixel's final (not pre-exposed) colour.
// The raster image the path tracer replaces had it applied there; without it the
// traced image loses every light shaft and glow the fog carries.
float3 ApplyVolumetricFog(uint2 pixel, float viewDistance, float3 color)
{
    if ((g_Flags & PT_FLAG_VOLUMETRIC_FOG) == 0u)
        return color;

    // ComputeFogUv in DeferredLighting.hlsl, with the same pull toward the camera.
    const float nearZ = max(g_FogStartDistance, 0.001f);
    const float farZ = max(g_FogMaxDistance, nearZ + 0.001f);
    const float fogDistance = max(max(viewDistance - 0.05f, g_FogStartDistance), nearZ);
    const float fogZ = saturate(log(fogDistance / nearZ) / log(farZ / nearZ));
    const float2 uv = (float2(pixel) + 0.5f) / float2(g_Width, g_Height);
    float4 fog = t_VolumetricFog.SampleLevel(s_LinearClamp, float3(uv, fogZ), 0.0f);

    // SanitizeFogSample in DeferredLighting.hlsl.
    if (any(isnan(fog)) || any(isinf(fog)))
        fog = float4(0.0f, 0.0f, 0.0f, 1.0f);
    fog.rgb = max(fog.rgb, 0.0f);
    fog.a = saturate(fog.a);
    if (fog.a < 0.001f && dot(fog.rgb, fog.rgb) < 1e-8f)
        fog.a = 1.0f;

    return color * fog.a + fog.rgb;
}

static const float kPi = 3.14159265f;
static const uint  kInvalidIndex = 0xffffffffu;
static const uint  kMaterialAlphaCutout = 1u;
static const uint  kMaterialTerrain     = 4u;   // kRtMaterialFlagTerrain
static const float kMaxRayT = 1e5f;

// ─── Random numbers ──────────────────────────────────────────────────────────
uint PcgHash(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float Rand(inout uint state)
{
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    word = (word >> 22u) ^ word;
    return (word >> 8) * (1.0f / 16777216.0f);
}

float Luminance(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

void BuildBasis(float3 n, out float3 t, out float3 b)
{
    // Frisvad / Duff et al., branchless and continuous enough for sampling.
    const float s = n.z >= 0.0f ? 1.0f : -1.0f;
    const float a = -1.0f / (s + n.z);
    const float c = n.x * n.y * a;
    t = float3(1.0f + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

// ─── Materials ───────────────────────────────────────────────────────────────
uint FindMaterialRange(GpuInstanceInfo info, uint primIdx)
{
    [loop]
    for (uint r = 0; r < info.materialRangeCount; ++r)
    {
        const GpuMaterialRange range = t_MaterialRanges[info.materialRangeOffset + r];
        if (primIdx >= range.startPrimitive && primIdx < range.startPrimitive + range.primitiveCount)
            return info.materialRangeOffset + r;
    }
    return kInvalidIndex;
}

float2 HitUv(GpuInstanceInfo info, uint primIdx, float2 bary)
{
    const GpuPackedVertex v0 = t_Vertices[info.vertexOffset + t_Indices[info.indexOffset + primIdx * 3 + 0]];
    const GpuPackedVertex v1 = t_Vertices[info.vertexOffset + t_Indices[info.indexOffset + primIdx * 3 + 1]];
    const GpuPackedVertex v2 = t_Vertices[info.vertexOffset + t_Indices[info.indexOffset + primIdx * 3 + 2]];
    const float b0 = 1.0f - bary.x - bary.y;
    return b0 * float2(v0.u, v0.v) + bary.x * float2(v1.u, v1.v) + bary.y * float2(v2.u, v2.v);
}

// The same opacity GBuffer.hlsl clips against: base alpha x tint alpha x opacity
// factor, times the opacity map's red channel.
bool PassesAlphaTest(uint instIdx, uint primIdx, float2 bary)
{
    const GpuInstanceInfo info = t_InstanceInfo[instIdx];
    const uint rangeIdx = FindMaterialRange(info, primIdx);
    if (rangeIdx == kInvalidIndex)
        return true;

    const GpuMaterialRange range = t_MaterialRanges[rangeIdx];
    if ((range.flags & kMaterialAlphaCutout) == 0u)
        return true;

    const GpuMaterialSurface surface = t_MaterialSurfaces[rangeIdx];
    const float2 uv = HitUv(info, primIdx, bary);
    float opacity = range.baseColorA * saturate(range.opacityFactor);
    if (surface.baseColorTexture != PTERO_RT_NO_TEXTURE)
        opacity *= t_Textures[NonUniformResourceIndex(surface.baseColorTexture)].SampleLevel(s_LinearWrap, uv, 0.0f).a;
    if (surface.opacityTexture != PTERO_RT_NO_TEXTURE)
        opacity *= t_Textures[NonUniformResourceIndex(surface.opacityTexture)].SampleLevel(s_LinearWrap, uv, 0.0f).r;
    return opacity >= saturate(range.alphaCutoff);
}

// ─── Traversal ───────────────────────────────────────────────────────────────
// Every BLAS is built opaque. With alpha testing on, the rays force non-opaque
// traversal so each candidate comes back here to be tested; without it the
// hardware commits hits on its own.
struct HitRecord
{
    bool   hit;
    float  t;
    uint   instance;
    uint   primitive;
    float2 bary;
    float3x4 objectToWorld;
};

HitRecord TraceClosest(float3 origin, float3 dir, float tMin, float tMax)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = tMin;
    ray.TMax = tMax;

    HitRecord rec;
    rec.hit = false;
    rec.t = tMax;
    rec.instance = 0;
    rec.primitive = 0;
    rec.bary = float2(0.0f, 0.0f);
    rec.objectToWorld = (float3x4)0;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
    const uint flags = (g_Flags & PT_FLAG_ALPHA_TEST) != 0u ? RAY_FLAG_FORCE_NON_OPAQUE : RAY_FLAG_NONE;
    rq.TraceRayInline(t_TLAS, flags, 0xFF, ray);
    while (rq.Proceed())
    {
        if (rq.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE
            && PassesAlphaTest(rq.CandidateInstanceIndex(), rq.CandidatePrimitiveIndex(), rq.CandidateTriangleBarycentrics()))
        {
            rq.CommitNonOpaqueTriangleHit();
        }
    }

    if (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        rec.hit = true;
        rec.t = rq.CommittedRayT();
        rec.instance = rq.CommittedInstanceIndex();
        rec.primitive = rq.CommittedPrimitiveIndex();
        rec.bary = rq.CommittedTriangleBarycentrics();
        rec.objectToWorld = rq.CommittedObjectToWorld3x4();
    }
    return rec;
}

// True when nothing blocks the segment.
bool TraceVisibility(float3 origin, float3 dir, float maxDistance)
{
    if (maxDistance <= 0.0f)
        return true;

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.0f;
    ray.TMax = maxDistance;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
    const uint flags = (g_Flags & PT_FLAG_ALPHA_TEST) != 0u ? RAY_FLAG_FORCE_NON_OPAQUE : RAY_FLAG_NONE;
    rq.TraceRayInline(t_TLAS, flags, 0xFF, ray);
    while (rq.Proceed())
    {
        if (rq.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE
            && PassesAlphaTest(rq.CandidateInstanceIndex(), rq.CandidatePrimitiveIndex(), rq.CandidateTriangleBarycentrics()))
        {
            rq.CommitNonOpaqueTriangleHit();
        }
    }
    return rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

// ─── Surface at a hit ────────────────────────────────────────────────────────
struct Surface
{
    float3 position;     // lifted onto the smooth surface (Hanika), the origin for new rays
    float3 normal;       // shading normal, facing the incoming ray
    float3 geoNormal;    // triangle normal, same side as `normal`
    float3 albedo;
    float  roughness;
    float  metallic;
    float  specular;
    float3 emissive;
    // The Disney extra lobes; all off unless PT_FLAG_DISNEY_BRDF is set.
    PteroPrincipled principled;
    // Terrain only: its GpuTerrainTrace record (kInvalidIndex otherwise), and how far
    // the drawn surface stands above `position` along geoNormal, where a shadow ray
    // marching the displacement starts (TerrainShadowVisible).
    uint   terrainTrace;
    float  displacement;
    // Subsurface scattering (PT_FLAG_SUBSURFACE): the material's separable-SSS profile,
    // sssRadius 0 where it has none. See SubsurfaceDirectDiffuse.
    float3 sssStrength;
    float3 sssFalloff;
    float  sssRadius;      // metres
    float  sssTranslucency;
};

// ─── Terrain displacement ────────────────────────────────────────────────────
// The traced terrain is its undisplaced mesh; the drawn one is tessellated and
// displaced by its layers' height maps (Terrain.hlsl, DisplaceLocalPosition). Far
// too fine to trace as triangles - sand ripples are centimetres apart - so shadow
// rays leaving terrain march the displacement itself, from these records
// (RtGlobalIllumination's GpuTerrainTrace, one per terrain instance).
struct GpuTerrainTraceLayer
{
    uint   heightTexture;   // surface texture table slot, or kInvalidIndex
    float  tileSize;
    float  rotationSin;
    float  rotationCos;
    float2 uvTiling;
    float2 uvOffset;
    float  displacementScale;
    float  midLevel;
    float2 _pad;
};
struct GpuTerrainTrace
{
    float4 worldToLocal[3];  // local = dot(row, float4(world, 1))
    uint   layerCount;
    uint   useSplat;
    uint   weightsTexture;
    uint   flags;            // 1 break-up tiling, 2 height blend
    float  heightBlendSharpness;
    float  reach;
    float  worldSize;
    float  _pad0;
    GpuTerrainTraceLayer layers[4];
};
StructuredBuffer<GpuTerrainTrace> t_TerrainTrace : register(t7);

static const uint kTerrainBreakUpTiling = 1u;
static const uint kTerrainHeightBlend   = 2u;

float TerrainHash21(float2 p)
{
    p = frac(p * float2(123.34f, 456.21f));
    p += dot(p, p + 45.32f);
    return frac(p.x * p.y);
}

float TerrainValueNoise(float2 p)
{
    const float2 i = floor(p);
    const float2 f = frac(p);
    const float2 u = f * f * (3.0f - 2.0f * f);
    return lerp(lerp(TerrainHash21(i),                      TerrainHash21(i + float2(1.0f, 0.0f)), u.x),
                lerp(TerrainHash21(i + float2(0.0f, 1.0f)), TerrainHash21(i + float2(1.0f, 1.0f)), u.x),
                u.y);
}

// One layer's height at local XY, as Terrain.hlsl's LayerUv + SampleTiledLevel.
// `footprint` is the spacing, in metres, the caller resolves detail at.
float TerrainLayerHeight(GpuTerrainTrace trace, GpuTerrainTraceLayer layer, float2 localXY, float footprint)
{
    const float2 centered = localXY / max(layer.tileSize, 1e-3f) - 0.5f;
    const float2 rotated = float2(centered.x * layer.rotationCos - centered.y * layer.rotationSin,
                                  centered.x * layer.rotationSin + centered.y * layer.rotationCos) + 0.5f;
    const float2 uv = rotated * layer.uvTiling + layer.uvOffset;

    Texture2D<float4> heightMap = t_Textures[NonUniformResourceIndex(layer.heightTexture)];
    float width, height;
    heightMap.GetDimensions(width, height);
    const float texelsPerMetre = max(width, height) * max(abs(layer.uvTiling.x), abs(layer.uvTiling.y)) / max(layer.tileSize, 1e-3f);
    const float mip = max(log2(max(footprint * texelsPerMetre, 1.0f)), 0.0f);

    float2 uvA = uv, uvB = uv;
    float blend = 0.0f;
    if ((trace.flags & kTerrainBreakUpTiling) != 0u)
    {
        const float index = TerrainValueNoise(uv * 0.125f) * 8.0f;
        const float i = floor(index);
        uvA = uv + sin(float2(3.0f, 7.0f) * i);
        uvB = uv + sin(float2(3.0f, 7.0f) * (i + 1.0f));
        blend = smoothstep(0.2f, 0.8f, index - i);
    }
    const float a = heightMap.SampleLevel(s_LinearWrap, uvA, mip).r;
    return blend > 0.0f ? lerp(a, heightMap.SampleLevel(s_LinearWrap, uvB, mip).r, blend) : a;
}

// Metres the drawn terrain stands off its traced mesh at worldPos (along the
// normal): Terrain.hlsl's DisplaceLocalPosition with the splat weights the traced
// grid baked.
float TerrainDisplacementAt(GpuTerrainTrace trace, float3 worldPos, float footprint)
{
    const float4 p = float4(worldPos, 1.0f);
    const float2 localXY = float2(dot(trace.worldToLocal[0], p), dot(trace.worldToLocal[1], p));

    float4 w = float4(1.0f, 0.0f, 0.0f, 0.0f);
    if (trace.useSplat != 0u)
    {
        Texture2D<float4> weights = t_Textures[NonUniformResourceIndex(trace.weightsTexture)];
        float gridWidth, gridHeight;
        weights.GetDimensions(gridWidth, gridHeight);
        const float2 grid = float2(gridWidth, gridHeight);
        // Texel centres sit on the traced vertices, which span the patch edge to edge.
        const float2 uv = (localXY / max(trace.worldSize, 1e-3f) + 0.5f) * (grid - 1.0f) / grid + 0.5f / grid;
        w = weights.SampleLevel(s_LinearWrap, saturate(uv), 0.0f);
        w *= float4(trace.layerCount > 0u, trace.layerCount > 1u, trace.layerCount > 2u, trace.layerCount > 3u);
        const float sum = dot(w, 1.0f);
        w = sum > 1e-5f ? w / sum : float4(1.0f, 0.0f, 0.0f, 0.0f);
    }

    float4 heights = 0.5f;
    [unroll]
    for (uint i = 0; i < 4u; ++i)
    {
        if (i < trace.layerCount && trace.layers[i].heightTexture != kInvalidIndex && w[i] > 1e-4f)
            heights[i] = TerrainLayerHeight(trace, trace.layers[i], localXY, footprint);
    }

    // Terrain.hlsl's ApplyHeightBlend.
    if ((trace.flags & kTerrainHeightBlend) != 0u && trace.useSplat != 0u)
    {
        const float band = lerp(0.5f, 0.02f, saturate(trace.heightBlendSharpness));
        const float4 score = select(w > 1e-4f, heights + w, -1e4f);
        const float top = max(max(score.x, score.y), max(score.z, score.w)) - band;
        const float4 b = max(score - top, 0.0f);
        const float sum = dot(b, 1.0f);
        if (sum > 1e-5f)
            w = b / sum;
    }

    float displacement = 0.0f;
    [unroll]
    for (uint j = 0; j < 4u; ++j)
    {
        if (j < trace.layerCount && trace.layers[j].heightTexture != kInvalidIndex)
            displacement += w[j] * (heights[j] - trace.layers[j].midLevel) * trace.layers[j].displacementScale;
    }
    return displacement;
}

// Whether the displaced terrain lets a shadow ray from s toward L through, over
// the stretch where it can rise above the ray at all. The ray starts on the drawn
// surface, s.displacement above the traced mesh, and climbs at dot(L, n); the
// mesh is taken as flat over the few metres that takes, which on terrain it is.
bool TerrainShadowVisible(Surface s, float3 L, float maxDistance, inout uint rng)
{
    if (s.terrainTrace == kInvalidIndex)
        return true;
    const GpuTerrainTrace trace = t_TerrainTrace[s.terrainTrace];
    const float climb = dot(L, s.geoNormal);
    if (trace.reach <= 0.0f || climb <= 0.0f)
        return true;

    // Past this the ray is above anything the displacement can raise.
    const float span = min(min(maxDistance, (trace.reach - s.displacement) / max(climb, 0.02f)), 6.0f);
    if (span <= 0.0f)
        return true;

    static const uint kSteps = 16u;
    const float step = span / float(kSteps);
    // A height that tells a surface from the ray grazing it: a share of the reach.
    const float bias = 0.002f + 0.03f * trace.reach;
    const float jitter = Rand(rng);
    [loop]
    for (uint i = 0; i < kSteps; ++i)
    {
        const float t = (float(i) + jitter) * step;
        const float rayHeight = s.displacement + t * climb;
        const float surfaceHeight = TerrainDisplacementAt(trace, s.position + L * t, step);
        if (surfaceHeight > rayHeight + bias)
            return false;
    }
    return true;
}

Surface ResolveSurface(HitRecord rec, float3 rayOrigin, float3 rayDir, float textureLod)
{
    const GpuInstanceInfo info = t_InstanceInfo[rec.instance];
    const uint idx[3] = {
        t_Indices[info.indexOffset + rec.primitive * 3 + 0],
        t_Indices[info.indexOffset + rec.primitive * 3 + 1],
        t_Indices[info.indexOffset + rec.primitive * 3 + 2] };

    const float3 w = float3(1.0f - rec.bary.x - rec.bary.y, rec.bary.x, rec.bary.y);
    float3 vertexPos[3];
    float3 vertexNrm[3];
    float2 vertexUv[3];
    float2 uv = float2(0.0f, 0.0f);
    float3 localNormal = float3(0.0f, 0.0f, 0.0f);
    [unroll]
    for (uint k = 0; k < 3; ++k)
    {
        const GpuPackedVertex v = t_Vertices[info.vertexOffset + idx[k]];
        vertexPos[k] = PteroObjectToWorldPoint(rec.objectToWorld, float3(v.px, v.py, v.pz));
        vertexNrm[k] = PteroObjectToWorldNormal(rec.objectToWorld, float3(v.nx, v.ny, v.nz));
        vertexUv[k] = float2(v.u, v.v);
        uv += w[k] * vertexUv[k];
        localNormal += w[k] * vertexNrm[k];
    }

    const float3 hitPos = rayOrigin + rayDir * rec.t;
    float3 geoNormal = SafeNormalizeOr(cross(vertexPos[1] - vertexPos[0], vertexPos[2] - vertexPos[0]), -rayDir);
    float3 normal = SafeNormalizeOr(localNormal, geoNormal);

    // Everything is shaded two-sided, facing the ray that found it.
    if (dot(geoNormal, rayDir) > 0.0f) geoNormal = -geoNormal;
    if (dot(normal, geoNormal) < 0.0f) normal = -normal;

    // Lift the hit onto the smooth surface its vertex normals describe (Hanika,
    // "Hacking the Shadow Terminator"), so shadow rays and bounces leave from the
    // curved surface the shading assumes rather than the flat facet.
    float3 lifted = hitPos;
    [unroll]
    for (uint j = 0; j < 3; ++j)
    {
        float3 n = vertexNrm[j];
        const float lengthSq = dot(n, n);
        if (lengthSq < 1e-12f)
            continue;
        n *= rsqrt(lengthSq);
        if (dot(n, geoNormal) < 0.0f)
            n = -n;
        lifted -= w[j] * min(0.0f, dot(hitPos - vertexPos[j], n)) * n;
    }

    Surface s;
    s.position = lifted;
    s.normal = normal;
    s.geoNormal = geoNormal;
    s.albedo = float3(0.8f, 0.8f, 0.8f);
    s.roughness = 1.0f;
    s.metallic = 0.0f;
    s.specular = 0.5f;
    s.emissive = float3(0.0f, 0.0f, 0.0f);
    s.principled = PteroPrincipledNone();
    s.terrainTrace = kInvalidIndex;
    s.displacement = 0.0f;
    s.sssStrength = float3(0.0f, 0.0f, 0.0f);
    s.sssFalloff = float3(0.0f, 0.0f, 0.0f);
    s.sssRadius = 0.0f;
    s.sssTranslucency = 0.0f;

    const uint rangeIdx = FindMaterialRange(info, rec.primitive);
    if (rangeIdx != kInvalidIndex)
    {
        const GpuMaterialSurface surface = t_MaterialSurfaces[rangeIdx];
        if ((g_Flags & PT_FLAG_TERRAIN_SHADOWS) != 0u
            && (t_MaterialRanges[rangeIdx].flags & kMaterialTerrain) != 0u && surface.terrainTrace != kInvalidIndex)
        {
            s.terrainTrace = surface.terrainTrace;
            // Resolved finer close by: the footprint grows with the ray's length.
            s.displacement = TerrainDisplacementAt(t_TerrainTrace[s.terrainTrace], s.position, max(rec.t * 1e-3f, 0.005f));
        }

        // Each map where the table holds it, its average where it does not - the
        // same values the G-Buffer pass computes for the surface on screen.
        const bool textured = surface.baseColorTexture != PTERO_RT_NO_TEXTURE;
        float3 textureColor = float3(1.0f, 1.0f, 1.0f);
        if (textured)
            textureColor = t_Textures[NonUniformResourceIndex(surface.baseColorTexture)].SampleLevel(s_LinearWrap, uv, textureLod).rgb;
        s.albedo = saturate(PteroHitBaseColor(surface, textured, textureColor));

        s.roughness = surface.roughness;
        if (surface.roughnessTexture != PTERO_RT_NO_TEXTURE)
        {
            const float4 sample = t_Textures[NonUniformResourceIndex(surface.roughnessTexture)].SampleLevel(s_LinearWrap, uv, textureLod);
            s.roughness = PteroSurfaceRoughnessChannel(surface, sample) * surface.roughnessFactor;
        }
        s.metallic = surface.metallic;
        if (surface.metallicTexture != PTERO_RT_NO_TEXTURE)
        {
            const float4 sample = t_Textures[NonUniformResourceIndex(surface.metallicTexture)].SampleLevel(s_LinearWrap, uv, textureLod);
            s.metallic = PteroSurfaceMetallicChannel(surface, sample) * surface.metallicFactor;
        }
        s.metallic = saturate(s.metallic);
        s.specular = surface.specular;
        if ((g_Flags & PT_FLAG_SUBSURFACE) != 0u && (surface.flags & PTERO_RT_SURFACE_SUBSURFACE) != 0u)
        {
            s.sssStrength = saturate(surface.sssColor);
            s.sssFalloff = max(surface.sssFalloff, float3(0.0f, 0.0f, 0.0f));
            s.sssRadius = max(surface.sssRadius, 1e-5f);
            s.sssTranslucency = saturate(surface.sssTranslucency);
        }
        s.emissive = surface.emissive;
        if (surface.emissiveTexture != PTERO_RT_NO_TEXTURE)
            s.emissive = surface.emissiveColor * t_Textures[NonUniformResourceIndex(surface.emissiveTexture)].SampleLevel(s_LinearWrap, uv, textureLod).rgb;

        // The normal map, in the triangle's UV-aligned frame (dP/du, dP/dv): what
        // TangentFrame.hlsli derives from screen derivatives, taken from the triangle
        // edges instead. The shading normal stays on the ray's side of the face.
        [branch]
        if (surface.normalTexture != PTERO_RT_NO_TEXTURE)
        {
            const float3 e1 = vertexPos[1] - vertexPos[0];
            const float3 e2 = vertexPos[2] - vertexPos[0];
            const float2 d1 = vertexUv[1] - vertexUv[0];
            const float2 d2 = vertexUv[2] - vertexUv[0];
            const float determinant = d1.x * d2.y - d2.x * d1.y;
            if (abs(determinant) > 1e-12f)
            {
                const float3 dpdu = (e1 * d2.y - e2 * d1.y) / determinant;
                const float3 dpdv = (e2 * d1.x - e1 * d2.x) / determinant;
                const float3 T = SafeNormalizeOr(dpdu - s.normal * dot(dpdu, s.normal), float3(0.0f, 0.0f, 0.0f));
                const float3 B = SafeNormalizeOr(dpdv - s.normal * dot(dpdv, s.normal), float3(0.0f, 0.0f, 0.0f));
                if (dot(T, T) > 0.0f && dot(B, B) > 0.0f)
                {
                    const float3 tangentNormal = PteroSurfaceTangentNormal(surface,
                        t_Textures[NonUniformResourceIndex(surface.normalTexture)].SampleLevel(s_LinearWrap, uv, textureLod).rg);
                    float3 mapped = SafeNormalizeOr(T * tangentNormal.x + B * tangentNormal.y + s.normal * tangentNormal.z, s.normal);
                    // A map can tip the normal under the face; nothing is lit from inside.
                    if (dot(mapped, geoNormal) < 0.05f)
                        mapped = SafeNormalizeOr(mapped + geoNormal * (0.05f - dot(mapped, geoNormal)), s.normal);
                    s.normal = mapped;
                }
            }
        }

        [branch]
        if ((g_Flags & PT_FLAG_DISNEY_BRDF) != 0u)
        {
            s.principled.subsurface     = surface.principledSubsurface;
            s.principled.specularTint   = surface.principledSpecularTint;
            s.principled.sheen          = surface.principledSheen;
            s.principled.sheenTint      = surface.principledSheenTint;
            s.principled.clearcoat      = surface.principledClearcoat;
            s.principled.clearcoatGloss = surface.principledClearcoatGloss;
            s.principled.anisotropic    = surface.principledAnisotropic;
            // The mask scales the sliders channel by channel, as GBuffer.hlsl does.
            if (surface.principledMaskTexture != PTERO_RT_NO_TEXTURE)
            {
                const float4 mask = t_Textures[NonUniformResourceIndex(surface.principledMaskTexture)].SampleLevel(s_LinearWrap, uv, textureLod);
                s.principled.clearcoat   *= mask.r;
                s.principled.sheen       *= mask.g;
                s.principled.anisotropic *= mask.b;
                s.principled.subsurface  *= mask.a;
            }

            // Anisotropy runs along the texture's U axis, as on screen (GBuffer.hlsl): the
            // triangle's dP/du, flattened onto the final shading normal - or along the
            // direction map, in the triangle's (dP/du, dP/dv) frame.
            if (s.principled.anisotropic > 0.0f)
            {
                const float3 e1 = vertexPos[1] - vertexPos[0];
                const float3 e2 = vertexPos[2] - vertexPos[0];
                const float2 d1 = vertexUv[1] - vertexUv[0];
                const float2 d2 = vertexUv[2] - vertexUv[0];
                const float determinant = d1.x * d2.y - d2.x * d1.y;
                if (abs(determinant) > 1e-12f)
                {
                    const float3 dpdu = (e1 * d2.y - e2 * d1.y) / determinant;
                    float3 tangent = dpdu;
                    if (surface.anisotropyDirectionTexture != PTERO_RT_NO_TEXTURE)
                    {
                        float2 direction = t_Textures[NonUniformResourceIndex(surface.anisotropyDirectionTexture)]
                            .SampleLevel(s_LinearWrap, uv, textureLod).rg * 2.0f - 1.0f;
                        if ((surface.flags & PTERO_RT_SURFACE_FLIP_NORMAL_GREEN) != 0u)
                            direction.y = -direction.y;
                        if (dot(direction, direction) > 1e-4f)
                        {
                            const float3 dpdv = (e2 * d1.x - e1 * d2.x) / determinant;
                            tangent = SafeNormalizeOr(dpdu, float3(0.0f, 0.0f, 0.0f)) * direction.x
                                    + SafeNormalizeOr(dpdv, float3(0.0f, 0.0f, 0.0f)) * direction.y;
                        }
                    }
                    s.principled.tangent = SafeNormalizeOr(tangent - s.normal * dot(tangent, s.normal), float3(0.0f, 0.0f, 0.0f));
                }
            }
        }
    }
    // Same floor as the deferred resolve.
    s.roughness = clamp(s.roughness, 0.04f, 1.0f);
    return s;
}

// ─── BRDF ────────────────────────────────────────────────────────────────────
// With PT_FLAG_DISNEY_BRDF, the Disney principled BRDF (PrincipledBrdf.hlsli) - the
// raster resolve's model, without its analytic-light masking remap, which Disney meant
// for point lights only. Otherwise GGX with height-correlated Smith masking, Schlick
// Fresnel, and the raster pass's Lambert weighting kD = (1 - F) (1 - metallic).
//
// Sampling stays the isotropic GGX lobe plus the cosine lobe for either model: the pdf
// is the sampler's own, so anisotropy, sheen and clearcoat are still estimated without
// bias - only with more noise than a sampler fitted to them would leave.
float SmithLambda(float cosTheta, float alpha2)
{
    const float c2 = max(cosTheta * cosTheta, 1e-6f);
    return 0.5f * (-1.0f + sqrt(1.0f + alpha2 * (1.0f - c2) / c2));
}

float GgxD(float NdotH, float alpha2)
{
    const float d = NdotH * NdotH * (alpha2 - 1.0f) + 1.0f;
    return alpha2 / max(kPi * d * d, 1e-12f);
}

float3 FresnelSchlick(float3 F0, float cosTheta)
{
    return F0 + (1.0f - F0) * pow(1.0f - saturate(cosTheta), 5.0f);
}

// f(V, L) * N.L, the two lobes apart.
void EvalBrdfCosSplit(Surface s, float3 V, float3 L, out float3 diffuse, out float3 specular)
{
    diffuse = float3(0.0f, 0.0f, 0.0f);
    specular = float3(0.0f, 0.0f, 0.0f);
    const float NdotL = dot(s.normal, L);
    const float NdotV = max(dot(s.normal, V), 1e-4f);
    if (NdotL <= 0.0f)
        return;

    [branch]
    if ((g_Flags & PT_FLAG_DISNEY_BRDF) != 0u)
    {
        // The sheen goes with the diffuse for the denoiser: it is as low-frequency.
        const PteroPrincipledLobes lobes = PteroEvalPrincipled(
            s.normal, V, L, s.albedo, s.metallic, s.roughness, s.specular, s.principled, false);
        diffuse = (lobes.diffuse + lobes.sheen) * NdotL;
        specular = (lobes.specular + lobes.clearcoat) * NdotL;
        return;
    }

    const float3 H = normalize(V + L);
    const float NdotH = saturate(dot(s.normal, H));
    const float VdotH = saturate(dot(V, H));
    const float alpha = s.roughness * s.roughness;
    const float alpha2 = alpha * alpha;

    const float3 F0 = PteroComputeF0(s.albedo, s.metallic, s.specular);
    const float3 F = FresnelSchlick(F0, VdotH);
    const float D = GgxD(NdotH, alpha2);
    const float G2 = 1.0f / (1.0f + SmithLambda(NdotV, alpha2) + SmithLambda(NdotL, alpha2));

    specular = F * (D * G2 / (4.0f * NdotV * NdotL)) * NdotL;
    diffuse = (1.0f - F) * (1.0f - s.metallic) * s.albedo / kPi * NdotL;
}

// f(V, L) * N.L, both lobes.
float3 EvalBrdfCos(Surface s, float3 V, float3 L)
{
    float3 diffuse, specular;
    EvalBrdfCosSplit(s, V, L, diffuse, specular);
    return diffuse + specular;
}

// How often the specular lobe is sampled, from how much each lobe reflects.
// The principled lobes are all off without PT_FLAG_DISNEY_BRDF, so the tinted F0 and the
// clearcoat term reduce to the plain model's there.
float SpecularProbability(Surface s, float3 V)
{
    const float3 F0 = PteroComputePrincipledF0(s.albedo, s.metallic, s.specular, s.principled.specularTint);
    const float NdotV = dot(s.normal, V);
    const float clearcoatWeight = 0.25f * s.principled.clearcoat * lerp(0.04f, 1.0f, PteroSchlickWeight(saturate(NdotV)));
    const float specularWeight = Luminance(FresnelSchlick(F0, NdotV)) + clearcoatWeight;
    const float diffuseWeight = Luminance(s.albedo) * (1.0f - s.metallic) * (1.0f - specularWeight);
    return clamp(specularWeight / max(specularWeight + diffuseWeight, 1e-4f), 0.1f, 0.95f);
}

float SpecularPdf(Surface s, float3 V, float3 L)
{
    // Visible-normal sampling: pdf(L) = G1(V) D(H) / (4 N.V).
    const float NdotV = max(dot(s.normal, V), 1e-4f);
    const float3 H = normalize(V + L);
    const float alpha = s.roughness * s.roughness;
    const float alpha2 = alpha * alpha;
    const float G1 = 1.0f / (1.0f + SmithLambda(NdotV, alpha2));
    return G1 * GgxD(saturate(dot(s.normal, H)), alpha2) / (4.0f * NdotV);
}

float CombinedPdf(Surface s, float3 V, float3 L, float pSpecular)
{
    const float NdotL = dot(s.normal, L);
    if (NdotL <= 0.0f)
        return 0.0f;
    return pSpecular * SpecularPdf(s, V, L) + (1.0f - pSpecular) * NdotL / kPi;
}

// Heitz 2018, "Sampling the GGX Distribution of Visible Normals". V in the local
// frame where the normal is +Z.
float3 SampleGgxVndf(float3 V, float alpha, float2 u)
{
    const float3 Vh = normalize(float3(alpha * V.x, alpha * V.y, V.z));
    const float lengthSq = Vh.x * Vh.x + Vh.y * Vh.y;
    const float3 T1 = lengthSq > 0.0f ? float3(-Vh.y, Vh.x, 0.0f) * rsqrt(lengthSq) : float3(1.0f, 0.0f, 0.0f);
    const float3 T2 = cross(Vh, T1);
    const float r = sqrt(u.x);
    const float phi = 2.0f * kPi * u.y;
    const float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    const float s = 0.5f * (1.0f + Vh.z);
    t2 = (1.0f - s) * sqrt(max(1.0f - t1 * t1, 0.0f)) + s * t2;
    const float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0f, 1.0f - t1 * t1 - t2 * t2)) * Vh;
    return normalize(float3(alpha * Nh.x, alpha * Nh.y, max(0.0f, Nh.z)));
}

// A direction from the specular lobe (visible normals) around s.normal.
float3 SampleSpecularDirection(Surface s, float3 V, inout uint rng)
{
    float3 t, b;
    BuildBasis(s.normal, t, b);
    const float3 localV = float3(dot(V, t), dot(V, b), dot(V, s.normal));
    const float alpha = s.roughness * s.roughness;
    const float3 H = SampleGgxVndf(localV, alpha, float2(Rand(rng), Rand(rng)));
    const float3 worldH = t * H.x + b * H.y + s.normal * H.z;
    return reflect(-V, worldH);
}

// A cosine-weighted direction around s.normal.
float3 SampleDiffuseDirection(Surface s, inout uint rng)
{
    const float r = sqrt(Rand(rng));
    const float phi = 2.0f * kPi * Rand(rng);
    float3 t, b;
    BuildBasis(s.normal, t, b);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + s.normal * sqrt(max(0.0f, 1.0f - r * r)));
}

// ─── Environment ─────────────────────────────────────────────────────────────
// Escaped rays. The sky above, without the sun disc (the sun is reached through
// next-event estimation only, so it is never counted twice); below the horizon a
// stand-in diffuse ground for the terrain the TLAS does not hold.
float3 EnvironmentRadiance(float3 dir)
{
    if (dir.z >= 0.0f)
        return PteroSkyRadiance(g_Sky, dir);

    // Raster units: a diffuse surface's radiance is albedo x (ambient + sun colour x cos).
    const float sunUp = saturate(-g_SunDir.z);
    return g_GroundAlbedo * (g_SkyAmbient + g_SunColor * sunUp);
}

// ─── Next-event estimation ───────────────────────────────────────────────────
float3 OffsetAlong(Surface s, float3 dir)
{
    // Away from the side the ray leaves through, scaled for float precision far out.
    const float3 side = dot(s.geoNormal, dir) >= 0.0f ? s.geoNormal : -s.geoNormal;
    const float epsilon = max(1e-3f, 1e-5f * length(s.position - g_CameraPos));
    return s.position + side * epsilon;
}

// Light arriving at s from the sun: a direction L inside its disc and, if that
// direction is unblocked and in front of the surface, its irradiance (pi x the
// raster colour). False when it contributes nothing.
bool SampleSunIncident(Surface s, inout uint rng, out float3 L, out float3 irradiance)
{
    L = -g_SunDir;
    irradiance = float3(0.0f, 0.0f, 0.0f);
    if (all(g_SunColor <= 0.0f))
        return false;

    if ((g_Flags & PT_FLAG_SOFT_SHADOWS) != 0u)
    {
        // Uniform over the disc's cone: every direction in it carries E / solid angle.
        const float cosTheta = 1.0f - Rand(rng) * (1.0f - g_SunCosAngularRadius);
        const float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
        const float phi = 2.0f * kPi * Rand(rng);
        float3 t, b;
        BuildBasis(L, t, b);
        L = normalize(t * (cos(phi) * sinTheta) + b * (sin(phi) * sinTheta) + L * cosTheta);
    }

    if (dot(s.normal, L) <= 0.0f || dot(s.geoNormal, L) <= 0.0f)
        return false;
    if (!TraceVisibility(OffsetAlong(s, L), L, kMaxRayT))
        return false;
    // The ripples and dunes of displaced terrain, which the traced mesh does not have.
    if (!TerrainShadowVisible(s, L, kMaxRayT, rng))
        return false;

    irradiance = kPi * g_SunColor;
    return true;
}

// The raster pass's falloff for `light` at `distance` from its emitting point.
float LightFalloff(PteroLightData light, float distance, float shapeMask)
{
    const float normalizedDistance = saturate(distance / max(light.Radius, 1e-4f));
    float rangeMask = saturate(1.0f - normalizedDistance * normalizedDistance);
    rangeMask *= rangeMask;
    return rangeMask
        * pow(PteroLightFalloffDistance(light, distance), -max(light.FalloffExponent, 0.001f))
        * shapeMask;
}

// The same for a scene light: a point on the emitter, the direction to it and the
// irradiance it delivers, after its shadow ray.
bool SampleLightIncident(PteroLightData light, Surface s, inout uint rng, out float3 L, out float3 irradiance)
{
    L = float3(0.0f, 0.0f, 1.0f);
    irradiance = float3(0.0f, 0.0f, 0.0f);

    const int lightType = (int)light.LightType;
    const bool soft = (g_Flags & PT_FLAG_SOFT_SHADOWS) != 0u;

    float3 samplePos;
    float  shapeMask;
    float  falloffDistance;

    if (lightType == PTERO_LIGHT_TYPE_RECT && soft)
    {
        // A point on the panel, so its shadows soften with the panel's size.
        const float3 rectUp = cross(light.RectRight, light.Direction);
        samplePos = light.Position
                  + light.RectRight * ((Rand(rng) * 2.0f - 1.0f) * light.RectHalfWidth)
                  + rectUp * ((Rand(rng) * 2.0f - 1.0f) * light.RectHalfHeight);
        const float3 towardSurface = s.position - samplePos;
        const float lengthSq = dot(towardSurface, towardSurface);
        if (lengthSq < 1e-8f)
            return false;
        const float emitCos = dot(light.Direction, towardSurface * rsqrt(lengthSq));
        shapeMask = light.RectTwoSided > 0.5f ? abs(emitCos) : saturate(emitCos);
        falloffDistance = sqrt(lengthSq);
    }
    else
    {
        // The raster pass's emitter: the shape resolved to one point, a spherical
        // source pulled toward the surface by its radius.
        const PteroResolvedLight shape = PteroResolveLightShape(light, s.position);
        shapeMask = shape.ShapeMask;
        samplePos = shape.Position;
        falloffDistance = length(s.position - samplePos);
        if (light.SourceRadius > 1e-4f && lightType != PTERO_LIGHT_TYPE_RECT)
        {
            falloffDistance = max(falloffDistance - min(light.SourceRadius, light.Radius * 0.5f), 0.0f);
            if (soft)
            {
                // Aim the shadow ray at a point across the source's disc.
                float3 t, b;
                BuildBasis(SafeNormalizeOr(s.position - samplePos, float3(0.0f, 0.0f, 1.0f)), t, b);
                const float r = light.SourceRadius * sqrt(Rand(rng));
                const float phi = 2.0f * kPi * Rand(rng);
                samplePos += (t * cos(phi) + b * sin(phi)) * r;
            }
        }
    }
    if (shapeMask <= 0.0f)
        return false;

    const float3 toLight = samplePos - s.position;
    const float distance = length(toLight);
    if (distance < 1e-4f)
        return false;
    L = toLight / distance;

    const float falloff = LightFalloff(light, falloffDistance, shapeMask);
    if (falloff <= 0.0f || dot(s.normal, L) <= 0.0f || dot(s.geoNormal, L) <= 0.0f)
        return false;

    // Lights the raster pass does not shadow stay unshadowed here too.
    if (light.CastShadows > 0.5f)
    {
        const float3 origin = OffsetAlong(s, L);
        const float maxDistance = length(samplePos - origin) * 0.999f - 1e-3f;
        if (!TraceVisibility(origin, L, maxDistance))
            return false;
        if (!TerrainShadowVisible(s, L, maxDistance, rng))
            return false;
    }

    irradiance = kPi * light.Color * falloff;
    return true;
}

// What a light would contribute to s without its shadow, from its emitting point
// alone: the weight a light is picked by when only one is traced.
float LightSelectionWeight(PteroLightData light, Surface s)
{
    const PteroResolvedLight shape = PteroResolveLightShape(light, s.position);
    if (shape.ShapeMask <= 0.0f)
        return 0.0f;
    const float3 toLight = shape.Position - s.position;
    const float distance = length(toLight);
    float falloffDistance = distance;
    if (light.SourceRadius > 1e-4f && (int)light.LightType != PTERO_LIGHT_TYPE_RECT)
        falloffDistance = max(distance - min(light.SourceRadius, light.Radius * 0.5f), 0.0f);
    const float falloff = LightFalloff(light, falloffDistance, shape.ShapeMask);
    // A sized light can reach a surface facing slightly away from its centre, so
    // the cosine is softened rather than cut at zero.
    const float facing = saturate(dot(s.normal, toLight / max(distance, 1e-4f)) * 0.5f + 0.5f);
    return Luminance(light.Color) * falloff * facing;
}

// Direct light at s, its two lobes apart: the sun, and every scene light - or, with
// PT_FLAG_ONE_LIGHT, one scene light picked by LightSelectionWeight and divided by
// the chance of picking it, which costs one shadow ray however many lights there are.
// The light reflected where it arrives; DirectLightingSplit adds subsurface scattering.
void DirectLightingSplitLocal(Surface s, float3 V, inout uint rng, out float3 diffuse, out float3 specular)
{
    diffuse = float3(0.0f, 0.0f, 0.0f);
    specular = float3(0.0f, 0.0f, 0.0f);

    float3 L, irradiance, d, sp;
    if (SampleSunIncident(s, rng, L, irradiance))
    {
        EvalBrdfCosSplit(s, V, L, d, sp);
        diffuse += d * irradiance;
        specular += sp * irradiance;
    }

    const uint lightCount = min(g_NumLights, (uint)PT_MAX_LIGHTS);
    if (lightCount == 0u)
        return;

    [branch]
    if ((g_Flags & PT_FLAG_ONE_LIGHT) != 0u)
    {
        float weights[PT_MAX_LIGHTS];
        float total = 0.0f;
        [loop]
        for (uint i = 0; i < lightCount; ++i)
        {
            weights[i] = LightSelectionWeight(g_Lights[i], s);
            total += weights[i];
        }
        if (total <= 0.0f)
            return;

        float pick = Rand(rng) * total;
        uint chosen = lightCount - 1u;
        [loop]
        for (uint j = 0; j < lightCount; ++j)
        {
            if (pick < weights[j]) { chosen = j; break; }
            pick -= weights[j];
        }
        const float probability = weights[chosen] / total;
        if (probability > 0.0f && SampleLightIncident(g_Lights[chosen], s, rng, L, irradiance))
        {
            EvalBrdfCosSplit(s, V, L, d, sp);
            diffuse += d * irradiance / probability;
            specular += sp * irradiance / probability;
        }
    }
    else
    {
        [loop]
        for (uint i = 0; i < lightCount; ++i)
        {
            if (SampleLightIncident(g_Lights[i], s, rng, L, irradiance))
            {
                EvalBrdfCosSplit(s, V, L, d, sp);
                diffuse += d * irradiance;
                specular += sp * irradiance;
            }
        }
    }
}

// ─── Subsurface scattering ───────────────────────────────────────────────────
// A diffusion-profile BSSRDF for materials with useSubsurfaceScattering
// (PT_FLAG_SUBSURFACE), the model of the raster pass's ray-traced mode
// (SubsurfaceScattering_RT.hlsl) so the two agree: the d'Eon/Jimenez separable-SSS
// profile (Subsurface.hlsli), importance sampled as a radius on the disc tangent to
// the surface and projected onto the real geometry with a pair of probe rays along
// the normal (Christensen and King). As in the raster pass only direct light is
// scattered: every next-event estimate at a subsurface vertex draws one disc point,
// lights it with its own shadow rays and puts that in place of the vertex's own
// direct diffuse, blended per channel by the strength (the SDK's kernel[0]).
// Specular stays where the light arrived. The sample is shaded with the receiver's
// albedo (post-scatter texturing, as the raster pass does, so the result still divides
// by the albedo for NRD in PathTracer_RealTime.hlsl) and its own smooth normal.
//
// Channels share one disc point, drawn from one channel's profile picked at random
// and weighted by the balance heuristic over the three (one-sample MIS), as in the
// raster pass; a weight is at most 3. The raster pass divides by the sum of its
// sample weights, which one sample cannot do, so a probe that misses the surface
// (past a mesh edge) leaves its disc weight on the vertex's own light instead: on a
// flat surface the weights then average exactly one, which is what the raster
// normalisation makes them, and energy is kept at edges.
//
// Transmission: light crossing the object, its thickness measured by a ray from
// inside the surface toward each light (the raster pass's TracedTransmission), is
// added to the diffuse.

// The five Gaussians of the reflectance profile; the SDK leaves out the narrowest
// (0.233 at variance 0.0064) as light the strength parameter keeps in place.
static const uint  kSssProfileTerms = 5u;
static const float kSssProfileVariance[5] = { 0.0484f, 0.187f, 0.567f, 1.99f, 7.41f };
static const float kSssProfileWeight[5]   = { 0.100f,  0.118f, 0.113f, 0.358f, 0.078f };
// Kernel range in profile millimetres; Surface::sssRadius is this many of them.
static const float kSssProfileRangeMm = 3.0f;

// Fraction of Gaussian term k (variance v, in falloff-scaled units) inside the range.
float SssTermMassInRange(uint k, float falloff)
{
    const float rangeScaled = kSssProfileRangeMm / (0.001f + falloff);
    return 1.0f - exp(-rangeScaled * rangeScaled / (2.0f * kSssProfileVariance[k]));
}

// Radial 2D density of one channel's profile at r (millimetres), truncated to the
// range and normalised to integrate to one over the disc.
float SssChannelDensity(float rMm, float falloff)
{
    if (rMm > kSssProfileRangeMm)
        return 0.0f;
    const float f = 0.001f + falloff;
    const float rr = rMm / f;
    float density = 0.0f;
    float mass = 0.0f;
    [unroll]
    for (uint k = 0u; k < kSssProfileTerms; ++k)
    {
        density += kSssProfileWeight[k] * exp(-rr * rr / (2.0f * kSssProfileVariance[k]))
                 / (2.0f * kPi * kSssProfileVariance[k]);
        mass += kSssProfileWeight[k] * SssTermMassInRange(k, falloff);
    }
    return density / (f * f * max(mass, 1e-8f));
}

float3 SssDensityRgb(float rMm, float3 falloff)
{
    return float3(SssChannelDensity(rMm, falloff.r), SssChannelDensity(rMm, falloff.g), SssChannelDensity(rMm, falloff.b));
}

// Draws a radius (millimetres) from one channel's truncated profile.
float SssSampleChannelRadius(float falloff, float u0, float u1)
{
    float mass = 0.0f;
    [unroll]
    for (uint k = 0u; k < kSssProfileTerms; ++k)
        mass += kSssProfileWeight[k] * SssTermMassInRange(k, falloff);

    // A Gaussian term in proportion to its mass inside the range.
    float target = u0 * mass;
    uint term = kSssProfileTerms - 1u;
    [unroll]
    for (uint j = 0u; j < kSssProfileTerms; ++j)
    {
        const float termMass = kSssProfileWeight[j] * SssTermMassInRange(j, falloff);
        if (target < termMass)
        {
            term = j;
            break;
        }
        target -= termMass;
    }

    // Truncated 2D Gaussian: invert its radial CDF over [0, range].
    const float truncatedU = u1 * SssTermMassInRange(term, falloff);
    const float rr = sqrt(-2.0f * kSssProfileVariance[term] * log(max(1.0f - truncatedU, 1e-7f)));
    return min(rr * (0.001f + falloff), kSssProfileRangeMm);
}

// The closest surface along a probe ray: the point lifted onto the smooth surface
// (Hanika, as in ResolveSurface), its face normal and its interpolated vertex normal,
// both turned to referenceNormal's side.
bool SssProbeRay(float3 origin, float3 dir, float tMax, float3 referenceNormal,
                 out float3 position, out float3 geoNormal, out float3 smoothNormal)
{
    position = origin;
    geoNormal = referenceNormal;
    smoothNormal = referenceNormal;
    const HitRecord rec = TraceClosest(origin, dir, 0.0f, tMax);
    if (!rec.hit)
        return false;

    const GpuInstanceInfo info = t_InstanceInfo[rec.instance];
    const float3 w = float3(1.0f - rec.bary.x - rec.bary.y, rec.bary.x, rec.bary.y);
    float3 vertexPos[3];
    float3 vertexNrm[3];
    [unroll]
    for (uint k = 0; k < 3; ++k)
    {
        const GpuPackedVertex v = t_Vertices[info.vertexOffset + t_Indices[info.indexOffset + rec.primitive * 3 + k]];
        vertexPos[k] = PteroObjectToWorldPoint(rec.objectToWorld, float3(v.px, v.py, v.pz));
        vertexNrm[k] = PteroObjectToWorldNormal(rec.objectToWorld, float3(v.nx, v.ny, v.nz));
    }
    geoNormal = SafeNormalizeOr(cross(vertexPos[1] - vertexPos[0], vertexPos[2] - vertexPos[0]), referenceNormal);
    if (dot(geoNormal, referenceNormal) < 0.0f)
        geoNormal = -geoNormal;

    const float3 hitPos = origin + dir * rec.t;
    float3 lifted = hitPos;
    float3 interpolated = float3(0.0f, 0.0f, 0.0f);
    [unroll]
    for (uint j = 0; j < 3; ++j)
    {
        float3 n = vertexNrm[j];
        const float lengthSq = dot(n, n);
        if (lengthSq < 1e-12f)
            continue;
        n *= rsqrt(lengthSq);
        if (dot(n, geoNormal) < 0.0f)
            n = -n;
        lifted -= w[j] * min(0.0f, dot(hitPos - vertexPos[j], n)) * n;
        interpolated += w[j] * n;
    }
    position = lifted;
    smoothNormal = SafeNormalizeOr(interpolated, geoNormal);
    return true;
}

// The real surface under a point on the tangent disc: probe down the normal first,
// then up, so the disc point may sit just above or just below a curved surface.
bool SssProbeSurface(float3 discPoint, float3 N, float reach, out float3 position, out float3 geoNormal,
                     out float3 smoothNormal)
{
    const float epsilon = max(reach * 0.02f, 1e-4f);
    if (SssProbeRay(discPoint + N * epsilon, -N, reach + epsilon, N, position, geoNormal, smoothNormal))
        return true;
    return SssProbeRay(discPoint - N * epsilon, N, reach + epsilon, N, position, geoNormal, smoothNormal);
}

// The vertex's direct diffuse with its light scattered under the surface. `localDiffuse`
// is the diffuse DirectLightingSplitLocal found at the vertex itself.
float3 SubsurfaceDirectDiffuse(Surface s, float3 V, float3 localDiffuse, inout uint rng)
{
    const float metresPerMm = s.sssRadius / kSssProfileRangeMm;
    const uint channel = min((uint)(Rand(rng) * 3.0f), 2u);
    const float rMm = SssSampleChannelRadius(s.sssFalloff[channel], Rand(rng), Rand(rng));
    const float phi = 2.0f * kPi * Rand(rng);

    // Density of the disc point (per disc mm^2): the mixture of the three channels.
    const float3 discDensity = SssDensityRgb(rMm, s.sssFalloff);
    const float mixturePdf = (discDensity.r + discDensity.g + discDensity.b) / 3.0f;
    if (mixturePdf <= 0.0f)
        return localDiffuse;

    float3 t, b;
    BuildBasis(s.normal, t, b);
    const float r = rMm * metresPerMm;
    const float3 discPoint = s.position + (t * cos(phi) + b * sin(phi)) * r;

    float3 scattered;
    float3 entryPosition, entryGeoNormal, entryNormal;
    if (SssProbeSurface(discPoint, s.normal, max(r * 1.5f, 0.002f), entryPosition, entryGeoNormal, entryNormal))
    {
        // The profile over the distance actually travelled, as the raster pass weighs it.
        const float distanceMm = min(length(entryPosition - s.position) / max(metresPerMm, 1e-8f), kSssProfileRangeMm);
        const float3 weight = SssDensityRgb(distanceMm, s.sssFalloff) / mixturePdf;

        // The light entering there, shaded with this vertex's albedo and the surface's own
        // orientation at the entry point - its interpolated vertex normal, without the
        // normal map. Scattering is what softens pore-scale detail; lit with the receiver's
        // normal-mapped normal, every pore kept its own N.L through the scatter, and
        // flipping the map's green channel changed the scattered light completely. The
        // unscattered share (1 - strength) keeps the normal map.
        Surface entry = s;
        entry.position = entryPosition;
        entry.geoNormal = entryGeoNormal;
        entry.normal = entryNormal;
        entry.terrainTrace = kInvalidIndex;
        entry.displacement = 0.0f;
        float3 entryDiffuse, entrySpecular;
        DirectLightingSplitLocal(entry, V, rng, entryDiffuse, entrySpecular);
        scattered = entryDiffuse * weight;
    }
    else
    {
        // Past an edge: the disc point's share stays with the light that arrived here.
        scattered = localDiffuse * (discDensity / mixturePdf);
    }
    return lerp(localDiffuse, scattered, s.sssStrength);
}

// Transmittance through `thickness` metres of the medium: Subsurface.hlsli's
// PteroSssTransmittanceProfile for this vertex's profile.
float3 SssTransmittance(Surface s, float thickness)
{
    const float scale = 8.25f * (1.0f - min(s.sssTranslucency, 0.995f)) / max(s.sssRadius, 1e-5f);
    const float3 d = (scale * thickness) / (0.001f + s.sssFalloff);
    const float3 dd = -d * d;
    return 0.233f * exp(dd / 0.0064f)
         + 0.100f * exp(dd / 0.0484f)
         + 0.118f * exp(dd / 0.187f)
         + 0.113f * exp(dd / 0.567f)
         + 0.358f * exp(dd / 1.99f)
         + 0.078f * exp(dd / 7.41f);
}

// Thickness past which no channel transmits anything (PteroSssMaxTransmissionThickness).
float SssMaxTransmissionThickness(Surface s)
{
    if (s.sssTranslucency <= 0.0f)
        return 0.0f;
    const float scale = 8.25f * (1.0f - min(s.sssTranslucency, 0.995f)) / max(s.sssRadius, 1e-5f);
    const float maxFalloff = max(s.sssFalloff.r, max(s.sssFalloff.g, s.sssFalloff.b));
    return 5.7f * (0.001f + maxFalloff) / scale;
}

// Light crossing the object from a light in direction L, `maxDistance` away, whose
// radiance in raster units (colour x falloff) is `radiance`. The raster pass's
// TracedTransmission: the thickness is measured by a ray from well inside the surface
// (a millimetre-scale start caught the next facet of the same side), a ray that
// leaves without hitting anything is an open sheet of zero thickness, and anything
// past the far side can still block the light. Raster units come out as radiance
// here directly: the raster pixel is albedo x colour, and so is the path tracer's.
float3 SssTracedTransmission(Surface s, float3 L, float maxDistance, float3 radiance, float maxThickness)
{
    // Light from behind only (PteroSssTransmission in Subsurface.hlsli), along the face,
    // not the normal map (PteroSssDepthNormal).
    if (dot(L, s.geoNormal) >= 0.0f)
        return float3(0.0f, 0.0f, 0.0f);

    const float startDepth = max(0.0015f, 0.1f * maxThickness);
    const float3 inside = s.position - s.geoNormal * startDepth;
    float thickness = 0.0f;
    const HitRecord rec = TraceClosest(inside, L, 0.0f, maxDistance);
    if (rec.hit)
    {
        thickness = rec.t + startDepth;
        if (thickness > maxThickness)
            return float3(0.0f, 0.0f, 0.0f);
        const float remaining = maxDistance - rec.t;
        if (remaining > 0.002f && !TraceVisibility(inside + L * (rec.t + 0.002f), L, remaining))
            return float3(0.0f, 0.0f, 0.0f);
    }
    return SssTransmittance(s, thickness) * saturate(dot(L, -s.geoNormal))
         * radiance * s.albedo * g_SssTransmission;
}

// Transmitted light from the sun and every scene light in reach (the raster pass's
// PixelTransmission), pre-exposed like the rest.
float3 SubsurfaceTransmission(Surface s)
{
    const float maxThickness = SssMaxTransmissionThickness(s);
    if (g_SssTransmission <= 0.0f || maxThickness <= 0.0f)
        return float3(0.0f, 0.0f, 0.0f);

    float3 transmitted = float3(0.0f, 0.0f, 0.0f);
    if (any(g_SunColor > 0.0f))
        transmitted += SssTracedTransmission(s, -g_SunDir, kMaxRayT, g_SunColor, maxThickness);

    const uint lightCount = min(g_NumLights, (uint)PT_MAX_LIGHTS);
    [loop]
    for (uint i = 0; i < lightCount; ++i)
    {
        const PteroLightData light = g_Lights[i];
        const PteroResolvedLight shape = PteroResolveLightShape(light, s.position);
        const float3 toLight = shape.Position - s.position;
        const float distance = max(length(toLight), 1e-4f);
        if (distance >= light.Radius || shape.ShapeMask <= 0.0f)
            continue;
        const float falloff = LightFalloff(light, distance, shape.ShapeMask);
        if (falloff <= 0.0f)
            continue;
        transmitted += SssTracedTransmission(s, toLight / distance, distance, light.Color * falloff, maxThickness);
    }
    return transmitted;
}

// Direct light at s, its two lobes apart, with a subsurface material's diffuse
// scattered and its transmission added. Everything else is DirectLightingSplitLocal
// unchanged, random numbers included.
void DirectLightingSplit(Surface s, float3 V, inout uint rng, out float3 diffuse, out float3 specular)
{
    DirectLightingSplitLocal(s, V, rng, diffuse, specular);
    [branch]
    if (s.sssRadius > 0.0f)
    {
        diffuse = SubsurfaceDirectDiffuse(s, V, diffuse, rng);
        diffuse += SubsurfaceTransmission(s);
    }
}

// The subsurface "Profile Mask" debug view (PT_FLAG_SSS_MASK). The raster pass draws it,
// but the path tracer replaces the whole image after that pass, so it draws its own from
// what it traces: the colour of the pixel's profile slot (SubsurfaceScattering.hlsl's
// SlotDebugColor) where the path tracer scatters the surface, black everywhere else.
float3 SubsurfaceMaskColor(Surface s, float packedSpecular)
{
    if (s.sssRadius <= 0.0f)
        return float3(0.0f, 0.0f, 0.0f);
    const uint slot = max(PteroDecodeSubsurfaceSlot(packedSpecular), 1u);
    const float hue = frac(slot * 0.618034f);
    return saturate(abs(frac(hue + float3(0.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - 3.0f) - 1.0f);
}

float3 DirectLighting(Surface s, float3 V, inout uint rng)
{
    float3 diffuse, specular;
    DirectLightingSplit(s, V, rng, diffuse, specular);
    return diffuse + specular;
}

float3 ClampIndirect(float3 contribution)
{
    if (g_IndirectClamp <= 0.0f)
        return contribution;
    const float luminance = Luminance(contribution);
    return luminance > g_IndirectClamp ? contribution * (g_IndirectClamp / luminance) : contribution;
}

// ─── Screen radiance cache (real time) ───────────────────────────────────────
// Where a path's last vertex was on screen last frame, at the depth it is at now,
// last frame's denoised diffuse lighting there already holds every bounce that
// frame had - including the ones it got from the frame before. Ending the path in
// it instead of in that vertex's direct light alone carries the light on without
// another ray; over a few frames it converges to unlimited bounces. Interiors
// that are lit only by light bouncing in from elsewhere are the ones that need it.
bool LookupScreenCache(float3 position, out float3 diffuseLighting)
{
    diffuseLighting = float3(0.0f, 0.0f, 0.0f);
#ifndef PT_SCREEN_CACHE
    return false;
#else
    const float4 clip = mul(float4(position, 1.0f), g_PrevViewProj);
    if (clip.w <= 1e-4f)
        return false;
    float2 uv = clip.xy / clip.w * 0.5f + 0.5f;
    uv.y = 1.0f - uv.y;
    if (any(uv <= 0.0f) || any(uv >= 1.0f))
        return false;

    const int2 pixel = int2(uv * float2(g_Width, g_Height));
    const float cachedViewZ = t_PrevViewZ.Load(int3(pixel, 0));
    // Perspective: clip.w is the view depth. A mismatch means something else was in
    // front of the point last frame (or it is the sky).
    if (cachedViewZ >= 1e5f || abs(cachedViewZ - clip.w) > 0.03f * clip.w + 0.02f)
        return false;

    diffuseLighting = t_PrevDiffuseLighting.Load(int3(pixel, 0)).rgb * g_ScreenCacheScale;
    return all(diffuseLighting >= 0.0f) && all(diffuseLighting < 1e30f);
#endif
}

// ─── The G-Buffer surface ───────────────────────────────────────────────────
float3 DecodeGBufferNormal(float2 encoded)
{
    const float2 octN = encoded * 2.0f - 1.0f;
    const float octZ = 1.0f - abs(octN.x) - abs(octN.y);
    const float2 wrapped = (1.0f - abs(octN.yx)) * (float2(octN.xy >= 0.0f) * 2.0f - 1.0f);
    return normalize(float3(octZ < 0.0f ? wrapped : octN.xy, octZ));
}

float3 ReconstructGBufferPosition(uint2 pixel, float depth)
{
    float2 ndc = (float2(pixel) + 0.5f) / float2(g_Width, g_Height) * 2.0f - 1.0f;
    ndc.y = -ndc.y;
    const float4 world = mul(float4(ndc, depth, 1.0f), g_ViewProjInvJittered);
    return world.xyz / world.w;
}

// For a hit on terrain, how far the drawn (tessellated, displaced) surface can
// stand off the traced mesh, in metres; negative for anything else. A G-Buffer
// point on displaced terrain is not on the traced triangles, so every match
// between the two has to allow this much.
float TerrainReach(HitRecord rec)
{
    const uint rangeIdx = FindMaterialRange(t_InstanceInfo[rec.instance], rec.primitive);
    if (rangeIdx == kInvalidIndex)
        return -1.0f;
    const GpuMaterialRange range = t_MaterialRanges[rangeIdx];
    return (range.flags & kMaterialTerrain) != 0u ? max(range.terrainReach, 0.0f) : -1.0f;
}

// The hit triangle's face normal, either side.
float3 HitFaceNormal(HitRecord rec)
{
    const GpuInstanceInfo info = t_InstanceInfo[rec.instance];
    float3 p[3];
    [unroll]
    for (uint k = 0; k < 3; ++k)
    {
        const GpuPackedVertex v = t_Vertices[info.vertexOffset + t_Indices[info.indexOffset + rec.primitive * 3 + k]];
        p[k] = PteroObjectToWorldPoint(rec.objectToWorld, float3(v.px, v.py, v.pz));
    }
    return SafeNormalizeOr(cross(p[1] - p[0], p[2] - p[0]), float3(0.0f, 0.0f, 1.0f));
}

// The reference pass's camera hits on terrain. The traced terrain carries only a
// baked, splat-blended albedo - its real material is tiled layers blended per
// pixel, which only the raster pass draws. Where the camera ray lands on terrain
// at the point the G-Buffer pixel shows, that pixel's material stands in, so the
// terrain is path traced with every map it is drawn with. Bounces keep the bake.
#ifdef PT_GBUFFER_TERRAIN
Texture2D<float4> t_GBufferAlbedo   : register(t39);
Texture2D<float4> t_GBufferNormal   : register(t40);   // xy oct normal, z device depth, w specular/flags
Texture2D<float4> t_GBufferMaterial : register(t41);   // r roughness, g metallic
static uint2 g_PrimaryPixel;   // set by the pass before it traces

void ApplyGBufferTerrain(HitRecord rec, inout Surface s)
{
    const float reach = TerrainReach(rec);
    if (reach < 0.0f)
        return;
    const float4 normalSample = t_GBufferNormal.Load(int3(g_PrimaryPixel, 0));
    const float depth = normalSample.z;
    if (depth <= 0.0f || depth >= 1.0f)
        return;
    // The ray's sub-pixel offset and the G-Buffer's TAA jitter differ by under a
    // pixel, and displacement moves the drawn surface by up to its reach; anything
    // further apart is not the same surface. The hit stays on the traced mesh, so
    // the path's rays leave from the geometry they are traced against.
    const float3 gbufferPos = ReconstructGBufferPosition(g_PrimaryPixel, depth);
    const float3 viewDirection = SafeNormalizeOr(s.position - g_CameraPos, float3(0.0f, 0.0f, -1.0f));
    if (length(gbufferPos - s.position) > max(0.05f, rec.t * 0.03f)
                                          + PteroTerrainReachAlongRay(reach, s.geoNormal, viewDirection))
        return;

    const float4 material = t_GBufferMaterial.Load(int3(g_PrimaryPixel, 0));
    s.albedo = saturate(t_GBufferAlbedo.Load(int3(g_PrimaryPixel, 0)).rgb);
    s.roughness = clamp(material.r, 0.04f, 1.0f);
    s.metallic = saturate(material.g);
    s.specular = PteroDecodeSurfaceSpecular(normalSample.w);
    float3 normal = DecodeGBufferNormal(normalSample.xy);
    // As for normal maps at traced hits: never lit from inside the face.
    if (dot(normal, s.geoNormal) < 0.05f)
        normal = SafeNormalizeOr(normal + s.geoNormal * (0.05f - dot(normal, s.geoNormal)), s.normal);
    s.normal = normal;
}
#endif

// ─── Paths ───────────────────────────────────────────────────────────────────
// The radiance arriving at `origin` along `dir`, from a path whose first segment
// is bounce `firstBounce` (0: a camera ray; 1: leaving the first surface). Light
// found after bounce 0 is indirect and clamped. `firstHit` says whether the first
// segment struck the TLAS, `firstHitT` how far it went.
float3 TracePathFrom(float3 origin, float3 dir, uint firstBounce, inout uint rng,
                     out bool firstHit, out float firstHitT)
{
    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    float3 throughput = float3(1.0f, 1.0f, 1.0f);
    firstHit = false;
    firstHitT = kMaxRayT;
    float pathLength = 0.0f;

    [loop]
    for (uint bounce = firstBounce; bounce <= g_MaxBounces; ++bounce)
    {
        const HitRecord rec = TraceClosest(origin, dir, 0.0f, kMaxRayT);
        if (!rec.hit)
        {
            if (bounce > 0)
                radiance += ClampIndirect(throughput * EnvironmentRadiance(dir));
            else
                radiance += EnvironmentRadiance(dir);
            break;
        }
        if (bounce == firstBounce)
        {
            firstHit = true;
            firstHitT = rec.t;
        }

        pathLength += rec.t;
        // Sharp at the camera, blurrier as paths spread.
        const float textureLod = bounce == 0 ? 0.0f : clamp(log2(max(pathLength * 0.25f, 1e-3f)), 0.0f, 6.0f);
        Surface s = ResolveSurface(rec, origin, dir, textureLod);
#ifdef PT_GBUFFER_TERRAIN
        if (bounce == 0)
            ApplyGBufferTerrain(rec, s);
#endif
        const float3 V = -dir;

        // Emission is only found by hitting it; nothing samples it as a light.
        const float3 emitted = s.emissive * g_EmissiveScale;
        if (any(emitted > 0.0f))
            radiance += bounce > 0 ? ClampIndirect(throughput * emitted) : emitted;

        // The last vertex of a real-time path: last frame's lighting there, if it was
        // on screen, stands in for its direct light and everything beyond it.
        float3 cachedLighting;
        if (bounce > 0 && bounce == g_MaxBounces && (g_Flags & PT_FLAG_SCREEN_CACHE) != 0u
            && LookupScreenCache(s.position, cachedLighting))
        {
            radiance += ClampIndirect(throughput * s.albedo * (1.0f - s.metallic) * cachedLighting);
            break;
        }

        const float3 direct = throughput * DirectLighting(s, V, rng);
        radiance += bounce > 0 ? ClampIndirect(direct) : direct;

        if (bounce == g_MaxBounces)
            break;

        // Next direction: one lobe, weighted by the pdf of both.
        const float pSpecular = SpecularProbability(s, V);
        const float3 L = Rand(rng) < pSpecular ? SampleSpecularDirection(s, V, rng) : SampleDiffuseDirection(s, rng);

        // A direction below the actual triangle would only find this surface again.
        if (dot(L, s.geoNormal) <= 0.0f)
            break;

        const float pdf = CombinedPdf(s, V, L, pSpecular);
        if (pdf <= 1e-8f)
            break;
        throughput *= EvalBrdfCos(s, V, L) / pdf;

        // Russian roulette once the path has had a few bounces to matter.
        if (bounce >= 3)
        {
            const float survive = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05f, 0.95f);
            if (Rand(rng) > survive)
                break;
            throughput /= survive;
        }

        origin = OffsetAlong(s, L);
        dir = L;
        if (!IsFinitePosition(origin) || !IsFinitePosition(dir) || all(throughput <= 0.0f))
            break;
    }

    return SanitizeRadiance(radiance);
}

// A full path from the camera; `cameraHit` says whether it struck the TLAS.
float3 TracePath(float3 origin, float3 dir, inout uint rng, out bool cameraHit)
{
    float unusedT;
    return TracePathFrom(origin, dir, 0u, rng, cameraHit, unusedT);
}

float3 CameraRayDirection(float2 pixelPos)
{
    float2 ndc = pixelPos / float2(g_Width, g_Height) * 2.0f - 1.0f;
    ndc.y = -ndc.y;
    // Any depth on the pixel's line lies on the camera ray.
    const float4 world = mul(float4(ndc, 0.5f, 1.0f), g_ViewProjInv);
    return SafeNormalizeOr(world.xyz / world.w - g_CameraPos, float3(1.0f, 0.0f, 0.0f));
}

// ─── The G-Buffer surface (real time) ────────────────────────────────────────
// Karis' fit of the GGX split-sum DFG term: the fraction of light the specular lobe
// reflects, averaged over the lobe. What NRD's specular signal is divided by and
// multiplied back with, so texture and Fresnel detail never go through the filter.
float3 EnvironmentBrdfApprox(float3 F0, float roughness, float NdotV)
{
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28f * NdotV)) * r.x + r.y;
    const float2 AB = float2(-1.04f, 1.04f) * a004 + r.zw;
    return F0 * AB.x + AB.y;
}

// Glass repacks the material target (DeferredLighting.hlsl) and is shaded by its
// own passes; the path tracer leaves those pixels to them.
bool IsGlassMaterial(float4 material)
{
    return material.a > (1.0f / 255.0f) && material.b < 0.75f;
}

#endif // PTERO_PATH_TRACER_COMMON_HLSLI
