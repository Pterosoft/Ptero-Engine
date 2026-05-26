// PointLight.hlsl
// Renders a wireframe sphere gizmo in the editor viewport to visualise each point light's
// world position and influence radius.  The sphere is drawn as a line-list so no fill
// or depth-write is needed; the gizmo is purely informational.

cbuffer PointLightGizmoConstants : register(b0)
{
    float4x4 gMVP;      // pre-transposed model-view-projection (sphere scaled to radius)
    float4    gColor;   // light colour used to tint the gizmo wireframe
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
    // Tint the wireframe with the light colour so it is easy to identify in a scene
    // that has multiple point lights with different colours.
    return float4(gColor.rgb, 0.8f);
}
