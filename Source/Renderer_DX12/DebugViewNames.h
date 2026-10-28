#pragma once

// The names of every renderer system's debug views, indexed by the system's DebugView
// value unless a Values table says otherwise. Shared by each system's settings panel
// (Graphics Settings) and the viewport's View Mode menu, so the two can never disagree.
// Index 0 is always the system's ordinary output.
namespace DebugViewNames
{
    // --- Global illumination ---------------------------------------------------------

    // The diagnostic ids are not contiguous with the display modes, hence RtgiValues.
    inline constexpr const char* Rtgi[] =
    {
        "Final Lit",
        "Raw GI Radiance",
        "GI Luminance",
        "Specular Reflections",
        "Diag: World Position",
        "Diag: G-Buffer Normal",
        "Diag: G-Buffer Depth",
        "Diag: Flat White",
        "Diag: Deterministic Probe",
        "Diag: NEE Sun Visibility",
        "Diag: NEE Shadow Map Coverage",
    };
    inline constexpr int RtgiValues[] = { 0, 1, 2, 3, 10, 11, 12, 13, 14, 15, 16 };

    inline constexpr const char* VoxelGi[] =
    {
        "Final Lit",
        "Raw GI Irradiance",
        "GI Luminance",
        "Reflections",
        "Voxels: Albedo",
        "Voxels: Radiance",
        "Voxels: Clipmap Level",
    };
    inline constexpr int VoxelGiValues[] = { 0, 1, 2, 3, 20, 21, 22 };

    inline constexpr const char* RadianceCascades[] = { "Final Lit", "Radiance", "Luminance" };

    // The reference (offline) mode has only the first PathTracerReferenceCount.
    inline constexpr const char* PathTracer[] =
    {
        "Final Image", "Base Colour", "Shading Normal", "Roughness / Metallic",
        "Diffuse Lighting", "Specular Lighting",
    };
    inline constexpr int PathTracerReferenceCount = 4;

    // --- Ambient occlusion -----------------------------------------------------------

    inline constexpr const char* Rtao[] = { "Final AO", "Raw Noisy AO", "NRD AO" };
    inline constexpr const char* Gtao[] = { "Disabled", "AO Only" };
    inline constexpr const char* Dple[] =
    {
        "Off",
        "1 - Ambient Visibility",
        "2 - Contact Shadows",
        "3 - Indirect Fraction",
        "4 - Material Class",
        "5 - AO Radii (RGB)",
        "6 - G-Buffer UV Mapping",
        "7 - Specular Share",
        "8 - Micro Specular Gain",
    };

    // --- Reflections, subsurface -----------------------------------------------------

    // SsrSettings::DebugView, read by whichever technique is selected.
    inline constexpr const char* Ssr[]  = { "Composite", "Reflection Only", "Confidence Mask" };
    inline constexpr const char* Sssr[] = { "Composite", "Reflection Only", "Raw Trace (Denoiser Off)" };
    inline constexpr const char* Subsurface[] = { "Composite", "Scattered Diffuse Only", "Profile Mask" };

    // --- Shadows ---------------------------------------------------------------------

    inline constexpr const char* VirtualShadowMap[] = { "Lighting", "Clipmap Level", "Sun Visibility" };
    inline constexpr const char* PointShadows[] = { "Lighting", "Shadow Factor" };

    // --- Volumetrics -----------------------------------------------------------------

    inline constexpr const char* VolumetricFog[] = { "Composite", "Scattering", "Transmittance" };
    inline constexpr const char* VolumetricClouds[] =
    {
        "Composite", "Luminance", "Transmittance", "Coverage", "Cloud Distance",
    };

    // --- Anti-aliasing ---------------------------------------------------------------

    inline constexpr const char* Smaa[]       = { "Final Output", "Edges", "Blend Weights" };
    inline constexpr const char* EdgeAa[]     = { "Final Output", "Classification", "Coverage" };
    inline constexpr const char* Aar[]        = { "Final Output", "Reconstruction Path", "Coverage" };

    // --- Geometry --------------------------------------------------------------------

    inline constexpr const char* VirtualGeometry[] = { "Off", "Clusters", "Instances", "LOD (DAG level)" };
}
