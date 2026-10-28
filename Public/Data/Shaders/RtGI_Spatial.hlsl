// RtGI_Spatial.hlsl
// RTGI Pass 2 – ReSTIR GI Spatial Resampling.
//
// Gathers reservoirs from a disc of screen-space neighbours and combines them
// with the current pixel's reservoir via weighted reservoir sampling.  This
// effectively shares GI path samples across pixels that see similar geometry,
// dramatically improving convergence without firing additional rays.
//
// Reference: Bitterli et al. 2020, Sec. 4.2 "Spatial reuse".

#include "RtGI_Common.hlsli"

// ─── Inputs ───────────────────────────────────────────────────────────────────
StructuredBuffer<PackedGIReservoir>   t_InputReservoir   : register(t0);
Texture2D<float4>                     t_NormalDepth      : register(t1);  // g-buffer

// ─── Outputs ──────────────────────────────────────────────────────────────────
RWStructuredBuffer<PackedGIReservoir> u_OutReservoir : register(u0);
RWTexture2D<float4>                   u_GIOutput      : register(u1);

// ─────────────────────────────────────────────────────────────────────────────
// Pseudo-random spatial offsets on a unit disc (Poisson-ish, 8-tap).
// Scaled by g_SpatialRadius at runtime.
// ─────────────────────────────────────────────────────────────────────────────
static const float2 kDiscOffsets[8] =
{
    float2( 0.000f,  1.000f),
    float2( 0.707f,  0.707f),
    float2( 1.000f,  0.000f),
    float2( 0.707f, -0.707f),
    float2( 0.000f, -1.000f),
    float2(-0.707f, -0.707f),
    float2(-1.000f,  0.000f),
    float2(-0.707f,  0.707f),
};

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────
[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    // A trace pixel; the G-Buffer is read at the frame pixel each one stands for.
    const uint2 pixel = DTid.xy;
    const uint2 traceSize = TraceSize();
    if (any(pixel >= traceSize))
        return;

    const uint curIdx  = PixelIndex(pixel);
    GIReservoir cur    = UnpackReservoir(t_InputReservoir[curIdx]);

    if (cur.M == 0)
    {
        u_OutReservoir[curIdx] = PackReservoir(cur);
        return;
    }

    // Read current pixel's normal + depth for similarity tests.
    float4 nd0           = t_NormalDepth.Load(int3(TraceToFramePixel(pixel), 0));
    float  curDepth      = nd0.z;
    float2 curOctN       = nd0.xy * 2.0f - 1.0f;
    float  curOctZ       = 1.0f - abs(curOctN.x) - abs(curOctN.y);
    float3 curSurfNormal = normalize(float3(curOctZ < 0.0f
        ? ((1.0f - abs(curOctN.yx)) * (float2(curOctN.xy >= 0.0f) * 2.0f - 1.0f))
        : curOctN.xy,
        curOctZ));

    // ── Spatial resampling loop ───────────────────────────────────────────
    GIReservoir merged = cur;
    uint rng = InitRng(pixel, g_FrameIndex + 7919u);

    const uint numSamples = clamp(g_SpatialSamples, 1u, 8u);

    // Random rotation angle for the disc pattern to break correlation.
    float angle     = RandFloat(rng) * 6.28318f;
    float cosA      = cos(angle);
    float sinA      = sin(angle);

    [loop]
    for (uint i = 0; i < numSamples; ++i)
    {
        // Rotate and scale the disc offset.
        float2 baseOffset  = kDiscOffsets[i % 8];
        float2 rotOffset   = float2(
            baseOffset.x * cosA - baseOffset.y * sinA,
            baseOffset.x * sinA + baseOffset.y * cosA);
        // The radius is in frame pixels; the neighbours are trace pixels.
        int2 offset       = int2(rotOffset * (g_SpatialRadius / float(g_TraceScale)));

        int2 neighborPixel = int2(pixel) + offset;

        // Bounds check.
        if (any(neighborPixel < 0) ||
            neighborPixel.x >= int(traceSize.x) ||
            neighborPixel.y >= int(traceSize.y))
            continue;

        // ── Surface similarity test ──────────────────────────────────────
        float4 ndN     = t_NormalDepth.Load(int3(TraceToFramePixel(uint2(neighborPixel)), 0));
        float  nDepth  = ndN.z;

        float depthDiff = abs(curDepth - nDepth) / max(curDepth, 0.001f);
        if (depthDiff > g_DepthThreshold)
            continue;

        float2 nOctN = ndN.xy * 2.0f - 1.0f;
        float  nOctZ = 1.0f - abs(nOctN.x) - abs(nOctN.y);
        float3 nNorm = normalize(float3(nOctZ < 0.0f
            ? ((1.0f - abs(nOctN.yx)) * (float2(nOctN.xy >= 0.0f) * 2.0f - 1.0f))
            : nOctN.xy,
            nOctZ));

        if (dot(curSurfNormal, nNorm) < g_NormalThreshold)
            continue;

        // ── Combine neighbour reservoir ──────────────────────────────────
        uint neighbourIdx = PixelIndex(uint2(neighborPixel));
        GIReservoir neigh = UnpackReservoir(t_InputReservoir[neighbourIdx]);

        if (!IsValidReservoir(neigh))
            continue;

        // RIS weight: p_hat(x_n) * W_n * M_n
        float neighTargetPdf = Luminance(neigh.radiance) * 3.14159265f;
        float neighRisWeight = neighTargetPdf * neigh.weightSum * float(neigh.M);

        GIReservoir candidate;
        candidate.position  = neigh.position;
        candidate.normal    = neigh.normal;
        candidate.radiance  = neigh.radiance;
        candidate.weightSum = 0.0f;
        candidate.M         = neigh.M;
        candidate.age       = neigh.age;

        UpdateReservoir(merged, candidate, neighRisWeight, rng);
        merged.M += neigh.M;
    }

    // Finalize weight.
    float finalTargetPdf = Luminance(merged.radiance) * 3.14159265f;
    FinalizeReservoir(merged, finalTargetPdf);

    u_OutReservoir[curIdx] = PackReservoir(merged);
    u_GIOutput[pixel]      = float4(merged.radiance * merged.weightSum, 1.0f);
}
