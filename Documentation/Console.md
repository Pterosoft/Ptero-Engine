# Console, logging and cvars

The Console panel docks on the left, tabbed beside Components (`Windows → Console`). It shows everything the engine logs and takes commands against every renderer setting.

Three things share one design: the log is the single stream, the Console is a view onto it, and a cvar is a name bound to the live variable. Nothing is mirrored, so nothing can drift.

## The log

`PteroLog` (`Source/System/include/System/PteroLog.h`) is the only sink. It lives in System rather than the renderer because it is not a renderer facility - asset import, the node graph and the editor shell all have things to say. Every line goes three places at once: an in-memory ring the Console renders, a session `.txt` under `Logs/`, and `OutputDebugStringA` for an attached debugger.

The file is the point. A renderer fault that takes ten minutes of editing to provoke is worth nothing if the evidence dies with the process, so the sink is written through rather than buffered to the end of the session:

- anything at Warning or above flushes immediately,
- ordinary traffic flushes every 200 ms, and once per frame from the render loop,
- an unhandled exception writes what it can through `SetUnhandledExceptionFilter` and closes the file before the process goes down,
- `std::terminate`, `atexit` and the console control handler (logoff, shutdown, Ctrl+C) all close it cleanly.

That covers everything except Task Manager's **End process**, which is `TerminateProcess` — no code runs, ever, in any application. Write-through is the only defence against it, and it bounds the loss to the current frame.

Files are named `Ptero_<date>_<time>.txt`. The thirty most recent are kept and older ones are deleted at startup, so the directory does not become an archive. `logpath` prints the current one.

### Watchdog

A frame that never returns writes nothing, because the thread that would write the line is the thread that is stuck. So a second thread watches: the render loop stamps a heartbeat each frame, and if the stamp stops moving for five seconds the watchdog writes

```
[Watchdog] Render thread has not completed frame 4821 for 5.0 s.
```

and flushes. A hang therefore leaves a record of which frame it died on, and everything above that line is what the renderer did last. It reports again if the stall continues, and once more if the frame eventually completes.

### Levels and categories

`Trace`, `Debug`, `Info`, `Warning`, `Error`, `Fatal`. The minimum level defaults to `Debug`; `log trace` lowers it, `log warning` quietens everything below. Categories (`Renderer`, `Meshes`, `Editor`, `Console`, `CVar`, `Watchdog`, `Crash`, `Log`) are a short tag per line, never formatted into the message, so the Console's category filter stays exact.

## CVars

The registry is `Source/System/include/System/CVar.h`; what it is bound *to* is renderer-specific and stays there, in `EngineCVars.cpp`.

A cvar holds a pointer into the settings struct the renderer already reads every frame. Setting one from the Console is the same write as moving the slider in a panel, so there is no synchronisation step to get out of date and the two can never disagree. Registration happens once, against the long-lived scene renderer; nothing is ever unregistered.

Names are dotted and lower-case, matched case-insensitively.

```
rtgi.specular.enabled            show the value, its range and its startup default
rtgi.specular.enabled false      set it
list rtgi.specular               every cvar whose name or description contains that
help rtgi.radianceclamp          one cvar, described
reset rtgi.specular.enabled      back to the startup default
reset all                        all of them
```

Booleans accept `true`/`false`, `1`/`0`, `on`/`off`, `yes`/`no`. Enums accept either the label or the number. A value outside a cvar's range is **rejected, not clamped**, because silently clamping hides a typo.

### Type-ahead

As you type, matching cvars drop down beneath the field with their descriptions beside them, the way a game console does. Matching is by substring rather than prefix and ignores case, because a half-remembered name is the usual reason to want the list: typing `specular` finds `rtgi.specular.enabled` without recalling which group it belongs to. Arrow keys move through the list, Enter or Tab inserts the name, and only the name is ever inserted — the description column is a hint.

### Built-in commands

| Command | Meaning |
|---|---|
| `help` | list the commands |
| `help <cvar>` | describe one cvar |
| `list [text]`, `find <text>` | cvars whose name or description contains `text` |
| `reset <cvar>` / `reset all` | restore the startup default |
| `dumpcvars`, `cvarlist` | every cvar and its current value |
| `log <level>` | `trace`, `debug`, `info`, `warning`, `error`, `fatal` |
| `logpath` | where this session's `.txt` is being written |
| `clear` | empty the Console; the log file keeps everything |
| `echo <text>` | print text |

`clear` deliberately empties only the on-screen ring. The file is the record of what happened, and clearing a view must not edit history.

## CVar reference

323 cvars, grouped by prefix. Defaults are the values the engine starts with, which is also what `reset` restores — not whatever the current level file happens to contain. Ranges are inclusive.

### Renderer

| CVar | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `r.grid` | bool | `true` |  | Draws the viewport ground grid. |
| `r.viewdistance` | float | `10000` | `500.0 .. 100000.0` | Camera far plane in metres. |
| `r.wireframe` | bool | `false` |  | Draws scene geometry as wireframe. |

### Global illumination mode

| CVar | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `gi.mode` | enum | `rtgi` | `disabled \| rtgi \| radiancecascades` | Which global-illumination system runs. |

### Ray-traced global illumination

