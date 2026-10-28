# 0. Getting started

## What Ptero is

Ptero is a real-time 3D engine and editor for Windows, built on DirectX 12. Its strength is
**lighting**: ray-traced global illumination, a physical sky with a full day–night cycle,
virtual shadow maps, volumetric fog and clouds, and film-style tonemapping, all running live
with nothing to bake. What you see in the editor is what the player sees.

![The editor with Sponza open](<../../Screenshots/screenshot_20260925_191159_editor.png>)

It suits:

- **First-person and exploration games**: walking simulators, horror, puzzle and adventure
  games. A first-person character, physics, interaction scripting and game UI are built in.
- **Atmospheric, lighting-led scenes**: interiors lit by windows and candles, outdoor scenes
  across a whole day, fog, night skies.
- **Small, contained games** such as tabletop and puzzle games. The engine shipped a dice game,
  *Farkle*, built entirely with its own tools.
- **Cinematics and renders**: the Sequencer animates cameras and objects and renders straight to
  image sequences or MP4, with an optional path tracer for final quality.

It is not (yet) a fit for games that need animated characters: there is no skeletal animation
system, so meshes are static (they can move, rotate and be thrown by physics, but not bend).
Networking and consoles are not supported.

![A candle-lit tomb from the Desert level: two candles light the whole room through bounced light](<../../Screenshots/screenshot_20261001_000556.png>)

### Feature tour

