//
// Description : Utilties for normal mapping
//
// Ported from tuxalin/water-shader (shaders/hlsl/normals.cginc and
// unpack.cginc).  Only the helpers the water shader uses are kept.
//

#ifndef WATER_NORMALS_HLSLI
#define WATER_NORMALS_HLSLI

float3 UnpackNormal(float4 n)
{
    n.xyz = n.xyz * 2.0 - 1.0;
    return n.xyz;
}

// normal, tangent and bitangent in world space.  With useFiltering the normal
// map is sampled with doubled screen-space gradients (a wider mip), which
// trades a little sharpness for much less shimmer on the tiling texture.
float3 ComputeSurfaceNormal(float3 normal, float3 tangent, float3 bitangent,
    Texture2D tex, SamplerState texSampler, float2 uv, bool useFiltering)
{
    float3x3 tangentFrame = float3x3(normalize(bitangent), normalize(tangent), normal);

    // Explicit gradients either way (x1 is exactly a plain Sample), so there is
    // no implicit-derivative sample inside a branch.
    const float gradScale = useFiltering ? 2.0 : 1.0;
    float2 duv1 = ddx(uv) * gradScale;
    float2 duv2 = ddy(uv) * gradScale;
    float3 n = UnpackNormal(tex.SampleGrad(texSampler, uv, duv1, duv2));
    return normalize(mul(n, tangentFrame));
}

#endif // WATER_NORMALS_HLSLI
