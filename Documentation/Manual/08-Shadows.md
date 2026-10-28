# 8. Shadows

## Virtual shadow maps

By default every shadow in Ptero (the sun's and every shadow-casting light's) comes from
**virtual shadow maps** (VSM). Settings are in **Graphics Settings → Virtual Shadow Maps**.

A normal shadow map is one texture stretched over an area, so it is sharp near the camera or
far away, never both. A virtual shadow map is enormous (the sun's has up to 16 levels, each a
16 384 × 16 384 virtual texture), but only the small 128 × 128 **pages** that pixels on screen
actually need are ever rendered. A rendered page stays **cached** until something moves through
it, the light moves, or it goes unused. The result is crisp shadows from a pebble at your feet
to a mountain on the horizon, at a cost that depends on what changes, not on how big the level
is.

| What | How it is covered |
| --- | --- |
| Sun | A **clipmap** centred on the camera: level 0 has 4 mm texels, and every further level doubles the texel size and the area. |
| Point, spot and rect lights | Each shadowed light gets a virtual cube of six 4096² faces (up to 16 lights). |

Consequences worth knowing:

- **Static scenes are nearly free.** Once pages are cached, a still camera re-renders almost
  nothing. Moving objects only invalidate the pages they pass through.
- **Rotating the sun re-renders everything.** A sun turn smaller than the *Light Rotation
  Threshold* (0.1°) keeps the cache, so a slowly animated time of day steps rather than
  re-rendering every frame.
- **Virtualized geometry** casts into the pages cluster by cluster, so dense scans cast detailed
  shadows cheaply.

| Setting | Meaning |
| --- | --- |
| Enabled (on) | Off falls back to a single 2K sun shadow map and cubemap shadows for lights. |
| Local Lights (on) | Shadow lights through the VSM too. Off uses the cubemap atlas. |
| Terrain Shadows (on) | Terrain casts shadows. |
| Terrain Displacement Shadows (off) | Cast from the tessellated, displaced terrain so ripples and small dunes shadow themselves. Much more expensive. |
| First Level Texel Size (0.004 m) | Sharpness of the finest sun level. |
| Level Count (13) | How far sun shadows reach. Points no level covers are unshadowed. |
| Resolution LOD Bias (0) | +1 reads half the resolution everywhere: softer shadows, a quarter of the pages. |
| Local Resolution Bias (0) | The same for lights. |
| Physical Pages (2048) | Size of the page pool. 2048 pages is 128 MB. Raise it if distant shadows go missing in huge scenes. |
| Max Pages Per Frame (160) | Rendering budget per frame. Pages waiting their turn use a coarser level meanwhile. |
| Depth Range (2000 m) | Light-space depth kept around the camera. |
| Normal Offset, Constant Bias, Slope-Scaled Depth Bias | Fight "shadow acne" (striped self-shadowing) and "peter-panning" (shadows detached from their caster). Raise the biases if you see stripes; lower them if shadows float. |
| Debug View | Tint by clipmap level, or show raw sun visibility. |
| Disable Caching | Re-render every page every frame, to compare against the cache. |

## Soft shadows

Real shadows are sharp where an object touches the ground and soften with distance, more so for
large light sources. VSM soft shadows (PCSS) do exactly that:

| Setting | Meaning |
| --- | --- |
| Soft Shadows (on) | Off uses a fixed small blur everywhere. |
| Sun Angular Diameter (0.53°) | The real sun is 0.53°. Larger gives a hazier, softer sun. |
| Local Light Softness (1) | Multiplies every light's emitter size for its shadow. |
| Min Local Light Size (0.03 m) | Size assumed for lights with a smaller Source Radius (point lights are 0), so their shadows soften a little too. 0 keeps them hard. |
| Max Penumbra (0.6 m) | The widest soft edge allowed. |

A light's shadow softness comes from its **Source Radius** (point and spot lights) or its
rectangle (rect lights). A big softbox throws soft shadows; a bare bulb throws sharper ones.

## Ray-traced shadows

On a ray-tracing GPU, shadows can be traced against the scene instead of read from a shadow map:
exact contact shadows (a candle on a table, a cup on a saucer), no acne or peter-panning to tune,
and penumbrae that widen correctly with distance from the caster. Settings are in **Graphics
Settings → Ray-Traced Shadows** and are saved with the level.

| Setting | Meaning |
| --- | --- |
| Enabled (off) | Trace the sun's shadow. |
| Local Lights (on) | Also trace point, spot and rect lights that cast shadows: up to **8 per pixel**. Any further lights on a pixel keep their shadow maps. |
| Rays Per Pixel (1) | Rays per light, per pixel, per frame. The shadows are denoised, so 1 is usually enough; raise it if wide soft shadows look grainy. |
| Sun Angular Size (0.53°) | The sun's size, as for VSM soft shadows. 0 = hard. |
| Light Softness (1) | Multiplies each light's Source Radius (or rect panel) for its shadow. 0 = hard. |
| Max Ray Length (500 m) | The sun is not shadowed by anything further away than this. |
| Ray Bias (0.01 m) | Minimum offset of each ray from the surface; it grows on its own where light grazes a surface. |
| Temporal Blend (0.1) | How fast the denoiser follows changes. Lower is smoother but lags behind moving objects. |
| Max Filter Radius (8 px) | The widest blur across the sun's soft edges. |
| Debug View | Sun Visibility, Raw Sun Rays, Local Light Visibility, Raw Local Light Rays (before denoising). |

