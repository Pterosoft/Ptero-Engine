// SkyGlare.hlsl
//
// The glare around the sun and the moon (SkyGlareRenderer), added on top of the
// bloomed image. Source 0 is the sun, 1 the moon.
//   CSVisibility - one group per source: how much of its disc is in view, from the
//                  depth buffer for geometry and the disc's brightness for cloud and
//                  fog. Smoothed over time in a small buffer.
//   CSApply      - input + each source's point spread function around its pixel,
//                  scaled by that visibility.
//
// The PSFs are in image-plane pixels and normalised to unit energy on the CPU, so a
// source's Energy (its light over one pixel's solid angle, times its Intensity) is
// the total its glare adds, whatever the resolution or field of view.

#define SKY_GLARE_SOURCES 2

struct GlareSource
{
    float2 Pixel;               // the body's position, output pixels (may be off screen)
    float  OffscreenFade;       // 0 = no glare from this source this frame
    float  Reset;               // 1 = take this frame's visibility as is
    float3 Energy;              // per channel, see above
    float  SampleRadius;        // pixels, the visibility taps' spiral
    float  HaloCore;            // pixels
    float  HaloRadius;          // pixels
    float  HaloFalloff;
    float  HaloWeight;          // halo share / its normalisation
    float  SpikeLength;         // pixels
    float  SpikeCore;           // pixels
    float  SpikeWeight;         // spike share / their normalisation
    float  SpikeCount;
    float3 ChannelScale;        // how far each channel reaches, relative to green
    float  ExpectedDisc;        // disc luminance below which it reads as covered; 0 = off
    float  AverageDiscTest;     // 1 = test the disc's mean brightness, not each tap
    float3 _Pad;
    float4 SpikeDirections[8];  // two unit directions per entry (xy, zw)
};

cbuffer SkyGlareConstants : register(b0)
{
    float2 OutputSize;
    float  VisibilityBlend;     // temporal blend toward this frame's visibility
    float  SpikeWidth;          // pixels, across a spike at its root
    GlareSource Sources[SKY_GLARE_SOURCES];
}

Texture2D<float4>         gInput      : register(t0);  // bloomed image
Texture2D<float4>         gSceneColor : register(t1);  // image before bloom, for the disc test
Texture2D<float>          gDepth      : register(t2);
RWTexture2D<float4>       gOutput     : register(u0);
RWStructuredBuffer<float> gVisibility : register(u1);

#define SKY_GLARE_TAPS 64

// Per tap: (in the sky, passes the per-tap disc test, on screen, luminance if in the sky).
groupshared float4 sTaps[SKY_GLARE_TAPS];

float Luminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

[numthreads(SKY_GLARE_TAPS, 1, 1)]
void CSVisibility(uint3 group : SV_GroupID, uint tap : SV_GroupIndex)
{
    const GlareSource source = Sources[group.x];

    // Golden-angle spiral over the inner part of the disc.
    const float radius = sqrt((tap + 0.5f) / SKY_GLARE_TAPS) * source.SampleRadius;
    const float angle = tap * 2.39996323f;
    const float2 pixel = source.Pixel + float2(cos(angle), sin(angle)) * radius;
    const float2 uv = pixel / OutputSize;

    float4 result = 0.0f;
    if (all(uv >= 0.0f) && all(uv < 1.0f))
    {
        uint depthWidth, depthHeight;
        gDepth.GetDimensions(depthWidth, depthHeight);
        // Only the sky (cleared depth) lets the body through.
        const float sky = gDepth.Load(int3(uv * float2(depthWidth, depthHeight), 0)) >= 0.99999f ? 1.0f : 0.0f;

        float luminance = 0.0f;
        float passes = sky;
        if (sky > 0.0f && source.ExpectedDisc > 0.0f)
        {
            uint colorWidth, colorHeight;
            gSceneColor.GetDimensions(colorWidth, colorHeight);
            luminance = Luminance(gSceneColor.Load(int3(uv * float2(colorWidth, colorHeight), 0)).rgb);
            passes = saturate(luminance / source.ExpectedDisc);
        }
        result = float4(sky, passes, 1.0f, luminance);
    }

    sTaps[tap] = result;
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint stride = SKY_GLARE_TAPS / 2; stride > 0; stride >>= 1)
    {
        if (tap < stride)
            sTaps[tap] += sTaps[tap + stride];
        GroupMemoryBarrierWithGroupSync();
    }

    if (tap == 0)
    {
        const float4 sum = sTaps[0];
        // Off screen there is nothing to test against; the CPU fades the glare out there.
        float current = 1.0f;
        if (sum.z > 0.0f)
        {
            if (source.AverageDiscTest > 0.0f && source.ExpectedDisc > 0.0f)
            {
                // A crescent moon is mostly dark, so it is judged on its mean brightness
                // (the CPU expects the lit share of it), not tap by tap.
                const float meanLuminance = sum.x > 0.0f ? sum.w / sum.x : 0.0f;
                current = (sum.x / sum.z) * saturate(meanLuminance / source.ExpectedDisc);
            }
            else
            {
                current = sum.y / sum.z;
            }
        }
        const float previous = gVisibility[group.x];
        gVisibility[group.x] = source.Reset > 0.0f ? current : lerp(previous, current, VisibilityBlend);
    }
}