| CVar | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `rtgi.accumulationblend` | float | `1.0` | `0.0 .. 1.0` | Weight of the current frame in the non-NRD accumulator. |
| `rtgi.colorleak` | float | `1.0` | `0.0 .. 10.0` | How much surface colour bleeds into the bounce. |
| `rtgi.debugview` | enum | `off` | `off \| radiance \| luminance \| reservoir \| deterministic_probe \| flat_white \| world_normal \| world_position \| hit_distance \| ray_direction` | Replaces the image with one RTGI intermediate. |
| `rtgi.depththreshold` | float | `0.1` | `0.0 .. 1.0` | Relative depth difference above which a reused sample is rejected. |
| `rtgi.enabled` | bool | `true` |  | Ray-traced global illumination. |
| `rtgi.gi.intensity` | float | `1.0` | `0.0 .. 10.0` | Multiplier on the final indirect contribution. |
| `rtgi.maxbounces` | int | `2` | `1 .. 8` | Indirect bounces per path. |
| `rtgi.maxhistorylength` | int | `10` | `1 .. 200` | Frames a temporal reservoir may accumulate. |
| `rtgi.nee` | bool | `true` |  | Next-event estimation: sample lights directly at each hit. |
| `rtgi.normalthreshold` | float | `0.5` | `0.0 .. 1.0` | Normal dot-product below which a reused sample is rejected. |
| `rtgi.nrd` | bool | `true` |  | Denoise with NVIDIA RELAX instead of the built-in temporal filter. |
| `rtgi.nrd.atrousiterations` | int | `5` | `0 .. 8` | RELAX a-trous blur passes. |
| `rtgi.nrd.disocclusionthreshold` | float | `0.2` | `0.0 .. 1.0` | RELAX disocclusion sensitivity; higher keeps more history through motion. |
| `rtgi.nrd.maxaccumulationtime` | float | `0.35` | `0.0 .. 5.0` | RELAX history length, in seconds. |
| `rtgi.nrd.sharpen` | float | `0.35` | `0.0 .. 2.0` | RELAX output sharpening. |
| `rtgi.radianceclamp` | float | `10.0` | `0.0 .. 1000.0` | Ceiling on a single sample's radiance; lower kills fireflies and dims bright bounces. |
| `rtgi.raysperpixel` | int | `1` | `1 .. 32` | GI rays traced per pixel per frame. |
| `rtgi.spatialradius` | float | `20.0` | `1.0 .. 200.0` | Spatial reuse search radius in pixels. |
| `rtgi.spatialreuse` | bool | `false` |  | ReSTIR spatial reservoir reuse. Unused while the NRD denoiser is on. |
| `rtgi.spatialsamples` | int | `4` | `1 .. 32` | Neighbours consulted during spatial reuse. |
| `rtgi.specular.enabled` | bool | `true` |  | Ray-traced specular (glossy reflections) on top of diffuse GI. |
| `rtgi.specular.intensity` | float | `0.10` | `0.0 .. 10.0` | Multiplier on ray-traced specular. |
| `rtgi.specular.roughnessthreshold` | float | `0.6` | `0.0 .. 1.0` | Surfaces rougher than this get no ray-traced specular. |
| `rtgi.temporalreuse` | bool | `false` |  | ReSTIR temporal reservoir reuse. Unused while the NRD denoiser is on. |

### Radiance cascades

| CVar | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `rc.cascadecount` | int | `4` | `1 .. 8` | Number of cascades. |
| `rc.colorbleeding` | float | `2.0` | `0.0 .. 10.0` | How much surface colour tints the bounce. |
| `rc.debugview` | int | `0` | `0 .. 8` | Cascade debug visualisation. |
| `rc.enabled` | bool | `false` |  | Radiance-cascade global illumination. |
| `rc.gi.intensity` | float | `1.0` | `0.0 .. 10.0` | Multiplier on the cascade indirect contribution. |
| `rc.historyclampscale` | float | `0.4` | `0.0 .. 4.0` | Neighbourhood clamp width applied to probe history. |
| `rc.historydepthsensitivity` | float | `128.0` | `0.0 .. 1024.0` | Depth sensitivity of the history reprojection test. |
| `rc.historynormalthreshold` | float | `0.92` | `0.0 .. 1.0` | Normal dot-product below which probe history is dropped. |
| `rc.hysteresis` | float | `0.8` | `0.0 .. 1.0` | Temporal blend of probe radiance; higher is more stable and more laggy. |
| `rc.intervallengthscale` | float | `1.0` | `0.1 .. 8.0` | Scales every cascade's ray interval. |
| `rc.probespacingbase` | int | `16` | `2 .. 128` | Probe spacing of cascade 0, in pixels. |
| `rc.raybias` | float | `0.02` | `0.0 .. 1.0` | Ray origin offset along the normal, in metres. |
| `rc.raylengthbase` | float | `1.5` | `0.01 .. 100.0` | Ray length of cascade 0, in metres. |
| `rc.raylengthscale` | float | `2.0` | `1.0 .. 8.0` | Ray-length multiplier between cascades. |
| `rc.raysperprobe` | int | `8` | `1 .. 256` | Rays cast per probe per cascade. |
| `rc.sparse.cellsize` | int | `16` | `1 .. 256` | Sparse probe cell size. |
| `rc.sparse.reusestrength` | float | `0.35` | `0.0 .. 1.0` | How strongly a nearby cached probe is reused. |
| `rc.sparse.searchsteps` | int | `32` | `1 .. 256` | Probe-lookup probe count before giving up. |
| `rc.sparse.tablecapacity` | int | `65536` | `256 .. 1048576` | Sparse probe hash-table size. |
| `rc.spatialfilter` | float | `1.0` | `0.0 .. 4.0` | Strength of the cascade spatial filter. |

### Irradiance probe grid

