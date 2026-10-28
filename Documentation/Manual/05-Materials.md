# 5. Materials

A material describes what a surface is made of. It is a small JSON file (`.material` or
`.json` in `Data/Materials`) edited in **Windows → Material Editor...**.

The Material Editor has two tabs:

- **Single Material**: one material. New, Open, Save, Save As.
- **Multi-Material**: a list of sub-materials, one per material slot of a model. Sub-material
  0 goes on the mesh's first FBX material slot, 1 on the second, and so on. Importing an FBX
  creates one of these for you in `Data/MultiMaterials`.

A Mesh component takes either kind.

## How surfaces are shaded

Ptero uses a **physically based** material model. You describe the surface with a few
real-world properties and the lighting does the rest, so one material looks right under noon
sun, candlelight and moonlight alike.

The maths that turns those properties into shading is the **Disney principled BRDF**, described
in its own section [below](#the-disney-principled-brdf).

## Core properties

| Property | Meaning |
| --- | --- |
| Base Colour (texture × tint) | The surface colour with no lighting. For metals, the colour of the reflection. Keep it out of pure black and pure white: real materials sit between roughly 0.03 and 0.9. |
| Metallic (factor 1, or texture) | 0 = non-metal (wood, plastic, stone, skin), 1 = bare metal. In-between values are only for transitions such as rust edges. |
| Roughness (factor 1, or texture) | 0 = mirror-smooth, 1 = completely matte. The single most important property for realism. |
| Specular (0.5) | Reflectivity. 0.5 gives the standard 4% reflection of most non-metals. Raise it for wet or polished looks, lower it to remove reflections. Applies to metals too. |
| Normal map, Normal Scale (1) | Fine surface relief. **Flip Normal Green (OpenGL)** for maps authored in the OpenGL convention. |
| Ambient Occlusion map, Strength (1) | Baked crevice darkening. |
| Emissive colour (and map) | Light the surface gives off. It feeds the ray-traced and voxel GI and the path tracer, so a glowing panel can light its surroundings. The normal raster image does not draw the glow itself yet; put a light next to it for a visible source. |
| Opacity (1, or map) | Used by alpha cutout and transparent blend. |

### Texture slots

| Slot | Contents |
| --- | --- |
| Base Colour | sRGB colour (alpha can carry opacity) |
| Normal | Tangent-space normal map |
| Metallic, Roughness | Separate greyscale maps |
| Metallic-Roughness (packed) | R = roughness, G = metallic, B = AO |
| Occlusion-Roughness-Metallic (ORM) | Unreal-style packed map: R = AO, G = roughness, B = metallic |
| Ambient Occlusion | Greyscale |
| Emissive | RGB |
| Height | Greyscale height for parallax or tessellation |
| Opacity | Greyscale |
| Principled Mask | R = clearcoat, G = sheen, B = anisotropy, A = subsurface flatten; each scales its slider |
| Anisotropy Direction | RG = the direction the highlight stretches in UV space (0.5 = none) |

### UV transform

Applied to every texture of the material: **Tiling** repeats, **Offset** slides, **Rotation**
spins around the texture centre (degrees).

## Transparency and sidedness

| Switch | Use for |
| --- | --- |
| **Double Sided** | Leaves, cloth, paper, anything single-sheet seen from both sides. |
| **Use Alpha Cutout** + Alpha Cutoff (0.5) | Hard-edged holes: foliage cards, fences, chain link. Pixels below the cutoff are removed entirely. Cheap, casts correct shadows. |
| **Use Transparent Blend** | Soft see-through surfaces. |
| **Glass Rendering** | Real glass (below). |

## Glass

**Glass Rendering** turns a material into refracting, reflecting glass. It is resolved after the
lighting:

1. Refraction is first looked up on screen, behind the glass.
2. Where the screen has nothing to show, one ray-traced sample finds what is behind or
   reflected (on GPUs with ray tracing).
3. Colour **dispersion** (the rainbow fringe of a prism) is faked cheaply by offsetting the
   colour channels.

| Property | Meaning |
| --- | --- |
| Thin Glass (on) | A window pane: light passes straight through with no bending. Off treats the object as solid glass (a bottle, a lens, an ice cube). |
| IOR (1.5) | How strongly solid glass bends light. Water 1.33, glass 1.5, diamond 2.4. |
| Thickness (0.08 m) | How thick the glass is assumed to be. |
| Dispersion (0.35) | Strength of the colour fringe. |
| Caustic Strength (0) | Light focused through the glass. |

## Parallax occlusion mapping

Gives flat surfaces convincing depth (bricks, cobbles, planks) by ray-marching a height map in
the pixel shader. No extra geometry; the silhouette stays flat.

| Property | Meaning |
| --- | --- |
| Use Parallax Occlusion | Needs a Height texture. |
| Height Scale (0.05) | Depth of the relief, in tiled UV units. |
| Min / Max Steps (8 / 32) | March quality. More steps, less stair-stepping, more cost. |
| Fade Distance (30 m) | Beyond this the effect fades out (far surfaces cannot resolve it anyway). 0 = never fade. |
| Height Reference (1) | Which height value sits at the actual surface. 1 is right for a normal 0–1 height map. Substance-style maps centred on 0.5 should set this to the map's brightest value, or the whole surface appears sunk. |

## Tessellation and displacement

Real geometry from a height map: the GPU subdivides the surface and pushes the new vertices out
along the normal, so the relief has a real silhouette and casts real shadows. Works on meshes
and terrain.

| Property | Meaning |
| --- | --- |
| Use Tessellation | Needs a Height texture. |
| Max Factor (16) | Most pieces an edge is split into (1–64). |
| Target Pixels (8) | Edges are split until each piece is about this long on screen, so detail follows distance. |
| Fade Distance (60 m) | No tessellation beyond this. 0 = no fade. |
| Displacement Scale (0.05 m) | Distance between the height map's black and white. |
| Mid Level (0.5) | Height value that stays at the original surface. 0.5 keeps the average surface where the polygon was. |

Use parallax for small detail on large areas, tessellation for hero surfaces where the
silhouette matters.

## Subsurface scattering

Light that enters a surface, scatters inside and comes out elsewhere: skin, wax, marble, jade,
milk, leaves. Without it, skin looks like painted plastic.

| Property | Meaning |
| --- | --- |
| Use Subsurface Scattering | Turn it on for this material. |
| Subsurface Colour (0.48, 0.41, 0.28) | How much of each colour channel scatters. |
| Falloff (1, 0.37, 0.3) | How far each channel spreads relative to the radius. Skin scatters red furthest, which is its warmth. |
| Radius (3 mm) | Size of the scattering. 3 mm is human skin at real scale; wax and marble want much more. |
| Translucency (0.8) | Light passing all the way through thin parts (ears, fingers, leaves) and lighting the far side. 0 = none. |

The scene's **Subsurface Scattering** settings choose how it is computed: a cheap screen-space
blur, or ray-traced scattering that follows the real mesh around ears and noses. See
[7. Global illumination](07-Global-Illumination.md#subsurface-scattering).

## The Disney principled BRDF

> **In depth:** [The Disney principled BRDF](05a-Disney-Principled-BRDF.md) explains every lobe,
> how the parameters interact, where each lobe applies, the exact formulas Ptero uses, and an
> authoring guide with values for common materials. This section is the summary.

A **BRDF** (bidirectional reflectance distribution function) is the rule that says, for light
arriving at a surface from one direction, how much leaves towards the camera. Every material
in Ptero is shaded by one BRDF: the **Disney principled BRDF**, from Brent Burley's 2012 paper
*Physically-Based Shading at Disney*, the model behind most film and game PBR since.

"Principled" means it was designed for artists rather than physicists: a few intuitive
sliders, each in 0–1, each doing one thing you can predict, which together cover nearly every
real material. The same BRDF code runs in the normal (raster) lighting and in the path tracer, so
the real-time image and the reference render agree on what a material is.

### Choosing the shading model

**Graphics Settings → Shading Model** (cvar `r.disneybrdf`) applies to the whole level:

| Model | What it is |
| --- | --- |
| **Disney principled BRDF** (default) | Burley's diffuse, anisotropic GGX specular with Disney's masking, and all the extra lobes below. |
| **Original model** | Lambert diffuse weighted by (1 − Fresnel) and Cook-Torrance GGX specular. The extra lobes are ignored. Kept for comparison and older levels. |

### How it shades a surface

The BRDF adds up a few **lobes**, each a separate kind of reflection:

**Diffuse.** Light that enters the surface, scatters and comes back out in all directions: the
colour of paint, wood or skin. Disney's diffuse is not plain Lambert: rough surfaces get a little
**retro-reflection** (they brighten at grazing angles, like dusty or matte materials do) and smooth
surfaces darken slightly at grazing angles. Metals have no diffuse.

**Specular.** The mirror-like reflection off the surface: highlights and reflections. Its shape is
**GGX** (also called Trowbridge-Reitz), whose long tail gives highlights the soft glow real
materials have. *Roughness* spreads it from a sharp mirror (0) to a broad sheen (1). How strong
it is head-on comes from *Specular* for non-metals (0.5 = the standard 4%) and from the base
colour for metals; at grazing angles everything becomes more reflective (the **Fresnel** effect),
which is why a floor reflects the far end of a room more than your feet.

**The extra lobes** (all off by default) add what the two base lobes cannot:

| Lobe | Material property | What it adds | Typical use |
| --- | --- | --- | --- |
| **Subsurface flatten** | Subsurface Flatten | Flattens the diffuse falloff towards a thin-subsurface look, as if light softened inside the surface. A change of shape only; real light transport is [subsurface scattering](#subsurface-scattering), and the two combine. | Skin, wax, marble, fruit, at low values |
| **Specular tint** | Specular Tint | Tints non-metal highlights towards the base colour's hue. Physically, dielectric highlights are white; this is an artistic control. | Stylised materials, some fabrics |
| **Anisotropic** | Anisotropic, Anisotropy Direction map | Stretches the highlight along one direction of the surface (its U axis, or the direction map) instead of a round spot. | Brushed metal, hair, satin, vinyl records, carbon fibre |
| **Sheen** | Sheen, Sheen Tint (0.5) | A soft extra reflection at grazing angles, the halo you see on velvet and cloth edges. *Sheen Tint* colours it towards the base colour. | Cloth, velvet, dusty surfaces, peach fuzz |
| **Clearcoat** | Clearcoat, Clearcoat Gloss (1) | A second, colourless specular layer on top, like a coat of varnish (fixed index of refraction 1.5). *Gloss* goes from satin (0) to glossy (1). Its highlight has an even longer tail than the base specular. | Car paint, lacquered wood, varnished floors, glazed ceramics, wet surfaces |

A material that leaves all of them at 0 looks exactly as it would under the base model, so they
cost nothing to ignore.

### Painting the lobes

Each extra lobe can vary across a surface with textures:

| Texture slot | Channels |
| --- | --- |
| **Principled Mask** | R = clearcoat, G = sheen, B = anisotropic, A = subsurface flatten. Each channel scales its slider: black = none, white = the full slider value. |
| **Anisotropy Direction** | RG = the direction the highlight stretches, in the texture's UV space (R along U, G along V, 0.5 = zero), in the normal map's green convention. |

Painting them by hand is fiddly; the [Substance 3D Painter plugin](03-Assets-And-Importing.md#substance-3d-painter)
paints them as ordinary channels with a live preview of the engine's BRDF and packs them for you.

### Recipes

Starting points; tune by eye under the lighting the material will live in.

| Material | Base | Extra lobes |
| --- | --- | --- |
| Car paint | Base colour = paint colour, Metallic 0–0.3, Roughness 0.3–0.5 | Clearcoat 1, Clearcoat Gloss 0.9–1 |
| Lacquered or varnished wood | Wood texture, Metallic 0, Roughness 0.4–0.6 | Clearcoat 0.6–1, Gloss 0.8 |
| Brushed metal | Metallic 1, Roughness 0.3–0.4 | Anisotropic 0.6–0.9; direction along the brushing |
| Velvet, cotton | Fabric colour, Roughness 0.8–1 | Sheen 0.5–1, Sheen Tint 0.3–0.6 |
| Skin | Skin texture, Roughness 0.4–0.6 | Subsurface Flatten 0.2–0.4, plus real [subsurface scattering](#subsurface-scattering) |
| Satin | Fabric colour, Roughness 0.3–0.5 | Anisotropic 0.4, Sheen 0.3 |
| Glazed ceramic | Base colour, Roughness 0.5 | Clearcoat 1, Gloss 1 |

### Where the lobes show up

- **Direct light** (the sun, the moon, every point, spot and rect light) and the **path tracer**
  use the full BRDF with every lobe.
- **Reflections** from SSR and ray tracing use the base specular; they only differ from the full
  model on tinted (Specular Tint) surfaces.
- The extra lobes are stored in two extra G-Buffer targets, so they work with every GI system,
  upscaler and post effect without special cases.

## Eye materials (Iris Adjust)

Moves, scales and dilates the iris in texture space, so an eye can look somewhere or have a
wider pupil without moving its mesh. Iris Centre and Radius locate the iris in the texture;
Offset moves it; Scale resizes it; Pupil Radius and Pupil Scale dilate the pupil; Warp Radius
limits the area that may stretch.

## Decal materials

Tick **Decal Material** to make a material usable by a Decal component. It is projected onto
whatever lies in the decal's box. See [4. Components](04-Components.md#decal).

## Particle materials

A particle material is drawn as camera-facing sprites by the particle system, never into the
G-Buffer. Its base colour texture is the sprite (or flipbook sheet), the tint multiplies it,
and the emissive colour is the glow.

| Property | Meaning |
| --- | --- |
| Blend Mode | **Additive** for fire, sparks, magic. **Alpha Blend** for smoke and steam. **Premultiplied** for flipbooks exported with premultiplied alpha (most fluid-sim output). |
| Emissive Intensity (1) | Pushes the glow into HDR without colour values above 1. |
| Lighting Influence (0) | How much scene lighting the sprite receives. Fire is 0 (it makes its own light); smoke wants about 1 so it darkens away from the flame and catches the sun. |
| Spherical Normal (0.6) | Shades the sprite like a sphere instead of a flat card. Reads much better for thick smoke. |
| Camera Fade Distance (0.6 m) | Fade sprites near the camera, so walking through a fire is not a full-screen flash. |
| Flipbook Columns × Rows | The sheet layout, inherited by every effect using the material. |
| Alpha From Luminance | Take opacity from brightness. Fire and spark sheets are usually colour on black with no useful alpha; this makes them work without editing. |

## Water materials

Water surfaces read their look from a material with a `water` block: surface and depth
colours, clarity and transparency, wave amplitudes and steepness, normal and height textures,
foam and shore foam, refraction and specular. Two presets ship with the engine:
`Materials/Ocean.json` (tropical sea) and `Materials/Water.json`, selectable with the
**Ocean** and **Water** buttons on the Water component. To make your own, copy one and edit its
`water` block. See [9. Outdoor worlds](09-Outdoor-Worlds.md#water).

## Terrain layer materials

A terrain paint layer can use a full material (base colour, normal, roughness, metallic, AO
and height). The height map is also what drives height-based blending between layers. See
[9. Outdoor worlds](09-Outdoor-Worlds.md#terrain).

## Practical tips

- Roughness does more for realism than any other value. Check it before anything else.
- Decide metal or not-metal per surface; avoid metallic values in between.
- A surface that looks black in an empty test level is often a metal with nothing to reflect.
  Add a sky (Time of Day) or lights, or lower Metallic.
- Name texture files by type (`_BaseColor`, `_Normal`, `_Roughness`) so the importer stores
  them correctly. See [3. Assets](03-Assets-And-Importing.md#textures).
