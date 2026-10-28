// AnalyticAwareReconstruction.hlsl
// AAR: a spatial upscaler that reconstructs geometric edges from the triangles' own
// edge equations instead of interpolating colours across them.
//
// A generic upscaler (Catmull-Rom, Lanczos, FSR 1) only sees colours, so an edge that
// the render resolution blurred into one in-between pixel stays blurred - it can only be
// re-sharpened, with halos. AAR keeps what Analytic Edge AA knows instead of resolving
// it into RGB first: the G-Buffer stores the screen-space lines of the triangle that won
// each render pixel (AnalyticEdge.hlsli), and those lines are as exact at the output
// resolution as at the render resolution. So for an output pixel crossed by an edge:
//
//     coverage A = Area(output pixel n triangle A) / Area(output pixel)      (exact)
//     colour     = coverage A * colour A + (1 - coverage A) * colour B
//
// where colours A and B are each reconstructed only from render samples on their own
// side of the edge and on their own surface, so the 70 % plaster / 30 % railing pixel
// does not become "some grey" before it is upscaled.
//
// Two passes:
//
//   AarGatePS          render resolution. Keeps only the stored edges that bound a
//                      real geometric break with a farther surface (or a crease) beyond
//                      them - the same classification Analytic Edge AA uses - and drops
//                      interior mesh edges, which have the same surface on both sides.
//
//   AarReconstructPS   output resolution. Gathers the triangles stored by the 3 x 3
//                      render pixels around the output pixel's footprint. The nearest
//                      surface among those that reach the footprint is surface A; its
//                      distinct triangles (up to three - a thin quad's two halves, a fan
//                      around a vertex) are merged, and the footprint's exact coverage by
//                      their union is found by inclusion-exclusion. A union that only
//                      partly covers the footprint makes an edge pixel; one that covers
//                      it, or none at all, an interior pixel: a Catmull-Rom over 4 x 4
//                      render pixels, weighted by depth (against the target surface's
//                      plane) and normal, so it never borrows colour across surfaces.
//
// Merging triangles is what keeps thin geometry: one render pixel only knows its own
// triangle, so a 1-pixel railing's left and right sides are stored by different pixels,
// and neither alone bounds the rail.
//
// Scene colour is the raw, point-sampled lit scene: Analytic Edge AA does not run while
// AAR does, since AAR evaluates the same edges at the output pixels instead.

#include "AnalyticEdgeResolve.hlsli"

cbuffer AarConstants : register(b0)
{
    uint2 gRenderSize;
    uint2 gOutputSize;
    // Edge classification, as in Analytic Edge AA.
    float gDepthThreshold;
    float gNormalThreshold;
    // The projection's _33: device depth minus this is proportional to 1/viewZ.
    float gProjectionZ33;
    // Relative 1/viewZ deviation from a surface's plane at which a tap's weight falls
    // to e^-0.5.
    float gDepthSigma;
    // Exponent on dot(normal, target normal) in the tap weights.
    float gNormalPower;
    // 0 = result, 1 = reconstruction path, 2 = output coverage.
    uint  gDebugView;
    float2 gPad;
};

Texture2D<float4> gSceneColor  : register(t0);   // render-resolution lit scene
Texture2D<float4> gNormalDepth : register(t1);   // G-Buffer normal target: oct normal xy, device depth z
Texture2D<uint4>  gEdges       : register(t2);   // gate: G-Buffer edges; reconstruct: gated edges

