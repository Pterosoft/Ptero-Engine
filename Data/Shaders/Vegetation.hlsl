// Vegetation.hlsl
// Geometry pass for instanced vegetation.
//
// This is a separate shader from GBuffer.hlsl rather than a variant of it,
// because foliage needs four things the opaque path deliberately does not do:
//
//   * alpha testing, so leaf cards clip to their texture instead of drawing as
//     opaque quads,
//   * two-sided rendering with a flipped normal on backfaces, so the underside
//     of a leaf is lit rather than black,
//   * a transmission term, because a backlit leaf glows and a backlit rock
//     does not,
//   * the wind and interaction bend from Vegetation_Common.hlsli.
//
// Output encoding matches GBuffer.hlsl exactly so the deferred lighting, RTGI
// and denoiser passes need no special case for vegetation.

#include "Vegetation_Common.hlsli"

cbuffer VegetationPassConstants : register(b0)
{
    float4x4 gViewProj;
    float3   gCameraPosition;    float gTime;
    // (directionRadians, strength, gustAmplitude, gustFrequency)
    float4   gWindParams0;
    // (gustWavelength, bendScale, flutterFrequency, enabled)
    float4   gWindParams1;
    // (interactionCentreX, interactionCentreY, interactionWorldSize, previousTime)
    float4   gInteractionParams;
    // Previous frame's view-projection.  Used only by VegetationMotion.hlsl,
    // but it lives in the shared block so all three vegetation passes bind one
    // identical constant buffer.
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
    float  gTranslucency;        // strength of the backlit leaf transmission
    float  gWindInfluence;
    float  gStiffness;

    float  gFlutterAmount;
    float  gInteractionInfluence;
    int    gBendModel;
    uint   gInstanceOffset;      // first instance of this layer in the shared buffer

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

// Instance data sits above the material texture slots in register space 0.
// A separate register space would read better, but spaces need shader model
// 5.1 and the startup shader cache compiles every VSMain/PSMain at 5.0; using
// one space keeps these shaders in the warm cache instead of stalling the
// first frame that draws vegetation.
StructuredBuffer<VegetationInstance> gInstances       : register(t6);
// Compacted list of visible instance indices produced by VegetationCull.hlsl.
StructuredBuffer<uint>               gVisibleIndices  : register(t7);

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
    float4 Position      : SV_Position;
    float3 WorldPosition : WORLDPOS;
    float3 WorldNormal   : NORMAL;
    float2 TexCoord      : TEXCOORD;
    float4 Color         : COLOR;
    // Distance fade, applied as screen-door dither in the pixel shader.
    float  Fade          : FADE;
};

struct PSOutput
{
    float4 Albedo   : SV_Target0;
    float4 Normal   : SV_Target1;
    float4 Material : SV_Target2;
};

float2 OctWrap(float2 v)
{
    return (1.0f - abs(v.yx)) * (float2(v.xy >= 0.0f) * 2.0f - 1.0f);
}

float2 EncodeOctNormal(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 oct = (n.z >= 0.0f) ? n.xy : OctWrap(n.xy);
    return oct * 0.5f + 0.5f;
}

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

PSInput VSMain(VSInput input, uint instanceId : SV_InstanceID)
{
    // The cull pass wrote a compacted list of surviving instances; this draw's
    // slice of it starts at gInstanceOffset.  The low 24 bits are the instance
    // index and the high byte is the quantised distance fade.
    const uint packed = gVisibleIndices[gInstanceOffset + instanceId];
    const uint instanceIndex = packed & 0x00FFFFFFu;
    const float fade = (float)(packed >> 24) * (1.0f / 255.0f);

    const VegetationInstance instance = gInstances[instanceIndex];

    const VegetationWind       wind       = UnpackWind();
    const VegetationBendParams bendParams = UnpackBendParams();

    // Grass reacts to whatever is pressing on it; trees do not, so we skip the
    // texture fetch entirely for them.
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
        input.Position,
        input.Normal,
        input.Color,
        instance.Position,
        push,
        gTime,
        wind,
        bendParams);

    const float3 worldPosition = VegetationLocalToWorld(bentLocal, instance);

    const float3x3 basis = VegetationInstanceBasis(instance.UpAxis, instance.RotationZ);

    PSInput output;
    output.Position      = mul(float4(worldPosition, 1.0f), gViewProj);
    output.WorldPosition = worldPosition;
    // Uniform scale, so the basis rotates normals correctly without an inverse
    // transpose.
    output.WorldNormal   = mul(input.Normal, basis);
    output.TexCoord      = input.TexCoord;
    output.Color         = input.Color;
    output.Fade          = fade;

    return output;
}

