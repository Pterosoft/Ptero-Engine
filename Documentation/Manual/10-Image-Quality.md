# 10. Image quality and post-processing

Everything here is in **Windows → Graphics Settings...** and saved with the level (MSAA and the
path tracer excepted). Each section has a **Revert All** button.

## Anti-aliasing

Aliasing is the jagged, crawling look of edges and fine detail. Ptero offers several tools; some
combine, some replace each other.

| Technique | What it fixes | Cost | Combines with |
| --- | --- | --- | --- |
| **TAA** (on by default) | Everything: edges, shading, texture shimmer, and it cleans up noisy effects (GI, soft shadows, SSR) over time | Low | Analytic Edge AA, SMAA |
| **SMAA** | Edges, by finding and blending them in the final image | Low | TAA |
| **Analytic Edge AA** | Geometric edges only, exactly: the G-Buffer stores each pixel's triangle edges and the pass computes how much of the pixel each surface really covers | Low | TAA, SMAA |
| **MSAA** 2×/4×/8× | Geometric edges, by rendering several samples per pixel | High memory and bandwidth | — (Analytic Edge AA and VG occlusion culling switch off) |
| **DLSS / FSR / XeSS / TAAU / AAR** | Everything, while upscaling (below) | Saves time | Replace TAA |

**TAA** is a port of Unreal Engine 5's temporal AA, and its controls use Unreal's names: Quality,
Current Frame Weight (0.04; lower accumulates longer, smoother but more ghosting), Samples (8),
Filter Size, Catmull-Rom filter, Jitter Scale. Keep TAA (or an upscaler) on: the GI, soft
shadows and reflections all rely on temporal accumulation to look clean.

## Upscaling

Upscalers render the scene at a lower resolution and reconstruct a full-resolution image,
which buys back a lot of performance. **One upscaler runs at a time**, in this order of
precedence:

| Upscaler | Notes |
| --- | --- |
| **DLSS Super Resolution** | NVIDIA RTX. Modes: Ultra Performance, Max Performance, Balanced, Max Quality, Ultra Quality, **DLAA** (native resolution, AA only). |
| **AMD FSR** | Any GPU. Native AA, Quality, Balanced, Performance, Ultra Performance. Includes its own sharpening (Sharpness 0.5). |
| **Intel XeSS** | Any GPU, best on Intel. Native AA, Ultra Quality Plus, Ultra Quality, Quality, Balanced, Performance, Ultra Performance. |
| **TAAU** | TAA with temporal upsampling at a *Screen Percentage* (67%). Built in, any GPU. |
| **AAR** (Analytic-Aware Reconstruction) | A spatial upscaler using the triangle edges: geometric edges are exact, everything else is a careful filter that never blurs across surfaces. No history, so no ghosting at all; but texture detail is only as fine as the render resolution. Native, Ultra Quality (80%), Quality (75%), Balanced (67%). |

The window shows what is actually running, the render and output resolutions, and any error.

## Frame generation

Frame generation presents extra, interpolated frames between rendered ones for smoother motion.
It adds a little latency.

| Option | Notes |
| --- | --- |
| **FSR Frame Generation** | One generated frame per rendered frame. Works with or without FSR upscaling. Takes precedence over XeSS frame generation. |
| **XeSS Frame Generation** | Generated frames per rendered frame (more than 1 only on Intel GPUs). Turns on **Xe Low Latency**, which paces the CPU so frames don't queue up. *UI Composition* draws the game UI over generated frames instead of interpolating it. |

Videos and the game UI are drawn after frame generation captures the scene, so they are never
smeared.

## Sharpening

| Setting | Meaning |
| --- | --- |
| Image Sharpening (on), Strength (0.2), Radius (2 px) | An unsharp mask on the final image. Larger radius sharpens broader detail. |
| Texture Sharpening (on), Mip LOD Bias (-1.5) | Samples textures at a sharper mip level. Recovers the texture detail temporal AA and upscalers soften. |

## Tonemapping and exposure

The scene is rendered in HDR scene-linear light. The **AgX tonemapper** turns it into the image
you see, the way film or a camera sensor does: highlights roll off smoothly instead of clipping,
and very bright colours desaturate towards white the way they do in photographs.

### Exposure

| Setting | Meaning |
| --- | --- |
| Exposure Mode | **Manual**: use EV100 as set. **Auto (Histogram)**: measure the scene and adapt. |
| EV100 (-4) | Photographic exposure. Higher = darker, one stop per unit. |
| Exposure (0) | A trim in stops on top of everything. Positive = brighter. |
| EV100 Min / Max | The range auto exposure may settle in. Collapse it to one value to pin the exposure. |
| Auto: Low / High Percent (10% / 90%) | Which part of the brightness histogram is averaged. Ignoring the extremes stops one lamp or one dark corner from swinging the whole image. |
| Auto: Speed Up (3) / Speed Down (1) | Adaptation in stops per second. Adapting to darkness is slower, like an eye. |
| Auto: Grey Point (0.18), Metering Mask (0.5) | Target middle grey, and how centre-weighted the metering is. |

