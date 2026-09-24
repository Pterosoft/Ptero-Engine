//
// Description : Utilties for waves displacement
//
// Ported from tuxalin/water-shader (shaders/hlsl/water/displacement.cginc).
// Y-up -> Z-up: horizontal positions are XY and height is Z.  The original's
// USE_DISPLACEMENT / USE_FILTERING compile-time switches are runtime flags
// here (material "useDisplacement" / "useFiltering").
//
// Requires Noise.hlsli, Normals.hlsli and Waves.hlsli.
//

#ifndef WATER_DISPLACEMENT_HLSLI
#define WATER_DISPLACEMENT_HLSLI

float2 GetNoise(in float2 position, in float2 timedWindDir)
{
    float2 noise;
    noise.x = snoise(position * 0.015 + timedWindDir * 0.0005); // large and slower noise
    noise.y = snoise(position * 0.1 + timedWindDir * 0.002);    // smaller and faster noise
    return saturate(noise);
}

void AdjustWavesValues(in float2 noise, inout float4 wavesNoise, inout float4 wavesIntensity)
{
    wavesNoise = wavesNoise * float4(noise.y * 0.25, noise.y * 0.25, noise.x + noise.y, noise.y);
    wavesIntensity = wavesIntensity + float4(saturate(noise.y - noise.x), noise.x, noise.y, noise.x + noise.y);
    wavesIntensity = clamp(wavesIntensity, 0.01, 10);
}

// uv in texture space, normal in world space
float3 ComputeNormal(Texture2D normalTexture, SamplerState texSampler, float2 worldPos, float2 texCoord,
    float3 normal, float3 tangent, float3 bitangent,
    float4 wavesNoise, float4 wavesIntensity, float2 timedWindDir,
    bool useDisplacement, bool useFiltering)
{
    float2 noise = GetNoise(worldPos, timedWindDir * 0.5);
    AdjustWavesValues(noise, wavesNoise, wavesIntensity);

    float2 texCoords[4] = { texCoord * 1.6 + timedWindDir * 0.064 + wavesNoise.x,
                            texCoord * 0.8 + timedWindDir * 0.032 + wavesNoise.y,
                            texCoord * 0.5 + timedWindDir * 0.016 + wavesNoise.z,
                            texCoord * 0.3 + timedWindDir * 0.008 + wavesNoise.w };

    float3 wavesNormal = float3(0, 0, 1);
    if (useDisplacement)
    {
        normal = normalize(normal);
        tangent = normalize(tangent);
        bitangent = normalize(bitangent);
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            wavesNormal += ComputeSurfaceNormal(normal, tangent, bitangent, normalTexture, texSampler, texCoords[i], useFiltering) * wavesIntensity[i];
        }
    }
    else
    {
        // The original accumulated tangent-space normals onto a Y-up vector
        // and then swizzled xzy.  In a Z-up world tangent space already lines
        // up with world space (the map's Z is up), so no swizzle is needed.
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            float2 duv1 = ddx(texCoords[i]);
            float2 duv2 = ddy(texCoords[i]);
            wavesNormal += UnpackNormal(normalTexture.SampleGrad(texSampler, texCoords[i], duv1, duv2)) * wavesIntensity[i];
        }
    }

    return wavesNormal;
}

float ComputeNoiseHeight(Texture2D heightTexture, SamplerState texSampler, float4 wavesIntensity, float4 wavesNoise,
    float2 texCoord, float2 noise, float2 timedWindDir)
{
    AdjustWavesValues(noise, wavesNoise, wavesIntensity);

    float2 texCoords[4] = { texCoord * 1.6 + timedWindDir * 0.064 + wavesNoise.x,
                            texCoord * 0.8 + timedWindDir * 0.032 + wavesNoise.y,
                            texCoord * 0.5 + timedWindDir * 0.016 + wavesNoise.z,
                            texCoord * 0.3 + timedWindDir * 0.008 + wavesNoise.w };
    float height = 0;
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        height += heightTexture.SampleLevel(texSampler, texCoords[i], 0).x * wavesIntensity[i];
    }

    return height;
}

float3 ComputeDisplacement(float3 worldPos, float cameraDistance, float2 noise, float timer,
    float4 waveSettings, float4 waveAmplitudes, float4 wavesIntensity, float4 waveNoise,
    out float3 normal, out float3 tangent)
{
    float2 windDir = waveSettings.xy;
    float waveSteepness = waveSettings.z;
    float waveTiling = waveSettings.w;

    //TODO: improve motion/simulation instead of just noise
    //TODO: fix UV due to wave distortion

    wavesIntensity = normalize(wavesIntensity);
    waveNoise = float4(noise.x - noise.x * 0.2 + noise.y * 0.1, noise.x + noise.y * 0.5 - noise.y * 0.1, noise.x, noise.x) * waveNoise;
    float4 wavelengths = float4(1, 4, 3, 6) + waveNoise;
    float4 amplitudes = waveAmplitudes + float4(0.5, 1, 4, 1.5) * waveNoise;

    // reduce wave intensity base on distance to reduce aliasing
    wavesIntensity *= 1.0 - saturate(float4(cameraDistance / 120.0, cameraDistance / 150.0, cameraDistance / 170.0, cameraDistance / 400.0));

    // compute position and normal from several sine and gerstner waves
    tangent = normal = float3(0, 0, 1);
    [unroll]
    for (int i = 2; i < 4; ++i)
    {
        float A = wavesIntensity[i] * amplitudes[i];
        float3 vals = SineWaveValues(worldPos.xy * waveTiling, windDir, A, wavelengths[i], timer);
        normal += wavesIntensity[i] * SineWaveNormal(windDir, A, vals);
        tangent += wavesIntensity[i] * SineWaveTangent(windDir, A, vals);
        worldPos.z += SineWaveDelta(A, vals);
    }

    // using normalized wave steepness, tranform to Q
    // (amplitude guarded: an all-zero amplitude would divide by zero)
    float2 Q = waveSteepness / ((2 * 3.14159265 / wavelengths.xy) * max(amplitudes.xy, 1e-4));
    [unroll]
    for (int j = 0; j < 2; ++j)
    {
        float A = wavesIntensity[j] * amplitudes[j];
        float3 vals = GerstnerWaveValues(worldPos.xy * waveTiling, windDir, A, wavelengths[j], Q[j], timer);
        normal += wavesIntensity[j] * GerstnerWaveNormal(windDir, A, Q[j], vals);
        tangent += wavesIntensity[j] * GerstnerWaveTangent(windDir, A, Q[j], vals);
        worldPos += GerstnerWaveDelta(windDir, A, Q[j], vals);
    }

    normal = normalize(normal);
    tangent = normalize(tangent);
    if (length(wavesIntensity) < 0.01)
    {
        normal = float3(0, 0, 1);
        tangent = float3(0, 1, 0);
    }

    return worldPos;
}

#endif // WATER_DISPLACEMENT_HLSLI
