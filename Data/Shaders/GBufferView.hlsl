// GBufferView.hlsl
// The viewport's G-Buffer view modes (View Mode > G-Buffer): one channel of the
// G-Buffer, decoded and drawn over the whole frame in place of the lit image, or every
// channel at once in a grid (the overview).
//
// Runs after the post chain into its own target, so exposure, tonemapping, bloom and
// TAA never touch what is shown: a roughness of 0.5 reads as 50% grey. Colour data
// (base colour) is encoded to sRGB for display; everything else is shown as stored.
//
// Root signature (GBufferViewRenderer):
//   param 0 : CBV  b0      GBufferViewConstants
//   param 1 : SRV  t0      albedo (RGB base colour, A opacity)
//   param 2 : SRV  t1      normal (XY oct normal, Z hw depth, W packed specular)
//   param 3 : SRV  t2      material (R roughness, G metallic, B AO)
//   param 4 : SRV  t3..t4  Disney principled targets
//   param 5 : SRV  t5      motion vectors (UV-space, current - previous)

#include "PrincipledBrdf.hlsli"

cbuffer GBufferViewConstants : register(b0)
{
    float4x4 gInvViewProj;   // transposed inverse view-projection, as DeferredLighting
    float3   gCameraPos;
    int      gView;          // GBufferView in GBufferViewRenderer.h
    uint     gWidth;
    uint     gHeight;
    uint     gHasMotion;
    uint     _Pad;
};

Texture2D<float4> gAlbedo      : register(t0);
Texture2D<float4> gNormal      : register(t1);
Texture2D<float4> gMaterial    : register(t2);
Texture2D<float4> gPrincipled0 : register(t3);
Texture2D<float4> gPrincipled1 : register(t4);
Texture2D<float2> gMotion      : register(t5);

// Keep in step with GBufferView in GBufferViewRenderer.h.
static const int kViewBaseColor         = 1;
static const int kViewWorldNormal       = 2;
static const int kViewRoughness         = 3;
static const int kViewMetallic          = 4;
static const int kViewMaterialAo        = 5;
static const int kViewSpecular          = 6;
static const int kViewOpacity           = 7;
static const int kViewSceneDepth        = 8;
static const int kViewWorldPosition     = 9;
static const int kViewMotionVectors     = 10;
static const int kViewSubsurfaceProfile = 11;
static const int kViewFoliage           = 12;
static const int kViewDisneySubsurface  = 13;
static const int kViewSpecularTint      = 14;
static const int kViewSheen             = 15;
static const int kViewSheenTint         = 16;
static const int kViewClearcoat         = 17;
static const int kViewClearcoatGloss    = 18;
static const int kViewAnisotropy        = 19;
static const int kViewCount             = 20;
static const int kViewOverview          = 100;
static const int kOverviewColumns       = 5;

