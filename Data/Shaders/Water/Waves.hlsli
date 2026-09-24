//
// Description : Utilties for waves simulation
//
// based on GPU Gems Chapter 1. Effective Water Simulation from Physical Models, by Mark Finch and Cyan Worlds
//
// Ported from tuxalin/water-shader (shaders/hlsl/water/waves.cginc).  The
// original is Y-up; the engine is Z-up, so every vector here has its Y and Z
// components swapped relative to the source: the wave direction D lies in the
// XY plane and height is Z.
//

#ifndef WATER_WAVES_HLSLI
#define WATER_WAVES_HLSLI

float3 GerstnerWaveValues(float2 position, float2 D, float amplitude, float wavelength, float Q, float timer)
{
    float w = 2 * 3.14159265 / wavelength;
    float dotD = dot(position, D);
    float v = w * dotD + timer;
    return float3(cos(v), sin(v), w);
}

float3 GerstnerWaveNormal(float2 D, float A, float Q, float3 vals)
{
    float C = vals.x;
    float S = vals.y;
    float w = vals.z;
    float WA = w * A;
    float WAC = WA * C;
    float3 normal = float3(-D.x * WAC, -D.y * WAC, 1.0 - Q * WA * S);
    return normalize(normal);
}

float3 GerstnerWaveTangent(float2 D, float A, float Q, float3 vals)
{
    float C = vals.x;
    float S = vals.y;
    float w = vals.z;
    float WA = w * A;
    float WAS = WA * S;
    float3 tangent = float3(Q * -D.x * D.y * WAS, 1.0 - Q * D.y * D.y * WAS, D.y * WA * C);
    return normalize(tangent);
}

float3 GerstnerWaveDelta(float2 D, float A, float Q, float3 vals)
{
    float C = vals.x;
    float S = vals.y;
    float QAC = Q * A * C;
    return float3(QAC * D.x, QAC * D.y, A * S);
}

float3 SineWaveValues(float2 position, float2 D, float amplitude, float wavelength, float timer)
{
    float w = 2 * 3.14159265 / wavelength;
    float dotD = dot(position, D);
    float v = w * dotD + timer;
    return float3(cos(v), sin(v), w);
}

float3 SineWaveNormal(float2 D, float A, float3 vals)
{
    float C = vals.x;
    float w = vals.z;
    float WA = w * A;
    float WAC = WA * C;
    float3 normal = float3(-D.x * WAC, -D.y * WAC, 1.0);
    return normalize(normal);
}

float3 SineWaveTangent(float2 D, float A, float3 vals)
{
    float C = vals.x;
    float w = vals.z;
    float WAC = w * A * C;
    float3 tangent = float3(0.0, 1.0, D.y * WAC);
    return normalize(tangent);
}

float SineWaveDelta(float A, float3 vals)
{
    return vals.y * A;
}

#endif // WATER_WAVES_HLSLI
