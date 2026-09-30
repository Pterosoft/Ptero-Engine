// VirtualShadowMap.hlsli
// Sampling side of the virtual shadow map (VirtualShadowMapRenderer): the sun's clipmap
// and a virtual cube per shadow-casting local light, all paged into one physical pool.
//
// Sun. A clipmap of up to 16 levels, each a 16384^2 virtual depth map centred on the
// camera. Level L's texels are FirstTexelSize * 2^L metres, so each level covers twice
// the area of the one before at half the density. A level is cut into 128 x 128 pages
// of 128^2 texels.
//   light space   - (p - Anchor) projected on AxisX/AxisY/AxisZ, in metres. AxisZ is the
//                   direction the sunlight travels; the anchor is a point near the camera
//                   that only moves when the whole map is invalidated.
//   virtual texel - (ls.x, -ls.y) / texelSize for a level. Page = floor(vt / 128);
//                   pages are world-anchored, so a page keeps its content while the
//                   camera moves and the level's window slides over it.
//   page slot     - page & 127 on both axes: where a page lives in the level's
//                   128 x 128 table (toroidal addressing).
//   depth         - (ls.z - DepthNear) * DepthRangeInv, orthographic.
//
// Local lights. Every slot is a cube of six 4096^2 faces with a mip chain of six
// levels (32 x 32 pages at mip 0 down to 1 page at mip 5). Face f looks along
// kFaceForward[f] with kFaceUp[f] up; a direction v lands at u = v.R / v.F,
// v = v.U / v.F in [-1, 1] (R = cross(U, F)), row 0 at v = +1. Depth is the ordinary
// perspective depth of a LocalNear..radius projection along F.
//
// Only the pages pixels on screen sample are backed, by 128^2 tiles of one physical
// depth texture (the pool). Page table entry: bit 31 = the tile holds valid depth; bits
// 0..15 = pool tile index, row-major over PoolPagesX tiles per row. Anything else reads
// as "not resident" and the lookup falls back to the next coarser level or mip.
//
// Must stay in step with VsmGpuConstants in VirtualShadowMapConstants.h.

#ifndef PTERO_VIRTUAL_SHADOW_MAP_HLSLI
#define PTERO_VIRTUAL_SHADOW_MAP_HLSLI

#define PTERO_VSM_MAX_LEVELS        16
#define PTERO_VSM_PAGE_TEXELS       128
#define PTERO_VSM_LEVEL_PAGES       128
#define PTERO_VSM_PAGES_PER_LEVEL   (PTERO_VSM_LEVEL_PAGES * PTERO_VSM_LEVEL_PAGES)
#define PTERO_VSM_VALID_BIT         0x80000000u

#define PTERO_VSM_MAX_LOCAL         16
#define PTERO_VSM_LOCAL_RESOLUTION  4096
#define PTERO_VSM_LOCAL_MIPS        6
#define PTERO_VSM_LOCAL_FACE_PAGES  1365      // 32^2 + 16^2 + 8^2 + 4^2 + 2^2 + 1
#define PTERO_VSM_LOCAL_LIGHT_PAGES (6 * PTERO_VSM_LOCAL_FACE_PAGES)

struct PteroVsmConstants
{
    // --- Sun clipmap ---
    float3 Anchor;          float FirstTexelSize;   // metres per texel at level 0
    float3 AxisX;           float PixelFootprint;   // world size of one screen pixel per metre of distance
    float3 AxisY;           float LodBias;          // added to the level choice; + is coarser
    float3 AxisZ;           float DepthNear;        // light-space z stored as depth 0
    float  DepthRangeInv;   // 1 / (far - near), depth units per metre
    uint   LevelCount;
    uint   PoolPagesX;      // pool tiles per row
    uint   Enabled;         // the sun is shadowed by the map
    float  NormalOffset;    // receiver offset along the normal, in texels of the level used
    float  ConstantBias;    // receiver depth bias, in texels of the level used
    int    DebugView;       // 0 off, 1 clipmap level tint, 2 raw sun visibility
    uint   Active;          // the map is in use at all (the pool is what is bound)
    int4   WindowOrigin[PTERO_VSM_MAX_LEVELS]; // xy: first page of each level's 128 x 128 window

