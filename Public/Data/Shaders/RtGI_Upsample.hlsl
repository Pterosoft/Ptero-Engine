// RtGI_Upsample.hlsl  –  cs_6_5
// Fills the frame back in from a reduced-resolution RTGI grid (RtGI_Common.hlsli,
// "Reduced-resolution grids"): the denoised diffuse GI on the trace grid, or the
// specular reflections on the specular grid.
//
// A joint bilateral upsample. Each frame pixel blends the four grid pixels around
// it - bilinearly, by where it falls between the frame pixels they were shaded for -
// weighted down where those frame pixels lie on another surface: a different
// distance from the camera, a different facing, and for reflections a different
// roughness. A pixel that is itself one of those frame pixels gets its grid value
// back unchanged. Where all four belong to other surfaces (a thin object no grid
// pixel landed on) the closest one in depth stands in.
//
// Root signature: RtGlobalIllumination's NRD layout.
//   param 0 : CBV  b0  RtGIConstants
//   param 1 : SRV  t0  grid result (RGBA16F)
//   param 2 : SRV  t1  normalDepth G-buffer (xy: oct normal, z: hw depth)
//   param 3 : SRV  t2  material G-buffer (r: roughness) - specular only
//   param 5 : UAV  u0  frame-resolution output (RGBA16F)

#include "RtGI_Common.hlsli"

Texture2D<float4>   t_Reduced     : register(t0);
Texture2D<float4>   t_NormalDepth : register(t1);
Texture2D<float4>   t_Material    : register(t2);
RWTexture2D<float4> u_Output      : register(u0);

float3 DecodeGBufferNormal(float2 encoded)
{
    const float2 octN = encoded * 2.0f - 1.0f;
    const float  octZ = 1.0f - abs(octN.x) - abs(octN.y);
    const float2 wrapped = (1.0f - abs(octN.yx)) * (float2(octN.xy >= 0.0f) * 2.0f - 1.0f);
    return normalize(float3(octZ < 0.0f ? wrapped : octN.xy, octZ));
}

bool IsSkyDepth(float depth)
{
    return depth <= 0.0f || depth >= 1.0f;
}

void Upsample(uint2 pixel, uint scale, bool matchRoughness)
{
    if (any(pixel >= uint2(g_FrameWidth, g_FrameHeight)))
        return;

    const float4 nd = t_NormalDepth.Load(int3(pixel, 0));
    if (IsSkyDepth(nd.z))
    {
        u_Output[pixel] = 0.0f.xxxx;
        return;
    }
    const float3 normal   = DecodeGBufferNormal(nd.xy);
    const float  distance = length(ReconstructWorldPos(pixel, nd.z) - g_CameraPos);
    const float  roughness = matchRoughness ? t_Material.Load(int3(pixel, 0)).r : 0.0f;

    // Grid pixel r was shaded for frame pixel r * scale + scale / 2, so this frame
    // pixel sits at r = (pixel - scale / 2) / scale on the grid.
    const int2   gridMax = int2(ReducedSize(scale)) - 1;
    const float2 gridPos = (float2(pixel) - float(scale / 2u)) / float(scale);
    const int2   base    = int2(floor(gridPos));
    const float2 f       = gridPos - float2(base);

    // A few percent of the distance, so the depth test is as strict far away as
    // near the camera.
    const float depthTolerance = max(distance * 0.03f, 0.01f);

    float4 sum = 0.0f.xxxx;
    float  weightSum = 0.0f;
    // The same blend without the bilinear term, for a pixel whose nearest grid
    // pixels all lie on other surfaces.
    float4 geometricSum = 0.0f.xxxx;
    float  geometricWeightSum = 0.0f;
    float4 closest = 0.0f.xxxx;
    float  closestDepthError = 1e30f;

    [unroll]
    for (uint i = 0; i < 4u; ++i)
    {
        const int2 corner = int2(i & 1u, i >> 1u);
        const uint2 tap = uint2(clamp(base + corner, int2(0, 0), gridMax));
        const uint2 tapPixel = ReducedToFramePixel(tap, scale);

        const float4 tapNd = t_NormalDepth.Load(int3(tapPixel, 0));
        if (IsSkyDepth(tapNd.z))
            continue;

        const float4 value = t_Reduced.Load(int3(tap, 0));
        const float  tapDistance = length(ReconstructWorldPos(tapPixel, tapNd.z) - g_CameraPos);
        const float  depthError = abs(tapDistance - distance);
        if (depthError < closestDepthError)
        {
            closestDepthError = depthError;
            closest = value;
        }

        // Depth is what separates surfaces; the normal and roughness terms only lean
        // away from a crease or a material change. They are soft on purpose: the
        // G-Buffer normal carries the normal map, and a steep weight on it let one
        // tap win outright wherever bumps tilted the others away - a different tap
        // each frame as the jitter moved the pixel, which sparkled up close.
        const float bilinear = (corner.x != 0 ? f.x : 1.0f - f.x) * (corner.y != 0 ? f.y : 1.0f - f.y);
        const float depthWeight = exp(-depthError / depthTolerance);
        const float facing = saturate(dot(normal, DecodeGBufferNormal(tapNd.xy)));
        const float normalWeight = lerp(0.2f, 1.0f, facing * facing);
        float geometric = depthWeight * normalWeight;
        if (matchRoughness)
            geometric *= exp(-abs(t_Material.Load(int3(tapPixel, 0)).r - roughness) * 4.0f);
        const float weight = bilinear * geometric;

        sum += weight * value;
        weightSum += weight;
        geometricSum += geometric * value;
        geometricWeightSum += geometric;
    }

    // Where the bilinear weights land on other surfaces, blend by surface match
    // alone rather than snapping to one grid pixel: a hard pick changed with the
    // jitter from frame to frame. Only with nothing matching at all does the
    // closest one in depth stand in.
    if (weightSum > 1e-3f)
        u_Output[pixel] = sum / weightSum;
    else if (geometricWeightSum > 1e-4f)
        u_Output[pixel] = geometricSum / geometricWeightSum;
    else
        u_Output[pixel] = closest;
}

// The diffuse GI, from the trace grid.
[numthreads(8, 8, 1)]
void CSMainDiffuse(uint3 DTid : SV_DispatchThreadID)
{
    Upsample(DTid.xy, g_TraceScale, false);
}

// The specular reflections, from the specular grid.
[numthreads(8, 8, 1)]
void CSMainSpecular(uint3 DTid : SV_DispatchThreadID)
{
    Upsample(DTid.xy, g_SpecularScale, true);
}
