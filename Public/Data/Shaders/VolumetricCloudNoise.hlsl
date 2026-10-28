// VolumetricCloudNoise.hlsl
// Init-time bake of every procedural texture the volumetric cloud raymarcher
// samples:
//
//   CSBaseShape  128^3 RGBA8  R = Perlin-Worley, GBA = Worley FBM octaves
//   CSDetail      32^3 RGBA8  RGB = high frequency Worley FBM, A = ridged Perlin
//   CSCurl       128^2 RGBA8  RG  = curl of a 2D Perlin potential field
//   CSWeather    512^2 RGBA8  R = coverage, G = cloud type, B = precipitation
//
// Every noise primitive is periodic over its lattice so the volumes tile
// seamlessly when the raymarcher wraps world space through them.  The hashes
// are transcendental-free so the whole bake stays well inside a single frame.

cbuffer CloudNoiseConstants : register(b0)
{
    uint   gTargetSize;        // edge length of the texture being written
    uint   gSeed;              // weather map seed
    float  gWeatherCellSize;   // relative size of the storm cells in the weather map
    float  gWeatherCoverageBias;

    float  gWeatherTypeBias;
    float3 _CloudNoisePad0;
};

RWTexture3D<float4> gVolumeOutput  : register(u0);
RWTexture2D<float4> gTextureOutput : register(u1);

static const float kPi = 3.14159265359f;

// ---------------------------------------------------------------------------
// Hashing (Hoskins-style, no transcendentals)
// ---------------------------------------------------------------------------

float3 Hash33(float3 p)
{
    p = frac(p * float3(0.1031f, 0.1030f, 0.0973f));
    p += dot(p, p.yxz + 33.33f);
    return frac((p.xxy + p.yxx) * p.zyx);
}

float2 Hash22(float2 p)
{
    float3 p3 = frac(p.xyx * float3(0.1031f, 0.1030f, 0.0973f));
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.xx + p3.yz) * p3.zy);
}

float Hash21(float2 p)
{
    float3 p3 = frac(p.xyx * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.x + p3.y) * p3.z);
}

// ---------------------------------------------------------------------------
// Periodic gradient (Perlin) noise
// ---------------------------------------------------------------------------

float3 PerlinGradient3D(float3 cell, float period)
{
    cell = fmod(cell + period, period);
    float3 h = Hash33(cell) * 2.0f - 1.0f;
    return normalize(h + 1e-4f);
}

float PerlinNoise3D(float3 p, float period)
{
    p *= period;
    float3 i = floor(p);
    float3 f = p - i;
    float3 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);

    float n000 = dot(PerlinGradient3D(i + float3(0, 0, 0), period), f - float3(0, 0, 0));
    float n100 = dot(PerlinGradient3D(i + float3(1, 0, 0), period), f - float3(1, 0, 0));
    float n010 = dot(PerlinGradient3D(i + float3(0, 1, 0), period), f - float3(0, 1, 0));
    float n110 = dot(PerlinGradient3D(i + float3(1, 1, 0), period), f - float3(1, 1, 0));
    float n001 = dot(PerlinGradient3D(i + float3(0, 0, 1), period), f - float3(0, 0, 1));
    float n101 = dot(PerlinGradient3D(i + float3(1, 0, 1), period), f - float3(1, 0, 1));
    float n011 = dot(PerlinGradient3D(i + float3(0, 1, 1), period), f - float3(0, 1, 1));
    float n111 = dot(PerlinGradient3D(i + float3(1, 1, 1), period), f - float3(1, 1, 1));

    float nx00 = lerp(n000, n100, u.x);
    float nx10 = lerp(n010, n110, u.x);
    float nx01 = lerp(n001, n101, u.x);
    float nx11 = lerp(n011, n111, u.x);
    return lerp(lerp(nx00, nx10, u.y), lerp(nx01, nx11, u.y), u.z);
}

float PerlinFbm3D(float3 p, float period, uint octaves)
{
    float sum = 0.0f;
    float amplitude = 1.0f;
    float normalization = 0.0f;

    for (uint i = 0; i < octaves; ++i)
    {
        sum += PerlinNoise3D(p, period) * amplitude;
        normalization += amplitude;
        period *= 2.0f;
        amplitude *= 0.5f;
    }

    return sum / max(normalization, 1e-5f);
}

