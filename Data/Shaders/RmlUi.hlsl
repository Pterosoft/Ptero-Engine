// RmlUi.hlsl - shading for the RmlUi user-interface pass.
//
// RmlUi hands the renderer a single vertex format (position, premultiplied RGBA, UV) and
// draws everything - backgrounds, borders, text glyphs, images - through it. Untextured
// geometry is drawn with a 1x1 white texture bound, so one pipeline covers every draw.
//
// gTransform is RmlUi's own transform pre-multiplied with the UI orthographic projection
// on the CPU. RmlUi uses column-vector math with column-major storage, which is also
// HLSL's default constant-buffer matrix packing, so the matrix uploads verbatim and is
// applied as mul(matrix, vector).

cbuffer RmlUiConstants : register(b0)
{
    float4x4 gTransform;
    float2   gTranslation;
    float2   gConstantsPadding;
};

Texture2D    gTexture : register(t0);
SamplerState gSampler : register(s0);

struct VSInput
{
    float2 Position : POSITION;
    float4 Color    : COLOR;
    float2 TexCoord : TEXCOORD;
};

struct VSOutput
{
    float4 Position : SV_POSITION;
    float4 Color    : COLOR;
    float2 TexCoord : TEXCOORD;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    // The per-draw translation arrives separately from the transform because RmlUi
    // reuses one compiled geometry at many positions.
    const float2 position = input.Position + gTranslation;
    output.Position = mul(gTransform, float4(position, 0.0f, 1.0f));
    output.Color    = input.Color;
    output.TexCoord = input.TexCoord;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    // Both the vertex colour and the texture carry premultiplied alpha, so this stays a
    // straight multiply and pairs with a (ONE, INV_SRC_ALPHA) blend.
    return gTexture.Sample(gSampler, input.TexCoord) * input.Color;
}