    // --- Local lights ---
    uint   LocalEnabled;
    float  LocalLodBias;
    uint   LocalPageTableBase;                  // first local entry in the page table
    float  LocalNear;
    float4 LocalLights[PTERO_VSM_MAX_LOCAL];      // slot: xyz position, w radius (0 = unused)
    float4 LocalDirections[PTERO_VSM_MAX_LOCAL];  // slot: xyz emission axis, w cosine below which it emits nothing
};

// ---------------------------------------------------------------------------
// Shared
// ---------------------------------------------------------------------------

// Stored depth of one texel through a page table entry, and whether it is resident.
float PteroVsmLoadTile(PteroVsmConstants vsm, Texture2D pool, uint entry, int2 texelInPage, out bool valid)
{
    valid = (entry & PTERO_VSM_VALID_BIT) != 0u;
    if (!valid)
        return 1.0f;
    const uint tile = entry & 0xFFFFu;
    const int2 tileOrigin = int2(tile % vsm.PoolPagesX, tile / vsm.PoolPagesX) * PTERO_VSM_PAGE_TEXELS;
    return pool.Load(int3(tileOrigin + texelInPage, 0)).r;
}

// ---------------------------------------------------------------------------
// Sun
// ---------------------------------------------------------------------------

float3 PteroVsmToLightSpace(PteroVsmConstants vsm, float3 worldPos)
{
    const float3 r = worldPos - vsm.Anchor;
    return float3(dot(r, vsm.AxisX), dot(r, vsm.AxisY), dot(r, vsm.AxisZ));
}

float PteroVsmTexelSize(PteroVsmConstants vsm, int level)
{
    return vsm.FirstTexelSize * exp2((float)level);
}

// The level whose texels are about the size of a screen pixel at this distance.
int PteroVsmDesiredLevel(PteroVsmConstants vsm, float distanceToCamera)
{
    const float footprint = max(distanceToCamera * vsm.PixelFootprint, 1e-6f);
    const float level = floor(log2(footprint / vsm.FirstTexelSize) + vsm.LodBias);
    return clamp((int)level, 0, (int)vsm.LevelCount - 1);
}

float2 PteroVsmVirtualTexel(float3 lightSpace, float texelSize)
{
    return float2(lightSpace.x, -lightSpace.y) / texelSize;
}

// The page table entry for one page of a level, or 0 when the page is outside the
// level's window.
uint PteroVsmPageEntry(PteroVsmConstants vsm, StructuredBuffer<uint> pageTable, int level, int2 page)
{
    const int2 rel = page - vsm.WindowOrigin[level].xy;
    if (any(rel < 0) || any(rel >= PTERO_VSM_LEVEL_PAGES))
        return 0u;
    const uint2 slot = uint2(page) & (PTERO_VSM_LEVEL_PAGES - 1);
    return pageTable[(uint)level * PTERO_VSM_PAGES_PER_LEVEL + slot.y * PTERO_VSM_LEVEL_PAGES + slot.x];
}

// Stored depth of one virtual texel, and whether its page is resident.
float PteroVsmLoadTexel(PteroVsmConstants vsm, StructuredBuffer<uint> pageTable, Texture2D pool,
                        int level, int2 virtualTexel, out bool valid)
{
    // Arithmetic shift: floor division for negative texel coordinates too.
    const int2 page = virtualTexel >> 7;
    const uint entry = PteroVsmPageEntry(vsm, pageTable, level, page);
    return PteroVsmLoadTile(vsm, pool, entry, virtualTexel & (PTERO_VSM_PAGE_TEXELS - 1), valid);
}

// Where a receiver lands in the map: the finest resident level at or above the one
// its distance asks for.
struct PteroVsmLocation
{
    int    Level;         // -1 = nothing resident covers the point
    float  TexelSize;
    float2 VirtualTexel;  // continuous
    float  LightZ;        // light-space z of the (offset) receiver, metres
};