float2 PerlinGradient2D(float2 cell, float period)
{
    cell = fmod(cell + period, period);
    float angle = Hash21(cell) * 2.0f * kPi;
    return float2(cos(angle), sin(angle));
}

float PerlinNoise2D(float2 p, float period)
{
    p *= period;
    float2 i = floor(p);
    float2 f = p - i;
    float2 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);

    float n00 = dot(PerlinGradient2D(i + float2(0, 0), period), f - float2(0, 0));
    float n10 = dot(PerlinGradient2D(i + float2(1, 0), period), f - float2(1, 0));
    float n01 = dot(PerlinGradient2D(i + float2(0, 1), period), f - float2(0, 1));
    float n11 = dot(PerlinGradient2D(i + float2(1, 1), period), f - float2(1, 1));

    return lerp(lerp(n00, n10, u.x), lerp(n01, n11, u.x), u.y);
}

float PerlinFbm2D(float2 p, float period, uint octaves)
{
    float sum = 0.0f;
    float amplitude = 1.0f;
    float normalization = 0.0f;

    for (uint i = 0; i < octaves; ++i)
    {
        sum += PerlinNoise2D(p, period) * amplitude;
        normalization += amplitude;
        period *= 2.0f;
        amplitude *= 0.5f;
    }

    return sum / max(normalization, 1e-5f);
}

// ---------------------------------------------------------------------------
// Periodic Worley (cellular) noise.  Inverted so 1 sits on a feature point and
// falls to 0 between cells, which is the orientation the shape model expects.
// ---------------------------------------------------------------------------

float WorleyNoise3D(float3 p, float cells)
{
    p *= cells;
    float3 i = floor(p);
    float3 f = p - i;

    float minDistSq = 1.0e9f;
    for (int z = -1; z <= 1; ++z)
    {
        for (int y = -1; y <= 1; ++y)
        {
            for (int x = -1; x <= 1; ++x)
            {
                float3 neighbour = float3(x, y, z);
                float3 cell = fmod(i + neighbour + cells, cells);
                float3 toFeature = neighbour + Hash33(cell) - f;
                minDistSq = min(minDistSq, dot(toFeature, toFeature));
            }
        }
    }

    return 1.0f - saturate(sqrt(minDistSq));
}

float WorleyFbm3D(float3 p, float cells)
{
    return WorleyNoise3D(p, cells) * 0.625f
         + WorleyNoise3D(p, cells * 2.0f) * 0.25f
         + WorleyNoise3D(p, cells * 4.0f) * 0.125f;
}

float RemapRange(float value, float inMin, float inMax, float outMin, float outMax)
{
    return outMin + (value - inMin) * (outMax - outMin) / max(inMax - inMin, 1e-5f);
}

// ---------------------------------------------------------------------------
// Base shape volume — 128^3.
// R holds the Perlin-Worley field that carries the billowy silhouette; GBA
// hold progressively higher frequency Worley FBM used to erode that silhouette.
// ---------------------------------------------------------------------------

[numthreads(8, 8, 8)]
void CSBaseShape(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId >= gTargetSize.xxx))
        return;

    float3 uvw = (float3(dispatchThreadId) + 0.5f) / (float)gTargetSize;

    // Perlin FBM remapped into [0,1], then dilated by the low frequency Worley
    // FBM so the result keeps Perlin's connectivity but Worley's puffy edges.
    float perlin = PerlinFbm3D(uvw, 4.0f, 4) * 0.5f + 0.5f;
    float worleyLow = WorleyFbm3D(uvw, 4.0f);
    float perlinWorley = RemapRange(perlin, worleyLow - 1.0f, 1.0f, 0.0f, 1.0f);

    gVolumeOutput[dispatchThreadId] = float4(
        saturate(perlinWorley),
        saturate(worleyLow),
        saturate(WorleyFbm3D(uvw, 8.0f)),
        saturate(WorleyFbm3D(uvw, 16.0f)));
}

// ---------------------------------------------------------------------------
// Detail volume — 32^3 of high frequency Worley used for edge erosion.
// ---------------------------------------------------------------------------

