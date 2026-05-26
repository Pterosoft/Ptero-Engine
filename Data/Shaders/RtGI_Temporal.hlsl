// RtGI_Temporal.hlsl
// RTGI Pass 1 – ReSTIR GI Temporal Resampling.
//
// Merges the current frame's initial reservoir (from RayGen) with the reprojected
// reservoir from the previous frame.  This is the core of ReSTIR's temporal
// path-reuse: we combine two reservoirs (each representing a candidate set of
// size M) into one, capping history at g_MaxHistoryLength to prevent ghosting.
//
// Reference: "Spatiotemporal reservoir resampling for real-time ray tracing
//             with dynamic direct lighting", Bitterli et al. 2020.

#include "RtGI_Common.hlsli"

// ─── Inputs ───────────────────────────────────────────────────────────────────
StructuredBuffer<PackedGIReservoir>   t_CurrentReservoir : register(t0);
StructuredBuffer<PackedGIReservoir>   t_HistoryReservoir : register(t1);

// ─── Outputs ──────────────────────────────────────────────────────────────────
RWStructuredBuffer<PackedGIReservoir> u_OutReservoir : register(u0);
RWTexture2D<float4>                   u_GIOutput      : register(u1);

// G-Buffer – needed to read depth/normal for surface validation.
// Bound as the second SRV table (slot 1) but we re-declare it here for the
// temporal pass where slot 1 holds the history reservoir, not the g-buffer.
// Surface validation uses the g-buffer from the frame that was written above –
// for simplicity we repurpose the normal+depth stored inside the reservoir.

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────
[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 pixel = DTid.xy;
    if (pixel.x >= g_FrameWidth || pixel.y >= g_FrameHeight)
        return;

    const uint curIdx = PixelIndex(pixel);
    GIReservoir cur = UnpackReservoir(t_CurrentReservoir[curIdx]);

    // Skip sky / invalid pixels.
    if (cur.M == 0)
    {
        u_OutReservoir[curIdx] = PackReservoir(cur);
        return;
    }

    // The first frame after RTGI init/resize has no valid history reservoir yet.
    // Treat it as history-free so we do not sample uninitialized ping-pong data.
    if (g_FrameIndex == 0)
    {
        u_OutReservoir[curIdx] = PackReservoir(cur);
        u_GIOutput[pixel]      = float4(cur.radiance * cur.weightSum, 1.0f);
        return;
    }

    // ── Reproject pixel to previous frame ───────────────────────────────
    float2 prevUV   = ReprojectUV(cur.position);
    bool   inBounds = all(prevUV >= 0.0f) && all(prevUV < 1.0f);

    if (!inBounds)
    {
        u_OutReservoir[curIdx] = PackReservoir(cur);
        u_GIOutput[pixel]      = float4(cur.radiance * cur.weightSum, 1.0f);
        return;
    }

    uint2 prevPixel = uint2(prevUV * float2(g_FrameWidth, g_FrameHeight));
    prevPixel = clamp(prevPixel, uint2(0, 0), uint2(g_FrameWidth - 1, g_FrameHeight - 1));

    const uint histIdx = PixelIndex(prevPixel);
    GIReservoir hist = UnpackReservoir(t_HistoryReservoir[histIdx]);

    // ── Surface similarity test ──────────────────────────────────────────
    bool surfaceMatch = false;
    if (hist.M > 0)
    {
        // Depth check: compare distance from camera to secondary hit positions.
        float curDist  = length(cur.position  - g_CameraPos);
        float histDist = length(hist.position - g_CameraPos);
        float depthDiff = abs(curDist - histDist) / max(curDist, 0.001f);

        // Normal check between the primary surface normals encoded in reservoir normals.
        float normalAlign = dot(cur.normal, hist.normal);

        surfaceMatch = (depthDiff  < g_DepthThreshold) &&
                       (normalAlign > g_NormalThreshold);
    }

    if (!surfaceMatch)
    {
        u_OutReservoir[curIdx] = PackReservoir(cur);
        u_GIOutput[pixel]      = float4(cur.radiance * cur.weightSum, 1.0f);
        return;
    }

    // ── Combine reservoirs ────────────────────────────────────────────────
    // Cap the history M to prevent unbounded accumulation (ghosting).
    uint histM   = min(hist.M, g_MaxHistoryLength);
    hist.M       = histM;

    // RIS weight for the history candidate: p_hat * W * M
    float histTargetPdf = Luminance(hist.radiance) * 3.14159265f;
    float histRisWeight = histTargetPdf * hist.weightSum * float(histM);

    float curTargetPdf  = Luminance(cur.radiance) * 3.14159265f;
    float curRisWeight  = curTargetPdf * cur.weightSum * float(cur.M);

    // Merge into a new combined reservoir.
    GIReservoir merged  = EmptyReservoir();
    uint rng = InitRng(pixel, g_FrameIndex);

    // Add current sample.
    {
        GIReservoir c;
        c.position  = cur.position;
        c.normal    = cur.normal;
        c.radiance  = cur.radiance;
        c.weightSum = 0;
        c.M         = 0;
        c.age       = 0;
        UpdateReservoir(merged, c, curRisWeight, rng);
    }

    // Add history sample.
    {
        GIReservoir h;
        h.position  = hist.position;
        h.normal    = hist.normal;
        h.radiance  = hist.radiance;
        h.weightSum = 0;
        h.M         = 0;
        h.age       = hist.age + 1;
        UpdateReservoir(merged, h, histRisWeight, rng);
    }

    merged.M   = cur.M + histM;
    merged.age = hist.age + 1;

    float mergedTargetPdf = Luminance(merged.radiance) * 3.14159265f;
    FinalizeReservoir(merged, mergedTargetPdf);

    // ── Write out ─────────────────────────────────────────────────────────
    u_OutReservoir[curIdx] = PackReservoir(merged);
    u_GIOutput[pixel]      = float4(merged.radiance * merged.weightSum, 1.0f);
}
