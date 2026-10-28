// PathTracer.hlsl  -  cs_6_5 (DXR inline RayQuery; compiled by DXC at runtime)
// Reference path tracer: progressive, accumulated over frames while nothing changes.
//
// Every pixel traces SamplesPerFrame paths from the camera through the scene TLAS
// that RtGlobalIllumination builds, with next-event estimation toward the sun and
// the scene lights at every vertex and a GGX + Lambert BRDF sampled by lobe.
// Paths are summed into an fp32 accumulation target; the resolve divides by the
// sample count and writes the image the raster lighting would otherwise have left.
// The integrator and its units are in PathTracer_Common.hlsli.
//
// Pixels whose camera rays all escape keep the raster image (sky, sun disc, stars,
// and anything else not in the TLAS). Camera hits on terrain take the G-Buffer
// pixel's material (PT_GBUFFER_TERRAIN in PathTracer_Common.hlsli).

#define PT_GBUFFER_TERRAIN
#include "PathTracer_Common.hlsli"

RWTexture2D<float4> u_Accumulation : register(u0);   // rgb: radiance sum, a: camera-ray hits
RWTexture2D<float4> u_Output       : register(u1);

// ─── Main ────────────────────────────────────────────────────────────────────
[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    const uint2 pixel = dtid.xy;
    if (pixel.x >= g_Width || pixel.y >= g_Height)
        return;

    const float4 background = t_Background.Load(int3(pixel, 0));
    g_PrimaryPixel = pixel;

    [branch]
    if ((g_Flags & PT_FLAG_SSS_MASK) != 0u)
    {
        const float3 dir = CameraRayDirection(float2(pixel) + 0.5f);
        const HitRecord rec = TraceClosest(g_CameraPos, dir, 0.0f, kMaxRayT);
        float3 maskColor = float3(0.0f, 0.0f, 0.0f);
        if (rec.hit)
            maskColor = SubsurfaceMaskColor(ResolveSurface(rec, g_CameraPos, dir, 0.0f), t_GBufferNormal.Load(int3(pixel, 0)).w);
        u_Output[pixel] = float4(maskColor, 1.0f);
        return;
    }

    [branch]
    if (g_DebugView != 0)
    {
        const float3 dir = CameraRayDirection(float2(pixel) + 0.5f);
        const HitRecord rec = TraceClosest(g_CameraPos, dir, 0.0f, kMaxRayT);
        float3 debugColor = background.rgb;
        if (rec.hit)
        {
            Surface s = ResolveSurface(rec, g_CameraPos, dir, 0.0f);
            ApplyGBufferTerrain(rec, s);
            if (g_DebugView == 1)      debugColor = s.albedo;
            else if (g_DebugView == 2) debugColor = s.normal * 0.5f + 0.5f;
            else                       debugColor = float3(s.roughness, s.metallic, 0.0f);
        }
        u_Output[pixel] = float4(debugColor, 1.0f);
        return;
    }

    float4 accumulated = g_SampleIndex > 0u ? u_Accumulation[pixel] : float4(0.0f, 0.0f, 0.0f, 0.0f);
    uint sampleCount = g_SampleIndex;

    [branch]
    if ((g_Flags & PT_FLAG_TRACE) != 0u)
    {
        uint rng = PcgHash(pixel.x ^ PcgHash(pixel.y ^ PcgHash(g_FrameSeed))) | 1u;
        [loop]
        for (uint i = 0; i < g_SamplesPerFrame; ++i)
        {
            // Uniform jitter over the pixel: accumulation is the anti-aliasing.
            const float2 subPixel = float2(Rand(rng), Rand(rng));
            const float3 dir = CameraRayDirection(float2(pixel) + subPixel);
            bool cameraHit;
            const float3 radiance = TracePath(g_CameraPos, dir, rng, cameraHit);
            accumulated += float4(radiance, cameraHit ? 1.0f : 0.0f);
        }
        sampleCount += g_SamplesPerFrame;
        u_Accumulation[pixel] = accumulated;
    }

    // A pixel no camera ray found anything in keeps the raster image, which also
    // has the sun disc, the stars and whatever is not in the TLAS.
    // The background already carries the fog; a traced pixel takes it at the
    // G-Buffer's distance, the surface the raster image fogged (the TAA jitter
    // apart). Where the G-Buffer drew no surface, the pixel is left as traced.
    float3 color = background.rgb;
    if (accumulated.a > 0.0f && sampleCount > 0u)
    {
        color = accumulated.rgb * (g_InvPreExposure / float(sampleCount));
        const float depth = t_GBufferNormal.Load(int3(pixel, 0)).z;
        if (depth > 0.0f && depth < 1.0f)
            color = ApplyVolumetricFog(pixel, distance(g_CameraPos, ReconstructGBufferPosition(pixel, depth)), SanitizeRadiance(color));
    }
    u_Output[pixel] = float4(color, 1.0f);
}