Things to know:

- The shadow maps keep rendering, because volumetric fog, GI bounces and subsurface transmission
  use them. Ray-traced shadows replace only what the lit image reads.
- The **shadow budget** does not limit ray-traced lights. Every light with **Cast Shadows** on
  is traced, up to 8 per pixel.
- A ray stops 5 cm short of a point or spot light, so a bulb inside its own lamp mesh is not
  shadowed by the lamp.
- The real-time path tracer shadows everything itself, so ray-traced shadows stop while it runs,
  and they are off whenever shadows are switched off (`shadows.enabled`).

### How ray-traced shadows work

Every frame, each pixel sends a ray toward a random point on the light: on the sun's disc, on
a sphere the size of the light's **Source Radius**, or on a rect light's panel. If something is
in the way, that ray is in shadow. A point near the edge of a shadow sees part of the light, so
over several rays it ends up partly lit, which is the soft edge (penumbra).

One ray per frame is black or white, so the result is **denoised**:

1. **Over time.** Each pixel blends this frame's rays with what it had last frame, following the
   surface as the camera moves. Where the surface it saw is no longer there (something moved in
   front, or it just came into view), the history is dropped for that pixel.
2. **Across neighbours.** The soft edges are blurred with nearby pixels on the same surface. The
   blur never crosses an edge to a different surface. For the sun, the blur is as wide as the
   penumbra should be: wider the further the caster is from the shadow.

Each local light is denoised on its own, so two lights never mix their shadows.

Rays leave the surface slightly in front of it, along the surface's true shape rather than its
normal map, and further where the light only grazes it. A candle on a table lights the table
at a very shallow angle. Without that offset, rays would skim back into the table and speckle
it with black dots ("acne").

### Cost

Ray-traced shadows run at the render resolution. Roughly, per pixel and per frame:

| | Rays |
| --- | --- |
| Sun | 2 (one to find the surface, one toward the sun) |
| Local lights | 1, plus *Rays Per Pixel* for each traced light on that pixel (up to 8) |

Interiors with many overlapping lights cost the most. To keep them cheap, turn **Cast Shadows**
off on lights whose shadow nobody would notice, or turn *Local Lights* off and let the shadow
maps handle the lights.

### When something looks wrong

- **Grainy soft shadows, especially while moving.** Raise *Rays Per Pixel* to 2 to 4, or lower
  *Light Softness* / *Sun Angular Size* so the penumbrae are narrower.
- **Shadows lag behind a moving object.** Raise *Temporal Blend*.
- **Black speckles on flat surfaces.** Raise *Ray Bias*. Use the *Raw Sun Rays* or *Raw Local
  Light Rays* debug view to see the rays before any denoising: a flat, open surface there should
  be almost entirely white.
- **A small gap under objects resting on a surface.** Lower *Ray Bias*.
- **A light's shadow looks different from the others.** That pixel may have more than 8 lights
  on it; the extra lights use their shadow maps. Check with *Local Light Visibility*.

## The shadow budget

Shadows are the expensive part of a light. With many shadow-casting lights, Ptero only shadows
the ones that matter most right now and lets the rest light without shadows:

| Setting | Meaning |
| --- | --- |
| Shadow Budget (on) | Off shadows every shadow-casting light. |
| Max Shadowed Lights (8) | Lights ranked by brightness over squared distance, ignoring lights out of view. |
| Shadow Fade Seconds (0.4) | A light entering or leaving the set fades its shadow in or out, and lights already shadowed are favoured, so the set does not flicker. |
| Importance Resolution (on), Full Resolution Lights (4), Reduced Resolution Bias (1) | Beyond the top 4, lights use half-resolution shadows. |
| Cheap Distant Filtering (on), Cheap Filter Distance (20 m) | Use a cheaper shadow filter for far receivers and the reduced lights. |

Tips:

- Turn **Cast Shadows** off on fill lights, small decorative lights and anything whose shadow
  nobody would notice. It is the biggest saving there is.
- Fire particles' stand-in lights default to no shadows; turn them on only for a hero fire.

## Point Light Shadows (fallback)

When VSM is off (or *Local Lights* is off), lights use cubemap shadows: Map Size (1024), Bias,
Slope-Scaled Depth Bias, Normal Offset, Seam Blend Distance, Filter Radius.

## Contact shadows

The shadow map cannot resolve the tiniest gaps (a cup on a table, a foot on the floor). DPLE's
**Contact Shadows** add short screen-space sun shadows for those. See
[7. Global illumination](07-Global-Illumination.md#dple).

## Shadows in GI

The GI systems ask the virtual shadow map whether a bounce point is in sunlight or lamplight
(*Shadow Map Visibility*) and only trace a ray where the map is too coarse. That is why VSM
being on also makes GI cheaper.
