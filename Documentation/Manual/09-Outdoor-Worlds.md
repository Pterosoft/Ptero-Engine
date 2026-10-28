# 9. Outdoor worlds

Terrain, water and vegetation build landscapes. Wind, rain, fog and clouds give them weather.

## Terrain

A terrain is a heightmap-driven landscape patch at the entity's position. It can be sculpted,
painted with up to four material layers, tessellated and displaced, and it collides with the
player and physics.

### Creating a terrain

1. **Components → Terrain**, then click in the viewport. A dialog asks for a heightmap.
2. Choose a **16-bit `.raw`** heightmap (the common Unity / Unreal format: one 16-bit
   little-endian value per sample, top row first; you enter its width and height) or an
   image (PNG, TIF, EXR, HDR, ...).

Use **16-bit** sources. An 8-bit image only has 256 height steps, which shows as terraces and
blotchy lighting on slopes.

The engine writes an uncompressed `.dds` of the heightmap beside the source for the GPU. The
source file itself is never changed.

### Terrain properties

| Property | Meaning |
| --- | --- |
| Select Heightmap... | Replace the heightmap. |
| World Size (1024 m) | Width of the (square) patch. |
| Height Scale (256 m) | Height of white in the heightmap. |
| Height Offset (0 m) | Raise or lower the whole patch. |
| Terrain Material, Material Tile Size (4 m) | A material for the whole surface (used when there are no paint layers), and how many metres one texture repeat covers. |
| Break Up Tiling (on) | Hides texture repetition by blending randomly offset copies of every map. Costs a second texture read per map. |
| Height-Based Blending (on), Sharpness (0.6) | Where layers meet, the higher surface wins: sand settles between rocks instead of cross-fading with them. Uses the layers' height maps. Sharpness 0 is soft, 1 is crisp. |
| Revert Sculpting | Throw away all sculpting and return to the original heightmap. |
| Show Tool Window | Opens the Terrain Tool. |

### Sculpting and painting: the Terrain Tool

**Windows → Terrain Tool** edits the selected terrain (or the first one in the level).

1. Pick a brush:

   | Brush | Does |
   | --- | --- |
   | Raise / Lower | Push the ground up or down. Strength is metres per second. |
   | Flatten | Pull the ground towards a set height (Flatten Height). |
   | Smooth | Average neighbouring heights (Smoothing Passes per tick). |
   | Paint | Paint the active material layer. Does not change height. |

2. Set **Radius** (metres) and **Strength**.
3. Press **Enter Brush Mode**. Left-click now belongs to the brush: hold the left button over
   the terrain to sculpt or paint. **Exit Brush Mode** to select things again.

Sculpting is saved to a `<heightmap>.sculpt.raw` file next to the source at the end of every
stroke. That file takes precedence over the original heightmap, which stays untouched;
**Revert Sculpting** simply stops using it.

### Paint layers

Up to **four** layers, blended by a painted weight map:

| Layer setting | Meaning |
| --- | --- |
| Material... | A full material: base colour, normal, roughness, metallic, AO and height all blend between layers. |
| Texture... | Or just a colour texture, for a simple layer. |
| Tile Size | Metres per texture repeat for this layer. |
| Tint | Multiplies the colour (or is the colour, with no texture). |
| Fill | Paint the whole terrain with this layer. |
| Remove | Delete the layer. |

Weights are saved to a splat map beside the heightmap the first time you paint.

### Vegetation follows paint

A vegetation layer can be told to only grow where a terrain layer is painted (its *Terrain Layer
Mask*), so painting a grass layer also grows grass there.

## Water

A flat, animated water surface in the entity's XY plane at its height: an ocean, lake or pond.

| Property | Meaning |
| --- | --- |
| Size X, Size Y (200 m) | Area, centred on the entity. |
| Resolution (384) | Grid vertices per side. Higher = smoother waves, more triangles. |
| Material (Ocean) | The look. **Ocean** and **Water** buttons pick the two built-in presets. |
| Wave Scale (1) | Calmer or rougher than the material, per placement. |
| Debug View | Normals, Fresnel, depth, refraction, reflection, specular, foam. |

