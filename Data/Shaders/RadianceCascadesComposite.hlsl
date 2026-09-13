cbuffer RadianceCascadesConstants : register(b0)
{
    uint  gFrameWidth;
    uint  gFrameHeight;
    uint  gFrameIndex;
    uint  gCascadeCount;

    uint  gProbeSpacingBase;
    uint  gRaysPerProbe;
    int   gDebugView;
    float gRayLengthBase;

    float gRayLengthScale;
    float gIntervalLengthScale;
    float gHysteresis;
    float gGiIntensity;

    float4x4 gViewProjInv;
    float4x4 gCurrViewProj;
    float4x4 gPrevViewProj;
    float3 gSunDir;
    float gProbeSpacingBaseFloat;

    float3 gSunColor;
    int    gNumPointLights;

    float3 gSkyColor;
    float  gSceneMaxDistance;

    float3 gCameraPos;
    float  gColorBleedingStrength;

    uint  gSparseProbeTableCapacity;
    uint  gSparseProbeCount;
    uint  gSparseProbeCellSize;
    uint  gSparseProbeSearchSteps;

    float gSparseProbeReuseStrength;
    float gRayBias;
    float gSpatialFilterStrength;
    float gHistoryClampScale;

    float gHistoryDepthSensitivity;
    float gHistoryNormalThreshold;
    float gSparsePadding0;
    float gSparsePadding1;
};

Texture2D<float4> gTrace : register(t0);
Texture2D<float4> gHistory : register(t1);
Texture2D<float4> gNormalDepth : register(t2);
Texture2D<float> gSceneDepth : register(t3);
RWTexture2D<float4> gOutput : register(u0);
SamplerState gLinearClamp : register(s0);

float3 DecodeOctNormal(float2 encoded)
{
    float2 oct = encoded * 2.0f - 1.0f;
    float3 n = float3(oct, 1.0f - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0f)
        n.xy = (1.0f - abs(n.yx)) * (float2(n.xy >= 0.0f) * 2.0f - 1.0f);
    return normalize(n);
}

float3 ReconstructWorldPosition(uint2 pixel, float depth)
{
    float2 uv = (float2(pixel) + 0.5f) / float2(gFrameWidth, gFrameHeight);
    float4 ndc = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
    float4 worldPos = mul(ndc, gViewProjInv);
    return worldPos.xyz / max(worldPos.w, 1e-6f);
}

bool IsInsideViewport(float2 uv)
{
    return all(uv >= 0.0f.xx) && all(uv <= 1.0f.xx);
}

float ComputeHistoryWeight(float currentDepth, float historyDepth, float3 currentNormal, float3 historyNormal)
{
    const float depthDelta = abs(currentDepth - historyDepth);
    const float depthWeight = 1.0f - saturate(depthDelta * max(gHistoryDepthSensitivity, 1.0f));
    const float normalWeight = saturate(dot(currentNormal, historyNormal));
    const float normalSimilarity = smoothstep(saturate(gHistoryNormalThreshold), 0.999f, normalWeight);
    return depthWeight * normalSimilarity;
}

