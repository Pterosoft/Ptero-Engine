// ChromaticAberration.hlsl
//
// Lateral chromatic aberration: a real lens focuses each wavelength at a slightly
// different magnification, so the three channels are sampled at slightly different
// distances from the optical centre. The split grows toward the edges of the frame and is
// zero dead centre, which is why a uniform per-channel offset never looks like a lens.
//
// Runs at the very end of the post chain, on the tonemapped display-referred image. Doing
// it in HDR before the tonemapper would let a bright highlight's fringe survive tone
// mapping as a saturated band rather than the subtle edge colouring a lens produces.

cbuffer ChromaticAberrationConstants : register(b0)
{
    uint  gFrameWidth;
    uint  gFrameHeight;
    float gStrength;        // displacement at the corner, in pixels
    float gFalloff;         // exponent on the normalised centre distance

    int   gSampleCount;     // 1 = plain 3-tap split, >1 = spectral smear
    float gCenterX;         // optical centre in UV space
    float gCenterY;
    float _CaPad0;
};

Texture2D           gInput  : register(t0);
RWTexture2D<float4> gOutput : register(u0);
SamplerState        gLinearClamp : register(s0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gFrameWidth || id.y >= gFrameHeight)
        return;

    const int2   pixel     = int2(id.xy);
    const float2 frameSize = float2(gFrameWidth, gFrameHeight);
    const float2 uv        = (float2(pixel) + 0.5f.xx) / frameSize;

    const float4 original = gInput.Load(int3(pixel, 0));

    if (gStrength <= 0.0f)
    {
        gOutput[pixel] = original;
        return;
    }

    // Direction and normalised distance from the optical centre. Normalising by the
    // centre-to-corner length keeps the corner displacement equal to gStrength whatever
    // the aspect ratio, so the effect does not change strength when the window resizes.
    const float2 center    = float2(gCenterX, gCenterY);
    const float2 toPixel   = uv - center;
    const float  maxRadius = max(length(float2(1.0f, 1.0f) - center), length(center));
    const float  radius    = saturate(length(toPixel) / max(maxRadius, 1.0e-4f));

    // pow() rather than a linear ramp: real lenses hold a clean centre and fall apart
    // toward the rim, so the fringe should stay out of the middle of the frame.
    const float  magnitude = pow(radius, max(gFalloff, 0.0f)) * gStrength;
    const float2 direction = (radius > 1.0e-5f) ? normalize(toPixel) : 0.0f.xx;
    const float2 offset    = direction * magnitude / frameSize;

    float3 color;

    if (gSampleCount <= 1)
    {
        // Cheap path: red pushed out, blue pulled in, green left where it is.
        color.r = gInput.SampleLevel(gLinearClamp, uv + offset, 0.0f).r;
        color.g = original.g;
        color.b = gInput.SampleLevel(gLinearClamp, uv - offset, 0.0f).b;
    }
    else
    {
        // Spectral path: walk the offset in steps and weight each tap by how much that
        // wavelength contributes to each channel. A 3-tap split bands visibly once the
        // strength is high; smearing along the same line keeps the fringe continuous.
        const int   sampleCount = min(gSampleCount, 16);
        float3 accumulated = 0.0f.xxx;
        float3 weightSum   = 0.0f.xxx;

        [loop]
        for (int i = 0; i < sampleCount; ++i)
        {
            // t runs -1..+1 across the taps: -1 is the blue end, +1 the red end.
            const float t = (sampleCount > 1)
                ? (float(i) / float(sampleCount - 1)) * 2.0f - 1.0f
                : 0.0f;

            const float3 tap = gInput.SampleLevel(gLinearClamp, uv + offset * t, 0.0f).rgb;

            // Triangular response per channel, peaking at the end of the sweep that
            // channel belongs to, so the taps sum back to roughly the original image.
            const float3 weight = float3(
                saturate(t),                 // red peaks at the outward end
                1.0f - abs(t),               // green peaks in the middle
                saturate(-t));               // blue peaks at the inward end

            accumulated += tap * weight;
            weightSum   += weight;
        }

        color = accumulated / max(weightSum, 1.0e-4f.xxx);
    }

    gOutput[pixel] = float4(color, original.a);
}
