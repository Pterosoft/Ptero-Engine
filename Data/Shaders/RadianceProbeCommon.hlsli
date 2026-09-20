// RadianceProbeCommon.hlsli
// The radiance probe SH record, and the one place its layout is decoded.
//
// The probe grid is the engine's only world-space irradiance field: unlike the
// screen-space RTGI accumulation buffer it can be queried at an arbitrary
// point, which is what anything volumetric needs. Deferred shading samples it
// per pixel and the volumetric fog samples it per froxel, so the packing, the
// basis normalisation and the grid addressing live here rather than being
// mirrored in both - the same reasoning as LightShapes.hlsli.
//
// The grid lookup deliberately stops at indices and weights instead of taking
// the buffer itself. Resource-typed function parameters need SM6, and the
// deferred lighting pass still compiles as ps_5_0.

#ifndef PTERO_RADIANCE_PROBE_COMMON_HLSLI
#define PTERO_RADIANCE_PROBE_COMMON_HLSLI

// L1 SH, 9 coefficients x RGB, packed into 7 float4s.
struct ProbeSH
{
    float4 c[7];
};

// Band-0 basis, with the cosine-lobe convolution already folded in, so
// EvaluateProbeSH returns irradiance rather than radiance.
#define PTERO_PROBE_DC_BASIS (0.282095f * 3.14159265f)

// Irradiance arriving at a surface with the given normal.
float3 PteroEvaluateProbeSH(ProbeSH sh, float3 dir)
{
    float x = dir.x;
    float y = dir.y;
    float z = dir.z;

    float basis0 = PTERO_PROBE_DC_BASIS;
    float basis1 = 0.488603f * y * 2.09439510f;
    float basis2 = 0.488603f * z * 2.09439510f;
    float basis3 = 0.488603f * x * 2.09439510f;
    float basis4 = 1.092548f * x * y * 0.78539816f;
    float basis5 = 1.092548f * y * z * 0.78539816f;
    float basis6 = 0.315392f * (3.0f * z * z - 1.0f) * 0.78539816f;
    float basis7 = 1.092548f * x * z * 0.78539816f;
    float basis8 = 0.546274f * (x * x - y * y) * 0.78539816f;

    float3 result = float3(0, 0, 0);
    result.r = sh.c[0].x * basis0 + sh.c[0].w * basis1 + sh.c[1].z * basis2 + sh.c[2].y * basis3
             + sh.c[3].x * basis4 + sh.c[3].w * basis5 + sh.c[4].z * basis6 + sh.c[5].y * basis7
             + sh.c[6].x * basis8;
    result.g = sh.c[0].y * basis0 + sh.c[1].x * basis1 + sh.c[1].w * basis2 + sh.c[2].z * basis3
             + sh.c[3].y * basis4 + sh.c[4].x * basis5 + sh.c[4].w * basis6 + sh.c[5].z * basis7
             + sh.c[6].y * basis8;
    result.b = sh.c[0].z * basis0 + sh.c[1].y * basis1 + sh.c[2].x * basis2 + sh.c[2].w * basis3
             + sh.c[3].z * basis4 + sh.c[4].y * basis5 + sh.c[5].x * basis6 + sh.c[5].w * basis7
             + sh.c[6].z * basis8;
    return max(result, 0.0f);
}

// Average radiance over the sphere, for a medium rather than a surface.
//
// A froxel has no normal to take a cosine against, so only the DC term is
// meaningful: the directional bands describe how the irradiance leans, which an
// isotropic phase function integrates away. Dividing by pi undoes the cosine
// lobe the basis folded in, turning irradiance back into the average radiance
// that in-scattering is expressed in.
float3 PteroEvaluateProbeAmbient(ProbeSH sh)
{
    return max(sh.c[0].xyz * (PTERO_PROBE_DC_BASIS / 3.14159265f), 0.0f);
}

// The eight probes surrounding a world position, with trilinear weights.
// Weights always sum to 1, and positions outside the grid clamp to its border
// rather than falling dark.
struct PteroProbeGridTap
{
    uint  Index[8];
    float Weight[8];
};

PteroProbeGridTap PteroProbeGridLookup(uint3 gridSize, float3 origin, float spacing, float3 worldPos)
{
    PteroProbeGridTap tap;

    float3 maxCoord = float3(gridSize) - 1.0f.xxx;
    float3 coordF = clamp((worldPos - origin) / max(spacing, 1e-4f), 0.0f.xxx, maxCoord);
    float3 baseF = floor(coordF);
    float3 frac = saturate(coordF - baseF);

    uint3 baseCoord = uint3(baseF);
    uint3 nextCoord = min(baseCoord + 1u, uint3(maxCoord));

    uint rowStride = gridSize.x;
    uint sliceStride = gridSize.x * gridSize.y;

    [unroll]
    for (uint corner = 0; corner < 8; ++corner)
    {
        uint3 pick = uint3(corner & 1u, (corner >> 1) & 1u, (corner >> 2) & 1u);
        uint3 coord = uint3(
            pick.x ? nextCoord.x : baseCoord.x,
            pick.y ? nextCoord.y : baseCoord.y,
            pick.z ? nextCoord.z : baseCoord.z);

        float3 axisWeight = float3(
            pick.x ? frac.x : (1.0f - frac.x),
            pick.y ? frac.y : (1.0f - frac.y),
            pick.z ? frac.z : (1.0f - frac.z));

        tap.Index[corner] = coord.x + coord.y * rowStride + coord.z * sliceStride;
        tap.Weight[corner] = axisWeight.x * axisWeight.y * axisWeight.z;
    }

    return tap;
}

// True when the grid parameters describe a probe field that can be sampled.
bool PteroProbeGridValid(uint3 gridSize, float spacing)
{
    return gridSize.x > 0 && gridSize.y > 0 && gridSize.z > 0 && spacing > 0.0f;
}

#endif // PTERO_RADIANCE_PROBE_COMMON_HLSLI
