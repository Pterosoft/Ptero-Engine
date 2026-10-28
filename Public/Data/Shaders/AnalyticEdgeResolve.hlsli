// AnalyticEdgeResolve.hlsli
// The consumer side of the G-Buffer's triangle edges (AnalyticEdge.hlsli), shared by
// Analytic Edge AA (AnalyticEdgeAA.hlsl) and Analytic-Aware Reconstruction
// (AnalyticAwareReconstruction.hlsl): unpacking a pixel's triangle, deciding which of
// its edges bound a real geometric break, and clipping rectangles against triangles for
// their exact covered area.
//
// Coordinates are in source (render-resolution) pixels. A stored triangle belongs to the
// pixel that wrote it, its "owner", and is measured from that pixel's centre; Shift()
// moves it into another frame.

#ifndef PTERO_ANALYTIC_EDGE_RESOLVE_HLSLI
#define PTERO_ANALYTIC_EDGE_RESOLVE_HLSLI

#include "AnalyticEdge.hlsli"

// +x, -x, +y, -y. The opposite of neighbour k is k ^ 1.
static const int2 kNeighbourOffsets[4] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1) };

// A rectangle clipped by up to three triangles: 4 + one vertex per clipping edge.
#define PTERO_EDGE_MAX_POLYGON 13

struct HalfPlane
{
    float2 Normal;     // inward, unit length
    float  Distance;   // inside where Distance + dot(Normal, x) >= 0
    bool   Valid;
};

// The part of a pixel's triangle near it: up to three edges. An invalid edge does not
// bound anything - it was too far away to store, or it was gated out.
struct TriangleEdges
{
    HalfPlane E[3];
};

HalfPlane NoEdge()
{
    HalfPlane edge;
    edge.Normal = float2(0.0f, 0.0f);
    edge.Distance = 0.0f;
    edge.Valid = false;
    return edge;
}

HalfPlane UnpackEdge(uint packed)
{
    HalfPlane edge;
    const uint distanceCode = packed & 0xFFu;
    const float angle = ((float)((packed >> 8) & 0xFFu) / 255.0f - 0.5f) * (2.0f * kPteroEdgePi);
    edge.Normal = float2(cos(angle), sin(angle));
    edge.Distance = (float)distanceCode / 255.0f * kPteroEdgeMaxDistance;
    edge.Valid = distanceCode != kPteroNoEdgeCode;
    return edge;
}

TriangleEdges UnpackTriangle(uint4 packed)
{
    TriangleEdges t;
    t.E[0] = UnpackEdge(packed.x);
    t.E[1] = UnpackEdge(packed.y);
    t.E[2] = UnpackEdge(packed.z);
    return t;
}

TriangleEdges NoTriangle()
{
    TriangleEdges t;
    t.E[0] = NoEdge();
    t.E[1] = NoEdge();
    t.E[2] = NoEdge();
    return t;
}

TriangleEdges SingleEdgeTriangle(HalfPlane edge)
{
    TriangleEdges t = NoTriangle();
    t.E[0] = edge;
    return t;
}

bool HasAnyEdge(TriangleEdges t)
{
    return t.E[0].Valid || t.E[1].Valid || t.E[2].Valid;
}

// Signed distance of x from the edge; >= 0 is inside.
float EdgeSide(HalfPlane edge, float2 x)
{
    return edge.Distance + dot(edge.Normal, x);
}

bool InsideTriangle(TriangleEdges t, float2 x)
{
    bool inside = true;
    [unroll]
    for (uint i = 0; i < 3; ++i)
        inside = inside && (!t.E[i].Valid || EdgeSide(t.E[i], x) >= 0.0f);
    return inside;
}

// Re-expresses a triangle given in a frame centred at 'from' in one centred at 'to'.
TriangleEdges Shift(TriangleEdges t, float2 from, float2 to)
{
    const float2 delta = to - from;
    [unroll]
    for (uint i = 0; i < 3; ++i)
        t.E[i].Distance += dot(t.E[i].Normal, delta);
    return t;
}

bool SameLine(HalfPlane a, HalfPlane b)
{
    return a.Valid && b.Valid && dot(a.Normal, b.Normal) > 0.995f && abs(a.Distance - b.Distance) < 0.1f;
}