| CVar | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `probes.debug.lightingmode` | int | `0` | `0 .. 4` | What the debug spheres display. |
| `probes.debug.show` | bool | `false` |  | Draws a sphere at every probe. |
| `probes.debug.sphereradius` | float | `0.18` | `0.01 .. 5.0` | Radius of the debug spheres, in metres. |
| `probes.enabled` | bool | `false` |  | Irradiance probe grid. |
| `probes.followcamera` | bool | `true` |  | Recentres the grid on the camera. |
| `probes.gridx` | int | `16` | `1 .. 128` | Probes along X. |
| `probes.gridy` | int | `16` | `1 .. 128` | Probes along Y. |
| `probes.gridz` | int | `16` | `1 .. 128` | Probes along Z. |
| `probes.originx` | float | `0.0` | `-10000.0 .. 10000.0` | Grid origin X. |
| `probes.originy` | float | `0.0` | `-10000.0 .. 10000.0` | Grid origin Y. |
| `probes.originz` | float | `2.0` | `-10000.0 .. 10000.0` | Grid origin Z. |
| `probes.raysperprobe` | int | `64` | `1 .. 1024` | Rays cast per probe per update. |
| `probes.spacing` | float | `2.0` | `0.1 .. 100.0` | Distance between probes, in metres. |
| `probes.updateblend` | float | `0.15` | `0.0 .. 1.0` | Weight of a fresh probe update against its history. |

### Ray-traced ambient occlusion

| CVar | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `rtao.debugview` | int | `0` | `0 .. 4` | RTAO debug visualisation. |
| `rtao.enabled` | bool | `false` |  | Ray-traced ambient occlusion. |
| `rtao.intensity` | float | `1.0` | `0.0 .. 4.0` | Multiplier on the occlusion term. |
| `rtao.maxraylength` | float | `1.0` | `0.01 .. 100.0` | Longest occlusion ray, in metres. |
| `rtao.power` | float | `1.5` | `0.1 .. 8.0` | Exponent applied to the occlusion term. |
| `rtao.raybias` | float | `0.005` | `0.0 .. 1.0` | Ray origin offset along the normal. |
| `rtao.raysperpixel` | int | `16` | `1 .. 128` | Occlusion rays per pixel. |

### Ground-truth ambient occlusion

| CVar | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `gtao.debugview` | int | `0` | `0 .. 4` | GTAO debug visualisation. |
| `gtao.denoisepasses` | int | `0` | `0 .. 4` | Denoise passes over the AO term. |
| `gtao.depthmipsamplingoffset` | float | `3.30` | `0.0 .. 8.0` | Depth mip bias used while sampling. |
| `gtao.enabled` | bool | `true` |  | Ground-truth ambient occlusion (screen space). |
| `gtao.falloffrange` | float | `0.615` | `0.0 .. 1.0` | Fraction of the radius over which occlusion fades out. |
| `gtao.finalvaluepower` | float | `2.55` | `0.1 .. 8.0` | Exponent applied to the AO term. |
| `gtao.intensity` | float | `1.0` | `0.0 .. 4.0` | Multiplier on the occlusion term. |
| `gtao.quality` | int | `3` | `0 .. 3` | 0 low .. 3 ultra. |
| `gtao.radius` | float | `1.0` | `0.01 .. 20.0` | World-space sampling radius, in metres. |
| `gtao.radiusmultiplier` | float | `1.457` | `0.1 .. 8.0` | Scales the effective radius. |
| `gtao.sampledistributionpower` | float | `2.0` | `0.1 .. 8.0` | Biases samples towards the centre as it rises. |
| `gtao.thinoccludercompensation` | float | `0.0` | `0.0 .. 1.0` | Reduces over-darkening behind thin geometry. |

### Screen-space reflections

| CVar | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `ssr.debugview` | int | `0` | `0 .. 4` | SSR debug visualisation. |
| `ssr.edgefadestart` | float | `0.85` | `0.0 .. 1.0` | Screen fraction at which reflections start fading at the border. |
| `ssr.enabled` | bool | `false` |  | Screen-space reflections. |
| `ssr.intensity` | float | `1.0` | `0.0 .. 4.0` | Multiplier on the reflection. |
| `ssr.maxdistance` | float | `60.0` | `1.0 .. 1000.0` | Longest reflection ray, in metres. |
| `ssr.maxroughness` | float | `0.6` | `0.0 .. 1.0` | Surfaces rougher than this get no screen-space reflection. |
| `ssr.maxsteps` | int | `48` | `1 .. 512` | March steps before a ray is abandoned. |
| `ssr.refinesteps` | int | `6` | `0 .. 32` | Binary-search steps used to refine a hit. |
| `ssr.stepgrowth` | float | `1.05` | `1.0 .. 2.0` | Multiplier applied to each successive step. |
| `ssr.stepsize` | float | `0.25` | `0.001 .. 10.0` | Length of the first march step, in metres. |
| `ssr.thickness` | float | `0.5` | `0.001 .. 10.0` | Assumed depth-buffer thickness when testing a hit. |

### Temporal anti-aliasing

| CVar | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `taa.blendfactor` | float | `0.5` | `0.0 .. 1.0` | Weight of the current frame; lower accumulates longer. |
| `taa.enabled` | bool | `true` |  | Temporal anti-aliasing. |
| `taa.jitterscale` | float | `1.0` | `0.0 .. 4.0` | Sub-pixel jitter magnitude in pixels; 0 disables jitter. |

### SMAA

