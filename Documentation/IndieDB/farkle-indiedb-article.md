# IndieDB article: Farkle: Tavern Dice

## Title

Farkle: Tavern Dice is out: a new engine, 48 achievements and 16 languages

## Summary (article teaser)

Farkle: Tavern Dice is now on Steam. Since launch the game has moved to a new version of our own Ptero-Engine, with virtualized geometry, virtual shadow maps, voxel GI, path tracing and two reconstruction techniques of our own, plus 48 Steam Achievements. The game is now also playable in 16 languages. Next up: multiplayer, progression and more music.

## Body

Paste the contents of `farkle-indiedb-article.html` into the IndieDB editor's source (HTML) view. The text is repeated below for reading and review.

---

Greetings, travellers!

**Farkle: Tavern Dice** came out on Steam on **21 October 2026**. It's the classic push-your-luck dice game, played by candlelight in a medieval tavern. A lot has happened at the table since then, so here's where things stand: the release, the engine update that followed it, the game's new languages, and what comes next.

### The tavern is open

Every turn starts with six dice. Roll, set aside the scoring dice, and decide whether to bank your points or push your luck with the rest. Roll nothing that scores and you **Farkle**, losing everything from that turn. First to 3,000 points wins (you can change the target in the settings). The rules are simple. The decisions are not.

- A detailed 3D medieval tavern with firelight, wooden beams, hanging garlic, physical dice and a crackling hearth
- Authentic medieval music from the period itself
- A computer opponent that will happily take your points
- Full graphics, audio and game settings, from low-end to path-traced

Farkle is built entirely in **Ptero-Engine**, our own DirectX 12 engine, from the renderer and lighting to the editor and visual scripting. Shipping a real game on Steam with it means a lot to us.

### A new engine foundation

The first item on our roadmap is already done. Farkle now runs on a new version of Ptero-Engine, and the tavern has been rebuilt with it:

- **Virtualized geometry**: models stream at the detail your screen can actually show, so close-ups stay crisp without detail levels popping
- **Virtual shadow maps**: much sharper, more stable shadows from every candle and from the fire, right down to the dice
- **Better global illumination**: improved RTGI and probe lighting with the old probe caching artifacts fixed. Virtualized geometry and virtual shadow maps are now lit correctly by GI
- **Voxel GI**: a new global illumination mode that **does not need a ray tracing GPU**, so more players get bounced light
- **Path tracing**, for the tavern at its most beautiful
- **New bloom and lens flares**, so firelight and candles glow the way a camera would see them
- **Intel XeSS** alongside DLSS and FSR, plus **frame generation** with FSR or XeSS on supported systems
- Refined volumetric fog, faster level loading, and a long list of smaller lighting fixes

#### Ptero-SR and Ptero-AA

We also built two techniques of our own. Most upscalers only see colours: when a triangle edge falls halfway across a low-resolution pixel, it becomes an in-between colour that can only be blurred or sharpened back afterwards, often with halos.

**Ptero Reconstruction** works from the geometry instead. During rendering the engine keeps the exact triangle edges inside each pixel, so every output pixel on an edge gets its exact coverage. Colours are rebuilt only from samples on the same side of the edge and on the same surface, so they never bleed into each other.

- **Ptero-SR** is a spatial upscaler (Native, Quality and Balanced). It produces clean, stable edges at full resolution with no ghosting from previous frames.
- **Ptero-AA** is native-resolution anti-aliasing that uses the same exact edge coverage, with no temporal pass, for a sharp, smear-free image.

Both need a GPU with Shader Model 6.1. Without it the options are hidden, and DLSS, FSR, XeSS, TAA, SMAA and MSAA are all still available.

#### 48 Steam Achievements

The update also added **48 Steam Achievements**. Some are earned at the table, like rolling a straight, scoring six of a kind or winning after trailing by 1,500. Some reward the nights that go wrong, like farkling on your very first roll. Others take the long game (100 matches, 10 wins in a row) or the graphics menu (win on all-Low, then on Ultra with path tracing). A few are hidden. Some will find you late at night.

#### Under the hood

For the engine-minded among you, this generation of Ptero-Engine also brings:

- Subsurface scattering (with an experimental ray-traced mode) and stochastic screen-space reflections
- Stars and a moon in the night sky, and exposure (EV) control across the time of day
- A new editor GUI with support for custom GUI designs
- An RmlUi-based game UI, which is what all of Farkle's menus are built with
- New Node Graph nodes that came straight out of making Farkle
- Game packaging and WebM video playback
- UDIM textures tracked in level loading

### A tavern in 16 languages

We want everyone to feel at home at the table, so localization has been our main focus since the engine update. It's now live on Steam: Farkle: Tavern Dice can be played in **16 languages**:

English, German, French, Italian, Spanish, Portuguese (Brazil), Portuguese (Portugal), Polish, Czech, Ukrainian, Greek, Turkish, Arabic, Simplified Chinese, Japanese and Korean.

This needed some engine work, not only translation files:

- Every menu, the HUD, the turn messages and the result screens are translated as the UI is built. Numbers follow your language's digit grouping
- The game picks its language automatically: first your Steam language for the game, then your Windows language. You can change it at any time in Settings → Game
- Font fallback covers Greek, Cyrillic, Arabic, Chinese, Japanese and Korean, so no text turns into empty boxes
- Arabic is shaped and laid out right-to-left in the engine

Our website, [farkle.pterosoftstudio.com](https://farkle.pterosoftstudio.com/), including the news, the roadmap and a new press kit, is already available in all 16 languages.

The translations haven't been reviewed by native speakers yet. If you play in one of these languages and something reads oddly, please tell us in the [Steam discussions](https://steamcommunity.com/app/5368450/discussions/). We'll fix it.

### The road ahead

With the engine and languages done, here's what's next:

1. **Company at the table**: leaderboards and multiplayer, so you can play against friends and rivals as well as the computer.
2. **Something to work toward**: a progression system and an in-game store, with goals to chase across many nights at the tavern.
3. **More music by the fire**: music extensions that build on the authentic medieval soundtrack already playing by the hearth.

We'll keep sharing progress here, on Steam and on our website as each one takes shape.

### Pull up a chair

If you've already played, thank you. A **Steam review** helps a small indie release like ours more than you might think. If you haven't, the table is waiting.

- Steam: [store.steampowered.com/app/5368450](https://store.steampowered.com/app/5368450/)
- Website and press kit: [farkle.pterosoftstudio.com](https://farkle.pterosoftstudio.com/)
- Feedback and bug reports: [Steam Community discussions](https://steamcommunity.com/app/5368450/discussions/)

Fortune favours one more roll.

— Pterosoft Studio
