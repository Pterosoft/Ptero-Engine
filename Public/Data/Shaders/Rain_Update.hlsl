// Rain_Update.hlsl
// Compute shader (cs_6_5) - updates rain particle positions and wraps them
// around a camera-relative bounding box for infinite-seeming rainfall.

struct RainDrop
{
    float3 Position;
    float  Age;
    float3 Velocity;
    float  State;
    float4 Variation;
    float4 ImpactData;
};

StructuredBuffer<RainDrop>   ParticlesIn  : register(t0);
RWStructuredBuffer<RainDrop> ParticlesOut : register(u0);

cbuffer RainConstants : register(b0)
{
    float  DeltaTime;
    float3 WindVector;
    float  Gravity;
    float3 CameraPosition;
    float  GroundHeight;
    float3 PreviousCameraPosition;
    float  GlobalTime;
    float3 CameraRight;
    float  TurbulenceStrength;
    float3 CameraForward;
    float  TurbulenceScale;
    float3 CameraUp;
    float  SplashLifetime;
    float3 PreviousCameraRight;
    float  MistLifetime;
    float3 PreviousCameraForward;
    float  BaseSplashSize;
    float3 PreviousCameraUp;
    float  BaseMistStrength;
    float3 BoundingBoxExtents;
    uint   ParticleCount;
};

// A simple low-quality but fast hash for pseudo-random numbers on the GPU.
float Hash(uint n)
{
    n = (n << 13u) ^ n;
    n = n * (n * n * 15731u + 789221u) + 1376312589u;
    return float(n & 0x7fffffffu) / float(0x7fffffff);
}

float Hash2(uint seed, uint offset)
{
    return Hash(seed * 1664525u + offset);
}

float3 Hash3(uint seed)
{
    return float3(
        Hash2(seed, 17u),
        Hash2(seed, 31u),
        Hash2(seed, 47u));
}

float Noise(float3 p)
{
    float3 cell = floor(p);
    float3 fracPart = frac(p);
    fracPart = fracPart * fracPart * (3.0f - 2.0f * fracPart);

    const uint baseSeed = asuint(cell.x * 73856093.0f) ^ asuint(cell.y * 19349663.0f) ^ asuint(cell.z * 83492791.0f);
    const float c000 = Hash2(baseSeed, 0u);
    const float c100 = Hash2(baseSeed, 1u);
    const float c010 = Hash2(baseSeed, 2u);
    const float c110 = Hash2(baseSeed, 3u);
    const float c001 = Hash2(baseSeed, 4u);
    const float c101 = Hash2(baseSeed, 5u);
    const float c011 = Hash2(baseSeed, 6u);
    const float c111 = Hash2(baseSeed, 7u);

    const float x00 = lerp(c000, c100, fracPart.x);
    const float x10 = lerp(c010, c110, fracPart.x);
    const float x01 = lerp(c001, c101, fracPart.x);
    const float x11 = lerp(c011, c111, fracPart.x);
    const float y0 = lerp(x00, x10, fracPart.y);
    const float y1 = lerp(x01, x11, fracPart.y);
    return lerp(y0, y1, fracPart.z);
}

float3 ComputeSpawnPosition(uint id, float ageSeed)
{
    const uint randomSeed = id + (uint)(ageSeed * 997.0f) + (uint)(GlobalTime * 131.0f);
    const float2 spawnOffset = float2(
        Hash2(randomSeed, 0u) * 2.0f - 1.0f,
        Hash2(randomSeed, 1u) * 2.0f - 1.0f);
    const float spawnHeight = Hash2(randomSeed, 2u) * BoundingBoxExtents.z;

    return CameraPosition
        + CameraRight * (spawnOffset.x * BoundingBoxExtents.x)
        + CameraForward * (spawnOffset.y * BoundingBoxExtents.y)
        + CameraUp * (BoundingBoxExtents.z * 0.65f + spawnHeight * 0.35f);
}

void ResetToRain(inout RainDrop p, uint id)
{
    const float4 previousVariation = p.Variation;
    const uint baseSeed = id + (uint)(GlobalTime * 211.0f) + (uint)(p.Age * 173.0f);
    const float4 nextVariation = float4(
        lerp(0.45f, 1.75f, Hash2(baseSeed, 10u)),
        lerp(0.65f, 1.55f, Hash2(baseSeed, 11u)),
        lerp(0.35f, 1.4f, Hash2(baseSeed, 12u)),
        Hash2(baseSeed, 13u) * 6.2831853f);

    p.Position = ComputeSpawnPosition(id, p.Age + nextVariation.w);
    p.Velocity = float3(
        WindVector.x * nextVariation.y,
        WindVector.y * nextVariation.y,
        WindVector.z * 0.35f - (Gravity + 5.5f * nextVariation.y));
    p.Age = 0.0f;
    p.State = 0.0f;
    p.Variation = nextVariation;
    p.ImpactData = float4(p.Position, previousVariation.z);
}