How it works: waves are **Gerstner** and sine waves plus a scrolling height map, displaced on the
GPU. The water is drawn after the scene is lit, so it can **refract** what is beneath it and
**reflect** what is on screen. Its colour comes from **depth**: shallow water shows the
bottom and the shore colour, deep water turns to the depth colour, with foam where the water is
shallow and on wave crests.

The look lives in the material's `water` block (surface, depth and shore colour, clarity and
transparency, wave amplitude, steepness and tiling, normal/height/foam textures, refraction and
specular). Copy `Materials/Ocean.json` to start your own.

## Vegetation

Vegetation is not painted tree by tree. You place a **Vegetation Area**, a volume that fills
itself with instances by **rules**. The level stores the rules, not a list of thousands of
trees, and the same rules always produce the same result.

### The area

| Property | Meaning |
| --- | --- |
| Shape | **Box**, **Sphere**, or **Polygon** (a footprint you draw, extruded up and down). |
| Extents | Half-sizes in metres (radius for a sphere; vertical half-height for a polygon). |
| Seed (Reroll) | Reshuffles the entire scatter without changing any rule. The fastest way out of an arrangement that looks wrong. |
| Snap to terrain / Snap to geometry | What instances may land on. With both, the highest surface under each point wins. |
| Snap Max Distance (500 m) | How far down from the top of the volume to search for ground. |
| **Exclusion volume** | This area *removes* vegetation instead of adding it. Use it to carve roads, clearings and building footprints out of other areas. |
| Show bounds | Draw the volume in the viewport. |
| Regenerate | Rebuild the scatter now. |

### Layers

An area holds up to 8 layers (canopy trees, undergrowth, grass, rocks), so one volume can describe
a whole biome.

| Layer setting | Meaning |
| --- | --- |
| Load Mesh..., Load Material... | The asset. Foliage materials should use **Alpha Cutout** so leaf cards clip. |
| Density (0.5 per m²) | Instances per square metre. Resizing the area keeps the same density. |
| Min / Max Scale (0.8 / 1.2) | Random size range. |
| Random Yaw (on), Max Tilt (0°) | Random rotation; a few degrees of tilt breaks up the "planted in a grid" look. |
| Align to Normal (0) | 0 = upright (trees), about 0.3 for grass, 1 = follow the slope (rocks, debris). |
| Max Slope (40°) | No instances on steeper ground. Keeps trees off cliffs. |
| Min / Max Altitude | A height band: tree lines, shorelines. |
| Sink Offset (0.02 m) | Push instances slightly into the ground so they never float. |
| Terrain Layer Mask, Threshold (0.5) | Only grow where that terrain paint layer is stronger than the threshold. |
| Collision Radius (0) | Self-thinning: no two instances of the layer closer than this. |
| Cull Distance (150 m), Fade Fraction (0.15) | Stop drawing beyond the distance, fading out over the last part instead of popping. |
| Billboard Texture, Billboard Scale | A flat card drawn instead of the mesh far away. Leave empty for grass (a card of a grass tuft costs as much as the tuft). |
| Cast Shadows (on) | |
| Contribute to Ray Tracing (off) | Put this layer in ray tracing for GI and reflections. Alpha-tested foliage is expensive to trace: only for hero trees. |

### Wind and interaction

| Setting | Meaning |
| --- | --- |
| Bend Model | **Tree**: trunk sway, branch sway and leaf flutter, but ignores the player. **Grass**: a cheap single-hinge bend that also reacts to the player walking through it (trampling, trails). **None**: rigid. |
| Wind Influence (1) | How much the scene wind moves this layer. 0 = rigid. |
| Stiffness (1) | Resistance: a mature trunk moves less than a sapling in the same wind. |
| Flutter Amount (1) | High-frequency leaf flutter. |
| Interaction Influence (1) | How strongly grass reacts to the player. |

