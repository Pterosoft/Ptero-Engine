// VegetationShadow.hlsl
// Depth-only vegetation pass, used by both the sun cascade and the point-light
// cube shadow renderers.
//
// This shares Vegetation_Common.hlsli with the colour pass and must apply
// exactly the same bend: if the shadow pass used the unbent mesh, every tree
// would cast a shadow from a position it is not actually in, and the mismatch
// grows with wind strength.
//
// It also honours the material's alpha cutoff.  A leaf card that clips
// correctly in the colour pass but casts a solid rectangular shadow is even
// more obvious than one that renders as a solid rectangle.

#include "Vegetation_Common.hlsli"

cbuffer VegetationPassConstants : register(b0)
{
    // Carries the light's view-projection during this pass, not the camera's.
    float4x4 gViewProj;
    float3   gCameraPosition;    float gTime;
    float4   gWindParams0;
    float4   gWindParams1;
    float4   gInteractionParams;
    float4x4 gPrevViewProj;
};

cbuffer VegetationLayerConstants : register(b1)
{
    float4 gBaseColorTint;

    float  gAlphaCutoff;
    float  gRoughnessFactor;
    float  gMetallicFactor;
    float  gNormalScale;

    float  gAoStrength;
    float  gTranslucency;
    float  gWindInfluence;
    float  gStiffness;

    float  gFlutterAmount;
    float  gInteractionInfluence;
    int    gBendModel;
    uint   gInstanceOffset;

    int    gHasNormalMap;
    int    gHasMetallicMap;
    int    gHasRoughnessMap;
    int    gHasAoMap;

    int    gHasPackedMaterialMap;
    float3 _LayerPad0;
};

Texture2D gBaseColorTexture : register(t0);
Texture2D gNormalTexture    : register(t1);
Texture2D gMetallicTexture  : register(t2);
Texture2D gRoughnessTexture : register(t3);
Texture2D gAoTexture        : register(t4);
Texture2D gInteractionMap   : register(t5);

StructuredBuffer<VegetationInstance> gInstances      : register(t6);
StructuredBuffer<uint>               gVisibleIndices : register(t7);

SamplerState gLinearSampler : register(s0);
SamplerState gClampSampler  : register(s1);

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
    float2 TexCoord : TEXCOORD;
};

PSInput VSMain(VSInput input, uint instanceId : SV_InstanceID)
{
    const uint instanceIndex = gVisibleIndices[gInstanceOffset + instanceId] & 0x00FFFFFFu;
    const VegetationInstance instance = gInstances[instanceIndex];

    VegetationWind wind;
    wind.DirectionRadians = gWindParams0.x;
    wind.Strength         = gWindParams0.y;
    wind.GustAmplitude    = gWindParams0.z;
    wind.GustFrequency    = gWindParams0.w;
    wind.GustWavelength   = gWindParams1.x;
    wind.BendScale        = gWindParams1.y;
    wind.FlutterFrequency = gWindParams1.z;
    wind.Enabled          = gWindParams1.w;

    VegetationBendParams bendParams;
    bendParams.Model                = gBendModel;
    bendParams.WindInfluence        = gWindInfluence;
    bendParams.Stiffness            = gStiffness;
    bendParams.FlutterAmount        = gFlutterAmount;
    bendParams.InteractionInfluence = gInteractionInfluence;

    float2 push = float2(0.0f, 0.0f);
    if (gBendModel == VEGETATION_BEND_GRASS)
    {
        push = VegetationSampleInteraction(
            gInteractionMap, gClampSampler,
            instance.Position,
            gInteractionParams.xy,
            gInteractionParams.z);
    }

    const float3 bentLocal = VegetationApplyBend(
        input.Position, input.Normal, input.Color,
        instance.Position, push, gTime, wind, bendParams);

    const float3 worldPosition = VegetationLocalToWorld(bentLocal, instance);

    PSInput output;
    output.Position = mul(float4(worldPosition, 1.0f), gViewProj);
    output.TexCoord = input.TexCoord;
    return output;
}

void PSMain(PSInput input)
{
    const float alpha = gBaseColorTexture.Sample(gLinearSampler, input.TexCoord).a;
    clip(alpha - gAlphaCutoff);
}
