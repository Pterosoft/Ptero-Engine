// VegetationBillboard.hlsl
// Farthest vegetation LOD: a single camera-facing card per instance.
//
// A distant tree occupies a handful of pixels but still costs thousands of
// triangles as a mesh, so past a certain range the cheapest correct answer is
// two triangles with a picture of the tree on them.
//
// The card only receives the main trunk bend, not the branch sway or leaf
// flutter.  That is deliberate: at billboard range the high-frequency terms
// are invisible, but if the card did not lean at all, trees would visibly snap
// upright as they crossed the LOD boundary on a windy day.

#include "Vegetation_Common.hlsli"

cbuffer VegetationPassConstants : register(b0)
{
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
    // Card half-width and height in mesh-local units, derived from the source
    // mesh's bounds so the billboard matches the geometry it replaces.
    float  gBillboardHalfWidth;
    float  gBillboardHeight;
    float  _LayerPad0;
};

// The billboard card's own texture, bound in place of the mesh base colour.
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

// The quad's four corners arrive as a unit card in the XY plane, spanning
// -0.5..0.5 in X and 0..1 in Y, so Y already reads as height above the base.
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

PSInput VSMain(VSInput input, uint instanceId : SV_InstanceID)
{
    const uint packed = gVisibleIndices[gInstanceOffset + instanceId];
    const uint instanceIndex = packed & 0x00FFFFFFu;
    const float fade = (float)(packed >> 24) * (1.0f / 255.0f);

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

    // Build the card in world space, facing the camera but keeping its up axis
    // vertical.  A fully camera-aligned card would roll as the camera pitched,
    // which reads as trees tipping over.
    const float3 up = normalize(instance.UpAxis);
    float3 toCamera = gCameraPosition - instance.Position;
    toCamera -= up * dot(toCamera, up);

    const float toCameraLength = length(toCamera);
    const float3 facing = (toCameraLength > 1e-4f) ? (toCamera / toCameraLength) : float3(1.0f, 0.0f, 0.0f);
    const float3 right = normalize(cross(up, facing));

    const float halfWidth = gBillboardHalfWidth * instance.Scale * 2.0f;
    const float height    = gBillboardHeight    * instance.Scale * 2.0f;

    // input.Position.x spans -0.5..0.5, input.Position.y spans 0..1.
    float3 worldPosition = instance.Position
                         + right * (input.Position.x * halfWidth * 2.0f)
                         + up    * (input.Position.y * height);

    // Main bend only, hinged at the base, so the card leans with the wind the
    // same way the mesh LODs do.
    const float speed = VegetationWindSpeed(instance.Position, gTime, wind);
    const float drive = speed * gWindInfluence * wind.BendScale / max(gStiffness, 0.01f);
    const float2 windDirection = VegetationWindDirection(wind);

    // Height along the card doubles as the trunk bend weight, matching the
    // vertex-colour alpha convention the mesh path uses.
    const float bendWeight = input.Position.y * input.Position.y;
    worldPosition.xy += windDirection * bendWeight * drive * 0.02f * height;

    PSInput output;
    output.Position      = mul(float4(worldPosition, 1.0f), gViewProj);
    output.WorldPosition = worldPosition;
    // Face the camera.  A flat card has no real normal, and anything more
    // elaborate would not survive the distance this is drawn at.
    output.WorldNormal   = facing;
    output.TexCoord      = input.TexCoord;
    output.Fade          = fade;
    return output;
}

static const float kBayer4x4[16] =
{
    0.0f / 16.0f,  8.0f / 16.0f,  2.0f / 16.0f, 10.0f / 16.0f,
    12.0f / 16.0f, 4.0f / 16.0f, 14.0f / 16.0f,  6.0f / 16.0f,
    3.0f / 16.0f, 11.0f / 16.0f,  1.0f / 16.0f,  9.0f / 16.0f,
    15.0f / 16.0f, 7.0f / 16.0f, 13.0f / 16.0f,  5.0f / 16.0f
};

PSOutput PSMain(PSInput input)
{
    PSOutput output;

    const float4 texColor = gBaseColorTexture.Sample(gLinearSampler, input.TexCoord);
    clip(texColor.a - gAlphaCutoff);

    if (input.Fade < 1.0f)
    {
        const uint2 pixel = uint2(input.Position.xy) & 3;
        clip(input.Fade - kBayer4x4[pixel.y * 4 + pixel.x]);
    }

    const float3 N = normalize(input.WorldNormal);

    output.Albedo   = float4(texColor.rgb * gBaseColorTint.rgb, texColor.a * gBaseColorTint.a);
    output.Normal   = float4(EncodeOctNormal(N), input.Position.z, 0.0f);
    // No material maps at billboard range; the scalar factors are all the
    // lighting needs to keep the card consistent with the mesh LODs.
    output.Material = float4(gRoughnessFactor, gMetallicFactor, 1.0f, 0.0f);

    return output;
}
