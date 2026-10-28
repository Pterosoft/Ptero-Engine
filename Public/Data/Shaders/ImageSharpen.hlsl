// Standalone image sharpening pass. This deliberately runs outside TAA so the
// same sharpening control works with TAA, SMAA, DLSS, or raw scene color.
//
// Unsharp mask: the image plus Strength times its difference from a blurred
// copy. The blur is a Gaussian of the given Radius, sampled on two rings of
// eight taps (at half the radius and at the full radius) around the centre,
// so a wide radius costs the same 17 loads as a narrow one.

cbuffer ImageSharpenConstants : register(b0)
{
    float Strength;
    uint  FrameWidth;
    uint  FrameHeight;
    float Radius;    // pixels, >= 1
}

Texture2D<float4>   gInput  : register(t0);
RWTexture2D<float4> gOutput : register(u0);

float3 SanitizeColor(float3 color)
{
    if (any(isnan(color)) || any(isinf(color)))
        return float3(0.0f, 0.0f, 0.0f);
    return max(color, 0.0f.xxx);
}

static const int2 kRingDirections[8] =
{
    int2(-1, -1), int2(0, -1), int2(1, -1),
    int2(-1,  0),              int2(1,  0),
    int2(-1,  1), int2(0,  1), int2(1,  1),
};

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint2 coord = dispatchId.xy;
    if (coord.x >= FrameWidth || coord.y >= FrameHeight)
        return;

    const int2 maxCoord = int2((int)FrameWidth - 1, (int)FrameHeight - 1);
    const float4 center = gInput[coord];

    // Ring distances in whole pixels. At radius 1 both rings sit one pixel out
    // and the weights come out as the original 3x3 box.
    const float radius = max(Radius, 1.0f);
    const int inner = max(1, (int)round(radius * 0.5f));
    const int outer = max(inner, (int)round(radius));
    const float sigma = radius * 0.5f;
    const float invTwoSigmaSq = (radius <= 1.0f) ? 0.0f : 1.0f / (2.0f * sigma * sigma);

    float3 blurAccum = center.rgb;
    float  weightSum = 1.0f;
    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        const int2 direction = kRingDirections[i];
        const float lengthSq = (float)dot(direction, direction);

        const float innerWeight = exp(-(float)(inner * inner) * lengthSq * invTwoSigmaSq);
        const int2 innerCoord = clamp((int2)coord + direction * inner, int2(0, 0), maxCoord);
        blurAccum += gInput[innerCoord].rgb * innerWeight;
        weightSum += innerWeight;

        if (outer > inner)
        {
            const float outerWeight = exp(-(float)(outer * outer) * lengthSq * invTwoSigmaSq);
            const int2 outerCoord = clamp((int2)coord + direction * outer, int2(0, 0), maxCoord);
            blurAccum += gInput[outerCoord].rgb * outerWeight;
            weightSum += outerWeight;
        }
    }

    const float3 blur = blurAccum / weightSum;
    const float3 sharpened = center.rgb + Strength * (center.rgb - blur);
    gOutput[coord] = float4(SanitizeColor(sharpened), center.a);
}
