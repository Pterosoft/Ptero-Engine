#pragma once

// Scene-wide switches for virtualized geometry (VirtualGeometryRenderer).
// Whether a given mesh is virtualized is its own MeshComponent::VirtualizedGeometry
// flag; these only govern how the system behaves once it is.
struct VirtualGeometrySettings
{
    // Master switch. Off draws every virtualized entity the ordinary way, with
    // its authored LODs - the quickest A/B comparison there is.
    bool  Enabled = true;

    // Draw every eligible mesh through virtualized geometry, whatever its own
    // flag says. A viewing switch for trying the system on a whole level; it
    // is not saved with the level.
    bool  VirtualizeAllMeshes = false;

    // Largest simplification error a cluster may show, in pixels. The whole
    // point of the system: detail follows screen size, not distance bands.
    // Lower is sharper and costs more triangles; 1 is visually lossless.
    float ErrorThresholdPixels = 1.0f;

    // Two-pass hierarchical-Z occlusion culling of clusters. Unavailable with
    // MSAA, whose depth buffer the HZB build does not read.
    bool  OcclusionCulling = true;

    // Skip clusters whose every triangle faces away from the camera.
    bool  BackfaceCulling = true;

    // Mesh shaders when the GPU has them; off forces the vertex shader path
    // every GPU can run, for comparison.
    bool  MeshShaders = true;

    // Virtualized meshes cast shadows from their clusters too. Off leaves the
    // shadow passes drawing them the ordinary way.
    bool  Shadows = true;

    // 0 off, 1 clusters, 2 instances, 3 DAG level (LOD).
    int   DebugView = 0;

    // Keep culling and LOD selection from the camera as it was when this was
    // switched on, while rendering from the live camera: fly out and look at
    // what was selected.
    bool  FreezeCulling = false;
};
