// RtShadow_Local.hlsl  –  cs_6_5
// RT shadow pass 3: shadow rays for the point, spot and rect lights that reach a pixel.
//
// Walks the pixel's clustered light list (LightClusters.hlsli) exactly as the deferred
// pass does, and for the first PTERO_RT_LOCAL_LIGHTS shadow-casting lights that can
// light the surface traces g_RaysPerPixel rays toward points on the emitter: a disc of
// the light's Source Radius facing the surface, or the rect panel. Each result is a
// 16-bit pair, light index << 8 | visibility * 255, two to a uint; 0xFFFF is an empty
// slot. The deferred pass looks lights up by index, so it needs no agreement with this
// pass on cluster order. Lights beyond the slots keep their shadow maps.
//
// The sample points move every frame; RtShadow_LocalTemporal accumulates each light's
// visibility over frames and RtShadow_LocalSpatial blurs what noise is left.

#include "RtShadow_Common.hlsli"
#include "LightClusters.hlsli"
#include "RtSurfaceSnap.hlsli"


RaytracingAccelerationStructure t_TLAS   : register(t1);
RWTexture2D<uint4>              u_Local  : register(u5); // raw; RtShadow_LocalTemporal and _LocalSpatial denoise it into u4

static const float kLocalShadowNearPlane = 0.05f; // PointShadowMapRenderer::kShadowNearPlane

