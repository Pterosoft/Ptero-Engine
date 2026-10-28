// TangentFrame.hlsli
// Shared by GBuffer.hlsl (meshes) and Terrain.hlsl so a normal map reads the same on both.

#ifndef PTERO_TANGENT_FRAME_HLSLI
#define PTERO_TANGENT_FRAME_HLSLI

// Builds a tangent frame aligned with the UV layout, derived from screen-space
// derivatives so no tangent vertex attribute is needed. Parallax only makes sense in
// a UV-aligned frame: the height volume is addressed in texture space, so the marching
// direction has to be expressed there too. Also reports how many world units one UV
// unit spans along each axis; parallax only uses this to tell a usable UV
// parameterisation from an unusable one. Meshes whose UVs are degenerate (a flat or
// missing UV set gives zero derivatives) fall back to an arbitrary frame around N and
// report a zero scale, which disables parallax.
void BuildTangentFrame(
    float3 worldPosition,
    float2 uv,
    float3 N,
    out float3 T,
    out float3 B,
    out float2 worldUnitsPerUv)
{
    const float3 dpdx = ddx(worldPosition);
    const float3 dpdy = ddy(worldPosition);
    const float2 duvdx = ddx(uv);
    const float2 duvdy = ddy(uv);

    // Solve for the tangent vectors in the plane perpendicular to N.
    const float3 dpdyPerp = cross(dpdy, N);
    const float3 dpdxPerp = cross(N, dpdx);
    float3 tangent   = dpdyPerp * duvdx.x + dpdxPerp * duvdy.x;
    float3 bitangent = dpdyPerp * duvdx.y + dpdxPerp * duvdy.y;

    // That solve leaves both vectors scaled by the UV determinant. Dividing it back out
    // is what makes them real tangents: their lengths become world units per UV unit,
    // which the parallax march needs to know, and the sign restores the right handedness
    // on mirrored UV shells, where a plain normalize() leaves T and B flipped.
    const float determinant = duvdx.x * duvdy.y - duvdx.y * duvdy.x;

    // The degeneracy test has to be relative. Both the determinant and the derivatives
    // shrink with the pixel's footprint, so the absolute epsilon this used to compare
    // against started rejecting perfectly good UVs once the camera came close enough to
    // a high-resolution texture - which swapped the frame for an arbitrary one and sent
    // the march off in a direction unrelated to the texture.
    const float uvDerivativeScale = dot(duvdx, duvdx) + dot(duvdy, duvdy);
    if (uvDerivativeScale <= 0.0f || abs(determinant) < 1e-8f * uvDerivativeScale)
    {
        const float3 up = (abs(N.z) < 0.999f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
        T = normalize(cross(up, N));
        B = cross(N, T);
        worldUnitsPerUv = float2(0.0f, 0.0f);
        return;
    }

    tangent   /= determinant;
    bitangent /= determinant;

    worldUnitsPerUv = max(float2(length(tangent), length(bitangent)), 1e-20f);
    T = tangent   / worldUnitsPerUv.x;
    B = bitangent / worldUnitsPerUv.y;
}

#endif // PTERO_TANGENT_FRAME_HLSLI
