# 1. How Ptero works

This page is the map. Everything else in the manual hangs off the ideas here.

## Two programs: the editor and the game

**The editor** (`Binaries/Editor.exe`) is where you build levels. It shows your level in a
viewport rendered by the same renderer the finished game uses, so what you see while editing
is what the player will see.

**The game** is what you ship. **Release → Build Game** turns your project into a folder with a
named `.exe`, the engine's DLLs and your content packed into encrypted `.ppak` archives (see
[13. Shipping](13-Shipping.md)). The game has no editor in it.

In between there is the **play session**. Pressing **Play** in the editor runs your level as a
game inside the editor process: the player character spawns, scripts run, physics simulates.
When you stop, the level snaps back to exactly how it was before you pressed Play. Nothing
that happens during play is ever saved.

## The project folder

| Folder | What lives there |
| --- | --- |
| `Data/` | **Your content.** Everything the game reads at runtime: levels, meshes, textures, materials, sounds, UI, videos, scripts. The editor finds it by walking up from `Editor.exe`. |
| `Binaries/` | The editor, the engine DLLs and their runtime DLLs. |
| `Cache/` | Things the engine can rebuild: compiled shaders, virtualized-geometry builds. Safe to delete; it fills up again. |
| `Logs/` | One text log per editor session (the 30 newest are kept). The first place to look when something goes wrong. |
| `ProjectSettings/` | `Packaging.key`, the key that encrypts your packaged game's data. Keep it private. |

Inside `Data/`, the editor and engine expect a few folders by name:

| `Data/` subfolder | Holds |
| --- | --- |
| `Levels/` | Level files (`.level`, older ones `.json`) |
| `Geometry/` | Imported models: the source `.fbx` and the engine's cooked `.ptero` beside it |
| `Textures/` | Imported textures (`.dds`) |
| `Materials/`, `MultiMaterials/` | Material files |
| `Particles/` | Particle effects (`.particle`) |
| `Cinematics/` | Sequences (`.sequence`) |
| `Audio/` | FMOD banks (`.bank`) the game plays from |
| `fmod_project/` | The FMOD Studio project the banks are built from (not shipped) |
| `UI/` | Game UI documents (`.rml`, `.rcss`) |
| `Videos/` | WebM videos |
| `Game/` | `GameSettings.json` and player controller graphs |
| `Shaders/` | The engine's shaders, read from disk at startup |
| `LensFlares/` | Lens prescriptions for physically based lens flares |
| `Fonts/`, `Icons/`, `Styles/` | Fonts, editor icons and editor colour styles |

Paths stored in levels and materials are **relative to `Data/`** (for example
`Materials/Fire.json`), so a project can be moved or copied without breaking.

## Levels, entities and components

A **level** is one file holding:

- the **entities** placed in it,
- the level's **Node Graph** script (optional),
- the editor camera position it was saved with,
- and the level's **render settings**: time of day, GI mode and its settings, anti-aliasing,
  upscaler, tonemapper and exposure, bloom, lens flares, fog, clouds, SSR, AO, subsurface
  scattering, DPLE and the shading model.

So each level carries its own look. Opening a level restores the lighting and image settings
it was saved with. A handful of switches are *not* saved per level: virtual shadow maps,
virtualized geometry, MSAA, the path tracer, scene wind and the editor's own preferences.

An **entity** is one thing in the level. Every entity has a **name**, a **transform** (position,
rotation, scale) and a permanent **id** number. The id is what scripts and sequences use to
refer to an entity, so renaming or reordering entities never breaks them.

What an entity *is* comes from its **components**. An entity can have any combination of:

| Component | Makes the entity... |
| --- | --- |
| Mesh | draw a model with a material |
| Light | a point, spot or rectangular area light |
| Camera | a camera that cinematics can cut to and the viewport can look through |
| Audio Emitter | play an FMOD event from its position |
| Decal | project a material onto surfaces inside a box |
| Rigid Body | move under physics during play |
| Particle System | emit GPU particles (fire, smoke, sparks) |
| Rain | rain around the camera |
| Terrain | a heightmap landscape you can sculpt and paint |
| Water | an animated water surface |
| Vegetation Area | fill a volume with procedurally scattered trees, grass and rocks |

