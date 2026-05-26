// SkyPass.hlsl
// Fullscreen sky background pass.
// Draws a simple vertical sky gradient derived from the Hosek-Wilkie sky colour
// (evaluated on the CPU and uploaded as a constant) toward the horizon.
// The sun disc is also drawn as a bright circular region in screen space.

cbuffer SkyConstants : register(b0)
{
    // Sky colour at the zenith (linear HDR RGB, already scaled to display range).
    float3 SkyZenithColor;
    float  _Pad0;

    // Sky colour at the horizon.
    float3 SkyHorizonColor;
    float  _Pad1;

    // Sun direction in view space (from scene toward sun, normalised).
    float3 SunDirectionVS;
    float  _Pad2;

    // Sun disc colour (linear HDR RGB).
    float3 SunColor;
    float  SunDiscHalfAngleCos; // cos(0.265°) ≈ cos(solar angular radius * 2)

    // Inverse projection matrix to reconstruct view-space ray from NDC.
    float4x4 InvProj;
}

struct VSOutput
{
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD;
};

// Emit a full-screen triangle with no vertex buffer.
VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput output;
    // Generate a triangle that covers the entire screen.
    output.TexCoord   = float2((vertexId << 1) & 2, vertexId & 2);
    output.Position   = float4(output.TexCoord * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 1.0f, 1.0f);
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    // Reconstruct the view-space ray direction for this pixel.
    float2 ndc      = input.Position.xy; // SV_Position is in screen px; we use TexCoord instead
    float2 ndcCoord = input.TexCoord * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);

    float4 viewRay4 = mul(float4(ndcCoord, 1.0f, 1.0f), InvProj);
    float3 viewDir  = normalize(viewRay4.xyz / viewRay4.w);

    // Elevation in view space: positive Z = up in view space.
    // We use the Y component for the gradient (view Y = world Z because of Z-up camera).
    float elevation = viewDir.y; // [-1,1], 1 = straight up, -1 = straight down

    // Sky gradient: blend zenith/horizon based on elevation.
    float t = saturate(elevation * 2.0f); // remap so horizon = 0, mid-sky = 1
    float3 skyColor = lerp(SkyHorizonColor, SkyZenithColor, t * t);

    // Sun disc: angle between view ray and sun direction.
    float cosAngle = dot(viewDir, SunDirectionVS);
    // Soft edge on the sun disc.
    float sunEdge  = saturate((cosAngle - SunDiscHalfAngleCos + 0.002f) / 0.002f);
    float3 finalColor = lerp(skyColor, SunColor, sunEdge);

    return float4(finalColor, 1.0f);
}
