// Ssr.hlsl
// Screen-space reflections.
//
// Runs after the scene is fully lit (deferred resolve, sky, clouds, water) and before
// anti-aliasing, so what gets reflected is finished shading rather than a partial frame.
// The pass reads the scene colour and writes the composited result to its own target; the
// caller copies that back over the scene colour, which keeps every downstream pass reading
// the same texture it always did.
//
// Rays are marched in world space rather than in screen space. A screen-space DDA is
// cheaper, but its step length means something different at every depth, so a single
// "thickness" tolerance cannot hold across the frustum. Marching in world units keeps
// Thickness an actual distance, which is far easier to author against.

// Same F0 the deferred resolve uses, so a surface does not reflect one amount under direct
// light and a different amount in its own reflection.
#include "SurfaceSpecular.hlsli"

cbuffer SsrConstants : register(b0)
{
    float4x4 gViewProj;      // world -> clip, matching the jittered frame
    float4x4 gInvViewProj;   // clip -> world
    float3   gCameraPos;     float gIntensity;
    uint     gFrameWidth;    uint  gFrameHeight;
    int      gMaxSteps;      float gStepSize;
    float    gStepGrowth;    float gThickness;
    float    gMaxRoughness;  int   gRefineSteps;
    float    gMaxDistance;   float gEdgeFadeStart;
    int      gDebugView;     float _SsrPad0;
};

Texture2D gSceneColor : register(t0);
Texture2D gDepth      : register(t1);  // R32_FLOAT, non-linear [0,1]
Texture2D gNormal     : register(t2);  // .xy = oct-encoded world normal
Texture2D gMaterial   : register(t3);  // .r roughness, .g metallic, .b ao
Texture2D gAlbedo     : register(t4);

RWTexture2D<float4> gOutput : register(u0);

SamplerState gLinearClamp : register(s0);

// Both helpers below match DeferredLighting.hlsl exactly. The two passes reconstruct the
// same surfaces from the same buffers, so any drift between them would show up as
// reflections that do not sit on the geometry they belong to.
float3 ReconstructWorldPosition(float2 uv, float depth)
{
    float4 ndc = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
    float4 worldPos = mul(ndc, gInvViewProj);
    return worldPos.xyz / worldPos.w;
}

float3 DecodeOctNormal(float2 encoded)
{
    float2 oct = encoded * 2.0f - 1.0f;
    float3 n = float3(oct, 1.0f - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0f)
    {
        n.xy = (1.0f - abs(n.yx)) * (float2(n.xy >= 0.0f) * 2.0f - 1.0f);
    }
    return normalize(n);
}

float2 ClipToUv(float3 ndc)
{
    return float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
}

