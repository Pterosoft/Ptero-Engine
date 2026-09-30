// VirtualGeometry_Common.hlsli
// Records and tests shared by the virtualized-geometry cull passes
// (VirtualGeometry_Cull.hlsl) and its raster passes (VirtualGeometry_Raster.hlsl).
// Every struct here mirrors one in VirtualGeometryRenderer.h / VirtualGeometryBuilder.h
// byte for byte; change them together.

#ifndef VIRTUAL_GEOMETRY_COMMON_HLSLI
#define VIRTUAL_GEOMETRY_COMMON_HLSLI

// One cluster of at most 64 vertices / 124 triangles. Spheres are in mesh space.
struct VgCluster
{
    float4 CullSphere;      // tight bounds of the cluster's own triangles
    float4 LodSphere;       // bounds its own error was measured over
    float4 ParentSphere;    // bounds of the coarser group that replaces it
    float3 ConeApex;
    float  ConeCutoff;      // > 1 disables the backface test
    float3 ConeAxis;
    float  LodError;        // 0 for the original triangles
    uint   VertexOffset;    // into the cluster-vertex pool
    uint   TriangleOffset;  // into the cluster-triangle pool
    uint   Packed;          // vertexCount | triangleCount << 7 | dagDepth << 14 | materialSlot << 20
    float  ParentError;     // FLT_MAX when nothing coarser replaces this cluster
};

uint VgClusterVertexCount(VgCluster cluster)   { return cluster.Packed & 0x7Fu; }
uint VgClusterTriangleCount(VgCluster cluster) { return (cluster.Packed >> 7) & 0x7Fu; }
uint VgClusterDepth(VgCluster cluster)         { return (cluster.Packed >> 14) & 0x3Fu; }
uint VgClusterMaterialSlot(VgCluster cluster)  { return cluster.Packed >> 20; }

// One mesh asset resident in the geometry pools.
struct VgAsset
{
    float4 BoundsSphere;    // encloses every sphere of every cluster, mesh space
    uint   ClusterOffset;   // first cluster in the cluster pool
    uint   ClusterCount;
    uint2  _Pad;
};

// Instance flags.
static const uint VG_INSTANCE_UNIFORM_SCALE = 1u;   // backface cones survive the transform

// One placed entity. World rows are the transposed model matrix, so a point
// transforms as float3(dot(World0, p), dot(World1, p), dot(World2, p)) - the
// same result as mul(float4(p, 1), gModel) in GBuffer.hlsl.
struct VgInstance
{
    float4 World0;
    float4 World1;
    float4 World2;
    float4 PrevWorld0;
    float4 PrevWorld1;
    float4 PrevWorld2;
    uint   AssetIndex;
    uint   BinTableOffset;  // this instance's material slot -> bin table
    float  MaxScale;        // largest axis scale, for sphere radii and errors
    uint   Flags;
};

// View flags.
static const uint VG_VIEW_OCCLUSION = 1u;   // the main camera: HZB tests apply
static const uint VG_VIEW_CONE      = 2u;   // backface cone culling applies
static const uint VG_VIEW_SPHERE    = 4u;   // cull against LightSphere instead of Planes
static const uint VG_VIEW_TEXEL_LOD = 8u;   // cut the DAG at LodTexelSize, not the camera's pixel error
static const uint VG_VIEW_POINT_LOD = 16u;  // cut the DAG at LodTexelSize radians as seen from LightSphere.xyz

// Something clusters are culled for: the camera, the sun's shadow map, one
// shadow-casting point light (whose six faces share the one list), or one page
// of the sun's virtual shadow map.
struct VgView
{
    float4 Planes[6];       // inward-facing, from the view's row-major view-projection
    float4 LightSphere;     // VG_VIEW_SPHERE views: centre and radius
    uint   Bin;             // bin every cluster goes to, or VG_MATERIAL_BINNED
    uint   Flags;
    float  LodTexelSize;    // VG_VIEW_TEXEL_LOD: world size of one shadow texel; VG_VIEW_POINT_LOD: its angle
    uint   _Pad;
};

