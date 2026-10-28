# FAQ and known issues

When something goes wrong, the first place to look is the newest log in `Logs/` (or the
**Console** panel). Most problems explain themselves there.

## Starting up

**The first launch takes minutes.**
The engine compiles all its shaders for your GPU on first launch and caches them in
`Cache/Shaders`. Later launches reuse the cache. After an engine update, the first launch is
slow again.

**The editor looks blurry on a high-resolution display.**
The editor is not DPI-aware, so with Windows display scaling above 100% Windows stretches it.
Known issue.

**A ray-tracing feature is greyed out or does nothing.**
Your GPU has no hardware ray tracing (DXR 1.1). Voxel GI replaces ray-traced GI automatically;
see [Getting started](00-Getting-Started.md#what-changes-without-a-ray-tracing-gpu) for the other
fallbacks.

## Levels and assets

**A sample level opens with missing models.**
The level refers to models under `Data/Geometry` that are not in your copy. Some older test
levels (`Mine.json`, `ModelTest.json`, `Sea.json`) refer to models by an absolute path on the
original development machine and will not load elsewhere.

**An old `.json` level has objects at the wrong size.**
Models are imported in metres since mesh format version 5. Levels saved before that were not
converted. Rescale the entities and save the level as `.level`.

**My model is huge, tiny or lying on its side.**
Ptero converts FBX units and axes on import, so this usually comes from the source file: check
the unit scale and up axis in your 3D tool's FBX export, and that the model is built at real
size. Re-import after fixing.

**A colour texture looks washed out.**
Colour textures imported with older engine builds (May–September 2026) from PNGs without colour
tags were converted twice and look washed out. Re-import them.

**A normal map looks blocky, or a texture shifts in tone at a distance.**
The importer picks the compression from the file name: names containing `normal` (or `_n.`,
`_nor`) become high-quality normal maps, and names containing `roughness`, `metallic`, `ao`,
`height`, `mask` and similar are stored as data. A normal map without `normal` in its name is
compressed as colour; a colour texture whose name happens to contain `ao` (`Chaos_Albedo.png`)
gets data-style mipmaps. Rename the file and re-import. See
[3. Assets](03-Assets-And-Importing.md#textures).

**My new material looks dark and metallic.**
A new material starts with Metallic at 1. Set it to 0 for anything that isn't bare metal.

**A metal surface is black.**
Metals only reflect their surroundings. In an empty scene with no sky there is nothing to
reflect. Turn on Time of Day, add lights, or lower Metallic.

**A normal map lights from the wrong direction.**
It is in the OpenGL convention. Tick *Flip Normal Green (OpenGL)* in the material.

**Parallax makes the whole surface look sunken or shifted.**
The height map is centred on 0.5 (common with Substance exports). Set the material's *Height
Reference* to the map's brightest value.

**My terrain has terraces and blotchy lighting on slopes.**
The heightmap is 8-bit. Use a 16-bit `.raw`, PNG or TIF.

**All geometry suddenly disappeared.**
The shaders in `Data/Shaders` and the engine DLLs in `Binaries/` come from different versions.
Make sure both are from the same build, then **File → Compile Shaders**.

## Lighting and rendering

**The image is noisy for a moment after I change something.**
The GI denoiser is accumulating. It settles in about half a second. Shorter accumulation time
(RTGI → NRD) reacts faster but is noisier.

**Light leaks through a thin wall.**
With voxel GI, the voxels are thicker than the wall: lower *Voxel Size* or make the wall
thicker. With RTGI, keep *Shadow Map Visibility*'s Max Texel Size small.

**The scene is far too bright or too dark.**
Check who controls exposure: with Time of Day on, it is the Day / Sunset / Night EV100 values
(higher = darker); otherwise the AgX Tonemapper's EV100. Remember Ptero's EV100 values are
offset from real-camera ones (a sunny day is about -2). See [6. Lighting](06-Lighting.md).

**A light makes no difference.**
Check its Radius reaches the surfaces, its intensity is in lumens (800 is a household bulb), and
that you are not looking at it at noon, when the sun is 100 000 lux.

**Shadows have stripes, or float away from objects.**
Raise the virtual shadow map biases for stripes ("acne"); lower them if shadows detach. See
[8. Shadows](08-Shadows.md).

**Some lights don't cast shadows.**
The shadow budget only shadows the 8 most important lights at a time. Raise *Max Shadowed
Lights* or turn the budget off.

**Ray-traced shadows are grainy, or flat surfaces get black speckles.**
Grain in soft shadows: raise *Rays Per Pixel* or lower the light's softness. Speckles on open,
flat surfaces: raise *Ray Bias*. The *Raw* debug views show the rays before denoising. See
[8. Shadows](08-Shadows.md#when-something-looks-wrong).

**Ray-traced AO seems to do nothing.**
In a room lit by lamps or candles there is little sky light for AO to darken. Raise *Local
Light Occlusion* so it darkens the lamps' light too. See
[7](07-Global-Illumination.md#ray-traced-ao).

**An emissive surface doesn't glow.**
Emissive materials light their surroundings through the GI and the path tracer, but the normal
image does not draw the glow on the surface itself yet. Place a light (a rect light for a panel)
at the surface as the visible source.

**Radiance Cascades does nothing.**
Its settings are still present, but radiance cascades are not drawn by the current renderer. Use
RTGI, radiance probes or voxel GI.

## Gameplay

**Play does nothing / the player falls through the floor.**
Check the floor is a mesh (meshes collide automatically) and that `PlayerStart`, if you have one,
is above the floor. The log says why a play session failed to start.

**My Node Graph doesn't react.**
Keys only reach the graph while the game window has focus. Add **Print String** nodes and watch
the Console to see how far execution gets. Entity references to deleted entities show as
`<missing id>`.

**Character nodes do nothing.**
They only work with the **Node Graph** player controller (Game Settings). With the C++ controller
the character lives in `Game.dll`.

**A sequence won't play in the game.**
There is no Node Graph node for playing sequences yet; they are started from C++ code.

## Known limitations

- No skeletal animation: meshes cannot bend or be rigged. They can move, rotate and be simulated.
- Entities cannot be hidden by scripts; move them out of sight instead.
- No depth of field. Camera roll is stored but not shown.
- Sequences have no audio tracks; rendered MP4s have no sound.
- Rain blows with the scene wind; the Rain component's own wind values are ignored.
- **Build Game** may need Visual Studio to compile the Release engine DLLs.
- Steam DRM-wrapped executables are sometimes flagged by antivirus software (a Valve issue).
- Only Windows and DirectX 12.

## Reporting a problem

Include the newest log from `Logs/`, your GPU and driver version, the level, and the steps to
reproduce it. If the editor froze, the log's last lines (including any `[Watchdog]` line) show
where.