PteroVsmLocation PteroVsmLocate(PteroVsmConstants vsm, StructuredBuffer<uint> pageTable, Texture2D pool,
                                float3 worldPos, float3 normal, float3 cameraPos, float normalOffsetTexels)
{
    PteroVsmLocation location;
    location.Level = -1;
    location.TexelSize = 0.0f;
    location.VirtualTexel = 0.0f.xx;
    location.LightZ = 0.0f;
    if (vsm.Enabled == 0u)
        return location;

    int level = PteroVsmDesiredLevel(vsm, distance(worldPos, cameraPos));
    [loop]
    for (; level < (int)vsm.LevelCount; ++level)
    {
        const float texel = PteroVsmTexelSize(vsm, level);
        const float3 ls = PteroVsmToLightSpace(vsm, worldPos + normal * (normalOffsetTexels * texel));
        const float2 vt = PteroVsmVirtualTexel(ls, texel);
        bool valid;
        PteroVsmLoadTexel(vsm, pageTable, pool, level, int2(floor(vt)), valid);
        if (valid)
        {
            location.Level = level;
            location.TexelSize = texel;
            location.VirtualTexel = vt;
            location.LightZ = ls.z;
            return location;
        }
    }
    return location;
}

// Sun visibility, 1 = lit. A 3-texel box filter with bilinear sub-texel weights (the
// 4 x 4 texel footprint of a 3 x 3 bilinear PCF), each tap resolved through the page
// table on its own because neighbouring pages sit anywhere in the pool.
float PteroVsmVisibility(PteroVsmConstants vsm, StructuredBuffer<uint> pageTable, Texture2D pool,
                         float3 worldPos, float3 normal, float3 cameraPos, out int usedLevel)
{
    const PteroVsmLocation location = PteroVsmLocate(vsm, pageTable, pool, worldPos, normal, cameraPos, vsm.NormalOffset);
    usedLevel = location.Level;
    if (location.Level < 0)
        return 1.0f;

    // Receivers beyond the far plane read as depth 1, the cleared value: lit.
    const float receiver = min((location.LightZ - vsm.DepthNear - vsm.ConstantBias * location.TexelSize) * vsm.DepthRangeInv, 1.0f);

    const float2 shifted = location.VirtualTexel - 0.5f;
    const int2 base = int2(floor(shifted));
    const float2 f = shifted - floor(shifted);
    const float weightsX[4] = { 1.0f - f.x, 1.0f, 1.0f, f.x };
    const float weightsY[4] = { 1.0f - f.y, 1.0f, 1.0f, f.y };

    float lit = 0.0f;
    float total = 0.0f;
    [unroll]
    for (int y = 0; y < 4; ++y)
    {
        [unroll]
        for (int x = 0; x < 4; ++x)
        {
            bool valid;
            const float stored = PteroVsmLoadTexel(vsm, pageTable, pool, location.Level, base + int2(x - 1, y - 1), valid);
            const float weight = weightsX[x] * weightsY[y];
            // A tap on a page that is not resident says nothing; leave it out rather
            // than let it vote "lit".
            if (valid)
            {
                lit += weight * (receiver <= stored ? 1.0f : 0.0f);
                total += weight;
            }
        }
    }
    return total > 0.0f ? lit / total : 1.0f;
}

// Metres between the first surface the sun reaches and this point, along the light,
// or -1 where the map has nothing to say. For transmission through thin objects.
float PteroVsmOccluderDistance(PteroVsmConstants vsm, StructuredBuffer<uint> pageTable, Texture2D pool,
                               float3 worldPos, float3 normal, float3 cameraPos)
{
    // No normal offset: this wants the surface itself, pushed slightly inside.
    const float3 shrunk = worldPos - 0.005f * normal;
    const PteroVsmLocation location = PteroVsmLocate(vsm, pageTable, pool, shrunk, normal, cameraPos, 0.0f);
    if (location.Level < 0)
        return -1.0f;

    bool valid;
    const float stored = PteroVsmLoadTexel(vsm, pageTable, pool, location.Level, int2(floor(location.VirtualTexel)), valid);
    if (!valid || stored >= 1.0f)
        return -1.0f;
    const float occluderZ = vsm.DepthNear + stored / max(vsm.DepthRangeInv, 1e-12f);
    return max(location.LightZ - occluderZ, 0.0f);
}

// ---------------------------------------------------------------------------
// Local lights
// ---------------------------------------------------------------------------

struct PteroVsmCubeFace
{
    uint   Face;
    float3 Forward;
    float3 Up;
    float3 Right;
};

