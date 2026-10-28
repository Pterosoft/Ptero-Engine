# PteroRender.exe — standalone offline renderer (plan)

Status: proposal, 2026-10-19. Nothing implemented yet.

## Goal

A command-line app that loads a level, plays a Ptero Sequencer sequence (or holds one
camera for a still), and writes frames to disk at a fixed frame rate. It does not depend
on how long the GPU takes. Quality runs from "same as the viewport" up to "path-traced
reference at N samples per pixel". It doesn't need Qt or the editor, and it can run from
a script or a render farm.

```
PteroRender.exe --level Levels/Desert.json --sequence Cinematics/Desert.sequence
                --out Renders/Desert --format mp4 --size 3840x2160 --fps 30
                --frames 0-300 --mode pathtrace --spp 1024
PteroRender.exe --job Renders/desert-4k.json
```

## What already exists (so this is mostly assembly, not a new renderer)

| Piece | Where | Reuse |
|---|---|---|
| Qt-free renderer build | `Renderer_DX12.vcxproj`, `PteroGameRuntime=true` (QtUiHeadless, NodeGraph/Sequencer headless stubs) | The renderer DLL PteroRender loads |
| Minimal host exe | `Source/Ptero-Engine/GameLauncher/main.cpp` | Template for the new exe: window, LoadLibrary, export lookup, render loop |
| Offline render job | `Renderer_DX12/EditorCinematics.cpp` (`CinematicRenderJob`) | Pose/capture pipeline, warm-up, frame latency, writer thread pool, ffmpeg MP4 |
| Fixed simulation step | `DX12SceneRenderer::SetFixedFrameDelta` | Deterministic time per frame |
| GPU readback + PNG/JPEG | `Renderer_DX12/FrameCapture.*` | Output (8-bit) |
| Sequence playback without the editor | `System/Cinematics/SequencePlayer.h` + `CinematicWorldHost` | Posing the level at time *t* (`SetTime` + `Evaluate`) |
| Reference path tracer | `PathTracer.*`: `ResetAccumulation()`, `GetSampleCount()`, `MaxSamples` | High-quality mode |

The one coupling problem: the render job is a member of `Editor`, and it poses the level
through `SequencerEditor::SetCinematicRenderTime`. That function is a no-op stub in the
runtime build (`SequencerEditorHeadless.cpp`). So the job has to come out of `Editor`, and
it has to pose through `SequencePlayer`/`CinematicWorldHost`, which work without Qt.

## Architecture

```
PteroRender.exe  (new, console subsystem)
  parse CLI / job JSON
  hidden window ─► Renderer_DX12.dll (PteroGameRuntime build)
                     RendererDX12_ConfigureOfflineRender(jobJson)   ◄ new export
                     RendererDX12_Initialize(hwnd)
                     loop: RendererDX12_Render()
                           RendererDX12_GetOfflineRenderStatus(&s)  ◄ new export
                     print progress, exit code from status
```

Inside the DLL, a new `OfflineRenderJob` (taken out of `CinematicRenderJob`) owns:
- the slot list (warm-up, frames, flush) and the in-flight capture latency. Move this
  logic as it is. It already accounts for readback returning the last finished frame, so
  don't redesign it.
- the writer thread pool and the ffmpeg encoder thread
- a `CinematicWorldHost` + `SequencePlayer` to pose the level and supply the cut camera
- a small interface back to the frame loop: `PoseFor(time)`, `Capture(ID3D12Resource*)`,
  `WantsAnotherSubframe()` (for path tracing)

The editor's Render Sequence then becomes a thin client of `OfflineRenderJob`, so there is
one implementation, not two that drift apart.

## Phases

### Phase 1: extract the job (no new features, editor behaviour unchanged)
1. Move `CinematicRenderJob`, `RunEncoder` and the writer pool from `EditorCinematics.cpp`
   into `Renderer_DX12/OfflineRenderJob.{h,cpp}`. Add both files to the vcxproj for **both**
   build flavours.
2. Replace the job's calls into `Editor` (`RequestViewportResolution`,
   `StopPilotingCamera`, `SequencerEditor::*`) with a small `OfflineRenderTarget`
   interface. The editor implements it with its current calls.
3. Done when the editor's Render Sequence still produces the same Desert MP4, and Debug
   x64 and `PteroGameRuntime=true` both build. Bare `std::max` breaks the runtime build:
   use `(std::max)`.

### Phase 2: PteroRender.exe, raster mode (parity with Render Sequence)
1. `Source/PteroRender/PteroRender.vcxproj`: copy GameLauncher's bootstrap, minus the
   packaging key, `.ppak` mounting, icon resource and audio. Read `Data/` from disk the way
   Editor.exe does. (Check that `System/DataFiles` falls back to disk when the host exports
   no `PteroData_*`.) Keep `CoInitializeEx` before init: the legacy D3DCompiler hangs
   without it.
