# User manual checklist

What a Ptero Engine user needs to read to make a level and a game with the editor, with no
programming. Each item is a page or section of the manual. The sub-points are what that page
should cover. Every page needs screenshots.

`[~]` means something already exists in `Documentation/`, but it explains how the feature is
built more than how to use it, so the user-facing part has to be extracted and rewritten.

Priority: **P0** is needed to get started, **P1** is needed to make a finished level or game,
**P2** is depth for people who want to tune things.

---

## 1. Getting started

- [ ] **P0** What Ptero is: what kind of games it suits, a feature tour with screenshots
- [ ] **P0** System requirements: Windows, DirectX 12 GPU; which features need a ray-tracing GPU (DXR 1.1) or mesh shaders, and what happens without one
- [ ] **P0** Installing and launching the editor
- [ ] **P0** The sample levels: what each one in `Data/Levels` shows and which to open first
- [ ] **P0** Tutorial: your first level, step by step (new level, place a floor and a few meshes, add a light, set the time of day, press Play, save)
- [ ] **P1** Tutorial: a first-person walkable level with a door or pickup scripted in Node Graph
- [ ] **P1** Glossary: entity, component, level, asset, material, cvar, GI, LOD, and so on
- [ ] **P2** FAQ and known issues

## 2. The editor

- [~] **P0** Editor layout: toolbar, viewport, Components, Level Explorer, Properties, Console; docking, floating panels, resetting the layout
- [ ] **P0** Moving around the viewport: camera controls, speed, focusing on the selection
- [ ] **P0** Selecting and transforming: gizmos (keys 1–4), Movement and Rotation snap, copy/paste, delete
- [~] **P0** Keyboard shortcuts reference (a table already exists in `QtEditor.md`)
- [~] **P0** Undo and redo: what is covered (level edits) and what isn't (render settings, node graphs)
- [ ] **P0** Levels: New, Open, Save, Save As, Recent Levels, unsaved-changes prompts
- [ ] **P1** View modes: Lit, G-Buffer, All Buffers, Placement Icons & Light Shapes, Grid, Fullscreen
- [ ] **P1** Level Explorer: finding, renaming and organising entities
- [ ] **P1** Viewport cameras: Pilot Selected Camera, Lock to Cinematic Camera
- [ ] **P1** Editor Settings window
- [ ] **P1** Taking screenshots (Screenshot window, resolutions, where files go)
- [ ] **P2** Renderer Statistics overlay: how to read it
- [~] **P2** Console: reading the log, typing commands, changing settings with cvars

## 3. Assets and importing

- [ ] **P0** Asset Browser: folders, Import Here, rename, new folder, dragging assets into the level
- [ ] **P0** Importing models (FBX): scale and orientation expectations, materials coming in, what to do when a model is huge, tiny or sideways
- [ ] **P0** Importing textures: supported formats, colour vs data (normal/roughness) textures, UDIM tiles
- [ ] **P1** Generate LODs and Generate Collisions: when to use them, what they do
- [ ] **P1** Importing from Unreal Engine (`.uasset`): what is needed (an Unreal install), what comes across
- [ ] **P1** Importing audio and setting up FMOD banks
- [ ] **P1** Importing video (WebM; ffmpeg converts other formats)
- [ ] **P2** Exporting from Substance Painter with the Ptero shader (`Tools/SubstancePainter`)
- [ ] **P2** Recommended asset budgets: texture sizes, triangle counts, how many lights

## 4. Building a level: components

One page each. For each: how to add it, every property in plain language, a typical setup,
common mistakes.

- [ ] **P0** Transform and Name
- [ ] **P0** Mesh: assigning a mesh and materials, shadows, LODs, Virtual Geometry
- [ ] **P0** Lights: Point, Spot and Rectangle (area) lights; colour, intensity units, radius, source size and soft shadows, cones, light styles and flicker, shadow budget
- [ ] **P1** Camera
- [ ] **P1** Decal
- [ ] **P1** Rigid Body (physics): static vs moving bodies, mass, collision shapes, generated collision
- [ ] **P1** Audio Emitter: choosing an event, 3D sound, Resonance Audio settings
- [ ] **P1** Particle System (and Edit Particle Effect)
- [ ] **P1** Terrain (see section 6)
- [ ] **P1** Water: shape, waves, colour, foam
- [ ] **P1** Vegetation Area: scatter layers, density, variation, wind and interaction
- [ ] **P2** Rain

## 5. Materials

- [ ] **P0** Material Editor basics: creating a material, assigning textures
- [ ] **P0** The Principled BRDF parameters explained: base colour, roughness, metallic, specular, normal maps
- [ ] **P1** Emission
- [ ] **P1** Transparency and glass (Transmission)
- [ ] **P1** Subsurface scattering: skin, wax, leaves
- [ ] **P1** Parallax occlusion and tessellation/displacement: height maps, when to use which
- [ ] **P1** Multi-Material vs Single Material
- [ ] **P2** UV Transform, Surface filtering, Micro Specular, Detail Enhancement
- [ ] **P2** Eye material (Iris layout)
- [ ] **P2** Material presets

## 6. Terrain and outdoor scenes

- [ ] **P1** Creating terrain and importing a heightmap (use 16-bit sources; 8-bit terraces)
- [ ] **P1** Terrain Tool: Raise, Lower, Flatten, Smooth brushes; radius and strength
- [ ] **P1** Painting terrain layers and the layer mask
- [ ] **P1** Placing vegetation on terrain
- [ ] **P2** Tutorial: an outdoor scene from scratch (terrain, water, vegetation, sky, fog)