static const uint VG_MATERIAL_BINNED = 0xFFFFFFFFu;
static const uint VG_INVALID_BIN     = 0xFFFFFFFFu;

// Entries in the chunk / candidate / visible lists pack the instance index in
// the low 24 bits and the view in the high 8.
uint VgPackInstanceView(uint instanceIndex, uint viewIndex) { return instanceIndex | (viewIndex << 24); }
uint VgUnpackInstance(uint packed) { return packed & 0x00FFFFFFu; }
uint VgUnpackView(uint packed)     { return packed >> 24; }

// Group count limit per dispatch dimension is 65535; the passes that dispatch
// one group per item fold the count into two dimensions of this width.
static const uint VG_DISPATCH_WIDTH = 32768u;

uint VgLinearGroup(uint3 groupId) { return groupId.x + groupId.y * VG_DISPATCH_WIDTH; }

float3 VgTransformPoint(VgInstance instance, float3 p)
{
    const float4 p1 = float4(p, 1.0f);
    return float3(dot(instance.World0, p1), dot(instance.World1, p1), dot(instance.World2, p1));
}

float3 VgTransformPointPrevious(VgInstance instance, float3 p)
{
    const float4 p1 = float4(p, 1.0f);
    return float3(dot(instance.PrevWorld0, p1), dot(instance.PrevWorld1, p1), dot(instance.PrevWorld2, p1));
}

float3 VgTransformVector(VgInstance instance, float3 v)
{
    return float3(dot(instance.World0.xyz, v), dot(instance.World1.xyz, v), dot(instance.World2.xyz, v));
}

// Projected simplification error in pixels, the metric the DAG was built for:
// error / distance * lodFactor, where lodFactor already folds in the
// viewport's pixels-per-radian and the error threshold. The distance is to the
// nearest point of the bounds, clamped to the near plane, which is what keeps
// the metric monotonic up the DAG (a parent's sphere contains its children's).
float VgProjectedError(float3 centreWorld, float radiusWorld, float errorWorld,
                       float3 cameraPosition, float lodFactor, float zNear)
{
    const float distance = max(length(centreWorld - cameraPosition) - radiusWorld, zNear);
    return errorWorld * lodFactor / distance;
}

// The DAG cut: draw a cluster when it is fine enough and the group that would
// replace it is not. Both halves compare against 1 because lodFactor divides
// by the pixel threshold.
bool VgClusterLodSelected(VgCluster cluster, VgInstance instance,
                          float3 cameraPosition, float lodFactor, float zNear)
{
    const float scale = instance.MaxScale;

    // FLT_MAX * scale overflows to +inf, which correctly reads as "too coarse".
    const float parentProjected = VgProjectedError(
        VgTransformPoint(instance, cluster.ParentSphere.xyz), cluster.ParentSphere.w * scale,
        cluster.ParentError * scale, cameraPosition, lodFactor, zNear);
    if (parentProjected <= 1.0f)
        return false;

    const float selfProjected = VgProjectedError(
        VgTransformPoint(instance, cluster.LodSphere.xyz), cluster.LodSphere.w * scale,
        cluster.LodError * scale, cameraPosition, lodFactor, zNear);
    return selfProjected <= 1.0f;
}

// The DAG cut for an orthographic shadow page: the coarsest clusters whose
// simplification error is still under one texel of the page. Independent of the
// camera, so a cached page holds the same geometry wherever the camera went since.
bool VgClusterLodSelectedTexel(VgCluster cluster, VgInstance instance, float texelSize)
{
    const float scale = instance.MaxScale;
    if (cluster.ParentError * scale <= texelSize)
        return false;
    return cluster.LodError * scale <= texelSize;
}