float TraceVisibility(float3 origin, float3 target, float stopShort)
{
    const float3 toTarget = target - origin;
    const float distance = length(toTarget);
    const float tMax = distance - stopShort;
    if (tMax <= 0.0f)
        return 1.0f;

    RayDesc ray;
    ray.Origin    = origin;
    ray.Direction = toTarget / distance;
    ray.TMin      = 0.0f;
    ray.TMax      = tMax;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
    rq.TraceRayInline(t_TLAS, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFF, ray);
    while (rq.Proceed()) {}
    return rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0f : 1.0f;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchId.xy;
    if (pixel.x >= g_FrameWidth || pixel.y >= g_FrameHeight)
        return;

    uint entries[PTERO_RT_LOCAL_LIGHTS];
    [unroll]
    for (uint e = 0u; e < PTERO_RT_LOCAL_LIGHTS; ++e)
        entries[e] = 0xFFFFu;

    const float4 normalDepth = t_NormalDepth[pixel];
    if (IsSkyDepth(normalDepth.b))
    {
        u_Local[pixel] = uint4(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu);
        return;
    }

    const float3 worldPos = ReconstructWorldPos(pixel, normalDepth.b);
    // Not flipped toward the camera: the deferred pass tests N.L with the normal as
    // stored, and the lights chosen here have to be the ones it lights with.
    const float3 N = DecodeOctNormal(normalDepth.rg);
    const float distanceScale = 1.0f + length(worldPos - g_CameraPos) * 0.002f;
    // From the traced surface, not the rasterised one (RtSurfaceSnap.hlsli).
    const float3 tracedPos = PteroSnapToTracedSurface(t_TLAS, g_CameraPos, worldPos);
    // True surface normal for the offset; the bias itself depends on each light's angle.
    const float3 geoN = GeometricNormal(pixel, worldPos, N);

    const uint pixelSeed = HashUint(pixel.x * 73856093u ^ pixel.y * 19349663u);
    const float2 kR2 = float2(0.754877666f, 0.569840291f);

    uint used = 0u;
    const uint lightCluster = PteroViewClusterBase(worldPos);
    [loop]
    for (uint lightWord = 0; lightWord < gLightClusters.WordCount && used < PTERO_RT_LOCAL_LIGHTS; ++lightWord)
    {
        uint lightBits = PteroViewClusterWord(lightCluster, lightWord);
        [loop]
        while (lightBits != 0u && used < PTERO_RT_LOCAL_LIGHTS)
        {
            const uint i = lightWord * 32u + firstbitlow(lightBits);
            lightBits &= lightBits - 1u;

            const PteroLightData light = gClusterLights[i];
            if (light.CastShadows < 0.5f || !any(light.Color > 0.0f))
                continue;

            // The deferred pass's "can this light reach the surface" test.
            const PteroResolvedLight shape = PteroResolveLightShape(light, worldPos);
            const bool isRect = (int)light.LightType == PTERO_LIGHT_TYPE_RECT;
            const float sourceRadius = isRect ? 0.0f : min(max(light.SourceRadius, 0.0f), light.Radius * 0.5f);
            float3 lightPos = shape.Position;
            const float3 toCenter = worldPos - lightPos;
            const float toCenterLen = length(toCenter);
            if (sourceRadius > 0.0001f && toCenterLen > 0.0001f)
                lightPos += (toCenter / toCenterLen) * sourceRadius;
            const float3 toLight = lightPos - worldPos;
            const float dist = length(toLight);
            if (shape.ShapeMask <= 0.0f || dist >= light.Radius || dist < 1e-4f || dot(N, toLight) <= 0.0f)
                continue;

            const uint seed = HashUint(pixelSeed ^ (i * 0x9e3779b9u));
            const float2 scramble = float2(seed & 0xFFFFu, seed >> 16) * (1.0f / 65536.0f);

            float lit = 0.0f;
            for (uint r = 0u; r < g_RaysPerPixel; ++r)
            {
                const float2 xi = frac(scramble + kR2 * float(g_FrameIndex * g_RaysPerPixel + r));
                float3 target;
                float stopShort;
                if (isRect)
                {
                    // A point on the panel, pulled toward the nearest point by the softness.
                    const float3 rectUp = cross(light.RectRight, light.Direction);
                    const float3 onPanel = light.Position
                        + light.RectRight * ((xi.x * 2.0f - 1.0f) * light.RectHalfWidth)
                        + rectUp * ((xi.y * 2.0f - 1.0f) * light.RectHalfHeight);
                    target = lerp(shape.Position, onPanel, saturate(g_LocalLightSoftness));
                    stopShort = 0.01f;
                }
                else
                {
                    // A point on the disc of the source sphere that faces the surface.
                    const float3 axis = toCenterLen > 0.0001f ? toCenter / toCenterLen : float3(0, 0, 1);
                    const float3 up = abs(axis.z) < 0.999f ? float3(0, 0, 1) : float3(1, 0, 0);
                    const float3 T = normalize(cross(up, axis));
                    const float3 B = cross(axis, T);
                    const float phi = 6.28318530f * xi.x;
                    const float radius = sourceRadius * g_LocalLightSoftness * sqrt(xi.y);
                    target = light.Position + (T * cos(phi) + B * sin(phi)) * radius;
                    // Stop before the bulb, like the shadow map's near plane, so a light
                    // sitting inside its own lamp mesh isn't shadowed by it.
                    stopShort = max(kLocalShadowNearPlane, sourceRadius * 1.05f);
                }
                // Off the true surface on the light's side, and toward the light, by a bias
                // that grows as the light grazes the surface (ShadowRayBias).
                const float3 toTarget = normalize(target - tracedPos);
                const float bias = ShadowRayBias(dot(geoN, toTarget), distanceScale);
                const float3 origin = tracedPos + (geoN + toTarget) * bias;
                lit += TraceVisibility(origin, target, stopShort);
            }

            const uint visibility8 = (uint)round(saturate(lit / float(g_RaysPerPixel)) * 255.0f);
            entries[used] = ((i & 0xFFu) << 8) | visibility8;
            ++used;
        }
    }

    u_Local[pixel] = uint4(
        entries[0] | (entries[1] << 16),
        entries[2] | (entries[3] << 16),
        entries[4] | (entries[5] << 16),
        entries[6] | (entries[7] << 16));
}