// The face a direction from the light falls on. Must match kLocalFaces in
// VirtualShadowMapRenderer.cpp.
PteroVsmCubeFace PteroVsmSelectFace(float3 v)
{
    PteroVsmCubeFace face;
    const float3 a = abs(v);
    if (a.x >= a.y && a.x >= a.z)
    {
        face.Face = v.x >= 0.0f ? 0u : 1u;
        face.Forward = float3(v.x >= 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f);
        face.Up = float3(0.0f, 0.0f, 1.0f);
    }
    else if (a.y >= a.z)
    {
        face.Face = v.y >= 0.0f ? 2u : 3u;
        face.Forward = float3(0.0f, v.y >= 0.0f ? 1.0f : -1.0f, 0.0f);
        face.Up = float3(0.0f, 0.0f, 1.0f);
    }
    else
    {
        face.Face = v.z >= 0.0f ? 4u : 5u;
        face.Forward = float3(0.0f, 0.0f, v.z >= 0.0f ? 1.0f : -1.0f);
        face.Up = float3(0.0f, 1.0f, 0.0f);
    }
    face.Right = cross(face.Up, face.Forward);
    return face;
}

uint PteroVsmLocalMipOffset(uint mip)
{
    // 1024, 256, 64, 16, 4, 1 pages per mip.
    static const uint kOffsets[PTERO_VSM_LOCAL_MIPS] = { 0u, 1024u, 1280u, 1344u, 1360u, 1364u };
    return kOffsets[min(mip, PTERO_VSM_LOCAL_MIPS - 1u)];
}

// Index of a local page in the request bits / page table, counted from the first
// local entry.
uint PteroVsmLocalPageIndex(uint slot, uint face, uint mip, uint2 page)
{
    const uint pagesPerAxis = 32u >> mip;
    return slot * PTERO_VSM_LOCAL_LIGHT_PAGES + face * PTERO_VSM_LOCAL_FACE_PAGES
         + PteroVsmLocalMipOffset(mip) + page.y * pagesPerAxis + page.x;
}

// Continuous texel position of a direction on its face at a mip.
float2 PteroVsmLocalTexel(PteroVsmCubeFace face, float3 v, uint mip)
{
    const float z = max(dot(v, face.Forward), 1e-6f);
    const float resolution = (float)(PTERO_VSM_LOCAL_RESOLUTION >> mip);
    return float2(dot(v, face.Right) / z * 0.5f + 0.5f, 0.5f - dot(v, face.Up) / z * 0.5f) * resolution;
}

float PteroVsmLoadLocalTexel(PteroVsmConstants vsm, StructuredBuffer<uint> pageTable, Texture2D pool,
                             uint slot, uint face, uint mip, int2 texel, out bool valid)
{
    const int resolution = PTERO_VSM_LOCAL_RESOLUTION >> mip;
    if (any(texel < 0) || any(texel >= resolution))
    {
        valid = false;
        return 1.0f;
    }
    const uint entry = pageTable[vsm.LocalPageTableBase + PteroVsmLocalPageIndex(slot, face, mip, uint2(texel) >> 7)];
    return PteroVsmLoadTile(vsm, pool, entry, texel & (PTERO_VSM_PAGE_TEXELS - 1), valid);
}

// The mip whose texels are about a screen pixel (times footprintScale) where the
// point is.
uint PteroVsmLocalDesiredMip(PteroVsmConstants vsm, float distanceToCamera, float depthFromLight, float footprintScale)
{
    const float footprint = max(distanceToCamera * vsm.PixelFootprint * footprintScale, 1e-6f);
    // Face texels at mip 0 span 2 / resolution of the unit-distance face plane.
    const float texelsWanted = footprint / max(depthFromLight, 1e-4f) * (PTERO_VSM_LOCAL_RESOLUTION * 0.5f);
    const float mip = floor(log2(texelsWanted) + vsm.LocalLodBias);
    return (uint)clamp((int)mip, 0, PTERO_VSM_LOCAL_MIPS - 1);
}

float PteroVsmLocalLinearDepth(PteroVsmConstants vsm, float stored, float radius)
{
    const float n = vsm.LocalNear;
    const float f = max(radius, n * 2.0f);
    return n * f / max(f - stored * (f - n), 1e-6f);
}

// Where a receiver lands in a local light's cube.
struct PteroVsmLocalLocation
{
    int    Mip;           // -1 = nothing resident covers the point
    PteroVsmCubeFace FaceInfo;
    float2 Texel;         // continuous, at Mip
    float  Depth;         // along the face's forward axis, metres
    float  TexelWorld;    // world size of one texel at Depth
};

