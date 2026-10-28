// LensFlare_Common.hlsli
//
// Physically based lens flare ghosts (Hullin et al. 2011), ported from the reference
// implementation in Source/SDKs/LensFlareFramework-master
// (Assets/Shaders/OpenGL/LensFlare/RayTraceLensFlare, analytical method).
//
// A ghost is light that reflects off two lens surfaces instead of passing straight
// through. For each ghost we trace a grid of parallel rays from the entrance plane
// through the real lens prescription, reflecting at the ghost's two surfaces, and
// rasterise the grid as it lands on the sensor. Where the grid is squeezed together the
// ghost is bright, where it is stretched it is faint; the iris clips it into its shape.
//
// Lens frame: optical axis z, entrance plane at z = gLens.x, sensor at z = 0, rays travel
// towards -z, lengths in millimetres. The lens is rotationally symmetric, so each light is
// traced as if it sat in the xz plane and the result is rotated back by the light's
// azimuth (LensFlareLight.Rotation) - that keeps every ghost's pupil bounds compact.
//
// Must match LensFlareRenderer.h (LensFlareCb) and LensFlareOptics.cpp (the CPU tracer).

#ifndef LENS_FLARE_COMMON_HLSLI
#define LENS_FLARE_COMMON_HLSLI

#define LF_MAX_CHANNELS 6
#define LF_MAX_SURFACES 48
#define LF_MAX_LIGHTS   8
#define LF_MAX_GHOSTS   256

struct LensFlareLight
{
    float4 DirectionLocal; // xyz: ray direction in the azimuth-rotated lens frame
    float4 Rotation;       // x: cos(azimuth), y: sin(azimuth)
    float4 Color;          // rgb: light arriving at the lens, times the flare intensity
    float4 Screen;         // xy: light position in UV, z: occlusion radius (UV, vertical), w: view depth (0 = sun)
    float4 Extra;          // x: visibility slot, y: 1 = reset temporal visibility, z: expected disc luminance (0 = no test)
    float4 Pupil;          // entrance-plane region (min xy, max xy) the direct path gets through; empty when max < min
};

cbuffer LensFlareConstants : register(b0)
{
    float4 gLens;       // x: entrance plane z, y: entrance plane half size, z: entrance pupil area (mm^2), w: unused
    float4 gTanHalf;    // xy: tan of the half screen extent inside the lens, zw: NDC per sensor mm
    float4 gAperture;   // x: blades, y: rotation (rad), z: roundness
    uint4  gCounts;     // x: ray grid N, y: ghosts, z: channels, w: surfaces
    uint4  gCounts2;    // x: lights
    float4 gTarget;     // xy: ghost target size (px), zw: output size (px)
    float4 gOcclusion;  // x: depth tolerance (m), y: projection _33, z: projection _43, w: temporal blend
    float4 gStarburst;  // x: half height (NDC), y: intensity, z: height / width, w: ghost intensity
    float4 gChannel[LF_MAX_CHANNELS];         // rgb: white-balanced weight, w: wavelength (nm)
    uint4  gGhosts[LF_MAX_GHOSTS / 2];        // (first, second) surface pairs, two per entry
    LensFlareLight gLights[LF_MAX_LIGHTS];
    float4 gSurfaces[LF_MAX_CHANNELS * LF_MAX_SURFACES * 2];
};

static const float LF_PI = 3.14159265f;

struct LensFlareGridEntry
{
    float2 Ndc;          // where the ray lands, in screen NDC
    float2 Aperture;     // where it crossed the iris, normalised by the iris height, screen-oriented
    float  Intensity;    // product of the two reflections; < 0 = the ray was lost
    float  RelRadius;    // worst |xy| / clear height over all surfaces (> 1 = clipped by a rim)
};

uint2 LensFlareGhostSurfaces(uint ghost)
{
    const uint4 packed = gGhosts[ghost >> 1];
    return (ghost & 1u) ? packed.zw : packed.xy;
}

// 1 on the boundary of the iris: a regular polygon blended towards a circle.
float LensFlareApertureDistance(float2 p)
{
    const float radius = length(p);
    const float blades = max(gAperture.x, 3.0f);
    const float sector = 2.0f * LF_PI / blades;
    float angle = atan2(p.y, p.x) - gAperture.y;
    angle = angle - sector * floor(angle / sector) - 0.5f * sector;
    const float polygon = radius * cos(angle) / cos(0.5f * sector);
    return lerp(polygon, radius, saturate(gAperture.z));
}

