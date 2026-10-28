// VolumetricCloudComposite.hlsl
// Blends the resolved cloud buffer over the lit scene colour.
//
// The pipeline blend is configured as (SrcBlend = ONE, DestBlend = SRC_ALPHA),
// so the render target ends up holding
//
//     scene * transmittance + inScatteredLuminance
//
// which is exactly the volume rendering equation the raymarch integrated.  The
// debug views return alpha = 0 so they replace the scene rather than blend
// with it, without needing a second pipeline state.

#include "VolumetricCloudConstants.hlsli"

Texture2D<float4> gCloudColor : register(t8);
Texture2D<float>  gCloudDepth : register(t9);

struct VSOutput
{
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput output;
    output.TexCoord = float2((vertexId << 1) & 2, vertexId & 2);
    output.Position = float4(output.TexCoord * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    int3 texel = int3((int2)input.Position.xy, 0);
    float4 cloud = gCloudColor.Load(texel);

    float3 luminance = max(cloud.rgb, float3(0.0f, 0.0f, 0.0f));
    float transmittance = saturate(cloud.a);

    switch (gDebugView)
    {
        case 1: // In-scattered luminance only.
            return float4(luminance, 0.0f);

        case 2: // Transmittance as greyscale.
            return float4(transmittance, transmittance, transmittance, 0.0f);

        case 3: // Cloud coverage (1 - transmittance).
        {
            float coverage = 1.0f - transmittance;
            return float4(coverage, coverage, coverage, 0.0f);
        }

        case 4: // Distance to the cloud, normalised over the trace range.
        {
            float distance = gCloudDepth.Load(texel);
            float normalized = saturate(distance / max(gMaxTraceDistance, 1.0f));
            return float4(normalized, normalized * 0.5f, 1.0f - normalized, 0.0f);
        }

        default:
            return float4(luminance, transmittance);
    }
}