float4 FilterNeighborhood(uint2 pixel, float centerDepth, float3 centerNormal, float4 centerGi)
{
    static const int2 offsets[4] =
    {
        int2(-1, 0),
        int2(1, 0),
        int2(0, -1),
        int2(0, 1)
    };

    float centerWeight = lerp(1.0f, 6.0f, saturate(gSpatialFilterStrength));
    float3 sum = centerGi.a > 0.0f ? centerGi.rgb * centerWeight : 0.0f.xxx;
    float totalWeight = centerGi.a > 0.0f ? centerWeight : 0.0f;

    [unroll]
    for (uint i = 0; i < 4; ++i)
    {
        int2 samplePixel = int2(pixel) + offsets[i];
        if (samplePixel.x < 0 || samplePixel.y < 0 || samplePixel.x >= int(gFrameWidth) || samplePixel.y >= int(gFrameHeight))
            continue;

        float4 sampleNormalDepth = gNormalDepth.Load(int3(samplePixel, 0));
        float sampleDepth = sampleNormalDepth.z;
        if (sampleDepth <= 0.0f || sampleDepth >= 1.0f)
            continue;

        float3 sampleNormal = DecodeOctNormal(sampleNormalDepth.xy);
        float depthWeight = 1.0f - saturate(abs(centerDepth - sampleDepth) * max(gHistoryDepthSensitivity * 0.75f, 1.0f));
        float normalWeight = smoothstep(saturate(gHistoryNormalThreshold - 0.02f), 0.999f, saturate(dot(centerNormal, sampleNormal)));
        float weight = depthWeight * normalWeight * saturate(gSpatialFilterStrength);
        if (weight <= 0.0f)
            continue;

        float4 sampleGi = gTrace.Load(int3(samplePixel, 0));
        if (sampleGi.a <= 0.0f)
            continue;

        sum += sampleGi.rgb * weight;
        totalWeight += weight;
    }

    return totalWeight > 0.0f ? float4(sum / totalWeight, 1.0f) : 0.0f.xxxx;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= gFrameWidth || dispatchThreadId.y >= gFrameHeight)
        return;

    const uint2 pixel = dispatchThreadId.xy;
    const float4 normalDepth = gNormalDepth.Load(int3(pixel, 0));
    const float depth = normalDepth.z;
    const float edgeFade = depth < 1.0f ? 1.0f : 0.0f;
    if (edgeFade == 0.0f || depth <= 0.0f)
    {
        gOutput[pixel] = 0.0f.xxxx;
        return;
    }

    const float3 normal = DecodeOctNormal(normalDepth.xy);
    const float4 filteredTrace = FilterNeighborhood(pixel, depth, normal, gTrace.Load(int3(pixel, 0)));
    const float3 filteredGi = filteredTrace.rgb;

    float3 resolvedGi = filteredGi;
    float resolvedValid = filteredTrace.a;
    if (gFrameIndex > 0)
    {
        const float3 worldPos = ReconstructWorldPosition(pixel, depth);
        const float4 prevClip = mul(float4(worldPos, 1.0f), gPrevViewProj);
        if (abs(prevClip.w) > 1e-5f)
        {
            const float2 prevUv = float2(prevClip.x / prevClip.w, prevClip.y / prevClip.w) * float2(0.5f, -0.5f) + 0.5f.xx;
            const float2 prevTexel = prevUv * float2(gFrameWidth, gFrameHeight);
            if (all(prevTexel >= 1.0f.xx) && all(prevTexel <= float2(gFrameWidth - 2, gFrameHeight - 2)))
            {
                const int2 prevPixel = int2(prevTexel);
                const float historyDepth = gSceneDepth.Load(int3(prevPixel, 0));
                const float4 historyNormalDepth = gNormalDepth.Load(int3(prevPixel, 0));
                const float3 historyNormal = DecodeOctNormal(historyNormalDepth.xy);
                const float historyWeight = ComputeHistoryWeight(depth, historyDepth, normal, historyNormal);
                const float blendWeight = saturate(gHysteresis * historyWeight);
                const float4 historySample = gHistory.Load(int3(prevPixel, 0));
                const float3 historyGi = historySample.rgb;
                if (filteredTrace.a <= 0.0f && historySample.a > 0.0f)
                {
                    resolvedGi = historyGi;
                    resolvedValid = 1.0f;
                }
                else if (filteredTrace.a > 0.0f)
                {
                    const float clampScale = max(gHistoryClampScale, 0.0f);
                    const float3 clampedHistoryGi = clamp(historyGi, filteredGi * (1.0f - clampScale), filteredGi * (1.0f + clampScale) + 0.02f.xxx);
                    resolvedGi = lerp(filteredGi, clampedHistoryGi, blendWeight);
                    resolvedValid = 1.0f;
                }
            }
        }
    }

    gOutput[pixel] = float4(resolvedGi * edgeFade, resolvedValid > 0.0f ? 1.0f : 0.0f);
}
