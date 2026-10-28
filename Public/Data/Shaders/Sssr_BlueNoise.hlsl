// Sssr_BlueNoise.hlsl
// FidelityFX SSSR: refreshes the 128x128 blue-noise texture the intersection pass draws
// its GGX samples from, rotated by the golden ratio every frame so the denoiser sees a new
// sample set each frame. Eric Heitz's blue-noise sampler; ported from the SSSR 1.3 sample's
// PrepareBlueNoiseTexture.hlsl.
//
// Part of the SSSR pass set, which is compiled for SM6 because its other passes use Wave
// intrinsics.

#include "Sssr_Common.hlsli"

StructuredBuffer<uint> gSobolBuffer       : register(t0);
StructuredBuffer<uint> gRankingTileBuffer : register(t1);
StructuredBuffer<uint> gScramblingTileBuffer : register(t2);

RWTexture2D<float2> gBlueNoiseTexture : register(u0);

#define GOLDEN_RATIO 1.61803398875f

// Returns a value in [0, 1].
float SampleRandomNumber(uint pixelI, uint pixelJ, uint sampleIndex, uint sampleDimension)
{
    pixelI = pixelI & 127u;
    pixelJ = pixelJ & 127u;
    sampleIndex = sampleIndex & 255u;
    sampleDimension = sampleDimension & 255u;

    // xor index based on optimized ranking
    const uint rankedSampleIndex = sampleIndex ^ gRankingTileBuffer[sampleDimension + (pixelI + pixelJ * 128u) * 8u];

    // Fetch value in sequence
    uint value = gSobolBuffer[sampleDimension + rankedSampleIndex * 256u];

    // If the dimension is optimized, xor sequence value based on optimized scrambling
    value = value ^ gScramblingTileBuffer[(sampleDimension % 8u) + (pixelI + pixelJ * 128u) * 8u];

    return (value + 0.5f) / 256.0f;
}

float2 SampleRandomVector2D(uint2 pixel)
{
    return float2(
        fmod(SampleRandomNumber(pixel.x, pixel.y, 0, 0u) + (gFrameIndex & 0xFFu) * GOLDEN_RATIO, 1.0f),
        fmod(SampleRandomNumber(pixel.x, pixel.y, 0, 1u) + (gFrameIndex & 0xFFu) * GOLDEN_RATIO, 1.0f));
}

[numthreads(8, 8, 1)]
void CSMain(uint2 dispatchThreadId : SV_DispatchThreadID)
{
    gBlueNoiseTexture[dispatchThreadId] = SampleRandomVector2D(dispatchThreadId);
}