PteroVsmLocalLocation PteroVsmLocalLocate(PteroVsmConstants vsm, StructuredBuffer<uint> pageTable, Texture2D pool,
                                          uint slot, float3 worldPos, float3 normal, float3 cameraPos,
                                          float normalOffsetTexels, float footprintScale)
{
    PteroVsmLocalLocation location;
    location.Mip = -1;
    location.FaceInfo = PteroVsmSelectFace(float3(1.0f, 0.0f, 0.0f));
    location.Texel = 0.0f.xx;
    location.Depth = 0.0f;
    location.TexelWorld = 0.0f;
    if (vsm.LocalEnabled == 0u || slot >= PTERO_VSM_MAX_LOCAL || vsm.LocalLights[slot].w <= 0.0f)
        return location;

    const float3 lightPos = vsm.LocalLights[slot].xyz;
    const float3 v0 = worldPos - lightPos;
    const PteroVsmCubeFace face0 = PteroVsmSelectFace(v0);
    const float depth0 = dot(v0, face0.Forward);
    if (depth0 <= 1e-4f)
        return location;

    uint mip = PteroVsmLocalDesiredMip(vsm, distance(worldPos, cameraPos), depth0, footprintScale);
    [loop]
    for (; mip < PTERO_VSM_LOCAL_MIPS; ++mip)
    {
        const float texelWorld = depth0 * 2.0f / (float)(PTERO_VSM_LOCAL_RESOLUTION >> mip);
        const float3 v = worldPos + normal * (normalOffsetTexels * texelWorld) - lightPos;
        const PteroVsmCubeFace face = PteroVsmSelectFace(v);
        const float depth = dot(v, face.Forward);
        if (depth <= 1e-4f)
            continue;
        const float2 texel = PteroVsmLocalTexel(face, v, mip);
        bool valid;
        PteroVsmLoadLocalTexel(vsm, pageTable, pool, slot, face.Face, mip, int2(floor(texel)), valid);
        if (valid)
        {
            location.Mip = (int)mip;
            location.FaceInfo = face;
            location.Texel = texel;
            location.Depth = depth;
            location.TexelWorld = depth * 2.0f / (float)(PTERO_VSM_LOCAL_RESOLUTION >> mip);
            return location;
        }
    }
    return location;
}