// Power-law halo, flat over the core and windowed to nothing at HaloRadius.
float Halo(GlareSource source, float r)
{
    const float x = r / source.HaloRadius;
    if (x >= 1.0f)
        return 0.0f;
    const float window = (1.0f - x * x) * (1.0f - x * x);
    const float q = r / source.HaloCore;
    return pow(1.0f + q * q, -0.5f * source.HaloFalloff) * window;
}

// One spike's profile along its length (both directions share it).
float SpikeAlong(GlareSource source, float along)
{
    const float t = along / source.SpikeLength;
    if (t >= 1.0f)
        return 0.0f;
    return pow(1.0f + along / source.SpikeCore, -1.5f) * (1.0f - t) * (1.0f - t);
}

float Spikes(GlareSource source, float2 offset, float scale)
{
    float sum = 0.0f;
    const uint count = (uint)source.SpikeCount;
    for (uint i = 0; i < count; ++i)
    {
        const float4 pair = source.SpikeDirections[i / 2];
        const float2 direction = (i & 1) ? pair.zw : pair.xy;
        const float along = abs(dot(offset, direction));
        const float across = abs(offset.x * direction.y - offset.y * direction.x);
        // A spike widens slowly along its length, as diffraction spikes do.
        const float width = SpikeWidth + along * 0.003f;
        const float profile = exp(-0.5f * (across / width) * (across / width)) / (2.50662827f * width);
        sum += profile * SpikeAlong(source, along / scale) / scale;
    }
    return sum;
}

float3 Glare(GlareSource source, float visibility, float2 pixelCenter)
{
    const float2 offset = pixelCenter - source.Pixel;
    const float r = length(offset);

    float3 glare;
    [unroll]
    for (int c = 0; c < 3; ++c)
    {
        // Stretching a channel's pattern by its scale keeps its energy.
        const float scale = source.ChannelScale[c];
        float value = source.HaloWeight * Halo(source, r / scale) / (scale * scale);
        if (source.SpikeWeight > 0.0f)
            value += source.SpikeWeight * Spikes(source, offset, scale);
        glare[c] = value;
    }
    return glare * source.Energy * visibility;
}

[numthreads(8, 8, 1)]
void CSApply(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= (uint2)OutputSize))
        return;

    float3 color = gInput.Load(int3(id.xy, 0)).rgb;
    const float2 pixelCenter = float2(id.xy) + 0.5f;

    float3 glare = 0.0f;
    [unroll]
    for (uint s = 0; s < SKY_GLARE_SOURCES; ++s)
    {
        const GlareSource source = Sources[s];
        const float visibility = gVisibility[s] * source.OffscreenFade;
        [branch]
        if (visibility > 0.0f && any(source.Energy > 0.0f))
            glare += Glare(source, visibility, pixelCenter);
    }

    gOutput[id.xy] = float4(color + min(glare, 60000.0f), 1.0f);
}
