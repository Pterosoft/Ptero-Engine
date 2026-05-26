// RtGI_Common.hlsli
// Shared types, root-signature binding, and utility functions used by all four
// RTGI compute shader passes.

#ifndef RTGI_COMMON_HLSLI
#define RTGI_COMMON_HLSLI

#define MAX_RTGI_POINT_LIGHTS 16

struct RtgiPointLightData
{
    float3 Position;
    float  Radius;
    float3 Color;
    float  InvRadiusSq;
    float  FalloffExponent;
    float  SourceRadius;
    float  CastShadows;
    float  _Pad0;
};

// ─── Root signature (matches RtGlobalIllumination::CreateRootSignature) ───────
// [0] CBV  b0  – RtGIConstants
// [1] SRV  t0  – input slot A  (albedo / current reservoir / GI output)
// [2] SRV  t1  – input slot B  (normal+depth / history reservoir / accum history)
// [3] UAV  u0  – output slot A (reservoir write / accum write)
// [4] UAV  u1  – output slot B (GI output / final output)
// ─────────────────────────────────────────────────────────────────────────────

// ─── Constant buffer ─────────────────────────────────────────────────────────
cbuffer RtGIConstants : register(b0)
{
    uint   g_FrameWidth;
    uint   g_FrameHeight;
    uint   g_FrameIndex;
    uint   g_RaysPerPixel;

    uint   g_MaxHistoryLength;
    uint   g_SpatialSamples;
    float  g_SpatialRadius;
    float  g_DepthThreshold;

    float  g_NormalThreshold;
    float  g_RadianceClamp;
    float  g_AccumulationBlend;
    int    g_MaxBounces;

    int    g_DebugView;
    int    g_TemporalReuseEnabled;
    int    g_SpatialReuseEnabled;
    int    g_NextEventEstimation;

    float  g_NrdSharpenAmount;
    float  g_ColorLeakIntensity;
    float  g_SpecularRoughnessThreshold;
    float  g_Pad0;

    float4x4 g_ViewProjInv;     // inverse view-projection (row-major)
    float4x4 g_CurrViewProj;    // current non-jittered view-projection
    float4x4 g_PrevViewProj;    // previous frame view-projection
    float4x4 g_WorldToView;     // current non-jittered world-to-view matrix (row-major)

    float3 g_CameraPos;
    float  g_Pad1;

    float3 g_SunDir;            // world-space direction FROM sun TOWARD scene (Z-up)
    float  g_Pad2;

    float3 g_SunColor;          // scaled Hosek-Wilkie sun colour
    float  g_Pad3;

    float3 g_SkyColor;          // scaled Hosek-Wilkie sky colour
    float  g_Pad4;

    int    g_NumPointLights;
    float3 g_Pad5;

    RtgiPointLightData g_PointLights[MAX_RTGI_POINT_LIGHTS];
}

// ─── GI Reservoir stored in the structured buffer ────────────────────────────
struct GIReservoir
{
    float3 position;    // world-space position of the visible primary surface pixel
    float3 normal;      // world-space normal of the visible primary surface pixel
    float3 radiance;    // indirect radiance chosen for that primary surface
    float  weightSum;   // running W sum (RIS weight accumulator)
    uint   M;           // number of candidates considered
    uint   age;         // frames this sample has been alive
};

// Packed form stored in StructuredBuffer<PackedGIReservoir> (12 floats + 2 uints)
struct PackedGIReservoir
{
    float posX, posY, posZ;
    uint  packedNormal;         // oct-encoded  2×snorm16

    float radR, radG, radB;
    float weight;

    uint  M;
    uint  age;
    uint  _pad0, _pad1;
};

// ─── Oct-normal encode / decode ───────────────────────────────────────────────
float2 OctWrap(float2 v)
{
    return (1.0f - abs(v.yx)) * (float2(v.xy >= 0.0f) * 2.0f - 1.0f);
}

uint EncodeNormal(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 oct = n.z >= 0.0f ? n.xy : OctWrap(n.xy);
    oct = oct * 0.5f + 0.5f;
    uint2 snorm16 = uint2(saturate(oct) * 65535.0f);
    return (snorm16.y << 16) | snorm16.x;
}

float3 DecodeNormal(uint packed)
{
    float2 oct;
    oct.x = (packed & 0xFFFFu) / 65535.0f * 2.0f - 1.0f;
    oct.y = (packed >> 16)     / 65535.0f * 2.0f - 1.0f;
    float3 n = float3(oct, 1.0f - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0f)
    {
        float2 wrapped = (1.0f - abs(n.yx)) * (float2(n.xy >= 0.0f) * 2.0f - 1.0f);
        n.xy = wrapped;
    }
    return normalize(n);
}

