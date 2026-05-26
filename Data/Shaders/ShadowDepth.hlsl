// ShadowDepth.hlsl
// Depth-only vertex shader for the shadow map pass.
// Transforms each vertex from world space into the sun's light-clip space.
// No pixel shader – only the depth buffer is written.

cbuffer ShadowEntityConstants : register(b0)
{
    float4x4 gLightMVP;  // pre-transposed model * light view-projection
};

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD;
    float4 Color    : COLOR;
};

float4 VSMain(VSInput input) : SV_Position
{
    return mul(float4(input.Position, 1.0f), gLightMVP);
}
