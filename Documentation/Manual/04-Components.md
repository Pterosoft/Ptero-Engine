# 4. Components

Every entity has a name, a transform and an id. Everything else comes from components, edited
in the **Properties** panel. This page lists every component and every property, with its
default in parentheses.

- [Transform](#transform)
- [Mesh](#mesh)
- [Light (point, spot, rect)](#light)
- [Camera](#camera)
- [Audio Emitter](#audio-emitter)
- [Decal](#decal)
- [Rigid Body (physics)](#rigid-body)
- [Particle System](#particle-system)
- [Rain](#rain)
- [Terrain, Water, Vegetation Area](#terrain-water-and-vegetation-area)
- [Special entity names](#special-entity-names)

## Transform

| Property | Meaning |
| --- | --- |
| Position | Metres. Z is up. |
| Rotation | Degrees about X, then Y, then Z. Z is the heading. |
| Scale | Per axis. 1 is the model's real size (models are imported in metres). |

## Mesh

Draws a model. Placed with **Components → Geometry**.

| Property | Meaning |
| --- | --- |
| **Select Geometry (.ptero)** | The imported model. `.ptero` is the engine's cooked mesh, made when an `.fbx` is imported. |
| **Select Material** | A material or multi-material file. **Use Geometry Default Material** goes back to the material created at import. |
| LOD Usage Scale (1) | How eagerly the mesh switches to its simpler LODs. Above 1 switches sooner (cheaper), below 1 keeps detail longer. |
| LOD Debug View | Force one LOD, to check what each looks like. |
| **Virtualized Geometry** (off) | Draw this mesh through the virtualized-geometry system instead of LODs (below). |

A mesh entity **always collides** with the player and with physics objects during play, as
static level geometry, even without a Rigid Body.

### LODs

When a model is imported, the engine generates simpler versions of it (levels of detail). The
renderer picks one per frame from how large the mesh is on screen. **Asset Browser → Generate
LODs** regenerates them for a model.

### Virtualized geometry

For very dense meshes (photo scans, sculpts, kitbash pieces), switch on **Virtualized
Geometry**. The engine splits the mesh into small clusters of triangles and builds a hierarchy
of simplified versions, the way Unreal's Nanite does. Every frame, the GPU picks, cluster by
cluster, the coarsest version whose error stays under one pixel. The result:

- a mesh costs roughly as many triangles as it covers pixels, at any distance;
- there are no LOD switches to pop between;
- clusters hidden behind other geometry are skipped (occlusion culling).

The first time a mesh is virtualized it is built on a background thread and cached in
`Cache/`, so the first load of a huge mesh takes a while and later loads are fast.

Scene-wide switches are in the **View → Virtual Geometry** menu and the Mesh section:

| Switch | Meaning |
| --- | --- |
| VG Enabled | Off draws every virtualized mesh with ordinary LODs, for an A/B comparison. |
| Virtualize All Meshes | Try the system on the whole level without changing any mesh. Not saved. |
| Error threshold (1 pixel) | Lower is sharper and costs more triangles. 1 is visually lossless. |
| Occlusion Culling | Skip clusters hidden behind others. Not available with MSAA. |
| Backface Cluster Culling | Skip clusters that face away entirely. |
| Mesh Shaders | Use mesh shaders where the GPU has them; off forces the slower path every GPU can run. |
| Shadows | Virtualized meshes also cast shadows cluster by cluster. |
| Debug View | Colour by cluster, instance or hierarchy level. |
| Freeze Culling | Keep the selection from the current camera while you fly away and inspect it. |

Ray-traced GI also traces against a simplified version of virtualized meshes, so they are cheap
to ray trace too.

## Light

One component covers three light shapes. Placed with **Components → Point Light / Spot Light /
Rect Light**. Spot and rect lights shine along the entity's local -Z: an unrotated one points
straight down, like a ceiling lamp.

| Property | Meaning |
| --- | --- |
| Type | Point (all directions), Spot (a cone) or Rect (a rectangular area light: a window, a panel, a softbox). |
| Intensity (800 lm) | Total light output in **lumens**. 800 lm is a 60 W bulb, 1600 lm a 100 W one. |
| Radius (5 m) | How far the light reaches. Nothing beyond it is lit. Keep it as small as looks right: it decides the light's cost. |
| Colour, or Use Temperature (6500 K) | RGB colour, or a blackbody colour temperature in Kelvin (2700 K warm bulb, 6500 K daylight). |
| Source Radius (0) | The physical size of the bulb. Above 0 gives soft shadows that widen with distance. |
| Falloff Exponent (2) | 2 is physically correct inverse-square falloff. Lower gives a softer, more stylised falloff. |
| Cast Shadows (on) | Whether the light casts shadows. See [8. Shadows](08-Shadows.md) for the budget. Ray-traced shadows also follow this switch. |
| Affect Volumetric Fog (on), Fog Intensity (1) | Whether the light scatters in fog, and how strongly. Only the fog's copy is scaled, so you can have a strong beam through haze without brightening the walls. |
| Affect Global Illumination (on), GI Contribution (1) | Whether the light's bounce light is computed, and a multiplier on it. Turn it off for cinematic key and rim lights that should not tint the room. |
| **Cone** (spot): Inner (25°), Outer (40°) | Full cone angles. Full brightness inside the inner cone, fading to nothing at the outer one. Equal values give a hard edge. |
| **Rectangle** (rect): Width, Height (1 m), Two Sided (off) | The emitting area in metres. Two Sided lights both faces, for a floating strip light. |
| **Light Style** | An animated brightness pattern (below). |

### Light styles

A light style animates the brightness. It reaches everything the light affects (direct
lighting, the GI bounce and the fog), so the whole room flickers together.

| Style | Character |
| --- | --- |
| Fire | Never-repeating noise flicker with a slow "breathing" under it. For open flames. |
| Torch | Like Fire, faster and shallower. |
| Flicker, Flicker (alt) | Classic stepped flicker. |
| Candle, Candle (alt), Candle (slow) | Classic candle patterns with dips. |
| Slow Strong Pulse, Gentle Pulse, Slow Pulse (no black) | Smooth pulsing. |
| Fast Strobe, Slow Strobe | On/off strobes. |
| Fluorescent Flicker | A failing tube. |
| Custom | Type a pattern of letters: `a` is black, `m` is the light's set brightness, `z` is about twice as bright. Example: `mmnmmommommnonmmonqnmmo`. |

| Style setting | Meaning |
| --- | --- |
| Speed | Pattern steps per second (or noise rate for Fire and Torch). |
| Amount | 0 = no effect, 1 = full style, up to 2 exaggerates it. The average brightness stays put. |
| Phase Offset | Seconds into the pattern. Give two torches different phases so they don't flicker in lockstep. |

The styles are deterministic: the same level flickers identically every time, so recorded
cinematics reproduce exactly.

## Camera

A camera placed in the level. Cinematics cut between cameras, and the viewport can look
through one or fly it. A camera looks along its local +Y.

| Property | Meaning |
| --- | --- |
| Focal Length (35 mm) | Like a real lens. Longer is more zoomed in. The field of view is derived from it and can be edited directly too. |
| Sensor Width × Height (36 × 24 mm) | Full-frame 35 mm by default. Together with the focal length this gives the field of view, so a shot matched to a real lens frames the same. |
| Near Clip (0.1 m) | Closest distance drawn. |
| Shake Amplitude (0°), Shake Frequency (1.5 Hz) | Procedural handheld shake. Driven by sequence time, so a rendered shot shakes the same every time. |

Camera roll is stored but not shown yet, and there is no depth of field yet.

## Audio Emitter

Plays an FMOD Studio event from the entity's position.

| Property | Meaning |
| --- | --- |
| Select Event | Any event in the banks in `Data/Audio`. |
| Auto Play (on) | Start playing when the level starts. Off leaves it for a script (Play Entity Audio). |

3D positioning, attenuation and spatialisation come from how the event is set up in FMOD
Studio. See [11. Gameplay](11-Gameplay.md#audio).

## Decal

Projects a material onto whatever surfaces are inside a box: dirt, stains, signs, cracks,
puddles. The box projects along the entity's local -Z.

| Property | Meaning |
| --- | --- |
| Material | Must be a material with **Is Decal Material** ticked. |
| Size X, Y, Z (1 m) | Half-sizes of the projection box. Z is the depth it projects through. |

The viewport draws the box and an arrow showing the projection direction.

## Rigid Body

Added from the **Physics** section of Properties (**Add Rigid Body**). Puts the entity into the
physics simulation during play. You only need it for things that **move** or for static
objects that need a **different collision shape** than their mesh; plain meshes already
collide as static geometry.

| Property | Meaning |
| --- | --- |
| Motion | **Static**: never moves. **Dynamic** (default): falls, bounces, gets pushed. **Kinematic**: moved by scripts; pushes dynamic bodies, never pushed itself (doors, lifts, platforms). |
| Shape | **Auto**: the mesh's generated collision hulls if it has them (Asset Browser → Generate Collisions), otherwise its convex hull (dynamic) or exact triangles (static/kinematic). **Box**, **Sphere**, **Capsule**: fitted to the mesh bounds. **Convex Hull**. **Triangle Mesh**: exact triangles; cannot be dynamic. |
| Mass (10 kg) | 0 works out the mass from the shape's volume at the density of water. |
| Friction (0.5), Restitution (0.1) | Grip, and bounciness from 0 to 1. |
| Linear / Angular Damping (0.05) | How quickly movement and spin die away. |
| Gravity Scale (1) | 0 floats, 2 falls twice as fast. |
| Continuous Collision (off) | For small, fast objects that would otherwise pass through walls. |
| Start Asleep (off) | Don't simulate until something touches it. Good for stacks. |

Physics only runs during play. When play stops, everything returns to where it was placed.

## Particle System

GPU particles for fire, smoke, sparks, embers, dust and steam. A particle effect is authored
once in **Windows → Particle Editor** and saved as a `.particle` file in `Data/Particles`. Each
Particle System entity just names an effect (plus its own *Enabled* switch), so one fire can be
placed fifty times and changed in one place. Edits in the Particle Editor show live on every
emitter using that effect.

The **look** (sprite texture, blend mode, glow colour) comes from a **particle material** (see
[5. Materials](05-Materials.md#particle-materials)), so several effects can share one flame
material.

**Emission**

| Setting | Meaning |
| --- | --- |
| Spawn Rate (120/s) | Particles born per second. |
| Lifetime (1.6 s), Lifetime Variance (0.35) | How long each lives, ± a random fraction. |
| Max Particles (4096) | Hard cap for this emitter. |
| Prewarm (on) | Start already full, instead of building up from empty when the level loads. |
| Burst, Burst Count, Burst Interval | Emit a batch at intervals instead of continuously. |

**Shape**: where particles are born, in the emitter's local space (the emitter faces +Z, so an
unrotated cone sprays upward).

| Shape | Notes |
| --- | --- |
| Point, Sphere, Box | Box uses Shape Extents. |
| Cone | A disc of Shape Radius spraying into Cone Angle. The default: a fire with no setup. |
| Disc, Edge | A ring or disc; a line along local X. |
| Shell Bias (0) | 0 fills the shape, 1 spawns only on its surface (a ring of flame around a log). |

**Motion**

| Setting | Meaning |
| --- | --- |
| Initial Speed, Speed Variance | Launch speed in m/s. |
| Acceleration (0, 0, 2.4) | Constant push in m/s². Fire rises (+Z); debris falls (-9.8). |
| Drag (0.6) | How quickly particles slow and settle into the flow. |
| Wind Influence (0.35) | How much the scene wind pushes them. |
| Turbulence Strength, Frequency, Speed | Swirling noise. This is what makes a cone of sprites lick and curl like flame. |
| Vortex Strength (0) | Spin around the emitter's axis as they rise. |

**Size and Rotation**: Start Size, End Size (metres, blended over life), Size Variance; start
rotation, random start rotation and spin speed.

**Colour Over Life**: three colours (start, middle, end) with alpha as the opacity curve, and
where the middle sits. A flame is white-hot core, orange body, dark red fade-out. *Emissive
Intensity (4)* is the main brightness control for glowing effects.

**Flipbook**: Columns × Rows treats the sprite texture as an animation sheet. FPS 0 plays the
whole sheet once over each particle's life. Blend Frames cross-fades between frames; Random
Start Frame stops a cluster animating in unison.

**Rendering**

| Setting | Meaning |
| --- | --- |
| Facing | **Billboard** (faces the camera: fire, smoke), **Velocity Stretched** (sparks, embers, rain), **Horizontal** (ground mist, ripples), **Vertical** (upright, turns to face the camera). |
| Stretch Factor | Stretch per m/s, for Velocity Stretched. |
| Soft Particles (on), Fade Distance (0.35 m) | Fade sprites where they meet geometry, instead of a hard intersection line. |
| Cull Distance (120 m) | Stop drawing beyond this. |

**Light and Global Illumination**: particles are not ray traced, so instead an emitter can
create an invisible light inside itself that lights the room, feeds the GI and glows in fog.

| Setting | Meaning |
| --- | --- |
| Emit Light (on) | Create the stand-in light. |
| Intensity (1400 lm), Radius (7 m), Height Offset (0.45 m) | The light, and how far above the emitter it sits (inside the flame, not at its base). |
| Colour From Particles (on) | Take the light colour from the particle colours, so recolouring the fire recolours its light. |
| GI Contribution, Cast Shadows (off), Affect Volumetric Fog (on) | As for lights. Shadows are worth it for a hero fire, wasteful for a row of candles. |
| Flicker: style, speed, amplitude, phase (Fire) | A light style for the stand-in light. **Flicker The Sprites Too** makes the flame itself brighten and dim with the light it casts. |

Limits: 16 particle systems per level, up to 131 072 particles each.

## Rain

Rain streaks simulated in a box that follows the camera, placed at the entity.

| Setting | Meaning |
| --- | --- |
| Enabled | |
| Gravity (9.8) | Fall acceleration. |
| Wind | Kept for older levels and ignored: rain is blown by the scene wind (**Time of Day → Wind**), so rain and trees always agree. |
| Bounding Box | Half-sizes of the box around the camera where rain exists. |
| Intensity, Streak Length, Colour (with alpha) | The look of the streaks. |
| Wetness Intensity (0.7) | How wet surfaces look. |

## Terrain, Water and Vegetation Area

These three build outdoor worlds and have their own page:
[9. Outdoor worlds](09-Outdoor-Worlds.md).

## Special entity names

| Name | Effect |
| --- | --- |
| `PlayerStart` | The player spawns here when you press Play: feet at its position, facing its local +Y. Without one, the player spawns at the editor camera. |