// ─── Reservoir pack / unpack ─────────────────────────────────────────────────
PackedGIReservoir PackReservoir(GIReservoir r)
{
    PackedGIReservoir p;
    p.posX         = r.position.x;
    p.posY         = r.position.y;
    p.posZ         = r.position.z;
    p.packedNormal = EncodeNormal(r.normal);
    p.radR         = r.radiance.x;
    p.radG         = r.radiance.y;
    p.radB         = r.radiance.z;
    p.weight       = r.weightSum;
    p.M            = min(r.M, 0xFFu);
    p.age          = min(r.age, 0xFFu);
    p._pad0        = 0;
    p._pad1        = 0;
    return p;
}

GIReservoir UnpackReservoir(PackedGIReservoir p)
{
    GIReservoir r;
    r.position  = float3(p.posX, p.posY, p.posZ);
    r.normal    = DecodeNormal(p.packedNormal);
    r.radiance  = float3(p.radR, p.radG, p.radB);
    r.weightSum = p.weight;
    r.M         = p.M;
    r.age       = p.age;
    return r;
}

GIReservoir EmptyReservoir()
{
    GIReservoir r;
    r.position  = float3(0, 0, 0);
    r.normal    = float3(0, 0, 1);
    r.radiance  = float3(0, 0, 0);
    r.weightSum = 0.0f;
    r.M         = 0;
    r.age       = 0;
    return r;
}

bool IsValidReservoir(GIReservoir r)
{
    return r.M > 0 && r.weightSum > 0.0f;
}

// ─── ReSTIR reservoir update (weighted reservoir sampling) ───────────────────
// Returns true if the incoming candidate was selected.
bool UpdateReservoir(inout GIReservoir r, GIReservoir candidate, float risWeight, inout uint rngState)
{
    r.weightSum += risWeight;
    r.M         += 1;

    // Random number in [0,1)
    rngState = rngState * 1664525u + 1013904223u;
    float xi = (rngState >> 8) * (1.0f / float(1 << 24));

    if (xi * r.weightSum <= risWeight)
    {
        r.position  = candidate.position;
        r.normal    = candidate.normal;
        r.radiance  = candidate.radiance;
        return true;
    }
    return false;
}

void FinalizeReservoir(inout GIReservoir r, float targetPdf)
{
    // W = w_sum / (M * p_hat)
    r.weightSum = (targetPdf > 0.0f) ? (r.weightSum / (float(r.M) * targetPdf)) : 0.0f;
}

// ─── Simple PCG-based RNG ─────────────────────────────────────────────────────
uint InitRng(uint2 pixel, uint frameIndex)
{
    return (pixel.x * 1973u + pixel.y * 9277u + frameIndex * 26699u) | 1u;
}

float RandFloat(inout uint state)
{
    state = state * 1664525u + 1013904223u;
    return (state >> 8) * (1.0f / float(1 << 24));
}

// ─── Reconstruct world-space position from depth ─────────────────────────────
float3 ReconstructWorldPos(uint2 pixel, float depth)
{
    float2 uv  = (float2(pixel) + 0.5f) / float2(g_FrameWidth, g_FrameHeight);
    float2 ndc = uv * 2.0f - 1.0f;
    ndc.y      = -ndc.y;
    float4 clip = float4(ndc, depth, 1.0f);

    // The renderer uploads Transpose(Inverse(ViewProjection)) so HLSL must use
    // row-vector multiplication here, matching the deferred lighting pass.
    float4 world = mul(clip, g_ViewProjInv);
    return world.xyz / world.w;
}

// ─── Project world position to previous-frame screen UV ──────────────────────
float2 ReprojectUV(float3 worldPos)
{
    // Previous-frame reprojection uses the same transposed matrix convention.
    float4 clip = mul(float4(worldPos, 1.0f), g_PrevViewProj);
    clip.xy /= clip.w;
    float2 uv = clip.xy * 0.5f + 0.5f;
    uv.y = 1.0f - uv.y;
    return uv;
}

float2 ProjectCurrentUV(float3 worldPos)
{
    float4 clip = mul(float4(worldPos, 1.0f), g_CurrViewProj);
    clip.xy /= clip.w;
    float2 uv = clip.xy * 0.5f + 0.5f;
    uv.y = 1.0f - uv.y;
    return uv;
}

// ─── Luminance helper ─────────────────────────────────────────────────────────
float Luminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// ─── Linear pixel index ───────────────────────────────────────────────────────
uint PixelIndex(uint2 pixel)
{
    return pixel.y * g_FrameWidth + pixel.x;
}

#endif // RTGI_COMMON_HLSLI
