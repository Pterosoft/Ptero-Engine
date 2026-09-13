// Ptero compute bridge for the SDK SMAA shader.
// The SDK header is kept as the SMAA source of truth; this file only exposes
// engine entry points and resources.

cbuffer SMAAConstants : register(b0)
{
    float2 gPixelSize;
    float EdgeThreshold;
    float MaxSearchSteps;
    float MaxSearchStepsDiag;
    float CornerRounding;
    uint FrameWidth;
    uint FrameHeight;
    uint DebugView;
}

#define SMAA_RT_METRICS float4(gPixelSize, float(FrameWidth), float(FrameHeight))
#define SMAA_THRESHOLD EdgeThreshold
#define SMAA_MAX_SEARCH_STEPS MaxSearchSteps
#define SMAA_MAX_SEARCH_STEPS_DIAG MaxSearchStepsDiag
#define SMAA_CORNER_ROUNDING CornerRounding
#define SMAA_HLSL_4 1
#define SMAA_PTERO_BRIDGE 1
// discard is invalid in included helpers compiled for compute; edge PS returns float2.
#define discard return float2(0.0f, 0.0f)

#include "SMAA.hlsl"

#undef discard

Texture2D colorTex : register(t0);
Texture2D edgesTex : register(t1);
Texture2D blendTex : register(t2);
Texture2D areaTex : register(t3);
Texture2D searchTex : register(t4);

RWTexture2D<float4> edgesOut : register(u0);
RWTexture2D<float4> blendOut : register(u1);
RWTexture2D<float4> outputTex : register(u2);

float2 TexCoord(uint2 pixel)
{
    return (float2(pixel) + 0.5f.xx) * gPixelSize;
}

// SMAA's luma edge detection is specified for gamma-encoded LDR input - its own header
// says so - but this pass runs on the linear HDR scene colour, before AgX tonemaps it.
// Comparing raw linear lumas against a fixed threshold is why edges went undetected in
// dim scenes: gamma encoding expands exactly the dark range where interior scenes live,
// so a step that clears 0.1 after tonemapping is only a few hundredths in linear light.
// Map the sample into a display-like range first, so EdgeThreshold means the same thing
// here as it does in the reference implementation.
float EdgeLuma(float2 texcoord)
{
    const float3 linearColor = max(SMAASample(colorTex, texcoord).rgb, 0.0f.xxx);
    const float  luma = dot(linearColor, float3(0.2126f, 0.7152f, 0.0722f));
    const float  toneMapped = luma / (1.0f + luma);   // Reinhard: HDR -> [0,1)
    return sqrt(toneMapped);                          // approximate gamma 2.0 encode
}

// Faithful port of SMAALumaEdgeDetectionPS that reads through EdgeLuma and returns zero
// instead of discarding, so every thread writes its texel of the edges target.
float2 PteroLumaEdgeDetection(float2 texcoord, float4 offset[3])
{
    const float2 threshold = float2(SMAA_THRESHOLD, SMAA_THRESHOLD);

    const float L     = EdgeLuma(texcoord);
    const float Lleft = EdgeLuma(offset[0].xy);
    const float Ltop  = EdgeLuma(offset[0].zw);

    float4 delta;
    delta.xy = abs(L - float2(Lleft, Ltop));
    float2 edges = step(threshold, delta.xy);

    if (dot(edges, float2(1.0f, 1.0f)) == 0.0f)
        return float2(0.0f, 0.0f);

    const float Lright  = EdgeLuma(offset[1].xy);
    const float Lbottom = EdgeLuma(offset[1].zw);
    delta.zw = abs(L - float2(Lright, Lbottom));

    float2 maxDelta = max(delta.xy, delta.zw);
    maxDelta = max(maxDelta.xx, maxDelta.yy);

    const float Lleftleft = EdgeLuma(offset[2].xy);
    const float Ltoptop   = EdgeLuma(offset[2].zw);
    delta.zw = abs(float2(Lleft, Ltop) - float2(Lleftleft, Ltoptop));

    // Drop edges whose contrast is small next to the strongest contrast around this
    // pixel; a strong neighbour would mask them anyway.
    maxDelta = max(maxDelta.xy, delta.zw);
    edges.xy *= step(0.5f * maxDelta, delta.xy);

    return edges;
}

[numthreads(8, 8, 1)]
void CSEdgeDetection(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= FrameWidth || id.y >= FrameHeight)
        return;

    float2 texcoord = TexCoord(id.xy);
    float4 offset[3];
    SMAAEdgeDetectionVS(texcoord, offset);

    const float2 edges = PteroLumaEdgeDetection(texcoord, offset);
    edgesOut[id.xy] = float4(edges, 0.0f, 0.0f);
}

[numthreads(8, 8, 1)]
void CSBlendWeights(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= FrameWidth || id.y >= FrameHeight)
        return;

    float2 texcoord = TexCoord(id.xy);
    float2 pixcoord;
    float4 offset[3];
    SMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);

    blendOut[id.xy] = SMAABlendingWeightCalculationPS(
        texcoord,
        pixcoord,
        offset,
        edgesTex,
        areaTex,
        searchTex,
        float4(0.0f, 0.0f, 0.0f, 0.0f));
}

[numthreads(8, 8, 1)]
void CSNeighborhoodResolve(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= FrameWidth || id.y >= FrameHeight)
        return;

    float2 texcoord = TexCoord(id.xy);
    float4 offset;
    SMAANeighborhoodBlendingVS(texcoord, offset);

    if (DebugView == 1)
    {
        float2 edges = edgesTex.SampleLevel(PointSampler, texcoord, 0).rg;
        float checker = (((id.x / 16 + id.y / 16) & 1) != 0) ? 0.35f : 0.55f;
        float edgeMask = max(edges.r, edges.g);
        float3 background = float3(checker * 0.15f, checker * 0.35f, checker);
        float3 edgeColor = float3(edges.r, edges.g, edgeMask);
        outputTex[id.xy] = float4(max(background, edgeColor), 1.0f);
        return;
    }

    if (DebugView == 2)
    {
        float4 weights = blendTex.SampleLevel(PointSampler, texcoord, 0);
        float checker = (((id.x / 16 + id.y / 16) & 1) != 0) ? 0.35f : 0.55f;
        float3 background = float3(checker, checker * 0.12f, checker * 0.45f);
        float3 weightColor = abs(weights.rgb) + abs(weights.a).xxx;
        outputTex[id.xy] = float4(max(background, weightColor), 1.0f);
        return;
    }

    outputTex[id.xy] = SMAANeighborhoodBlendingPS(texcoord, offset, colorTex, blendTex);
}
