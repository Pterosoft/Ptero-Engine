# 7. Global illumination, reflections and ambient occlusion

**Direct light** is light travelling straight from a source to a surface. **Global
illumination (GI)** is everything after that: sunlight bouncing off a floor onto a ceiling, a
red wall tinting a white one, light spilling round a corner into a corridor. Without GI,
anything the lights cannot see directly is black (or flat ambient grey). With it, interiors
light up from their windows and colours bleed between surfaces.

All the settings on this page are in **Windows → Graphics Settings...** and are saved with the
level.

## The GI systems

Ptero has several ways to compute GI. **One runs at a time**, chosen by which are switched on:

| Priority | System | Needs | When it runs |
| --- | --- | --- | --- |
| 1 | **Voxel GI** | Any DX12 GPU | When *Enabled* is on. Also automatically, as a **fallback**, when the level asks for RTGI or probes on a GPU without ray tracing. |
| 2 | **Radiance Probes** | Ray tracing | When *Enabled* is on and voxel GI is not running. |
| 3 | **RTGI** (ray-traced GI) | Ray tracing | When *Enabled* is on and neither of the above is running. **On by default.** |
| — | Path tracer, real-time mode | Ray tracing | Lights every pixel itself, so RTGI and RTAO stop running while it is on. See [10](10-Image-Quality.md#path-tracer). |

The **Global Illumination Solution** selector and the **Radiance Cascades** settings are still in
the window and saved in levels, but radiance cascades are not drawn by the current renderer.
Use one of the three systems above.

### Choosing

| Situation | Use |
| --- | --- |
| Ray-tracing GPU, anything that needs the best quality, interiors lit through windows | **RTGI** |
| Ray-tracing GPU, large open levels, want stable and cheap diffuse GI | **Radiance Probes** |
| No ray tracing, or need the cheapest GI | **Voxel GI** |
| Final-quality stills and reference | **Path tracer** |

All GI systems react to moving lights, time of day and moving objects in real time. Nothing is
baked.

## RTGI: ray-traced global illumination

Every pixel fires rays into the scene (using the GPU's ray tracing hardware), finds what they
hit, lights those hit points with the sun, sky and lights, and carries that light back. The
noisy result is cleaned by **NVIDIA NRD** (the RELAX denoiser), which blends it over time and
space. Glossy surfaces get ray-traced **reflections** from a second set of rays.

| Setting | Meaning |
| --- | --- |
| Enable RTGI | |
| Rays Per Pixel (1) | More rays = less noise, proportionally more cost. 1 with the denoiser is the usual choice. |
| Diffuse Resolution (1) | Trace diffuse GI at full (1), half (2) or quarter (4) resolution and upsample. Half is rarely visible; diffuse bounce light is smooth. |
| Specular Resolution (1) | Reflections at full or half resolution. |
| Max Bounces (2) | How many times light bounces. More fills dark interiors better. |
| Next Event Estimation (on) | At each bounce, explicitly check the sun and lights. Much less noise. Leave on. |
| Shadow Map Visibility (on), Max Texel Size (0.04 m) | Answer "is this bounce point in shadow?" from the shadow map where it is detailed enough, instead of tracing a ray. Cheaper, and sees alpha-tested foliage. Coarser pages trace a ray so thin walls don't leak. |
| Stochastic Lights (on), Light Samples (2) | Test a few lights per bounce (picked by how much they would add) instead of all of them. Keeps cost flat with many lights, at the price of slight extra noise. |
| GI Intensity (1) | Multiplier on the bounce light. 1 is physically correct. |
| Colour Leak (1) | How much surface colour tints the bounce. Above 1 exaggerates colour bleeding. |
| Radiance Clamp (10) | Caps very bright single samples ("fireflies"). Lower is cleaner but dims bright bounces. 0 = off. |
| Use NRD Denoiser (on) | NVIDIA RELAX. Off uses a simple built-in accumulator. |
| NRD: Max Accumulation Time (0.5 s), Disocclusion Threshold (0.2), A-trous Iterations (5), Sharpen (0.35) | Longer accumulation is smoother but slower to react to lighting changes. |
| Temporal / Spatial Reuse (ReSTIR) | Reuse good samples from previous frames and neighbours. Only used when NRD is off. |
| Specular: Enabled (on), Roughness Threshold (0.6), Intensity (0.1) | Ray-traced reflections on surfaces smoother than the threshold. Rougher surfaces take their reflection from the GI. |
| Virtual Geometry BLAS (on), LOD Error, Tier Distance, Tier Count | Ray trace simplified versions of virtualized meshes, coarser with distance. GI is soft enough that the lost detail never shows, and it saves a lot of memory. |
| Ray Debug | Draws the actual GI rays of a grid of pixels (or the centre pixel) as lines in the viewport, coloured by bounce or by the light they carry. *Freeze* keeps them while you fly around them. |

Things to know:

- **GI takes a moment to settle** after a big change (a light switched on, a teleport). That is
  the denoiser accumulating. Shorter accumulation time reacts faster but is noisier.
- **Emissive surfaces** feed RTGI as light sources.
- **Particles** are not ray traced; fire lights the scene through its stand-in light (see
  [Particle System](04-Components.md#particle-system)).
- **Vegetation** only appears in ray tracing for layers with *Contribute to ray tracing* on
  (hero trees); grass would be too expensive.

## Radiance Probes

A 3D grid of probes around the camera. Each probe fires rays every frame and stores the light
arriving from every direction (as spherical harmonics). Surfaces then look up the probes around
them. Several nested grids (**cascades**), each twice as widely spaced, cover distance cheaply:
detail near the camera, reach far away.

Probe GI is smooth and very stable, and its cost does not depend on screen resolution. It is
less detailed than RTGI in small spaces (probe spacing limits how sharp the bounce light can be).

| Setting | Meaning |
| --- | --- |
| Enabled | Takes over from RTGI. |
| Grid X, Y, Z (16 each) | Probes per cascade along each axis. |
| Spacing (2 m) | Distance between probes in the finest cascade. Smaller = more detail, less reach. |
| Cascades (4) | Nested grids; each doubles spacing and reach. |
| Follow Camera (on) | The grid moves with the camera (snapped to whole cells so probes don't flicker). Off fixes it at Origin X/Y/Z, with one cascade. |
| Rays Per Probe (64) | Per update. |
| Trace Resolution (1) | Update all probes each frame (1), half (2) or a quarter (4); the rest keep their previous light. |
| Update Blend (0.15) | How fast probes follow changes. Lower is smoother but laggier. |
| Shadow Map Visibility, Stochastic Lights | As for RTGI. |
| GI Intensity (1) | |
| Specular (on), Specular Intensity (1) | Probes cannot hold reflections, so RTGI's reflection rays run alongside. |
| Show Probes | Draw a sphere at every probe. |

The volumetric fog reads the same probe grid when probes are the GI, so fog is lit by bounce
light too.

## Voxel GI

Works on **every DX12 GPU**, no ray tracing needed. The scene around the camera is turned into
voxels (small cubes) in nested clipmap levels; each voxel is lit by the sun and lights, and
pixels gather bounce light by tracing **cones** through the voxels.

| Setting | Meaning |
| --- | --- |
| Enabled (off) | Use voxel GI even on a ray-tracing GPU. |
| Fallback Without Ray Tracing (on) | Run automatically on GPUs that cannot ray trace when the level asks for RTGI or probes, so the level still has bounce light. |
| Resolution (64) | Voxels per side of each level (32/64/96/128). Memory grows with the cube: 64 × 6 levels is about 40 MB. |
| Levels (5) | Nested levels, each twice the size of the one inside. |
| Voxel Size (0.2 m) | Size of the finest voxels. The finest level covers Resolution × Voxel Size metres. |
| LOD Bias (1) | Voxelise simpler mesh LODs. |
| Sun Shadows (on), Local Light Shadows (on) | Trace shadows through the voxels when lighting them. Without sun shadows, sunlight leaks into interiors. |
| Shadow Map Visibility, Stochastic Lights | As for RTGI. |
| Multi Bounce (1) | Feed previous bounces back in, converging to multiple bounces over a few frames. 0 = one bounce. |
| Temporal Blend (0.15) | Smoothing. Lower is steadier (hides flicker) but slower to follow changes. |
| Diffuse Cones (5) | 1, 5 or 9 cones per pixel. |
| Half Resolution (on) | Trace at half resolution; a quarter of the cost, almost invisible on diffuse light. |
| Max Trace Distance (60 m), Surface Offset (1.5 voxels) | How far cones travel before taking the sky, and how far from the surface they start. |
| GI Intensity (1) | |
| Specular (on), Roughness Threshold (0.8), Intensity (1) | Cone-traced reflections. Screen-space reflections, when on, take over wherever they find a hit. |

Voxel GI is blurrier than RTGI (voxels are coarse) and thin walls can leak a little light if
voxels are larger than the wall is thick. Smaller Voxel Size fixes leaks at the cost of reach.

## Reflections

Reflections come from several places, combined automatically:

1. **Screen-space reflections** (when on): accurate, but can only reflect what is on screen.
2. **Ray-traced reflections** from RTGI (or cone-traced from voxel GI): reflect anything, on
   surfaces smoother than the roughness threshold.
3. Rough surfaces take a blurry reflection from the GI itself.

### Screen-Space Reflections

| Setting | Meaning |
| --- | --- |
| Enabled (off) | |
| Technique | **Ray March**: one sharp ray per pixel; mirror-like. **FidelityFX SSSR**: AMD's stochastic SSR with a denoiser, so glossy surfaces get properly blurred reflections. |
| Intensity (1), Max Roughness (0.6) | Strength, and the roughest surface that gets SSR. |
| Ray March: Max Steps, Step Size, Step Growth, Thickness, Refine Steps, Max Distance, Edge Fade | Reach, precision and how thick the depth buffer is assumed to be. Too thin and rays pass through thin objects; too thick and they stick to the wrong surface. |
| SSSR: Depth Thickness, Max Traversal Intersections, Samples Per Quad, Temporal Stability, ... | AMD's controls. Max Traversal Intersections is the main cost control. |

Reflections fade at the screen edges, where there is no information left to reflect.

## Ambient occlusion

Ambient occlusion darkens creases, corners and contact points where ambient light cannot reach.

| System | Notes |
| --- | --- |
| **XeGTAO** (on by default) | Intel's screen-space Ground Truth AO. Cheap, any GPU. Quality Low–Ultra, Radius (1 m), Final Value Power (higher = darker), Intensity, denoise passes. |
| **Ray-Traced AO** (off) | Fires short rays per pixel. Accurate contact shadowing that does not depend on the screen. Needs a ray-tracing GPU; while it runs it **replaces XeGTAO**. See below. |
| **DPLE** | Multiscale AO and contact shadows as part of the lighting enhancer below. |

### Ray-traced AO

Settings are in **Graphics Settings → Ray Traced Ambient Occlusion**. Each pixel sends *Rays Per
Pixel* short rays out over the surface. The share that hit something within *Max Ray Length* is
how occluded the point is. NVIDIA NRD then smooths the result over time and space.

| Setting | Meaning |
| --- | --- |
| Rays Per Pixel (16) | More rays, less noise, more cost. |
| Max Ray Length (1 m) | How far away something can be and still darken the point. Short gives tight contact darkening; long darkens whole rooms. |
| Ray Bias (0.005 m) | Minimum offset of the rays from the surface. Raise it if flat surfaces look dirty. |
| AO Power (1.5) | Above 1 darkens, below 1 lightens. |
| AO Intensity (1) | Blend of RTAO over the material's own AO map. |
| Local Light Occlusion (0.5) | How much AO also darkens point, spot and rect lights. |
| Debug View | Final AO, Raw Noisy AO (one frame, before denoising), NRD AO. |

**Local Light Occlusion.** AO is meant for light that arrives from everywhere: the sky and
bounced light. Direct light has its own shadows. But in a room lit only by lamps or candles
there is almost no sky light, so AO would have nothing to darken and would seem to do nothing.
This setting lets it darken the local lights too. 0 restores sky light and GI only. The sun is
never affected.

Like ray-traced shadows, RTAO's rays leave along the surface's true shape rather than its normal
map, so bumpy materials do not darken themselves. The path tracer replaces RTAO while it runs in
real time.

## Subsurface scattering

Scene-wide settings for materials that use subsurface scattering (skin, wax, marble, leaves).
What each surface scatters like is a material property; see
[5. Materials](05-Materials.md#subsurface-scattering).

| Setting | Meaning |
| --- | --- |
| Enabled (on) | Costs nothing when no subsurface material is on screen. |
| Mode | **Screen space**: a depth-aware blur of the lighting. Cheap and stable, but cannot see past the screen edge or around silhouettes. **Ray traced**: probes the real mesh with rays, so light scatters around ears, noses and fingers and works behind silhouettes; thickness for back-lighting is measured through the actual mesh. Needs ray tracing; falls back to screen space without it. |
| Quality (screen space) | 11, 17 or 25 samples. |
| Follow Surface | Stop the blur at depth edges so it doesn't bleed onto the background. |
| Transmission, Transmission Intensity | Light through thin parts (ears glowing red against a light). |
| RT Samples (12), ReSTIR (on), History Length (5) | Ray-traced quality. ReSTIR reuses good samples over time for about half the noise. |

## DPLE

**Windows → DPLE...** opens the **Deterministic Photoreal Lighting Enhancer**, a post-lighting
pass ported from an Unreal plugin. It adds no new light; it re-presents the lighting the
renderer already computed so it reads more like a photograph:

| Section | Does |
| --- | --- |
| Multiscale Ambient Occlusion | AO at three scales at once: micro (pores, weave, 6 cm), contact (objects touching, 25 cm), broad (room corners and alcoves, 1.2 m), with a cap so they cannot stack into black creases. Applied only to the estimated *indirect* part of each pixel, never to direct sunlight, which is what stops post-process AO looking like dirt. |
| Contact Shadows | Short screen-space shadows from the sun, for the small gaps the shadow map is too coarse for. |
| Material Response | Skin (subsurface materials) gets less AO and warms in occlusion; foliage gets less AO and keeps its saturation; wet-looking surfaces (smooth non-metals) respond differently. |
| Micro-Specular | Occludes reflections in crevices, and brings out grain-scale detail in the sun's highlight. |
| Indirect Estimation | How each pixel's light is split into direct and indirect. |
| Temporal / Denoise | Stabilises the result over frames. |
| Detail Enhancement (off) | Sharpens fine detail. Off by default: it re-amplifies what TAA and upscalers remove. Check it on slow and fast pans. |

**Presets**: Subtle, Balanced (the defaults), Strong, Performance. *Working Resolution Divisor*
(2) is the main performance control: it runs the passes at half the output resolution. Every
section has its own reset button. DPLE is off by default; a level opts in.

## Debugging GI

- **View Mode → Global Illumination** shows the raw GI, its luminance, reflections only, and
  each system's internal views (voxel albedo and radiance, clipmap levels).
- **RTGI → Ray Debug** draws the rays themselves.
- **Radiance Probes → Show Probes** draws the probe grid.
- Black or blotchy interiors usually mean too few bounces, a GI intensity of 0, or (voxel GI)
  voxels too coarse for the walls.
