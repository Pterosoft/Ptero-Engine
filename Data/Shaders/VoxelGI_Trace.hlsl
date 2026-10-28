// VoxelGI_Trace.hlsl
// Screen passes of the voxel GI (VoxelGIRenderer):
//
//   TraceCS    per pixel of the trace resolution (half or full), cone traces the
//              clipmap for diffuse irradiance and, below the roughness threshold,
//              one reflection cone. The voxel debug views march the clipmap from
//              the camera here instead.
//   ResolveCS  per full-resolution pixel, upsamples the trace with weights that
//              respect depth and normal edges, and writes
//                u0  diffuse irradiance / pi, demodulated - the RTGI buffer's units
//                u1  reflection weighted by the environment BRDF, for the
//                    deferred resolve
//                u2  the unweighted reflected radiance, for screen-space
//                    reflections to fall back on where their rays miss

#include "VoxelGI_Common.hlsli"
#include "SurfaceSpecular.hlsli"

Texture2D<float4> gGBufferAlbedo   : register(t0);
Texture2D<float4> gGBufferNormal   : register(t1);   // xy oct normal, z device depth, w specular
Texture2D<float4> gGBufferMaterial : register(t2);   // r roughness, g metallic
Texture3D<float4> gRadiance        : register(t3);
Texture3D<float4> gVoxelAlbedo     : register(t4);
Texture2D<float4> gTraceDiffuse    : register(t5);
Texture2D<float4> gTraceReflection : register(t6);

RWTexture2D<float4> gOutput0 : register(u0);
RWTexture2D<float4> gOutput1 : register(u1);
RWTexture2D<float4> gOutput2 : register(u2);

SamplerState gLinearClamp : register(s0);

static const int kDebugVoxelAlbedo   = 20;
static const int kDebugVoxelRadiance = 21;
static const int kDebugVoxelLevel    = 22;

float3 LevelColor(uint level)
{
    const float3 colors[6] =
    {
        float3(1.0f, 0.2f, 0.2f), float3(0.2f, 1.0f, 0.2f), float3(0.2f, 0.4f, 1.0f),
        float3(1.0f, 1.0f, 0.2f), float3(1.0f, 0.2f, 1.0f), float3(0.2f, 1.0f, 1.0f),
    };
    return colors[min(level, 5u)];
}

// Marches the clipmap from the camera and shows the first voxel it meets.
float3 DebugMarch(uint2 fullPixel)
{
    const float2 uv = (float2(fullPixel) + 0.5f) / float2(gFullWidth, gFullHeight);
    const float3 dir = normalize(ReconstructWorldPosVoxel(uv, 0.5f) - gCameraPos);

    float t = 0.0f;
    [loop]
    for (uint step = 0; step < 512; ++step)
    {
        const float3 p = gCameraPos + dir * t;
        const uint level = FinestLevelContaining(p, 1.0f);
        if (level >= gLevelCount)
            break;

        const float4 albedo = SampleVoxelLevel(gVoxelAlbedo, gLinearClamp, p, level);
        if (albedo.a > 0.5f)
        {
            if (gDebugView == kDebugVoxelLevel)
                return LevelColor(level) * (0.35f + 0.65f * albedo.rgb);
            if (gDebugView == kDebugVoxelAlbedo)
                return albedo.rgb;
            const float4 radiance = SampleVoxelLevel(gRadiance, gLinearClamp, p, level);
            return radiance.rgb / max(radiance.a, 1.0e-3f);
        }
        t += VoxelSizeOf(level) * 0.5f;
    }
    return VoxelSkyRadiance(dir) * 0.25f;
}

uint2 TraceToFullPixel(uint2 tracePixel)
{
    const uint2 pixel = gHalfResolution != 0 ? tracePixel * 2u : tracePixel;
    return min(pixel, uint2(gFullWidth - 1, gFullHeight - 1));
}

bool IsSky(float depth)
{
    return depth <= 0.0f || depth >= 1.0f;
}

// A cone of roughly the GGX lobe's width: tight enough to keep a polished floor
// readable, wide enough at the threshold to blend into the diffuse look.
float ReflectionConeTan(float roughness)
{
    return clamp(roughness * roughness * 0.9f + 0.02f, 0.02f, 1.0f);
}

