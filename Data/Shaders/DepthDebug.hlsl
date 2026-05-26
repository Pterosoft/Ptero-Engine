cbuffer DepthDebugConstants : register(b0)
{
    float gDepthScale;
    float3 _Pad;
};

Texture2D gDepthBuffer : register(t0);
SamplerState gPointSampler : register(s0);

struct VSOutput
{
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput output;
    float2 pos;
    pos.x = (vertexId == 2) ? 3.0f : -1.0f;
    pos.y = (vertexId == 1) ? -3.0f : 1.0f;
    output.Position = float4(pos, 0.0f, 1.0f);
    output.TexCoord = float2((pos.x + 1.0f) * 0.5f, 1.0f - ((pos.y + 1.0f) * 0.5f));
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    float depth = saturate(gDepthBuffer.Sample(gPointSampler, input.TexCoord).r);
    // Show a reversed preview of the scene depth so surfaces that sit at large
    // raw depth values do not saturate to white in the debug view.
    float preview = saturate((1.0f - depth) * gDepthScale);
    return float4(preview, preview, preview, 1.0f);
}