// Two owners on the same triangle store the same lines (in a common frame).
bool SameTriangle(TriangleEdges a, TriangleEdges b)
{
    bool same = true;
    uint countA = 0;
    uint countB = 0;
    [unroll]
    for (uint i = 0; i < 3; ++i)
    {
        countA += a.E[i].Valid ? 1u : 0u;
        countB += b.E[i].Valid ? 1u : 0u;
        if (a.E[i].Valid)
            same = same && (SameLine(a.E[i], b.E[0]) || SameLine(a.E[i], b.E[1]) || SameLine(a.E[i], b.E[2]));
    }
    return same && countA == countB;
}

// Must match EncodeOctNormal in GBuffer.hlsl (and DecodeOctNormal in DeferredLighting.hlsl).
float3 DecodeOctNormal(float2 encoded)
{
    float2 oct = encoded * 2.0f - 1.0f;
    float3 n = float3(oct, 1.0f - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0f)
        n.xy = (1.0f - abs(n.yx)) * (float2(n.xy >= 0.0f) * 2.0f - 1.0f);
    return normalize(n);
}

bool IsSky(float deviceDepth)
{
    // The normal target's depth clears to 1.0 where nothing was drawn.
    return deviceDepth >= 1.0f;
}

// Device depth is a + b / viewZ, so |z - _33| is proportional to 1/viewZ. Depth steps
// divided by it are relative changes in 1/viewZ: scale-free, the same near and far.
float RelativeDepthStep(float step, float referenceDepth, float projectionZ33)
{
    return abs(step) / max(abs(referenceDepth - projectionZ33), 1e-12f);
}

// Does the surface stop between pixel P and a sample N, or carry on? Device depth is
// affine in screen space across any plane, so a plane runs on through P exactly when
// zN - zP == zP - zOpposite (zOpposite mirrored through P); what that straight line
// fails to predict is the step.
bool IsGeometricBreak(
    float zP, float zN, float zOpposite, float3 normalP, float3 normalN,
    float projectionZ33, float depthThreshold, float normalThreshold)
{
    if (IsSky(zP) || IsSky(zN))
        return IsSky(zP) != IsSky(zN);

    const float predicted = IsSky(zOpposite) ? zP : 2.0f * zP - zOpposite;
    return RelativeDepthStep(zN - predicted, zP, projectionZ33) > depthThreshold
        || dot(normalP, normalN) < normalThreshold;
}

uint NeighbourToward(float2 direction)
{
    if (abs(direction.x) >= abs(direction.y))
        return direction.x >= 0.0f ? 0u : 1u;
    return direction.y >= 0.0f ? 2u : 3u;
}

// Does this edge of pixel P's triangle bound something visible - a break with a surface
// that is farther (or a crease) on its other side? It is judged against the
// neighbouring render sample deepest beyond it, not simply the axis neighbour its normal
// points at: next to a sharp vertex the axis neighbour can still be inside the same
// triangle, which used to drop real silhouettes. An edge more than a pixel away is
// judged two pixels out. Interior mesh edges, with the same surface beyond, are dropped.
bool GateEdge(
    Texture2D<float4> normalDepth, int2 pixel, int2 size, HalfPlane edge,
    float zP, float3 normalP, float projectionZ33, float depthThreshold, float normalThreshold)
{
    if (!edge.Valid)
        return false;

    int2 offset = int2(0, 0);
    float deepest = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float side = EdgeSide(edge, float2(x, y));
            if ((x != 0 || y != 0) && side < deepest)
            {
                deepest = side;
                offset = int2(x, y);
            }
        }
    }
    if (deepest >= 0.0f)
        offset = 2 * kNeighbourOffsets[NeighbourToward(-edge.Normal)];

    const int2 beyond = clamp(pixel + offset, int2(0, 0), size - 1);
    const int2 mirror = clamp(pixel - offset, int2(0, 0), size - 1);
    const float4 beyondNormalDepth = normalDepth.Load(int3(beyond, 0));
    const float zN = beyondNormalDepth.z;
    const float zOpposite = normalDepth.Load(int3(mirror, 0)).z;

    // At the screen border the clamped sample is the pixel itself: no evidence of a break.
    const bool isBreak = any(beyond != pixel)
        && IsGeometricBreak(zP, zN, zOpposite, normalP, DecodeOctNormal(beyondNormalDepth.xy),
                            projectionZ33, depthThreshold, normalThreshold);
    return isBreak && !(zN < zP);
}

