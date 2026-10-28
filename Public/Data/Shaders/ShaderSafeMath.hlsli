// ShaderSafeMath.hlsli
// Numerically defensive helpers for the ray-traced passes.
//
// Path tracing is unusually unforgiving of a single bad value. A normal that
// comes back NaN does not just shade one pixel wrong - it becomes the next
// bounce's ray origin and direction, so the whole path is lost, and in a
// resampling scheme like ReSTIR the poisoned sample is then reused by
// neighbouring pixels and by later frames. One degenerate triangle can show up
// as flickering black blotches across a whole surface.
//
// Degenerate triangles are not rare: LOD simplification readily emits slivers
// whose edge cross-product collapses to zero, and imported meshes can carry
// zero-length vertex normals. So the ray paths guard rather than assume.

#ifndef PTERO_SHADER_SAFE_MATH_HLSLI
#define PTERO_SHADER_SAFE_MATH_HLSLI

// Normalize, falling back when the input is degenerate or non-finite.
//
// The range comparison rejects NaN for free, because NaN fails every
// comparison. That matters more than it looks: isnan() is something an
// optimiser is permitted to fold away under fast-math assumptions, whereas a
// plain comparison survives.
float3 SafeNormalizeOr(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return (lengthSquared > 1e-12f && lengthSquared < 1e30f)
        ? (value * rsqrt(lengthSquared))
        : fallback;
}

// Reject NaN, infinity and negative radiance in one test, again by relying on
// NaN failing every comparison. Radiance is never legitimately negative, so a
// negative value is as much a symptom of corruption as a NaN is.
float3 SanitizeRadiance(float3 radiance)
{
    const bool finite =
        radiance.x >= 0.0f && radiance.x <= 1e30f &&
        radiance.y >= 0.0f && radiance.y <= 1e30f &&
        radiance.z >= 0.0f && radiance.z <= 1e30f;
    return finite ? radiance : float3(0.0f, 0.0f, 0.0f);
}

// True when a position is usable as a ray origin. A NaN origin makes the whole
// TraceRayInline call undefined, so it is worth one check before spawning the
// next bounce.
bool IsFinitePosition(float3 position)
{
    return position.x >= -1e30f && position.x <= 1e30f
        && position.y >= -1e30f && position.y <= 1e30f
        && position.z >= -1e30f && position.z <= 1e30f;
}

#endif // PTERO_SHADER_SAFE_MATH_HLSLI
