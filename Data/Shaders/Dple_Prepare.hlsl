// Dple_Prepare.hlsl
// Builds the working-resolution guide buffer every later DPLE pass reads:
//   xyz = world normal, w = linear view depth in metres (negative = pass through).
//
// The occlusion passes run below output resolution, so they would otherwise each re-decode
// the G-Buffer through the view/G-Buffer bridge for their centre pixel. Doing it once here
// also gives the temporal pass a history target with matching geometry, which is what
// disocclusion rejection compares against.

#include "Dple_Common.hlsli"

RWTexture2D<float4> gGuideOut : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gWorkSize))
        return;

    DpleSurface surface;
    // Sky, glass and forward-drawn surfaces are tagged with a negative depth, so every
    // consumer rejects them with one compare instead of re-deriving the reason.
    if (!DpleLoadSurface(DpleWorkPixelToViewUV(id.xy), surface))
    {
        gGuideOut[id.xy] = float4(0.0f, 0.0f, 1.0f, -1.0f);
        return;
    }

    gGuideOut[id.xy] = float4(surface.Normal, surface.ViewDepth);
}
