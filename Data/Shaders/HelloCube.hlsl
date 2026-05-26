cbuffer PassConstants : register(b0)
{
    float4x4 gViewProj;
}

struct VSInput
{
    float3 Position : POSITION;
    float4 Color : COLOR;
};

struct PSInput
{
    float4 Position : SV_Position;
    float4 Color : COLOR;
};

PSInput VSMain(VSInput input)
{
    PSInput output;

    // The CPU uploads a transposed matrix, so a row-vector multiply keeps the transform straightforward.
    output.Position = mul(float4(input.Position, 1.0f), gViewProj);
    output.Color = input.Color;
    return output;
}

float4 PSMain(PSInput input) : SV_Target
{
    return input.Color;
}

PSInput main(VSInput input)
{
    return VSMain(input);
}

float4 main(PSInput input) : SV_Target
{
    return PSMain(input);
}
