// RtAO_Common.hlsli
// Shared constants, utilities, and root-signature binding for all three
// RTAO compute shader passes.

#ifndef RTAO_COMMON_HLSLI
#define RTAO_COMMON_HLSLI

// ─── Root signature (matches RtAmbientOcclusion::CreateRootSignature) ─────────
// [0] CBV  b0  – RtAOConstants
// [1] SRV  t0  – input slot A  (normal+depth / raw AO / accum)
// [2] SRV  t1  – input slot B  (TLAS / accum history / G-Buffer normal)
// [3] SRV  t2  – input slot C  (current G-Buffer normal+depth for temporal reprojection)
// [4] SRV  t3  – input slot D  (previous-frame normal+depth history)
// [5] UAV  u0  – output        (raw AO / accum write / resolved AO / cached history)
// ─────────────────────────────────────────────────────────────────────────────

cbuffer RtAOConstants : register(b0)
{
    uint   g_FrameWidth;
    uint   g_FrameHeight;
    uint   g_FrameIndex;
    uint   g_RaysPerPixel;

    float  g_MaxRayLength;
    float  g_RayBias;
    float  g_AOPower;
    float  g_Intensity;

    int    g_DebugView;
    float3 g_Pad0;

    float4x4 g_ViewProjInv;   // inverse view-projection (row-major)
    float4x4 g_CurrViewProj;  // current non-jittered view-projection (row-major)
    float4x4 g_PrevViewProj;  // previous non-jittered view-projection (row-major)
    float4x4 g_WorldToView;   // current non-jittered world-to-view (row-major)

    float3 g_CameraPos;
    float  g_Pad1;
}

// ─── PCG random number generator ─────────────────────────────────────────────
uint InitRng(uint2 pixel, uint frameIndex)
{
    return (pixel.x * 1973u + pixel.y * 9277u + frameIndex * 26699u) | 1u;
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

uint InitSurfaceRng(float3 worldPos, float3 normal)
{
    const int3 posCell = int3(floor(worldPos * 64.0f));
    const int3 nCell = int3(round(normal * 1024.0f));

    uint seed = 0x9e3779b9u;
    seed ^= HashUint(asuint(posCell.x) + 0x68e31da4u);
    seed ^= HashUint(asuint(posCell.y) + 0xb5297a4du);
    seed ^= HashUint(asuint(posCell.z) + 0x1b56c4e9u);
    seed ^= HashUint(asuint(nCell.x) + 0x7f4a7c15u);
    seed ^= HashUint(asuint(nCell.y) + 0x9e3779b1u);
    seed ^= HashUint(asuint(nCell.z) + 0x94d049bbu);
    return seed | 1u;
}

float RandFloat(inout uint state)
{
    state = state * 1664525u + 1013904223u;
    return (state >> 8) * (1.0f / float(1 << 24));
}

float RadicalInverse_VdC(uint bits)
{
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
    bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
    bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
    bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
    return float(bits) * 2.3283064365386963e-10f;
}

float2 Hammersley2D(uint i, uint n)
{
    return float2((float(i) + 0.5f) / float(n), RadicalInverse_VdC(i));
}

// ─── Oct-normal decode (matching GBuffer.hlsl encoding) ──────────────────────
float3 DecodeOctNormal(float2 f)
{
    f = f * 2.0f - 1.0f;
    float3 n = float3(f.x, f.y, 1.0f - abs(f.x) - abs(f.y));
    if (n.z < 0.0f)
    {
        float2 wrapped = (1.0f - abs(n.yx)) * (float2(n.xy >= 0.0f) * 2.0f - 1.0f);
        n.xy = wrapped;
    }
    return normalize(n);
}

// ─── Reconstruct world-space position from depth ─────────────────────────────
float3 ReconstructWorldPos(uint2 pixel, float depth)
{
    float2 uv  = (float2(pixel) + 0.5f) / float2(g_FrameWidth, g_FrameHeight);
    float2 ndc = uv * 2.0f - 1.0f;
    ndc.y      = -ndc.y;
    float4 clip  = float4(ndc, depth, 1.0f);
    float4 world = mul(clip, g_ViewProjInv);
    return world.xyz / world.w;
}

float3 ReconstructWorldPosClamped(int2 pixel, Texture2D<float4> normalDepthTex)
{
    const uint2 p = uint2(
        clamp(pixel.x, 0, int(g_FrameWidth) - 1),
        clamp(pixel.y, 0, int(g_FrameHeight) - 1));
    const float depth = normalDepthTex.Load(int3(p, 0)).b;
    return ReconstructWorldPos(p, depth);
}

float3 ComputeViewFacingGeometryNormal(uint2 pixel, Texture2D<float4> normalDepthTex, float3 fallbackNormal, float3 cameraPos)
{
    const int2 p = int2(pixel);

    const float  centerDepth = normalDepthTex.Load(int3(pixel, 0)).b;
    const float3 centerPos = ReconstructWorldPos(pixel, centerDepth);
    const float3 posR = ReconstructWorldPosClamped(p + int2(1, 0), normalDepthTex);
    const float3 posL = ReconstructWorldPosClamped(p + int2(-1, 0), normalDepthTex);
    const float3 posU = ReconstructWorldPosClamped(p + int2(0, -1), normalDepthTex);
    const float3 posD = ReconstructWorldPosClamped(p + int2(0, 1), normalDepthTex);

    float3 dx = (distance(posR, centerPos) < distance(posL, centerPos)) ? (posR - centerPos) : (centerPos - posL);
    float3 dy = (distance(posD, centerPos) < distance(posU, centerPos)) ? (posD - centerPos) : (centerPos - posU);

    float3 geoNormal = normalize(cross(dx, dy));
    if (any(isnan(geoNormal)) || any(isinf(geoNormal)) || dot(geoNormal, geoNormal) < 0.25f)
        geoNormal = fallbackNormal;

    const float3 viewDir = normalize(cameraPos - centerPos);
    if (dot(geoNormal, viewDir) < 0.0f)
        geoNormal = -geoNormal;

    return geoNormal;
}

float2 ReprojectUV(float3 worldPos)
{
    float4 clip = mul(float4(worldPos, 1.0f), g_PrevViewProj);
    clip.xy /= clip.w;
    float2 uv = clip.xy * 0.5f + 0.5f;
    uv.y = 1.0f - uv.y;
    return uv;
}

float2 ProjectCurrentUV(float3 worldPos)
{
    float4 clip = mul(float4(worldPos, 1.0f), g_CurrViewProj);
    clip.xy /= clip.w;
    float2 uv = clip.xy * 0.5f + 0.5f;
    uv.y = 1.0f - uv.y;
    return uv;
}

// ─── Cosine-weighted hemisphere sample around N ───────────────────────────────
float3 CosineSampleHemisphere(float2 xi, float3 N)
{
    float phi      = 6.28318530f * xi.x;
    float cosTheta = sqrt(1.0f - xi.y);
    float sinTheta = sqrt(xi.y);

    float3 localDir = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

    float3 up    = abs(N.z) < 0.999f ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 right = normalize(cross(up, N));
    float3 fwd   = cross(N, right);

    return normalize(localDir.x * right + localDir.y * fwd + localDir.z * N);
}

// ─── Linear pixel index ───────────────────────────────────────────────────────
uint PixelIndex(uint2 pixel)
{
    return pixel.y * g_FrameWidth + pixel.x;
}

#endif // RTAO_COMMON_HLSLI
