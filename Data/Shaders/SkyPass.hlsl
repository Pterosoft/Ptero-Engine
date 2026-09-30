// SkyPass.hlsl
// Fullscreen sky background pass.
// Draws a vertical sky gradient from the evaluated time-of-day ambient, a sunset
// glow, the sun disc, the moon shaded by its phase, and a star field (with a
// faint Milky Way) that turns with the hour. Everything is worked out in world
// space, Z up; SkyRenderer.cpp fills the constants.

cbuffer SkyConstants : register(b0)
{
    // Inverse projection to reconstruct the view-space ray from NDC, then the
    // inverse view to take it into world space.
    float4x4 InvProj;
    float4x4 InvView;

    // Sky colour at the zenith and at the horizon (linear HDR RGB).
    float3 SkyZenithColor;
    float  PixelAngle;          // radians covered by one pixel
    float3 SkyHorizonColor;
    float  TimeSeconds;

    // Directions point from the scene toward the body.
    float3 SunDirection;
    float  SunDiscHalfAngleCos;
    float3 SunColor;            // sun disc radiance, already faded at the horizon
    float  MoonDiscSin;         // sin of the moon's angular radius
    float3 MoonDirection;
    float  StarVisibility;      // 0 by day, 1 once the sun is well down
    float3 MoonColor;           // full-face radiance; shaded by phase here
    float  StarRadiance;
    float3 TwilightGlowColor;
    float  MoonHalo;

    // World -> the frame the stars are fixed in (rows, xyz). Row 2 is the
    // celestial pole in world space.
    float4 StarRotation[3];

    float  MoonTextureOn;
    float  SunTextureOn;
    float  StarMapGain;
    float  ProceduralStarGain;
    float  ProceduralMilkyWayGain;
    float3 _SkyPad3;
}

// Equirectangular maps (Data/Textures/Sky): longitude across, latitude down,
// longitude 0 in the middle. Only read when the matching flag above is set.
Texture2D<float4> gMoonMap : register(t0);   // NASA LROC colour
Texture2D<float4> gSunMap  : register(t1);   // Solar System Scope sun
Texture2D<float4> gStarMap : register(t2);   // Solar System Scope stars + Milky Way
SamplerState      gSkySampler : register(s0);

static const float kPi = 3.14159265f;

// UV of a unit vector given in a map's own frame (x = longitude 0, z = north).
float2 EquirectUv(float3 local)
{
    const float longitude = atan2(local.y, local.x);
    const float latitude  = asin(clamp(local.z, -1.0f, 1.0f));
    return float2(0.5f + longitude / (2.0f * kPi), 0.5f - latitude / kPi);
}

float Luma(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// Mip level for a disc of the given angular radius covered by half a map's
// width (the visible hemisphere). Explicit, because these samples sit in
// branches and across the map's longitude seam, where derivatives are useless.
float DiscMapLod(uint mapWidth, float discSin)
{
    const float discPixels = 2.0f * discSin / max(PixelAngle, 1e-6f);
    return log2(max(0.5f * float(mapWidth) / max(discPixels, 1.0f), 1.0f));
}

// A map's overall brightness, from its last mip. Surface detail is taken
// relative to this, so the map shapes a disc without changing how bright the
// time of day made it.
#define PTERO_MAP_AVERAGE_LUMA(map, outWidth, outAverage)                                          \
    {                                                                                               \
        uint mapHeight_, mapLevels_;                                                                \
        map.GetDimensions(0, outWidth, mapHeight_, mapLevels_);                                     \
        outAverage = max(Luma(map.SampleLevel(gSkySampler, float2(0.25f, 0.5f),                    \
                                              float(mapLevels_ - 1)).rgb), 1e-3f);                  \
    }

// Frame for a body seen along viewDir (scene -> body): x points back at the
// viewer, z toward celestial north, y completes it so +longitude sits on the
// viewer's right with north up - how the Moon's near side is mapped.
void BodyFrame(float3 viewDir, out float3 x, out float3 y, out float3 z)
{
    x = -viewDir;
    float3 north = StarRotation[2].xyz - x * dot(StarRotation[2].xyz, x);
    north = dot(north, north) > 1e-6f ? normalize(north) : float3(0.0f, 0.0f, 1.0f);
    z = north;
    y = cross(viewDir, z);
}

struct VSOutput
{
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD;
};

// Emit a full-screen triangle with no vertex buffer.
VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput output;
    // Generate a triangle that covers the entire screen.
    output.TexCoord   = float2((vertexId << 1) & 2, vertexId & 2);
    output.Position   = float4(output.TexCoord * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 1.0f, 1.0f);
    return output;
}