**When Time of Day controls exposure** (the default), the EV100 comes from the time of day
instead (see [6. Lighting](06-Lighting.md#exposure)); the trim and the grade still apply.

### Curve and grade

| Setting | Meaning |
| --- | --- |
| Toe Strength (0.95), Shoulder Strength (1.05) | Contrast in the shadows and the highlight roll-off. |
| Colour grade: Global, Shadows, Midtones, Highlights | Each region has **Contrast**, **Gamma**, **Gain**, **Saturation** and **Vibrance**, each with a Total plus Red, Green, Blue and Yellow controls. Defaults add a touch of global contrast and saturation. |

Night Desaturation from the time of day is applied here too.

## Bloom

Bright parts of the image glow, as they do through a real lens and in the eye.

| Setting | Meaning |
| --- | --- |
| Enabled (on), Intensity (0.03) | |
| Threshold (0.9), Knee (0.35) | Brightness above which pixels bloom, and how soft the cut-off is. |
| Method | **Mip Chain**: cheap, soft, shapeless glow (Radius, Mip Levels). **FFT Convolution**: convolves the image with a physically motivated glare shape (sharp core, wide halo, aperture diffraction spikes, red reaching further than blue). Costs more; every bright pixel gets the same shape. |
| FFT: Resolution (512), Kernel Size (0.6), Halo Falloff and Strength, Streak Strength, Aperture Blades and Rotation, Chromatic Spread | The shape of the glare. |

The sun and moon have their own glare, separate from bloom (see
[6. Lighting](06-Lighting.md#sun-and-moon-glare)).

## Lens flares

Physically based lens flares: ghost reflections are traced through the prescription of a real
camera lens (files in `Data/LensFlares/Lenses`), plus a diffraction starburst on each bright
light. Off by default.

| Setting | Meaning |
| --- | --- |
| Lens | Which lens prescription to simulate. Zoom lenses have more glass surfaces and so more, brighter ghosts. |
| Intensity (1), Ghost Intensity (25) | Real flares are faint; games run them well above physical. |
| F-Number (0 = the lens's own) | Stopping down makes ghosts smaller, sharper and shaped like the iris. The bundled zoom lenses look best around f/8. |
| Aperture Blades, Rotation, Roundness | Iris shape, shared by ghosts and starburst. |
| Max Ghosts (64), Ray Grid Size (32), Wavelengths (3) | Quality. 3 wavelengths give RGB fringes. |
| Starburst Intensity, Size | The star on each light. |
| Sun Flares, Local Light Flares, Max Lights (4), Local Light Threshold | Which lights flare. |
| Occlusion Depth Tolerance (0.15 m) | So a bulb inside its own lamp mesh still counts as visible. |
| Sun Cloud Occlusion | Dim the sun's flare behind clouds and fog. |

## Chromatic aberration

Colour fringing towards the edges of the frame, applied last. Strength (2 px at the corner;
1–3 is subtle lens character, above 8 reads as an effect), Falloff (2; higher keeps the centre
clean), Sample Count (6; more samples stop the fringe banding), Optical Centre.

## Path tracer

**Graphics Settings → Path Tracing** replaces the normal image with a path-traced one: light
simulated by following random paths through the scene, which converges to the ground truth.
Needs ray tracing. Not saved with the level.

| Mode | Use |
| --- | --- |
| **Reference** | Accumulates full paths for as long as nothing changes, converging to a noise-free, unbiased image. Noisy while anything moves. For checking what the real-time lighting should look like, and for final stills. |
| **Real Time** | One diffuse and one specular path per pixel per frame, denoised by NVIDIA NRD. Stable in motion and interactive. Replaces RTGI, RTAO and ray-traced shadows while on. |

| Setting | Meaning |
| --- | --- |
| Max Bounces (Reference 6, Real Time 2) | |
| Samples Per Frame / Per Pixel | More converges faster, proportionally more cost. |
| Max Samples (4096) | Reference stops refining here and holds the image. 0 = forever. |
| Screen Cache (Real Time, on) | Paths that reach their last bounce reuse last frame's lighting there: effectively unlimited bounces for no extra rays, which keeps interiors lit only by bounced light from going black. |
| Denoise, Denoiser History Frames (30) | Real time only. |
| Indirect Clamp (20) | Caps bright bounced samples to kill fireflies. 0 = unbiased. |
| Emissive Intensity (1) | Emissive materials light the scene in the path tracer. |
| Ground Albedo (0.2) | Rays leaving the scene downward see a flat ground of this brightness instead of the sky from below. |
| Soft Shadows (on) | Sun, spheres and rect lights at their real size. |
| Alpha Test (on) | Cut-out materials let light through their holes. |
| Debug View | Base colour, normal, roughness/metallic; diffuse and specular lighting in real time. |

To render a final still: Reference mode, set Max Samples, keep the camera still until it
finishes, then **Viewport → Screenshot...**.

## Viewport resolution and screenshots

**Viewport → Resolution...** renders at a fixed resolution regardless of the window size (useful
for matching a target output or for testing performance at 4K). **Viewport → Screenshot...**
saves the current frame.

## Performance checklist

1. **Resource Debug** shows which passes cost the most. Start there.
2. Use an **upscaler** (DLSS/FSR/XeSS Quality, or TAAU) before turning features off.
3. **RTGI**: Diffuse Resolution 2 is usually invisible; keep 1 ray per pixel.
4. **Shadows**: turn off Cast Shadows on lights that don't need them; keep the shadow budget on. Ray-traced shadows cost more where many lights overlap: turn their *Local Lights* off if the shadow maps look good enough there.
5. **Light radii**: as small as looks right.
6. **Volumetric clouds**: Resolution Divisor 2 or 4.
7. **DPLE**: Working Resolution Divisor 2–4, or the Performance preset.
8. **Vegetation**: shorter cull distances for grass; ray tracing only on hero trees.
9. **Virtualized geometry** for dense meshes instead of high-poly LOD 0s.
10. MSAA is the most expensive anti-aliasing; prefer TAA or an upscaler.
