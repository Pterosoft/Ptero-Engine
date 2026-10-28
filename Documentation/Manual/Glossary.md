# Glossary

**AgX.** The tonemapper Ptero uses: the curve that turns the renderer's HDR light values into a
displayable image, with film-like highlight roll-off. See [10](10-Image-Quality.md#tonemapping-and-exposure).

**Albedo / base colour.** A surface's colour with no lighting on it.

**Alpha cutout.** Making parts of a surface fully invisible using a texture's alpha, for leaves
and fences. Cheaper than transparency and casts correct shadows.

**Ambient occlusion (AO).** Darkening in creases and corners where surrounding geometry blocks
ambient light.

**Anti-aliasing (AA).** Removing jagged, flickering edges. Ptero's default is TAA.

**Asset.** Any file a level uses: a model, texture, material, sound, video, UI document.

**Asset Browser.** The editor window for importing and managing files under `Data/`.

**Bank.** An FMOD file (`.bank`) containing sound events, built from FMOD Studio.

**Binding.** In the Sequencer, an entity that a sequence animates.

**BLAS / TLAS.** The ray-tracing data structures for each mesh (bottom level) and the whole scene
(top level). You never edit them; they explain some RTGI settings.

**Bloom.** The glow around bright parts of the image.

**Cascade.** One of several nested grids or levels, each covering a larger area at lower detail
(radiance probe cascades, shadow clipmap levels).

**Clipmap.** Nested levels centred on the camera, each twice the size and half the detail of
the one inside it. Used by virtual shadow maps and voxel GI.

**Collision hulls.** Simplified convex shapes generated from a mesh for physics.

**Component.** A building block attached to an entity that gives it a behaviour: Mesh, Light,
Camera, Decal, Rigid Body, and so on. See [4](04-Components.md).

**Cook / `.ptero`.** Converting an imported model into the engine's own fast format. `Chair.fbx`
is cooked to `Chair.fbx.ptero`.

**Cvar (console variable).** A named setting such as `rtgi.maxbounces` that can be read and
changed from the Console. Every renderer setting is one.

**Decal.** A material projected onto existing surfaces (stains, signs, cracks).

**Deferred rendering.** Drawing all surfaces' properties into the G-Buffer first and lighting
them afterwards, which lets hundreds of lights shade cheaply.

**Denoiser.** A filter that turns the noisy result of firing a few rays per pixel into a clean
image by blending over space and time. Ptero uses NVIDIA NRD.

**DPLE.** Deterministic Photoreal Lighting Enhancer: a post-lighting pass that adds multiscale
AO, contact shadows and material-aware tweaks. See [7](07-Global-Illumination.md#dple).

**DXR.** DirectX Raytracing, the hardware ray-tracing feature of DirectX 12. Ptero needs DXR 1.1
for ray-traced features.

**Emissive.** Light a material gives off by itself.

**Entity.** One thing placed in a level. It has a name, a transform, a permanent id, and any
number of components.

**Event (FMOD).** A sound as authored in FMOD Studio, played by name.

**Event (Node Graph).** A node that starts a script: On Game Start, On Tick, On Key Pressed...

**Event (Sequencer).** A named marker on a sequence's timeline that fires when the playhead
passes it.

**EV100.** Exposure value: how bright the image is made. Higher is darker. Ptero's EV100 values
are offset from a photographer's because of how its light units are normalised.

**Exposure.** How much the captured light is scaled before tonemapping, like a camera's
aperture and shutter.

**Frame generation.** Showing extra interpolated frames between rendered ones (FSR or XeSS FG).

**G-Buffer.** The set of screen-sized textures the renderer writes each surface's colour,
normal, roughness, metallic and depth into before lighting. **View Mode → G-Buffer** shows it.

**Gizmo.** The on-screen arrows, rings and handles for moving, rotating and scaling.

**Global illumination (GI).** Light that has bounced off surfaces: what lights the parts of a
scene no light shines on directly. See [7](07-Global-Illumination.md).

**HDR / scene-linear.** The renderer works with real light quantities (which can be far brighter
than a screen can show) until the tonemapper makes them displayable.

**Kinematic.** A physics body moved by scripts rather than by forces; it pushes other bodies but
is never pushed.

**Latent node.** A Node Graph node that takes time (Delay, Move Entity To) and fires *Completed*
later.

**Level.** One file holding a scene: its entities, its script and its render settings.

**Light style.** An animated brightness pattern for a light (Fire, Candle, Strobe, ...).

**LOD (level of detail).** Simpler versions of a model, swapped in as it gets smaller on screen.

**Lumen (lm), lux (lx).** Units of light. Lumens measure a lamp's total output; lux measures light
arriving on a surface (the sun is about 100 000 lx).

**Material.** A file describing what a surface is made of: colour, roughness, metallic, normal
map and optional effects. See [5](05-Materials.md).

**Metallic.** Whether a surface is bare metal (1) or not (0).

**Mesh.** The geometry of a model. The Mesh component draws one.

**Multi-material.** A material file with one sub-material per material slot of a model.

**Node Graph.** Ptero's visual scripting, in the style of Unreal's Blueprints.

**Normal map.** A texture that fakes fine surface relief by bending the lighting.

**NRD.** NVIDIA Real-time Denoisers, used to clean up ray-traced GI, AO and the real-time path
tracer.

**Path tracer.** A renderer that simulates light by following many random paths; slow to
converge but physically correct. Ptero has a reference mode and a real-time mode.

**PBR (physically based rendering).** Describing materials and lights with real-world
properties so they look right under any lighting.

**Penumbra.** The soft edge of a shadow, where a point sees only part of the light. It widens
with the distance from the object casting the shadow and with the size of the light.

**Play session.** Running the level as a game inside the editor. Everything resets when it stops.

**`PlayerStart`.** An entity with this name is where the player spawns.

**`.ppak`.** An encrypted, compressed archive holding one `Data/` folder in a built game.

**Probe.** A point in space that records the light arriving from every direction, for nearby
surfaces to look up.

**Ray tracing.** Finding what a line through the scene hits, in hardware; the basis of RTGI,
RTAO, ray-traced shadows, ray-traced reflections and the path tracer.

**Ray-traced shadows.** Shadows found by tracing a ray from each pixel to the light, instead of
reading a shadow map. See [8](08-Shadows.md#ray-traced-shadows).

**Rect light.** A rectangular area light, like a window or a softbox.

**ReSTIR.** A technique for reusing good light samples across pixels and frames to reduce noise.

**Rigid body.** An entity simulated by physics (falling, colliding, being pushed).

**RmlUi, RML, RCSS.** The game UI library and its HTML-like documents and CSS-like stylesheets.

**Roughness.** How matte (1) or glossy (0) a surface is.

**RTAO.** Ray-traced ambient occlusion. See [7](07-Global-Illumination.md#ray-traced-ao).

**RTGI.** Ray-traced global illumination.

**Sequence, Sequencer.** A cinematic timeline, and the editor that edits it.

**Shadow budget.** The limit on how many lights cast shadows at once.

**SSR.** Screen-space reflections: reflections traced against what is on screen.

**Subsurface scattering (SSS).** Light entering a surface and leaving elsewhere, as in skin,
wax and leaves.

**TAA.** Temporal anti-aliasing: smoothing edges by blending each frame with previous ones.

**Tessellation.** Subdividing a surface on the GPU so a height map can displace real geometry.

**Tonemapping.** Turning HDR light values into a displayable image.

**Transform.** An entity's position, rotation and scale.

**UDIM.** A texture set split into numbered tiles (1001, 1002...) across UV space.

**Upscaler.** DLSS, FSR, XeSS, TAAU or AAR: render at lower resolution and reconstruct a sharp
full-resolution image.

**Vegetation Area.** A volume that fills itself with trees, grass and rocks by rules.

**Virtual shadow map (VSM).** A huge virtual shadow texture of which only the visible, needed
pages are rendered and cached.

**Virtualized geometry.** Splitting a dense mesh into clusters whose detail is chosen per frame
from their size on screen (like Unreal's Nanite).

**Voxel GI.** Global illumination computed on a 3D grid of cubes around the camera; works without
ray tracing.

**Z-up.** Ptero's convention: the Z axis points up, the ground is the XY plane.