| CVar | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `smaa.cornerrounding` | float | `25.0` | `0.0 .. 100.0` | How much sharp corners are rounded, as a percentage. |
| `smaa.debugview` | enum | `off` | `off \| edges \| blendweights` | SMAA debug visualisation. |
| `smaa.edgethreshold` | float | `0.05` | `0.0 .. 1.0` | Luma difference that counts as an edge; lower finds more edges. |
| `smaa.enabled` | bool | `false` |  | SMAA spatial anti-aliasing. |
| `smaa.maxsearchsteps` | float | `16.0` | `0.0 .. 112.0` | Horizontal/vertical edge search length. |
| `smaa.maxsearchstepsdiag` | float | `8.0` | `0.0 .. 20.0` | Diagonal edge search length. |

### MSAA

| CVar | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `msaa.enabled` | bool | `false` |  | Multi-sample the G-Buffer. Costs SampleCount times the G-Buffer bandwidth. |
| `msaa.quality` | int | `0` | `0 .. 32` | Hardware quality level; 0 is the default for the sample count. |
| `msaa.samplecount` | int | `4` | `2 .. 8` | Samples per pixel: 2, 4 or 8. |

### DLSS

| CVar | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `dlss.enabled` | bool | `false` |  | NVIDIA DLSS upscaling. |
| `dlss.mode` | enum | `maxquality` | `off \| maxperformance \| balanced \| maxquality \| ultraperformance \| ultraquality \| dlaa` | DLSS quality preset. |

### Sharpening

| CVar | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `sharpen.image.enabled` | bool | `true` |  | Post-process image sharpening. |
| `sharpen.image.strength` | float | `0.2` | `0.0 .. 1.5` | Image sharpening strength. |
| `sharpen.texture.enabled` | bool | `true` |  | Applies a negative mip bias when sampling textures. |
| `sharpen.texture.miplodbias` | float | `-1.5` | `-3.0 .. 0.0` | Mip LOD bias; more negative is sharper and noisier. |

### Bloom

| CVar | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `bloom.enabled` | bool | `true` |  | Bloom. |
| `bloom.intensity` | float | `0.03` | `0.0 .. 2.0` | How much bloom is added back to the image. |
| `bloom.knee` | float | `0.35` | `0.0 .. 1.0` | Softness of the threshold. |
| `bloom.miplevels` | int | `6` | `1 .. 12` | Mip levels in the bloom chain. |
| `bloom.radius` | float | `1.0` | `0.0 .. 8.0` | Upsample filter radius. |
| `bloom.threshold` | float | `0.9` | `0.0 .. 20.0` | Luminance above which a pixel contributes to bloom. |

### Chromatic aberration

| CVar | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `chroma.centerx` | float | `0.5` | `0.0 .. 1.0` | Distortion centre, X, in normalised screen space. |
| `chroma.centery` | float | `0.5` | `0.0 .. 1.0` | Distortion centre, Y, in normalised screen space. |
| `chroma.enabled` | bool | `false` |  | Chromatic aberration. |
| `chroma.falloff` | float | `2.0` | `0.0 .. 8.0` | How sharply the effect grows towards the edge. |
| `chroma.samplecount` | int | `6` | `1 .. 32` | Samples taken along the separation. |
| `chroma.strength` | float | `2.0` | `0.0 .. 20.0` | Channel separation in pixels at the frame edge. |

### AgX tonemapping and colour grading