// ─── Hashing and noise ───────────────────────────────────────────────────────
// pcg3d (Jarzynski & Olano, "Hash Functions for GPU Rendering").
uint3 Pcg3d(uint3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}

float3 HashToUnit(uint3 h)
{
    return float3(h >> 8u) * (1.0f / 16777216.0f);
}

float ValueNoise3(float3 p)
{
    const float3 cell = floor(p);
    float3 f = p - cell;
    f = f * f * (3.0f - 2.0f * f);
    const uint3 base = uint3(int3(cell));

    const float n000 = HashToUnit(Pcg3d(base + uint3(0, 0, 0))).x;
    const float n100 = HashToUnit(Pcg3d(base + uint3(1, 0, 0))).x;
    const float n010 = HashToUnit(Pcg3d(base + uint3(0, 1, 0))).x;
    const float n110 = HashToUnit(Pcg3d(base + uint3(1, 1, 0))).x;
    const float n001 = HashToUnit(Pcg3d(base + uint3(0, 0, 1))).x;
    const float n101 = HashToUnit(Pcg3d(base + uint3(1, 0, 1))).x;
    const float n011 = HashToUnit(Pcg3d(base + uint3(0, 1, 1))).x;
    const float n111 = HashToUnit(Pcg3d(base + uint3(1, 1, 1))).x;

    return lerp(lerp(lerp(n000, n100, f.x), lerp(n010, n110, f.x), f.y),
                lerp(lerp(n001, n101, f.x), lerp(n011, n111, f.x), f.y), f.z);
}

// ─── Stars ───────────────────────────────────────────────────────────────────
// One layer of stars: the sphere is cut into a cube-mapped grid, and a cell
// holds at most one star, kept away from the cell's edges so a pixel only ever
// has to look at its own cell. Each star is a Gaussian about a pixel wide
// whatever the field of view, so it neither aliases when small nor bloats when
// zoomed. Brightness follows a steep power law: a few bright stars, many faint.
float3 StarLayer(float3 s, float cellsPerFace, float density, float brightness, uint seed)
{
    const float3 a = abs(s);
    float2 uv;
    uint face;
    if (a.x >= a.y && a.x >= a.z)      { uv = s.yz / a.x; face = s.x > 0.0f ? 0u : 1u; }
    else if (a.y >= a.z)               { uv = s.xz / a.y; face = s.y > 0.0f ? 2u : 3u; }
    else                               { uv = s.xy / a.z; face = s.z > 0.0f ? 4u : 5u; }

    const float2 grid = (uv * 0.5f + 0.5f) * cellsPerFace;
    const float2 cell = floor(grid);
    const uint3 h0 = Pcg3d(uint3(uint2(cell), face + seed * 8u));
    const float3 r0 = HashToUnit(h0);
    if (r0.x > density)
        return float3(0.0f, 0.0f, 0.0f);
    const float3 r1 = HashToUnit(Pcg3d(h0));

    const float2 starPos = 0.25f + 0.5f * r0.yz;
    // Angle subtended by one cell here: the cube face is a gnomonic projection,
    // so cells shrink toward the face's corners.
    const float cellAngle = (2.0f / cellsPerFace) / (1.0f + dot(uv, uv));
    const float2 delta = (grid - cell - starPos) * cellAngle;
    const float sigma = max(PixelAngle, 1e-5f) * 0.6f;
    const float profile = exp(-0.5f * dot(delta, delta) / (sigma * sigma));

    const float magnitude = 0.08f + 5.0f * pow(r1.x, 10.0f);
    const float twinkle = 0.8f + 0.2f * sin(TimeSeconds * (1.5f + 3.0f * r1.y) + r1.z * 40.0f);
    // Mostly white, some warm, some blue.
    const float3 tint = lerp(float3(1.0f, 0.78f, 0.58f), float3(0.78f, 0.86f, 1.0f), r1.y);

    return tint * (magnitude * twinkle * brightness * profile);
}

