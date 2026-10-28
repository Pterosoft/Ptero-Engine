# Ray-traced shadows and ambient occlusion

Two DXR passes that trace the scene's acceleration structure (the TLAS RTGI builds) with inline `RayQuery` compute shaders. Both need a GPU with DirectX Raytracing; without one they stay off and the shadow maps and GTAO are used.

## Ray-traced ambient occlusion (RTAO)

**Editor:** Graphics Settings → *Ray Traced Ambient Occlusion*. **Console:** `rtao.*`. **Saved with the level:** `RtAOSettings` in the scene JSON.

Each pixel fires *Rays Per Pixel* cosine-weighted rays up to *Max Ray Length*; the fraction that escape, raised to *AO Power*, is the AO. NRD (RELAX) denoises it. *AO Intensity* blends it over the G-Buffer's baked AO.

- **RTAO replaces GTAO.** While RTAO can run, GTAO is skipped. Previously GTAO (on by default) overwrote RTAO's result, so enabling RTAO changed nothing on screen.
- **Local Light Occlusion** (default 0.5): AO darkens sky ambient and GI only by default. In an interior lit by lamps or candles that is almost nothing, so this also lets AO darken point, spot and rect lights' direct light. 0 restores the old behaviour. The sun is never darkened by it, since it has its own shadow.
- **Debug View:** Final AO, Raw Noisy AO (one frame, before NRD), NRD AO.

## Ray-traced shadows

**Editor:** Graphics Settings → *Ray-Traced Shadows*. **Console:** `shadow.rt.*`. **Saved with the level:** `RtShadowSettings` in the scene JSON. Code: `Source/Renderer_DX12/RtShadows.*`, shaders `Data/Shaders/RtShadow_*.hlsl`.

They replace the shadow-map *lookups* in the deferred lighting pass. The shadow maps still render, because volumetric fog, RTGI bounce lighting and subsurface transmission read them. They are off whenever shadows are (`shadows.enabled`, or Shadows Off in a game's settings) and while the real-time path tracer runs. The shadow budget does not limit them: every light with Cast Shadows on is traced, up to 8 per pixel.

### Sun

Rays go toward points spread over the sun's disc (*Sun Angular Size*, about 0.53° for the real sun; 0 gives hard shadows), so penumbrae widen with the distance to the caster. The sun's shadow is then denoised:

1. **Temporal:** reprojects last frame's result and blends in this frame's rays (*Temporal Blend*). History is dropped per pixel where the surface moved (its stored camera distance no longer matches), and clamped to the current frame's neighbourhood so moving casters don't smear.
2. **Spatial:** an edge-aware blur whose radius follows the penumbra width (blocker distance × sun size), capped by *Max Filter Radius*. Pixels that just lost their history get a wider blur.

### Point, spot and rect lights

With *Local Lights* on, each pixel walks its clustered light list, the same one the lighting pass uses. It traces the first **8** shadow-casting lights that can reach it, toward points on the emitter: a disc of the light's *Source Radius*, or the rect panel. *Light Softness* scales the emitter size (0 = hard). Any further lights on that pixel keep their shadow maps.

Each light's result is stored with its light index, so the lighting pass finds it by index and lights never swap shadows. The local shadows are denoised per light the same way as the sun's: temporal blending matched by light index, then an edge-aware spatial blur.

Rays stop 5 cm short of a spherical light, like the shadow map's near plane, so a bulb inside its own lamp mesh isn't shadowed by the mesh.

### Settings

| Setting | Default | Meaning |
|---|---|---|
| Enabled | off | Ray trace the sun's shadow. |
| Local Lights | on | Also ray trace point, spot and rect lights (8 per pixel). |
| Rays Per Pixel | 1 | Rays per light per pixel per frame. The denoisers make 1 usually enough; raise it for wide, noisy penumbrae. |
| Sun Angular Size | 0.53° | Sun penumbra size. |
| Light Softness | 1.0 | Local lights' emitter size multiplier. |
| Max Ray Length | 500 m | Sun casters further away cast nothing. |
| Ray Bias | 0.01 m | Minimum ray offset. It grows automatically at grazing light angles. |
| Temporal Blend | 0.1 | Current-frame weight in the history: lower is smoother, but lags more behind moving casters. |
| Max Filter Radius | 8 px | Widest sun penumbra blur. |
| Debug View | — | Sun Visibility, Raw Sun Rays, Local Light Visibility (the darkest traced light), Raw Local Light Rays (before denoising). |

## Ray origins (both passes)

Rays don't start from the G-Buffer position as-is:

- **Snapped to the traced surface.** The ray-traced scene differs slightly from the rasterised one: virtualized meshes are traced against a coarser cut through their cluster DAG, and terrain displacement exists only in the raster pass. So a camera ray first finds the traced surface behind the pixel, and rays start just in front of it (`Data/Shaders/RtSurfaceSnap.hlsli`, the same idea as RTGI's `ComputePrimaryRayOrigin`).
- **Offset along the geometric normal, not the normal-mapped one.** For shadows the offset grows as the light grazes the surface, as RTGI's shadow rays do. A candle standing on a table lights it at a grazing angle; offset along a bumped normal, its rays skim back into the table and speckle it with self-shadowing. AO rays that point into the true surface are mirrored back out.

## Performance

Roughly: the sun costs two rays per pixel (surface snap plus shadow). Local lights cost one snap ray plus *Rays Per Pixel* × the traced lights per pixel. RTAO costs one snap ray plus *Rays Per Pixel* (16 by default). The passes run at render resolution.

## Implementation notes

- Each pass keeps its per-frame constants in a **ring of 8 constant-buffer slots**. With a single slot, the CPU rewrote next frame's camera matrices while the GPU was still tracing the previous frame. That was invisible with a still camera, but while moving or rotating rays started inside surfaces and the image darkened, most toward the screen edges. Any new pass with per-frame upload constants needs the same ring (DeferredLightingPass, RtGlobalIllumination and NrdDenoiser have one too).
- History is not reset when meshes stream in a new LOD or the virtual-geometry instance list changes, which happens continuously while the camera moves. The temporal passes already reject history per pixel.
- Lighting pass bindings: `t14` sun visibility (R16F), `t15` local light pairs (RGBA32_UINT, eight `index << 8 | visibility` entries, `0xFFFF` empty), root parameters 21 and 22.

## Farkle settings menu

Settings → Lighting:

- **AO Method:** Screen-Space (GTAO, default) or Ray Traced (RTAO).
- **Ray-Traced Shadows:** Off (default), Sun, or Sun + Lights.

Both are only offered on a DXR GPU, and are greyed out while Ambient Occlusion or Shadows is Off. Ambient Occlusion's tiers also set RTAO's rays per pixel (Low at most 4, Medium at most 8, High the level's own, Ultra at least 32). Details are in `Data/UI/Farkle/README.md`.
