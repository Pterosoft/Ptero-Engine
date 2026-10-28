cbuffer DepthDebugConstants : register(b0)
{
    float gDepthScale;
    float gNearPlane;
    float gFarPlane;
    float _Pad;
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

    // Nothing was drawn here - show it as black rather than as the far plane.
    if (depth >= 1.0f)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    // Device depth back to a linear distance along the view axis.
    const float denominator = gFarPlane - depth * (gFarPlane - gNearPlane);
    const float viewZ = (gNearPlane * gFarPlane) / max(denominator, 1e-6f);

    // Map that distance logarithmically across the whole frustum, so the near
    // plane reads black and the far plane white with usable contrast in
    // between. A linear mapping is useless here: with an 80,000:1 far-to-near
    // ratio an entire room occupies a sliver at the dark end, which is how this
    // preview came out uniformly black before.
    const float preview = saturate(
        log2(max(viewZ / gNearPlane, 1.0f)) / log2(max(gFarPlane / gNearPlane, 2.0f)) * gDepthScale);

    return float4(preview, preview, preview, 1.0f);
}