[numthreads(8, 8, 1)]
void TraceCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gTraceWidth || id.y >= gTraceHeight)
        return;

    const uint2 fullPixel = TraceToFullPixel(id.xy);

    if (gDebugView >= kDebugVoxelAlbedo)
    {
        gOutput0[id.xy] = float4(DebugMarch(fullPixel), 1.0f);
        gOutput1[id.xy] = 0.0f.xxxx;
        return;
    }

    const float4 normalDepth = gGBufferNormal.Load(int3(fullPixel, 0));
    if (IsSky(normalDepth.z))
    {
        gOutput0[id.xy] = 0.0f.xxxx;
        gOutput1[id.xy] = 0.0f.xxxx;
        return;
    }

    const float3 N = DecodeOctNormalVoxel(normalDepth.xy);
    const float2 uv = (float2(fullPixel) + 0.5f) / float2(gFullWidth, gFullHeight);
    const float3 P = ReconstructWorldPosVoxel(uv, normalDepth.z);

    const uint level = FinestLevelContaining(P, 2.0f);
    if (level >= gLevelCount)
    {
        // Beyond the clipmap there is nothing to trace; open sky over the
        // hemisphere is the best guess for something that far out.
        gOutput0[id.xy] = float4(VoxelSkyRadiance(N) * saturate(0.5f + 0.5f * N.z), 1.0f);
        gOutput1[id.xy] = float4(VoxelSkyRadiance(reflect(normalize(P - gCameraPos), N)), 0.0f);
        return;
    }

    // Leave the surface by a voxel or so of the level it sits in, so the cones
    // do not start inside the voxels this very surface filled.
    const float voxelSize = VoxelSizeOf(level);
    const float offset = gSurfaceOffset * voxelSize;
    const float3 origin = P + N * offset;
    const float maxDistance = gMaxTraceDistance;

    float occlusion;
    const float3 diffuse = TraceDiffuse(gRadiance, gLinearClamp, origin, N, 0.5f * voxelSize,
                                        maxDistance, gDiffuseCones, 0.0f, occlusion);
    gOutput0[id.xy] = float4(diffuse, 1.0f);

    float4 reflection = 0.0f.xxxx;
    if (gSpecularEnabled != 0)
    {
        const float roughness = saturate(gGBufferMaterial.Load(int3(fullPixel, 0)).r);
        if (roughness <= gSpecularRoughnessThreshold)
        {
            const float3 V = normalize(gCameraPos - P);
            const float3 R = reflect(-V, N);
            const float4 cone = ConeTrace(gRadiance, gLinearClamp, origin, R, ReflectionConeTan(roughness),
                                          0.5f * voxelSize, maxDistance, true);
            reflection = float4(cone.rgb, 1.0f);
        }
    }
    gOutput1[id.xy] = reflection;
}

struct ResolveTap
{
    float4 Diffuse;
    float4 Reflection;
    float  Weight;
};

