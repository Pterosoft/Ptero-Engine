// AnalyticEdge.hlsli
// The G-Buffer side of Analytic Edge AA and Analytic-Aware Reconstruction (see
// AnalyticEdgeResolve.hlsli for the consumers).
//
// Rasterisation decides visibility from one point sample per pixel. The pixel shader
// that wins that sample, though, knows the exact screen-space lines of the triangle it
// belongs to: noperspective SV_Barycentrics are affine in screen space, so each
// barycentric b_i(x) = b_i + grad(b_i) . x over the whole triangle, and b_i = 0 is
// edge i. Dividing by |grad(b_i)| turns that into a signed distance in pixels:
//
//     inside edge i  <=>  d_i + n_i . x >= 0,   d_i = b_i / |grad b_i|,  n_i = grad b_i / |grad b_i|
//
// with x measured in pixels from the pixel centre. Consumers clip rectangles against
// these half-planes, which gives the exact area of (triangle n rectangle) instead of a
// sample-count estimate of it.
//
// All three edges are stored, so a pixel describes its whole triangle near it: a thin
// sliver or the tip of a triangle is bounded on every side. An edge is stored only when
// it is close enough to matter to this pixel's neighbourhood (kPteroEdgeMaxDistance).
//
// Edge target, R16G16B16A16_UINT (AnalyticEdge::kGBufferFormat on the C++ side), one
// edge per channel in R, G, B (A unused):
//   bits 15..8  edge direction: atan2 of the inward normal, mapped to 0..255
//   bits  7..0  distance from the pixel centre to the edge / kPteroEdgeMaxDistance,
//               0..254; 255 means no edge
//
// Shaders opt in with PTERO_ANALYTIC_EDGES=1, which needs SM 6.1 (SV_Barycentrics);
// AnalyticEdge::MakeGBufferPixelShaderRequest() sets both when the GPU supports them.
// Without it, PTERO_EDGE_VALUE is "no edge" and the consumers leave the pixel alone.

#ifndef PTERO_ANALYTIC_EDGE_HLSLI
#define PTERO_ANALYTIC_EDGE_HLSLI

#ifndef PTERO_ANALYTIC_EDGES
#define PTERO_ANALYTIC_EDGES 0
#endif

// An edge this many pixels from a centre can still cut the footprint of an output pixel
// whose render-resolution neighbourhood this pixel belongs to.
static const float kPteroEdgeMaxDistance = 2.0f;
static const float kPteroEdgePi = 3.14159265358979f;

static const uint kPteroNoEdgeCode = 0x00FFu;
// Also the edge target's clear colour (DeferredLightingPass.cpp).
static const uint4 kPteroNoEdge = uint4(kPteroNoEdgeCode, kPteroNoEdgeCode, kPteroNoEdgeCode, kPteroNoEdgeCode);

uint PteroPackEdge(float2 gradient, float barycentric)
{
    const float gradientLength = length(gradient);
    if (gradientLength < 1e-8f)
        return kPteroNoEdgeCode;

    const float distance = max(barycentric, 0.0f) / gradientLength;
    if (distance >= kPteroEdgeMaxDistance)
        return kPteroNoEdgeCode;

    const float2 normal = gradient / gradientLength;
    const uint angleCode = (uint)round(saturate(atan2(normal.y, normal.x) * (0.5f / kPteroEdgePi) + 0.5f) * 255.0f);
    const uint distanceCode = min((uint)round(distance / kPteroEdgeMaxDistance * 255.0f), 254u);
    return (angleCode << 8) | distanceCode;
}

#if PTERO_ANALYTIC_EDGES

uint4 PteroEncodeAnalyticEdges(float3 barycentrics)
{
    // Constant over the triangle, so the quad's helper lanes give the exact gradient.
    const float3 ddxBary = ddx_fine(barycentrics);
    const float3 ddyBary = ddy_fine(barycentrics);
    return uint4(
        PteroPackEdge(float2(ddxBary.x, ddyBary.x), barycentrics.x),
        PteroPackEdge(float2(ddxBary.y, ddyBary.y), barycentrics.y),
        PteroPackEdge(float2(ddxBary.z, ddyBary.z), barycentrics.z),
        kPteroNoEdgeCode);
}

// Appended to a G-Buffer pixel shader's parameter list.
#define PTERO_EDGE_PS_INPUT , noperspective float3 pteroEdgeBarycentrics : SV_Barycentrics
#define PTERO_EDGE_VALUE    PteroEncodeAnalyticEdges(pteroEdgeBarycentrics)

#else

#define PTERO_EDGE_PS_INPUT
#define PTERO_EDGE_VALUE    kPteroNoEdge

#endif

#endif // PTERO_ANALYTIC_EDGE_HLSLI