[numthreads(256, 1, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    const uint id = DTid.x;
    if (id >= ParticleCount)
        return;

    RainDrop p = ParticlesIn[id];

    if (all(p.Variation.xyz == 0.0f))
    {
        const float4 seedVariation = float4(
            lerp(0.45f, 1.75f, Hash2(id, 20u)),
            lerp(0.65f, 1.55f, Hash2(id, 21u)),
            lerp(0.35f, 1.4f, Hash2(id, 22u)),
            Hash2(id, 23u) * 6.2831853f);
        p.Variation = seedVariation;
        p.ImpactData = float4(p.Position, 0.0f);
    }

    // Keep the rain volume camera-relative by translation only. The weather box
    // stays aligned to world space so rotating the camera cannot move the rain
    // out of view.
    p.Position += CameraPosition - PreviousCameraPosition;

    if (p.State > 0.5f)
    {
        p.Age += DeltaTime;

        if (p.State < 1.5f)
        {
            const float splashAge = p.Age;
            const float splashProgress = SplashLifetime > 0.0f ? saturate(splashAge / SplashLifetime) : 1.0f;
            p.Position = p.ImpactData.xyz;
            p.ImpactData.w = BaseSplashSize * (0.8f + p.Variation.x * 0.2f) * (0.45f + splashProgress * 0.35f);

            if (splashProgress >= 1.0f)
            {
                ResetToRain(p, id);
            }
        }
        else
        {
            const float mistAge = p.Age;
            const float mistProgress = MistLifetime > 0.0f ? saturate(mistAge / MistLifetime) : 1.0f;
            const float mistNoise = Noise(float3(p.ImpactData.xy * 0.35f, GlobalTime * 0.45f) + p.Variation.w);
            const float2 driftDir = normalize(float2(cos(p.Variation.w + mistNoise), sin(p.Variation.w + mistNoise)) + float2(1.0e-4f, 1.0e-4f));

            p.Position = p.ImpactData.xyz
                + float3(driftDir * (mistProgress * 0.45f * (0.5f + p.Variation.x)), 0.05f + mistProgress * 0.18f);
            p.ImpactData.w = BaseMistStrength * (1.0f - mistProgress) * (0.5f + p.Variation.z);

            if (mistProgress >= 1.0f)
            {
                ResetToRain(p, id);
            }
        }

        ParticlesOut[id] = p;
        return;
    }

    // Integrate in the engine's Z-up space. Wind is treated as a steady drift,
    // while gravity is the only continuous acceleration.
    const float3 noiseSamplePos = p.Position * TurbulenceScale + float3(GlobalTime * 0.85f + p.Variation.w, GlobalTime * 0.57f, GlobalTime * 0.33f + p.Variation.x * 3.1f);
    const float2 turbulence = float2(
        Noise(noiseSamplePos + float3(11.0f, 0.0f, 0.0f)),
        Noise(noiseSamplePos + float3(0.0f, 23.0f, 0.0f))) * 2.0f - 1.0f;
    const float2 swirl = float2(cos(p.Age * 3.5f + p.Variation.w), sin(p.Age * 2.75f + p.Variation.w));
    const float2 horizontalWind = float2(WindVector.x, WindVector.y) * p.Variation.y
        + (turbulence + swirl * 0.35f) * TurbulenceStrength * p.Variation.z;

    p.Velocity.x = horizontalWind.x;
    p.Velocity.y = horizontalWind.y;
    p.Velocity.z += WindVector.z * DeltaTime * 0.35f;
    p.Velocity.z -= Gravity * DeltaTime;
    p.Velocity.z = min(p.Velocity.z, -3.5f * p.Variation.y);

    p.Position += p.Velocity * DeltaTime;
    p.Age      += DeltaTime;

    const float alphaVariance = saturate(p.Variation.z);
    const float streakVariance = saturate(p.Variation.x * 0.75f);
    p.ImpactData.w = alphaVariance * 0.7f + streakVariance * 0.3f;

    if (p.Position.z <= GroundHeight)
    {
        p.Position.z = GroundHeight;
        p.State = 1.0f;
        p.Age = 0.0f;
        p.ImpactData = float4(p.Position, BaseSplashSize * (0.8f + p.Variation.x * 0.2f));
        p.Velocity = float3(0.0f, 0.0f, 0.0f);
        ParticlesOut[id] = p;
        return;
    }

    // Camera-relative bounding box check in the engine's Z-up space. If the
    // particle has fallen below the bottom of the box, or drifted outside the
    // horizontal XY extents, recycle it.
    float3 localPos = p.Position - CameraPosition;
    bool outOfBounds =
        localPos.z < -BoundingBoxExtents.z ||
        abs(localPos.x) > BoundingBoxExtents.x ||
        abs(localPos.y) > BoundingBoxExtents.y;

    if (outOfBounds)
    {
        ResetToRain(p, id);
    }

    ParticlesOut[id] = p;
}