[4. Components](04-Components.md) covers each one in full.

There is no separate "static mesh" or "light actor" type. A lamp is one entity with a Mesh and
a Light; a fire is a Particle System that also lights the room.

## Units and directions

- **1 unit = 1 metre.** Models are converted to metres when they are imported, whatever unit
  the source file used.
- **Z is up.** The ground is the XY plane.
- Rotations are entered in degrees in the editor. They are applied X first, then Y, then Z,
  so the Z rotation is always the heading.
- Lights and cameras have a facing direction: spot lights, rect lights and decals point along
  their local **-Z** (so an unrotated one points straight down); cameras and the
  `PlayerStart` look along their local **+Y**.
- Light is physically based: local lights are in **lumens**, the sun and sky in **lux**, and
  exposure in **EV100**. See [6. Lighting](06-Lighting.md) for how those map onto the screen.

## What happens every frame

Ptero is a **deferred** renderer. Each frame runs roughly in this order. You never need to
manage it, but knowing the order explains a lot ("why does fog not hide my particles?",
"why does the upscaler change my bloom?").

1. **Simulation and culling.** Particles and vegetation update on the GPU; virtualized
   geometry decides which clusters to draw for the camera and every shadow.
2. **Shadows.** The sun and shadow-casting lights render into the virtual shadow map. Only
   pages that changed are redrawn.
3. **G-Buffer.** Every opaque surface writes its base colour, normal, roughness, metallic and
   depth. Decals are applied here, and terrain, vegetation and tessellated surfaces draw here.
4. **Global illumination and AO.** Whichever GI system the level uses (ray-traced GI, radiance
   probes, radiance cascades or voxel GI) works out bounced light. Ambient occlusion runs, and
   ray-traced shadows are traced if they are on.
5. **Sky and clouds.** The sky, sun, moon, stars and volumetric clouds draw behind the scene.
6. **Lighting.** Every light, the sun, the sky ambient and the GI are combined per pixel, with
   volumetric fog. Subsurface scattering then blurs light under skin and wax.
7. **Transparent and forward passes.** Glass, water, rain and particles draw over the lit scene.
8. **Reflections.** Screen-space reflections are added.
9. **Temporal and upscaling.** TAA, or DLSS / FSR / XeSS / AAR, turns the frame into a clean
   full-resolution image. Frame generation inputs are prepared here.
10. **Post-processing.** DPLE, sharpening, bloom, sun and moon glare, lens flares, the AgX
    tonemapper and colour grade, and chromatic aberration.
11. **UI.** The game's RmlUi interface draws on top.

The scene renders in **scene-linear HDR** all the way to step 10. Only the tonemapper turns it
into a displayable image, which is why exposure is set there.

## Settings, cvars and the console

Every renderer setting in every window is also a **cvar**: a console variable such as
`rtgi.maxbounces` or `taa.enabled`. Moving a slider and typing `rtgi.maxbounces 3` into the
**Console** change the same value. The console has type-ahead search, so typing part of a name
(`specular`) lists every matching setting with its description. See the
[cvar reference](CVar-Reference.md) for all of them.

## Logs

Everything the engine reports goes to the **Console** panel and to a text file in `Logs/`
named after the session's date and time. The file is written as it goes, so it survives a
crash. If the renderer ever freezes, a watchdog writes a line saying which frame stalled. When
something looks wrong, read the newest log first; most failures (a missing texture, a shader
that failed to compile, a broken material file) are explained there.

## Shaders load from disk

The engine compiles its shaders from `Data/Shaders` when it starts and caches the results in
`Cache/Shaders`. **File → Compile Shaders** rebuilds them. If geometry ever disappears after
an engine update, the shaders and the DLLs are out of step: make sure both come from the same
version.
