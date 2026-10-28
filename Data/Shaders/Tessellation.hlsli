// Tessellation.hlsli
// Shared by the tessellated G-Buffer paths (GBuffer.hlsl for meshes, Terrain.hlsl for
// terrain): adaptive edge factors, patch culling and the displacement mip.
//
// Crack-free by construction: an edge's factor depends only on its two end points, and
// both triangles sharing the edge see the same points, so they split it identically.
// That only holds where the mesh shares vertices - a hard edge or UV seam duplicates
// them, and displacement opens a gap there.

#ifndef PTERO_TESSELLATION_HLSLI
#define PTERO_TESSELLATION_HLSLI

// Hardware limit for SV_TessFactor.
static const float kPteroMaxTessFactor = 64.0f;

struct PteroTessPatchConstants
{
    // Edge i is the one opposite control point i.
    float Edges[3] : SV_TessFactor;
    float Inside   : SV_InsideTessFactor;
    // Mip the domain shader samples the displacement map at.
    float DisplacementMip : DISPLACEMENTMIP;
};

// Split an edge until each piece is about targetPixels long on screen.
//   pixelScale   = (viewport height / 2) / tan(fovY / 2): screen pixels spanned by a
//                  length L at distance d is L * pixelScale / d.
//   fadeDistance = metres at which subdivision has tapered to none; 0 = no fade.
float PteroTessEdgeFactor(
    float3 a,
    float3 b,
    float3 cameraPositionWS,
    float  pixelScale,
    float  targetPixels,
    float  maxFactor,
    float  fadeDistance)
{
    const float edgeLength = distance(a, b);
    const float viewDistance = max(distance(0.5f * (a + b), cameraPositionWS), 1e-3f);
    float factor = edgeLength * pixelScale / (viewDistance * max(targetPixels, 0.5f));
    if (fadeDistance > 0.0f)
        factor *= saturate(1.0f - viewDistance / fadeDistance);
    return clamp(factor, 1.0f, clamp(maxFactor, 1.0f, kPteroMaxTessFactor));
}

// True when the triangle lies entirely outside one clip plane. `slack` widens every
// plane so displacement cannot push visible geometry out of a culled patch; it is a
// clip-space distance, which for a margin of D metres needs roughly D * max(P00, P11).
bool PteroTessPatchOutsideFrustum(float4 c0, float4 c1, float4 c2, float slack)
{
    const float3 x = float3(c0.x, c1.x, c2.x);
    const float3 y = float3(c0.y, c1.y, c2.y);
    const float3 z = float3(c0.z, c1.z, c2.z);
    const float3 w = float3(c0.w, c1.w, c2.w) + slack;
    return all(x < -w) || all(x > w)
        || all(y < -w) || all(y > w)
        || all(z < -slack);   // behind the near plane (depth runs 0..1)
}

// Mip level whose texels match the spacing of the tessellated vertices: sampling
// finer than that aliases the displacement into spikes.
float PteroDisplacementMip(float2 uv0, float2 uv1, float2 uv2, float insideFactor, float2 textureSize)
{
    const float uvEdge = max(max(length((uv1 - uv0) * textureSize),
                                 length((uv2 - uv1) * textureSize)),
                                 length((uv0 - uv2) * textureSize));
    return max(log2(max(uvEdge / max(insideFactor, 1.0f), 1.0f)), 0.0f);
}

#endif // PTERO_TESSELLATION_HLSLI
