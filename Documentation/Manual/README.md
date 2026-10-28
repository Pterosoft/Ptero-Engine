# Ptero Engine manual

How the engine works and how each feature works, from the point of view of someone building
levels and games in the editor. No programming is needed for anything in here.

New to Ptero? Start with **Getting started** and the two tutorials, then read pages 1 and 2.
After that, jump to whatever you are working on.

| Start here | |
| --- | --- |
| [0. Getting started](00-Getting-Started.md) | What Ptero is, system requirements, installing, the sample levels |
| [Tutorial 1: your first level](Tutorial-1-First-Level.md) | Floor, objects, a light, time of day, Play, Save (20 minutes) |
| [Tutorial 2: a first-person level with a door](Tutorial-2-First-Person-Door.md) | A walkable room and your first Node Graph script (30 minutes) |
| [Glossary](Glossary.md) | Every term used in the manual |
| [FAQ and known issues](FAQ.md) | When something doesn't work |
| [CVar reference](CVar-Reference.md) | All 563 console settings: what each does, its values and its default |

| Page | What it covers |
| --- | --- |
| [1. How Ptero works](01-How-Ptero-Works.md) | The big picture: editor and game, the `Data` folder, levels, entities and components, units, what a frame does, where settings are saved |
| [2. The editor](02-The-Editor.md) | Layout, moving around, placing and editing things, undo, levels, every window |
| [3. Assets and importing](03-Assets-And-Importing.md) | Models, textures, Unreal assets, Substance Painter, audio, video; LODs, collisions, asset budgets |
| [4. Components](04-Components.md) | Every component an entity can have and every property on it |
| [5. Materials](05-Materials.md) | The material model, every material setting, glass, decals, particles, water, terrain layers |
| ↳ [The Disney principled BRDF](05a-Disney-Principled-BRDF.md) | In depth: what a BRDF is, every lobe, the formulas Ptero uses, an authoring guide |
| [6. Lighting, sky and time of day](06-Lighting.md) | Light units, the three light types, light styles, sun, moon, stars, exposure |
| [7. Global illumination, reflections and AO](07-Global-Illumination.md) | The four GI systems and how to choose, reflections, ambient occlusion, DPLE |
| [8. Shadows](08-Shadows.md) | Virtual shadow maps, soft shadows, the shadow budget, ray-traced shadows, fallbacks |
| [9. Outdoor worlds](09-Outdoor-Worlds.md) | Terrain, water, vegetation, wind, rain, volumetric fog and clouds |
| [10. Image quality and post-processing](10-Image-Quality.md) | Anti-aliasing, upscalers, frame generation, tonemapping, bloom, lens flares, the path tracer |
| [11. Gameplay](11-Gameplay.md) | Play sessions, the player character, Node Graph scripting, physics, game UI, audio, video |
| [12. Cinematics](12-Cinematics.md) | Ptero Sequencer: cameras, tracks, keys, camera cuts, events, rendering to video |
| [13. Shipping a game](13-Shipping.md) | Game Settings, Build Game, packages, the player's options, Steam |

**Adding a page?** Give it a website excerpt and page order in [EXCERPTS.md](EXCERPTS.md) in the
same change.

## Hardware

Ptero runs on Windows with a DirectX 12 GPU. Several features need more than that:

| Feature | Needs | Without it |
| --- | --- | --- |
| Ray-traced GI, radiance probes, radiance cascades, RT ambient occlusion, ray-traced shadows, ray-traced subsurface scattering, path tracer | Ray tracing (DXR 1.1, i.e. RTX 20-series / RX 6000 / Arc or newer) | Voxel GI takes over the GI; SSS falls back to screen space; shadows use the shadow maps; the others are unavailable |
| Virtualized geometry at full speed | Mesh shaders (Shader Model 6.5) | A slower vertex-shader path is used |
| Analytic Edge AA, AAR upscaling | Shader Model 6.1 barycentrics | The pass is skipped |
| DLSS | NVIDIA RTX GPU | Use FSR, XeSS or TAAU |
| XeSS frame generation with more than one generated frame | Intel GPU | One generated frame |

## Conventions used in this manual

- **Menu → Item** means a menu path in the editor's main menu bar.
- Distances are in **metres**, angles in the editor are in **degrees**, and **Z is up**.
- Default values are given in parentheses, for example *Radius (5 m)*.