| CVar | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `agx.enabled` | bool | `true` |  | AgX tonemapping. |
| `agx.ev100max` | float | `16.0` | `-16.0 .. 24.0` | Top of the auto-exposure range, in EV100. |
| `agx.ev100min` | float | `-1.0` | `-16.0 .. 16.0` | Bottom of the auto-exposure range, in EV100. |
| `agx.exposure` | float | `0.0` | `-10.0 .. 10.0` | Exposure compensation in stops. |
| `agx.global.contrast.blue` | float | `0` | `-2 .. 2` | Contrast, whole image (blue). |
| `agx.global.contrast.green` | float | `0` | `-2 .. 2` | Contrast, whole image (green). |
| `agx.global.contrast.red` | float | `0` | `-2 .. 2` | Contrast, whole image (red). |
| `agx.global.contrast.total` | float | `0.10` | `-2 .. 2` | Contrast, whole image (total). |
| `agx.global.contrast.yellow` | float | `0` | `-2 .. 2` | Contrast, whole image (yellow). |
| `agx.global.gain.blue` | float | `0` | `-2 .. 2` | Gain, whole image (blue). |
| `agx.global.gain.green` | float | `0` | `-2 .. 2` | Gain, whole image (green). |
| `agx.global.gain.red` | float | `0` | `-2 .. 2` | Gain, whole image (red). |
| `agx.global.gain.total` | float | `0` | `-2 .. 2` | Gain, whole image (total). |
| `agx.global.gain.yellow` | float | `0` | `-2 .. 2` | Gain, whole image (yellow). |
| `agx.global.gamma.blue` | float | `0` | `-2 .. 2` | Gamma, whole image (blue). |
| `agx.global.gamma.green` | float | `0` | `-2 .. 2` | Gamma, whole image (green). |
| `agx.global.gamma.red` | float | `0` | `-2 .. 2` | Gamma, whole image (red). |
| `agx.global.gamma.total` | float | `0` | `-2 .. 2` | Gamma, whole image (total). |
| `agx.global.gamma.yellow` | float | `0` | `-2 .. 2` | Gamma, whole image (yellow). |
| `agx.global.saturation.blue` | float | `0` | `-2 .. 2` | Saturation, whole image (blue). |
| `agx.global.saturation.green` | float | `0` | `-2 .. 2` | Saturation, whole image (green). |
| `agx.global.saturation.red` | float | `0` | `-2 .. 2` | Saturation, whole image (red). |
| `agx.global.saturation.total` | float | `0.12` | `-2 .. 2` | Saturation, whole image (total). |
| `agx.global.saturation.yellow` | float | `0` | `-2 .. 2` | Saturation, whole image (yellow). |
| `agx.global.vibrance.blue` | float | `0` | `-2 .. 2` | Vibrance, whole image (blue). |
| `agx.global.vibrance.green` | float | `0` | `-2 .. 2` | Vibrance, whole image (green). |
| `agx.global.vibrance.red` | float | `0` | `-2 .. 2` | Vibrance, whole image (red). |
| `agx.global.vibrance.total` | float | `0` | `-2 .. 2` | Vibrance, whole image (total). |
| `agx.global.vibrance.yellow` | float | `0` | `-2 .. 2` | Vibrance, whole image (yellow). |
| `agx.highlights.contrast.blue` | float | `0` | `-2 .. 2` | Contrast, highlights only (blue). |
| `agx.highlights.contrast.green` | float | `0` | `-2 .. 2` | Contrast, highlights only (green). |
| `agx.highlights.contrast.red` | float | `0` | `-2 .. 2` | Contrast, highlights only (red). |
| `agx.highlights.contrast.total` | float | `0` | `-2 .. 2` | Contrast, highlights only (total). |
| `agx.highlights.contrast.yellow` | float | `0` | `-2 .. 2` | Contrast, highlights only (yellow). |
| `agx.highlights.gain.blue` | float | `0` | `-2 .. 2` | Gain, highlights only (blue). |
| `agx.highlights.gain.green` | float | `0` | `-2 .. 2` | Gain, highlights only (green). |
| `agx.highlights.gain.red` | float | `0` | `-2 .. 2` | Gain, highlights only (red). |
| `agx.highlights.gain.total` | float | `0` | `-2 .. 2` | Gain, highlights only (total). |
| `agx.highlights.gain.yellow` | float | `0` | `-2 .. 2` | Gain, highlights only (yellow). |
| `agx.highlights.gamma.blue` | float | `0` | `-2 .. 2` | Gamma, highlights only (blue). |
| `agx.highlights.gamma.green` | float | `0` | `-2 .. 2` | Gamma, highlights only (green). |
| `agx.highlights.gamma.red` | float | `0` | `-2 .. 2` | Gamma, highlights only (red). |
| `agx.highlights.gamma.total` | float | `0` | `-2 .. 2` | Gamma, highlights only (total). |
| `agx.highlights.gamma.yellow` | float | `0` | `-2 .. 2` | Gamma, highlights only (yellow). |
| `agx.highlights.saturation.blue` | float | `0` | `-2 .. 2` | Saturation, highlights only (blue). |
| `agx.highlights.saturation.green` | float | `0` | `-2 .. 2` | Saturation, highlights only (green). |
| `agx.highlights.saturation.red` | float | `0` | `-2 .. 2` | Saturation, highlights only (red). |
| `agx.highlights.saturation.total` | float | `0` | `-2 .. 2` | Saturation, highlights only (total). |
| `agx.highlights.saturation.yellow` | float | `0` | `-2 .. 2` | Saturation, highlights only (yellow). |
| `agx.highlights.vibrance.blue` | float | `0` | `-2 .. 2` | Vibrance, highlights only (blue). |
| `agx.highlights.vibrance.green` | float | `0` | `-2 .. 2` | Vibrance, highlights only (green). |
| `agx.highlights.vibrance.red` | float | `0` | `-2 .. 2` | Vibrance, highlights only (red). |
| `agx.highlights.vibrance.total` | float | `0` | `-2 .. 2` | Vibrance, highlights only (total). |
| `agx.highlights.vibrance.yellow` | float | `0` | `-2 .. 2` | Vibrance, highlights only (yellow). |
| `agx.midtones.contrast.blue` | float | `0` | `-2 .. 2` | Contrast, midtones only (blue). |
| `agx.midtones.contrast.green` | float | `0` | `-2 .. 2` | Contrast, midtones only (green). |
| `agx.midtones.contrast.red` | float | `0` | `-2 .. 2` | Contrast, midtones only (red). |
| `agx.midtones.contrast.total` | float | `0` | `-2 .. 2` | Contrast, midtones only (total). |
| `agx.midtones.contrast.yellow` | float | `0` | `-2 .. 2` | Contrast, midtones only (yellow). |
| `agx.midtones.gain.blue` | float | `0` | `-2 .. 2` | Gain, midtones only (blue). |
| `agx.midtones.gain.green` | float | `0` | `-2 .. 2` | Gain, midtones only (green). |
| `agx.midtones.gain.red` | float | `0` | `-2 .. 2` | Gain, midtones only (red). |
| `agx.midtones.gain.total` | float | `0` | `-2 .. 2` | Gain, midtones only (total). |
| `agx.midtones.gain.yellow` | float | `0` | `-2 .. 2` | Gain, midtones only (yellow). |
| `agx.midtones.gamma.blue` | float | `0` | `-2 .. 2` | Gamma, midtones only (blue). |
| `agx.midtones.gamma.green` | float | `0` | `-2 .. 2` | Gamma, midtones only (green). |
| `agx.midtones.gamma.red` | float | `0` | `-2 .. 2` | Gamma, midtones only (red). |
| `agx.midtones.gamma.total` | float | `0` | `-2 .. 2` | Gamma, midtones only (total). |
| `agx.midtones.gamma.yellow` | float | `0` | `-2 .. 2` | Gamma, midtones only (yellow). |
| `agx.midtones.saturation.blue` | float | `0` | `-2 .. 2` | Saturation, midtones only (blue). |
| `agx.midtones.saturation.green` | float | `0` | `-2 .. 2` | Saturation, midtones only (green). |
| `agx.midtones.saturation.red` | float | `0` | `-2 .. 2` | Saturation, midtones only (red). |
| `agx.midtones.saturation.total` | float | `0` | `-2 .. 2` | Saturation, midtones only (total). |
| `agx.midtones.saturation.yellow` | float | `0` | `-2 .. 2` | Saturation, midtones only (yellow). |
| `agx.midtones.vibrance.blue` | float | `0` | `-2 .. 2` | Vibrance, midtones only (blue). |
| `agx.midtones.vibrance.green` | float | `0` | `-2 .. 2` | Vibrance, midtones only (green). |
| `agx.midtones.vibrance.red` | float | `0` | `-2 .. 2` | Vibrance, midtones only (red). |
| `agx.midtones.vibrance.total` | float | `0` | `-2 .. 2` | Vibrance, midtones only (total). |
| `agx.midtones.vibrance.yellow` | float | `0` | `-2 .. 2` | Vibrance, midtones only (yellow). |
| `agx.shadows.contrast.blue` | float | `0` | `-2 .. 2` | Contrast, shadows only (blue). |
| `agx.shadows.contrast.green` | float | `0` | `-2 .. 2` | Contrast, shadows only (green). |
| `agx.shadows.contrast.red` | float | `0` | `-2 .. 2` | Contrast, shadows only (red). |
| `agx.shadows.contrast.total` | float | `0` | `-2 .. 2` | Contrast, shadows only (total). |
| `agx.shadows.contrast.yellow` | float | `0` | `-2 .. 2` | Contrast, shadows only (yellow). |
| `agx.shadows.gain.blue` | float | `0` | `-2 .. 2` | Gain, shadows only (blue). |
| `agx.shadows.gain.green` | float | `0` | `-2 .. 2` | Gain, shadows only (green). |
| `agx.shadows.gain.red` | float | `0` | `-2 .. 2` | Gain, shadows only (red). |
| `agx.shadows.gain.total` | float | `0` | `-2 .. 2` | Gain, shadows only (total). |
| `agx.shadows.gain.yellow` | float | `0` | `-2 .. 2` | Gain, shadows only (yellow). |
| `agx.shadows.gamma.blue` | float | `0` | `-2 .. 2` | Gamma, shadows only (blue). |
| `agx.shadows.gamma.green` | float | `0` | `-2 .. 2` | Gamma, shadows only (green). |
| `agx.shadows.gamma.red` | float | `0` | `-2 .. 2` | Gamma, shadows only (red). |
| `agx.shadows.gamma.total` | float | `0` | `-2 .. 2` | Gamma, shadows only (total). |
| `agx.shadows.gamma.yellow` | float | `0` | `-2 .. 2` | Gamma, shadows only (yellow). |
| `agx.shadows.saturation.blue` | float | `0` | `-2 .. 2` | Saturation, shadows only (blue). |
| `agx.shadows.saturation.green` | float | `0` | `-2 .. 2` | Saturation, shadows only (green). |
| `agx.shadows.saturation.red` | float | `0` | `-2 .. 2` | Saturation, shadows only (red). |
| `agx.shadows.saturation.total` | float | `0` | `-2 .. 2` | Saturation, shadows only (total). |
| `agx.shadows.saturation.yellow` | float | `0` | `-2 .. 2` | Saturation, shadows only (yellow). |
| `agx.shadows.vibrance.blue` | float | `0` | `-2 .. 2` | Vibrance, shadows only (blue). |
| `agx.shadows.vibrance.green` | float | `0` | `-2 .. 2` | Vibrance, shadows only (green). |
| `agx.shadows.vibrance.red` | float | `0` | `-2 .. 2` | Vibrance, shadows only (red). |
| `agx.shadows.vibrance.total` | float | `0` | `-2 .. 2` | Vibrance, shadows only (total). |
| `agx.shadows.vibrance.yellow` | float | `0` | `-2 .. 2` | Vibrance, shadows only (yellow). |
| `agx.shoulderstrength` | float | `1.05` | `0.0 .. 4.0` | Highlight roll-off of the AgX curve. |
| `agx.toestrength` | float | `0.95` | `0.0 .. 4.0` | Shadow roll-off of the AgX curve. |

