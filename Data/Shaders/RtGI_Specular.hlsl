// RtGI_Specular.hlsl  –  cs_6_5
// Ray-traced GGX specular reflections pass.
//
// For each G-Buffer pixel whose roughness is below the threshold:
//   1. Read world-position, normal, roughness/metallic from G-Buffer.
//   2. Importance-sample a GGX microfacet normal and reflect the view vector.
//   3. Trace a reflection ray via inline RayQuery.
//   4. On hit, shade the secondary surface (diffuse + sun NEE).
//   5. On miss, return sky radiance.
//   6. Apply the GGX specular weight (Fresnel × G / (4 × NdotV)).
//   7. Write the result to the specular output buffer for compositing in the deferred pass.

#include "RtGI_Common.hlsli"

// ─── Additional bindings beyond the base 5 root parameters ───────────────────
// These match the extended root signature used by the specular pass (params 1-13).
RaytracingAccelerationStructure t_SpecTLAS     : register(t2);   // shared TLAS

struct GpuPackedVertex { float px, py, pz, nx, ny, nz, u, v; };
struct GpuInstanceInfo { uint vertexOffset, indexOffset, vertexCount, indexCount, materialRangeOffset, materialRangeCount, _pad0, _pad1; };
struct GpuMaterialRange
{
    uint  startPrimitive;
    uint  primitiveCount;
    float baseColorR;
    float baseColorG;
    float baseColorB;
    float baseColorA;
    float opacityFactor;
    float alphaCutoff;
    uint  baseColorTextureIndex;
    uint  opacityTextureIndex;
    uint  flags;
    float _pad0;
};

StructuredBuffer<GpuPackedVertex> t_SpecVertices    : register(t3);
StructuredBuffer<uint>            t_SpecIndices      : register(t4);
StructuredBuffer<GpuInstanceInfo> t_SpecInstanceInfo : register(t5);
StructuredBuffer<GpuMaterialRange> t_SpecMaterialRanges : register(t7);
Texture2D<float4>                 t_SpecBaseTextures[32] : register(t8);

// ─── G-Buffer inputs ──────────────────────────────────────────────────────────
Texture2D<float4> t_Albedo      : register(t0);   // RGB albedo / A=unused
Texture2D<float4> t_NormalDepth : register(t1);   // xy: oct normal, z: depth

// Additional material G-Buffer (roughness/metallic/AO), bound at t6.
Texture2D<float4> t_Material    : register(t6);   // R=roughness, G=metallic, B=AO

// ─── Output specular buffer (RGBA16F) ─────────────────────────────────────────
RWTexture2D<float4> u_SpecularOutput : register(u0);
// (u1 is unused in this pass; declare as dummy to match root sig)
RWTexture2D<float4> u_Dummy          : register(u1);

SamplerState gLinearWrapSampler : register(s0);

static const uint INVALID_TEXTURE_INDEX = 0xffffffffu;

// ─────────────────────────────────────────────────────────────────────────────
// GGX importance sampling helpers
// ─────────────────────────────────────────────────────────────────────────────

// Smith G term for GGX (Schlick approximation).
float G_Smith(float NdotV, float NdotL, float roughness)
{
    float k = (roughness + 1.0f) * (roughness + 1.0f) / 8.0f;
    float gV = NdotV / (NdotV * (1.0f - k) + k);
    float gL = NdotL / (NdotL * (1.0f - k) + k);
    return gV * gL;
}

// Schlick Fresnel.
float3 F_Schlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

// GGX NDF.
float D_GGX(float NdotH, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / (3.14159265f * d * d);
}

// Sample a GGX microfacet half-vector H in the local frame of N.
// Returns the reflected ray direction.
float3 SampleGGX(float2 xi, float roughness, float3 N, float3 V)
{
    float a  = roughness * roughness;
    float phi     = 6.28318530f * xi.x;
    float cosTheta = sqrt((1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
    float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));

    float3 H_local = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

    float3 up    = abs(N.z) < 0.999f ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 right = normalize(cross(up, N));
    float3 fwd   = cross(N, right);

    float3 H = normalize(H_local.x * right + H_local.y * fwd + H_local.z * N);
    return reflect(-V, H);
}