## 7. Lighting and atmosphere

- [ ] **P0** Lighting overview: sun, sky, local lights, global illumination, exposure, and how they fit together
- [ ] **P0** Time of Day window: sun and moon position, night, stars, animating the day
- [ ] **P1** Scene Settings: sky, atmosphere, fog and per-level defaults
- [ ] **P1** Exposure: auto exposure vs manual EV, why a light that looks right at noon is dark at night
- [ ] **P1** Global illumination: which GI mode to pick (RTGI, Radiance Probes, Radiance Cascades, Voxel GI) for which GPU and scene
- [ ] **P1** Shadows: sun shadows, Virtual Shadow Maps, point-light shadows, contact shadows, soft shadows
- [ ] **P1** Volumetric fog and god rays
- [ ] **P1** Volumetric clouds
- [ ] **P2** Interior lighting guide: rooms lit by windows and lamps without noise or light leaks
- [ ] **P2** Lighting troubleshooting: noise, flicker, light leaking, GI taking time to settle

## 8. Graphics settings and image quality

- [ ] **P0** Graphics Settings window tour, and presets
- [ ] **P1** Anti-aliasing: TAA, SMAA, MSAA, Analytic Edge AA; which to pick
- [ ] **P1** Upscaling and frame generation: DLSS, FSR, XeSS; which GPU supports what
- [ ] **P1** Post effects: AgX tonemapper, bloom, lens flares, chromatic aberration, sharpening
- [ ] **P1** Reflections: screen-space reflections and what reflects where
- [ ] **P1** Ambient occlusion: XeGTAO, RT AO, Multiscale AO (DPLE)
- [ ] **P2** Path Tracing: reference renders and the real-time mode
- [ ] **P2** Performance guide: which settings cost the most, reading the statistics, viewport resolution
- [ ] **P2** Debug views (G-Buffer / RT Debug, Resource Debug) for diagnosing visual problems

## 9. Gameplay without code

- [~] **P0** Node Graph basics: opening it, the canvas, adding nodes (Q), comments (C), connecting pins, white execution vs coloured value wires
- [ ] **P0** Tutorial: a first script (open a door when the player gets close)
- [ ] **P1** Events: begin play, tick, overlaps, input
- [ ] **P1** Variables, functions and graph libraries (Export / Import as Library)
- [ ] **P1** Working with entities and physics from a graph
- [ ] **P1** Player controllers: the first-person controller, choosing C++ vs Node Graph controller in Game Settings
- [ ] **P1** Debugging a graph while playing
- [ ] **P2** Node reference: every node, its pins and an example

## 10. Playing and testing

- [ ] **P0** Play, Play In Viewport, Play In New Window, Stop
- [ ] **P1** Startup level and Game Settings
- [ ] **P2** Running a level standalone (`Editor.exe --game <level>`)

## 11. Game UI

- [~] **P1** Making a HUD: RmlUi documents (`.rml` and `.rcss`) in `Data/UI`, fonts, images
- [~] **P1** UI Editor: previewing a document, clicking it, reading the event trace
- [ ] **P1** Menus: main menu, pause menu, buttons that do things
- [ ] **P2** Game UI Inspector and Reload Game UI while playing

## 12. Cinematics

- [~] **P1** Ptero Sequencer basics: creating a sequence, adding entities, tracks and keys, playback range
- [ ] **P1** Camera cuts and the cinematic camera
- [ ] **P1** Event tracks: triggering gameplay from a sequence
- [ ] **P1** Playing a sequence in game (from Node Graph)
- [ ] **P2** Curve editing: interpolation and tangents, scaling a selection
- [ ] **P2** Tutorial: a short cutscene

## 13. Audio and video

- [ ] **P1** Audio Manager: loaded banks, failures, previewing events, Stop All
- [ ] **P1** Using the bundled FMOD Studio project (`Data/fmod_project`)
- [~] **P1** Video Player and playing video in game
- [ ] **P2** Spatial audio with Resonance Audio

## 14. Shipping a game

- [ ] **P1** Game Settings: name, startup level, player controller, icons, splash
- [ ] **P1** Build Game: what it produces, where, and how to test the result
- [ ] **P1** Packaged data and encryption: what the package key is and why to keep it private
- [ ] **P2** Extract Package
- [~] **P2** Publishing on Steam: App ID, achievements, DRM wrapping

## 15. Reference

- [ ] **P1** Every editor window, one short entry each (Windows menu order)
- [ ] **P1** Units and conventions: metres, Z-up, light units, colour (sRGB vs linear)
- [~] **P2** Console commands and cvars (`Console.md` has the table)
- [ ] **P2** File types a user sees: `.level`, `.material`, `.particle`, `.nodegraph`, sequences, `.ppak`

## Existing docs to mine

| File | User-facing part worth keeping |
| --- | --- |
| `QtEditor.md` | Shortcuts, undo, light shape gizmos |
| `NodeGraph.md` | "Working in the canvas", node set, functions and libraries |
| `Cinematics.md` | "Using Ptero Sequencer" |
| `Console.md` | Using the console, cvar table |
| `UiEditor.md` | Most of it |
| `Video.md` | Importing and playing video |
| `Steam.md` | Turning Steam on |
| `wip/Time of Day.docx`, `wip/AgX.docx` | Background for the lighting and tonemapping pages |

Developer docs (building from source, architecture, C++ game API) are a separate list.