// One Sutherland-Hodgman step.
void ClipPolygon(inout float2 polygon[PTERO_EDGE_MAX_POLYGON], inout uint count, HalfPlane plane)
{
    float2 clipped[PTERO_EDGE_MAX_POLYGON];
    uint clippedCount = 0;

    [loop]
    for (uint i = 0; i < count; ++i)
    {
        const float2 a = polygon[i];
        const float2 b = polygon[(i + 1 == count) ? 0 : i + 1];
        const float sideA = EdgeSide(plane, a);
        const float sideB = EdgeSide(plane, b);

        if (sideA >= 0.0f && clippedCount < PTERO_EDGE_MAX_POLYGON)
            clipped[clippedCount++] = a;
        if ((sideA >= 0.0f) != (sideB >= 0.0f) && clippedCount < PTERO_EDGE_MAX_POLYGON)
            clipped[clippedCount++] = lerp(a, b, sideA / (sideA - sideB));
    }

    polygon = clipped;
    count = clippedCount;
}

void ClipByTriangle(inout float2 polygon[PTERO_EDGE_MAX_POLYGON], inout uint count, TriangleEdges t)
{
    [unroll]
    for (uint i = 0; i < 3; ++i)
        if (t.E[i].Valid && count > 0)
            ClipPolygon(polygon, count, t.E[i]);
}

void InitRectangle(out float2 polygon[PTERO_EDGE_MAX_POLYGON], out uint count, float2 centre, float2 halfExtent)
{
    polygon = (float2[PTERO_EDGE_MAX_POLYGON])0;
    polygon[0] = centre + float2(-halfExtent.x, -halfExtent.y);
    polygon[1] = centre + float2( halfExtent.x, -halfExtent.y);
    polygon[2] = centre + float2( halfExtent.x,  halfExtent.y);
    polygon[3] = centre + float2(-halfExtent.x,  halfExtent.y);
    count = 4;
}

// Shoelace formula, as a share of the rectangle's area.
float PolygonFraction(float2 polygon[PTERO_EDGE_MAX_POLYGON], uint count, float2 halfExtent)
{
    float doubledArea = 0.0f;
    [loop]
    for (uint i = 0; i < count; ++i)
    {
        const float2 a = polygon[i];
        const float2 b = polygon[(i + 1 == count) ? 0 : i + 1];
        doubledArea += a.x * b.y - a.y * b.x;
    }
    return saturate(0.5f * abs(doubledArea) / max(4.0f * halfExtent.x * halfExtent.y, 1e-12f));
}

// Exact share of the rectangle centre +- halfExtent (in the triangle's frame) inside it.
float CoveredFraction(float2 centre, float2 halfExtent, TriangleEdges t)
{
    float2 polygon[PTERO_EDGE_MAX_POLYGON];
    uint count;
    InitRectangle(polygon, count, centre, halfExtent);
    ClipByTriangle(polygon, count, t);
    return PolygonFraction(polygon, count, halfExtent);
}

// Exact share of the rectangle (centred at the frame origin) inside every triangle
// whose bit is set in 'mask'.
float IntersectionFraction(float2 halfExtent, TriangleEdges a, TriangleEdges b, TriangleEdges c, uint mask)
{
    float2 polygon[PTERO_EDGE_MAX_POLYGON];
    uint count;
    InitRectangle(polygon, count, float2(0.0f, 0.0f), halfExtent);
    if (mask & 1u) ClipByTriangle(polygon, count, a);
    if (mask & 2u) ClipByTriangle(polygon, count, b);
    if (mask & 4u) ClipByTriangle(polygon, count, c);
    return PolygonFraction(polygon, count, halfExtent);
}

// Exact share of the rectangle inside the union of 'triangleCount' (1..3) triangles,
// by inclusion-exclusion.
float UnionFraction(float2 halfExtent, TriangleEdges a, TriangleEdges b, TriangleEdges c, uint triangleCount)
{
    float total = 0.0f;
    const uint subsets = (1u << triangleCount) - 1u;
    [loop]
    for (uint mask = 1u; mask <= subsets; ++mask)
    {
        const float area = IntersectionFraction(halfExtent, a, b, c, mask);
        total += (countbits(mask) & 1u) ? area : -area;
    }
    return saturate(total);
}

float Luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

#endif // PTERO_ANALYTIC_EDGE_RESOLVE_HLSLI
