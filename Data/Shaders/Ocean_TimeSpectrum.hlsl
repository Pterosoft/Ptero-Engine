// Ocean_TimeSpectrum.hlsl
// -----------------------
// Evolves the static spectrum h0(k) to time t and packs the four complex fields
// the inverse FFT needs:
//
//   h(k,t) = h0(k) * exp(i*omega*t) + conj(h0(-k)) * exp(-i*omega*t)
//
//   Dz (height)      = h
//   Dx, Dy (choppy)  = -i * (k / |k|) * h        horizontal displacement
//   Slopes           =  i * k * h                analytic derivatives
//
// Two complex fields are packed per RG/BA texture so one FFT pass transforms
// two signals at once (a complex FFT of (a + i*b) recovers both real signals).

cbuffer EvolveConstants : register(b0)
{
    uint  gSize;
    float gPatchSize;
    float gTime;
    float gDepth;
};

Texture2D<float4>   gSpectrumIn      : register(t0);  // xy = h0(k), zw = h0(-k)*
RWTexture2D<float4> gDisplacementOut : register(u0);  // xy = Dx, zw = Dy
RWTexture2D<float4> gHeightSlopeOut  : register(u1);  // xy = Dz, zw = slope packing

static const float kTwoPi   = 6.28318531f;
static const float kGravity = 9.81f;

float2 ComplexMul(float2 a, float2 b)
{
    return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

float Omega(float k, float depth)
{
    return sqrt(kGravity * k * tanh(min(k * max(depth, 0.1f), 20.0f)));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gSize || id.y >= gSize)
        return;

    const float halfSize = float(gSize) * 0.5f;
    const float2 nm = float2(float(id.x) - halfSize, float(id.y) - halfSize);
    const float2 k = nm * (kTwoPi / max(gPatchSize, 0.01f));
    const float kLen = length(k);
    const float2 kDir = (kLen > 1e-6f) ? (k / kLen) : float2(0.0f, 0.0f);

    const float4 spectrum = gSpectrumIn[id.xy];
    const float2 h0    = spectrum.xy;
    const float2 h0Conj = spectrum.zw;

    const float omega = Omega(kLen, gDepth);
    const float phase = omega * gTime;
    const float2 rotor = float2(cos(phase), sin(phase));
    const float2 rotorConj = float2(rotor.x, -rotor.y);

    // h(k,t): Hermitian sum so the inverse transform is real-valued.
    const float2 h = ComplexMul(h0, rotor) + ComplexMul(h0Conj, rotorConj);

    // Horizontal (choppy) displacement: D = -i * (k/|k|) * h.
    // Multiplying a complex number by -i maps (x, y) -> (y, -x).
    const float2 iH = float2(h.y, -h.x);
    const float2 dx = iH * kDir.x;
    const float2 dy = iH * kDir.y;

    // Slopes: i*k*h, giving exact surface derivatives after the transform.
    // Multiplying by i maps (x, y) -> (-y, x).
    const float2 ih = float2(-h.y, h.x);
    const float2 slopeX = ih * k.x;
    const float2 slopeY = ih * k.y;

    // Pack two complex signals per texture: FFT(a + i*b) recovers both.
    // Displacement texture: Dx in xy, Dy in zw.
    gDisplacementOut[id.xy] = float4(dx, dy);
    // Height/slope texture: height in xy, and the two slopes combined into one
    // complex channel (slopeX + i*slopeY) in zw.
    gHeightSlopeOut[id.xy] = float4(h, slopeX.x - slopeY.y, slopeX.y + slopeY.x);
}