| Area | Highlights | Read more |
| --- | --- | --- |
| Lighting | Physical units (lumens, lux, EV100); point, spot and rectangular area lights; animated light styles (fire, candle, strobe) | [6](06-Lighting.md) |
| Sky | Physical sky with sun, moon phases, stars and the Milky Way; exposure that follows the time of day; eye adaptation between interiors and exteriors | [6](06-Lighting.md) |
| Global illumination | Ray-traced GI (with NVIDIA's NRD denoiser), radiance probes, and voxel GI for GPUs without ray tracing | [7](07-Global-Illumination.md) |
| Shadows | Virtual shadow maps: sharp from your feet to the horizon, soft with distance; or ray-traced shadows for the sun and lights | [8](08-Shadows.md) |
| Geometry | Nanite-style virtualized geometry for film-quality scans and sculpts | [4](04-Components.md#virtualized-geometry) |
| Materials | Disney principled BRDF, glass, parallax and tessellation, subsurface scattering for skin | [5](05-Materials.md) |
| Worlds | Sculpted and painted terrain, ocean and lake water, rule-based vegetation, wind, rain, volumetric fog and clouds | [9](09-Outdoor-Worlds.md) |
| Image | TAA, DLSS, FSR, XeSS and frame generation; AgX tonemapping; FFT bloom; physically traced lens flares; a path tracer | [10](10-Image-Quality.md) |
| Gameplay | Blueprint-style Node Graph scripting, a first-person character, Jolt physics, HTML/CSS game UI, FMOD audio, WebM video | [11](11-Gameplay.md) |
| Cinematics | A Sequencer with camera cuts, keys, events and rendering to video | [12](12-Cinematics.md) |
| Shipping | One-click Build Game with encrypted packages, splash screen, icon and Steam support | [13](13-Shipping.md) |
| Importing | FBX, textures, UDIM sets, and Unreal Engine 5 `.uasset` files directly | [3](03-Assets-And-Importing.md) |

## System requirements

| | Minimum | Recommended |
| --- | --- | --- |
| OS | Windows 10 64-bit | Windows 11 64-bit |
| GPU | DirectX 12 GPU | Ray-tracing GPU: NVIDIA RTX 20-series, AMD RX 6000-series, Intel Arc, or newer |
| GPU memory | 6 GB | 8 GB or more |
| System memory | 16 GB | 32 GB |
| Other | The Microsoft Visual C++ Redistributable (x64), which most PCs already have | |

The memory figures are guidance, not hard limits: large scanned assets and 4K textures need more.

### What changes without a ray-tracing GPU

Ptero runs on any DirectX 12 GPU. Features that need ray tracing (DXR 1.1) fall back or turn off:

| Feature | Without ray tracing |
| --- | --- |
| Ray-traced GI, radiance probes | **Voxel GI** takes over automatically, so levels still get bounce light |
| Ray-traced reflections | Screen-space reflections and voxel cone-traced reflections |
| Ray-traced subsurface scattering | Screen-space subsurface scattering |
| Ray-traced ambient occlusion | Not available; screen-space XeGTAO is used |
| Ray-traced shadows | Not available; virtual shadow maps are used |
| Path tracer | Not available |
| Glass | Refraction comes from the screen only |

Other hardware-dependent features:

| Feature | Needs | Without it |
| --- | --- | --- |
| Virtualized geometry at full speed | Mesh shaders (Shader Model 6.5) | A slower path every GPU can run |
| Analytic Edge AA, AAR upscaling | Shader Model 6.1 | The pass is skipped |
| DLSS | NVIDIA RTX GPU | Use FSR, XeSS or TAAU |

## Installing and launching

Ptero needs no installer. A Ptero folder looks like this:

```
Ptero-Engine/
  Binaries/        Editor.exe and the engine
  Data/            content: levels, models, textures, shaders...
  Cache/           rebuilt automatically
  Logs/            created on first run
  ProjectSettings/ created when you first build a game
```

1. Put the folder anywhere you have write access (not inside `Program Files`). A fast SSD helps:
   the engine reads a lot of textures and models.
2. Run **`Binaries/Editor.exe`**. The editor finds `Data/` by looking upwards from its own
   folder, so keep `Binaries` and `Data` side by side.
3. The **first launch** compiles every shader for your GPU and caches the results in
   `Cache/Shaders`. This can take a few minutes. Later launches are much faster.
4. Open a sample level with **File → Open...** (see below). **File → Recent Levels** remembers
   the last ten you opened.

Optional extras:

- **Video import and MP4 rendering** need `ffmpeg.exe` and `ffprobe.exe` in
  `Binaries/ffmpeg/bin`.
- **Importing Unreal `.uasset` files** needs Unreal Engine 5 installed on the same PC.
- **Building a game** may need Visual Studio (see [13. Shipping](13-Shipping.md#build-game)).

If the editor does not start, look at the newest file in `Logs/`. See also the [FAQ](FAQ.md).

## The sample levels

The levels in `Data/Levels` show off different parts of the engine. Open them with **File →
Open...**. Start with the first three.

| Level | What it shows | Start here? |
| --- | --- | --- |
| **`Sponza.json`** | The classic Sponza atrium: radiance-probe GI bouncing sunlight around colonnades and coloured curtains. It also carries a Node Graph script. | **1st**: a good first look at GI |
| **`Desert.level`** | A desert at noon with sculpted terrain and a temple (mastaba) whose interior is lit by candle particle effects. Shows terrain, particles that light the room, and the interior/exterior exposure change. | **2nd** |
| **`Island.level`** | An island with terrain, an ocean and a fort built from modular pieces, at morning light. Over 120 entities. | **3rd**: the outdoor showcase |
| `Forrest.level` | Terrain covered by two vegetation areas: rule-based scattering, wind, radiance-probe GI. | For vegetation |
| `SSSDemo.level` | A character head with subsurface-scattering skin. | For materials |
| `SSS_Demo.json` | The same head with time of day off, lit by a single rect light. | For SSS and area lights |
| `CinematicDemo.level` | Two cameras, a light and a cube animated by `Cinematics/Intro.sequence`. Double-click the sequence in the Asset Browser, turn on Camera View, press Play in the Sequencer. | For cinematics |
| `FirstPerson.level`, `HorrorGame.level` | Modular room kits to walk around with Play. | For first-person play |
| `TerrainTest.json` | A bare terrain and water, to experiment with sculpting. | For terrain |
| `Glass_Rendering.json` | Glass and a traffic cone under one light. | For glass |
| `Room_Showcase.json`, `House.json`, `Tree.json` | Single-asset showcases. | |
| `Mine.json`, `ModelTest.json`, `Sea.json` | Older test levels that refer to models by an absolute path on the original development machine; they may not load elsewhere. | No |

Levels ending in `.json` are an older format. They still load; save them as `.level` to update.

## Where to go next

1. [Tutorial 1: your first level](Tutorial-1-First-Level.md): floor, objects, a light, time of
   day, Play, Save.
2. [Tutorial 2: a first-person level with a door](Tutorial-2-First-Person-Door.md): walking
   around and your first Node Graph script.
3. [1. How Ptero works](01-How-Ptero-Works.md) and [2. The editor](02-The-Editor.md): the
   concepts and the tools behind both tutorials.
4. The [Glossary](Glossary.md) when a term is unfamiliar, and the [FAQ](FAQ.md) when something
   does not work.