[numthreads(8, 8, 1)]
void ResolveCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gFullWidth || id.y >= gFullHeight)
        return;

    const uint2 pixel = id.xy;
    const float4 normalDepth = gGBufferNormal.Load(int3(pixel, 0));

    if (gDebugView >= kDebugVoxelAlbedo)
    {
        const uint2 tracePixel = gHalfResolution != 0 ? pixel / 2u : pixel;
        gOutput0[pixel] = gTraceDiffuse.Load(int3(min(tracePixel, uint2(gTraceWidth - 1, gTraceHeight - 1)), 0));
        gOutput1[pixel] = 0.0f.xxxx;
        gOutput2[pixel] = 0.0f.xxxx;
        return;
    }

    if (IsSky(normalDepth.z))
    {
        gOutput0[pixel] = 0.0f.xxxx;
        gOutput1[pixel] = 0.0f.xxxx;
        gOutput2[pixel] = 0.0f.xxxx;
        return;
    }

    const float3 N = DecodeOctNormalVoxel(normalDepth.xy);
    const float2 uv = (float2(pixel) + 0.5f) / float2(gFullWidth, gFullHeight);
    const float3 P = ReconstructWorldPosVoxel(uv, normalDepth.z);

    float4 diffuse = 0.0f.xxxx;
    float4 reflection = 0.0f.xxxx;

    if (gHalfResolution != 0)
    {
        // Joint bilateral upsample from the four nearest trace pixels. The plane
        // test scales with distance so far surfaces are not rejected for the
        // depth spread a half-resolution pixel covers there.
        const float2 traceCoord = (float2(pixel) + 0.5f) * 0.5f - 0.5f;
        const int2 base = int2(floor(traceCoord));
        const float2 f = traceCoord - float2(base);
        const float planeTolerance = max(0.02f, 0.01f * distance(P, gCameraPos));

        float weightSum = 0.0f;
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            const int2 offset = int2(i & 1, i >> 1);
            const int2 tap = clamp(base + offset, int2(0, 0), int2(gTraceWidth - 1, gTraceHeight - 1));
            const uint2 tapFull = TraceToFullPixel(uint2(tap));
            const float4 tapNormalDepth = gGBufferNormal.Load(int3(tapFull, 0));
            if (IsSky(tapNormalDepth.z))
                continue;

            const float2 tapUv = (float2(tapFull) + 0.5f) / float2(gFullWidth, gFullHeight);
            const float3 tapP = ReconstructWorldPosVoxel(tapUv, tapNormalDepth.z);
            const float3 tapN = DecodeOctNormalVoxel(tapNormalDepth.xy);

            const float bilinear = (offset.x ? f.x : 1.0f - f.x) * (offset.y ? f.y : 1.0f - f.y);
            const float planeWeight = exp(-abs(dot(N, tapP - P)) / planeTolerance);
            const float normalWeight = pow(saturate(dot(N, tapN)), 8.0f);
            const float w = max(bilinear, 1.0e-3f) * planeWeight * normalWeight;

            diffuse    += gTraceDiffuse.Load(int3(tap, 0)) * w;
            reflection += gTraceReflection.Load(int3(tap, 0)) * w;
            weightSum  += w;
        }

        if (weightSum > 1.0e-4f)
        {
            diffuse /= weightSum;
            reflection /= weightSum;
        }
        else
        {
            // No neighbour lies on this surface (a thin edge): the nearest tap
            // is still a better answer than black.
            const int2 nearest = clamp(int2(round(traceCoord)), int2(0, 0), int2(gTraceWidth - 1, gTraceHeight - 1));
            diffuse = gTraceDiffuse.Load(int3(nearest, 0));
            reflection = gTraceReflection.Load(int3(nearest, 0));
        }
    }
    else
    {
        diffuse = gTraceDiffuse.Load(int3(pixel, 0));
        reflection = gTraceReflection.Load(int3(pixel, 0));
    }

    gOutput0[pixel] = float4(max(diffuse.rgb, 0.0f.xxx), 1.0f);

    // Reflection weighted the way the SSSR composite weights its own, faded out
    // over the last quarter below the roughness threshold so the cut-off is not
    // a visible seam.
    float3 weighted = 0.0f.xxx;
    if (gSpecularEnabled != 0 && reflection.a > 0.0f)
    {
        const float4 material = gGBufferMaterial.Load(int3(pixel, 0));
        const float roughness = saturate(material.r);
        const float metallic = material.g;
        const float3 albedo = gGBufferAlbedo.Load(int3(pixel, 0)).rgb;
        const float3 F0 = PteroComputeF0(albedo, metallic, PteroDecodeSurfaceSpecular(normalDepth.w));
        const float NoV = saturate(dot(N, normalize(gCameraPos - P)));
        const float fade = 1.0f - smoothstep(gSpecularRoughnessThreshold * 0.75f, gSpecularRoughnessThreshold, roughness);
        weighted = max(reflection.rgb, 0.0f.xxx) * VoxelEnvBrdfApprox(F0, roughness, NoV) * fade;
    }

    gOutput1[pixel] = float4(weighted, 1.0f);
    gOutput2[pixel] = float4(max(reflection.rgb, 0.0f.xxx), saturate(reflection.a));
}