### Time of day

| CVar | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `tod.enabled` | bool | `true` |  | Sun, sky ambient, procedural sky and clouds as one system. |
| `tod.groundalbedo` | float | `0.1` | `0.0 .. 1.0` | Ground albedo; tints the sky near the horizon. |
| `tod.overrideskycolor` | bool | `false` |  | Use the manual sky colour instead of the Hosek-Wilkie one. |
| `tod.overridesuncolor` | bool | `false` |  | Use the manual sun colour instead of the Hosek-Wilkie one. |
| `tod.skycolor.b` | float | `1.00` | `0.0 .. 10.0` | Manual sky colour, blue (linear). |
| `tod.skycolor.g` | float | `0.45` | `0.0 .. 10.0` | Manual sky colour, green (linear). |
| `tod.skycolor.r` | float | `0.25` | `0.0 .. 10.0` | Manual sky colour, red (linear). |
| `tod.skyintensitylux` | float | `20000.0` | `0.0 .. 200000.0` | Sky ambient intensity in lux. |
| `tod.suncolor.b` | float | `0.80` | `0.0 .. 10.0` | Manual sun colour, blue (linear). |
| `tod.suncolor.g` | float | `0.95` | `0.0 .. 10.0` | Manual sun colour, green (linear). |
| `tod.suncolor.r` | float | `1.00` | `0.0 .. 10.0` | Manual sun colour, red (linear). |
| `tod.sunintensitylux` | float | `120000.0` | `0.0 .. 400000.0` | Sun intensity in lux; real noon sun is about 100000. |
| `tod.time` | float | `12.0` | `0.0 .. 24.0` | Time of day in hours; noon is 12. |
| `tod.turbidity` | float | `2.5` | `1.0 .. 10.0` | Atmospheric turbidity: 2 very clear, 10 heavy haze. |

