// RtGI_RayDebug.hlsli
// The ray visualisation overlay's capture format, shared by RtGI_RayGen.hlsl,
// which records paths into it, and RtGI_RayDebugDraw.hlsl, which draws them.
// Mirrors RtGIDebugRaySegment in RtGlobalIllumination.h.
//
// Slots are laid out per recorded pixel: for each ray, two per bounce - the
// bounce segment at 2b and the sun shadow ray from its hit at 2b + 1 - then
// one for the camera ray that found the pixel's surface. A path that ends
// early leaves its later slots empty (kind 0), which is how the draw pass
// finds where a path stops.

#ifndef RTGI_RAY_DEBUG_HLSLI
#define RTGI_RAY_DEBUG_HLSLI

static const uint kRayDebugEmpty         = 0u;
static const uint kRayDebugBounceHit     = 1u;   // GI segment that hit a surface
static const uint kRayDebugBounceMiss    = 2u;   // GI segment that escaped; end = start + direction
static const uint kRayDebugShadowLit     = 3u;   // sun shadow ray that reached the sun; end = start + direction
static const uint kRayDebugShadowBlocked = 4u;   // sun shadow ray that was blocked
static const uint kRayDebugCamera        = 5u;   // camera to the recorded pixel's surface

static const uint kRayDebugNoSlot = 0xffffffffu;

struct RtGIDebugRaySegment
{
    float3 start;
    uint   kind;
    float3 end;
    uint   bounce;
    // Bounce hit: the light the hit reflects toward the segment's start from
    // next-event estimation (albedo already applied). Miss: the sky radiance.
    float3 light;
    float  _pad0;
    // Bounce hit: the hit's albedo, which scales whatever the next bounce brings back.
    float3 albedo;
    float  _pad1;
};

#endif