struct PSInput
{
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

PSInput VSMain(uint vertexId : SV_VertexID)
{
    // One triangle over the whole target, as DeferredLighting.hlsl.
    PSInput output;
    output.TexCoord = float2(vertexId == 2 ? 2.0f : 0.0f, vertexId == 1 ? 2.0f : 0.0f);
    output.Position = float4(output.TexCoord.x * 2.0f - 1.0f, 1.0f - output.TexCoord.y * 2.0f, 0.0f, 1.0f);
    return output;
}

float3 DecodeOctNormal(float2 encoded)
{
    float2 oct = encoded * 2.0f - 1.0f;
    float3 n = float3(oct, 1.0f - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0f)
        n.xy = (1.0f - abs(n.yx)) * (float2(n.xy >= 0.0f) * 2.0f - 1.0f);
    return normalize(n);
}

float3 LinearToSrgb(float3 c)
{
    c = saturate(c);
    return lerp(1.055f * pow(c, 1.0f / 2.4f) - 0.055f, c * 12.92f, float3(c <= 0.0031308f));
}

// A distinct, stable colour per subsurface profile slot.
float3 SlotColor(uint slot)
{
    const float hue = frac(float(slot) * 0.618034f);
    return saturate(abs(frac(hue + float3(0.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - 3.0f) - 1.0f) * 0.8f + 0.2f;
}

float3 Shade(int view, uint2 pixel, float2 uv)
{
    const float4 normalSample = gNormal.Load(int3(pixel, 0));
    const float  depth = normalSample.z;
    // Nothing was drawn here: the sky, or past the far plane.
    if (depth <= 0.0f || depth >= 1.0f)
        return float3(0.04f, 0.04f, 0.05f);

    const float4 albedo   = gAlbedo.Load(int3(pixel, 0));
    const float4 material = gMaterial.Load(int3(pixel, 0));
    const float3 n = DecodeOctNormal(normalSample.xy);

    const float4 ndc = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
    const float4 world = mul(ndc, gInvViewProj);
    const float3 worldPos = world.xyz / world.w;

    switch (view)
    {
    case kViewBaseColor:     return LinearToSrgb(albedo.rgb);
    case kViewWorldNormal:   return n * 0.5f + 0.5f;
    case kViewRoughness:     return material.rrr;
    case kViewMetallic:      return material.ggg;
    case kViewMaterialAo:    return material.bbb;
    case kViewSpecular:      return PteroDecodeSurfaceSpecular(normalSample.w).xxx;
    case kViewOpacity:       return albedo.aaa;
    case kViewSceneDepth:
    {
        // Log-scaled distance: white at the camera, black a kilometre out, so both a
        // room and a landscape keep their detail.
        const float distance = length(worldPos - gCameraPos);
        return (1.0f - saturate(log2(distance + 1.0f) / log2(1001.0f))).xxx;
    }
    case kViewWorldPosition: return frac(worldPos);   // repeats every metre
    case kViewMotionVectors:
    {
        if (gHasMotion == 0u)
            return float3(0.0f, 0.0f, 0.0f);
        // Per-axis speed, a full channel at a fiftieth of the screen per frame.
        const float2 motion = gMotion.Load(int3(pixel, 0));
        return float3(saturate(abs(motion) * 50.0f), 0.0f);
    }
    case kViewSubsurfaceProfile:
    {
        const uint slot = PteroDecodeSubsurfaceSlot(normalSample.w);
        return slot != 0u ? SlotColor(slot) : float3(0.0f, 0.0f, 0.0f);
    }
    case kViewFoliage:       return PteroIsFoliageSurface(normalSample.w) ? float3(0.2f, 0.9f, 0.2f) : float3(0.0f, 0.0f, 0.0f);
    default:
        break;
    }

    const PteroPrincipled p = PteroDecodePrincipled(gPrincipled0.Load(int3(pixel, 0)), gPrincipled1.Load(int3(pixel, 0)), n);
    switch (view)
    {
    case kViewDisneySubsurface: return p.subsurface.xxx;
    case kViewSpecularTint:     return p.specularTint.xxx;
    case kViewSheen:            return p.sheen.xxx;
    case kViewSheenTint:        return p.sheenTint.xxx;
    case kViewClearcoat:        return p.clearcoat.xxx;
    case kViewClearcoatGloss:   return p.clearcoatGloss.xxx;
    case kViewAnisotropy:       return p.anisotropic.xxx;
    default:                    return float3(1.0f, 0.0f, 1.0f);
    }
}

float4 PSMain(PSInput input) : SV_Target
{
    const float2 size = float2(gWidth, gHeight);
    float2 uv = input.Position.xy / size;
    int view = gView;

    if (view == kViewOverview)
    {
        // Every channel in a grid, each tile the whole frame scaled down. The editor
        // draws the tile names over it from the same layout.
        const int tileCount = kViewCount - 1;
        const int rows = (tileCount + kOverviewColumns - 1) / kOverviewColumns;
        const float2 grid = float2(kOverviewColumns, rows);
        const int2 tile = int2(uv * grid);
        const int index = tile.y * kOverviewColumns + tile.x;
        if (index >= tileCount)
            return float4(0.0f, 0.0f, 0.0f, 1.0f);
        const float2 local = frac(uv * grid);
        // A thin gap between tiles.
        const float2 gap = 2.0f * grid / size;
        if (any(local < gap))
            return float4(0.0f, 0.0f, 0.0f, 1.0f);
        uv = local;
        view = index + 1;
    }

    const uint2 pixel = min(uint2(uv * size), uint2(gWidth - 1u, gHeight - 1u));
    return float4(Shade(view, pixel, (float2(pixel) + 0.5f) / size), 1.0f);
}
