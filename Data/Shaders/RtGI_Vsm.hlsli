// RtGI_Vsm.hlsli
// Next-event visibility at a bounce hit from the virtual shadow map, for the RTGI
// RayGen and specular passes. Include after RtGI_Common.hlsli.
//
// The map is already rendered for direct lighting, so where it holds a page
// covering the hit a lookup replaces a shadow ray - far cheaper, and it sees
// exactly what the raster shadows see, alpha-tested foliage included (the
// acceleration structure treats every triangle as opaque).
//
// It only holds the pages pixels on screen asked for, plus a few coarse levels
// and mips kept resident as fallbacks. Bounce hits are often off screen, where
// only those fallbacks exist, and a coarse page leaks light through anything
// about as thin as its texels - the wall between a sunlit exterior and a room.
// So a lookup that lands on a texel coarser than g_VsmMaxTexelSize, or on no
// page at all, answers RTGI_VSM_NO_ANSWER and the caller traces the ray.
//
// Filtering is a 2x2 bilinear PCF rather than the 4x4 the deferred pass uses:
// GI is denoised anyway, and the point is to be cheaper than the ray.

#ifndef RTGI_VSM_HLSLI
#define RTGI_VSM_HLSLI

Texture2D              t_VsmPool      : register(t40);
StructuredBuffer<uint> t_VsmPageTable : register(t41);

static const float RTGI_VSM_NO_ANSWER = -1.0f;

bool RtgiVsmUsable()
{
    return g_VsmVisibility != 0 && g_Vsm.Active != 0u;
}

// Sun visibility in [0, 1], or RTGI_VSM_NO_ANSWER. lightSideNormal is the
// geometric normal flipped to face the sun.
float RtgiVsmSunVisibility(float3 worldPos, float3 lightSideNormal)
{
    if (!RtgiVsmUsable() || g_Vsm.Enabled == 0u)
        return RTGI_VSM_NO_ANSWER;

    const PteroVsmLocation location = PteroVsmLocate(g_Vsm, t_VsmPageTable, t_VsmPool,
        worldPos, lightSideNormal, g_CameraPos, g_Vsm.NormalOffset);
    if (location.Level < 0 || location.TexelSize > g_VsmMaxTexelSize)
        return RTGI_VSM_NO_ANSWER;

    const float receiver = min((location.LightZ - g_Vsm.DepthNear - g_Vsm.ConstantBias * location.TexelSize)
                               * g_Vsm.DepthRangeInv, 1.0f);

    const float2 shifted = location.VirtualTexel - 0.5f;
    const int2   base    = int2(floor(shifted));
    const float2 f       = shifted - floor(shifted);

    float lit = 0.0f;
    float total = 0.0f;
    [unroll]
    for (int y = 0; y < 2; ++y)
    {
        [unroll]
        for (int x = 0; x < 2; ++x)
        {
            bool valid;
            const float stored = PteroVsmLoadTexel(g_Vsm, t_VsmPageTable, t_VsmPool, location.Level, base + int2(x, y), valid);
            const float weight = (x == 0 ? 1.0f - f.x : f.x) * (y == 0 ? 1.0f - f.y : f.y);
            // A tap on a page that is not resident says nothing.
            if (valid)
            {
                lit += weight * (receiver <= stored ? 1.0f : 0.0f);
                total += weight;
            }
        }
    }
    return total > 1e-3f ? lit / total : RTGI_VSM_NO_ANSWER;
}

// Visibility from a shadow-casting local light in [0, 1], or RTGI_VSM_NO_ANSWER.
// The light's ShadowIndex is its slot in the map whenever local lights are
// shadowed through it (DX12SceneRenderer assigns them before the GI copy).
float RtgiVsmLocalVisibility(PteroLightData light, float3 worldPos, float3 lightSideNormal)
{
    if (!RtgiVsmUsable() || g_Vsm.LocalEnabled == 0u || light.CastShadows < 0.5f || light.ShadowIndex < 0.0f)
        return RTGI_VSM_NO_ANSWER;

    const uint slot = (uint)light.ShadowIndex;
    const PteroVsmLocalLocation location = PteroVsmLocalLocate(g_Vsm, t_VsmPageTable, t_VsmPool, slot,
        worldPos, lightSideNormal, g_CameraPos, g_Vsm.NormalOffset, 1.0f);
    if (location.Mip < 0 || location.TexelWorld > g_VsmMaxTexelSize)
        return RTGI_VSM_NO_ANSWER;

    // The receiver in stored-depth units, as PteroVsmLocalVisibility does it.
    const float radius   = g_Vsm.LocalLights[slot].w;
    const float receiver = location.Depth - g_Vsm.ConstantBias * location.TexelWorld;
    const float n = g_Vsm.LocalNear;
    const float farPlane = max(radius, n * 2.0f);
    const float receiverStored = (farPlane - n * farPlane / max(receiver, 1e-6f)) / (farPlane - n);

    const float2 shifted = location.Texel - 0.5f;
    const int2   base    = int2(floor(shifted));
    const float2 f       = shifted - floor(shifted);

    float lit = 0.0f;
    float total = 0.0f;
    [unroll]
    for (int y = 0; y < 2; ++y)
    {
        [unroll]
        for (int x = 0; x < 2; ++x)
        {
            bool valid;
            const float stored = PteroVsmLoadLocalTexel(g_Vsm, t_VsmPageTable, t_VsmPool, slot,
                location.FaceInfo.Face, (uint)location.Mip, base + int2(x, y), valid);
            const float weight = (x == 0 ? 1.0f - f.x : f.x) * (y == 0 ? 1.0f - f.y : f.y);
            if (valid)
            {
                lit += weight * (stored >= receiverStored ? 1.0f : 0.0f);
                total += weight;
            }
        }
    }
    return total > 1e-3f ? lit / total : RTGI_VSM_NO_ANSWER;
}

#endif // RTGI_VSM_HLSLI
