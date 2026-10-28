// Vegetation_Common.hlsli
// -----------------------
// Instance decoding and the wind/interaction bend model, shared by the
// vegetation G-Buffer pass, the shadow depth pass and the motion vector pass.
//
// Every pass that draws vegetation MUST use the same bend from this file.
// If the motion vector pass disagreed with the colour pass by even a little,
// TAA and DLSS would reproject every leaf to the wrong place and the canopy
// would smear whenever the wind blew.  That is why the bend lives here rather
// than inside Vegetation.hlsl.
//
// World space is Z-up and left-handed.  Bending is evaluated in instance-local
// space, where the origin is the base of the plant and +Z runs up its trunk.

#ifndef VEGETATION_COMMON_HLSLI
#define VEGETATION_COMMON_HLSLI

static const float kVegTwoPi = 6.28318530718f;

// Matches VegetationRenderer::GpuInstance (32 bytes).
struct VegetationInstance
{
    float3 Position;    // world-space base of the plant
    float  RotationZ;   // yaw about UpAxis, radians
    float3 UpAxis;      // resolved growth axis, already blended toward the surface normal
    float  Scale;       // uniform scale, with the engine's mesh world scale folded in
};

// BendModel values, mirroring VegetationBendModel in Components.h.
#define VEGETATION_BEND_NONE  0
#define VEGETATION_BEND_TREE  1
#define VEGETATION_BEND_GRASS 2

// Scene wind, unpacked from the pass constant buffer.
struct VegetationWind
{
    float DirectionRadians;
    float Strength;
    float GustAmplitude;
    float GustFrequency;
    float GustWavelength;
    float BendScale;
    float FlutterFrequency;
    float Enabled;
};

// Per-layer bend tuning, unpacked from the layer constant buffer.
struct VegetationBendParams
{
    int   Model;
    float WindInfluence;
    float Stiffness;
    float FlutterAmount;
    float InteractionInfluence;
};

float2 VegetationWindDirection(VegetationWind wind)
{
    return float2(cos(wind.DirectionRadians), sin(wind.DirectionRadians));
}

// Low-frequency gust field, scrolling downwind.  Sampling this in world space
// rather than per instance is what makes a gust travel visibly across a field
// instead of every plant reaching its extreme at the same instant.
float VegetationGust(float3 worldPosition, float time, VegetationWind wind)
{
    const float2 direction = VegetationWindDirection(wind);
    const float wavelength = max(wind.GustWavelength, 0.01f);
    const float travel = dot(worldPosition.xy, direction) / wavelength - time * wind.GustFrequency;

    // Two detuned waves; the irrational-ish ratio keeps the sum from repeating
    // on any period an observer would notice.
    return sin(travel * kVegTwoPi) * 0.6f + sin(travel * 2.7f + 1.3f) * 0.4f;
}

// Instantaneous wind speed at a point, in metres per second.
float VegetationWindSpeed(float3 worldPosition, float time, VegetationWind wind)
{
    if (wind.Enabled < 0.5f)
        return 0.0f;

    return wind.Strength + wind.GustAmplitude * VegetationGust(worldPosition, time, wind);
}

// Rotate `position` about the origin so it leans by `offset`, then restore its
// original distance from the origin.
//
// The renormalisation is the important part: without it a bent trunk gets
// visibly longer as the wind rises, and a grass blade pulls its own root out
// of the ground.  Preserving the radius turns the displacement into a rotation
// about the base, which is what an actual stem does.
float3 VegetationPivotBend(float3 position, float2 offset)
{
    const float radius = length(position);
    if (radius < 1e-4f)
        return position;

    position.xy += offset * radius;
    return normalize(position) * radius;
}

// Hierarchical tree bend.
//
// Vertex colour carries the weights, baked at import:
//   r - per-branch phase offset, decorrelates neighbouring branches
//   g - detail flutter amplitude, highest at leaf tips
//   b - branch stiffness weight
//   a - trunk bend weight, normalised height along the trunk
float3 VegetationBendTree(
    float3               localPosition,
    float3               localNormal,
    float4               vertexColour,
    float3               instanceWorldPosition,
    float                time,
    VegetationWind       wind,
    VegetationBendParams params)
{
    const float speed = VegetationWindSpeed(instanceWorldPosition, time, wind);
    const float drive = speed * params.WindInfluence * wind.BendScale / max(params.Stiffness, 0.01f);

    // 1. Main bend: the whole tree leans downwind, hinged at the base.
    //    Squaring the trunk weight keeps the lower trunk rigid and puts the
    //    travel in the crown, matching how a real trunk loads up.
    const float trunkWeight = vertexColour.a * vertexColour.a;
    const float2 windDirection = VegetationWindDirection(wind);

    // 0.02 converts m/s into a usable lean; at 8 m/s a fully weighted vertex
    // leans about 9 degrees, which reads as "branches in motion".
    float3 bent = VegetationPivotBend(localPosition, windDirection * trunkWeight * drive * 0.02f);

    // 2. Branch sway and 3. leaf flutter, both driven off the same clock but
    //    at different rates so they never lock together.
    const float phase = vertexColour.r * kVegTwoPi
                      + dot(instanceWorldPosition.xy, float2(0.7f, 1.3f));
    const float t = time * wind.FlutterFrequency;

    const float branch = sin(t + phase) * vertexColour.b;
    const float detail = sin(t * 2.7f + phase * 1.7f) * vertexColour.g;

    const float amount = params.FlutterAmount * drive * 0.01f;
    bent.z += branch * amount;
    bent   += localNormal * detail * amount * params.FlutterAmount;

    return bent;
}

