// Particle_Update.hlsl
// Compute shader - spawns and integrates one particle system's particles.
//
// The buffer is a ring: the CPU advances SpawnCursor by however many particles
// the emitter owes this frame, and the slots inside that window respawn. Since
// the ring advances at the spawn rate and the CPU sizes the buffer from
// SpawnRate * maximum lifetime, a slot always comes back around after its
// previous occupant has expired. That keeps the spawn rate exact without an
// atomic free-list, and if a buffer is ever undersized the oldest particles are
// the ones recycled, which is the right answer anyway.
//
// Every thread owns exactly one slot and writes only that slot, so the update
// runs in place - no ping-pong buffer and no UAV barrier between passes.
//
// Targets cs_5_0 so the startup shader cache (which guesses that profile for
// compute entry points) produces the same bytecode this renderer asks for at
// run time, rather than compiling the file a second time.

struct Particle
{
    float3 Position;
    float  Age;          // seconds lived
    float3 Velocity;
    float  Life;         // total lifetime in seconds; <= 0 means the slot is empty
    float4 Seed;         // four stable per-particle random values in [0,1)
    float  Rotation;     // current sprite rotation, radians
    float  RotationRate; // radians per second
    float  SizeScale;    // per-particle size multiplier from SizeVariance
    float  FrameOffset;  // flipbook start frame offset in [0,1)
};

RWStructuredBuffer<Particle> Particles : register(u0);

// Emitter shape ids - must match ParticleEmitterShape in Components.h.
#define SHAPE_POINT  0u
#define SHAPE_SPHERE 1u
#define SHAPE_BOX    2u
#define SHAPE_CONE   3u
#define SHAPE_DISC   4u
#define SHAPE_EDGE   5u

// ResetMode values written by ParticleRenderer.
#define RESET_NONE    0u
#define RESET_CLEAR   1u
#define RESET_PREWARM 2u

cbuffer ParticleUpdateConstants : register(b0)
{
    // Emitter local space -> world. Transposed on upload, so this shader uses
    // row-vector multiplication, matching the rest of the engine.
    float4x4 EmitterToWorld;

    float3 EmitterPosition;
    float  DeltaTime;

    float3 Acceleration;
    float  GlobalTime;

    float3 WindVelocity;
    float  WindInfluence;

    float3 ShapeExtents;
    float  ShapeRadius;

    float  ConeAngleRadians;
    float  ShapeShellBias;
    uint   ShapeType;
    uint   ParticleCount;

    float  Lifetime;
    float  LifetimeVariance;
    float  InitialSpeed;
    float  SpeedVariance;

    float  Drag;
    float  TurbulenceStrength;
    float  TurbulenceFrequency;
    float  TurbulenceSpeed;

    float  VortexStrength;
    float  RotationRateRadians;
    float  RotationRateVariance;
    float  StartRotationRadians;

    float  RandomStartRotation;
    float  SizeVariance;
    uint   SpawnCursor;
    uint   SpawnCount;

    uint   RandomSeed;
    uint   ResetMode;
    uint   FlipbookRandomStart;
    float  PrewarmSpan;   // seconds of life to stagger a prewarm fill across
};

// ─── Hashing and noise ───────────────────────────────────────────────────────