// Visibility of a point from a local light's slot, 1 = lit, filtered as the sun is.
// normal may be zero (a point in a medium); footprintScale widens the pixel footprint
// the mip is chosen for (1 for surfaces).
float PteroVsmLocalVisibility(PteroVsmConstants vsm, StructuredBuffer<uint> pageTable, Texture2D pool,
                              uint slot, float3 worldPos, float3 normal, float3 cameraPos, float footprintScale)
{
    const PteroVsmLocalLocation location = PteroVsmLocalLocate(vsm, pageTable, pool, slot, worldPos, normal, cameraPos,
                                                               vsm.NormalOffset, footprintScale);
    if (location.Mip < 0)
        return 1.0f;

    const float radius = vsm.LocalLights[slot].w;
    const float receiver = location.Depth - vsm.ConstantBias * location.TexelWorld;

    // The receiver in stored-depth units, so each tap is one compare rather than a
    // linearising divide. PteroVsmLocalLinearDepth rises with the stored value, so
    // receiver <= linear(stored) is exactly stored >= this.
    const float n = vsm.LocalNear;
    const float f = max(radius, n * 2.0f);
    const float receiverStored = (f - n * f / max(receiver, 1e-6f)) / (f - n);

    const float2 shifted = location.Texel - 0.5f;
    const int2 base = int2(floor(shifted));
    const float2 texelFrac = shifted - floor(shifted);
    const float weightsX[4] = { 1.0f - texelFrac.x, 1.0f, 1.0f, texelFrac.x };
    const float weightsY[4] = { 1.0f - texelFrac.y, 1.0f, 1.0f, texelFrac.y };

    // The 4x4 footprint is texels base-1 .. base+2. Nearly always it lies inside one
    // 128-texel page (and inside the face), and then one page-table read serves all
    // sixteen taps. Per-tap lookups made this the costliest thing a pixel lit by many
    // shadowed local lights did: an interior full of candles paid ~34 dependent loads
    // per light per pixel.
    const int resolution = PTERO_VSM_LOCAL_RESOLUTION >> location.Mip;
    const int2 footprintMin = base - 1;
    const int2 footprintMax = base + 2;
    [branch]
    if (all(footprintMin >= 0) && all(footprintMax < resolution)
        && all((footprintMin >> 7) == (footprintMax >> 7)))
    {
        const uint entry = pageTable[vsm.LocalPageTableBase
            + PteroVsmLocalPageIndex(slot, location.FaceInfo.Face, (uint)location.Mip, uint2(footprintMin) >> 7)];
        if ((entry & PTERO_VSM_VALID_BIT) != 0u)
        {
            const uint tile = entry & 0xFFFFu;
            const int2 tileOrigin = int2(tile % vsm.PoolPagesX, tile / vsm.PoolPagesX) * PTERO_VSM_PAGE_TEXELS;
            const int2 origin = tileOrigin + (footprintMin & (PTERO_VSM_PAGE_TEXELS - 1));

            float litFast = 0.0f;
            [unroll]
            for (int y = 0; y < 4; ++y)
            {
                [unroll]
                for (int x = 0; x < 4; ++x)
                {
                    const float stored = pool.Load(int3(origin + int2(x, y), 0)).r;
                    litFast += weightsX[x] * weightsY[y] * (stored >= receiverStored ? 1.0f : 0.0f);
                }
            }
            // The weights sum to 3 per axis.
            return litFast * (1.0f / 9.0f);
        }
    }

    float lit = 0.0f;
    float total = 0.0f;
    [unroll]
    for (int y = 0; y < 4; ++y)
    {
        [unroll]
        for (int x = 0; x < 4; ++x)
        {
            bool valid;
            const float stored = PteroVsmLoadLocalTexel(vsm, pageTable, pool, slot, location.FaceInfo.Face,
                                                        (uint)location.Mip, base + int2(x - 1, y - 1), valid);
            const float weight = weightsX[x] * weightsY[y];
            if (valid)
            {
                lit += weight * (stored >= receiverStored ? 1.0f : 0.0f);
                total += weight;
            }
        }
    }
    return total > 0.0f ? lit / total : 1.0f;
}

// Metres between the first surface the light reaches along the ray to this point and
// the point itself, or -1 where the map has nothing to say.
float PteroVsmLocalOccluderDistance(PteroVsmConstants vsm, StructuredBuffer<uint> pageTable, Texture2D pool,
                                    uint slot, float3 worldPos, float3 normal, float3 cameraPos)
{
    const float3 shrunk = worldPos - 0.005f * normal;
    const PteroVsmLocalLocation location = PteroVsmLocalLocate(vsm, pageTable, pool, slot, shrunk, normal, cameraPos, 0.0f, 1.0f);
    if (location.Mip < 0)
        return -1.0f;

    bool valid;
    const float stored = PteroVsmLoadLocalTexel(vsm, pageTable, pool, slot, location.FaceInfo.Face, (uint)location.Mip,
                                                int2(floor(location.Texel)), valid);
    if (!valid || stored >= 1.0f)
        return -1.0f;
    const float occluderDepth = PteroVsmLocalLinearDepth(vsm, stored, vsm.LocalLights[slot].w);
    // Depths are along the face axis; scale the gap to distance along the ray.
    const float rayLength = length(shrunk - vsm.LocalLights[slot].xyz);
    return max(location.Depth - occluderDepth, 0.0f) / max(location.Depth, 1e-4f) * rayLength;
}

// Tint for the level debug view.
float3 PteroVsmLevelColour(int level)
{
    if (level < 0)
        return float3(1.0f, 0.0f, 1.0f);
    static const float3 kColours[8] =
    {
        float3(1.0f, 0.25f, 0.25f), float3(1.0f, 0.6f, 0.2f), float3(1.0f, 1.0f, 0.25f), float3(0.35f, 1.0f, 0.35f),
        float3(0.25f, 1.0f, 1.0f), float3(0.3f, 0.5f, 1.0f), float3(0.7f, 0.35f, 1.0f), float3(1.0f, 0.4f, 0.8f),
    };
    return kColours[level & 7] * ((level & 8) != 0 ? 0.55f : 1.0f);
}

#endif // PTERO_VIRTUAL_SHADOW_MAP_HLSLI
