// VegetationMotion.hlsl
// Per-pixel motion vectors for instanced vegetation.
//
// Without this pass, TAA and DLSS reproject foliage using only camera motion.
// The wind bend would then be invisible to the temporal filter, so every leaf
// would be compared against whatever was at its previous *screen* position,
// and the whole canopy would smear into a blur as soon as the wind picked up.
// The fix is to evaluate the bend twice -- once at this frame's time and once
// at the previous frame's -- and report the difference.
//
// This is why the bend lives in Vegetation_Common.hlsli: the function called
// here must be bit-for-bit the one the colour pass used, or the motion vectors
// would describe a movement that never happened.

#include "Vegetation_Common.hlsli"

cbuffer VegetationPassConstants : register(b0)
{
    float4x4 gViewProj;
    float3   gCameraPosition;    float gTime;
    float4   gWindParams0;
    float4   gWindParams1;
    // .w carries the previous frame's time.
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

struct VSOutput
{
    float4 Position        : SV_Position;
    float4 CurrentClipPos  : TEXCOORD0;
    float4 PreviousClipPos : TEXCOORD1;
    float2 TexCoord        : TEXCOORD2;
};

VegetationWind UnpackWind()
{
    VegetationWind wind;
    wind.DirectionRadians = gWindParams0.x;
    wind.Strength         = gWindParams0.y;
    wind.GustAmplitude    = gWindParams0.z;
    wind.GustFrequency    = gWindParams0.w;
    wind.GustWavelength   = gWindParams1.x;
    wind.BendScale        = gWindParams1.y;
    wind.FlutterFrequency = gWindParams1.z;
    wind.Enabled          = gWindParams1.w;
    return wind;
}

VegetationBendParams UnpackBendParams()
{
    VegetationBendParams params;
    params.Model                = gBendModel;
    params.WindInfluence        = gWindInfluence;
    params.Stiffness            = gStiffness;
    params.FlutterAmount        = gFlutterAmount;
    params.InteractionInfluence = gInteractionInfluence;
    return params;
}

VSOutput VSMain(VSInput input, uint instanceId : SV_InstanceID)
{
    const uint instanceIndex = gVisibleIndices[gInstanceOffset + instanceId] & 0x00FFFFFFu;
    const VegetationInstance instance = gInstances[instanceIndex];

    const VegetationWind       wind       = UnpackWind();
    const VegetationBendParams bendParams = UnpackBendParams();

    float2 push = float2(0.0f, 0.0f);
    if (gBendModel == VEGETATION_BEND_GRASS)
    {
        push = VegetationSampleInteraction(
            gInteractionMap, gClampSampler,
            instance.Position,
            gInteractionParams.xy,
            gInteractionParams.z);
    }

    // The bend at this frame's time.
    const float3 currentLocal = VegetationApplyBend(
        input.Position, input.Normal, input.Color,
        instance.Position, push, gTime, wind, bendParams);

    // The same bend one frame earlier.  The interaction push is reused rather
    // than kept as history: the map already decays smoothly, so the error is
    // far smaller than the cost of a second ping-pong buffer would justify.
    const float previousTime = gInteractionParams.w;
    const float3 previousLocal = VegetationApplyBend(
        input.Position, input.Normal, input.Color,
        instance.Position, push, previousTime, wind, bendParams);

    // Instances never move between frames -- the scatter only changes on an
    // edit -- so the same instance transform applies to both samples.
    const float3 currentWorld  = VegetationLocalToWorld(currentLocal,  instance);
    const float3 previousWorld = VegetationLocalToWorld(previousLocal, instance);

    VSOutput output;
    output.CurrentClipPos  = mul(float4(currentWorld,  1.0f), gViewProj);
    output.PreviousClipPos = mul(float4(previousWorld, 1.0f), gPrevViewProj);
    output.Position        = output.CurrentClipPos;
    output.TexCoord        = input.TexCoord;
    return output;
}

float2 ClipToUv(float4 clipPos)
{
    const float invW = rcp(max(abs(clipPos.w), 1e-6f));
    float2 uv = clipPos.xy * invW;
    uv.y = -uv.y;
    return uv * 0.5f + 0.5f;
}

float4 PSMain(VSOutput input) : SV_Target
{
    // The same alpha test as the colour pass, so a cut-out leaf does not write
    // motion vectors for the transparent part of its card.
    const float alpha = gBaseColorTexture.Sample(gLinearSampler, input.TexCoord).a;
    clip(alpha - gAlphaCutoff);

    const float2 currentUv  = ClipToUv(input.CurrentClipPos);
    const float2 previousUv = ClipToUv(input.PreviousClipPos);
    return float4(currentUv - previousUv, 0.0f, 0.0f);
}
