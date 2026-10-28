# Manual excerpts

One excerpt per manual page, for the website. On the Ptero WordPress theme a documentation
page's **Excerpt** is shown twice: as the introduction under the page title, and as the text of
the page's card on its parent page. Pages without one fall back to the first 22 words of the page,
which is usually a heading or a table, so every page should have one.

To publish a page: create a WordPress Page with the **Title** below, set **Parent** to the
Documentation page, paste the **Excerpt** into the page's Excerpt box, and set **Page Attributes →
Order** to the **Order** below so the sidebar and previous/next links follow the manual.

## Rules for excerpts

- **Plain text only.** The theme prints it as plain text, so no Markdown, links, bold or
  backticks. Write menu paths as words ("Windows, then Console"), not with arrows.
- **20 to 35 words**, one or two sentences. The full excerpt shows on the card; anything longer
  makes the cards uneven.
- **Say what the reader will be able to do**, not that the page "describes" something. Start
  with the subject ("How...", "Every...", "Build...").
- Do not repeat the title.
- **Every new page gets an excerpt here in the same change that adds the page**, with the next
  free Order number in its section.

Long chapters also get **section excerpts**: one short summary per main (`##`) section, for
section cards, search results, social previews or a "what's in this chapter" list. They follow
the same rules, but are **12 to 30 words**. Once a chapter has section excerpts, every `##`
section in it needs one, so adding a section means adding its excerpt.

`node Documentation/Tools/CheckExcerpts.mjs` checks all of this: a missing excerpt, a page or
section that no longer exists, a section without an excerpt, the word count, stray Markdown and
duplicate Order numbers.

## Excerpts

### Documentation (the parent page)

- **Source:** `README.md`
- **Title:** Documentation
- **Order:** 0
- **Excerpt:** The Ptero Engine manual: how the engine and editor work, how every feature works, step-by-step tutorials, and a reference for every setting. No programming needed.

### Start here

| Order | Source | Title | Excerpt |
| --- | --- | --- | --- |
| 1 | `00-Getting-Started.md` | Getting started | What Ptero is and which games it suits, the hardware you need and what changes without ray tracing, how to install and launch the editor, and which sample levels to open first. |
| 2 | `Tutorial-1-First-Level.md` | Tutorial 1: your first level | Build a small scene in twenty minutes: a floor, a few objects and a lamp, an evening sky from the time of day, then walk around it with Play and save it. |
| 3 | `Tutorial-2-First-Person-Door.md` | Tutorial 2: a first-person level with a door | Build a room the player can walk around and write your first Node Graph script: look at the door, press E, and it slides open. |

### Manual chapters

| Order | Source | Title | Excerpt |
| --- | --- | --- | --- |
| 10 | `01-How-Ptero-Works.md` | How Ptero works | The big picture behind everything else: the editor and the game, the Data folder, levels, entities and components, units and axes, what happens every frame, and where settings are saved. |
| 11 | `02-The-Editor.md` | The editor | Find your way around the editor: the panels and docking, flying the viewport camera, placing, selecting and moving things, undo, levels, view modes, screenshots, the Console and every shortcut. |
| 12 | `03-Assets-And-Importing.md` | Assets and importing | Bring models, textures, Unreal Engine 5 assets, Substance Painter exports, sound and video into your project, learn what the importers do with them, and keep your assets within budget. |
| 13 | `04-Components.md` | Components | Every component an entity can have, from meshes and lights to particles, decals and rigid bodies, with what each property does and the value it starts with. |
| 14 | `05-Materials.md` | Materials | Make surfaces look real with physically based materials and the Disney principled BRDF: colour, roughness and metal, clearcoat and sheen, glass, parallax, tessellation and subsurface scattering. |
| 15 | `06-Lighting.md` | Lighting, sky and time of day | Light scenes in real-world units with point, spot and area lights, animate them with light styles, and drive the sun, sky, moon, stars and exposure from the time of day. |
| 16 | `07-Global-Illumination.md` | Global illumination, reflections and AO | How bounced light works in Ptero and which system to choose: ray-traced GI, radiance probes or voxel GI, plus reflections, ambient occlusion, subsurface scattering and DPLE. |
| 17 | `08-Shadows.md` | Shadows | How virtual shadow maps keep shadows sharp from your feet to the horizon, how soft shadows follow each light's size, how the shadow budget keeps many lights fast, and when to ray trace shadows instead. |
| 18 | `09-Outdoor-Worlds.md` | Outdoor worlds | Build landscapes and weather: sculpt and paint terrain, add oceans and lakes, grow forests with rule-based vegetation, and fill the air with wind, rain, volumetric fog and clouds. |
| 19 | `10-Image-Quality.md` | Image quality and post-processing | Get a clean, fast image: anti-aliasing, DLSS, FSR and XeSS upscaling, frame generation, AgX tonemapping and colour grading, bloom, lens flares, the path tracer and a performance checklist. |
| 20 | `11-Gameplay.md` | Gameplay | Turn a level into a game: play sessions, the first-person character, visual scripting with the Node Graph, physics, game interfaces, sound and video. |
| 21 | `12-Cinematics.md` | Cinematics | Animate cameras, lights and objects on a timeline with the Sequencer, cut between cameras, fire events, and render your shots to images or MP4 video. |
| 22 | `13-Shipping.md` | Shipping a game | Turn your project into a game you can hand out: Game Settings, Build Game, encrypted packages and the packaging key, the player's options menu, and publishing on Steam. |

