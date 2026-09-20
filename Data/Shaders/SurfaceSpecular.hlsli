// SurfaceSpecular.hlsli
// The material "Specular" knob and how it becomes a surface's normal-incidence
// reflectance (F0). Shared by every pass that needs an F0 - the deferred resolve, screen
// space reflections and the ray-traced specular pass - because a surface that reflects one
// amount under direct light and a different amount in its reflections stops reading as one
// material.
//
// The knob travels through the G-Buffer in the normal target's W channel. That target is
// R32G32B32A32_FLOAT and only XYZ were spoken for (oct normal + device depth), so the
// channel was free; the material target is RGBA8 and its alpha is reserved for the glass
// re-pack, which is why the value does not live there.

#ifndef PTERO_SURFACE_SPECULAR_HLSLI
#define PTERO_SURFACE_SPECULAR_HLSLI

// Neutral reflectivity. Reproduces the textbook 0.04 dielectric F0 and leaves a metal
// reflecting exactly its base colour, so a material that never touches the slider shades
// the way it always did.
static const float kPteroDefaultSpecular = 0.5f;

// Floor applied when storing, so a surface an artist deliberately set to zero specular
// still stores something non-zero and stays distinguishable from a channel nobody wrote.
static const float kPteroMinStoredSpecular = 0.001f;

float PteroEncodeSurfaceSpecular(float specular)
{
    return max(saturate(specular), kPteroMinStoredSpecular);
}

// The G-Buffer clears to zero, so a pixel produced by a pass that does not carry the
// specular channel reads back exactly 0. That has to mean "use the default" rather than
// "this surface reflects nothing", which would quietly flatten those pixels instead.
float PteroDecodeSurfaceSpecular(float packedSpecular)
{
    return (packedSpecular > 0.0f) ? packedSpecular : kPteroDefaultSpecular;
}

// Normal-incidence reflectance.
//
// `specular` scales the whole reflectance rather than only the dielectric half. At the 0.5
// default this is the usual lerp(0.04, albedo, metallic); below it the surface loses its
// highlight, above it gains one. Scaling the metallic end is the point of the knob: a
// fully metallic surface has no diffuse term at all, so with nothing bright enough in the
// scene for it to mirror it resolves to black, and this is what lets an artist push the
// reflection back up without faking it through the base colour.
float3 PteroComputeF0(float3 albedo, float metallic, float specular)
{
    return saturate(lerp(0.04f.xxx, albedo, metallic) * (specular * 2.0f));
}

#endif // PTERO_SURFACE_SPECULAR_HLSLI
