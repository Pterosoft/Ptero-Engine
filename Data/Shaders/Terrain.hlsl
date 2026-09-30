// Terrain.hlsl
// Geometry pass for terrain patches generated from a heightmap.
// Writes the same G-Buffer layout as GBuffer.hlsl (albedo, normal, material)
// so the deferred lighting pass can shade it identically to mesh entities.
//
// The surface is up to four material layers blended by a painted splat map
// (the vertex colour carries the four weights).  A terrain with no paint layers
// is drawn as one implicit layer holding the terrain's own material.  Each layer
// is a full material: base colour, normal, roughness / metallic / AO (or packed)
// and height, which drives both height-based blending and tessellated
// displacement.
//
// Root signature (terrain-specific):
//   b0 ALL - MVP + Model, camera data for tessellation
//   b1 ALL - blend + tessellation settings, per-layer material parameters
//   t0..t3   base colour     (layer 0..3)
//   t4..t7   normal
//   t8..t11  roughness, or packed metallic-roughness
//   t12..t15 metallic
//   t16..t19 ambient occlusion
//   t20..t23 height / displacement (also read by the domain shader)
//
// Two pipelines share this file: VSMain -> PSMain, and when a layer's material
// enables tessellation VSMainTess -> HSMain -> DSMain -> PSMain.

#include "SurfaceSpecular.hlsli"
#include "Tessellation.hlsli"
#include "TangentFrame.hlsli"

static const int kTerrainLayers = 4;

cbuffer TerrainConstants : register(b0)
{
    float4x4 gMVP;    // pre-transposed model-view-projection
    float4x4 gModel;  // pre-transposed model-to-world
    float3   gCameraPositionWS;
    float    gTessPixelScale;   // (viewport height / 2) / tan(fovY / 2)
};

struct TerrainLayer
{
    float4 BaseTint;
    // Layer UV: local XY in metres / TileSize, then the material's own
    // rotate-about-0.5 / tiling / offset transform (as on meshes).
    float2 UvTiling;
    float2 UvOffset;
    float  UvRotationSin;
    float  UvRotationCos;
    float  TileSize;
    float  NormalScale;
    float  Roughness;
    float  Metallic;
    float  AoStrength;
    float  Specular;
    int    HasBaseMap;
    int    HasNormalMap;
    int    HasRoughnessMap;
    int    HasMetallicMap;
    int    HasAoMap;
    int    HasPackedMaterialMap;   // roughness slot holds one packed map: 1 = RMA (R rough, G metal, B AO), 2 = ORM (R AO, G rough, B metal)
    int    HasHeightMap;
    int    FlipNormalGreen;
    float  DisplacementScale;      // metres; 0 when the layer's material does not tessellate
    float  DisplacementMidLevel;
    float2 _Pad0;
    float4 _Pad1;
};

cbuffer TerrainMaterial : register(b1)
{
    int    gLayerCount;            // 1..4
    int    gUseSplat;              // 0 = single implicit layer, vertex colour is not weights
    int    gBreakUpTiling;         // blend randomly offset copies to hide repetition
    int    gUseTessellation;
    int    gHeightBlend;
    float  gHeightBlendSharpness;  // 0 soft .. 1 hard
    float  gMaxDisplacement;       // largest displacement of any layer, for patch culling
    float  _HeaderPad0;
    float  gTessMaxFactor;
    float  gTessTargetPixels;
    float  gTessFadeDistance;
    float  _HeaderPad1;
    float4 _HeaderPad2;
    TerrainLayer gLayers[kTerrainLayers];
};