bool VgSphereInView(VgView view, float3 centre, float radius)
{
    if ((view.Flags & VG_VIEW_SPHERE) != 0)
    {
        const float3 offset = centre - view.LightSphere.xyz;
        const float reach = radius + view.LightSphere.w;
        return dot(offset, offset) <= reach * reach;
    }

    [unroll]
    for (uint i = 0; i < 6; ++i)
    {
        if (dot(view.Planes[i].xyz, centre) + view.Planes[i].w < -radius)
            return false;
    }
    return true;
}

// meshoptimizer's cone test: every triangle of the cluster faces away from a
// camera at cameraPosition.
bool VgClusterBackfacing(VgCluster cluster, VgInstance instance, float3 cameraPosition)
{
    if (cluster.ConeCutoff > 1.0f || (instance.Flags & VG_INSTANCE_UNIFORM_SCALE) == 0)
        return false;

    const float3 apex = VgTransformPoint(instance, cluster.ConeApex);
    const float3 axis = normalize(VgTransformVector(instance, cluster.ConeAxis));
    return dot(normalize(apex - cameraPosition), axis) >= cluster.ConeCutoff;
}

// Hierarchical-Z occlusion test of a world-space sphere.
//
// The sphere's bounding box is projected with the view-projection the HZB was
// rendered with, and its nearest depth compared against the farthest depth
// the HZB records over the box's screen rectangle. The HZB's level 0 is the
// largest power of two that fits the depth buffer, and every texel of it holds
// the maximum of the whole depth footprint it covers, so reading the 2x2
// texels that span the rectangle at the right mip is conservative: it can
// only ever call an occluded cluster visible, never the reverse.
bool VgSphereOccluded(Texture2D<float> hzb, float2 hzbSize, uint hzbMipCount,
                      float4x4 viewProjection, float3 centre, float radius)
{
    float2 minUv = float2(1.0f, 1.0f);
    float2 maxUv = float2(0.0f, 0.0f);
    float  nearestDepth = 1.0f;

    [unroll]
    for (uint corner = 0; corner < 8; ++corner)
    {
        const float3 offset = float3(
            (corner & 1u) ? radius : -radius,
            (corner & 2u) ? radius : -radius,
            (corner & 4u) ? radius : -radius);
        const float4 clip = mul(float4(centre + offset, 1.0f), viewProjection);

        // A corner at or behind the camera plane means the box straddles it;
        // nothing sensible can be said about its rectangle, so keep it.
        if (clip.w <= 1e-4f)
            return false;

        const float3 ndc = clip.xyz / clip.w;
        const float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
        minUv = min(minUv, uv);
        maxUv = max(maxUv, uv);
        nearestDepth = min(nearestDepth, ndc.z);
    }

    // Entirely off screen is the frustum test's business; partly off screen
    // is tested over the part that is on it.
    minUv = saturate(minUv);
    maxUv = saturate(maxUv);
    if (nearestDepth <= 0.0f)
        return false;

    const float2 texelMin = minUv * hzbSize;
    const float2 texelMax = maxUv * hzbSize;
    const float2 extent = max(texelMax - texelMin, 1.0f);

    // The mip at which the rectangle spans at most two texels on each axis,
    // so the 2x2 read below covers all of it.
    const float mip = clamp(ceil(log2(max(extent.x, extent.y))), 0.0f, float(hzbMipCount - 1));
    const uint  level = (uint)mip;
    const float scale = exp2(-mip);

    const int2 mipSize = max(int2(hzbSize * scale), int2(1, 1));
    const int2 lo = clamp(int2(floor(texelMin * scale)), int2(0, 0), mipSize - 1);
    const int2 hi = clamp(int2(floor(texelMax * scale)), int2(0, 0), mipSize - 1);

    float farthest = hzb.Load(int3(lo.x, lo.y, level));
    farthest = max(farthest, hzb.Load(int3(hi.x, lo.y, level)));
    farthest = max(farthest, hzb.Load(int3(lo.x, hi.y, level)));
    farthest = max(farthest, hzb.Load(int3(hi.x, hi.y, level)));

    return nearestDepth > farthest;
}

#endif
