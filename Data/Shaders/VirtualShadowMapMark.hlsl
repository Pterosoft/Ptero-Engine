// VirtualShadowMapMark.hlsl
// Page requests for the virtual shadow map (VirtualShadowMapRenderer): the sun's
// clipmap and each shadowed local light's virtual cube.
//
// A fullscreen pass over the finished G-Buffer: every opaque pixel works out which
// clipmap level (and, for each local light that reaches it, which cube face and mip)
// the lighting pass will read for it and sets the request bit of each page its filter
// footprint touches. It also requests the page two levels coarser, which covers
// sixteen times the area and so is nearly always resident already: when a fine page
// is missing (just revealed, or over the frame's render budget) the lighting falls
// back to it instead of to nothing.
//
// The bits go back to the CPU, which allocates and renders the pages a few frames
// later; see VirtualShadowMapRenderer.cpp.
//
// Drawn with no render target: the output is the UAV alone.

#include "VirtualShadowMap.hlsli"

cbuffer VsmMarkConstants : register(b0)
{
    float4x4 gInvViewProj;   // Transpose(inverse(viewProj)), for mul(ndc, M)
    float3   gCameraPos;
    float    _MarkPad0;
    PteroVsmConstants gVsm;
};

Texture2D<float>  gDepth        : register(t0);
Texture2D<float4> gGBufferNormal : register(t1); // oct normal in xy

RWByteAddressBuffer gRequests : register(u0);    // one bit per page slot, level-major

struct VSOutput
{
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput output;
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.TexCoord = uv;
    output.Position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

// Same decode as DeferredLighting.hlsl.
float3 DecodeOctNormal(float2 encoded)
{
    float2 oct = encoded * 2.0f - 1.0f;
    float3 n = float3(oct, 1.0f - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0f)
    {
        n.xy = (1.0f - abs(n.yx)) * (float2(n.xy >= 0.0f) * 2.0f - 1.0f);
    }
    return normalize(n);
}

void RequestBit(uint bit)
{
    const uint address = (bit >> 5) * 4u;
    const uint mask = 1u << (bit & 31u);
    // Neighbouring pixels nearly always want the same page: read first, so most of
    // them skip the atomic.
    if ((gRequests.Load(address) & mask) == 0u)
        gRequests.InterlockedOr(address, mask);
}

void RequestPage(int level, int2 page)
{
    const int2 rel = page - gVsm.WindowOrigin[level].xy;
    if (any(rel < 0) || any(rel >= PTERO_VSM_LEVEL_PAGES))
        return;
    const uint2 slot = uint2(page) & (PTERO_VSM_LEVEL_PAGES - 1);
    RequestBit((uint)level * PTERO_VSM_PAGES_PER_LEVEL + slot.y * PTERO_VSM_LEVEL_PAGES + slot.x);
}

void RequestLocalPage(uint slot, uint face, uint mip, int2 page)
{
    const int pagesPerAxis = 32 >> mip;
    if (any(page < 0) || any(page >= pagesPerAxis))
        return;
    RequestBit(gVsm.LocalPageTableBase + PteroVsmLocalPageIndex(slot, face, mip, uint2(page)));
}

// The pages of each shadowed local light this pixel will read.
void RequestLocalPages(float3 worldPos, float3 normal)
{
    const float cameraDistance = distance(worldPos, gCameraPos);
    [loop]
    for (uint slot = 0; slot < PTERO_VSM_MAX_LOCAL; ++slot)
    {
        const float4 light = gVsm.LocalLights[slot];
        if (light.w <= 0.0f)
            continue;
        const float3 v0 = worldPos - light.xyz;
        const float lightDistance = length(v0);
        if (lightDistance >= light.w || lightDistance <= 1e-4f)
            continue;
        // Outside a spot's cone or behind a one-sided panel the light emits nothing,
        // so nothing there needs its shadow. A little margin for the filter.
        const float4 emission = gVsm.LocalDirections[slot];
        if (dot(v0 / lightDistance, emission.xyz) < emission.w - 0.05f)
            continue;

        const PteroVsmCubeFace face0 = PteroVsmSelectFace(v0);
        const float depth0 = dot(v0, face0.Forward);
        const uint mip = PteroVsmLocalDesiredMip(gVsm, cameraDistance, depth0, 1.0f);
        const float texelWorld = depth0 * 2.0f / (float)(PTERO_VSM_LOCAL_RESOLUTION >> mip);
        const float3 v = worldPos + normal * (gVsm.NormalOffset * texelWorld) - light.xyz;
        const PteroVsmCubeFace face = PteroVsmSelectFace(v);

        const float2 texel = PteroVsmLocalTexel(face, v, mip);
        const int2 low = int2(floor(texel - 2.0f)) >> 7;
        const int2 high = int2(floor(texel + 2.0f)) >> 7;
        RequestLocalPage(slot, face.Face, mip, low);
        if (high.x != low.x) RequestLocalPage(slot, face.Face, mip, int2(high.x, low.y));
        if (high.y != low.y) RequestLocalPage(slot, face.Face, mip, int2(low.x, high.y));
        if (high.x != low.x && high.y != low.y) RequestLocalPage(slot, face.Face, mip, high);

        // Coarse fallback, two mips up.
        const uint coarse = min(mip + 2u, PTERO_VSM_LOCAL_MIPS - 1u);
        if (coarse != mip)
            RequestLocalPage(slot, face.Face, coarse, int2(floor(PteroVsmLocalTexel(face, v, coarse))) >> 7);
    }
}

void PSMain(VSOutput input)
{
    const int2 pixel = int2(input.Position.xy);
    const float depth = gDepth.Load(int3(pixel, 0));
    if (depth >= 1.0f)
        return;

    const float2 uv = input.TexCoord;
    const float4 ndc = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
    const float4 world = mul(ndc, gInvViewProj);
    const float3 worldPos = world.xyz / world.w;
    const float3 normal = DecodeOctNormal(gGBufferNormal.Load(int3(pixel, 0)).xy);

    if (gVsm.LocalEnabled != 0u)
        RequestLocalPages(worldPos, normal);

    if (gVsm.Enabled == 0u)
        return;

    const int level = PteroVsmDesiredLevel(gVsm, distance(worldPos, gCameraPos));

    // The lighting pass samples at the normal-offset position with a 4 x 4 texel filter,
    // so request every page that footprint can reach.
    const float texel = PteroVsmTexelSize(gVsm, level);
    const float3 ls = PteroVsmToLightSpace(gVsm, worldPos + normal * (gVsm.NormalOffset * texel));
    const float2 vt = PteroVsmVirtualTexel(ls, texel);
    const int2 low = int2(floor(vt - 2.0f)) >> 7;
    const int2 high = int2(floor(vt + 2.0f)) >> 7;
    RequestPage(level, low);
    if (high.x != low.x) RequestPage(level, int2(high.x, low.y));
    if (high.y != low.y) RequestPage(level, int2(low.x, high.y));
    if (high.x != low.x && high.y != low.y) RequestPage(level, high);

    // Coarse fallback.
    const int coarse = min(level + 2, (int)gVsm.LevelCount - 1);
    if (coarse != level)
    {
        const float coarseTexel = PteroVsmTexelSize(gVsm, coarse);
        const float3 coarseLs = PteroVsmToLightSpace(gVsm, worldPos + normal * (gVsm.NormalOffset * coarseTexel));
        RequestPage(coarse, int2(floor(PteroVsmVirtualTexel(coarseLs, coarseTexel))) >> 7);
    }
}