// Screen-door dither pattern used to fade instances in and out without needing
// alpha blending (which would force a sorted pass and break the G-Buffer).
static const float kBayer4x4[16] =
{
    0.0f / 16.0f,  8.0f / 16.0f,  2.0f / 16.0f, 10.0f / 16.0f,
    12.0f / 16.0f, 4.0f / 16.0f, 14.0f / 16.0f,  6.0f / 16.0f,
    3.0f / 16.0f, 11.0f / 16.0f,  1.0f / 16.0f,  9.0f / 16.0f,
    15.0f / 16.0f, 7.0f / 16.0f, 13.0f / 16.0f,  5.0f / 16.0f
};

PSOutput PSMain(PSInput input, bool isFrontFace : SV_IsFrontFace)
{
    PSOutput output;

    const float4 texColor = gBaseColorTexture.Sample(gLinearSampler, input.TexCoord);

    // Alpha test.  Without this the leaf cards render as opaque rectangles,
    // which is the single most visible difference between foliage and ordinary
    // opaque geometry.
    clip(texColor.a - gAlphaCutoff);

    // Distance fade as an ordered dither, so a fading instance dissolves
    // instead of popping.  TAA resolves the dither pattern into a smooth
    // gradient over a few frames.
    if (input.Fade < 1.0f)
    {
        const uint2 pixel = uint2(input.Position.xy) & 3;
        clip(input.Fade - kBayer4x4[pixel.y * 4 + pixel.x]);
    }

    float3 baseAlbedo = texColor.rgb * input.Color.rgb * gBaseColorTint.rgb;

    // --- Normal ---
    float3 N = normalize(input.WorldNormal);

    // Foliage is drawn with culling off, so half the leaf cards present their
    // back face.  Flipping the normal there is what keeps the underside of a
    // canopy from reading as unlit black.
    if (!isFrontFace)
        N = -N;

    if (gHasNormalMap)
    {
        float2 tsNormalXY = gNormalTexture.Sample(gLinearSampler, input.TexCoord).rg * 2.0f - 1.0f;
        float3 tsNormal = float3(tsNormalXY, sqrt(saturate(1.0f - dot(tsNormalXY, tsNormalXY))));
        tsNormal.xy *= gNormalScale;
        tsNormal.z = sqrt(saturate(1.0f - dot(tsNormal.xy, tsNormal.xy)));

        const float3 up = (abs(N.z) < 0.999f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
        const float3 T  = normalize(cross(up, N));
        const float3 B  = cross(N, T);
        N = normalize(T * tsNormal.x + B * tsNormal.y + N * tsNormal.z);
    }

    output.Normal = float4(EncodeOctNormal(N), input.Position.z, 0.0f);

    // --- Material ---
    const float4 metallicSample  = gMetallicTexture.Sample(gLinearSampler, input.TexCoord);
    const float4 roughnessSample = gRoughnessTexture.Sample(gLinearSampler, input.TexCoord);
    const float4 aoSample        = gAoTexture.Sample(gLinearSampler, input.TexCoord);

    const float roughness = gHasRoughnessMap ? roughnessSample.r * gRoughnessFactor : gRoughnessFactor;
    const float metallic  = gHasMetallicMap
        ? (gHasPackedMaterialMap ? metallicSample.g : metallicSample.r) * gMetallicFactor
        : gMetallicFactor;
    const float ao = gHasAoMap
        ? lerp(1.0f, gHasPackedMaterialMap ? aoSample.b : aoSample.r, gAoStrength)
        : 1.0f;

    // Leaf transmission.  A thin leaf scatters light through itself, so the
    // side facing away from the sun still carries some of its colour.  The
    // G-Buffer has no dedicated transmission channel, so this is folded into
    // albedo as a cheap approximation: it lifts the darkest side rather than
    // modelling real subsurface transport, which is enough to stop a backlit
    // canopy reading as a silhouette.
    if (gTranslucency > 0.0f)
    {
        const float3 viewDirection = normalize(gCameraPosition - input.WorldPosition);
        const float  backlight = saturate(-dot(N, viewDirection)) * gTranslucency;
        baseAlbedo = lerp(baseAlbedo, baseAlbedo * 1.6f, backlight);
    }

    output.Albedo   = float4(baseAlbedo, texColor.a * gBaseColorTint.a);
    output.Material = float4(roughness, metallic, ao, 0.0f);

    return output;
}

// The depth-only variant used by the shadow passes lives in
// VegetationShadow.hlsl, so its entry points are named VSMain/PSMain and get
// picked up by the startup shader cache warmup along with everything else.