// Individual textures, not Texture2D arrays: the root signature binds every
// texture through its own single-descriptor table, and D3D12 requires a shader
// array to lie inside one descriptor range - an array split across tables fails
// PSO creation ("not fully bound in root signature") and the terrain vanishes.
// SampleLayerMap() and friends select one by (map, layer); with the literal
// indices the unrolled loops pass, the switch folds away at compile time.
Texture2D    gLayerBase0      : register(t0);
Texture2D    gLayerBase1      : register(t1);
Texture2D    gLayerBase2      : register(t2);
Texture2D    gLayerBase3      : register(t3);
Texture2D    gLayerNormal0    : register(t4);
Texture2D    gLayerNormal1    : register(t5);
Texture2D    gLayerNormal2    : register(t6);
Texture2D    gLayerNormal3    : register(t7);
Texture2D    gLayerRoughness0 : register(t8);
Texture2D    gLayerRoughness1 : register(t9);
Texture2D    gLayerRoughness2 : register(t10);
Texture2D    gLayerRoughness3 : register(t11);
Texture2D    gLayerMetallic0  : register(t12);
Texture2D    gLayerMetallic1  : register(t13);
Texture2D    gLayerMetallic2  : register(t14);
Texture2D    gLayerMetallic3  : register(t15);
Texture2D    gLayerAo0        : register(t16);
Texture2D    gLayerAo1        : register(t17);
Texture2D    gLayerAo2        : register(t18);
Texture2D    gLayerAo3        : register(t19);
Texture2D    gLayerHeight0    : register(t20);
Texture2D    gLayerHeight1    : register(t21);
Texture2D    gLayerHeight2    : register(t22);
Texture2D    gLayerHeight3    : register(t23);
SamplerState gLinearSampler   : register(s0);

static const int kMapBase      = 0;
static const int kMapNormal    = 1;
static const int kMapRoughness = 2;   // or packed metallic-roughness
static const int kMapMetallic  = 3;
static const int kMapAo        = 4;
static const int kMapHeight    = 5;

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
    float2 LocalXY       : TEXCOORD0;   // metres on the patch; every layer UV derives from it
    float4 Color         : COLOR;       // splat weights (layers 0..3)
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

// Metres on the patch -> layer UV.  Local (not world) XY, so moving the terrain
// entity carries its texture with it.
float2 LayerUv(int layer, float2 localXY)
{
    const TerrainLayer L = gLayers[layer];
    const float2 uv = localXY / max(L.TileSize, 1e-3f);
    const float2 centered = uv - 0.5f;
    const float2 rotated = float2(
        centered.x * L.UvRotationCos - centered.y * L.UvRotationSin,
        centered.x * L.UvRotationSin + centered.y * L.UvRotationCos) + 0.5f;
    return rotated * L.UvTiling + L.UvOffset;
}

// Splat weights for this point: the painted weights (or layer 0 alone),
// restricted to the layers that exist and renormalised.
float4 BaseLayerWeights(float4 vertexColour)
{
    float4 w = (gUseSplat != 0) ? max(vertexColour, 0.0f) : float4(1.0f, 0.0f, 0.0f, 0.0f);
    w *= float4(gLayerCount > 0, gLayerCount > 1, gLayerCount > 2, gLayerCount > 3);
    const float sum = dot(w, 1.0f);
    return (sum > 1e-5f) ? w / sum : float4(1.0f, 0.0f, 0.0f, 0.0f);
}

// Height-based blending: where layers overlap, the one whose surface is higher
// wins, over a band that narrows as the sharpness rises.  Layers with no weight
// never compete, so a tall layer cannot appear where it was not painted.
float4 ApplyHeightBlend(float4 w, float4 heights)
{
    if (gHeightBlend == 0 || gUseSplat == 0)
        return w;
    const float band = lerp(0.5f, 0.02f, saturate(gHeightBlendSharpness));
    const float4 score = select(w > 1e-4f, heights + w, -1e4f);
    const float top = max(max(score.x, score.y), max(score.z, score.w)) - band;
    const float4 b = max(score - top, 0.0f);
    const float sum = dot(b, 1.0f);
    return (sum > 1e-5f) ? b / sum : w;
}

// ---------------------------------------------------------------------------
// Tiling break-up (after Inigo Quilez, "texture repetition", technique 3).
// A low-frequency noise picks one of eight random offsets per region and blends
// to the next one across a soft band, so the same tile never lines up with its
// neighbours.  Pure translations: the tangent frame is unaffected, and every map
// of a layer uses the same offsets and weight, so they stay in register.
// ---------------------------------------------------------------------------

