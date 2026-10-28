// RtSurfaceSnap.hlsli
// Starting point for rays leaving a G-Buffer pixel, on the surface the TLAS holds.
//
// The traced scene is not the rasterised one: virtualized meshes are traced against a
// coarser cut through their cluster DAG that changes with camera distance, terrain is
// displaced in the raster pass only, and depth carries jitter and precision error. A
// ray started from the G-Buffer position can therefore start inside the traced mesh,
// and every ray it fires is blocked - black AO and black shadows that come and go as
// the camera moves and the cut changes. RTGI solves this (ComputePrimaryRayOrigin in
// RtGI_RayGen.hlsl) by tracing the camera ray and starting from where it hits; this is
// the same idea without the vertex fetch, for passes that only need a position.

#ifndef PTERO_RT_SURFACE_SNAP_HLSLI
#define PTERO_RT_SURFACE_SNAP_HLSLI

// worldPos moved onto the traced surface the camera sees through it (if one lies within
// the depth tolerance), lifted a hair back toward the camera so it is on the visible
// side. Returns worldPos unchanged when nothing traced is there (alpha-tested or
// otherwise raster-only geometry).
float3 PteroSnapToTracedSurface(RaytracingAccelerationStructure tlas, float3 cameraPos, float3 worldPos)
{
    const float3 toSurface = worldPos - cameraPos;
    const float  surfaceDistance = length(toSurface);
    if (surfaceDistance < 1e-4f)
        return worldPos;

    // Depth precision, TAA jitter and the coarser traced cut all grow with distance.
    const float tolerance = max(0.03f, surfaceDistance * 0.015f);

    RayDesc ray;
    ray.Origin    = cameraPos;
    ray.Direction = toSurface / surfaceDistance;
    ray.TMin      = max(surfaceDistance - tolerance, 0.0f);
    ray.TMax      = surfaceDistance + tolerance;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
    rq.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, ray);
    while (rq.Proceed()) {}
    if (rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return worldPos;

    const float lift = max(0.002f, surfaceDistance * 0.0005f);
    return cameraPos + ray.Direction * max(rq.CommittedRayT() - lift, 0.0f);
}

#endif // PTERO_RT_SURFACE_SNAP_HLSLI
