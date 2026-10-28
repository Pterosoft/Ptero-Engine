// RtTransform.hlsli
// Object-to-world transforms for ray-traced hits (RayQuery CommittedObjectToWorld3x4).
//
// m[r] is row r of the 3x4 matrix M with world = M * float4(p, 1), so a point is
// dot(m[r], float4(p, 1)) per component - which every pass already did. Normals
// are where they went wrong: they used to be mul(o2wRot, n) with o2wRot's rows
// taken from M's *columns*, which is M^T * n - the inverse rotation. Facing the
// normal toward the ray hid it for 90 and 180 degree turns (M^T n = +-M n there);
// any other angle shaded the hit with a normal tilted the wrong way, e.g. a wall
// yawed 30 degrees as if yawed -30.

#ifndef PTERO_RT_TRANSFORM_HLSLI
#define PTERO_RT_TRANSFORM_HLSLI

float3 PteroObjectToWorldPoint(float3x4 m, float3 p)
{
    return float3(dot(m[0], float4(p, 1.0f)), dot(m[1], float4(p, 1.0f)), dot(m[2], float4(p, 1.0f)));
}

// Normals transform by the inverse transpose of M's linear part. Up to the
// determinant - a positive scale, or a sign every caller discards by facing the
// normal toward the ray or the triangle - that is the cofactor matrix: with M's
// columns c0, c1, c2, its columns are c1 x c2, c2 x c0 and c0 x c1. Unlike M
// itself it stays perpendicular to the surface under non-uniform scale.
// Not normalized: the caller does that (and may interpolate first).
float3 PteroObjectToWorldNormal(float3x4 m, float3 n)
{
    const float3 c0 = float3(m[0][0], m[1][0], m[2][0]);
    const float3 c1 = float3(m[0][1], m[1][1], m[2][1]);
    const float3 c2 = float3(m[0][2], m[1][2], m[2][2]);
    return n.x * cross(c1, c2) + n.y * cross(c2, c0) + n.z * cross(c0, c1);
}

#endif // PTERO_RT_TRANSFORM_HLSLI
