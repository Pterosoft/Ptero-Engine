// RtShadow_RayGen.hlsl  –  cs_6_5
// RT shadow pass 0: one or more shadow rays per pixel toward a point on the sun's disc.
//
// Each frame picks different points on the disc (an R2 sequence offset per pixel), so
// the temporal pass integrates the penumbra over time. A ray that reaches g_MaxRayLength
// without a hit sees the sun. The nearest blocker distance goes to .y; the spatial pass
// sizes its filter from it.

#include "RtShadow_Common.hlsli"
#include "RtSurfaceSnap.hlsli"

RaytracingAccelerationStructure t_TLAS : register(t1);
RWTexture2D<float2>             u_Raw  : register(u0);

// Uniform point on the unit disc from two numbers in [0, 1).
float2 ConcentricDisc(float2 u)
{
    const float2 o = u * 2.0f - 1.0f;
    if (o.x == 0.0f && o.y == 0.0f)
        return float2(0.0f, 0.0f);
    float r, theta;
    if (abs(o.x) > abs(o.y))
    {
        r = o.x;
        theta = 0.785398163f * (o.y / o.x);
    }
    else
    {
        r = o.y;
        theta = 1.570796327f - 0.785398163f * (o.x / o.y);
    }
    return r * float2(cos(theta), sin(theta));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchId.xy;
    if (pixel.x >= g_FrameWidth || pixel.y >= g_FrameHeight)
        return;

    const float4 normalDepth = t_NormalDepth[pixel];
    if (IsSkyDepth(normalDepth.b))
    {
        u_Raw[pixel] = float2(1.0f, kRtShadowNoBlocker);
        return;
    }

    const float3 worldPos = ReconstructWorldPos(pixel, normalDepth.b);
    float3 N = DecodeOctNormal(normalDepth.rg);
    if (dot(N, g_CameraPos - worldPos) < 0.0f)
        N = -N;

    const float3 L = g_SunDirection;
    const float3 up = abs(L.z) < 0.999f ? float3(0, 0, 1) : float3(1, 0, 0);
    const float3 T = normalize(cross(up, L));
    const float3 B = cross(L, T);

    // Offset along the normal, plus a little toward the sun so grazing surfaces
    // don't catch themselves; grows with distance like the depth error does.
    const float distanceScale = 1.0f + length(worldPos - g_CameraPos) * 0.002f;
    // From the traced surface, not the rasterised one (RtSurfaceSnap.hlsli).
    const float3 tracedPos = PteroSnapToTracedSurface(t_TLAS, g_CameraPos, worldPos);
    // Off the true surface and toward the sun, by a bias that grows at grazing angles.
    const float3 geoN = GeometricNormal(pixel, worldPos, N);
    const float3 origin = tracedPos + (geoN + L) * ShadowRayBias(dot(geoN, L), distanceScale);

    // Per-pixel scramble, then an R2 low-discrepancy step per frame and per ray.
    const uint seed = HashUint(pixel.x * 73856093u ^ pixel.y * 19349663u);
    const float2 scramble = float2(seed & 0xFFFFu, seed >> 16) * (1.0f / 65536.0f);
    const float2 kR2 = float2(0.754877666f, 0.569840291f);

    float lit = 0.0f;
    float nearestBlocker = kRtShadowNoBlocker;
    for (uint i = 0u; i < g_RaysPerPixel; ++i)
    {
        const float2 xi = frac(scramble + kR2 * float(g_FrameIndex * g_RaysPerPixel + i));
        const float2 disc = ConcentricDisc(xi) * g_SunTanHalfAngle;

        RayDesc ray;
        ray.Origin    = origin;
        ray.Direction = normalize(L + T * disc.x + B * disc.y);
        ray.TMin      = 0.0f;
        ray.TMax      = g_MaxRayLength;

        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
        rq.TraceRayInline(t_TLAS, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFF, ray);
        while (rq.Proceed()) {}

        if (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
            nearestBlocker = min(nearestBlocker, rq.CommittedRayT());
        else
            lit += 1.0f;
    }

    u_Raw[pixel] = float2(lit / float(g_RaysPerPixel), nearestBlocker);
}
