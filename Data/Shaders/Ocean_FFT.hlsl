// Ocean_FFT.hlsl
// --------------
// Radix-2 inverse FFT over a square texture, one dispatch per axis.  Each
// thread group transforms a single row (horizontal pass) or column (vertical
// pass) entirely in groupshared memory, so the 1-D transform never round-trips
// to VRAM.
//
// Two independent complex signals are transformed per invocation (RG and BA),
// which resolves the displacement / height+slope packing produced by
// Ocean_TimeSpectrum.hlsl in a single pass.
//
// The transform size is a compile-time constant: `GroupMemoryBarrierWithGroupSync`
// requires every thread in the group to reach it, so threads must not early-out,
// which in turn means the group size has to match the transform size exactly.

#define OCEAN_FFT_SIZE 256
#define OCEAN_FFT_BITS 8      // log2(OCEAN_FFT_SIZE)

cbuffer FftConstants : register(b0)
{
    uint gDirection;   // 0 = transform rows, 1 = transform columns
    uint gNormalize;   // 1 on the final pass: divide by OCEAN_FFT_SIZE
    uint2 _fftPad;
};

RWTexture2D<float4> gData : register(u0);

groupshared float4 gPingPong[2][OCEAN_FFT_SIZE];

static const float kTwoPi = 6.28318531f;

float2 ComplexMul(float2 a, float2 b)
{
    return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

// Reverse the low OCEAN_FFT_BITS bits (decimation-in-time input ordering).
uint BitReverse(uint value)
{
    uint result = 0;
    [unroll]
    for (uint i = 0; i < OCEAN_FFT_BITS; ++i)
    {
        result = (result << 1) | ((value >> i) & 1u);
    }
    return result;
}

[numthreads(OCEAN_FFT_SIZE, 1, 1)]
void CSMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID)
{
    const uint index = threadId.x;
    const uint lineIndex = groupId.x;

    // --- Load this row/column into groupshared in bit-reversed order ---
    const uint reversed = BitReverse(index);
    const uint2 loadCoord = (gDirection == 0u)
        ? uint2(reversed, lineIndex)
        : uint2(lineIndex, reversed);

    uint src = 0;
    gPingPong[src][index] = gData[loadCoord];
    GroupMemoryBarrierWithGroupSync();

    // --- Butterfly stages ---
    // Gather formulation: every thread computes its own output, so the two
    // partners of a butterfly each read the same pair and apply opposite signs.
    [unroll]
    for (uint stage = 1u; stage <= OCEAN_FFT_BITS; ++stage)
    {
        const uint span     = 1u << stage;
        const uint halfSpan = span >> 1u;      // NOTE: `half` is an HLSL type
        const uint dst      = src ^ 1u;

        const uint pairIndex  = index % span;
        const uint blockStart = index - pairIndex;
        const uint offset     = pairIndex % halfSpan;

        // Inverse transform: positive exponent in the twiddle factor.
        const float angle = kTwoPi * float(offset) / float(span);
        const float2 twiddle = float2(cos(angle), sin(angle));

        const uint lowIndex  = blockStart + offset;
        const uint highIndex = lowIndex + halfSpan;

        const float4 low  = gPingPong[src][lowIndex];
        const float4 high = gPingPong[src][highIndex];

        // Both packed complex signals share the same twiddle factor.
        const float4 rotated = float4(
            ComplexMul(high.xy, twiddle),
            ComplexMul(high.zw, twiddle));

        gPingPong[dst][index] = (pairIndex < halfSpan) ? (low + rotated) : (low - rotated);

        GroupMemoryBarrierWithGroupSync();
        src = dst;
    }

    // --- Store ---
    const uint2 storeCoord = (gDirection == 0u) ? uint2(index, lineIndex) : uint2(lineIndex, index);
    float4 value = gPingPong[src][index];
    if (gNormalize != 0u)
        value /= float(OCEAN_FFT_SIZE);

    gData[storeCoord] = value;
}
