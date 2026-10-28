# 3. Assets and importing

Everything a level uses is a file under `Data/`. This chapter covers getting files in and what
the engine does with them.

- [The Asset Browser](#the-asset-browser)
- [Files that appear in Data on their own](#files-that-appear-in-data-on-their-own)
- [Models (FBX)](#models-fbx)
- [LODs](#lods)
- [Collisions](#collisions)
- [Textures](#textures)
- [Unreal Engine 5 assets](#unreal-engine-5-assets)
- [Substance 3D Painter](#substance-3d-painter)
- [Audio (FMOD)](#audio-fmod)
- [Video](#video)
- [Asset budgets](#asset-budgets)
- [File types you will see](#file-types-you-will-see)

## The Asset Browser

**Windows → Asset Browser...** shows the `Data/` folder: a **folder tree** on the left and the
**contents** of the selected folder on the right, each file with an icon for its type. Above them
the browser shows the Data folder's location, the current folder, and the result of the last
action (green for success, red for failure).

| Button | Does |
| --- | --- |
| **Import...** | Pick any mix of models, Unreal assets, textures and videos. They are imported **into the current folder**, each by the right importer for its extension. |
| **New Folder** | Create a folder inside the current one. |
| **Rename** | Rename the selected file or folder. |
| **Copy**, **Paste** | Copy the selected file or folder, then paste it into the current folder. A name clash gets a unique name. |
| **Remove** | **Permanently delete** the selected file or folder (see the warning below). |
| **Generate LODs** | Rebuild the selected model's levels of detail ([LODs](#lods)). |
| **Generate Collisions** | Build the selected model's convex collision hulls ([Collisions](#collisions)). |
| **Refresh** | Re-read the folder from disk. |

Right-click a file or folder for the same actions, plus **Import Here...** (import into that
folder) and the type-specific ones below.

> **Warning: Remove deletes immediately and permanently.** There is no confirmation, it does
> not go to the Recycle Bin, a folder is removed with everything inside it, and it cannot be
> undone. Keep your project under version control or backed up.

**Double-click** opens a folder, and opens these files in their tools:

| File | Opens in |
| --- | --- |
| `.webm` video | the Video Player (also *Play Video* in the right-click menu) |
| `.particle` effect | the Particle Editor (also *Edit Particle Effect*) |
| `.sequence` | the Sequencer (also *Open in Sequencer*) |

Levels open from **File → Open...**; materials from the Material Editor's **Open...**; meshes are
assigned with **Select Geometry (.ptero)** on a Mesh component.

Assets cannot be dragged from the Asset Browser into the viewport or onto Properties yet. To use
an asset, place an entity from the Components panel and pick the file in its Properties.

### Importing

Imports run in the background behind a progress window listing each file as it finishes (`OK`
or `ERR` with the reason). **Cancel** stops a video conversion in progress and skips whatever is
still queued; a model or texture already being processed finishes first. Large 4K textures and
long videos can take a while before the first file completes.

| Kind | Extensions | Result |
| --- | --- | --- |
| Models | `.fbx` | The FBX plus a cooked `.fbx.ptero`, a multi-material, textures and collision hulls |
| Unreal Engine 5 assets | `.uasset` | Meshes, textures and materials, converted |
| Textures | `.png .jpg .jpeg .tga .bmp .hdr .dds` | A compressed `.dds` |
| Videos | `.webm .mp4 .mov .mkv .avi .m4v .wmv .mpg .mpeg .flv .ogv .3gp .ts .mts .m2ts .gif` | A `.webm` |

Sound is not imported here; it comes from FMOD Studio banks ([Audio](#audio-fmod)).

## Files that appear in Data on their own

The editor watches `Data/` while it runs. When files that still need converting appear there from
outside the editor (copied in from Explorer, unpacked from a Fab download, exported from Substance
Painter or another tool), it waits until they have finished copying and then shows a
notification:

> **New files detected**: 12 new files in Data/Props/Barrels: 1 FBX model, 11 textures.
> **Import** / **Don't Import**

**Import** converts them **in place**, next to where they arrived (models first, so their
textures are mostly converted along the way). **Don't Import** ignores those files for the rest
of the session.

Only files that still need converting are offered: an FBX without an up-to-date `.ptero`, an image
without a `.dds` beside it (or in `Data/Textures`), a video that is not yet WebM, an Unreal asset
not yet converted. Files in folders the engine reads as they are (`UI`, `Icons`, `Styles`, `Fonts`,
`Shaders`, `Levels`, `Audio`, `fmod_project`, `Game`) are never offered.

This is the quickest workflow for big asset packs: unzip them straight into a folder under
`Data/`, and accept the notification.

## Models (FBX)

### What an import does

1. **Copies** the FBX into the target folder, so the project owns its content.
2. **Cooks** it into a `.ptero` file beside it (`Chair.fbx` → `Chair.fbx.ptero`), the format the
   engine and the packaged game actually load:
   - **Units** are converted to **metres**, whatever the FBX used. Files from Maya, Blender and
     Houdini (centimetres) and 3ds Max (often inches) all arrive at their real size.
   - **Axes** are converted to Ptero's **Z-up**, whether the file is Y-up or Z-up. A model's front
     (the side that faced the *front* view in your 3D tool, -Y in 3ds Max terms) faces -Y in
     Ptero, and its left and right are kept.
   - Each **FBX material slot** becomes a sub-mesh, in slot order.
   - **LODs** are generated ([below](#lods)).
3. Writes a **multi-material** to `Data/MultiMaterials/<name>.json` with one sub-material per
   FBX material slot, named after the FBX materials.
4. **Imports the base colour textures** the FBX materials reference. For each, the importer looks
   for the texture next to the source FBX (by file name, with any common image extension) and
   imports it into `Data/Textures`. Only the **diffuse / base colour** texture is carried over
   from FBX materials; add normal, roughness and other maps in the Material Editor afterwards.
5. **Generates collision hulls** ([below](#collisions)). If this fails, the import still succeeds;
   run Generate Collisions later.

When a newer engine version changes the `.ptero` format, older cooked files are re-cooked from
their FBX automatically the next time they load (and their collision hulls rebuilt).

### Getting models right

| Check | Why |
| --- | --- |
| **Model at real-world size.** A door should measure about 2 m in your 3D tool. | The importer converts units, but it cannot know that a model built 100× too big was meant to be small. |
| **Apply transforms** (Blender: *Apply All Transforms*; Maya: *Freeze Transformations*; Max: *Reset XForm*) before export. | Unapplied scales and rotations are baked in as the importer sees them, which is not always what the viewport showed. |
| **Pivot / origin where you want to place it from**, usually the base centre. | The FBX origin becomes the entity's position. A pivot far from the mesh makes it hard to place and rotate. |
| **One material slot per distinct surface.** | Each slot is one sub-material, and one draw. |
| **Textures next to the FBX** (or embedded paths that resolve). | That is where the importer looks for them. |
| **Triangulated or quad meshes, no n-gons with holes.** | The FBX SDK triangulates, but complex n-gons can triangulate badly. |
| **Tangent-space normal maps in the DirectX convention** (green down). | For OpenGL-convention maps, tick *Flip Normal Green (OpenGL)* in the material. |

### When a model is wrong

| Symptom | Likely cause and fix |
| --- | --- |
| **Huge** (or you are inside it) | Exported at the wrong unit scale, or built at the wrong size. Fix the scale in your 3D tool, apply transforms and re-import; or set the entity's Scale. |
| **Tiny** or invisible | The reverse: check the unit scale on export (Blender's FBX exporter: *Apply Scalings: FBX All*). Look for it in the Level Explorer and type a large Scale to find it. |
| **Lying on its side** | Unapplied rotation in the source file, or an exporter forcing a different up axis. Apply transforms and re-export; or rotate the entity 90° about X. |
| **Facing backwards** | The model's front faces +Y in your tool. Rotate the entity 180° about Z, or turn the model in the source. |
| **Inside out / see-through from outside** | Flipped normals in the source. Recalculate normals outward and re-export, or tick *Double Sided* on the material as a stopgap. |
| **Hard to place, rotates around a far point** | Pivot far from the mesh. Move the origin in the source. |
| **Shading looks faceted** | No smoothing groups / custom normals exported. Enable smoothing in the export settings. |
| **Grey, untextured** | The base colour texture was not found next to the FBX. Copy it there and re-import, or assign textures in the Material Editor. |

### Dense meshes

Photo scans, ZBrush sculpts and kitbash pieces with millions of triangles should use
**Virtualized Geometry** on their Mesh component instead of relying on LODs: the engine then
draws each cluster at the detail its size on screen needs. See
[4. Components](04-Components.md#virtualized-geometry).

## LODs

A **level of detail** (LOD) is a simpler version of a model the renderer switches to as the model
gets smaller on screen. Importing an FBX generates **up to three LODs** after the original
(LOD 0): each one aims for **half the triangles** of the one before, so LOD 3 has about an
eighth of the original's triangles. Simplification works per material slot, keeps the
simplification error within 2% of the mesh's size, and stops early when a mesh cannot be
simplified any further (very low-poly meshes may get fewer LODs).

**Generate LODs** (Asset Browser, with a model selected) rebuilds them. Use it after re-exporting a
model outside the editor, or if the LODs are missing.

Per entity, the Mesh component's **LOD Usage Scale** changes how eagerly it switches (above 1 =
sooner and cheaper, below 1 = keeps detail longer), and **LOD Debug View** forces one LOD so you
can check what each looks like.

When to care:

- Props seen from far away (rocks, crates, furniture in big levels): LODs save a lot.
- Hero objects always seen close: LODs rarely kick in.
- Very dense meshes: use virtualized geometry instead.
- If a LOD switch is visible as a pop, lower LOD Usage Scale for that entity.

## Collisions

Every mesh already **collides as static level geometry** using its exact triangles: the player
walks on it, physics objects bounce off it. You need generated collisions only for **moving**
physics objects.

A **dynamic** rigid body (one that falls and gets pushed) cannot use exact triangles; physics
needs convex shapes. **Generate Collisions** runs **convex decomposition** (CoACD): it splits the
mesh into a set of convex hulls that follow its shape closely, so a chair collides as a seat,
a back and four legs rather than one box. The hulls are saved to `<name>.collisions.json` beside
the `.ptero`.

- It runs automatically when an FBX or Unreal mesh is imported; run it again after re-exporting.
- A Rigid Body with Shape **Auto** uses the hulls when they exist (otherwise the mesh's single
  convex hull). Box, Sphere and Capsule shapes ignore them.
- **Proxy** on the editor toolbar draws every mesh's hulls as green wireframes, so you can check
  them.
- Decomposition can take a few seconds to minutes for a dense mesh.

When **not** to bother: static scenery (walls, floors, terrain props). It already collides
exactly.

## Textures

Importing an image converts it to a GPU-compressed `.dds` with a full mipmap chain. A texture
imported on its own goes into the folder you import into; textures pulled in by an FBX go into
`Data/Textures`; files converted by the Data watcher go next to the source image.

### Formats

| Source formats | `.png`, `.jpg`/`.jpeg`, `.tga`, `.bmp`, `.hdr` (and `.dds`, used as it is if already inside Data) |
| --- | --- |
| Output | BC-compressed `.dds` (about 1 byte per pixel; 4K ≈ 22 MB with mips, 2K ≈ 5.6 MB, 1K ≈ 1.4 MB) |
| Size | Any; powers of two (512, 1024, 2048, 4096) compress and mipmap best |

### Colour textures and data textures

Textures hold two kinds of thing, and the engine stores them differently:

| Kind | Examples | Stored as |
| --- | --- | --- |
| **Colour** | Base colour / albedo, emissive | BC3 sRGB: compression tuned for colour, mipmaps filtered in the right colour space |
| **Normal maps** | Normal | BC5: two high-quality channels, the right choice for normals |
| **Data** | Roughness, metallic, AO, height, opacity, masks, packed maps | BC3 linear: values kept as numbers, not colours |

The importer picks the kind **from the file name**:

| If the name contains... | Treated as |
| --- | --- |
| `normal`, `_n.`, `_nor` | Normal map |
| `roughness`, `metallic`, `ao`, `ambientocclusion`, `opacity`, `height`, `displacement`, `mask`, `principled`, `clearcoat`, `sheen`, `aniso`, `flowmap`, `direction` | Data |
| anything else | Colour |

So **name textures by what they are**: `Brick_BaseColor.png`, `Brick_Normal.png`,
`Brick_Roughness.png`. When a material uses a texture, it is read in the right colour space for
its slot regardless, so a misnamed texture still *displays* correctly. What a wrong name costs
is quality: a normal map that is not named as one is compressed as colour (blockier normals), and
a colour texture that happens to contain `ao` (`Chaos_Albedo.png`) has its mipmaps filtered as
data, so it shifts slightly in tone at a distance. Rename and re-import to fix it.

HDR (`.hdr`) sources are genuinely linear and imported as such.

### Packed maps

Ptero reads two packed layouts, so exports from most tools load without repacking:

| Material slot | Channels |
| --- | --- |
| Metallic-Roughness | R = roughness, G = metallic, B = AO |
| Occlusion-Roughness-Metallic (ORM) | R = AO, G = roughness, B = metallic (Unreal's layout, and what the Substance exporter writes) |

### UDIM tile sets

Materials can use UDIM tiles as exported by Substance Painter or Mari: put `<UDIM>` in the
texture path, such as `Textures/Walls_BaseColor_<UDIM>.dds`. The mesh keeps its UVs across tiles
(U from 0 to 10, V upwards), and each tile's triangles are drawn with that tile's texture
(1001 for the first, then 1002 ... 1010 along U, 1011 starting the next row). Import each tile
image like any other texture. UDIM tiles are tracked by the loading screen.

## Unreal Engine 5 assets

Ptero imports UE5 editor assets (`.uasset` files, as sold on Fab or saved in your own projects)
directly, without running Unreal:

| Unreal asset | Becomes |
| --- | --- |
| **Static Mesh** | `.ptero` geometry, a multi-material saved as `<mesh>.json` beside it (picked up as the mesh's default material), and collision hulls |
| **Texture2D** | A BC-compressed `.dds` |
| **Material / Material Instance** | A Ptero material, with textures wired to the matching slots |

Other asset types (levels/Worlds, skeletal meshes, Blueprints, Niagara) are not imported.

### What you need

Unreal compresses its editor assets with **Oodle**, which Ptero does not ship. It borrows
`oo2core.dll` from **your own Unreal Engine 5 installation** (found through the Epic Games
Launcher's records), so Unreal must be installed on the same PC; it does not need to be running.
Without it, importing reports that no Oodle decompressor was found. (Advanced: the environment
variable `PTERO_OODLE_DLL` can point at a specific `oo2core.dll`.)

### How to import

- **Import...** in the Asset Browser and pick `.uasset` files; or
- copy a Fab pack's `Content` folder somewhere under `Data/` and accept the **New files detected**
  notification. Assets inside `Data/` are converted **in place**, next to themselves. References
  between assets (a mesh to its materials, a material to its textures) are followed and imported
  too, even when they live in other folders of the pack.

Several Unreal imports run in parallel; textures are encoded on background threads.

### What comes across

- **Geometry**: Unreal's centimetres and axes are converted to Ptero's metres and Z-up; normal
  maps need no green flip.
- **Materials**: the importer reads the material graph to see which texture feeds which input,
  and falls back to parameter and texture names when it cannot. Constant colour parameters become
  tints; 1×1 placeholder textures become plain values.
- **Packed maps**: separate roughness, metallic and AO maps are used as they are; packed ones
  (ORM, ARM, MRA, RMA or any non-red channel) are repacked into Ptero's layout.
- **Opacity** from the base colour's alpha works as it does in Ptero.
- **Height maps** are imported, but parallax stays **off**: Unreal content often uses them for
  Nanite displacement at a scale that means nothing to Ptero's parallax. Turn it on and tune
  *Height Scale* in the Material Editor if you want it.
- **Material logic** (blends, math nodes, world-aligned textures, vertex painting) does not come
  across: a Ptero material is a set of textures and values, not a graph. Complex master materials
  arrive as their main texture set.

## Substance 3D Painter

Ptero ships a **viewport shader** and an **export plugin** for Adobe Substance 3D Painter, in
`Tools/SubstancePainter`. The shader shows your material in Painter exactly as Ptero's
[Disney BRDF](05a-Disney-Principled-BRDF.md) will shade it, including its extra
lobes; the plugin exports the textures packed the way the engine reads them, together with a
ready-made `.material` file.

### Installing

| File | Copy to |
| --- | --- |
| `Tools/SubstancePainter/shaders/ptero-principled.glsl` | `Documents\Adobe\Adobe Substance 3D Painter\assets\shaders` |
| `Tools/SubstancePainter/python/plugins/ptero_export.py` | `Documents\Adobe\Adobe Substance 3D Painter\python\plugins` |

Restart Painter (or use **Python → Reload Plugins Folder**), then enable **Python →
ptero_export**. A **Ptero Engine** menu appears in Painter's menu bar.

### Painting

1. In **Shader Settings**, switch the Texture Set's shader to **ptero-principled**.
2. Paint the usual channels: Base Color, Roughness, Metallic, Normal, Height, AO, plus Emissive and
   Opacity if you need them.
3. Set the **Ptero Material** sliders in Shader Settings. They are the material's values in Ptero:
   *Specular*, *Specular tint*, *Subsurface flatten*, *Anisotropic*, *Sheen*, *Sheen tint*,
   *Clearcoat*, *Clearcoat gloss*, *AO strength*.
4. To vary the extra lobes across the surface, use **Ptero Engine → Add Ptero channels to active
   Texture Set**. It adds:

   | Channel | Paints |
   | --- | --- |
   | User0 "Clearcoat" | Where the clearcoat is (scales the Clearcoat slider) |
   | User1 "Sheen" | Where the sheen is |
   | Anisotropy level | Where the highlight stretches |
   | User2 "Flatten" | Where the subsurface flatten applies |
   | User3 "Aniso direction" | The direction the highlight stretches (RG, 0.5 = none) |

   Add only the ones you paint: a channel that does not exist exports as white, so the slider
   applies to the whole surface. A channel that exists but is unpainted exports as black (none).

The viewport preview integrates every lobe against Painter's environment light, so it shows the
engine's BRDF itself rather than Painter's own approximation. *Direct-light masking remap* under
*Ptero Preview* matches how Ptero treats its punctual lights.

### Exporting

1. **Ptero Engine → Export to Ptero Engine...**
2. Pick a folder **under the engine's `Data/` folder** (for example `Data/Props/Barrel`). Outside
   `Data/`, the `.material` can only refer to textures by bare file name.
3. For every Texture Set the plugin writes:

   | File | Contents |
   | --- | --- |
   | `<Set>_BaseColor.png` | Base colour |
   | `<Set>_Normal.png` | Normal map with the height channel folded in, DirectX convention |
   | `<Set>_ORM.png` | R = AO, G = roughness, B = metallic |
   | `<Set>_Emissive.png`, `<Set>_Opacity.png` | When those channels exist |
   | `<Set>_Height.png` | 16-bit, when the height channel exists |
   | `<Set>_PrincipledMask.png` | R clearcoat, G sheen, B anisotropy, A flatten, when any of those channels exist |
   | `<Set>_AnisoDirection.png` | When the direction channel exists |
   | `<Set>.material` | A Ptero material referring to all of the above as `.dds`, with the shader's slider values, emissive intensity and alpha-test settings |

4. Switch to the Ptero editor. The **New files detected** notification offers to import the PNGs;
   accept it, and they are converted to `.dds` beside themselves, which is what the `.material`
   refers to.
5. Assign `<Set>.material` to your mesh (or a sub-material of its multi-material).

If the Texture Set used a different shader, the `.material` gets default slider values (the log
says so). Layered Texture Sets are skipped. With a height channel, the material's *Height
Reference* and *Displacement Mid Level* are set to 0.5, because Painter's exported height is
centred there; turn on parallax or tessellation in the Material Editor to use it.

## Audio (FMOD)

Ptero plays sound through **FMOD Studio** (version 2.03). Sounds are not imported one by one;
they are authored as **events** in an FMOD Studio project and built into **banks**.

### Workflow

1. Open `Data/fmod_project/Ptero-Engine/Ptero-Engine.fspro` in **FMOD Studio 2.03**. Match the
   version: banks built by a newer FMOD Studio may not load in the engine.
2. Import your audio files into FMOD Studio and create **events** (a footstep, a door, a music
   track, an ambience loop). Assign each event to a **bank** (the project's `Master` bank is fine
   for small games).
3. **File → Build**. FMOD writes the banks into the project's `Build/Desktop` folder.
4. Copy the `.bank` files, including **`Master.strings.bank`**, into **`Data/Audio`**. (Or set FMOD
   Studio's built-banks output directory to `Data/Audio` so builds land there directly.)
5. Restart the editor. The engine loads every `.bank` in `Data/Audio` when it starts, the strings
   bank first.
6. **Audio → Audio Manager...** shows the loaded banks, any that failed and every event; click an
   event to audition it.

The **strings bank** is what lets the engine find events by name. Without it, events cannot be
referred to as `event:/Ambient/Wind` or by bare name.

### Using events

- **Audio Emitter** component: plays an event from the entity's position, optionally on level
  start.
- **Node Graph**: *Play Sound* (one-shot), *Play Music* (one music channel), *Play Music
  Playlist* (shuffled tracks), *Play / Stop Entity Audio*.
- Events can be named in full (`event:/Ambient/DiceRoll`) or by bare name (`DiceRoll`); the
  engine finds the full path.

### 3D sound

Positioning, distance attenuation and spatialisation are set up **in FMOD Studio**, on each event.
Spatialised events use Google's **Resonance Audio** plugin (bundled with FMOD): give the event a
Resonance Audio **Source** and the master a matching **Listener**. Events without them play as
plain 2D sound.

One FMOD quirk to know: a Resonance Source takes the event's signal, so the event's own master
fader cannot turn a spatialised event down. Set its level before the Source (on the audio track)
instead.

The player's volume settings (overall, music, sound) scale everything at runtime.

## Video

The engine plays **WebM** video: VP8 or VP9 picture with an Opus soundtrack.

| You import | Result |
| --- | --- |
| A `.webm` that is already VP8/VP9, 8-bit | Copied untouched |
| Anything else (MP4, MOV, MKV, AVI, GIF, 10-bit WebM...) | Converted with ffmpeg: VP9 at constant quality, 8-bit, a key frame at least every 120 frames (so seeking stays quick), first audio track as Opus 128 kb/s |

Conversion needs `ffmpeg.exe` and `ffprobe.exe` in `Binaries/ffmpeg/bin`. Without them, only
ready-made WebM files import. The progress window shows how far a conversion has got; a cancelled
or failed conversion never leaves a half-written file.

To prepare video yourself, export VP9 WebM, 8-bit 4:2:0, with Opus audio. Double-click a `.webm` to
preview it; see [11. Gameplay](11-Gameplay.md#video) for playing it in a game.

## Asset budgets

### Engine limits

These are hard limits in the engine:

| Thing | Limit |
| --- | --- |
| Lights shading surfaces at once | 256 |
| Lights lighting volumetric fog and voxel GI | The 16 most important at any moment |
| Lights casting shadows at once | 8 by default (the shadow budget; adjustable) |
| Shadowed lights in the virtual shadow map | 16 |
| Particle systems per level | 16, up to 131 072 particles each |
| Terrain paint layers | 4 per terrain |
| Vegetation | 8 layers per area, 500 000 instances per area |
| LODs per imported mesh | LOD 0 plus up to 3 |

### Recommendations

Guidance for a game that runs well on a mid-range ray-tracing GPU at 1080p–1440p. Measure your
own levels with **Resource Debug** rather than treating these as rules.

**Textures**

| Asset | Suggested size |
| --- | --- |
| Hero props, characters seen up close | 2K (4K only if they fill the screen) |
| Ordinary props | 1K–2K |
| Small props (cups, books, debris) | 512–1K |
| Tiling surfaces (floors, walls, terrain layers) | 2K, tiled |
| Decals | 512–1K |
| Particle sprites and flipbooks | 512–2K for the whole sheet |

A material with base colour, normal and ORM at 2K costs about 17 MB of video memory; at 4K about
67 MB. Share materials between meshes wherever you can, and keep 4K for what deserves it.

**Triangles**

| Asset | Suggested triangles (LOD 0) |
| --- | --- |
| Small props | 500–5 000 |
| Furniture, medium props | 5 000–30 000 |
| Hero props, buildings | 30 000–150 000 |
| Scans, sculpts, very dense assets | Any, with **Virtualized Geometry** on |

With virtualized geometry, triangle count matters much less than how many *different* meshes
are on screen, because each mesh costs about as many triangles as it covers pixels.

**Lights**

- Keep each light's **Radius** as small as looks right: radius is cost.
- Turn **Cast Shadows** off on fill lights and small decorative lights.
- Tens of lights in view is fine; hundreds work but leave fog and voxel GI using only the 16
  most important.

**Materials and draws**

- Fewer, shared materials beat many unique ones.
- Combine small props that never move separately into one mesh in your 3D tool.
- Alpha-cutout foliage is expensive in ray tracing: leave *Contribute to Ray Tracing* off on
  grass and small plants.

## File types you will see

| Extension | What it is |
| --- | --- |
| `.level` (older: `.json` in `Levels/`) | A level |
| `.fbx`, `.fbx.ptero` | A model and its cooked engine version |
| `.collisions.json` | Generated collision hulls for a model |
| `.dds` | An imported texture |
| `.material` / `.json` in `Materials/` | A material |
| `.json` in `MultiMaterials/` or beside an Unreal mesh | A multi-material (one sub-material per mesh slot) |
| `.particle` | A particle effect |
| `.nodegraph` | An exported Node Graph |
| `.sequence` | A cinematic sequence |
| `.rml`, `.rcss` | Game UI document and stylesheet |
| `.webm` | Video |
| `.bank` | FMOD sound bank |
| `.raw`, `.sculpt.raw` | Terrain heightmap and its sculpted version |
| `.style` | An editor colour style |
| `.ppak` | Packaged game data (in a built game) |