float Hash21(float2 p)
{
    p = frac(p * float2(123.34f, 456.21f));
    p += dot(p, p + 45.32f);
    return frac(p.x * p.y);
}

float ValueNoise(float2 p)
{
    const float2 i = floor(p);
    const float2 f = frac(p);
    const float2 u = f * f * (3.0f - 2.0f * f);
    return lerp(lerp(Hash21(i),                      Hash21(i + float2(1.0f, 0.0f)), u.x),
                lerp(Hash21(i + float2(0.0f, 1.0f)), Hash21(i + float2(1.0f, 1.0f)), u.x),
                u.y);
}

struct TiledUv
{
    float2 UvA;
    float2 UvB;
    float  Blend;   // 0 = UvA only
    float2 Ddx;
    float2 Ddy;
};

TiledUv MakeTiledUv(float2 uv, float2 uvDdx, float2 uvDdy)
{
    TiledUv t;
    t.UvA = uv;
    t.UvB = uv;
    t.Blend = 0.0f;
    t.Ddx = uvDdx;
    t.Ddy = uvDdy;
    if (gBreakUpTiling != 0)
    {
        // One noise cell spans about eight tiles.
        const float index = ValueNoise(uv * 0.125f) * 8.0f;
        const float i = floor(index);
        t.UvA = uv + sin(float2(3.0f, 7.0f) * i);
        t.UvB = uv + sin(float2(3.0f, 7.0f) * (i + 1.0f));
        t.Blend = smoothstep(0.2f, 0.8f, index - i);
    }
    return t;
}

float4 SampleTiled(Texture2D tex, TiledUv t)
{
    // Explicit gradients: the offsets are piecewise constant, so hardware derivatives
    // would spike at every offset boundary and pick the smallest mip there.
    const float4 a = tex.SampleGrad(gLinearSampler, t.UvA, t.Ddx, t.Ddy);
    if (t.Blend <= 0.0f)
        return a;
    const float4 b = tex.SampleGrad(gLinearSampler, t.UvB, t.Ddx, t.Ddy);
    return lerp(a, b, t.Blend);
}

// Same break-up as SampleTiled, at an explicit mip (hull/domain stages have no
// derivatives).
float SampleTiledLevel(Texture2D tex, float2 uv, float mip)
{
    float2 uvA = uv;
    float2 uvB = uv;
    float blend = 0.0f;
    if (gBreakUpTiling != 0)
    {
        const float index = ValueNoise(uv * 0.125f) * 8.0f;
        const float i = floor(index);
        uvA = uv + sin(float2(3.0f, 7.0f) * i);
        uvB = uv + sin(float2(3.0f, 7.0f) * (i + 1.0f));
        blend = smoothstep(0.2f, 0.8f, index - i);
    }
    const float a = tex.SampleLevel(gLinearSampler, uvA, mip).r;
    if (blend <= 0.0f)
        return a;
    return lerp(a, tex.SampleLevel(gLinearSampler, uvB, mip).r, blend);
}

float4 SampleLayerMap(int map, int layer, TiledUv t)
{
    switch (map * kTerrainLayers + layer)
    {
    case  0: return SampleTiled(gLayerBase0,      t);
    case  1: return SampleTiled(gLayerBase1,      t);
    case  2: return SampleTiled(gLayerBase2,      t);
    case  3: return SampleTiled(gLayerBase3,      t);
    case  4: return SampleTiled(gLayerNormal0,    t);
    case  5: return SampleTiled(gLayerNormal1,    t);
    case  6: return SampleTiled(gLayerNormal2,    t);
    case  7: return SampleTiled(gLayerNormal3,    t);
    case  8: return SampleTiled(gLayerRoughness0, t);
    case  9: return SampleTiled(gLayerRoughness1, t);
    case 10: return SampleTiled(gLayerRoughness2, t);
    case 11: return SampleTiled(gLayerRoughness3, t);
    case 12: return SampleTiled(gLayerMetallic0,  t);
    case 13: return SampleTiled(gLayerMetallic1,  t);
    case 14: return SampleTiled(gLayerMetallic2,  t);
    case 15: return SampleTiled(gLayerMetallic3,  t);
    case 16: return SampleTiled(gLayerAo0,        t);
    case 17: return SampleTiled(gLayerAo1,        t);
    case 18: return SampleTiled(gLayerAo2,        t);
    case 19: return SampleTiled(gLayerAo3,        t);
    case 20: return SampleTiled(gLayerHeight0,    t);
    case 21: return SampleTiled(gLayerHeight1,    t);
    case 22: return SampleTiled(gLayerHeight2,    t);
    default: return SampleTiled(gLayerHeight3,    t);
    }
}