float3 StarField(float3 worldDir)
{
    const float3 s = float3(dot(StarRotation[0].xyz, worldDir),
                            dot(StarRotation[1].xyz, worldDir),
                            dot(StarRotation[2].xyz, worldDir));

    float3 stars = float3(0.0f, 0.0f, 0.0f);

    [branch]
    if (ProceduralStarGain > 0.0f)
    {
        stars += ProceduralStarGain * (StarLayer(s, 110.0f, 0.35f, 1.0f, 0u)
                                     + StarLayer(s, 260.0f, 0.18f, 0.3f, 1u));
    }

    // A faint band of unresolved stars along a great circle, mottled by noise.
    [branch]
    if (ProceduralMilkyWayGain > 0.0f)
    {
        const float3 galacticPole = normalize(float3(0.35f, -0.6f, 0.72f));
        const float band = exp(-18.0f * dot(s, galacticPole) * dot(s, galacticPole));
        const float mottle = ValueNoise3(s * 5.0f) * 0.65f + ValueNoise3(s * 13.0f) * 0.35f;
        stars += float3(0.85f, 0.9f, 1.0f) * (ProceduralMilkyWayGain * 0.25f * band * smoothstep(0.3f, 0.85f, mottle));
    }

    // The star map is in equatorial coordinates, which is the frame s already
    // is: its north pole is the celestial pole, so the real sky turns about it.
    [branch]
    if (StarMapGain > 0.0f)
    {
        uint width, height;
        gStarMap.GetDimensions(width, height);
        const float texelAngle = 2.0f * kPi / float(width);
        const float lod = log2(max(PixelAngle / texelAngle, 1.0f));
        stars += StarMapGain * 1.5f * gStarMap.SampleLevel(gSkySampler, EquirectUv(s), lod).rgb;
    }

    return stars;
}

// ─── Moon ────────────────────────────────────────────────────────────────────
// The disc is shaded as a sphere lit from the sun's actual direction, so the
// phase - crescent, quarter, gibbous - falls out of where the two bodies are
// rather than being drawn. Maria are low-frequency noise on the disc.
// Returns the moon's radiance; outCoverage is how much of the pixel it covers.
float3 Moon(float3 dir, out float outCoverage)
{
    outCoverage = 0.0f;
    const float cosToMoon = dot(dir, MoonDirection);
    if (cosToMoon < 0.9f || MoonDiscSin <= 0.0f)
        return float3(0.0f, 0.0f, 0.0f);

    // Offset from the moon's centre in the plane facing the viewer, in disc radii.
    const float3 offset = (dir - MoonDirection * cosToMoon) / MoonDiscSin;
    const float radius = length(offset);

    // Faint glow around it, before the disc is tested.
    float3 result = MoonColor * MoonHalo * exp(-radius * 0.25f);

    outCoverage = saturate((1.0f - radius) * MoonDiscSin / max(PixelAngle, 1e-6f) + 0.5f);
    if (outCoverage <= 0.0f)
        return result;

    // Sphere normal on the near face.
    const float r2 = saturate(radius * radius);
    const float3 normal = normalize(offset - MoonDirection * sqrt(1.0f - r2));

    // The Moon's surface is closer to Lommel-Seeliger than Lambert: nearly flat
    // across the lit face, with a soft terminator.
    const float lit = smoothstep(-0.02f, 0.12f, dot(normal, SunDirection));

    float3 albedo;
    [branch]
    if (MoonTextureOn > 0.0f)
    {
        // The real near side, held facing the scene with lunar north toward the
        // celestial pole, as it is seen from the ground.
        float3 frameX, frameY, frameZ;
        BodyFrame(MoonDirection, frameX, frameY, frameZ);
        const float3 local = float3(dot(normal, frameX), dot(normal, frameY), dot(normal, frameZ));

        uint mapWidth;
        float averageLuma;
        PTERO_MAP_AVERAGE_LUMA(gMoonMap, mapWidth, averageLuma);
        const float3 texel = gMoonMap.SampleLevel(gSkySampler, EquirectUv(local),
                                                  DiscMapLod(mapWidth, MoonDiscSin)).rgb;
        // Keep the map's shading and slight colour, normalised to an average of
        // 0.85 so the disc is as bright as the procedural one was.
        albedo = texel * (0.85f / averageLuma);
    }
    else
    {
        // Surface detail in a frame fixed to the disc.
        float3 tangent = cross(MoonDirection, float3(0.0f, 0.0f, 1.0f));
        tangent = dot(tangent, tangent) > 1e-6f ? normalize(tangent) : float3(1.0f, 0.0f, 0.0f);
        const float3 bitangent = cross(tangent, MoonDirection);
        const float2 p = float2(dot(offset, tangent), dot(offset, bitangent));
        const float maria = smoothstep(0.42f, 0.62f, ValueNoise3(float3(p * 2.2f, 3.7f)));
        const float grain = ValueNoise3(float3(p * 9.0f, 1.3f));
        albedo = (lerp(1.0f, 0.6f, maria) * (0.88f + 0.12f * grain)).xxx;
    }
    const float limb = 0.85f + 0.15f * sqrt(1.0f - r2);

    // A little earthshine keeps the unlit part of a crescent just visible.
    result += MoonColor * albedo * limb * (lit + 0.015f) * outCoverage;
    return result;
}

