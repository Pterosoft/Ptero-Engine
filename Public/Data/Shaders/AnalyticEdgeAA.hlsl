// AnalyticEdgeAA.hlsl
// Analytic Edge AA: frequency-aware anti-aliasing of geometric edges.
//
// Aliasing has more than one source, and they want different treatment. A geometric
// edge is an exact step in visibility, and its correct pixel value is a box filter
// over that step:
//
//     colour = sum over surfaces of  Area(surface n pixel) / Area(pixel) * radiance
//
// MSAA estimates those areas with a handful of point samples, and SMAA and TAA infer
// them from the colours an edge leaves behind. Neither needs to: the G-Buffer pass
// already stored the exact screen-space lines of the triangle that won each pixel
// (AnalyticEdge.hlsli). Clipping the pixel square against them gives the area itself,
// so a silhouette pixel gets e.g. 0.3724 of its own surface rather than 1/4, 2/4, 3/4.
//
// The pass is a hybrid, so that only edge pixels pay for it:
//
//   1. Classify. Each pixel compares itself with its four neighbours in the G-Buffer.
//      A geometric break is a depth step that the neighbouring plane does not predict,
//      or a crease in the normal. Everything else - the interior of a surface, shading
//      and texture detail - is not this pass's frequency band; it discards, leaving
//      that to TAA / SMAA.
//
//   2. Own coverage. The pixel's own triangle is clipped by those of its edges that
//      bound a break with a farther surface (or a crease) on the other side
//      (GateEdge). What the triangle leaves uncovered shows the neighbour across.
//
//   3. Intrusion. A nearer triangle whose centre landed in the next pixel can still
//      cover part of this one. Its edges, moved into this pixel's frame, clip this
//      pixel's square too, and that area takes the neighbour's colour.
//
// Steps 2 and 3 describe the same edge line from both sides, so the foreground and
// background pixels of a silhouette agree on where it is.
//
// Runs on the lit opaque scene (before water, particles, clouds and SSR are layered
// on) and writes edge pixels back into the scene colour; it reads a copy of it, so
// neighbours are always the unfiltered values.

#include "AnalyticEdgeResolve.hlsli"

cbuffer AnalyticEdgeConstants : register(b0)
{
    uint2 gSize;
    // Smallest relative change in 1/viewZ, beyond what the neighbouring plane predicts,
    // that counts as a geometric edge.
    float gDepthThreshold;
    // Normals with a smaller dot product than this meet at a crease. Below -1 disables.
    float gNormalThreshold;
    // The projection's _33: device depth minus this is proportional to 1/viewZ.
    float gProjectionZ33;
    // 0 = result, 1 = classification, 2 = coverage.
    uint  gDebugView;
    float2 gPad;
};

Texture2D<float4> gSceneColor  : register(t0);   // copy of the lit scene
Texture2D<float4> gNormalDepth : register(t1);   // G-Buffer normal target: oct normal xy, device depth z
Texture2D<uint4>  gEdges       : register(t2);   // G-Buffer edge target

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

int3 PixelAddress(int2 pixel)
{
    return int3(clamp(pixel, int2(0, 0), int2(gSize) - 1), 0);
}

