// RtGI_RayDebugDraw.hlsl  –  vs_5_0 / ps_5_0
// Draws the GI paths RtGI_RayGen.hlsl recorded (RtGI_RayDebug.hlsli) as
// anti-aliased lines over the lit scene, depth tested against it.
//
// No vertex buffer: six vertices per segment slot expand it into a screen-space
// quad of constant pixel width. Empty slots, and kinds that are switched off,
// collapse to nothing.

#include "RtGI_RayDebug.hlsli"

cbuffer RayDebugDrawConstants : register(b0)   // root constants
{
    float4x4 g_ViewProj;          // transposed, as the engine's other overlays take it
    float2   g_ViewportSize;
    float    g_HalfWidth;         // pixels
    float    g_MissLength;        // metres drawn of a ray that escaped
    float    g_ShadowLength;      // metres drawn of a sun shadow ray
    float    g_Intensity;         // fixed colours: brightness x a sunlit white surface
    float    g_RadianceScale;     // radiance colours: brightness / GI pre-exposure
    uint     g_ColorMode;         // 0 by bounce, 1 radiance
    uint     g_Flags;             // 1 shadow rays, 2 camera rays
    uint     g_SlotsPerRay;
    uint     g_SlotsPerPixel;
    uint     g_SegmentCount;
};

// Read as a UAV so the buffer stays in the state RayGen wrote it in (see
// RtGlobalIllumination::mDebugRayBuffer); the vertex shader only reads it.
RWStructuredBuffer<RtGIDebugRaySegment> u_Segments : register(u1);

static const uint kFlagShadowRays = 1u;
static const uint kFlagCameraRays = 2u;

// Bounce 0 to 3: cyan, amber, magenta, green.
static const float3 kBounceColors[4] =
{
    float3(0.10f, 0.75f, 1.00f),
    float3(1.00f, 0.55f, 0.08f),
    float3(1.00f, 0.15f, 0.75f),
    float3(0.30f, 1.00f, 0.25f),
};
static const float3 kShadowLitColor     = float3(1.00f, 0.85f, 0.30f) * 0.6f;
static const float3 kShadowBlockedColor = float3(1.00f, 0.10f, 0.06f) * 0.6f;
static const float3 kCameraColor        = float3(0.85f, 0.85f, 0.85f) * 0.35f;

struct VSOutput
{
    float4 Position : SV_Position;
    nointerpolation float3 Color : COLOR0;
    // Signed distance from the line's centre, in pixels.
    noperspective float Side : TEXCOORD0;
    // 0 at the segment's start, 1 at its end, in world space.
    float Along : TEXCOORD1;
    // 1 for rays drawn to an arbitrary length, which fade out instead of ending.
    nointerpolation float FadesOut : TEXCOORD2;
};

VSOutput Culled()
{
    VSOutput o = (VSOutput)0;
    o.Position = float4(2.0f, 2.0f, 2.0f, 1.0f);   // outside the clip volume
    return o;
}