float2 SafeNormalize2(float2 v)
{
    const float lengthSq = dot(v, v);
    return lengthSq > 1e-10f ? v * rsqrt(lengthSq) : float2(0.0f, 0.0f);
}

float4 PSMain(VSOutput input) : SV_Target
{
    // Reconstruct the view-space ray direction for this pixel, then take it to world space.
    float2 ndcCoord = input.TexCoord * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);

    float4 viewRay4 = mul(float4(ndcCoord, 1.0f, 1.0f), InvProj);
    float3 viewDir  = normalize(viewRay4.xyz / viewRay4.w);
    float3 dir      = normalize(mul(float4(viewDir, 0.0f), InvView).xyz);

    // Sky gradient: blend zenith/horizon based on world elevation (Z up), so
    // pitching the camera no longer drags the horizon with it.
    float elevation = dir.z;
    float t = saturate(elevation * 2.0f); // remap so horizon = 0, mid-sky = 1
    float3 color = lerp(SkyHorizonColor, SkyZenithColor, t * t);

    // Sunset glow, low on the horizon toward the sun.
    const float towardSun = saturate(dot(SafeNormalize2(dir.xy), SafeNormalize2(SunDirection.xy)) * 0.5f + 0.5f);
    color += TwilightGlowColor * pow(towardSun, 4.0f) * exp(-abs(elevation) * 8.0f);

    float moonCoverage = 0.0f;
    const float3 moon = Moon(dir, moonCoverage);

    // Stars sit behind the moon and fade into the murk near the horizon.
    [branch]
    if (StarVisibility > 0.0f && elevation > 0.0f)
    {
        const float horizonFade = saturate(elevation * 10.0f);
        color += StarField(dir) * (StarRadiance * StarVisibility * horizonFade * (1.0f - moonCoverage));
    }

    // The moon adds to the sky rather than covering it: by day it is the pale
    // moon it should be, never a dark hole.
    color += moon;

    // Sun disc: angle between view ray and sun direction, with a soft edge. The
    // max() keeps a disc that has faded out below the horizon from punching a
    // black hole in the sky.
    float cosAngle = dot(dir, SunDirection);
    float sunEdge  = saturate((cosAngle - SunDiscHalfAngleCos + 0.002f) / 0.002f);
    float3 sunDisc = SunColor;
    [branch]
    if (SunTextureOn > 0.0f && sunEdge > 0.0f)
    {
        // Granulation from the map, taken as luminance relative to its average so
        // the colour and brightness stay the time of day's, plus limb darkening
        // (normalised to keep the disc's mean brightness where it was).
        const float discSin = sqrt(max(1.0f - SunDiscHalfAngleCos * SunDiscHalfAngleCos, 1e-8f));
        const float3 offset = (dir - SunDirection * cosAngle) / discSin;
        const float r2 = saturate(dot(offset, offset));
        const float mu = sqrt(1.0f - r2);
        const float3 normal = normalize(offset - SunDirection * mu);

        float3 frameX, frameY, frameZ;
        BodyFrame(SunDirection, frameX, frameY, frameZ);
        const float3 local = float3(dot(normal, frameX), dot(normal, frameY), dot(normal, frameZ));

        uint mapWidth;
        float averageLuma;
        PTERO_MAP_AVERAGE_LUMA(gSunMap, mapWidth, averageLuma);
        const float detail = Luma(gSunMap.SampleLevel(gSkySampler, EquirectUv(local),
                                                      DiscMapLod(mapWidth, discSin)).rgb) / averageLuma;
        const float limbDarkening = (0.5f + 0.5f * mu) / 0.8333f;
        sunDisc *= lerp(1.0f, clamp(detail, 0.0f, 2.0f), 0.8f) * limbDarkening;
    }
    float3 finalColor = lerp(color, max(color, sunDisc), sunEdge);

    return float4(finalColor, 1.0f);
}
