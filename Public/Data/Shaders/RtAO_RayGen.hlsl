// RtAO_RayGen.hlsl  –  cs_6_5
// RTAO Pass 0: per-pixel inline occlusion ray tracing using RayQuery.
//
// For each G-Buffer pixel:
//   1. Reconstruct world-space position and decode the surface normal.
//   2. Fire g_RaysPerPixel cosine-weighted hemisphere rays via RayQuery.
//   3. A ray that hits any geometry within g_MaxRayLength contributes 0 (occluded).
//   4. A ray that misses contributes 1 (unoccluded).
//   5. The average is raised to g_AOPower and written to u_RawAO.x, while
//      u_RawAO.y stores the average hit distance for denoiser reprojection.

#include "RtAO_Common.hlsli"

// ─── Inputs ───────────────────────────────────────────────────────────────────
Texture2D<float4>                t_NormalDepth : register(t0); // RG = oct-normal, B = geometry-pass depth
RaytracingAccelerationStructure  t_TLAS        : register(t1);
Texture2D<float>                 t_SceneDepth  : register(t2);

// ─── Outputs ──────────────────────────────────────────────────────────────────
RWTexture2D<float2> u_RawAO : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchId.xy;
    if (pixel.x >= g_FrameWidth || pixel.y >= g_FrameHeight)
        return;

    float4 normalDepthSample = t_NormalDepth[pixel];
    float  depth             = normalDepthSample.b;

    // Skip sky / background pixels.
    if (depth >= 1.0f)
    {
        u_RawAO[pixel] = float2(1.0f, 0.0f);
        return;
    }

    float3 worldPos = ReconstructWorldPos(pixel, depth);
    float3 N        = DecodeOctNormal(normalDepthSample.rg);
    float3 V        = normalize(g_CameraPos - worldPos);
    if (dot(N, V) < 0.0f)
        N = -N;

    const float rayBias = max(g_RayBias, 0.001f);
    uint rng = InitSurfaceRng(worldPos, N);
    const float rotation = RandFloat(rng);

    float unoccluded = 0.0f;
    float hitDistanceSum = 0.0f;
    float hitCount = 0.0f;
    for (uint i = 0u; i < g_RaysPerPixel; ++i)
    {
        float2 xi  = Hammersley2D(i, g_RaysPerPixel);
        xi.x = frac(xi.x + rotation);
        float3 dir = CosineSampleHemisphere(xi, N);
        float3 biasedOrigin = worldPos + N * rayBias;

        RayDesc ray;
        ray.Origin    = biasedOrigin;
        ray.Direction = dir;
        ray.TMin      = rayBias;
        ray.TMax      = g_MaxRayLength;

        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_BACK_FACING_TRIANGLES | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
        rq.TraceRayInline(t_TLAS, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_BACK_FACING_TRIANGLES | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFF, ray);
        while (rq.Proceed()) {}

        if (rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
            unoccluded += 1.0f;
        else
        {
            hitDistanceSum += rq.CommittedRayT();
            hitCount += 1.0f;
        }
    }

    float ao = pow(saturate(unoccluded / float(g_RaysPerPixel)), g_AOPower);
    float avgHitDistance = (hitCount > 0.0f) ? (hitDistanceSum / hitCount) : g_MaxRayLength;
    u_RawAO[pixel] = float2(ao, avgHitDistance);
}