2. Window: a hidden `WS_POPUP` window at the render size. The swap chain still exists and
   is presented with vsync off, but nobody sees it. `RendererDX12_Initialize` needs an HWND,
   so true windowless is a later refinement (see Risks).
3. New exports in `DX12RendererAPI.cpp`:
   - `RendererDX12_ConfigureOfflineRender(const char* jobJson)`, called before init.
     It sets the level like `ConfigureStandaloneGame`, plus an `gOfflineRender` flag.
   - `RendererDX12_GetOfflineRenderStatus(OfflineRenderStatus*)` returns frames done/total,
     stage, message and failed.
4. In the standalone branch of `RendererDX12_Render`, when `gOfflineRender` is set:
   - skip the player's display-mode setup (borderless/fullscreen), the warm-up-until-smooth
     gate (`gStandaloneStarted`/`hideSceneUntilIntro`), the game session start and RmlUi
     menus
   - load the level, fix the scene resolution to the job size, and start the job
   - after the scene renders, call `job.Update(gSceneRenderer->GetSceneColorTargetResource())`
     where the editor calls `UpdateCinematicRender`
5. CLI: `--level --sequence --camera <entity> --out --name --format png|jpg|mp4 --size
   --fps --frames a-b --warmup --quality`, or `--job file.json` with the same keys.
   Exit code 0 means OK, 1 bad arguments, 2 load failure, 3 render or write failure.
   Print progress lines to stdout.
6. Done when `PteroRender.exe` renders Desert frames 0–30 to PNG at 1080p, and they match
   the editor's Render Sequence frames for the same settings (pixel diff within TAA noise).

### Phase 3: path-traced reference mode
1. Job option `"mode": "pathtrace"`, `"spp": N`. Per output frame: pose, then
   `PathTracer::ResetAccumulation()` (otherwise the history smears across frames). Hold the
   scene clock (fixed delta 0) and render until `GetSampleCount() >= spp`, then capture.
   Use `SamplesPerFrame = 16` to cut the number of frames per output frame.
2. Expose `--bounces`, `--indirect-clamp`, and `--denoise` (an NRD pass on the final
   accumulation for low spp, optional).
3. The capture-latency logic must count *submitted frames since the last pose*, not output
   frames. That is the main change to the moved pipeline.
4. Needs DXR. Fail cleanly with a clear message on non-DXR GPUs.

### Phase 4: HDR output and motion blur (optional)
1. **EXR/HDR**: read back a scene-linear target before tonemapping (RGBA16F/32F) with a new
   `FrameCapture::ReadbackFloat`. Today's readback clamps to 8-bit [0,1]. Write EXR with
   tinyexr (single header, vendored into `SDKs/`), or Radiance `.hdr` with DirectXTex,
   which is already in `SDKs/`. Divide out PreExposure: GI runs pre-exposed (see memory
   notes). Document that values are scene-linear, not photometric.
2. **Motion blur**: K subframes spread across the shutter interval (`--shutter 0.5`),
   accumulated in a float buffer before writing. Combined with path tracing, spread the
   spp across the subframes.
3. **AOVs** (depth, normals, albedo) from the G-Buffer via `GetDebugSrvs()`, for compositing.

## Risks / things to verify early

- **Swap chain without a visible window.** A hidden window should work. If the DXGI or
  Streamline/FSR frame-gen hooks misbehave, disable frame generation and DLSS-FG in offline
  mode outright. They add frames and make no sense here.
- **Standalone path assumptions.** `ConfigureStandaloneGame` also flips
  `QtUi::SetStandaloneGame` and the console panel. Offline mode should share the level
  loading but none of the game-session or input behaviour. Keep it a separate flag, not
  a variant of the game path.
- **Shaders load from disk.** PteroRender needs `Shaders/` next to it (or the repo path).
  A stale DLL with an edited shader drops all geometry silently.
- **Frames in flight.** The render job's latency handling is the subtle part. Move it as
  it is, and test the first/last frames of a sequence explicitly: off-by-one shows up as a
  duplicated first frame or a missing last frame.
- **Determinism.** Particles, water and light styles respect `SetFixedFrameDelta`. Check
  that physics (Jolt, interpolated 60 Hz) and the vegetation wind do too, or a
  re-render won't match.
- **Upscalers.** Default offline mode to native resolution with TAA or path tracing.
  Allow DLSS/XeSS only as an explicit opt-in.

## Testing

- PteroRender is a CLI, so it can run headless on the RTX 3060 (the same box used for the
  RT pass harness). No editor GUI is involved.
- Golden check: 5 frames of Desert at 640×360, raster mode. Compare against images from
  the editor's Render Sequence, with a small per-pixel tolerance.
- Path trace check: one still at 64 spp. Mean luminance within a few % of the editor's
  reference path tracer at 64 spp.
