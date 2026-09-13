// VegetationInteraction.hlsl
// Maintains the camera-centred interaction map that grass and bushes bend away
// from.
//
// The map is a top-down field covering a square region around the camera.  Each
// texel stores the accumulated horizontal shove at that patch of ground:
//   RG - push vector in world XY, metres
//   B  - how recently it was touched, used to shape the spring-back
//   A  - unused
//
// The key property is that cost is completely independent of how many grass
// instances exist.  A hundred interactors splatting into a 512x512 texture
// costs the same whether the field holds ten thousand blades or ten million,
// which is why this is the right model for grass and why per-instance physics
// is not.
//
// The pass ping-pongs: it reads the previous frame's map, reprojects it for
// however far the camera has moved, decays it, then splats this frame's
// interactors on top.  Reprojection is what lets a trail persist on the ground
// while the camera walks away from it.

Texture2D<float4>   gPreviousMap : register(t0);
StructuredBuffer<float4> gInteractors : register(t1); // xyz = world position, w = radius
StructuredBuffer<float4> gInteractorVelocities : register(t2); // xyz = world velocity, w = strength

RWTexture2D<float4> gCurrentMap  : register(u0);

SamplerState gClampSampler : register(s0);

cbuffer VegetationInteractionConstants : register(b0)
{
    // Centre of the map this frame and last frame, world XY.
    float2 gCentre;
    float2 gPreviousCentre;

    // Side length of the region the map covers, in metres.
    float  gWorldSize;
    // Texture resolution (square).
    float  gResolution;
    float  gDeltaTime;
    uint   gInteractorCount;

    // Seconds for a full-strength impression to relax back to nothing.  This is
    // what decides whether footsteps leave a lasting trail or the grass snaps
    // straight behind the player.
    float  gRecoverySeconds;
    // Hard cap on the shove magnitude so a fast interactor cannot fold grass
    // through the ground.
    float  gMaxPush;
    float2 _InteractionPad0;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 texel = dispatchThreadId.xy;
    if (texel.x >= (uint)gResolution || texel.y >= (uint)gResolution)
        return;

    const float halfSize = gWorldSize * 0.5f;

    // World position of this texel's centre, using this frame's map origin.
    const float2 uv = (float2(texel) + 0.5f) / gResolution;
    const float2 worldXY = gCentre + (uv - 0.5f) * gWorldSize;

    // --- Reproject the previous frame ---------------------------------------
    // Sampling the old map at this texel's world position (rather than at the
    // same texel) is what keeps impressions anchored to the ground as the
    // camera moves.
    const float2 previousUv = (worldXY - gPreviousCentre) / gWorldSize + 0.5f;

    float4 value = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (all(previousUv >= 0.0f) && all(previousUv <= 1.0f))
    {
        value = gPreviousMap.SampleLevel(gClampSampler, previousUv, 0);
    }

    // --- Decay ---------------------------------------------------------------
    // Exponential relaxation, so grass springs back quickly at first and then
    // eases into place.  Framerate independent by construction.
    const float recovery = max(gRecoverySeconds, 0.01f);
    const float decay = exp(-gDeltaTime / recovery * 3.0f);
    value.xy *= decay;
    value.z = max(value.z - gDeltaTime / recovery, 0.0f);

    // --- Splat this frame's interactors ---------------------------------------
    for (uint i = 0; i < gInteractorCount; ++i)
    {
        const float4 interactor = gInteractors[i];
        const float4 motion     = gInteractorVelocities[i];

        const float radius = max(interactor.w, 0.01f);
        const float2 toTexel = worldXY - interactor.xy;
        const float  distance = length(toTexel);

        if (distance > radius)
            continue;

        // Smooth falloff to the rim; a hard edge would show up as a visible
        // disc of flattened grass.
        const float falloff = 1.0f - saturate(distance / radius);
        const float weight = falloff * falloff * max(motion.w, 0.0f);

        // Push outward from the interactor, plus a component along its motion
        // so a moving object lays grass down in the direction it travels
        // instead of only shouldering it aside.
        const float2 outward = (distance > 1e-4f) ? (toTexel / distance) : float2(0.0f, 0.0f);
        const float2 push = (outward * 0.6f + motion.xy * 0.4f) * weight;

        value.xy += push;
        value.z = max(value.z, weight);
    }

    // Clamp the magnitude rather than each axis, so limiting never rotates the
    // push direction.
    const float magnitude = length(value.xy);
    if (magnitude > gMaxPush)
        value.xy *= gMaxPush / magnitude;

    gCurrentMap[texel] = float4(value.xy, value.z, 0.0f);
}