### Reference

| Order | Source | Title | Excerpt |
| --- | --- | --- | --- |
| 30 | `CVar-Reference.md` | CVar reference | All 552 console settings in one place, grouped by feature, with what each one does, the values it accepts and the value it starts with. |
| 31 | `Glossary.md` | Glossary | Short, plain definitions of the terms used throughout the manual, from albedo and cvars to virtual shadow maps and Z-up. |
| 32 | `FAQ.md` | FAQ and known issues | Answers to the problems people run into most, from washed-out textures and dark metals to noisy GI and missing models, plus the engine's current limitations. |

Orders leave gaps (1–3, 10–22, 30–32) so new pages can slot into a section without renumbering
the rest.

### Subtopics

A subtopic is an in-depth page that belongs under a chapter. In WordPress, set its **Parent** to
that chapter's page (not to Documentation), so it appears as a topic card at the bottom of the
chapter and nested under it in the sidebar. **Order** counts from 1 within each parent. Name the
file after its parent with a letter: `05a-...md`, `05b-...md`.

| Order | Source | Title | Parent | Excerpt |
| --- | --- | --- | --- | --- |
| 1 | `05a-Disney-Principled-BRDF.md` | The Disney principled BRDF | `05-Materials.md` | What a BRDF is and why Ptero uses Disney's, what every lobe does to a surface, the exact formulas behind it, and values to start common materials from. |

## Section excerpts

### 3. Assets and importing

| Section | Title | Excerpt |
| --- | --- | --- |
| `03-Assets-And-Importing.md#the-asset-browser` | The Asset Browser | Browse the Data folder, import files into any folder, and rename, copy or remove them. Remove deletes permanently, so keep backups. |
| `03-Assets-And-Importing.md#files-that-appear-in-data-on-their-own` | Files that appear in Data on their own | Copy files straight into Data from Explorer, a Fab download or Substance Painter, and the editor offers to convert them in place. |
| `03-Assets-And-Importing.md#models-fbx` | Models (FBX) | What happens when you import an FBX, how to export models that arrive at the right size and orientation, and how to fix ones that don't. |
| `03-Assets-And-Importing.md#lods` | LODs | How the engine generates simpler versions of every model, when they switch in, and how to rebuild or tune them per entity. |
| `03-Assets-And-Importing.md#collisions` | Collisions | Why only moving physics objects need generated collision, how convex decomposition builds it, and how to check the result in the viewport. |
| `03-Assets-And-Importing.md#textures` | Textures | Supported image formats, how file names decide colour, normal or data compression, packed roughness and metal maps, and UDIM tile sets. |
| `03-Assets-And-Importing.md#unreal-engine-5-assets` | Unreal Engine 5 assets | Import meshes, textures and materials from Unreal Engine 5 and Fab packs directly, what you need installed, and what does not come across. |
| `03-Assets-And-Importing.md#substance-3d-painter` | Substance 3D Painter | Install the Ptero shader and export plugin, preview the engine's materials inside Painter, and export packed textures with a ready-made material. |
| `03-Assets-And-Importing.md#audio-fmod` | Audio (FMOD) | Author sound events in FMOD Studio, build them into banks the engine loads, and set up positional sound with Resonance Audio. |
| `03-Assets-And-Importing.md#video` | Video | Which video formats play, how other formats are converted to WebM on import, and how to prepare video yourself. |
| `03-Assets-And-Importing.md#asset-budgets` | Asset budgets | The engine's hard limits, plus suggested texture sizes, triangle counts and light rules for a level that stays fast. |
| `03-Assets-And-Importing.md#file-types-you-will-see` | File types you will see | A quick reference to every file extension in a Ptero project and what each one holds. |