float SampleLayerHeightLevel(int layer, float2 uv, float mip)
{
    switch (layer)
    {
    case 0:  return SampleTiledLevel(gLayerHeight0, uv, mip);
    case 1:  return SampleTiledLevel(gLayerHeight1, uv, mip);
    case 2:  return SampleTiledLevel(gLayerHeight2, uv, mip);
    default: return SampleTiledLevel(gLayerHeight3, uv, mip);
    }
}

float2 LayerHeightMapSize(int layer)
{
    float width = 1.0f;
    float height = 1.0f;
    switch (layer)
    {
    case 0:  gLayerHeight0.GetDimensions(width, height); break;
    case 1:  gLayerHeight1.GetDimensions(width, height); break;
    case 2:  gLayerHeight2.GetDimensions(width, height); break;
    default: gLayerHeight3.GetDimensions(width, height); break;
    }
    return float2(width, height);
}

// ---------------------------------------------------------------------------
// Plain pipeline
// ---------------------------------------------------------------------------

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.Position      = mul(float4(input.Position, 1.0f), gMVP);
    output.WorldPosition = mul(float4(input.Position, 1.0f), gModel).xyz;
    output.WorldNormal   = normalize(mul(float4(input.Normal, 0.0f), gModel).xyz);
    output.LocalXY       = input.Position.xy;
    output.Color         = input.Color;
    return output;
}

// ---------------------------------------------------------------------------
// Tessellated pipeline
// ---------------------------------------------------------------------------

struct TessControlPoint
{
    float3 LocalPosition : POSITION;
    float3 LocalNormal   : NORMAL;
    float4 Color         : COLOR;
    float3 WorldPosition : WORLDPOS;
    float4 ClipPosition  : CLIPPOS;
};

struct TerrainPatchConstants
{
    float Edges[3] : SV_TessFactor;
    float Inside   : SV_InsideTessFactor;
    // World length of one tessellated edge; each layer turns it into the mip of
    // its own height map in the domain shader.
    float SubEdgeMeters : SUBEDGE;
};

TessControlPoint VSMainTess(VSInput input)
{
    TessControlPoint output;
    output.LocalPosition = input.Position;
    output.LocalNormal   = input.Normal;
    output.Color         = input.Color;
    output.WorldPosition = mul(float4(input.Position, 1.0f), gModel).xyz;
    output.ClipPosition  = mul(float4(input.Position, 1.0f), gMVP);
    return output;
}

TerrainPatchConstants HSConstants(InputPatch<TessControlPoint, 3> patch)
{
    TerrainPatchConstants output;

    const float slack = gMaxDisplacement * 3.0f;
    if (PteroTessPatchOutsideFrustum(patch[0].ClipPosition, patch[1].ClipPosition, patch[2].ClipPosition, slack))
    {
        // A zero factor discards the patch.
        output.Edges[0] = output.Edges[1] = output.Edges[2] = 0.0f;
        output.Inside = 0.0f;
        output.SubEdgeMeters = 1.0f;
        return output;
    }

    float longestEdge = 0.0f;
    [unroll]
    for (int edge = 0; edge < 3; ++edge)
    {
        const float3 a = patch[(edge + 1) % 3].WorldPosition;
        const float3 b = patch[(edge + 2) % 3].WorldPosition;
        output.Edges[edge] = PteroTessEdgeFactor(
            a, b, gCameraPositionWS, gTessPixelScale, gTessTargetPixels, gTessMaxFactor, gTessFadeDistance);
        longestEdge = max(longestEdge, distance(a, b));
    }
    output.Inside = max(output.Edges[0], max(output.Edges[1], output.Edges[2]));
    output.SubEdgeMeters = longestEdge / output.Inside;
    return output;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("HSConstants")]