struct VSOutput
{
    float4 Position : SV_Position;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    // Fullscreen triangle.
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    VSOutput output;
    output.Position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

int3 RenderAddress(int2 pixel)
{
    return int3(clamp(pixel, int2(0, 0), int2(gRenderSize) - 1), 0);
}

bool InRender(int2 pixel)
{
    return all(pixel >= 0) && all(pixel < int2(gRenderSize));
}

// ---------------------------------------------------------------------------------
// Pass 1: gate the stored edges (render resolution)
// ---------------------------------------------------------------------------------

uint4 AarGatePS(VSOutput input) : SV_Target
{
    const int2 pixel = int2(input.Position.xy);

    const float4 centreNormalDepth = gNormalDepth.Load(int3(pixel, 0));
    const float  zP = centreNormalDepth.z;
    const float3 normalP = DecodeOctNormal(centreNormalDepth.xy);

    // An edge bounds something visible only with a break on its far side, and belongs
    // to this pixel's surface only when what lies beyond is not nearer.
    const uint4 packed = gEdges.Load(int3(pixel, 0));
    const TriangleEdges stored = UnpackTriangle(packed);
    uint4 gated = kPteroNoEdge;
    [unroll]
    for (uint i = 0; i < 3; ++i)
    {
        if (GateEdge(gNormalDepth, pixel, int2(gRenderSize), stored.E[i], zP, normalP,
                     gProjectionZ33, gDepthThreshold, gNormalThreshold))
            gated[i] = packed[i];
    }
    return gated;
}
// ---------------------------------------------------------------------------------
// Pass 2: reconstruct (output resolution)
// ---------------------------------------------------------------------------------

struct SurfaceSample
{
    float3 Color;
    float  Depth;
    float3 Normal;
};

SurfaceSample LoadSurface(int2 pixel)
{
    const float4 normalDepth = gNormalDepth.Load(RenderAddress(pixel));
    SurfaceSample s;
    s.Color = gSceneColor.Load(RenderAddress(pixel)).rgb;
    s.Depth = normalDepth.z;
    s.Normal = DecodeOctNormal(normalDepth.xy);
    return s;
}

// The surface's device-depth slope at a pixel, per render pixel. Device depth is affine
// across a plane, so this predicts the plane anywhere nearby. Each axis takes the
// smaller of its one-sided differences, so a silhouette next to the pixel does not tilt
// the plane.
float2 DepthGradient(int2 pixel, float z)
{
    const float zr = gNormalDepth.Load(RenderAddress(pixel + int2(1, 0))).z;
    const float zl = gNormalDepth.Load(RenderAddress(pixel - int2(1, 0))).z;
    const float zd = gNormalDepth.Load(RenderAddress(pixel + int2(0, 1))).z;
    const float zu = gNormalDepth.Load(RenderAddress(pixel - int2(0, 1))).z;

    const float forwardX = IsSky(zr) ? 1e9f : zr - z;
    const float backX    = IsSky(zl) ? 1e9f : z - zl;
    const float forwardY = IsSky(zd) ? 1e9f : zd - z;
    const float backY    = IsSky(zu) ? 1e9f : z - zu;

    float2 gradient;
    gradient.x = abs(forwardX) < abs(backX) ? forwardX : backX;
    gradient.y = abs(forwardY) < abs(backY) ? forwardY : backY;
    gradient.x = abs(gradient.x) < 1e8f ? gradient.x : 0.0f;
    gradient.y = abs(gradient.y) < 1e8f ? gradient.y : 0.0f;
    return gradient;
}

// How much a render sample belongs to the reference surface: its depth against the
// reference's plane and its normal against the reference's normal.
float SurfaceWeight(SurfaceSample s, float2 sCentre, SurfaceSample reference, float2 referenceCentre, float2 gradient)
{
    if (IsSky(reference.Depth) || IsSky(s.Depth))
        return (IsSky(reference.Depth) && IsSky(s.Depth)) ? 1.0f : 0.0f;

    const float predicted = reference.Depth + dot(gradient, sCentre - referenceCentre);
    const float deviation = RelativeDepthStep(s.Depth - predicted, reference.Depth, gProjectionZ33)
                          / max(gDepthSigma, 1e-6f);
    const float depthWeight = exp(-0.5f * deviation * deviation);
    const float normalWeight = pow(saturate(dot(s.Normal, reference.Normal)), gNormalPower);
    return depthWeight * normalWeight;
}

// Catmull-Rom weights for the taps at -1, 0, +1, +2 around a fractional position t.
float4 CatmullRomWeights(float t)
{
    return float4(
        ((-0.5f * t + 1.0f) * t - 0.5f) * t,
        ((1.5f * t - 2.5f) * t) * t + 1.0f,
        ((-1.5f * t + 2.0f) * t + 0.5f) * t,
        ((0.5f * t - 0.5f) * t) * t);
}

// Interior: geometry-aware Catmull-Rom over 4 x 4 render pixels. Taps from another
// surface are dropped and the rest renormalised, then the result is clamped to the
// accepted taps nearest the position, which stops the kernel's negative lobes from
// ringing once some of its taps are missing.
float3 ReconstructInterior(float2 srcPos, int2 targetPixel, out bool fellBack)
{
    const SurfaceSample target = LoadSurface(targetPixel);
    const float2 targetCentre = float2(targetPixel) + 0.5f;
    const float2 gradient = DepthGradient(targetPixel, target.Depth);

    const float2 samplePos = srcPos - 0.5f;
    const int2   base = int2(floor(samplePos));
    const float2 t = samplePos - float2(base);
    const float4 wx = CatmullRomWeights(t.x);
    const float4 wy = CatmullRomWeights(t.y);

    float3 sum = 0.0f;
    float  weightSum = 0.0f;
    float3 innerMin = 1e30f;
    float3 innerMax = -1e30f;
    float3 anyMin = 1e30f;
    float3 anyMax = -1e30f;

    [unroll]
    for (int y = 0; y < 4; ++y)
    {
        [unroll]
        for (int x = 0; x < 4; ++x)
        {
            const int2 tap = base + int2(x - 1, y - 1);
            if (!InRender(tap))
                continue;
            const SurfaceSample s = LoadSurface(tap);
            const float surface = SurfaceWeight(s, float2(tap) + 0.5f, target, targetCentre, gradient);
            if (surface <= 1e-3f)
                continue;

            const float w = wx[x] * wy[y] * surface;
            sum += s.Color * w;
            weightSum += w;

            anyMin = min(anyMin, s.Color);
            anyMax = max(anyMax, s.Color);
            if (x >= 1 && x <= 2 && y >= 1 && y <= 2)
            {
                innerMin = min(innerMin, s.Color);
                innerMax = max(innerMax, s.Color);
            }
        }
    }

    fellBack = weightSum < 0.2f;
    if (fellBack)
        return target.Color;

    const bool haveInner = all(innerMin <= innerMax);
    const float3 lo = haveInner ? innerMin : anyMin;
    const float3 hi = haveInner ? innerMax : anyMax;
    return clamp(sum / weightSum, lo, hi);
}

// The front surface's triangles near an output pixel, in the frame centred on its
// footprint.
struct SurfaceUnion
{
    TriangleEdges T[3];
    uint          Count;
};

bool InsideUnion(SurfaceUnion u, float2 x)
{
    bool inside = false;
    [unroll]
    for (uint i = 0; i < 3; ++i)
        inside = inside || (i < u.Count && InsideTriangle(u.T[i], x));
    return inside;
}

// One side of the edge: a smooth, positive-weight reconstruction from the render
// samples on that side of the union and on the reference surface. Positive weights,
// because a two-sided kernel cut in half by the edge has nothing to balance its
// negative lobes.
float3 ReconstructSide(float2 srcPos, int2 referencePixel, SurfaceUnion u, bool insideUnion)
{
    const SurfaceSample reference = LoadSurface(referencePixel);
    const float2 referenceCentre = float2(referencePixel) + 0.5f;
    const float2 gradient = DepthGradient(referencePixel, reference.Depth);

    const int2 base = int2(floor(srcPos - 0.5f));
    float3 sum = 0.0f;
    float  weightSum = 0.0f;

    [loop]
    for (int y = -1; y <= 2; ++y)
    {
        [loop]
        for (int x = -1; x <= 2; ++x)
        {
            const int2 tap = base + int2(x, y);
            if (!InRender(tap))
                continue;
            const float2 tapCentre = float2(tap) + 0.5f;
            if (InsideUnion(u, tapCentre - srcPos) != insideUnion)
                continue;

            const SurfaceSample s = LoadSurface(tap);
            const float2 d = tapCentre - srcPos;
            // Gaussian, sigma 0.6 render pixels.
            const float w = exp(-dot(d, d) * 1.3889f)
                          * SurfaceWeight(s, tapCentre, reference, referenceCentre, gradient);
            sum += s.Color * w;
            weightSum += w;
        }
    }

    return weightSum > 1e-4f ? sum / weightSum : reference.Color;
}
float4 AarReconstructPS(VSOutput input) : SV_Target
{
    // Render pixel i spans [i, i + 1]; srcPos is the output pixel centre in those units.
    const float2 renderPerOutput = float2(gRenderSize) / float2(gOutputSize);
    const float2 srcPos = input.Position.xy * renderPerOutput;
    const float2 halfFootprint = 0.5f * renderPerOutput;
    const int2   nearest = clamp(int2(floor(srcPos)), int2(0, 0), int2(gRenderSize) - 1);
    const float  nearestDepth = gNormalDepth.Load(int3(nearest, 0)).z;

    // --- Gather the triangles that reach the footprint --------------------------------
    // Each owner's triangle, bounded by its gated edges and moved into the footprint's
    // frame. The nearest owner that reaches the footprint decides the front surface.
    TriangleEdges ownerTriangle[9];
    float         ownerDepth[9];
    int2          ownerPixel[9];
    uint          ownerCount = 0;
    int           front = -1;
    float         frontDepth = 2.0f;

    [loop]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [loop]
        for (int dx = -1; dx <= 1; ++dx)
        {
            const int2 owner = nearest + int2(dx, dy);
            if (!InRender(owner))
                continue;

            TriangleEdges t = UnpackTriangle(gEdges.Load(int3(owner, 0)));
            if (!HasAnyEdge(t))
                continue;
            t = Shift(t, float2(owner) + 0.5f, srcPos);
            if (CoveredFraction(float2(0.0f, 0.0f), halfFootprint, t) <= 1e-4f)
                continue;

            const float z = gNormalDepth.Load(int3(owner, 0)).z;
            ownerTriangle[ownerCount] = t;
            ownerDepth[ownerCount] = z;
            ownerPixel[ownerCount] = owner;
            if (z < frontDepth)
            {
                frontDepth = z;
                front = (int)ownerCount;
            }
            ++ownerCount;
        }
    }