### 4. Components

| Section | Title | Excerpt |
| --- | --- | --- |
| `04-Components.md#transform` | Transform | Position, rotation and scale: the three values every entity has, in metres and degrees, with Z pointing up. |
| `04-Components.md#mesh` | Mesh | Draw a model with a material, tune when it switches to simpler LODs, and switch dense meshes to virtualized geometry. |
| `04-Components.md#light` | Light | Point, spot and rectangular area lights in real units, with soft shadows, fog and GI controls, and animated light styles for flames and flicker. |
| `04-Components.md#camera` | Camera | Place cameras with real lens settings for cinematics, look through them in the viewport, and add procedural handheld shake. |
| `04-Components.md#audio-emitter` | Audio Emitter | Play an FMOD sound event from an entity's position, on level start or when a script asks for it. |
| `04-Components.md#decal` | Decal | Project dirt, stains, signs and cracks onto whatever surfaces lie inside a box, using a decal material. |
| `04-Components.md#rigid-body` | Rigid Body | Make an entity fall, bounce and get pushed during play, or move it from scripts, with shapes, mass and friction. |
| `04-Components.md#particle-system` | Particle System | GPU particle effects for fire, smoke and sparks, authored once in the Particle Editor and placed anywhere, with light that flickers with the flame. |
| `04-Components.md#rain` | Rain | Rain streaks that follow the camera, blown by the scene wind, with control over their look and how wet surfaces become. |
| `04-Components.md#terrain-water-and-vegetation-area` | Terrain, Water and Vegetation Area | Where to find the landscape, water and vegetation components, which have a chapter of their own. |
| `04-Components.md#special-entity-names` | Special entity names | The entity names the engine looks for, such as PlayerStart, which decides where the player appears when you press Play. |

### The Disney principled BRDF (subtopic of 5. Materials)

| Section | Title | Excerpt |
| --- | --- | --- |
| `05a-Disney-Principled-BRDF.md#what-a-brdf-is` | What a BRDF is | How light reflects off and scatters inside a surface, and how one formula captures matte, glossy and metal surfaces alike. |
| `05a-Disney-Principled-BRDF.md#why-principled` | Why "principled" | The five design principles Disney's artists asked for, and why the model became the basis of modern physically based rendering. |
| `05a-Disney-Principled-BRDF.md#the-parameters` | The parameters | The four base parameters every surface has, the seven optional ones, and what the Specular value means in real reflectance. |
| `05a-Disney-Principled-BRDF.md#the-lobes` | The lobes | Diffuse, subsurface flatten, specular, specular tint, anisotropy, sheen and clearcoat: what each kind of reflection looks like and when to use it. |
| `05a-Disney-Principled-BRDF.md#how-the-parameters-work-together` | How the parameters work together | A single table showing which parameter affects which lobe, and why a material that ignores the extra lobes costs nothing. |
| `05a-Disney-Principled-BRDF.md#where-each-lobe-applies` | Where each lobe applies | Which lobes the sun and lights, sky ambient, reflections, global illumination and the path tracer each take into account. |
| `05a-Disney-Principled-BRDF.md#how-ptero-computes-it` | How Ptero computes it | The exact formulas for every lobe, as written in the engine's shader, and the two conventions that differ between real time and path tracing. |
| `05a-Disney-Principled-BRDF.md#differences-from-disneys-original` | Differences from Disney's original | The few places Ptero departs from the 2012 model, such as Specular scaling metals, and the reason for each. |
| `05a-Disney-Principled-BRDF.md#the-original-model` | The original model | The simpler shading model a level can switch back to, what it leaves out, and why new work should not need it. |
| `05a-Disney-Principled-BRDF.md#authoring-guide` | Authoring guide | Starting values for snow, wood, skin, metals and more, when to reach for each extra lobe, and the mistakes that make materials look wrong. |
| `05a-Disney-Principled-BRDF.md#further-reading` | Further reading | The papers the model comes from, for anyone who wants the original derivations. |
