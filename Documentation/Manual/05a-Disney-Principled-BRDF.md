# The Disney principled BRDF

*A subtopic of [5. Materials](05-Materials.md).*

Every surface in Ptero is shaded by one model: the **Disney principled BRDF**. This page explains
what that means, what each of its parts does to a surface, and exactly how Ptero computes it. The
first half needs no maths; the [second half](#how-ptero-computes-it) shows the formulas for anyone
who wants them.

- [What a BRDF is](#what-a-brdf-is)
- [Why "principled"](#why-principled)
- [The parameters](#the-parameters)
- [The lobes](#the-lobes)
- [How the parameters work together](#how-the-parameters-work-together)
- [Where each lobe applies](#where-each-lobe-applies)
- [How Ptero computes it](#how-ptero-computes-it)
- [Differences from Disney's original](#differences-from-disneys-original)
- [The original model](#the-original-model)
- [Authoring guide](#authoring-guide)
- [Further reading](#further-reading)

## What a BRDF is

When light hits a surface, some of it bounces straight off (a **reflection**) and some goes into
the surface, scatters around inside, and comes back out in random directions (the surface's
**colour**). How much light leaves towards your eye depends on three things: the direction the
light arrives from, the direction you look from, and what the surface is made of.

A **BRDF** (bidirectional reflectance distribution function) is the formula that answers that
question. Give it a light direction, a view direction and the material's properties, and it says
how bright the surface looks. The renderer evaluates it for every pixel, for every light.

Real BRDFs have shapes you can recognise:

- A **matte** surface (chalk, unfinished wood) scatters light almost evenly in every direction,
  so it looks the same from wherever you view it.
- A **glossy** surface (plastic, polished stone) has a **highlight**: a bright spot where light
  bounces towards you mirror-wise, blurred by the surface's microscopic roughness.
- A **metal** has only the highlight, tinted by the metal's colour, and no matte colour at all.
- **Everything** gets more reflective at grazing angles. Look across a table or a lake and it
  mirrors the far end; look straight down and you see into it. This is the **Fresnel effect**.

A BRDF also has to be **energy conserving**: a surface cannot send out more light than arrives.
That is what makes physically based materials look right under any lighting, from noon sun to
a single candle, without being retuned.

## Why "principled"

In 2012 Brent Burley of Walt Disney Animation Studios published *Physically-Based Shading at
Disney*. Disney had measured hundreds of real materials and found that the physically exact
models were hard for artists to use: dozens of parameters, many in units nobody has intuition for.
So they designed a model on **principles** rather than strict physics:

1. Intuitive parameters, not physical ones.
2. As few parameters as possible.
3. Every parameter from 0 to 1 over its plausible range.
4. Values outside that range allowed where they make sense.
5. All combinations should look robust and plausible.

The result covers almost every real material with a handful of sliders, each of which does one
thing you can predict. It became the basis of PBR in most film renderers and game engines since,
including Unreal's and Blender's Principled BSDF.

Ptero implements Burley's model as published (the paper, the course notes and the reference
`disney.brdf` file) with a few small, documented changes ([below](#differences-from-disneys-original)).
The same code shades the real-time image and the path tracer, so the two agree on what every
material is.

## The parameters

Four **base parameters** describe every surface:

| Parameter | Range | What it means |
| --- | --- | --- |
| **Base colour** | colour | The colour of the surface. For non-metals it is the diffuse colour; for metals it is the colour of the reflection. |
| **Metallic** | 0–1 | 0 = non-metal (dielectric): wood, stone, plastic, skin, paint. 1 = bare metal. Values in between are for transitions (rust edges, worn paint), not for "slightly metallic" materials, which do not exist. |
| **Roughness** | 0–1 | How rough the surface is at the microscopic level. 0 = a perfect mirror, 1 = completely matte. Controls how sharp or broad the highlight is. |
| **Specular** | 0–1, 0.5 neutral | How reflective the surface is head-on. 0.5 gives the 4% reflectance of most non-metals. See [below](#specular). |

And seven **extra parameters**, all off (0) by default:

| Parameter | Range | What it adds |
| --- | --- | --- |
| **Subsurface (flatten)** | 0–1 | Softens the diffuse falloff, as if light diffused inside the surface |
| **Specular tint** | 0–1 | Tints non-metal reflections towards the base colour |
| **Anisotropic** | 0–1 | Stretches the highlight along one direction |
| **Sheen** | 0–1 | A soft extra reflection at grazing angles, for cloth |
| **Sheen tint** | 0–1, default 0.5 | How much the sheen takes on the base colour |
| **Clearcoat** | 0–1 | A second, clear varnish layer on top |
| **Clearcoat gloss** | 0–1, default 1 | How glossy that varnish layer is |

All of these are material settings in the Material Editor, and all except Specular and Sheen Tint
can also be painted per pixel with the **Principled Mask** texture (see
[Painting the lobes](05-Materials.md#painting-the-lobes)).

### Specular

Specular sets the reflectance at normal incidence, the amount a surface reflects when you look at
it straight on (called **F0**):

| Specular | F0 for a non-metal | Looks like |
| --- | --- | --- |
| 0 | 0% | No highlight or reflection at all |
| 0.25 | 2% | Very dull (some fabrics, rubber) |
| **0.5** | **4%** | **Most materials**: plastic, wood, stone, skin, paint (index of refraction about 1.5) |
| 0.75 | 6% | Gemstones, some plastics |
| 1 | 8% | The brightest common dielectrics |

Ptero also lets Specular scale **metals**: at 0.5 a metal reflects exactly its base colour; below
it reflects less, above it more. A fully metallic surface has no diffuse colour, so in a scene with
nothing bright for it to mirror it can go nearly black; raising Specular brings its reflection
back up without faking it through the base colour.

## The lobes

The BRDF is a sum of **lobes**, each a separate kind of reflection with its own shape. Picture
each one as the "bulge" of light it sends out from a point on the surface.

### Diffuse

Light that enters the surface, scatters and comes back out in every direction. It carries the
base colour. Metals have none: `diffuse weight = 1 − metallic`.

The simplest diffuse model, Lambert, is perfectly even. Real materials are not, and Burley's
diffuse adds a **retro-reflection** term that depends on roughness:

- **Rough** surfaces (concrete, clay, dusty materials) get *brighter* at grazing angles. It is why
  the edge of a dusty ball or the full moon does not darken towards its rim.
- **Smooth** surfaces get slightly *darker* at grazing angles, because more of the light is
  reflected by the specular lobe there instead of entering the surface.

You do not set this; it follows from Roughness.

### Subsurface flatten

At **Subsurface** above 0, the diffuse lobe blends towards a flatter shape inspired by the
Hanrahan-Krueger model of thin subsurface scattering: the falloff across the surface softens, as
if the light spread a little under the skin before coming back out. It **preserves the total
amount of diffuse light**; it only changes where it goes.

This is a change of *shape*, not real light transport: light does not travel from one point to
another. For real scattering (light glowing through ears and fingers, softening across pores) use
the material's **Subsurface Scattering** settings, which Ptero computes separately; the two
combine well. See [Subsurface scattering](05-Materials.md#subsurface-scattering).

### Specular (the main highlight)

The reflection off the surface itself: highlights from lights and mirror images of the
surroundings. It has three parts:

- **The distribution** decides the highlight's shape. Ptero uses **GGX** (also known as
  Trowbridge-Reitz, or GTR with γ = 2). A rough surface is made of countless tiny facets pointing
  in slightly different directions; GGX describes how many face each way. Its characteristic is a
  **long tail**: real highlights do not end abruptly but fade into a soft glow, and GGX reproduces
  that. Roughness spreads it: Ptero squares Roughness first (`α = roughness²`), which makes the
  slider feel even from mirror to matte.
- **Fresnel** decides how strong it is: F0 head-on (from Specular and Metallic), rising to 100% at
  grazing angles. Even a matte, dark material becomes a mirror when you look at it edge-on.
- **Masking-shadowing** accounts for the tiny facets blocking each other at grazing angles, which
  stops rough surfaces gaining impossible brightness at their edges.

### Specular tint

Non-metal highlights are white in reality: a red plastic ball has a white highlight. **Specular
Tint** moves the non-metal highlight towards the hue of the base colour (at unit brightness, so it
changes colour without making the highlight brighter or darker). It is an artistic control,
useful for stylised looks and some fabrics; physically based materials leave it at 0. Metals are
always tinted by their colour regardless.

### Anisotropic

Normally a highlight is round. On surfaces with a directional microstructure (brushed metal, hair,
satin, a vinyl record, carbon fibre) the facets are stretched one way and the highlight stretches
across them. **Anisotropic** elongates the highlight along the surface's **tangent**, which is the
texture's U direction, or the direction painted in the **Anisotropy Direction** map.

As it stretches, the highlight keeps roughly the same area, so it does not get brighter or
dimmer, just longer. At 1, it is about three times rougher than normal along the stretch and three
times smoother across it: roughly ten times longer than it is wide.

### Sheen

Cloth is made of fibres, and fibres catch light at grazing angles. That is the soft halo you see at
the edges of a velvet cushion or a cotton shirt. **Sheen** adds that grazing reflection. **Sheen
Tint** colours it towards the base colour (0 = white sheen, 1 = fully tinted; 0.5 by default).

Sheen is weak head-on and strong at the edges, and it only applies to non-metals. Use it for
fabrics, and in small amounts for dust and peach fuzz.

### Clearcoat

A second specular layer on top of everything else, like a coat of varnish or lacquer:

- It is **colourless** (a clear coat does not tint what it reflects).
- It has a fixed index of refraction of **1.5** (4% head-on reflectance), like real varnish.
- Its distribution is **GTR1**, which has an even longer tail than GGX: the tight, bright highlight
  with a broad soft glow around it that car paint has.
- **Clearcoat Gloss** goes from a satin coat (0) to a glossy one (1).
- At full strength it is still deliberately subtle (it reflects at a quarter of a full specular
  lobe), because a real coat reads as a coat, not a mirror.

Use it on car paint, lacquered wood, varnished floors, glazed ceramics, and to make a surface look
wet.

## How the parameters work together

| | Diffuse | Sheen | Specular | Clearcoat |
| --- | --- | --- | --- | --- |
| **Base colour** | is its colour | tints it (with Sheen Tint) | colours it for metals; tints it for non-metals (with Specular Tint) | no effect |
| **Metallic** | fades it out (none at 1) | fades it out | switches F0 from 4% to the base colour | no effect |
| **Roughness** | shapes its retro-reflection | no effect | spreads the highlight | no effect (Gloss controls it) |
| **Specular** | no effect | no effect | sets F0 | no effect |

A useful consequence: because the extra lobes are all off by default, a material that never
touches them looks *exactly* like the base model, and turning one on changes only what it is for.

## Where each lobe applies

| Light | What is evaluated |
| --- | --- |
| **The sun, the moon, and every point, spot and rect light** | The full BRDF: every lobe, exactly as described here. |
| **Sky ambient** | Base diffuse and specular, plus cheaper stand-ins for sheen (a rim growing with the grazing Fresnel of the view) and clearcoat (one more Fresnel-weighted reflection). Not exact integrals of the lobes, but they keep cloth and coated surfaces reading correctly in shadow. |
| **Reflections** (screen-space and ray-traced) | The base specular. They match the full model everywhere except on surfaces with Specular Tint, where the tint is not applied. |
| **Global illumination** (bounce light) | The base model: bounced light is gathered without the extra lobes. |
| **The path tracer** | The full BRDF, for every bounce. |

The extra parameters travel through the renderer in two additional G-Buffer targets (8 bits per
parameter; the anisotropy direction as an angle, accurate to 0.7°), so they work with every GI
system, upscaler and post effect without special cases.

## How Ptero computes it

This section is for the curious; nothing here needs to be understood to make good materials. It
follows `Data/Shaders/PrincipledBrdf.hlsli`, where the code lives.

### Notation

| Symbol | Meaning |
| --- | --- |
| N | Surface normal |
| L | Direction towards the light |
| V | Direction towards the viewer |
| H | Half vector, normalize(L + V): the facet orientation that would reflect L into V |
| θl, θv, θd | Angles between N and L, N and V, and L and H |
| X, Y | Tangent and bitangent (the anisotropy directions) |
| F_S(u) | Schlick's Fresnel weight, (1 − u)⁵ |
| tint | Base colour divided by its luminance (0.3 R + 0.6 G + 0.1 B): its hue at unit brightness |

### Diffuse

```
Fd90    = 0.5 + 2 · roughness · cos²θd
Fd      = (1 + (Fd90 − 1) · F_S(cos θl)) · (1 + (Fd90 − 1) · F_S(cos θv))

Fss90   = roughness · cos²θd
Fss     = (1 + (Fss90 − 1) · F_S(cos θl)) · (1 + (Fss90 − 1) · F_S(cos θv))
ss      = 1.25 · (Fss · (1 / (cos θl + cos θv) − 0.5) + 0.5)

diffuse = baseColor / π · lerp(Fd, ss, subsurface) · (1 − metallic)
```

`Fd90` above 1 brightens grazing angles (rough surfaces); below 1 darkens them (smooth ones). The
1.25 factor keeps the flattened lobe's total energy equal to the normal one's.

### Sheen

```
sheen = F_S(cos θd) · sheen · lerp(1, tint, sheenTint) · (1 − metallic)
```

### Specular

```
F0         = lerp(0.04 · lerp(1, tint, specularTint), baseColor, metallic) · 2 · specular
F          = lerp(F0, 1, F_S(cos θd))

α          = roughness²
aspect     = sqrt(1 − 0.9 · anisotropic)
αx, αy     = α / aspect,  α · aspect                (each at least 0.001)

D (GTR2)   = 1 / (π · αx · αy · ((H·X / αx)² + (H·Y / αy)² + (N·H)²)²)

G1(w)      = 1 / (N·w + sqrt((w·X · αx)² + (w·Y · αy)² + (N·w)²))
G          = G1(L) · G1(V)

specular   = D · F · G
```

`G1` here is the Smith GGX masking term already divided by 2 N·w, the form Disney's code uses, so
the usual `D F G / (4 N·L N·V)` reduces to `D F G`. With `2 · specular` at the neutral 0.5, F0 is the
textbook `lerp(0.04, baseColor, metallic)`.

### Clearcoat

```
αc         = lerp(0.1, 0.001, clearcoatGloss)
D (GTR1)   = (αc² − 1) / (π · ln(αc²) · (1 + (αc² − 1) · (N·H)²))
F          = lerp(0.04, 1, F_S(cos θd))
G          = G1(L) · G1(V)   with a fixed roughness of 0.25

clearcoat  = 0.25 · clearcoat · D · F · G
```

### Putting it together

```
f(L, V) = diffuse + sheen + specular + clearcoat
radiance leaving towards V = f(L, V) · light · cos θl
```

Two conventions differ between the two places the BRDF runs:

- **Direct lights in the real-time image** use Disney's *roughness remap* on the masking term,
  `(0.5 + roughness / 2)²` in place of `roughness²` for G only. Burley introduced it for analytic
  lights, to stop very smooth surfaces gaining too much brightness at grazing angles. The path
  tracer, which samples light rather than evaluating point lights, leaves it out, as the notes
  intend.
- The real-time renderer's lighting convention leaves the 1/π off its diffuse term, so it scales
  the Disney diffuse and the sheen (which Disney sizes against the diffuse) back up by π, keeping
  them at the same brightness relative to the lights and to the specular lobes. The path tracer
  uses the true BRDF values.

The Substance Painter preview shader evaluates the same formulas against Painter's environment
light by importance sampling each lobe, and has a switch for the masking remap, so you can see
either convention.

## Differences from Disney's original

| Disney 2012 | Ptero | Why |
| --- | --- | --- |
| *Specular* scales only non-metals | Also scales metals; at 0.5 the result is identical to Disney's | Lets an artist raise a metal's reflection without faking it through the base colour |
| Tangent from the mesh | Tangent from the texture's U direction, or the Anisotropy Direction map, stored as an angle | Works on any mesh, including ones without authored tangents, and fits in 8 bits |
| Subsurface parameter | Named *Subsurface Flatten* | Ptero also has real subsurface scattering, and the name keeps the two apart |
| Specular transmission, IOR (the 2015 BSDF extension) | Not part of the BRDF | Glass is a separate material mode with its own refraction ([Glass](05-Materials.md#glass)) |

## The original model

**Graphics Settings → Shading Model** (cvar `r.disneybrdf`) can switch a level back to Ptero's
original shading model: Lambert diffuse weighted by (1 − Fresnel), and Cook-Torrance GGX specular.
It has no retro-reflection and none of the extra lobes (they are ignored). It is kept for comparing
against and for older levels; there is no reason to use it for new work, since the Disney model
with its extra lobes off is just as cheap and more accurate.

## Authoring guide

### Start with the base four

Most of a material's realism comes from **Roughness** and **Metallic**; get those right before
touching anything else.

| Material | Base colour (sRGB) | Metallic | Roughness | Specular |
| --- | --- | --- | --- | --- |
| Fresh snow | very light grey (~240) | 0 | 0.7–0.9 | 0.5 |
| Concrete | mid grey (~130) | 0 | 0.8–0.95 | 0.5 |
| Unvarnished wood | wood texture | 0 | 0.6–0.85 | 0.5 |
| Plastic | any colour | 0 | 0.2–0.6 | 0.5 |
| Skin | skin texture | 0 | 0.4–0.6 | 0.5 |
| Water, wet stone | dark | 0 | 0.05–0.15 | 0.5 |
| Gold | (255, 195, 85) | 1 | 0.1–0.4 | 0.5 |
| Iron, steel | (140–200 grey) | 1 | 0.2–0.6 | 0.5 |
| Copper | (240, 160, 130) | 1 | 0.15–0.45 | 0.5 |
| Aluminium | (230 grey) | 1 | 0.1–0.4 | 0.5 |

These are starting points. Keep non-metal base colours between about 30 and 240 (sRGB): nothing
real is pure black or pure white.

### Then add a lobe only when the material needs it

| If the material... | Add | Typical values |
| --- | --- | --- |
| has a varnish, lacquer or paint coat | Clearcoat | 0.6–1, Gloss 0.8–1 |
| is brushed or has fibres in one direction | Anisotropic | 0.4–0.9 |
| is cloth, velvet or dusty | Sheen | 0.3–1, Sheen Tint 0.3–0.6 |
| is soft and fleshy | Subsurface Flatten | 0.1–0.4, with real Subsurface Scattering |
| needs a coloured highlight (stylised) | Specular Tint | 0.2–0.6 |

Paint them with the **Principled Mask** where they should vary: clearcoat only on the painted
parts of a guitar, sheen only on the fabric parts of a chair.

### Common mistakes

| Mistake | What you see | Fix |
| --- | --- | --- |
| Metallic between 0.2 and 0.8 on a plain surface | Muddy, dark, unconvincing | Metal or not: 0 or 1 |
| Roughness 0 on everything shiny | Mirror-like plastic that looks fake | Real polished surfaces sit around 0.05–0.3 |
| Dark highlights colour baked into base colour | Lighting that looks painted on | Keep lighting out of the base colour; let the BRDF make highlights |
| Specular far from 0.5 to "brighten" a material | Plastic that glows at its edges | Change base colour or roughness instead |
| Clearcoat to make metal shinier | Double highlight | Lower Roughness instead; clearcoat is for coatings |
| A metal that looks black | Nothing for it to reflect | Add a sky or lights, or raise Specular slightly |
| Sheen on hard surfaces | Edges look fuzzy | Keep sheen for fabrics and dust |

### Previewing

The [Substance 3D Painter plugin](03-Assets-And-Importing.md#substance-3d-painter) previews all of
these lobes inside Painter with the engine's own formulas, and exports the textures and a
`.material` ready to use.

## Further reading

- Brent Burley, *Physically-Based Shading at Disney*, SIGGRAPH 2012 course notes. The source of the
  model and of every term on this page.
- Brent Burley, *Extending the Disney BRDF to a BSDF with Integrated Subsurface Scattering*,
  SIGGRAPH 2015. The transmission extension, which Ptero handles separately.
- Bruce Walter et al., *Microfacet Models for Refraction through Rough Surfaces*, EGSR 2007. The
  origin of the GGX distribution.