    // --- Merge the front surface's distinct triangles ---------------------------------
    SurfaceUnion u;
    u.T[0] = NoTriangle();
    u.T[1] = NoTriangle();
    u.T[2] = NoTriangle();
    u.Count = 0;
    float coverage = 0.0f;
    int2  frontPixel = nearest;
    if (front >= 0)
    {
        frontPixel = ownerPixel[front];
        const float2 frontCentre = float2(frontPixel) + 0.5f;
        const float2 gradient = DepthGradient(frontPixel, frontDepth);

        [loop]
        for (uint i = 0; i < ownerCount; ++i)
        {
            // Same surface: on the front owner's plane.
            const float predicted = frontDepth + dot(gradient, (float2(ownerPixel[i]) + 0.5f) - frontCentre);
            if (RelativeDepthStep(ownerDepth[i] - predicted, frontDepth, gProjectionZ33) > 4.0f * gDepthThreshold)
                continue;

            bool duplicate = false;
            [unroll]
            for (uint j = 0; j < 3; ++j)
                duplicate = duplicate || (j < u.Count && SameTriangle(u.T[j], ownerTriangle[i]));
            if (!duplicate && u.Count < 3)
            {
                u.T[u.Count] = ownerTriangle[i];
                ++u.Count;
            }
        }
        coverage = UnionFraction(halfFootprint, u.T[0], u.T[1], u.T[2], u.Count);
    }

