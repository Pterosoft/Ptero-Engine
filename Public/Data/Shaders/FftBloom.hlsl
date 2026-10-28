// FftBloom.hlsl
//
// Convolution bloom in the frequency domain. The thresholded frame is copied, downscaled,
// into one corner of an N x N grid (the rest is zero padding, so the circular convolution
// cannot wrap one edge of the frame onto the other), transformed, multiplied by the
// transform of the kernel, and transformed back:
//
//   CSPrefilter           frame -> grid, soft threshold (same curve as the mip-chain bloom)
//   CSFftRowsForward      FFT of every row that holds image
//   CSFftColumnsConvolve  FFT of every column, x kernel spectrum, inverse FFT of the column
//   CSFftRowsInverse      inverse FFT of the rows that hold image
//   CSComposite           frame + intensity x (bilinear) convolved grid -> output
//   CSFftColumnsForward   used once per kernel change, on the kernel itself
//
// Each FFT is one thread group per line, radix-2 decimation in time in groupshared memory
// (N <= 1024). The grid holds three complex channels: R and G in a float4 (re, im, re, im)
// and B in a float2. The kernel's spectrum is pre-divided by N^2, which normalises the
// inverse transform. See FftBloomRenderer.h.

cbuffer FftBloomConstants : register(b0)
{
    uint   gN;
    uint   gLog2N;
    uint2  gRegion;        // size of the frame's copy inside the grid

    uint2  gSourceSize;
    uint2  gOutputSize;

    float  gThreshold;
    float  gKnee;
    float  gIntensity;
    float  _FftPad0;
};

Texture2D<float4>          gSource       : register(t0);
StructuredBuffer<float4>   gKernelRG     : register(t3);
StructuredBuffer<float2>   gKernelB      : register(t4);

RWStructuredBuffer<float4> gDataRG       : register(u0);
RWStructuredBuffer<float2> gDataB        : register(u1);
RWTexture2D<float4>        gOutput       : register(u2);

SamplerState               gLinearClamp  : register(s0);

#define FFT_MAX_N    1024
#define FFT_THREADS  256

static const float FFT_PI = 3.14159265f;

// ---------------------------------------------------------------------------------
// Prefilter
// ---------------------------------------------------------------------------------

float3 QuadraticThreshold(float3 color, float threshold, float knee)
{
    const float brightness = max(color.r, max(color.g, color.b));
    float rq = clamp(brightness - threshold + knee, 0.0f, 2.0f * knee);
    rq = (rq * rq) / (4.0f * knee + 0.00001f);
    const float weight = max(rq, brightness - threshold) / max(brightness, 0.00001f);
    return color * weight;
}

[numthreads(8, 8, 1)]
void CSPrefilter(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gN || id.y >= gN)
        return;

    float3 value = 0.0f.xxx;
    if (id.x < gRegion.x && id.y < gRegion.y)
    {
        // One grid texel covers several source pixels; a 4x4 set of bilinear taps over
        // that footprint keeps small highlights from flickering in and out.
        const float2 texel = 1.0f / float2(gRegion);
        const float2 corner = float2(id.xy) * texel;
        [unroll]
        for (int y = 0; y < 4; ++y)
        {
            [unroll]
            for (int x = 0; x < 4; ++x)
            {
                const float2 uv = corner + (float2(x, y) + 0.5f) * 0.25f * texel;
                value += gSource.SampleLevel(gLinearClamp, uv, 0.0f).rgb;
            }
        }
        value = QuadraticThreshold(value * (1.0f / 16.0f), gThreshold, gKnee);
        // Guard the transform against a NaN or inf pixel, which would smear across the frame.
        value = all(isfinite(value)) ? min(value, 65000.0f.xxx) : 0.0f.xxx;
    }

    const uint index = id.y * gN + id.x;
    gDataRG[index] = float4(value.r, 0.0f, value.g, 0.0f);
    gDataB[index] = float2(value.b, 0.0f);
}

// ---------------------------------------------------------------------------------
// FFT
// ---------------------------------------------------------------------------------

groupshared float2 sR[FFT_MAX_N];
groupshared float2 sG[FFT_MAX_N];
groupshared float2 sB[FFT_MAX_N];

