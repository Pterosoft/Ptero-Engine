// Decal.hlsl
// Renders a wireframe oriented-bounding-box gizmo in the editor viewport to
// visualise each decal's world position, size, and facing direction.
// The box is drawn as a line-list.  A separate arrow (two lines) indicates
// the -Z facing direction of the decal projection.

cbuffer DecalGizmoConstants : register(b0)
{
    float4x4 gMVP;      // pre-transposed model-view-projection (box scaled to decal size)
    float4   gColor;    // tint colour for the wireframe
};

struct VSInput
{
    float3 Position : POSITION;
};

struct PSInput
{
    float4 Position : SV_Position;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.Position = mul(float4(input.Position, 1.0f), gMVP);
    return output;
}

float4 PSMain(PSInput input) : SV_Target
{
    return float4(gColor.rgb, 0.85f);
}
