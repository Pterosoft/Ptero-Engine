# 6. Lighting, sky and time of day

## How light is measured

Ptero lights scenes with real-world units, then turns the result into an image the way a camera
does: with an **exposure**.

| Source | Unit | Reference values |
| --- | --- | --- |
| Point, spot, rect lights | **lumens** (total output) | 450 lm candle-ish bulb, 800 lm 60 W bulb, 1600 lm 100 W bulb, 5000+ lm floodlight |
| Sun | **lux** (light arriving per m²) | 100 000–120 000 lx clear noon |
| Sky ambient | **lux** | 20 000 lx clear blue sky; 30 lx at night (a game value) |
| Moon | **lux** | 5000 lx in-engine for a full moon (a game value; the real one is 0.25 lx) |
| Exposure | **EV100** | Higher = darker image. Ptero's default is -4 |

The renderer works in *scene-linear* units: the sun's 100 000 lx and the sky's 20 000 lx map to
1.0, and local lights are scaled against an 800 lm reference. This means **EV100 numbers here
are offset from a photographer's**: a sunny day is about -2 EV100 in Ptero, not +15. Use the
engine's own numbers (below) rather than a light meter's.

## Local lights

Point, spot and rect lights are described property by property in
[4. Components](04-Components.md#light). The things that matter for lighting design:

- **Radius is cost.** Each light only touches pixels within its radius. Big radii on many lights
  is what makes a scene slow. Make each one just big enough.
- **Many lights are fine.** Up to 256 lights can shade surfaces at once: the screen is split into
  clusters and each pixel only evaluates the lights whose radius reaches it. Volumetric fog and
  voxel GI use the 16 most important lights at any moment (brightness over distance, in view).
- **Shadows have a budget.** Only the shadow-casting lights that matter most to the view cast
  shadows at once. See [8. Shadows](08-Shadows.md#the-shadow-budget). Ray-traced shadows, if
  you turn them on, are not budgeted: see [8. Shadows](08-Shadows.md#ray-traced-shadows).
- **Rect lights for anything wide.** A window, a strip light or a softbox lights a room the way
  it really does: the highlight wraps around surfaces instead of pinching to a dot.
- **Source radius for soft shadows.** A non-zero Source Radius makes a light's shadows soften
  with distance from the caster, like a real bulb.
- **Temperature for believable colour.** 2700 K tungsten, 4000 K neutral white, 6500 K daylight.
- **Light styles** animate brightness (fire, candle, strobe, fluorescent). The animation reaches
  direct light, the GI bounce and fog alike.
- **GI per light.** *Affect Global Illumination* off makes a light purely direct, for cinematic
  key and rim lights.

## Time of Day

**Windows → Time of Day...** controls the sun, sky, moon, stars, exposure and wind. It is
saved with the level. **Revert All** returns to the defaults.

**Enable Time Of Day** off removes the sun, sky light, procedural sky and clouds entirely, so the
scene is lit only by the lights you place. Use it for interiors with no windows and for fully
artificial sets.

### Time and place

| Setting | Meaning |
| --- | --- |
| Time Of Day (12.0) | Hours, 0–24. |
| Latitude (30°) | How high the sun gets. The sun is modelled at an equinox: it rises due east at 06:00 and sets due west at 18:00, and its noon height is 90° minus the latitude. |
| North Offset (0°) | Rotates the whole sky (sun path, moon, stars) around the vertical. At 0 the sun rises towards +X. |

### Atmosphere and sky

The sky is the **Hosek-Wilkie** physical sky model: its colour comes from the sun's position and
the air's clarity, so sunsets and blue hours come out right on their own.

| Setting | Meaning |
| --- | --- |
| Turbidity (2.5) | Haze. 2 = very clear, 5 = hazy, 10 = heavy haze. |
| Ground Albedo (0.1) | How much the ground brightens the horizon. |
| Sun Intensity (120 000 lx) | Direct sunlight. |
| Sky Intensity (20 000 lx) | Ambient light from the sky, at full daylight. It fades through twilight to the night value. |
| Override Sun Colour / Override Sky Colour | Replace the physically computed colours with your own. |
| Sun Glare | The halo and diffraction spikes around the sun (below). |

### Night

| Setting | Meaning |
| --- | --- |
| Night Sky Intensity (30 lx), Night Sky Colour | Ambient light once the sun is well down. Dark but readable. |
| Night Desaturation (0.6) | Dark parts of the frame lose colour at night, as the eye's rods take over. Lights and the moon keep theirs. Fades in from sunset, full at 8° below the horizon. |
| Moon (on) | The moon becomes the directional light while the sun is down. |
| Moon Phase (0.5) | 0 = new, 0.25 = first quarter, 0.5 = full, 0.75 = last quarter. Also sets where it is: a full moon rises at sunset. |
| Moon Intensity (5000 lx), Moon Colour | How much it lights the scene (scaled by the phase). |
| Moon Size (1), Moon Disc Brightness (4) | How big and how bright the disc looks, separate from how much light it gives. |
| Moonlit Sky (0.25) | The blue glow the moon gives the night sky. |
| Moon Glare | Soft glow around the moon. |
| Stars (on), Star Intensity (1) | |
| Star Field | **Procedural**: pin-sharp twinkling stars and a noise Milky Way. **Star map**: the real sky from a 2K texture (soft up close). **Both**: map for the Milky Way, procedural for the sharp points. |

### Exposure

| Setting | Meaning |
| --- | --- |
| **Time of Day Controls Exposure** (on) | The time of day picks the exposure: Day EV100 with the sun high, Sunset EV100 with it on the horizon, Night EV100 once it is 12° below. |
| Day / Sunset / Night EV100 (-2 / -3.5 / -5.25) | The three anchor exposures. Tuned on the Desert level. |
| **Eye Adaptation** (on) | Measures what the camera sees and compares it with what an open outdoor view would be at this hour. Walk into a dark interior by day and the image opens up; into a torch-lit tomb at night and it stops down. Differences under one stop are ignored, so outdoors stays on the curve. |
| Adaptation Strength (0.8) | How much of the measured difference is followed. |
| Adaptation Range (-7.5 to 1 EV100) | The brightest a dark interior may open up to and the darkest a bright one may stop down to. Keep it wider than the Day/Sunset/Night values; the window warns when it is not. |

With *Time of Day Controls Exposure* off, exposure comes from the **AgX Tonemapper** settings in
Graphics Settings (manual EV100, or histogram auto-exposure). See
[10. Image quality](10-Image-Quality.md#tonemapping-and-exposure). The AgX *Exposure* trim
applies on top either way.

### Wind

Scene wind, shared by everything that reacts to it (vegetation, rain, particles, clouds), so
nothing can blow the wrong way.

| Setting | Meaning |
| --- | --- |
| Enable Wind | |
| Direction | Compass heading. |
| Strength (4 m/s) | 2 = leaves barely move, 8 = branches in motion, 15 = whole trees swaying. |
| Gust Amplitude (2.5 m/s), Gust Frequency (0.25 Hz) | Extra speed from gusts, and how often they pulse. |
| Gust Wavelength (40 m) | Size of a gust. Large values look like a breeze crossing a meadow; small ones make the whole field twitch at once. |
| Vegetation Bend Scale (1) | Dial all vegetation motion up or down at once. |
| Flutter Frequency (2.2 Hz) | Speed of the leaf flutter. |

## Sun and moon glare

The sun disc is drawn just bright enough to clip to white. The **glare** around it (the light
the eye and the lens scatter around a very bright source) is a separate pass computed from the
sun's true energy, so it can be tuned without touching bloom. The moon gets a softer, wider
glow with no spikes.

| Setting | Meaning |
| --- | --- |
| Enabled, Intensity (0.03 sun / 0.08 moon) | Share of the light spread into glare. |
| Halo Core, Halo Radius, Halo Falloff | Size of the bright centre, how far the halo reaches (degrees), and how quickly it fades. |
| Streak Strength, Streak Length | The aperture's diffraction spikes. |
| Aperture Blades, Aperture Rotation | Spike count and angle. An even number of blades gives that many spikes; an odd number twice as many. |
| Chromatic Spread | Red reaches further than blue, tinting the glare's edge warm. |

## Lighting recipes

**Sunny exterior.** Time of Day on, time 10–15, latitude 30–45, Exposure controlled by time of
day. Add RTGI (or voxel GI) for bounce light. Raise turbidity for haze.

**Golden hour.** Time 7 or 17.5, turbidity 3–4. Volumetric fog with low density and a small
height falloff gives sun shafts.

**Moonlit night.** Time 23, moon on, phase 0.5, stars on. Night Desaturation 0.5–0.7. Add
practical lights (lamps, torches with Fire style) to give the eye something warm.

**Interior with windows.** Keep time of day on so the sun and sky come through the windows;
use RTGI or radiance probes so the light bounces into the room. Put rect lights in the window
frames if the room is too dark. Eye adaptation opens the exposure up indoors.

**Windowless interior.** Time of Day off. Light only with placed lights; a GI system is what
fills the shadows. Set exposure manually in the AgX settings.
