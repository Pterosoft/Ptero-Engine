# Ray-traced shadows and RTAO: implementation notes

What they are and how to use them is in the manual: [8. Shadows → Ray-traced shadows](Manual/08-Shadows.md#ray-traced-shadows),
[7 → Ray-traced AO](Manual/07-Global-Illumination.md#ray-traced-ao) and the
[CVar reference](Manual/CVar-Reference.md). This page is for working on the code.

## Where things are

| | Code | Shaders |
| --- | --- | --- |
| Ray-traced shadows | `Source/Renderer_DX12/RtShadows.*`, `RtShadowSettings.h` | `Data/Shaders/RtShadow_*.hlsl`, `RtShadow_Common.hlsli` |
| RTAO | `Source/Renderer_DX12/RtAmbientOcclusion.*`, `RtAOSettings.h` | `Data/Shaders/RtAO_*.hlsl` |
| Ray origins (both) | | `Data/Shaders/RtSurfaceSnap.hlsli` |

Both trace the TLAS that `RtGlobalIllumination::BuildTlas` builds once per frame, with inline
`RayQuery` in `cs_6_5` compute shaders, and are dispatched from `DX12SceneRenderer::Render`
after RTGI.

## Passes

Ray-traced shadows, one root signature for all six:

| Pass | Reads | Writes |
| --- | --- | --- |
| RayGen (sun) | G-Buffer normal + depth, TLAS | raw visibility + blocker distance |
| Temporal (sun) | raw, previous history | history: visibility, blocker distance, camera distance, frame count |
| Spatial (sun) | history | final sun visibility (R16F) |
| Local | G-Buffer, TLAS, light clusters (space 7) | raw local pairs |
| LocalTemporal | raw local pairs, previous local history, previous sun history (for the camera distance) | local history |
| LocalSpatial | local history | final local pairs |

- **Local pairs** (RGBA32_UINT): eight 16-bit entries `light index << 8 | visibility * 255`,
  `0xFFFF` = empty. The local history keeps eight 32-bit entries `index << 24 | visibility *
  65535 << 8 | frames` over two RGBA32_UINT textures. Everything is matched by light index, so
  the deferred pass need not agree with the trace on cluster order.
- The deferred lighting pass reads them at `t14` (sun) and `t15` (local), root parameters 21
  and 22, and falls back to the shadow maps for any light not in the pairs.
- RTAO: RayGen, then NRD RELAX (occlusion packed as diffuse radiance), then a composite.

## Things that went wrong once

- **Upload constant buffers must be ring-buffered.** Both passes once wrote their per-frame
  constants into a single persistently mapped slot. The CPU then rewrote the next frame's camera
  matrices while the GPU still traced the previous frame. Nothing showed with a still camera,
  but while moving or rotating, rays started inside surfaces: speckle and darkening, worst at
  the screen edges. Both now keep a ring of 8 slots, as `DeferredLightingPass`,
  `RtGlobalIllumination` and `NrdDenoiser` do. Any new pass with per-frame upload constants needs
  the same.
- **No history reset on `sceneContentChanged`.** That flag fires whenever a mesh LOD streams in
  or the virtual-geometry instance list changes, which is continual while the camera moves. The
  temporal passes reject history per pixel instead.
- **Reconstruct with the jittered inverse view-projection.** The G-Buffer depth is rasterised
  with the TAA/upscaler jitter. Reprojection uses the non-jittered matrices.
- **Rays start on the traced surface**, found by a camera ray (`RtSurfaceSnap.hlsli`, as RTGI's
  `ComputePrimaryRayOrigin` does). They leave along the geometric normal from the depth buffer,
  not the shading normal, with an offset that grows at grazing angles. Otherwise
  coarser virtual-geometry cuts and normal maps cause self-occlusion.
- **RTAO replaces GTAO** whenever it can run (`gtaoWillRun = mGtaoSettings.Enabled && !rtaoWillRun`).
  GTAO is on by default and used to overwrite RTAO's result.
