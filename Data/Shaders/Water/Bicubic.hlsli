//
// Description : Bicubic filtering functions
//
// Ported from tuxalin/water-shader (shaders/hlsl/bicubic.cginc).  The original
// took one scalar texture size for both axes, which is only right for square
// textures; this takes the full width/height because it filters the (screen
// sized) scene-colour copy.
//

#ifndef WATER_BICUBIC_HLSLI
#define WATER_BICUBIC_HLSLI

float4 cubic(float v)
{
    float4 n = float4(1.0, 2.0, 3.0, 4.0) - v;
    float4 s = n * n * n;
    float x = s.x;
    float y = s.y - 4.0 * s.x;
    float z = s.z - 4.0 * s.y + 6.0 * s.x;
    float w = 6.0 - x - y - z;
    return float4(x, y, z, w) * (1.0 / 6.0);
}

// 4 taps bicubic filtering, requires sampler to use bilinear filtering
float4 SampleBicubic(Texture2D tex, SamplerState linearSampler, float2 texSize, float2 texCoords)
{
    float2 invTexSize = 1.0 / texSize;

    texCoords = texCoords * texSize - 0.5;
    float2 fxy = frac(texCoords);
    texCoords -= fxy;

    float4 xcubic = cubic(fxy.x);
    float4 ycubic = cubic(fxy.y);

    float4 c = texCoords.xxyy + float2(-0.5, +1.5).xyxy;

    float4 s = float4(xcubic.xz + xcubic.yw, ycubic.xz + ycubic.yw);
    float4 offset = c + float4(xcubic.yw, ycubic.yw) / s;

    offset *= invTexSize.xxyy;

    float4 sample0 = tex.SampleLevel(linearSampler, offset.xz, 0);
    float4 sample1 = tex.SampleLevel(linearSampler, offset.yz, 0);
    float4 sample2 = tex.SampleLevel(linearSampler, offset.xw, 0);
    float4 sample3 = tex.SampleLevel(linearSampler, offset.yw, 0);

    float sx = s.x / (s.x + s.y);
    float sy = s.z / (s.z + s.w);

    return lerp(lerp(sample3, sample2, sx), lerp(sample1, sample0, sx), sy);
}

#endif // WATER_BICUBIC_HLSLI