float2 ComplexMul(float2 a, float2 b)
{
    return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

uint BitReverse(uint i)
{
    return reversebits(i) >> (32u - gLog2N);
}

uint ElementIndex(uint lineIndex, uint i, bool columns)
{
    return columns ? i * gN + lineIndex : lineIndex * gN + i;
}

// Loads a line into groupshared memory in bit-reversed order, ready for the butterflies.
void LoadLine(uint lineIndex, uint thread, bool columns)
{
    for (uint i = thread; i < gN; i += FFT_THREADS)
    {
        const uint index = ElementIndex(lineIndex, i, columns);
        const float4 rg = gDataRG[index];
        const uint target = BitReverse(i);
        sR[target] = rg.xy;
        sG[target] = rg.zw;
        sB[target] = gDataB[index];
    }
    GroupMemoryBarrierWithGroupSync();
}

void StoreLine(uint lineIndex, uint thread, bool columns)
{
    for (uint i = thread; i < gN; i += FFT_THREADS)
    {
        const uint index = ElementIndex(lineIndex, i, columns);
        gDataRG[index] = float4(sR[i], sG[i]);
        gDataB[index] = sB[i];
    }
}

// In-place iterative Cooley-Tukey on bit-reversed input; output in natural order.
void Butterflies(uint thread, bool inverse)
{
    const float direction = inverse ? 1.0f : -1.0f;
    for (uint length = 2; length <= gN; length <<= 1)
    {
        const uint halfLength = length >> 1;
        for (uint b = thread; b < (gN >> 1); b += FFT_THREADS)
        {
            const uint k = b % halfLength;
            const uint i = (b / halfLength) * length + k;
            const uint j = i + halfLength;

            float s, c;
            sincos(direction * 2.0f * FFT_PI * float(k) / float(length), s, c);
            const float2 w = float2(c, s);

            const float2 tR = ComplexMul(w, sR[j]);
            const float2 tG = ComplexMul(w, sG[j]);
            const float2 tB = ComplexMul(w, sB[j]);
            sR[j] = sR[i] - tR; sR[i] += tR;
            sG[j] = sG[i] - tG; sG[i] += tG;
            sB[j] = sB[i] - tB; sB[i] += tB;
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

[numthreads(FFT_THREADS, 1, 1)]
void CSFftRowsForward(uint3 group : SV_GroupID, uint thread : SV_GroupIndex)
{
    LoadLine(group.x, thread, false);
    Butterflies(thread, false);
    StoreLine(group.x, thread, false);
}

[numthreads(FFT_THREADS, 1, 1)]
void CSFftRowsInverse(uint3 group : SV_GroupID, uint thread : SV_GroupIndex)
{
    LoadLine(group.x, thread, false);
    Butterflies(thread, true);
    StoreLine(group.x, thread, false);
}

[numthreads(FFT_THREADS, 1, 1)]
void CSFftColumnsForward(uint3 group : SV_GroupID, uint thread : SV_GroupIndex)
{
    LoadLine(group.x, thread, true);
    Butterflies(thread, false);
    StoreLine(group.x, thread, true);
}

[numthreads(FFT_THREADS, 1, 1)]
void CSFftColumnsConvolve(uint3 group : SV_GroupID, uint thread : SV_GroupIndex)
{
    const uint lineIndex = group.x;
    LoadLine(lineIndex, thread, true);
    Butterflies(thread, false);

    // Pointwise product with the kernel's spectrum, written straight back into
    // bit-reversed order so the inverse transform can start without another pass.
    float2 r[FFT_MAX_N / FFT_THREADS];
    float2 g[FFT_MAX_N / FFT_THREADS];
    float2 b[FFT_MAX_N / FFT_THREADS];
    uint slot = 0;
    for (uint i = thread; i < gN; i += FFT_THREADS, ++slot)
    {
        const uint index = ElementIndex(lineIndex, i, true);
        const float4 kernelRG = gKernelRG[index];
        r[slot] = ComplexMul(sR[i], kernelRG.xy);
        g[slot] = ComplexMul(sG[i], kernelRG.zw);
        b[slot] = ComplexMul(sB[i], gKernelB[index]);
    }
    GroupMemoryBarrierWithGroupSync();

    slot = 0;
    for (uint i2 = thread; i2 < gN; i2 += FFT_THREADS, ++slot)
    {
        const uint target = BitReverse(i2);
        sR[target] = r[slot];
        sG[target] = g[slot];
        sB[target] = b[slot];
    }
    GroupMemoryBarrierWithGroupSync();

    Butterflies(thread, true);
    StoreLine(lineIndex, thread, true);
}

// ---------------------------------------------------------------------------------
// Composite
// ---------------------------------------------------------------------------------

float3 LoadGrid(int2 p)
{
    p = clamp(p, int2(0, 0), int2(gRegion) - 1);
    const uint index = uint(p.y) * gN + uint(p.x);
    return float3(gDataRG[index].x, gDataRG[index].z, gDataB[index].x);
}

[numthreads(8, 8, 1)]
void CSComposite(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gOutputSize.x || id.y >= gOutputSize.y)
        return;

    const float2 uv = (float2(id.xy) + 0.5f) / float2(gOutputSize);
    const float2 p = uv * float2(gRegion) - 0.5f;
    const int2 p0 = int2(floor(p));
    const float2 t = p - float2(p0);
    const float3 bloom = lerp(
        lerp(LoadGrid(p0), LoadGrid(p0 + int2(1, 0)), t.x),
        lerp(LoadGrid(p0 + int2(0, 1)), LoadGrid(p0 + int2(1, 1)), t.x),
        t.y);

    const float4 original = gSource.Load(int3(id.xy, 0));
    gOutput[id.xy] = float4(original.rgb + max(bloom, 0.0f.xxx) * gIntensity, original.a);
}