uint HashUint(uint x)
{
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

float HashFloat(uint x)
{
    return float(HashUint(x) & 0x00FFFFFFu) / 16777216.0f;
}

float HashFloat3(int3 p)
{
    const uint h = HashUint(uint(p.x * 73856093 ^ p.y * 19349663 ^ p.z * 83492791));
    return float(h & 0x00FFFFFFu) / 16777216.0f;
}

// Trilinearly interpolated value noise. Cheaper than gradient noise and the
// difference is invisible once it is only perturbing a velocity.
float ValueNoise3(float3 p)
{
    const float3 floored = floor(p);
    const int3 i = int3(floored);
    const float3 f = p - floored;
    const float3 u = f * f * (3.0f - 2.0f * f);

    const float n000 = HashFloat3(i + int3(0, 0, 0));
    const float n100 = HashFloat3(i + int3(1, 0, 0));
    const float n010 = HashFloat3(i + int3(0, 1, 0));
    const float n110 = HashFloat3(i + int3(1, 1, 0));
    const float n001 = HashFloat3(i + int3(0, 0, 1));
    const float n101 = HashFloat3(i + int3(1, 0, 1));
    const float n011 = HashFloat3(i + int3(0, 1, 1));
    const float n111 = HashFloat3(i + int3(1, 1, 1));

    const float nx00 = lerp(n000, n100, u.x);
    const float nx10 = lerp(n010, n110, u.x);
    const float nx01 = lerp(n001, n101, u.x);
    const float nx11 = lerp(n011, n111, u.x);

    return lerp(lerp(nx00, nx10, u.y), lerp(nx01, nx11, u.y), u.z);
}

// Three decorrelated noise fields read as one vector, scrolled downward over
// time. Not strictly divergence free - that would cost six times as many noise
// fetches - but a flame only needs the flow to look like it curls, and two
// octaves of this is what turns a cone of sprites into something that licks.
float3 TurbulenceVector(float3 worldPosition, float time)
{
    const float3 p = worldPosition * TurbulenceFrequency + float3(0.0f, 0.0f, -time * TurbulenceSpeed);

    float3 coarse;
    coarse.x = ValueNoise3(p);
    coarse.y = ValueNoise3(p + float3(31.7f, 11.3f, 47.1f));
    coarse.z = ValueNoise3(p + float3(71.9f, 83.3f, 23.5f));

    const float3 q = p * 2.13f;
    float3 fine;
    fine.x = ValueNoise3(q + float3(5.1f, 91.7f, 13.9f));
    fine.y = ValueNoise3(q + float3(63.3f, 37.1f, 97.7f));
    fine.z = ValueNoise3(q + float3(19.3f, 59.9f, 3.7f));

    return (coarse - 0.5f) * 2.0f + (fine - 0.5f) * 0.9f;
}

// ─── Spawn ───────────────────────────────────────────────────────────────────

// Blend a volume sample toward the shape's surface. Lerping the radius rather
// than snapping keeps a partial shell bias usable as a dial instead of a switch.
float ApplyShellBias(float normalizedRadius)
{
    return lerp(normalizedRadius, 1.0f, ShapeShellBias);
}

float3 RandomUnitVector(float u1, float u2)
{
    const float z = u1 * 2.0f - 1.0f;
    const float phi = u2 * 6.28318530718f;
    const float r = sqrt(max(0.0f, 1.0f - z * z));
    return float3(r * cos(phi), r * sin(phi), z);
}

struct SpawnState
{
    float3 LocalPosition;
    float3 LocalDirection;
};

SpawnState SampleEmitterShape(uint seed)
{
    const float r0 = HashFloat(seed + 11u);
    const float r1 = HashFloat(seed + 23u);
    const float r2 = HashFloat(seed + 37u);
    const float r3 = HashFloat(seed + 53u);

    SpawnState state;
    state.LocalPosition = float3(0.0f, 0.0f, 0.0f);
    state.LocalDirection = float3(0.0f, 0.0f, 1.0f);

    if (ShapeType == SHAPE_SPHERE)
    {
        // Cube root distributes samples evenly through the volume; without it
        // the centre of the sphere is far denser than its shell.
        const float radius = ApplyShellBias(pow(max(r2, 1e-6f), 1.0f / 3.0f)) * ShapeRadius;
        const float3 direction = RandomUnitVector(r0, r1);
        state.LocalPosition = direction * radius;
        state.LocalDirection = direction;
    }
    else if (ShapeType == SHAPE_BOX)
    {
        state.LocalPosition = (float3(r0, r1, r2) * 2.0f - 1.0f) * ShapeExtents;
        state.LocalDirection = float3(0.0f, 0.0f, 1.0f);
    }
    else if (ShapeType == SHAPE_CONE)
    {
        const float radius = ApplyShellBias(sqrt(r2)) * ShapeRadius;
        const float angle = r3 * 6.28318530718f;
        state.LocalPosition = float3(cos(angle) * radius, sin(angle) * radius, 0.0f);

        // Spray direction: a cone about +Z whose half-angle is ConeAngleRadians.
        // The particle leans outward in the direction it spawned from the axis,
        // so a wide cone flares rather than crossing over itself.
        const float spread = sqrt(r0) * ConeAngleRadians;
        const float spreadAngle = r1 * 6.28318530718f;
        const float sinSpread = sin(spread);
        state.LocalDirection = normalize(float3(
            cos(spreadAngle) * sinSpread,
            sin(spreadAngle) * sinSpread,
            cos(spread)));
    }
    else if (ShapeType == SHAPE_DISC)
    {
        const float radius = ApplyShellBias(sqrt(r2)) * ShapeRadius;
        const float angle = r3 * 6.28318530718f;
        const float2 offset = float2(cos(angle), sin(angle));
        state.LocalPosition = float3(offset * radius, 0.0f);
        state.LocalDirection = normalize(float3(offset * 0.35f, 1.0f));
    }
    else if (ShapeType == SHAPE_EDGE)
    {
        state.LocalPosition = float3((r0 * 2.0f - 1.0f) * ShapeRadius, 0.0f, 0.0f);
        state.LocalDirection = float3(0.0f, 0.0f, 1.0f);
    }
    else // SHAPE_POINT
    {
        state.LocalDirection = RandomUnitVector(r0, r1);
    }

    return state;
}

Particle SpawnParticle(uint slot, uint spawnEvent, float ageAtSpawn)
{
    // Mixing the slot with the spawn event id means the same slot draws fresh
    // randomness each time it is recycled, instead of every generation of that
    // slot looking identical.
    const uint seed = HashUint(slot * 2654435761u + spawnEvent * 40503u + RandomSeed);

    const SpawnState shape = SampleEmitterShape(seed);

    const float lifeRandom = HashFloat(seed + 71u) * 2.0f - 1.0f;
    const float speedRandom = HashFloat(seed + 89u) * 2.0f - 1.0f;
    const float sizeRandom = HashFloat(seed + 101u) * 2.0f - 1.0f;
    const float rotRandom = HashFloat(seed + 113u) * 2.0f - 1.0f;

    Particle p;
    p.Life = max(Lifetime * (1.0f + lifeRandom * LifetimeVariance), 0.01f);
    p.Age = ageAtSpawn;

    const float3 worldPosition = mul(float4(shape.LocalPosition, 1.0f), EmitterToWorld).xyz;
    const float3 worldDirection = normalize(mul(float4(shape.LocalDirection, 0.0f), EmitterToWorld).xyz);
    const float speed = max(InitialSpeed * (1.0f + speedRandom * SpeedVariance), 0.0f);

    p.Position = worldPosition;
    p.Velocity = worldDirection * speed;

    // A prewarmed particle is placed where the closed-form ballistic solution
    // says it would be after ageAtSpawn seconds. Turbulence is not replayed -
    // it converges within a frame or two - but without this the whole system
    // would start bunched at the emitter and visibly bloom outward on load.
    if (ageAtSpawn > 0.0f)
    {
        p.Position += p.Velocity * ageAtSpawn + 0.5f * Acceleration * ageAtSpawn * ageAtSpawn;
        p.Velocity += Acceleration * ageAtSpawn;
    }

    p.Seed = float4(
        HashFloat(seed + 131u),
        HashFloat(seed + 149u),
        HashFloat(seed + 163u),
        HashFloat(seed + 181u));

    p.Rotation = StartRotationRadians + p.Seed.x * RandomStartRotation * 6.28318530718f;
    p.RotationRate = RotationRateRadians * (1.0f + rotRandom * RotationRateVariance);
    p.Rotation += p.RotationRate * ageAtSpawn;
    p.SizeScale = max(1.0f + sizeRandom * SizeVariance, 0.05f);
    p.FrameOffset = (FlipbookRandomStart != 0u) ? p.Seed.y : 0.0f;

    return p;
}

// ─── Entry point ─────────────────────────────────────────────────────────────

[numthreads(256, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint slot = dispatchThreadId.x;
    if (slot >= ParticleCount)
        return;

    if (ResetMode == RESET_CLEAR)
    {
        Particle dead = (Particle)0;
        dead.Life = 0.0f;
        Particles[slot] = dead;
        return;
    }

    if (ResetMode == RESET_PREWARM)
    {
        // Stagger the fill across the emission window so the system comes up
        // already at steady state: slot 0 is about to die, the last slot has
        // just been born.
        const float fraction = (ParticleCount > 1u)
            ? (float(slot) / float(ParticleCount - 1u))
            : 0.0f;
        Particles[slot] = SpawnParticle(slot, 0u, PrewarmSpan * (1.0f - fraction));
        return;
    }

    Particle p = Particles[slot];

    // Respawn if this slot falls inside the ring window the CPU opened for this
    // frame. The window is expressed relative to the cursor so the comparison
    // stays correct across the wrap.
    if (SpawnCount > 0u)
    {
        const uint distance = (slot + ParticleCount - SpawnCursor) % ParticleCount;
        if (distance < SpawnCount)
        {
            // Spread the frame's spawns across the frame's time step instead of
            // stamping them all at the same instant, which would emit the whole
            // frame's worth of particles as one visible shell.
            const float subFrameAge = DeltaTime * (float(distance) / float(SpawnCount));
            const uint spawnEvent = (SpawnCursor + distance) / max(ParticleCount, 1u) + 1u;
            p = SpawnParticle(slot, spawnEvent, subFrameAge);
            Particles[slot] = p;
            return;
        }
    }

    if (p.Life <= 0.0f)
        return;

    p.Age += DeltaTime;
    if (p.Age >= p.Life)
    {
        // Leave the slot empty rather than wrapping it: the ring decides when a
        // slot is reused, and a particle that dies early must stay gone until
        // the cursor reaches it.
        p.Life = 0.0f;
        Particles[slot] = p;
        return;
    }

    float3 velocity = p.Velocity;

    velocity += Acceleration * DeltaTime;

    if (TurbulenceStrength > 0.0f)
    {
        velocity += TurbulenceVector(p.Position, GlobalTime) * TurbulenceStrength * DeltaTime;
    }

    if (WindInfluence > 0.0f)
    {
        // Push toward the wind's velocity rather than adding it, so a strong
        // wind advects the plume instead of accelerating it without limit.
        velocity += (WindVelocity - velocity) * saturate(WindInfluence * DeltaTime);
    }

    if (VortexStrength != 0.0f)
    {
        // Swirl about the emitter's world up axis. The tangent is taken in the
        // horizontal plane, so the twist reads as rotation rather than lift.
        const float3 toAxis = p.Position - EmitterPosition;
        const float2 radial = toAxis.xy;
        const float radiusSq = dot(radial, radial);
        if (radiusSq > 1e-6f)
        {
            const float3 tangent = float3(-radial.y, radial.x, 0.0f) * rsqrt(radiusSq);
            velocity += tangent * VortexStrength * DeltaTime;
        }
    }

    // Exponential drag, so the damping is frame-rate independent.
    if (Drag > 0.0f)
    {
        velocity *= exp(-Drag * DeltaTime);
    }

    p.Velocity = velocity;
    p.Position += velocity * DeltaTime;
    p.Rotation += p.RotationRate * DeltaTime;

    Particles[slot] = p;
}