[numthreads(8, 8, 8)]
void CSDetail(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId >= gTargetSize.xxx))
        return;

    float3 uvw = (float3(dispatchThreadId) + 0.5f) / (float)gTargetSize;

    // Ridged Perlin in alpha gives the raymarcher a wispy, filament-like term
    // to blend towards near the top of the layer.
    float ridged = 1.0f - abs(PerlinFbm3D(uvw, 4.0f, 3) * 2.0f);

    gVolumeOutput[dispatchThreadId] = float4(
        saturate(WorleyFbm3D(uvw, 2.0f)),
        saturate(WorleyFbm3D(uvw, 3.0f)),
        saturate(WorleyFbm3D(uvw, 4.0f)),
        saturate(ridged));
}

// ---------------------------------------------------------------------------
// Curl noise — 128^2.  Curl of a 2D Perlin potential, encoded into [0,1].
// Swirls the detail lookup so erosion follows plausible turbulence instead of
// looking like uniform noise subtraction.
// ---------------------------------------------------------------------------

[numthreads(8, 8, 1)]
void CSCurl(uint2 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId >= gTargetSize.xx))
        return;

    float2 uv = (float2(dispatchThreadId) + 0.5f) / (float)gTargetSize;
    const float epsilon = 1.0f / (float)gTargetSize;

    float potentialUp    = PerlinFbm2D(uv + float2(0.0f, epsilon), 8.0f, 3);
    float potentialDown  = PerlinFbm2D(uv - float2(0.0f, epsilon), 8.0f, 3);
    float potentialRight = PerlinFbm2D(uv + float2(epsilon, 0.0f), 8.0f, 3);
    float potentialLeft  = PerlinFbm2D(uv - float2(epsilon, 0.0f), 8.0f, 3);

    float2 curl = float2(
         (potentialUp - potentialDown),
        -(potentialRight - potentialLeft)) / (2.0f * epsilon);

    curl = clamp(curl * 0.05f, -1.0f, 1.0f);

    gTextureOutput[dispatchThreadId] = float4(curl * 0.5f + 0.5f, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// Weather map — 512^2.
//   R = coverage        (how much of the layer fills the sky at this point)
//   G = cloud type      (0 stratiform .. 1 towering cumulonimbus)
//   B = precipitation   (darkens and thickens the medium)
//   A = height offset   (pushes the local cloud base up or down)
// ---------------------------------------------------------------------------

[numthreads(8, 8, 1)]
void CSWeather(uint2 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId >= gTargetSize.xx))
        return;

    float2 uv = (float2(dispatchThreadId) + 0.5f) / (float)gTargetSize;
    float2 seedOffset = Hash22(float2((float)gSeed, (float)gSeed * 7.0f + 13.0f)) * 32.0f;
    float2 p = uv + seedOffset;

    float cellPeriod = max(gWeatherCellSize, 0.25f);

    // Large storm cells modulated by a finer break-up layer.
    float cells = PerlinFbm2D(p, cellPeriod * 2.0f, 4) * 0.5f + 0.5f;
    float breakup = PerlinFbm2D(p + 11.37f, cellPeriod * 8.0f, 3) * 0.5f + 0.5f;
    float coverage = saturate(RemapRange(cells, 0.15f, 0.9f, 0.0f, 1.0f));
    coverage = saturate(coverage * lerp(0.65f, 1.0f, breakup) + gWeatherCoverageBias);

    // Cloud type correlates with coverage: the densest regions build vertically.
    float typeNoise = PerlinFbm2D(p + 53.71f, cellPeriod * 3.0f, 3) * 0.5f + 0.5f;
    float cloudType = saturate(lerp(typeNoise * 0.6f, 1.0f, coverage * coverage) + gWeatherTypeBias);

    // Precipitation only forms inside the thickest, most vertical cells.
    float precipitation = saturate(RemapRange(coverage * cloudType, 0.55f, 1.0f, 0.0f, 1.0f));

    float heightOffset = PerlinFbm2D(p + 91.13f, cellPeriod * 4.0f, 2) * 0.5f + 0.5f;

    gTextureOutput[dispatchThreadId] = float4(coverage, cloudType, precipitation, heightOffset);
}