### Wind

| CVar | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `wind.direction` | float | `0.6` | `-6.2832 .. 6.2832` | Heading in radians, counter-clockwise from world +X. |
| `wind.enabled` | bool | `true` |  | Scene-wide wind, shared by rain and vegetation. |
| `wind.flutterfrequency` | float | `2.2` | `0.0 .. 20.0` | Leaf flutter rate in Hz. |
| `wind.gustamplitude` | float | `2.5` | `0.0 .. 60.0` | Peak extra speed contributed by gusts, m/s. |
| `wind.gustfrequency` | float | `0.25` | `0.0 .. 10.0` | Gust pulse rate in Hz. |
| `wind.gustwavelength` | float | `40.0` | `1.0 .. 1000.0` | Size of one gust cell in metres; keep it larger than the visible area. |
| `wind.strength` | float | `4.0` | `0.0 .. 60.0` | Sustained wind speed, m/s. 2 light, 8 branches moving, 15 trees swaying. |
| `wind.vegetationbendscale` | float | `1.0` | `0.0 .. 4.0` | Global multiplier on vegetation bending. |

### Volumetric fog

| CVar | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `fog.anisotropy` | float | `0.5` | `-0.99 .. 0.99` | Henyey-Greenstein g: positive scatters forward. |
| `fog.baseheight` | float | `0.0` | `-1000.0 .. 10000.0` | Height at which the density falloff starts, in metres. |
| `fog.color.b` | float | `1.0` | `0.0 .. 10.0` | Fog scattering colour, blue. |
| `fog.color.g` | float | `1.0` | `0.0 .. 10.0` | Fog scattering colour, green. |
| `fog.color.r` | float | `1.0` | `0.0 .. 10.0` | Fog scattering colour, red. |
| `fog.debugview` | int | `0` | `0 .. 4` | Volumetric fog debug visualisation. |
| `fog.density` | float | `0.02` | `0.0 .. 2.0` | Extinction per metre. |
| `fog.depthslices` | int | `64` | `8 .. 256` | Froxel slices along the view ray. |
| `fog.emissive.b` | float | `0.0` | `0.0 .. 10.0` | Fog self-emission, blue. |
| `fog.emissive.g` | float | `0.0` | `0.0 .. 10.0` | Fog self-emission, green. |
| `fog.emissive.intensity` | float | `0.0` | `0.0 .. 100.0` | Multiplier on fog self-emission. |
| `fog.emissive.r` | float | `0.0` | `0.0 .. 10.0` | Fog self-emission, red. |
| `fog.enabled` | bool | `true` |  | Froxel volumetric fog. |
| `fog.froxeltilesize` | int | `8` | `2 .. 32` | Froxel width in pixels; smaller is sharper and costlier. |
| `fog.heightfalloff` | float | `0.0` | `0.0 .. 2.0` | Exponential density falloff with height; 0 is uniform. |
| `fog.maxdistance` | float | `100.0` | `1.0 .. 10000.0` | Furthest fogged distance, in metres. |
| `fog.startdistance` | float | `0.1` | `0.0 .. 1000.0` | Nearest fogged distance, in metres. |

### Volumetric clouds

