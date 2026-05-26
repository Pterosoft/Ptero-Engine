
// RadianceProbes_Debug.hlsl  -  vs_6_5 / ps_6_5
// Renders real world-space debug spheres for each radiance probe using an
// instanced sphere mesh. The sphere shading visualizes either diffuse probe
// irradiance or a specular-style directional response.

#include "RadianceProbes_Common.hlsli"

StructuredBuffer<ProbeSH> t_ProbeSH : register(t0);

struct VSInput
{
    float3 Position : POSITION;
};

struct PSInput
{
    float4 Position   : SV_Position;
    float3 WorldPos   : TEXCOORD0;
    float3 Normal     : TEXCOORD1;
    uint   ProbeIndex : TEXCOORD2;
};

PSInput VSMain(VSInput input, uint instanceID : SV_InstanceID)
{
    PSInput output;

    float3 probeCenter = ProbeCoordToWorld(ProbeIndexToCoord(instanceID));
    float3 localNormal = normalize(input.Position);
    float3 worldPos    = probeCenter + input.Position * g_DebugSphereRadius;

    output.Position   = mul(float4(worldPos, 1.0f), g_ViewProj);
    output.WorldPos   = worldPos;
    output.Normal     = localNormal;
    output.ProbeIndex = instanceID;
    return output;
}

float4 PSMain(PSInput input) : SV_Target
{
    float3 N = normalize(input.Normal);
    float3 V = normalize(g_CameraPos - input.WorldPos);
    ProbeSH sh = t_ProbeSH[input.ProbeIndex];

    float3 directionalColor;
    if (g_DebugLightingMode == 0)
    {
        directionalColor = SHEvaluateDiffuseIrradiance(sh, N);
    }
    else
    {
        float3 R = reflect(-V, N);
        directionalColor = SHEvaluate(sh, R);
        directionalColor *= pow(saturate(dot(N, V)), 24.0f) * 1.5f;
    }

    directionalColor = max(directionalColor, 0.0f);
    const float kL0ToAverageRadiance = 1.0f / (SHBasis0() * 4.0f * 3.14159265f);
    float3 averageColor = max(sh.c[0].xyz * kL0ToAverageRadiance, 0.0f);
    float3 directionalHueSource = max(SHEvaluate(sh, normalize(float3(N.x, N.y, max(N.z * 0.35f + 0.65f, 0.05f)))), 0.0f);

    float directionalLuma = dot(directionalColor, float3(0.2126f, 0.7152f, 0.0722f));
    float averageLuma = dot(averageColor, float3(0.2126f, 0.7152f, 0.0722f));
    float luminance = max(directionalLuma, averageLuma);
    if (luminance < 0.001f)
    {
        directionalColor = float3(1.0f, 0.15f, 1.0f);
    }
    else
    {
        float averageMax = max(averageColor.r, max(averageColor.g, averageColor.b));
        float directionalMax = max(directionalHueSource.r, max(directionalHueSource.g, directionalHueSource.b));
        float3 averageHue = averageColor / max(averageMax, 1e-4f);
        float3 directionalHue = directionalHueSource / max(directionalMax, 1e-4f);
        float3 hue = max(lerp(averageHue, directionalHue, 0.75f), 1e-4f.xxx);
        float energy = saturate(log2(1.0f + luminance) / 3.5f);
        float facing = saturate(dot(N, V) * 0.5f + 0.5f);
        float horizon = saturate(1.0f - abs(N.z));
        directionalColor = hue * lerp(0.45f, 1.0f, energy) * lerp(0.7f, 1.05f, facing) * lerp(0.85f, 1.15f, horizon);
    }

    return float4(saturate(directionalColor), 0.95f);
}