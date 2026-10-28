// RtShadow_Common.hlsli
// Constants and helpers shared by the ray-traced sun shadow passes (RtShadows.cpp).

#ifndef RTSHADOW_COMMON_HLSLI
#define RTSHADOW_COMMON_HLSLI

// ─── Root signature (matches RtShadows::CreateRootSignature) ─────────────────
// [0] CBV  b0  – RtShadowConstants
// [1] SRV  t0  – G-Buffer normal target (RG = oct normal, B = hardware depth)
// [2] SRV  t1  – scene TLAS
// [3] UAV  u0  – raw visibility (x) + blocker distance (y)
// [4] UAV  u1  – previous frame's history
// [5] UAV  u2  – this frame's history (x = visibility, y = blocker distance, z = camera distance)
// [6] UAV  u3  – final visibility (R16F)
// [7] UAV  u4  – local light visibility pairs, denoised (read by DeferredLighting)
// [8] UAV  u5  – local light visibility pairs, raw (RtShadow_Local.hlsl)
// [9,10]  u6,u7 – local light history, this frame (entries 0-3, 4-7)
// [11,12] u8,u9 – local light history, last frame
// [13..17]     – light clusters, b0 and t0..t3 in space 7 (LightClusters.hlsli)
// ─────────────────────────────────────────────────────────────────────────────

cbuffer RtShadowConstants : register(b0)
{
    uint   g_FrameWidth;
    uint   g_FrameHeight;
    uint   g_FrameIndex;
    uint   g_RaysPerPixel;

    float3 g_SunDirection;    // toward the sun
    float  g_SunTanHalfAngle;

    float  g_MaxRayLength;
    float  g_RayBias;
    float  g_TemporalBlend;
    int    g_MaxFilterRadius;

    float3 g_CameraPos;
    uint   g_HistoryValid;

    float3 g_PrevCameraPos;
    float  g_PixelAngle;

    float4x4 g_ViewProjInv;   // inverse jittered view-projection (row-major)
    float4x4 g_PrevViewProj;  // previous non-jittered view-projection (row-major)

    int    g_DebugView;
    float  g_LocalLightSoftness;
    uint   g_LocalHistoryValid;
    float  g_Pad0;
}

Texture2D<float4> t_NormalDepth : register(t0);

static const float kRtShadowNoBlocker = 65504.0f; // largest half: "nothing was hit"

bool IsSkyDepth(float depth)
{
    return depth <= 0.0f || depth >= 1.0f;
}

float3 DecodeOctNormal(float2 f)
{
    f = f * 2.0f - 1.0f;
    float3 n = float3(f.x, f.y, 1.0f - abs(f.x) - abs(f.y));
    if (n.z < 0.0f)
        n.xy = (1.0f - abs(n.yx)) * (float2(n.xy >= 0.0f) * 2.0f - 1.0f);
    return normalize(n);
}

float3 ReconstructWorldPos(uint2 pixel, float depth)
{
    float2 uv  = (float2(pixel) + 0.5f) / float2(g_FrameWidth, g_FrameHeight);
    float2 ndc = uv * 2.0f - 1.0f;
    ndc.y      = -ndc.y;
    float4 world = mul(float4(ndc, depth, 1.0f), g_ViewProjInv);
    return world.xyz / world.w;
}

// The surface's true (geometric) normal from the depth buffer, facing the camera.
// Shadow rays must leave along this, not the normal-mapped shading normal: at the
// grazing angles a light standing on a surface (a candle on a table) arrives at, a
// ray offset along a bumped normal skims straight back into the surface - acne that
// TAA hides while the camera is still and that shows as darkening once it moves.
float3 GeometricNormal(uint2 pixel, float3 centerPos, float3 fallbackNormal)
{
    const int2 p = int2(pixel);
    const int2 maxP = int2(g_FrameWidth - 1, g_FrameHeight - 1);
    const int2 pr = clamp(p + int2(1, 0), int2(0, 0), maxP);
    const int2 pl = clamp(p - int2(1, 0), int2(0, 0), maxP);
    const int2 pd = clamp(p + int2(0, 1), int2(0, 0), maxP);
    const int2 pu = clamp(p - int2(0, 1), int2(0, 0), maxP);
    const float3 posR = ReconstructWorldPos(uint2(pr), t_NormalDepth[pr].b);
    const float3 posL = ReconstructWorldPos(uint2(pl), t_NormalDepth[pl].b);
    const float3 posD = ReconstructWorldPos(uint2(pd), t_NormalDepth[pd].b);
    const float3 posU = ReconstructWorldPos(uint2(pu), t_NormalDepth[pu].b);

    // The nearer neighbour on each axis, so the normal does not straddle an edge.
    const float3 dx = (distance(posR, centerPos) < distance(posL, centerPos)) ? (posR - centerPos) : (centerPos - posL);
    const float3 dy = (distance(posD, centerPos) < distance(posU, centerPos)) ? (posD - centerPos) : (centerPos - posU);

    float3 n = cross(dx, dy);
    const float lengthSq = dot(n, n);
    n = (lengthSq > 1e-12f && !any(isnan(n))) ? n * rsqrt(lengthSq) : fallbackNormal;
    if (dot(n, g_CameraPos - centerPos) < 0.0f)
        n = -n;
    return n;
}

// Offset for a shadow ray toward direction L, as RTGI biases its own (RtGI_RayGen.hlsl):
// at least g_RayBias, growing as the light grazes the true surface.
float ShadowRayBias(float geoNdotL, float distanceScale)
{
    return max(g_RayBias, 0.02f * rsqrt(max(geoNdotL, 0.05f))) * distanceScale;
}

// ─── Local light visibility encodings ────────────────────────────────────────
// Pairs (raw trace, final output): 16 bits each, light index << 8 | visibility * 255,
// two to a uint, 0xFFFF = empty. History: 32 bits each, light index << 24 |
// visibility * 65535 << 8 | accumulated frames, four to a uint4 texture (two of them),
// 0xFFFFFFFF = empty.
#define PTERO_RT_LOCAL_LIGHTS 8 // RtShadowSettings::kMaxLocalLightsPerPixel
static const uint kRtLocalEmptyPair    = 0xFFFFu;
static const uint kRtLocalEmptyHistory = 0xFFFFFFFFu;

uint RtLocalPair(uint4 pairs, uint slot)
{
    const uint word = slot < 2u ? pairs.x : slot < 4u ? pairs.y : slot < 6u ? pairs.z : pairs.w;
    return (word >> ((slot & 1u) * 16u)) & 0xFFFFu;
}

uint RtLocalHistoryEntry(uint4 historyA, uint4 historyB, uint slot)
{
    const uint4 h = slot < 4u ? historyA : historyB;
    const uint k = slot & 3u;
    return k == 0u ? h.x : k == 1u ? h.y : k == 2u ? h.z : h.w;
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

#endif // RTSHADOW_COMMON_HLSLI