float LensFlareFresnelAR(float theta0, float lambda, float n0, float n1, float n2, float d)
{
    theta0 = max(abs(theta0), 1.0e-3f);

    const float st0 = sin(theta0);
    const float s1 = st0 * n0 / n1;
    const float s2 = st0 * n0 / n2;
    if (abs(s1) >= 1.0f || abs(s2) >= 1.0f)
        return 1.0f;

    const float theta1 = asin(s1);
    const float theta2 = asin(s2);

    const float st01 = sin(theta0 + theta1);
    const float tt01 = tan(theta0 + theta1);

    const float rs01 = -sin(theta0 - theta1) / st01;
    const float rp01 = tan(theta0 - theta1) / tt01;
    const float ts01 = 2.0f * sin(theta1) * cos(theta0) / st01;
    const float tp01 = ts01 * cos(theta0 - theta1);

    const float rs12 = -sin(theta1 - theta2) / sin(theta1 + theta2);
    const float rp12 = tan(theta1 - theta2) / tan(theta1 + theta2);

    const float ris = ts01 * ts01 * rs12;
    const float rip = tp01 * tp01 * rp12;

    const float dy = d * n1;
    const float dx = tan(theta1) * dy;
    const float delay = sqrt(dx * dx + dy * dy);
    const float relPhase = 4.0f * LF_PI / lambda * (delay - dx * st0);
    const float crp = cos(relPhase);

    const float outS2 = rs01 * rs01 + ris * ris + 2.0f * rs01 * ris * crp;
    const float outP2 = rp01 * rp01 + rip * rip + 2.0f * rp01 * rip * crp;
    return (outS2 + outP2) * 0.5f;
}

struct LensFlareTraceResult
{
    float2 Sensor;
    float2 Aperture;
    float  Intensity;
    float  RelRadius;
    bool   Valid;
};

// Traces one ray from the entrance plane to the sensor, reflecting at ghost.x on the way
// in and at ghost.y on the way back (ghost.y < ghost.x). Same algorithm as
// LensFlareOptics::TraceRay.
LensFlareTraceResult LensFlareTraceRay(uint channel, uint2 ghost, float3 position, float3 direction)
{
    LensFlareTraceResult result;
    result.Sensor = 0.0f.xx;
    result.Aperture = 0.0f.xx;
    result.Intensity = 1.0f;
    result.RelRadius = 0.0f;
    result.Valid = false;

    const float lambda = gChannel[channel].w;
    const int surfaceCount = (int)gCounts.w;
    const uint surfaceBase = channel * LF_MAX_SURFACES * 2;

    int phase = 0;
    int delta = 1;
    int index = 1;

    // Two reflections: at most three passes over the lens.
    [loop]
    for (int step = 0; step < LF_MAX_SURFACES * 3; ++step)
    {
        if (index < 1 || index >= surfaceCount)
            break;

        const float4 geometry = gSurfaces[surfaceBase + index * 2 + 0]; // centre z, radius, height, iris
        const float4 media = gSurfaces[surfaceBase + index * 2 + 1];    // n before, n coating, n after, coating thickness

        float3 hit;
        float3 normal;
        if (geometry.y == 0.0f)
        {
            const float t = (geometry.x - position.z) / direction.z;
            hit = position + direction * t;
            normal = float3(0.0f, 0.0f, direction.z > 0.0f ? -1.0f : 1.0f);
        }
        else
        {
            const float3 toCentre = position - float3(0.0f, 0.0f, geometry.x);
            const float b = dot(toCentre, direction);
            const float c = dot(toCentre, toCentre) - geometry.y * geometry.y;
            const float discriminant = b * b - c;
            if (discriminant <= 0.0f)
                return result;

            // The surface is the cap around the vertex; pick the root on that hemisphere.
            const float root = sqrt(discriminant);
            const float vertexZ = geometry.x - geometry.y;
            float t = -b - root;
            if (abs(position.z + direction.z * t - vertexZ) > abs(geometry.y))
                t = -b + root;
            hit = position + direction * t;
            normal = (hit - float3(0.0f, 0.0f, geometry.x)) / abs(geometry.y);
            if (dot(normal, direction) > 0.0f)
                normal = -normal;
        }

        position = hit;

        if (geometry.w > 0.0f)
        {
            result.Aperture = hit.xy / geometry.w;
            index += delta;
            continue;
        }

        result.RelRadius = max(result.RelRadius, length(hit.xy) / geometry.z);

        const float cosI = -dot(direction, normal);
        const bool backwards = (phase & 1) != 0;
        const float n0 = backwards ? media.z : media.x;
        const float n2 = backwards ? media.x : media.z;

        const int reflectAt = phase == 0 ? (int)ghost.x : (phase == 1 ? (int)ghost.y : -1);
        if (index == reflectAt)
        {
            result.Intensity *= LensFlareFresnelAR(acos(clamp(cosI, -1.0f, 1.0f)), lambda, n0, media.y, n2, media.w);
            direction += 2.0f * cosI * normal;
            delta = -delta;
            ++phase;
        }
        else
        {
            const float eta = n0 / n2;
            const float k = 1.0f - eta * eta * (1.0f - cosI * cosI);
            if (k < 0.0f)
                return result;
            direction = eta * direction + (eta * cosI - sqrt(k)) * normal;
        }

        direction = normalize(direction);
        index += delta;
    }

    if (phase < 2)
        return result;

    result.Sensor = position.xy;
    result.Valid = all(isfinite(position.xy)) && isfinite(result.Intensity);
    return result;
}

float2 LensFlareRotate(float2 v, float4 rotation)
{
    return float2(v.x * rotation.x - v.y * rotation.y, v.x * rotation.y + v.y * rotation.x);
}

float LensFlareLuminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

#endif // LENS_FLARE_COMMON_HLSLI
