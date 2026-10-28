#pragma once

// ShadingSettings
// Scene-wide choice of surface shading model, edited in the renderer settings window and
// persisted with the level.
struct ShadingSettings
{
    // true  = the Disney principled BRDF (Burley 2012, PrincipledBrdf.hlsli): Burley's
    //         retro-reflective diffuse, GTR2 specular with Disney's masking, and the
    //         material lobes it adds - subsurface, specular tint, anisotropy, sheen and
    //         clearcoat.
    // false = the original model: Lambert diffuse weighted by (1 - F) and Cook-Torrance
    //         GGX specular. The materials' principled lobes are ignored.
    // Applies to the deferred resolve and to the path tracer.
    bool DisneyBrdf = true;
};