// Projects a world point and reports how far in front of the depth buffer it sits.
// Positive means the ray has passed behind visible geometry, which is what a hit looks
// like. Returns false when the point leaves the frustum and nothing can be said about it.
bool SampleSceneAlongRay(float3 worldPos, out float2 uv, out float depthDifference)
{
    uv = 0.0f.xx;
    depthDifference = 0.0f;

    const float4 clip = mul(float4(worldPos, 1.0f), gViewProj);
    if (clip.w <= 0.0f)
        return false;

    const float3 ndc = clip.xyz / clip.w;
    if (max(abs(ndc.x), abs(ndc.y)) > 1.0f || ndc.z < 0.0f || ndc.z > 1.0f)
        return false;

    uv = ClipToUv(ndc);

    const float sceneDepth = gDepth.SampleLevel(gLinearClamp, uv, 0.0f).r;
    if (sceneDepth >= 1.0f)
        return false;   // sky: nothing solid to intersect

    const float3 scenePos = ReconstructWorldPosition(uv, sceneDepth);
    depthDifference = distance(gCameraPos, worldPos) - distance(gCameraPos, scenePos);
    return true;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gFrameWidth || id.y >= gFrameHeight)
        return;

    const int2   pixel = int2(id.xy);
    const float2 uv    = (float2(pixel) + 0.5f.xx) / float2(gFrameWidth, gFrameHeight);

    const float3 sceneColor = gSceneColor.Load(int3(pixel, 0)).rgb;
    const float  rawDepth   = gDepth.Load(int3(pixel, 0)).r;
    const float4 material   = gMaterial.Load(int3(pixel, 0));

    const float roughness = material.r;
    const float metallic  = material.g;

    // Glass repacks this buffer (.a = encoded IOR, .b = dispersion), so its roughness and
    // metallic channels do not mean what they do elsewhere. The glass pass draws its own
    // reflections; reading these as PBR values here would produce nonsense.
    const bool isGlass = material.a > (1.0f / 255.0f) && material.b < 0.75f;

    float3 reflection = 0.0f.xxx;
    float  weight     = 0.0f;

    // Sky, glass, and anything too rough to carry a coherent reflection are left alone.
    if (rawDepth < 1.0f && !isGlass && roughness <= gMaxRoughness)
    {
        const float3 N = DecodeOctNormal(gNormal.Load(int3(pixel, 0)).xy);
        const float3 P = ReconstructWorldPosition(uv, rawDepth);
        const float3 V = normalize(gCameraPos - P);
        const float3 R = reflect(-V, N);

        // Start slightly off the surface, otherwise the first sample reads the pixel's own
        // depth and every surface immediately self-intersects.
        float3 rayPos    = P + N * 0.02f;
        float3 previous  = rayPos;
        float  step      = max(gStepSize, 1.0e-3f);
        float  travelled = 0.0f;

        float2 hitUv = 0.0f.xx;
        bool   hit   = false;

        [loop]
        for (int i = 0; i < gMaxSteps; ++i)
        {
            if (travelled >= gMaxDistance)
                break;

            previous = rayPos;
            rayPos  += R * step;
            travelled += step;
            // Grow the step as the ray travels: near the surface precision matters, far
            // away it does not, and a constant step spends most of its budget on the part
            // of the ray least likely to hit anything.
            step *= max(gStepGrowth, 1.0f);

            float2 stepUv;
            float  difference;
            if (!SampleSceneAlongRay(rayPos, stepUv, difference))
                continue;

            // The tolerance has to include the step length: a thin tolerance with a long
            // step tunnels straight through geometry between samples.
            if (difference > 0.0f && difference < gThickness + step)
            {
                hit   = true;
                hitUv = stepUv;
                break;
            }
        }

        if (hit)
        {
            // Binary search between the last miss and the first hit. The march deliberately
            // uses coarse steps, so without this the reflection lands wherever the step
            // happened to stop rather than on the actual intersection.
            float3 lo = previous;
            float3 hi = rayPos;
            [loop]
            for (int r = 0; r < gRefineSteps; ++r)
            {
                const float3 mid = (lo + hi) * 0.5f;

                float2 midUv;
                float  midDifference;
                if (!SampleSceneAlongRay(mid, midUv, midDifference))
                    break;

                hitUv = midUv;
                if (midDifference > 0.0f)
                    hi = mid;   // already behind geometry, pull back
                else
                    lo = mid;
            }

            reflection = gSceneColor.SampleLevel(gLinearClamp, hitUv, 0.0f).rgb;

            const float3 albedo = gAlbedo.Load(int3(pixel, 0)).rgb;
            const float specular = PteroDecodeSurfaceSpecular(gNormal.Load(int3(pixel, 0)).w);
            const float3 F0 = PteroComputeF0(albedo, metallic, specular);
            const float3 fresnel = F0 + (1.0f.xxx - F0) * pow(1.0f - saturate(dot(N, V)), 5.0f);

            // A reflection that leaves the screen has no data behind it, so fade it out
            // rather than clamping to the edge and smearing the border pixel.
            const float2 edge = abs(hitUv * 2.0f - 1.0f);
            const float edgeFade = 1.0f - smoothstep(gEdgeFadeStart, 1.0f, max(edge.x, edge.y));

            // Approaching the roughness cutoff, blend out instead of stopping dead - a hard
            // cutoff shows up as a visible seam across a surface with varying roughness.
            const float roughFade = 1.0f - smoothstep(gMaxRoughness * 0.5f, gMaxRoughness, roughness);

            // Long rays accumulate the most error, so trust them least.
            const float distanceFade = 1.0f - saturate(travelled / max(gMaxDistance, 1.0e-3f));

            weight = edgeFade * roughFade * distanceFade * gIntensity;
            reflection *= fresnel;
        }
    }

    float3 result = sceneColor + reflection * weight;
    if (gDebugView == 1)
        result = reflection * weight;      // reflection contribution alone
    else if (gDebugView == 2)
        result = weight.xxx;               // where SSR is trusted, and how much

    gOutput[pixel] = float4(result, 1.0f);
}