The level format can also store per-instance edits (remove this one tree, move that one) on top
of the rules; they survive later density changes. Limits: 500 000 instances per area.

## Wind

Set in **Time of Day → Wind** (see [6. Lighting](06-Lighting.md#wind)). Vegetation, rain,
particles and clouds all read the same wind, and gusts roll across the world as travelling
noise, so a gust visibly crosses a field instead of every blade moving at once.

## Rain

See the [Rain component](04-Components.md#rain). Rain falls in a box that follows the camera,
blown by the scene wind, and wets surfaces.

## Volumetric fog

Real fog in the air between surfaces: light shafts from the sun through windows and trees,
glowing halos around lamps, haze that thickens with distance. Settings are in **Graphics Settings
→ Volumetric Fog** and saved with the level.

How it works: the view is divided into a 3D grid of cells ("froxels") following the camera's
frustum. Each cell is lit by the sun (with shadows), every light marked *Affect Volumetric Fog*
(with shadows), the sky and the GI, and the result is accumulated from the camera outwards.

| Setting | Meaning |
| --- | --- |
| Enabled (on) | |
| Density (0.02) | How much light the air removes per metre. A ray of length *d* keeps e^(-Density × d) of its light. 0.02 is light haze; 0.1 is thick fog. |
| Anisotropy (0.5) | How much light scatters forward. Higher makes shafts towards the sun much brighter than away from it. |
| Scattering Albedo (0.9) | Share of removed light that is scattered rather than absorbed. Below 1 leaves unlit fog grey instead of black. |
| Start / Max Distance (0.1 / 100 m) | The fog volume's range from the camera. |
| Base Height, Height Falloff (0 = off) | Fog that thins with altitude: valley mist, ground fog. |
| Colour | Scattering colour. |
| Emissive Colour, Intensity | Light the fog gives off by itself (glowing mist, magic). |
| GI Intensity (1) | Bounce light in the fog. In an interior with no sky, this is the only light the fog gets. |
| GI Volume (on), Cell Size (0.5 m) | Gather bounce light into a 3D volume around the camera, so the air is lit by the surfaces around it and light behind the camera is remembered. |
| Froxel Tile Size (8), Depth Slices (64) | Grid resolution: screen pixels per cell, and slices in depth. Smaller tiles and more slices are sharper and dearer. |

Lights have their own *Fog Intensity* multiplier, so a lamp can throw a strong beam through haze
without lighting the walls any more.

## Volumetric clouds

Ray-marched 3D clouds in a layer curving around the planet. They are lit by the sun and sky,
drift with their own wind, and change with the time of day. Settings in **Graphics
Settings → Volumetric Clouds**; they vanish with the sky when Time of Day is off.

**Presets**: Clear Skies, Scattered Cumulus, Overcast, Stormy. A preset only changes the weather
values, so your lighting and performance tuning survives switching.

| Group | Settings |
| --- | --- |
| Layer | Bottom (1200 m) and Thickness (4000 m), planet radius. |
| Shape | **Coverage** (0.55; 0 clears the sky, 1 fills it), **Cloud Type** (0 flat stratus, 0.5 cumulus, 1 towering cumulonimbus), Density, noise scales and detail strength, curl, anvil bias, weather map scale, seed and biases. |
| Wind | Direction, Speed (12 m/s), Skew (shear across the layer), detail speed. |
| Lighting | Scattering colour, extinction, forward and back scattering lobes, "powder" darkening of cloud edges facing the sun, multiple-scattering octaves, sun, ambient and ground-bounce strength. |
| Ray marching | Steps, light steps, maximum trace distance, distance fade, resolution divisor (2 = half resolution), temporal upsampling and blend. |
| Debug | Luminance, transmittance, coverage, distance. |

Clouds also dim the sun's lens flare when they pass in front of it.
