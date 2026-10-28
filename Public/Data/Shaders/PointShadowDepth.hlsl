cbuffer ShadowEntityConstants : register(b0)
{
    float4x4 gLightMVP;
    float4x4 gModel;
};

cbuffer PointShadowFaceConstants : register(b1)
{
    float3 gLightPosition;
    float  gFarPlane;
};

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD;
    float4 Color    : COLOR;
};

struct PSInput
{
    float4 Position : SV_Position;
    float3 WorldPos : WORLDPOS;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    float4 localPos = float4(input.Position, 1.0f);
    float4 worldPos = mul(localPos, gModel);
    output.Position = mul(localPos, gLightMVP);
    output.WorldPos = worldPos.xyz;
    return output;
}

float PSMain(PSInput input) : SV_Depth
{
    const float distanceToLight = distance(input.WorldPos, gLightPosition);
    return saturate(distanceToLight / max(gFarPlane, 1e-4f));
}