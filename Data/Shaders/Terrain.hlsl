// Terrain.hlsl
// Geometry pass for terrain patches generated from a heightmap.
// Writes the same G-Buffer layout as GBuffer.hlsl (albedo, normal, material)
// so the deferred lighting pass can shade it identically to mesh entities.
// The root signature is terrain-specific (smaller than EntityMeshRenderer's):
//   b0 VS  - MVP + Model
//   b1 PS  - base colour tint and material parameters
//   t0 PS  - base-colour texture (sampled with s0 anisotropic wrap)

#include "SurfaceSpecular.hlsli"

cbuffer TerrainConstants : register(b0)
{
    float4x4 gMVP;    // pre-transposed model-view-projection
    float4x4 gModel;  // pre-transposed model-to-world
};

cbuffer TerrainMaterial : register(b1)
{
    float4 gBaseTint;          // multiplied with the base-colour texture
    float  gMetallic;
    float  gRoughness;
    float  gAoStrength;
    int    gLayerCount;        // 0 = legacy single-material; >0 = splat blend
    float4 gLayerTileScale;    // per-layer UV tiling (x,y,z,w = layers 0..3)
    int4   gLayerHasTex;       // per-layer has-texture flag
    float4 gLayerTint[4];      // per-layer tint / solid colour
    int    gHasBaseMap;        // legacy: 0 = use tint as solid colour
    int    gUseVertexColour;   // legacy: 0/1
    float  gSpecular;          // reflectivity, 0.5 = neutral (SurfaceSpecular.hlsli)
    int    _Pad0;
};

// Layer textures.  In legacy (gLayerCount == 0) mode only gLayer0 is used and
// it holds the material's base-colour map.  In paint mode gLayer0..3 hold the
// per-layer diffuse textures blended by the vertex-colour splat weights.
Texture2D    gLayer0 : register(t0);
Texture2D    gLayer1 : register(t1);
Texture2D    gLayer2 : register(t2);
Texture2D    gLayer3 : register(t3);
SamplerState gLinearSampler : register(s0);

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

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.Position      = mul(float4(input.Position, 1.0f), gMVP);
    output.WorldPosition = mul(float4(input.Position, 1.0f), gModel).xyz;
    output.WorldNormal   = normalize(mul(float4(input.Normal, 0.0f), gModel).xyz);
    output.TexCoord      = input.TexCoord;
    output.Color         = input.Color;
    return output;
}

PSOutput PSMain(PSInput input)
{
    PSOutput output;

    // --- Albedo ---
    float3 baseAlbedo;
    if (gLayerCount > 0)
    {
        // CryEngine-style splat blend: input.Color holds the four per-layer
        // weights (r,g,b,a = layers 0..3).  Each layer samples its own tiled
        // texture and the results are combined by the (renormalised) weights.
        float4 w = input.Color;

        float3 l0 = (gLayerHasTex.x != 0) ? gLayer0.Sample(gLinearSampler, input.TexCoord * gLayerTileScale.x).rgb : float3(1,1,1);
        float3 l1 = (gLayerHasTex.y != 0) ? gLayer1.Sample(gLinearSampler, input.TexCoord * gLayerTileScale.y).rgb : float3(1,1,1);
        float3 l2 = (gLayerHasTex.z != 0) ? gLayer2.Sample(gLinearSampler, input.TexCoord * gLayerTileScale.z).rgb : float3(1,1,1);
        float3 l3 = (gLayerHasTex.w != 0) ? gLayer3.Sample(gLinearSampler, input.TexCoord * gLayerTileScale.w).rgb : float3(1,1,1);

        l0 *= gLayerTint[0].rgb;
        l1 *= gLayerTint[1].rgb;
        l2 *= gLayerTint[2].rgb;
        l3 *= gLayerTint[3].rgb;

        float wsum = w.x + w.y + w.z + w.w;
        float3 blended = l0 * w.x + l1 * w.y + l2 * w.z + l3 * w.w;
        blended = (wsum > 1e-4f) ? (blended / wsum) : l0;

        baseAlbedo = blended * gBaseTint.rgb;
    }
    else
    {
        // Legacy single-material path (gLayer0 == base-colour map).
        float3 baseTexture = gHasBaseMap
            ? gLayer0.Sample(gLinearSampler, input.TexCoord).rgb
            : float3(1.0f, 1.0f, 1.0f);

        baseAlbedo = baseTexture * gBaseTint.rgb;
        if (gUseVertexColour != 0)
        {
            baseAlbedo *= input.Color.rgb;
        }
    }
    output.Albedo = float4(baseAlbedo, gBaseTint.a);

    // --- Normal (oct-encoded world normal) ---
    const float3 N = normalize(input.WorldNormal);
    const float2 octNormal = EncodeOctNormal(N);
    output.Normal = float4(octNormal, input.Position.z, PteroEncodeSurfaceSpecular(gSpecular));

    // --- Material ---
    output.Material = float4(
        saturate(gRoughness),
        saturate(gMetallic),
        saturate(gAoStrength),
        0.0f);

    return output;
}