[maxtessfactor(64.0)]
TessControlPoint HSMain(InputPatch<TessControlPoint, 3> patch, uint pointId : SV_OutputControlPointID)
{
    return patch[pointId];
}

// Height map of one layer at an explicit mip matching the tessellation density.
float LayerHeightLevel(int layer, float2 localXY, float subEdgeMeters)
{
    const TerrainLayer L = gLayers[layer];
    const float2 size = LayerHeightMapSize(layer);
    const float texelsPerMeter = max(size.x, size.y) * max(abs(L.UvTiling.x), abs(L.UvTiling.y))
                               / max(L.TileSize, 1e-3f);
    const float mip = max(log2(max(subEdgeMeters * texelsPerMeter, 1.0f)), 0.0f);
    return SampleLayerHeightLevel(layer, LayerUv(layer, localXY), mip);
}

[domain("tri")]
PSInput DSMain(
    TerrainPatchConstants patchConstants,
    float3 barycentrics : SV_DomainLocation,
    const OutputPatch<TessControlPoint, 3> patch)
{
    float3 localPosition = patch[0].LocalPosition * barycentrics.x
                         + patch[1].LocalPosition * barycentrics.y
                         + patch[2].LocalPosition * barycentrics.z;
    const float3 localNormal = normalize(patch[0].LocalNormal * barycentrics.x
                                       + patch[1].LocalNormal * barycentrics.y
                                       + patch[2].LocalNormal * barycentrics.z);
    const float4 color = patch[0].Color * barycentrics.x
                       + patch[1].Color * barycentrics.y
                       + patch[2].Color * barycentrics.z;

    // UVs come from the undisplaced position, so textures do not slide
    // sideways as the surface moves along its normal.
    const float2 localXY = localPosition.xy;

    if (gUseTessellation != 0)
    {
        float4 heights = 0.5f;
        [unroll]
        for (int layer = 0; layer < kTerrainLayers; ++layer)
        {
            if (layer < gLayerCount && gLayers[layer].HasHeightMap != 0)
                heights[layer] = LayerHeightLevel(layer, localXY, patchConstants.SubEdgeMeters);
        }
        // Same weights the pixel shader shades with, so geometry and shading agree.
        const float4 w = ApplyHeightBlend(BaseLayerWeights(color), heights);

        float displacement = 0.0f;
        [unroll]
        for (int i = 0; i < kTerrainLayers; ++i)
            displacement += w[i] * (heights[i] - gLayers[i].DisplacementMidLevel) * gLayers[i].DisplacementScale;

        const float worldPerLocal = max(length(mul(float4(localNormal, 0.0f), gModel).xyz), 1e-6f);
        localPosition += localNormal * (displacement / worldPerLocal);
    }

    PSInput output;
    output.Position      = mul(float4(localPosition, 1.0f), gMVP);
    output.WorldPosition = mul(float4(localPosition, 1.0f), gModel).xyz;
    output.WorldNormal   = normalize(mul(float4(localNormal, 0.0f), gModel).xyz);
    output.LocalXY       = localXY;
    output.Color         = color;
    return output;
}

// ---------------------------------------------------------------------------
// Pixel shader (both pipelines)
// ---------------------------------------------------------------------------

