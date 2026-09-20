// LightShapes.hlsli
// The one GPU light record, and the one place an emitter shape is resolved.
//
// PteroLightData must match DeferredLightingPass::PointLightGpu exactly. Every
// pass that reads the scene's light array - deferred shading, ray-traced GI,
// radiance probes, radiance cascades - uploads that same C++ struct, so they
// all include this header rather than each keeping a private copy. Five
// hand-maintained mirrors of one struct is how a field added on the C++ side
// silently shifts the array stride in whichever shader nobody remembered.
//
// Shape handling lives here for the same reason. If the deferred pass and the
// GI pass disagreed about where a spot's cone ends, indirect light would spill
// outside the cone that direct light respects.

#ifndef PTERO_LIGHT_SHAPES_HLSLI
#define PTERO_LIGHT_SHAPES_HLSLI

// Emitter shapes - must match LightType in Components.h.
#define PTERO_LIGHT_TYPE_POINT 0
#define PTERO_LIGHT_TYPE_SPOT  1
#define PTERO_LIGHT_TYPE_RECT  2

struct PteroLightData
{
    float3 Position;        // world-space position (rect: the panel's centre)
    float  Radius;          // influence radius in metres
    float3 Color;           // pre-multiplied HDR colour (intensity baked in)
    float  InvRadiusSq;     // 1 / (Radius^2)
    float  FalloffExponent;
    float  SourceRadius;    // spherical source size; 0 = a true point
    float  CastShadows;
    float  ShadowIndex;     // index into the point shadow atlas, or -1

    float3 Direction;       // emission axis (spot and rect), from local -Z
    float  LightType;

    float  SpotCosInner;    // cosine of the inner (hotspot) half-angle
    float  SpotCosOuter;    // cosine of the outer half-angle
    float  RectHalfWidth;
    float  RectHalfHeight;

    float3 RectRight;       // rectangle's local X in world space
    float  RectTwoSided;
};

// A light reduced to a single point to light from, plus a scalar for whatever
// the shape blocks. Everything downstream - distance falloff, shadows, the
// BRDF - then treats all three shapes identically.
struct PteroResolvedLight
{
    float3 Position;
    float  ShapeMask;
};

//   Point : the light's own position, mask 1.
//   Spot  : the same position, masked by the cone falloff.
//   Rect  : the nearest point on the rectangle, masked by the emitting face's
//           cosine. Tracking the nearest point per pixel is what makes an area
//           light wrap around a surface and draw a long soft highlight instead
//           of pinching to the single hot dot a point light gives.
PteroResolvedLight PteroResolveLightShape(PteroLightData light, float3 worldPos)
{
    PteroResolvedLight resolved;
    resolved.Position  = light.Position;
    resolved.ShapeMask = 1.0f;

    const int lightType = (int)light.LightType;

    if (lightType == PTERO_LIGHT_TYPE_SPOT)
    {
        const float3 offset = worldPos - light.Position;
        const float  offsetLengthSq = dot(offset, offset);
        if (offsetLengthSq < 1e-8f)
            return resolved;

        const float cosAngle = dot(offset * rsqrt(offsetLengthSq), light.Direction);

        // Squared so the cone edge rolls off smoothly. A linear ramp reads as a
        // hard band across the lit ellipse once the cone is wide.
        const float cone = saturate((cosAngle - light.SpotCosOuter) /
                                    max(light.SpotCosInner - light.SpotCosOuter, 1e-4f));
        resolved.ShapeMask = cone * cone;
    }
    else if (lightType == PTERO_LIGHT_TYPE_RECT)
    {
        // cross(right, forward) is the local +Y the rectangle is defined in;
        // the operands the other way round give -Y, which symmetric extents
        // hide but which disagrees with the component, the C++ upload and the
        // editor gizmo.
        const float3 rectUp = cross(light.RectRight, light.Direction);
        const float3 offset = worldPos - light.Position;

        // Project the surface into the rectangle's own axes and clamp it to the
        // extents: the clamped point is the closest point on the rectangle, and
        // stands in for the whole emitter.
        const float2 projected = float2(dot(offset, light.RectRight), dot(offset, rectUp));
        const float2 clamped = clamp(
            projected,
            float2(-light.RectHalfWidth, -light.RectHalfHeight),
            float2( light.RectHalfWidth,  light.RectHalfHeight));

        resolved.Position = light.Position
                          + light.RectRight * clamped.x
                          + rectUp * clamped.y;

        // A rectangle emits from its front face only, falling off with the
        // cosine toward the surface - a panel seen edge-on contributes nothing.
        const float3 towardSurface = worldPos - resolved.Position;
        const float  towardLengthSq = dot(towardSurface, towardSurface);
        if (towardLengthSq < 1e-8f)
        {
            // The surface is sitting on the emitter. There is no direction to
            // take a cosine along, and leaving the mask at its initial 1.0 gave
            // geometry that intersects the panel the full unattenuated light -
            // the brightest part of the leak.
            resolved.ShapeMask = 0.0f;
            return resolved;
        }

        const float emitCos = dot(light.Direction, towardSurface * rsqrt(towardLengthSq));
        resolved.ShapeMask = (light.RectTwoSided > 0.5f) ? abs(emitCos) : saturate(emitCos);
    }

    return resolved;
}

// The distance the inverse-square law should be evaluated at.
//
// A true point light has a singularity at zero, and nothing ever gets that
// close to one because the light has no size to get inside of. An emitter with
// real extent is different: you cannot be nearer to it than its own size, and
// the irradiance from a panel saturates as you approach it rather than
// diverging. Evaluating 1/d^n at the closest point on a rectangle therefore
// explodes wherever geometry passes near the panel - which is what the bright
// streaks along beams and floor edges were, the closest point landing almost
// exactly on the surface being shaded.
//
// Softened rather than clamped, because max(d, r) puts a visible crease at
// d == r. sqrt(d^2 + r^2) is the same curve far away, flattens smoothly inside
// the emitter, and costs one multiply-add.
float PteroLightFalloffDistance(PteroLightData light, float dist)
{
    // A point light with no source radius keeps 1e-3, so this changes nothing
    // for the shapes that never had the problem.
    float emitterExtent = max(light.SourceRadius, 1e-3f);

    if ((int)light.LightType == PTERO_LIGHT_TYPE_RECT)
    {
        // The smaller half-extent: closer than that, a single stand-in point
        // has stopped describing an area at all.
        emitterExtent = max(min(light.RectHalfWidth, light.RectHalfHeight), 1e-3f);
    }

    return sqrt(dist * dist + emitterExtent * emitterExtent);
}

#endif // PTERO_LIGHT_SHAPES_HLSLI
