// Ocean_InitialSpectrum.hlsl
// --------------------------
// Builds the time-independent wave spectrum h0(k) for one FFT cascade, using a
// JONSWAP / Phillips-style directional spectrum (Tessendorf, "Simulating Ocean
// Water").  This runs only when the ocean parameters change, not every frame.
//
// h0(k) = (1/sqrt(2)) * (xi_r + i*xi_i) * sqrt(2 * S(k) * dk)
//
// where xi are Gaussian random numbers and S(k) is the directional spectrum.
// The conjugate term h0(-k)* is stored alongside so the time evolution pass can
// build a Hermitian (real-valued) field without a second texture fetch.

cbuffer SpectrumConstants : register(b0)
{
    uint  gSize;            // FFT resolution (texels per axis)
    float gPatchSize;       // world-space size of this cascade, metres
    float gWindSpeed;       // m/s
    float gWindAngle;       // radians

    float gFetch;           // km, JONSWAP fetch length
    float gDepth;           // m, water depth for the dispersion relation
    float gSwell;           // 0..1, how much energy sits in the long swell
    float gSpreadBlend;     // 0..1, directional spreading tightness

    // Only wavenumbers inside [gCutoffLow, gCutoffHigh) belong to this cascade,
    // which is what keeps the cascades from overlapping and double-counting.
    float gCutoffLow;
    float gCutoffHigh;
    float gAmplitude;       // overall scale
    uint  gSeed;
};

RWTexture2D<float4> gSpectrumOut : register(u0);   // xy = h0(k), zw = h0(-k)*

static const float kPi      = 3.14159265f;
static const float kTwoPi   = 6.28318531f;
static const float kGravity = 9.81f;

// --- Deterministic hash-based Gaussian pair (Box-Muller) -------------------
uint WangHash(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return seed;
}

float UintToUnitFloat(uint value)
{
    // Keep it strictly inside (0,1]: log(0) in Box-Muller would be -inf.
    return max(float(value & 0x00FFFFFFu) / 16777216.0f, 1e-7f);
}

float2 GaussianPair(uint seed)
{
    const uint h0 = WangHash(seed);
    const uint h1 = WangHash(h0);
    const float u1 = UintToUnitFloat(h0);
    const float u2 = UintToUnitFloat(h1);
    const float mag = sqrt(-2.0f * log(u1));
    return float2(mag * cos(kTwoPi * u2), mag * sin(kTwoPi * u2));
}

// --- Dispersion ------------------------------------------------------------
// Finite-depth relation; converges to the deep-water sqrt(g*k) as depth grows.
float Omega(float k)
{
    return sqrt(kGravity * k * tanh(min(k * max(gDepth, 0.1f), 20.0f)));
}

// --- JONSWAP scalar spectrum ----------------------------------------------
float JonswapSpectrum(float omega)
{
    const float fetchMetres = max(gFetch, 0.01f) * 1000.0f;
    const float windSpeed = max(gWindSpeed, 0.1f);

    // Peak frequency for a fetch-limited sea.
    const float dimensionlessFetch = kGravity * fetchMetres / (windSpeed * windSpeed);
    const float omegaPeak = 22.0f * pow(max(dimensionlessFetch, 1e-3f), -0.33f)
                          * kGravity / windSpeed;

    const float alpha = 0.076f * pow(max(dimensionlessFetch, 1e-3f), -0.22f);
    const float sigma = (omega <= omegaPeak) ? 0.07f : 0.09f;
    const float r = exp(-(omega - omegaPeak) * (omega - omegaPeak)
                      / (2.0f * sigma * sigma * omegaPeak * omegaPeak));
    const float gamma = 3.3f;

    const float o = max(omega, 1e-4f);
    return alpha * kGravity * kGravity / pow(o, 5.0f)
         * exp(-1.25f * pow(omegaPeak / o, 4.0f))
         * pow(gamma, r);
}

// Directional spreading: energy concentrates along the wind axis, and short
// waves spread wider than the swell.  cos^2s form.
float DirectionalSpread(float theta, float omega)
{
    const float s = lerp(1.0f, 12.0f, saturate(gSpreadBlend));
    const float c = cos(theta * 0.5f);
    float spread = pow(max(c * c, 1e-6f), s);

    // A fraction of the energy is long-crested swell, held tightly to the axis.
    const float swellSpread = pow(max(cos(theta * 0.5f), 0.0f), 32.0f);
    spread = lerp(spread, swellSpread, saturate(gSwell));

    return spread;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gSize || id.y >= gSize)
        return;

    const float halfSize = float(gSize) * 0.5f;
    // Wavevector for this texel, centred so index [size/2] maps to k = 0.
    const float2 nm = float2(float(id.x) - halfSize, float(id.y) - halfSize);
    const float2 k = nm * (kTwoPi / max(gPatchSize, 0.01f));
    const float kLen = length(k);

    float4 result = float4(0.0f, 0.0f, 0.0f, 0.0f);

    // k = 0 carries no wave, and each cascade owns only its own band.
    if (kLen > 1e-5f && kLen >= gCutoffLow && kLen < gCutoffHigh)
    {
        const float omega = Omega(kLen);
        // dOmega/dk, needed to convert the frequency spectrum to a wavenumber one.
        const float dOmegaDk = kGravity * 0.5f / max(omega, 1e-4f);

        const float theta = atan2(k.y, k.x) - gWindAngle;
        const float spectrum = JonswapSpectrum(omega)
                             * DirectionalSpread(theta, omega)
                             * dOmegaDk / max(kLen, 1e-5f);

        // dk area element for this texel.
        const float dk = kTwoPi / max(gPatchSize, 0.01f);
        const float amplitude = gAmplitude * sqrt(max(2.0f * spectrum * dk * dk, 0.0f));

        const uint seed = id.y * gSize + id.x + gSeed * 0x9E3779B9u;
        const float2 xi = GaussianPair(seed);
        const float2 h0 = xi * amplitude * 0.70710678f;   // 1/sqrt(2)

        // Conjugate partner at -k, needed for a real-valued inverse transform.
        const uint2 mirror = (gSize - id.xy) % gSize;
        const uint mirrorSeed = mirror.y * gSize + mirror.x + gSeed * 0x9E3779B9u;
        const float2 xiMirror = GaussianPair(mirrorSeed);
        const float2 h0Conj = float2(xiMirror.x, -xiMirror.y) * amplitude * 0.70710678f;

        result = float4(h0, h0Conj);
    }

    gSpectrumOut[id.xy] = result;
}