// The light travelling back along a bounce segment: what its hit reflects from
// the lights, plus its albedo times whatever the next bounce brings back.
float3 PathRadiance(uint slot, RtGIDebugRaySegment segment)
{
    const uint withinRay = (slot % g_SlotsPerPixel) % g_SlotsPerRay;
    const uint rayBase = slot - withinRay;

    float3 radiance = 0.0f.xxx;
    float3 throughput = 1.0f.xxx;
    [loop]
    for (uint b = segment.bounce; b * 2u < g_SlotsPerRay; ++b)
    {
        const RtGIDebugRaySegment next = u_Segments[rayBase + b * 2u];
        if (next.kind != kRayDebugBounceHit && next.kind != kRayDebugBounceMiss)
            break;
        radiance += throughput * next.light;
        if (next.kind == kRayDebugBounceMiss)
            break;
        throughput *= next.albedo;
    }
    return radiance;
}

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    const uint slot = vertexId / 6u;
    const uint corner = vertexId % 6u;
    if (slot >= g_SegmentCount)
        return Culled();

    const RtGIDebugRaySegment segment = u_Segments[slot];
    const bool isBounce = segment.kind == kRayDebugBounceHit || segment.kind == kRayDebugBounceMiss;
    const bool isShadow = segment.kind == kRayDebugShadowLit || segment.kind == kRayDebugShadowBlocked;
    const bool isCamera = segment.kind == kRayDebugCamera;
    if (!isBounce && !isShadow && !isCamera)
        return Culled();
    if (isShadow && (g_Flags & kFlagShadowRays) == 0u)
        return Culled();
    if (isCamera && (g_Flags & kFlagCameraRays) == 0u)
        return Culled();

    float3 p0 = segment.start;
    float3 p1 = segment.end;
    float fadesOut = 0.0f;
    if (segment.kind == kRayDebugBounceMiss || isShadow)
    {
        // Stored as start + direction; drawn to a chosen length, fading out.
        const float3 direction = p1 - p0;
        const float directionLengthSq = dot(direction, direction);
        if (directionLengthSq < 1e-12f)
            return Culled();
        p1 = p0 + direction * rsqrt(directionLengthSq) * (isShadow ? g_ShadowLength : g_MissLength);
        fadesOut = 1.0f;
    }

    float3 color;
    if (isShadow)
        color = (segment.kind == kRayDebugShadowLit ? kShadowLitColor : kShadowBlockedColor) * g_Intensity;
    else if (isCamera)
        color = kCameraColor * g_Intensity;
    else if (g_ColorMode == 1u)
        color = PathRadiance(slot, segment) * g_RadianceScale;
    else
        color = kBounceColors[min(segment.bounce, 3u)] * g_Intensity;

    // Clip against a plane just in front of the camera, so a segment passing
    // behind it still projects sensibly.
    float4 c0 = mul(float4(p0, 1.0f), g_ViewProj);
    float4 c1 = mul(float4(p1, 1.0f), g_ViewProj);
    const float nearW = 1e-3f;
    if (c0.w < nearW && c1.w < nearW)
        return Culled();
    float t0 = 0.0f;
    float t1 = 1.0f;
    if (c0.w < nearW)
    {
        t0 = (nearW - c0.w) / (c1.w - c0.w);
        c0 = lerp(c0, c1, t0);
    }
    else if (c1.w < nearW)
    {
        t1 = (nearW - c0.w) / (c1.w - c0.w);
        c1 = lerp(c0, c1, t1);
    }

    const float2 halfViewport = 0.5f * g_ViewportSize;
    const float2 s0 = c0.xy / c0.w * halfViewport;
    const float2 s1 = c1.xy / c1.w * halfViewport;
    const float2 delta = s1 - s0;
    const float deltaLength = length(delta);
    const float2 along = deltaLength > 1e-4f ? delta / deltaLength : float2(1.0f, 0.0f);
    const float2 across = float2(-along.y, along.x);

    // Two triangles: (start, end, start) and (start, end, end), each corner on
    // one side of the line or the other.
    const uint  cornerEnd[6]  = { 0u, 1u, 0u, 0u, 1u, 1u };
    const float cornerSide[6] = { -1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f };
    const bool atEnd = cornerEnd[corner] != 0u;
    // One pixel of margin beyond the width, for the anti-aliased edge.
    const float extent = g_HalfWidth + 1.0f;
    const float side = cornerSide[corner] * extent;

    float4 position = atEnd ? c1 : c0;
    position.xy += across * side / halfViewport * position.w;

    VSOutput o;
    o.Position = position;
    o.Color = color;
    o.Side = side;
    o.Along = atEnd ? t1 : t0;
    o.FadesOut = fadesOut;
    return o;
}

float4 PSMain(VSOutput input) : SV_Target
{
    const float coverage = saturate(g_HalfWidth + 0.5f - abs(input.Side));
    const float fade = input.FadesOut > 0.5f ? (1.0f - input.Along) * (1.0f - input.Along) : 1.0f;
    const float weight = coverage * fade;
    if (weight <= 0.0f)
        discard;
    return float4(input.Color * weight, 0.0f);
}
