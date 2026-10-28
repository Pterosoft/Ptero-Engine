// SkyModel.hlsli
// The sky's radiance in any direction: the Hosek-Wilkie daylight sky, the moonlit
// sky (the same model lit by the moon), and the night gradient under them - the sky
// the sky pass draws, without its discs, stars and twilight glow. The sky pass uses
// PteroHosekWilkieF; the GI passes take PteroSkyRadiance for rays that escape, so
// what they bounce is the sky that is on screen. Filled by MakeGpuSkyModel
// (GpuSkyModel.h), already scaled by the GI pre-exposure where a pass wants it.

#ifndef PTERO_SKY_MODEL_HLSLI
#define PTERO_SKY_MODEL_HLSLI

// Mirrors GpuSkyModel in GpuSkyModel.h (24 float4s).
struct PteroSkyModel
{
    float4 SunModel[9];     // Hosek-Wilkie coefficients A..I, xyz = r, g, b
    float4 SunScale;        // xyz: per-channel scale on F; zero at night. w: ground r
    float4 MoonModel[9];    // the same for the moon's elevation
    float4 MoonScale;       // zero when the moon is down, new or off. w: ground g
    float4 SunDirection;    // xyz: from the scene toward the sun
    float4 MoonDirection;   // xyz: from the scene toward the moon
    float4 NightZenith;     // xyz: the night gradient's zenith colour. w: ground b
    float4 NightHorizon;    // xyz: and its horizon colour
};

// The lit ground under the horizon (HosekWilkieResult::GroundRadiance).
float3 PteroGroundRadiance(PteroSkyModel sky)
{
    return float3(sky.SunScale.w, sky.MoonScale.w, sky.NightZenith.w);
}

// How much of the ground shows through the haze toward `dir`: none at the horizon,
// where the ground is infinitely far and the view is all air, rising to nearly all
// of it a few degrees down. Beer-Lambert over the path to a flat ground, with the
// camera height and the haze's density folded into one length. Zero above the
// horizon.
float PteroGroundVisibility(float3 dir)
{
    static const float kHazeOverHeight = 0.02f;
    const float down = -dir.z;
    return down > 0.0f ? exp(-kHazeOverHeight / down) : 0.0f;
}

// `radiance` toward `dir` with the ground in front of it below the horizon. The sky
// model there carries the horizon's own colour and so is the haze the ground fades
// into in the distance.
float3 PteroApplyGround(float3 radiance, float3 groundRadiance, float3 dir)
{
    return lerp(radiance, groundRadiance, PteroGroundVisibility(dir));
}

// The Hosek-Wilkie model's F(theta, gamma) for all three channels at once
// (ArHosekSkyModel_GetRadianceInternal). theta is the zenith angle, gamma the
// angle from the light (the sun, or the moon for the moonlit sky). The model is
// only defined above the horizon, so lower directions take the horizon's value.
float3 PteroHosekWilkieF(float3 dir, float3 lightDirection, float4 model[9])
{
    const float cosTheta = max(dir.z, 0.0f);
    const float cosGamma = dot(dir, lightDirection);
    // gamma from the chord, which keeps its precision right up against the light
    // where acos does not.
    const float gamma = 2.0f * asin(saturate(0.5f * length(dir - lightDirection)));

    const float3 A = model[0].xyz, B = model[1].xyz, C = model[2].xyz;
    const float3 D = model[3].xyz, E = model[4].xyz, F = model[5].xyz;
    const float3 G = model[6].xyz, H = model[7].xyz, I = model[8].xyz;

    const float3 expM = exp(E * gamma);
    const float  rayM = cosGamma * cosGamma;
    const float3 mieM = (1.0f + rayM) / pow(max(1.0f + I * I - 2.0f * I * cosGamma, 1e-6f), 1.5f);
    const float  zenith = sqrt(cosTheta);

    return (1.0f + A * exp(B / (cosTheta + 0.01f)))
         * (C + D * expM + F * rayM + G * mieM + H * zenith);
}

// The night gradient: zenith to horizon by world elevation (Z up).
float3 PteroNightGradient(float3 zenithColor, float3 horizonColor, float3 dir)
{
    const float t = saturate(dir.z * 2.0f); // horizon = 0, mid-sky = 1
    return lerp(horizonColor, zenithColor, t * t);
}

// The sky's radiance toward `dir` (unit, world space).
float3 PteroSkyRadiance(PteroSkyModel sky, float3 dir)
{
    float3 radiance = PteroNightGradient(sky.NightZenith.xyz, sky.NightHorizon.xyz, dir);
    [branch]
    if (any(sky.SunScale.xyz > 0.0f))
        radiance += max(PteroHosekWilkieF(dir, sky.SunDirection.xyz, sky.SunModel), 0.0f) * sky.SunScale.xyz;
    [branch]
    if (any(sky.MoonScale.xyz > 0.0f))
        radiance += max(PteroHosekWilkieF(dir, sky.MoonDirection.xyz, sky.MoonModel), 0.0f) * sky.MoonScale.xyz;
    return PteroApplyGround(radiance, PteroGroundRadiance(sky), dir);
}

#endif // PTERO_SKY_MODEL_HLSLI
