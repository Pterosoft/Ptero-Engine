//
// Description : Foam color based on water depth, near the shore
//
// Ported from tuxalin/water-shader (shaders/hlsl/water/foam.cginc).
// Z-up: horizontal positions are XY.  The high-wave foam threshold
// (foamRanges.z) is compared against the height above the water entity's rest
// level instead of absolute world height, so it works at any water elevation.
//
// Requires Noise.hlsli.
//

#ifndef WATER_FOAM_HLSLI
#define WATER_FOAM_HLSLI

float FoamColor(Texture2D tex, SamplerState texSampler, float2 texCoord, float2 texCoord2, float2 ranges, float2 factors,
    float waterDepth, float baseColor)
{
    float f1 = tex.Sample(texSampler, texCoord).r;
    float f2 = tex.Sample(texSampler, texCoord2).r;
    return lerp(f1 * factors.x + f2 * factors.y, baseColor, smoothstep(ranges.x, ranges.y, waterDepth));
}

// surfacePosition, depthPosition, eyeVec in world space
// waterDepth is the horizontal water depth in world space
// surfaceHeight is the surface's height above the water's rest level
float FoamValue(Texture2D shoreTexture, Texture2D foamTexture, SamplerState texSampler, float2 foamTiling,
    float4 foamNoise, float2 foamSpeed, float3 foamRanges, float maxAmplitude,
    float3 surfacePosition, float3 depthPosition, float3 eyeVec, float waterDepth, float surfaceHeight,
    float2 timedWindDir, float timer)
{
    float2 position = (surfacePosition.xy + eyeVec.xy * 0.1) * 0.5;

    float s = sin(timer * 0.01 + depthPosition.x);
    float2 texCoord = position + timer * 0.01 * foamSpeed + s * 0.05;
    s = sin(timer * 0.01 + depthPosition.y);
    float2 texCoord2 = (position + timer * 0.015 * foamSpeed + s * 0.05) * -0.5; // also flip
    float2 texCoord3 = texCoord * foamTiling.x;
    float2 texCoord4 = (position + timer * 0.015 * -foamSpeed * 0.3 + s * 0.05) * -0.5 * foamTiling.x; // reverse direction
    texCoord *= foamTiling.y;
    texCoord2 *= foamTiling.y;

    float2 ranges = foamRanges.xy;
    ranges.x += snoise(surfacePosition.xy + foamNoise.z * timedWindDir) * foamNoise.x;
    ranges.y += snoise(surfacePosition.xy + foamNoise.w * timedWindDir) * foamNoise.y;
    ranges = clamp(ranges, 0.0, 10.0);

    float foamEdge = max(ranges.x, ranges.y);
    float deepFoam = FoamColor(foamTexture, texSampler, texCoord, texCoord2, float2(ranges.x, foamEdge), float2(1.0, 0.5), waterDepth, 0.0);
    float foam = FoamColor(shoreTexture, texSampler, texCoord3 * 0.25, texCoord4, float2(0.0, ranges.x), float2(0.75, 1.5), waterDepth, deepFoam);

    // high waves foam
    // (sampled unconditionally: implicit-derivative samples must stay out of
    // per-pixel branches; amount is zero below the threshold, as before)
    float amount = (surfaceHeight - foamRanges.z > 0.0001f)
        ? saturate((surfaceHeight - foamRanges.z) / max(maxAmplitude, 1e-4)) * 0.25
        : 0.0;
    foam += (shoreTexture.Sample(texSampler, texCoord3).x + shoreTexture.Sample(texSampler, texCoord4).x * 0.5f) * amount;

    return foam;
}

#endif // WATER_FOAM_HLSLI
