// ResolveMsaaDepth.hlsl
// Resolves a multisampled hardware depth buffer into a single-sample R32_FLOAT
// texture for passes that use Texture2D<float> scene depth.

cbuffer ResolveDepthConstants : register(b0)
{
    uint gSampleCount;
    uint3 _Pad0;
};

Texture2DMS<float> gSceneDepthMsaa : register(t0);
RWTexture2D<float> gResolvedDepth : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    gResolvedDepth.GetDimensions(width, height);

    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
        return;

    const int2 pixel = int2(dispatchThreadId.xy);
    const uint sampleCount = clamp(gSampleCount, 1u, 8u);

    float resolvedDepth = 1.0f;
    [unroll]
    for (uint sampleIndex = 0; sampleIndex < 8u; ++sampleIndex)
    {
        if (sampleIndex < sampleCount)
            resolvedDepth = min(resolvedDepth, gSceneDepthMsaa.Load(pixel, sampleIndex));
    }

    gResolvedDepth[dispatchThreadId.xy] = saturate(resolvedDepth);
}