    float3 result;
    uint   path;   // 0 edge, 1 interior, 2 interior on a nearer covering surface
    if (u.Count > 0 && coverage > 1e-4f && coverage < 0.9999f)
    {
        // --- Edge path ----------------------------------------------------------------
        // Surface B is represented by the render sample nearest the footprint outside
        // the union.
        const int2 base = int2(floor(srcPos - 0.5f));
        int2  outsidePixel = frontPixel;
        float outsideDistance = 1e9f;
        [loop]
        for (int y = -1; y <= 2; ++y)
        {
            [loop]
            for (int x = -1; x <= 2; ++x)
            {
                const int2 tap = base + int2(x, y);
                const float2 d = (float2(tap) + 0.5f) - srcPos;
                if (InRender(tap) && !InsideUnion(u, d) && dot(d, d) < outsideDistance)
                {
                    outsideDistance = dot(d, d);
                    outsidePixel = tap;
                }
            }
        }

        const float3 colorA = ReconstructSide(srcPos, frontPixel, u, true);
        const float3 colorB = outsideDistance < 1e8f ? ReconstructSide(srcPos, outsidePixel, u, false) : colorA;
        result = lerp(colorB, colorA, coverage);
        path = 0u;
    }
    else
    {
        // --- Interior path --------------------------------------------------------------
        // Normally the nearest render pixel's surface; but a nearer surface whose
        // triangles put this whole footprint inside them owns the pixel even when none of
        // its render samples landed here (the foreground side of a silhouette, magnified).
        const bool nearerCovering = u.Count > 0 && coverage >= 0.9999f && frontDepth < nearestDepth;
        bool fellBack;
        result = ReconstructInterior(srcPos, nearerCovering ? frontPixel : nearest, fellBack);
        path = nearerCovering ? 2u : 1u;
    }

    // --- Debug views ---------------------------------------------------------------------
    if (gDebugView != 0u)
    {
        // Still HDR here: scale the overlay by the local brightness.
        const float brightness = max(Luminance(gSceneColor.Load(int3(nearest, 0)).rgb), 1e-4f) * 2.0f + 1e-3f;
        if (gDebugView == 1u)
        {
            // Red: analytic edge reconstruction. Green: interior Catmull-Rom.
            // Blue: interior, on a nearer surface found through its triangles.
            const float3 mask = path == 0u ? float3(1, 0, 0) : (path == 1u ? float3(0, 0.35f, 0) : float3(0, 0, 1));
            return float4(lerp(result, mask * brightness, path == 1u ? 0.5f : 0.85f), 1.0f);
        }
        // Coverage of surface A at output resolution; interior pixels dimmed.
        return float4(path == 0u ? coverage.xxx * brightness : result * 0.15f, 1.0f);
    }

    return float4(max(result, 0.0f), 1.0f);
}