// Grass blade bend: one hinge at the root, driven by wind plus the runtime
// interaction map.  Cheaper than the tree model because grass is drawn in
// vastly greater numbers, and it deliberately has no branch hierarchy.
//
// `pushWorldXY` is the accumulated shove sampled from the interaction map, in
// world XY.  Vertex colour alpha is the normalised height along the blade.
float3 VegetationBendGrass(
    float3               localPosition,
    float4               vertexColour,
    float3               instanceWorldPosition,
    float2               pushWorldXY,
    float                time,
    VegetationWind       wind,
    VegetationBendParams params)
{
    const float heightWeight = vertexColour.a * vertexColour.a;

    const float speed = VegetationWindSpeed(instanceWorldPosition, time, wind);
    const float drive = speed * params.WindInfluence * wind.BendScale / max(params.Stiffness, 0.01f);

    const float2 windDirection = VegetationWindDirection(wind);

    // A fast ripple on top of the sustained lean, phase-shifted per blade so a
    // patch of grass shimmers rather than moving as one sheet.
    const float phase  = dot(instanceWorldPosition.xy, float2(1.7f, 2.3f));
    const float ripple = sin(time * wind.FlutterFrequency * 1.6f + phase) * 0.25f;

    float2 offset = windDirection * heightWeight * drive * (0.02f + ripple * 0.01f);

    // Interaction dominates wind when something is actually pressing on the
    // blade, which is what makes a player walking through read as trampling
    // rather than as a breeze.
    offset += pushWorldXY * heightWeight * params.InteractionInfluence;

    return VegetationPivotBend(localPosition, offset);
}

// Orthonormal basis for an instance: yaw applied about its resolved up axis.
float3x3 VegetationInstanceBasis(float3 upAxis, float rotationZ)
{
    const float3 up = normalize(upAxis);

    // Any reference that is not parallel to `up`; the branch only matters for
    // instances planted on near-vertical surfaces.
    const float3 reference = (abs(up.z) > 0.999f) ? float3(1.0f, 0.0f, 0.0f) : float3(0.0f, 0.0f, 1.0f);

    const float3 right   = normalize(cross(reference, up));
    const float3 forward = cross(up, right);

    float sinYaw;
    float cosYaw;
    sincos(rotationZ, sinYaw, cosYaw);

    const float3 rotatedRight   = right * cosYaw + forward * sinYaw;
    const float3 rotatedForward = forward * cosYaw - right * sinYaw;

    // Rows are the basis axes, so mul(localVector, basis) expands to
    // local.x * right + local.y * forward + local.z * up.
    return float3x3(rotatedRight, rotatedForward, up);
}

// Sample the camera-centred interaction map and return the world XY shove at
// this position.  Returns zero outside the map, so grass beyond the map's
// radius simply stops reacting rather than snapping.
float2 VegetationSampleInteraction(
    Texture2D    interactionMap,
    SamplerState interactionSampler,
    float3       worldPosition,
    float2       mapCentreXY,
    float        mapWorldSize)
{
    const float2 uv = (worldPosition.xy - mapCentreXY) / max(mapWorldSize, 0.01f) + 0.5f;

    if (any(uv < 0.0f) || any(uv > 1.0f))
        return float2(0.0f, 0.0f);

    return interactionMap.SampleLevel(interactionSampler, uv, 0).xy;
}

// Full local-space bend for one vertex, dispatching on the layer's bend model.
// Every pass calls exactly this function so they cannot drift apart.
float3 VegetationApplyBend(
    float3               localPosition,
    float3               localNormal,
    float4               vertexColour,
    float3               instanceWorldPosition,
    float2               interactionPush,
    float                time,
    VegetationWind       wind,
    VegetationBendParams params)
{
    if (params.Model == VEGETATION_BEND_TREE)
    {
        return VegetationBendTree(
            localPosition, localNormal, vertexColour,
            instanceWorldPosition, time, wind, params);
    }

    if (params.Model == VEGETATION_BEND_GRASS)
    {
        return VegetationBendGrass(
            localPosition, vertexColour, instanceWorldPosition,
            interactionPush, time, wind, params);
    }

    return localPosition;
}

// Place a bent local-space vertex into the world.
float3 VegetationLocalToWorld(float3 localPosition, VegetationInstance instance)
{
    const float3x3 basis = VegetationInstanceBasis(instance.UpAxis, instance.RotationZ);
    return instance.Position + mul(localPosition * instance.Scale, basis);
}

#endif // VEGETATION_COMMON_HLSLI