float4 PSMain(VSOutput input) : SV_Target
{
    const int2 pixel = int2(input.Position.xy);

    const float4 centreNormalDepth = gNormalDepth.Load(int3(pixel, 0));
    const float  zP = centreNormalDepth.z;
    const float3 normalP = DecodeOctNormal(centreNormalDepth.xy);
    const float4 centreColor = gSceneColor.Load(int3(pixel, 0));

    // --- 1. Classify -------------------------------------------------------------
    float  neighbourDepth[4];
    float3 neighbourNormal[4];
    [unroll]
    for (uint n = 0; n < 4; ++n)
    {
        const float4 normalDepth = gNormalDepth.Load(PixelAddress(pixel + kNeighbourOffsets[n]));
        neighbourDepth[n] = normalDepth.z;
        neighbourNormal[n] = DecodeOctNormal(normalDepth.xy);
    }

    bool breaks[4];
    bool inFront[4];
    bool anyBreak = false;
    [unroll]
    for (uint k = 0; k < 4; ++k)
    {
        breaks[k] = IsGeometricBreak(zP, neighbourDepth[k], neighbourDepth[k ^ 1u], normalP, neighbourNormal[k],
                                     gProjectionZ33, gDepthThreshold, gNormalThreshold);
        inFront[k] = breaks[k] && neighbourDepth[k] < zP;
        anyBreak = anyBreak || breaks[k];
    }

    if (!anyBreak)
    {
        if (gDebugView != 0u)
            return float4(centreColor.rgb * 0.15f, centreColor.a);
        discard;
        return centreColor;
    }

    float3 neighbourColor[4];
    [unroll]
    for (uint c = 0; c < 4; ++c)
        neighbourColor[c] = gSceneColor.Load(PixelAddress(pixel + kNeighbourOffsets[c])).rgb;

    // --- 2. Own coverage ----------------------------------------------------------
    // Only edges with a break beyond them bound anything visible: an interior edge of
    // the mesh (same surface beyond) is dropped, and so is one whose far side is
    // nearer - that neighbour's triangle covers the region, which step 3 handles.
    TriangleEdges own = UnpackTriangle(gEdges.Load(int3(pixel, 0)));
    uint across[3];
    [unroll]
    for (uint e = 0; e < 3; ++e)
    {
        own.E[e].Valid = GateEdge(gNormalDepth, pixel, int2(gSize), own.E[e], zP, normalP,
                                  gProjectionZ33, gDepthThreshold, gNormalThreshold);
        across[e] = NeighbourToward(-own.E[e].Normal);
    }

    const float ownCoverage = CoveredFraction(float2(0.0f, 0.0f), float2(0.5f, 0.5f), own);

    float3 result = centreColor.rgb;
    if (ownCoverage < 1.0f)
    {
        // The uncovered part shows the surface beyond each edge, in proportion to how
        // much of the square that edge cuts away.
        float3 beyond = float3(0.0f, 0.0f, 0.0f);
        float  cutTotal = 0.0f;
        [unroll]
        for (uint i = 0; i < 3; ++i)
        {
            if (own.E[i].Valid)
            {
                const float cut = 1.0f - CoveredFraction(float2(0.0f, 0.0f), float2(0.5f, 0.5f), SingleEdgeTriangle(own.E[i]));
                beyond += cut * neighbourColor[across[i]];
                cutTotal += cut;
            }
        }
        result = lerp(beyond / max(cutTotal, 1e-6f), centreColor.rgb, ownCoverage);
    }

    // --- 3. Intrusion from nearer neighbours ---------------------------------------
    // Two neighbours on the same nearer triangle (the step of a staircase: left and
    // below) describe the same region across the same edge line, so their areas must
    // not add. An intruder whose separating line matches an earlier one's keeps only
    // the larger area; different lines (different edges or surfaces) still add.
    float  intruderArea[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float3 intruderLine[4];   // separating edge in this pixel's frame: normal xy, distance z
    [unroll]
    for (uint j = 0; j < 4; ++j)
    {
        intruderLine[j] = float3(0.0f, 0.0f, 0.0f);
        if (inFront[j])
        {
            const float2 offset = float2(kNeighbourOffsets[j]);
            TriangleEdges theirs = UnpackTriangle(gEdges.Load(PixelAddress(pixel + kNeighbourOffsets[j])));

            // In the neighbour's frame this pixel's centre sits at -offset. Only an edge
            // that centre is outside of separates the two surfaces; one it is inside of
            // is most likely an interior edge of the nearer mesh, whose next triangle
            // carries on covering, so it must not clip. With no separating edge (nothing
            // stored, or a cut-out silhouette) there is nothing exact to add.
            bool anySeparating = false;
            float2 lineNormal = float2(0.0f, 0.0f);
            float  lineDistance = 0.0f;
            [unroll]
            for (uint i = 0; i < 3; ++i)
            {
                theirs.E[i].Valid = theirs.E[i].Valid && EdgeSide(theirs.E[i], -offset) < 0.0f;
                if (theirs.E[i].Valid && !anySeparating)
                {
                    lineNormal = theirs.E[i].Normal;
                    lineDistance = theirs.E[i].Distance;
                }
                anySeparating = anySeparating || theirs.E[i].Valid;
            }

            if (anySeparating)
            {
                intruderArea[j] = CoveredFraction(-offset, float2(0.5f, 0.5f), theirs);
                intruderLine[j] = float3(lineNormal, lineDistance - dot(lineNormal, offset));

                [unroll]
                for (uint m = 0; m < j; ++m)
                {
                    const bool sameLine = intruderArea[m] > 0.0f
                        && dot(intruderLine[m].xy, intruderLine[j].xy) > 0.995f
                        && abs(intruderLine[m].z - intruderLine[j].z) < 0.15f;
                    if (sameLine)
                    {
                        if (intruderArea[j] > intruderArea[m])
                            intruderArea[m] = 0.0f;
                        else
                            intruderArea[j] = 0.0f;
                    }
                }
            }
        }
    }

    float  intrusion = 0.0f;
    float3 intrusionColor = float3(0.0f, 0.0f, 0.0f);
    [unroll]
    for (uint s = 0; s < 4; ++s)
    {
        intrusion += intruderArea[s];
        intrusionColor += intruderArea[s] * neighbourColor[s];
    }

    if (intrusion > 0.0f)
        result = lerp(result, intrusionColor / intrusion, saturate(intrusion));

    // --- Debug views -----------------------------------------------------------------
    if (gDebugView != 0u)
    {
        // The scene colour is still HDR here, so scale the overlay by the local
        // brightness; a fixed 1.0 could be anything from black to white after exposure.
        float brightness = Luminance(centreColor.rgb);
        [unroll]
        for (uint d = 0; d < 4; ++d)
            brightness = max(brightness, Luminance(neighbourColor[d]));
        brightness = max(brightness, 1e-4f) * 2.0f;

        if (gDebugView == 1u)
        {
            // Red: own triangle clipped. Green: covered by a nearer neighbour's triangle.
            // Blue: a geometric break with no triangle edge to resolve it exactly.
            const bool clippedOwn = ownCoverage < 0.999f;
            const bool covered = intrusion > 0.001f;
            const float3 mask = float3(clippedOwn ? 1.0f : 0.0f, covered ? 1.0f : 0.0f,
                                       (clippedOwn || covered) ? 0.0f : 1.0f);
            return float4(mask * brightness, centreColor.a);
        }

        // Coverage: the share of the pixel its own surface keeps.
        const float kept = ownCoverage * (1.0f - saturate(intrusion));
        return float4(kept.xxx * brightness, centreColor.a);
    }

    return float4(result, centreColor.a);
}