| CVar | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `clouds.ambientintensityscale` | float | `1.0` | `0.0 .. 10.0` | Multiplier on sky ambient reaching the clouds. |
| `clouds.anvilbias` | float | `0.25` | `0.0 .. 1.0` | Spreads cloud tops outward into anvils. |
| `clouds.basenoisescale` | float | `24000.0` | `100.0 .. 200000.0` | Base shape noise wavelength, in metres. |
| `clouds.cloudtopoffset` | float | `350.0` | `0.0 .. 5000.0` | Wind shear applied towards the cloud top, in metres. |
| `clouds.cloudtype` | float | `0.50` | `0.0 .. 1.0` | 0 stratus, 1 cumulonimbus. |
| `clouds.coverage` | float | `0.55` | `0.0 .. 1.0` | Fraction of sky covered. |
| `clouds.curlstrength` | float | `0.60` | `0.0 .. 4.0` | Curl-noise warp applied to the detail. |
| `clouds.debugview` | int | `0` | `0 .. 8` | Cloud debug visualisation. |
| `clouds.density` | float | `0.60` | `0.0 .. 4.0` | Cloud density multiplier. |
| `clouds.detailfadedistance` | float | `25000.0` | `100.0 .. 200000.0` | Where detail noise stops being evaluated, in metres. |
| `clouds.detailnoisescale` | float | `1600.0` | `10.0 .. 50000.0` | Detail erosion noise wavelength, in metres. |
| `clouds.detailstrength` | float | `0.35` | `0.0 .. 2.0` | How hard the detail noise erodes the base shape. |
| `clouds.detailwindspeedscale` | float | `2.0` | `0.0 .. 10.0` | Detail noise wind speed, relative to the base. |
| `clouds.distancefadestart` | float | `80000.0` | `1000.0 .. 500000.0` | Where clouds start fading out, in metres. |
| `clouds.enabled` | bool | `true` |  | Ray-marched volumetric clouds. |
| `clouds.extinctionscale` | float | `0.05` | `0.0 .. 2.0` | Extinction per unit density. |
| `clouds.groundbouncescale` | float | `0.30` | `0.0 .. 10.0` | Light bounced from the ground into the cloud base. |
| `clouds.layerbottom` | float | `1200.0` | `0.0 .. 20000.0` | Cloud base altitude, in metres. |
| `clouds.layerthickness` | float | `4000.0` | `1.0 .. 20000.0` | Cloud layer thickness, in metres. |
| `clouds.lightmarchdistance` | float | `1600.0` | `10.0 .. 20000.0` | Length of the shadow ray, in metres. |
| `clouds.lightsteps` | int | `6` | `1 .. 32` | Steps along the shadow ray towards the sun. |
| `clouds.maxsteps` | int | `96` | `8 .. 512` | Primary ray-march steps. |
| `clouds.maxtracedistance` | float | `120000.0` | `1000.0 .. 500000.0` | Longest primary ray, in metres. |
| `clouds.ms.extinctionfalloff` | float | `0.55` | `0.0 .. 1.0` | Extinction falloff per octave. |
| `clouds.ms.phasefalloff` | float | `0.50` | `0.0 .. 1.0` | Phase falloff per octave. |
| `clouds.ms.scatterfalloff` | float | `0.55` | `0.0 .. 1.0` | Scatter falloff per multiple-scattering octave. |
| `clouds.multiscatteroctaves` | int | `3` | `1 .. 8` | Multiple-scattering octaves. |
| `clouds.phaseblend` | float | `0.28` | `0.0 .. 1.0` | Blend between the two phase lobes. |
| `clouds.phaseg0` | float | `0.80` | `-0.99 .. 0.99` | Forward-scattering lobe. |
| `clouds.phaseg1` | float | `-0.25` | `-0.99 .. 0.99` | Back-scattering lobe. |
| `clouds.planetradiuskm` | float | `6360.0` | `100.0 .. 20000.0` | Planet radius in kilometres; sets the curvature of the layer. |
| `clouds.powderstrength` | float | `0.35` | `0.0 .. 2.0` | Powder (dark-edge) approximation strength. |
| `clouds.resolutiondivisor` | int | `2` | `1 .. 8` | Renders clouds at 1/N resolution. |
| `clouds.scattering.b` | float | `1.0` | `0.0 .. 1.0` | Cloud scattering albedo, blue. |
| `clouds.scattering.g` | float | `1.0` | `0.0 .. 1.0` | Cloud scattering albedo, green. |
| `clouds.scattering.r` | float | `1.0` | `0.0 .. 1.0` | Cloud scattering albedo, red. |
| `clouds.shadowconespread` | float | `0.12` | `0.0 .. 1.0` | Cone spread of the shadow ray. |
| `clouds.shadowstepgrowth` | float | `1.35` | `1.0 .. 4.0` | Step growth along the shadow ray. |
| `clouds.sunintensityscale` | float | `1.0` | `0.0 .. 10.0` | Multiplier on sunlight reaching the clouds. |
| `clouds.temporalblend` | float | `0.92` | `0.0 .. 1.0` | Weight of cloud history; higher is smoother and laggier. |
| `clouds.temporalupsampling` | bool | `true` |  | Reprojects previous frames to fill the low-resolution trace. |
| `clouds.weathercellsize` | float | `1.0` | `0.01 .. 10.0` | Weather-map cell size multiplier. |
| `clouds.weathercoveragebias` | float | `0.0` | `-1.0 .. 1.0` | Added to the weather map's coverage. |
| `clouds.weatherscale` | float | `90000.0` | `1000.0 .. 500000.0` | Weather-map wavelength, in metres. |
| `clouds.weatherseed` | int | `1337` | `0 .. 1000000` | Seed for the procedural weather map. |
| `clouds.weathertypebias` | float | `0.0` | `-1.0 .. 1.0` | Added to the weather map's cloud type. |
| `clouds.winddirection` | float | `45.0` | `-360.0 .. 360.0` | Cloud wind heading, in degrees. |
| `clouds.windskew` | float | `0.35` | `0.0 .. 2.0` | How much the wind leans the column with altitude. |
| `clouds.windspeed` | float | `12.0` | `0.0 .. 200.0` | Cloud wind speed, m/s. |

### Point shadows

| CVar | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `shadow.point.bias` | float | `0.002` | `0.0 .. 0.1` | Constant depth bias. |
| `shadow.point.debugview` | int | `0` | `0 .. 4` | Point-shadow debug visualisation. |
| `shadow.point.filterradius` | int | `2` | `0 .. 8` | PCF radius in texels. |
| `shadow.point.mapsize` | int | `1024` | `128 .. 4096` | Cube face resolution. Changing this rebuilds the shadow atlas. |
| `shadow.point.normaloffset` | float | `1.0` | `0.0 .. 16.0` | Offsets the lookup along the normal, in texels. |
| `shadow.point.seamblenddistance` | float | `0.05` | `0.0 .. 1.0` | Blend width across cube-face seams. |
| `shadow.point.slopescaleddepthbias` | float | `2.0` | `0.0 .. 16.0` | Depth bias proportional to the surface slope. |