PSOutput PSMain(PSInput input)
{
    PSOutput output;

    const float3 geometricNormal = normalize(input.WorldNormal);

    // Everything that needs derivatives happens here, before any per-pixel
    // branching: layer UVs, their gradients and tangent frames.  The branch on
    // gLayerCount is uniform, so derivatives stay valid inside it.
    TiledUv tiled[kTerrainLayers];
    float3  tangents[kTerrainLayers];
    float3  bitangents[kTerrainLayers];
    [unroll]
    for (int layer = 0; layer < kTerrainLayers; ++layer)
    {
        tiled[layer] = MakeTiledUv(0.0f.xx, 0.0f.xx, 0.0f.xx);
        tangents[layer] = float3(1.0f, 0.0f, 0.0f);
        bitangents[layer] = float3(0.0f, 1.0f, 0.0f);
        if (layer < gLayerCount)
        {
            const float2 uv = LayerUv(layer, input.LocalXY);
            tiled[layer] = MakeTiledUv(uv, ddx(uv), ddy(uv));
            float2 worldUnitsPerUv;
            BuildTangentFrame(input.WorldPosition, uv, geometricNormal,
                              tangents[layer], bitangents[layer], worldUnitsPerUv);
        }
    }

    float4 w = BaseLayerWeights(input.Color);
    if (gHeightBlend != 0 && gUseSplat != 0)
    {
        float4 heights = 0.5f;
        [unroll]
        for (int i = 0; i < kTerrainLayers; ++i)
        {
            if (w[i] > 1e-4f && gLayers[i].HasHeightMap != 0)
                heights[i] = SampleLayerMap(kMapHeight, i, tiled[i]).r;
        }
        w = ApplyHeightBlend(w, heights);
    }

    float3 albedo    = 0.0f;
    float3 normalSum = 0.0f;
    float  roughness = 0.0f;
    float  metallic  = 0.0f;
    float  ao        = 0.0f;
    float  specular  = 0.0f;

    [unroll]
    for (int l = 0; l < kTerrainLayers; ++l)
    {
        const float weight = w[l];
        if (weight <= 1e-3f)
            continue;
        const TerrainLayer L = gLayers[l];

        const float3 baseColour = (L.HasBaseMap != 0)
            ? SampleLayerMap(kMapBase, l, tiled[l]).rgb
            : float3(1.0f, 1.0f, 1.0f);
        albedo += weight * baseColour * L.BaseTint.rgb;

        float3 N = geometricNormal;
        if (L.HasNormalMap != 0)
        {
            // BC5 stores only XY; rebuild Z after remapping into tangent space.
            float2 tsNormalXY = SampleLayerMap(kMapNormal, l, tiled[l]).rg * 2.0f - 1.0f;
            if (L.FlipNormalGreen != 0)
                tsNormalXY.y = -tsNormalXY.y;
            float3 tsNormal;
            tsNormal.xy = tsNormalXY * L.NormalScale;
            tsNormal.z = sqrt(saturate(1.0f - dot(tsNormal.xy, tsNormal.xy)));
            N = normalize(tangents[l] * tsNormal.x + bitangents[l] * tsNormal.y + geometricNormal * tsNormal.z);
        }
        normalSum += weight * N;

        float layerRoughness = L.Roughness;
        float layerMetallic  = L.Metallic;
        float layerAo        = 1.0f;
        if (L.HasPackedMaterialMap != 0)
        {
            const float4 packed = SampleLayerMap(kMapRoughness, l, tiled[l]);
            const bool orm = (L.HasPackedMaterialMap == 2);
            layerRoughness *= orm ? packed.g : packed.r;
            layerMetallic  *= orm ? packed.b : packed.g;
            layerAo = lerp(1.0f, orm ? packed.r : packed.b, L.AoStrength);
        }
        else
        {
            if (L.HasRoughnessMap != 0)
                layerRoughness *= SampleLayerMap(kMapRoughness, l, tiled[l]).r;
            if (L.HasMetallicMap != 0)
                layerMetallic *= SampleLayerMap(kMapMetallic, l, tiled[l]).r;
            if (L.HasAoMap != 0)
                layerAo = lerp(1.0f, SampleLayerMap(kMapAo, l, tiled[l]).r, L.AoStrength);
        }
        roughness += weight * layerRoughness;
        metallic  += weight * layerMetallic;
        ao        += weight * layerAo;
        specular  += weight * L.Specular;
    }

    const float3 N = (dot(normalSum, normalSum) > 1e-8f) ? normalize(normalSum) : geometricNormal;

    output.Albedo   = float4(albedo, 1.0f);
    output.Normal   = float4(EncodeOctNormal(N), input.Position.z, PteroEncodeSurfaceSpecular(specular));
    output.Material = float4(saturate(roughness), saturate(metallic), saturate(ao), 0.0f);
    return output;
}