// ─────────────────────────────────────────────────────────────────────────────
// Shadow ray (same as RTGI).
// ─────────────────────────────────────────────────────────────────────────────
bool TraceShadowRay(float3 origin, float3 dir, float maxDist)
{
    RayDesc ray;
    ray.Origin    = origin;
    ray.Direction = dir;
    ray.TMin      = 0.0f;
    ray.TMax      = maxDist;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_BACK_FACING_TRIANGLES | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
    rq.TraceRayInline(t_SpecTLAS,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_BACK_FACING_TRIANGLES | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES,
        0xFF, ray);
    while (rq.Proceed()) {}
    return rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

// ─────────────────────────────────────────────────────────────────────────────
// Shade a specular secondary hit (same diffuse model as RTGI secondary).
// ─────────────────────────────────────────────────────────────────────────────
float3 ShadeSpecHit(float3 hitPos, float3 hitNormal, float3 geoNormal, float3 albedo)
{
    const float3 sunDir   = -g_SunDir;
    float sunNdotL  = saturate(dot(hitNormal, sunDir));
    float geoNdotL  = saturate(dot(geoNormal, sunDir));
    float sunVis = 1.0f;

    if (g_NextEventEstimation != 0 && sunNdotL > 0.0f && geoNdotL > 0.0f)
    {
        float bias = max(0.01f, 0.02f * rsqrt(max(geoNdotL, 0.05f)));
        float3 org = hitPos + geoNormal * bias + sunDir * bias;
        sunVis     = TraceShadowRay(org, sunDir, max(1e4f - bias * 2.0f, 1.0f)) ? 1.0f : 0.0f;
        sunVis     = lerp(0.35f, 1.0f, sunVis);
    }

    float3 direct = albedo * g_SunColor * sunNdotL * sunVis;
    float  sky    = saturate(dot(hitNormal, float3(0, 0, 1)) * 0.5f + 0.5f);
    float3 ambient = albedo * g_SkyColor * sky * 0.3f;
    return direct + ambient;
}

GpuMaterialRange ResolveMaterialRange(GpuInstanceInfo info, uint primIdx)
{
    [loop]
    for (uint r = 0; r < info.materialRangeCount; ++r)
    {
        GpuMaterialRange range = t_SpecMaterialRanges[info.materialRangeOffset + r];
        if (primIdx >= range.startPrimitive && primIdx < range.startPrimitive + range.primitiveCount)
            return range;
    }

    GpuMaterialRange fallback = (GpuMaterialRange)0;
    fallback.baseColorR = 1.0f;
    fallback.baseColorG = 1.0f;
    fallback.baseColorB = 1.0f;
    return fallback;
}

float ComputeSpecularTextureLod(float hitDist, float roughness)
{
    const float footprint = max(hitDist * max(roughness, 0.02f), 1e-3f);
    return saturate(log2(footprint * 64.0f));
}

float4 SampleMaterialTexture(uint textureIndex, float2 uv, float lod, float4 fallbackValue)
{
    if (textureIndex == INVALID_TEXTURE_INDEX)
        return fallbackValue;

    return t_SpecBaseTextures[NonUniformResourceIndex(textureIndex)].SampleLevel(gLinearWrapSampler, uv, lod);
}

// ─────────────────────────────────────────────────────────────────────────────
// Sky radiance for reflection rays that miss all geometry.
// ─────────────────────────────────────────────────────────────────────────────
float3 SkyRadiance(float3 dir)
{
    float t = saturate(dir.z);
    return lerp(g_SkyColor * 0.7f, g_SkyColor, t);
}

uint HashUint(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

uint InitStableSpecularRng(uint2 pixel, float3 worldPos)
{
    const int3 cell = int3(floor(worldPos * 32.0f));
    uint seed = pixel.x * 1973u + pixel.y * 9277u + 0x68bc21ebu;
    seed ^= HashUint(asuint(cell.x));
    seed ^= HashUint(asuint(cell.y) + 0x9e3779b9u);
    seed ^= HashUint(asuint(cell.z) + 0x85ebca6bu);
    return seed | 1u;
}

float ComputeReflectionRayBias(float3 worldPos, float3 N, float3 reflDir, float roughness)
{
    const float viewDistance = distance(g_CameraPos, worldPos);
    const float grazingScale = rsqrt(max(abs(dot(N, reflDir)), 0.2f));
    const float distanceBias = viewDistance * 0.00003f;
    const float roughnessBias = roughness * 0.0015f;
    return min(max(0.002f, distanceBias + roughnessBias) * grazingScale, 0.01f);
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────
[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 pixel = DTid.xy;
    if (pixel.x >= g_FrameWidth || pixel.y >= g_FrameHeight)
        return;

    // ── Sample G-Buffer ──────────────────────────────────────────────────────
    float4 normalDepthSample = t_NormalDepth.Load(int3(pixel, 0));
    float  depth             = normalDepthSample.z;

    // Sky pixels → write zero specular and exit.
    if (depth <= 0.0f || depth >= 1.0f)
    {
        u_SpecularOutput[pixel] = float4(0, 0, 0, 0);
        return;
    }

    float4 materialSample = t_Material.Load(int3(pixel, 0));
    float roughness = saturate(materialSample.r);
    float metallic  = materialSample.g;

    // Surfaces rougher than the threshold skip dedicated specular rays.
    if (roughness > saturate(g_SpecularRoughnessThreshold))
    {
        u_SpecularOutput[pixel] = float4(0, 0, 0, 0);
        return;
    }

    // Decode normal.
    float2 octN = normalDepthSample.xy * 2.0f - 1.0f;
    float  octZ = 1.0f - abs(octN.x) - abs(octN.y);
    float2 wrapped = (1.0f - abs(octN.yx)) * (float2(octN.xy >= 0.0f) * 2.0f - 1.0f);
    float3 N = normalize(float3(octZ < 0.0f ? wrapped : octN.xy, octZ));

    float3 worldPos = ReconstructWorldPos(pixel, depth);
    float3 V        = normalize(g_CameraPos - worldPos);

    // ── Sample GGX direction ─────────────────────────────────────────────────
    uint   rng    = InitStableSpecularRng(pixel, worldPos);
    float2 xi     = float2(RandFloat(rng), RandFloat(rng));
    float  clampedRoughness = max(roughness, 0.04f);  // prevent singularities
    float3 mirrorDir = reflect(-V, N);
    float3 ggxDir = SampleGGX(xi, clampedRoughness, N, V);
    const float viewDistance = distance(g_CameraPos, worldPos);
    const float distanceStability = saturate((viewDistance - 2.0f) / 12.0f);
    float  ggxBlend = saturate((clampedRoughness - 0.04f) / 0.20f) * (1.0f - distanceStability * 0.85f);
    float3 reflDir = normalize(lerp(mirrorDir, ggxDir, ggxBlend));

    // If the reflection direction is below the surface, clamp to avoid black artifacts.
    float NdotL = dot(N, reflDir);
    if (NdotL <= 0.0f)
    {
        u_SpecularOutput[pixel] = float4(0, 0, 0, 0);
        return;
    }

    float NdotV = max(dot(N, V), 0.0001f);

    // ── Trace reflection ray ─────────────────────────────────────────────────
    const float rayBias = ComputeReflectionRayBias(worldPos, N, reflDir, clampedRoughness);

    RayDesc ray;
    ray.Origin    = worldPos + N * rayBias + reflDir * (rayBias * 0.5f);
    ray.Direction = reflDir;
    ray.TMin      = 0.001f;
    ray.TMax      = 1e4f;

    RayQuery<RAY_FLAG_CULL_BACK_FACING_TRIANGLES> rq;
    rq.TraceRayInline(t_SpecTLAS, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, ray);
    while (rq.Proceed()) {}

    float3 incomingRadiance;
    if (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        float tHit  = rq.CommittedRayT();
        float3 hitPos = worldPos + reflDir * tHit;

        uint instIdx = rq.CommittedInstanceIndex();
        uint primIdx = rq.CommittedPrimitiveIndex();
        float2 bary  = rq.CommittedTriangleBarycentrics();

        GpuInstanceInfo info = t_SpecInstanceInfo[instIdx];
        uint i0 = t_SpecIndices[info.indexOffset + primIdx * 3 + 0];
        uint i1 = t_SpecIndices[info.indexOffset + primIdx * 3 + 1];
        uint i2 = t_SpecIndices[info.indexOffset + primIdx * 3 + 2];

        GpuPackedVertex v0 = t_SpecVertices[info.vertexOffset + i0];
        GpuPackedVertex v1 = t_SpecVertices[info.vertexOffset + i1];
        GpuPackedVertex v2 = t_SpecVertices[info.vertexOffset + i2];

        float b0 = 1.0f - bary.x - bary.y;
        float3 localNormal = normalize(b0   * float3(v0.nx, v0.ny, v0.nz)
                                     + bary.x * float3(v1.nx, v1.ny, v1.nz)
                                     + bary.y * float3(v2.nx, v2.ny, v2.nz));
        float2 hitUv = b0 * float2(v0.u, v0.v)
                     + bary.x * float2(v1.u, v1.v)
                     + bary.y * float2(v2.u, v2.v);
        float3x4 o2w = rq.CommittedObjectToWorld3x4();
        float3x3 o2wRot = float3x3(
            float3(o2w[0][0], o2w[1][0], o2w[2][0]),
            float3(o2w[0][1], o2w[1][1], o2w[2][1]),
            float3(o2w[0][2], o2w[1][2], o2w[2][2]));

        float3 worldNormal = normalize(mul(o2wRot, localNormal));
        if (dot(worldNormal, -reflDir) < 0.0f)
            worldNormal = -worldNormal;

        // Compute geometric normal for shadow terminator.
        float3 localPos0 = float3(v0.px, v0.py, v0.pz);
        float3 localPos1 = float3(v1.px, v1.py, v1.pz);
        float3 localPos2 = float3(v2.px, v2.py, v2.pz);
        float3 wP0 = float3(dot(o2w[0], float4(localPos0, 1)), dot(o2w[1], float4(localPos0, 1)), dot(o2w[2], float4(localPos0, 1)));
        float3 wP1 = float3(dot(o2w[0], float4(localPos1, 1)), dot(o2w[1], float4(localPos1, 1)), dot(o2w[2], float4(localPos1, 1)));
        float3 wP2 = float3(dot(o2w[0], float4(localPos2, 1)), dot(o2w[1], float4(localPos2, 1)), dot(o2w[2], float4(localPos2, 1)));
        float3 geoNormal = normalize(cross(wP1 - wP0, wP2 - wP0));
        if (dot(geoNormal, -reflDir) < 0.0f)
            geoNormal = -geoNormal;

        const GpuMaterialRange materialRange = ResolveMaterialRange(info, primIdx);
        const float4 fallbackBaseColor = float4(materialRange.baseColorR, materialRange.baseColorG, materialRange.baseColorB, 1.0f);
        const bool useTexturedHit = (materialRange.baseColorTextureIndex != INVALID_TEXTURE_INDEX);
        float3 hitAlbedo = fallbackBaseColor.rgb;
        if (useTexturedHit)
        {
            const float lod = clamp(ComputeSpecularTextureLod(tHit, clampedRoughness), 1.5f, 8.0f);
            const float3 sampledAlbedo = SampleMaterialTexture(materialRange.baseColorTextureIndex, hitUv, lod, fallbackBaseColor).rgb;
            const float sampledLuma = dot(sampledAlbedo, float3(0.2126f, 0.7152f, 0.0722f));
            const float fallbackLuma = dot(fallbackBaseColor.rgb, float3(0.2126f, 0.7152f, 0.0722f));
            hitAlbedo = (sampledLuma > 0.01f || fallbackLuma <= 0.01f) ? sampledAlbedo : fallbackBaseColor.rgb;
        }
        incomingRadiance = ShadeSpecHit(hitPos, worldNormal, geoNormal, hitAlbedo);
        incomingRadiance = max(incomingRadiance, hitAlbedo * lerp(0.45f, 1.0f, metallic));
    }
    else
    {
        incomingRadiance = SkyRadiance(reflDir);
    }

    // ── GGX BRDF weight ──────────────────────────────────────────────────────
    // Half vector between V and reflDir.
    float3 H    = normalize(V + reflDir);
    float NdotH = max(dot(N, H), 0.0001f);
    float VdotH = max(dot(V, H), 0.0001f);

    float4 albedoSample = t_Albedo.Load(int3(pixel, 0));
    float3 albedo       = albedoSample.rgb;
    float3 F0           = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    float3 F = F_Schlick(VdotH, F0);
    float  G = G_Smith(NdotV, NdotL, clampedRoughness);
    // Keep reflections visibly present even while the ray-traced specular path is being tuned.
    float3 specWeight = F * G * VdotH / max(NdotH * NdotV, 0.0001f);
    specWeight = max(specWeight, F * (0.75f + (1.0f - clampedRoughness) * 1.25f));

    // Clamp for firefly suppression (reuse RadianceClamp).
    float3 specular = incomingRadiance * specWeight;
    specular = max(specular, incomingRadiance * (0.25f + metallic * 0.75f) * saturate(1.0f - clampedRoughness * 0.35f));
    if (g_RadianceClamp > 0.0f)
        specular = min(specular, g_RadianceClamp);

    u_SpecularOutput[pixel] = float4(specular, 1.0f);
}
