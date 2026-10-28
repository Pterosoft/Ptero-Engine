# Player menus and HUD (September 2026)

The following native RmlUi documents use the same bundled fonts and skin atlas as the existing HUD. Every document has transparent scene margins; there is no tavern background or fullscreen dimmer.

| Document | Purpose |
| --- | --- |
| `menu.rml` | Play, Settings, Credits, Exit; no save slots, Continue, or Load Game |
| `settings-graphics.rml` | Resolution; fullscreen/windowed/borderless; texture, model, GI quality; GI method (level / RTGI / probe / voxel); virtual geometry; path tracing (Off / Real Time / Reference); ambient occlusion, reflections, subsurface scattering, volumetric fog and shadows (each can be Off) |
| `settings-image.rml` | TAA/MSAA/SMAA; None/DLSS/FSR with quality and sharpening; frame generation; bloom and lens flares (each can be Off); chromatic aberration |
| `settings-sound.rml` | Overall, sound, and music volume only |
| `settings-game.rml` | Custom winning score (default 3000), English language, FPS/latency overlay |
| `credits.rml` | Engine, UI library, and font credits |
| `shader-preload.rml` | Standalone startup, first screen: the shader cache warm-up with a progress bar, entry count and the shader in hand |
| `loading.rml` | Standalone startup, second screen: reading and streaming the level, then the warm-up frames; one of its `tip-N` lines is shown at random |
| `game.rml` | Player HUD: compact score card, scoring reference, larger dice, selected dice and actions; no testing log or editor exit label |
| `farkle.rml`, `menu-editor.rml` | Preserved editor/testing versions |

`menus.rcss` and `game.rcss` extend the existing `farkle.rcss` without changing the original HUD or result screens. The layouts are designed for landscape screens and validated at 1280×720 and 1600×900. The preview PNGs in `Preview/` have real alpha; black in an image viewer represents transparency.

## Behaviour and integration

`FarkleMenuController.h` is a small native C++ controller included by `RmlUiRenderer`. It runs in the UI editor as well as the game. It handles click/keyboard navigation, editable draft values across categories, per-page Reset, Apply, Back (discard unapplied edits), slider readouts, and positive integer score validation (1–9,999,999). Choosing DLSS or FSR disables the AA control immediately. Returning to None restores the previous AA selection. FSR frame generation is independent of the chosen upscaler.

Navigation is queued and loaded outside DOM event dispatch. The renderer attaches/detaches the controller with each document. Applied settings are saved to `%LOCALAPPDATA%\Ptero-Engine\Farkle\Settings.ini` and read back at the start of every play session.

**Two hosts, two UIs.** The standalone game (`Editor.exe --game <level>`) starts at `menu.rml`; Play and rematches use `game.rml`. Play from the editor is the testing build: it starts at `menu-editor.rml` and uses `farkle.rml` (with its log). The game module picks by `GameServices::Standalone`; Game API is **5**, so rebuild Game and Renderer_DX12 together. The winning score (from the saved settings, in both hosts) is read once at match start and used by the win condition, opponent decisions, help, result summary, and target label. Exit still uses the host's existing stop behaviour (return to the editor when hosted there).

**What Apply does** (`Source/Renderer_DX12/GameSettings.cpp`). Quality tiers are relative to what the level authored: High is the level as tuned, and the other tiers scale cost, never the look (fog density/colour, GI intensity and the like stay the level's).

| Setting | Effect |
| --- | --- |
| Resolution | Listed from the monitor's own display modes (landscape, ≥1280×720). Sizes the window's client area in Windowed; switches the monitor to it in Fullscreen. |
| Display Mode | Windowed / Borderless (over the monitor) / Fullscreen (monitor mode switch, restored on exit; falls back to Borderless if refused). F11 still toggles window ↔ borderless. |
| Texture Quality | Extra mip bias on top of texture sharpening: Low +2, Medium +1, High 0, Ultra −0.5. |
| Model Quality | Scales LOD switch distances: Low 2×, Medium 1.5×, High 1×, Ultra 0.6×. For virtualized meshes it scales the allowed cluster error instead: 4× / 2× / 1× / 0.5× the level's pixel threshold. |
| Virtual Geometry | On keeps the level's virtualized geometry; Off draws those meshes with their authored LODs. The editor's viewing aids (virtualize all, debug views, frozen culling) are always off in play. |
| GI Quality | Off disables GI (RTGI, probe GI and voxel GI, including voxel GI's no-ray-tracing fallback). Tiers scale Radiance Cascades rays/probe spacing, RTGI rays/bounces, probe GI rays per probe (½× / ¾× / 1× / 2×) or voxel GI resolution/cones, whichever runs. A level without GI stays without it. |
| GI Method | Level Default (whatever the level authored) / RTGI / Probe GI / Voxel GI. The chosen method is switched on and the other two off; it uses the level's own settings for that method. RTGI and Probe GI are only offered with DXR, and a method whose renderer failed to start is not offered. Inactive while GI is Off or path tracing is on. |
| Path Tracing | Off / Real Time / Reference; only Off is offered without DXR 1.1. Real Time replaces the GI with NRD-denoised paths and holds up in motion; Reference path traces the whole lit scene and converges only while the camera is still. GI Quality is inactive while either is on. Bounces and samples are the path tracer's own defaults. |
| Ambient Occlusion | Off disables the level's GTAO/RTAO. Low/Medium/Ultra set GTAO quality 0/1/3; High is the level's own. |
| Reflections | Off removes the level's screen-space reflections. Tiers scale the trace budget of the technique the level uses (ray-march steps, or FidelityFX SSSR traversal steps, start mip and rays per quad). A level without SSR stays without. |
| Subsurface Scattering | Off renders subsurface materials as plain diffuse. Low: screen space, 11 taps. Medium: screen space, 17 taps (25 if the level ray traces). High: the level's own. Ultra: 25 taps, at least 16 ray-traced probes. Never switches a level to ray tracing. |
| Volumetric Fog | Off removes the fog. The tiers change froxel tile size and depth slices only; whether the level has fog stays the level's. |
| Shadows | Off: nothing casts (sun map cleared, no point-light cubes). Tiers: point-light cube size / filter radius 512/1, 1024/1, level default, 2048/3. |
| Anti-Aliasing | TAA, SMAA or MSAA. Inactive while an upscaler is chosen (TAA and MSAA are then off). |
| Upscaler | None / DLSS / FSR / XeSS / TAA Upsampling / AAR, listing only what the GPU and installed runtimes support. AAR (Analytic-Aware Reconstruction) is the engine's spatial upscaler guided by the triangle edges; it needs Shader Model 6.1 barycentrics. |
| Upscaler Quality | Native (AA only) / Quality / Balanced / Performance / Ultra Performance, for DLSS and FSR alike. AAR maps them to 100 % / 75 % / 66.7 %, and stops at Balanced. |
| Upscaler Sharpening | Off / Low / Medium / High (0 / 0.25 / 0.5 / 0.8). FSR applies it through RCAS; after DLSS the engine's image sharpen pass does. AAR ignores it: sharpening would halo the edges it reconstructs exactly. |
| Frame Generation | FSR frame generation; only offered when the FidelityFX frame-generation API is available. |
| Bloom | Off removes it. Low uses the mip chain (≤4 mips) even on an FFT level; Medium caps the FFT grid at 256; High is the level's own; Ultra uses a 1024 FFT grid (or one more mip). A level without bloom stays without. |
| Lens Flares | Off removes them. Low: ¼ of the ghosts, 16-ray grid, monochrome, one light at a time. Medium: ½ the ghosts, 24-ray grid, ≤2 lights. High: the level's own. Ultra: 1.5× ray grid, 6 wavelengths. A level without flares stays without. |
| Chromatic Aberration | On is the level's own look; Off removes it. |
| Show FPS & Latency | Overlay (`Data/UI/stats-overlay.rml`): FPS and frame time averaged over 0.25 s, and render latency - frame start (input sampled) to GPU completion, from GPU timestamps. |
| Volumes | Overall = FMOD master bus; Music = the music channel; Sound = every other event instance. The slider value is squared into gain. |

**Startup screens** (`DX12RendererAPI.cpp`, `loading.rcss`). The standalone game no longer warms the shader cache under the splash. After the renderer is up it shows `shader-preload.rml` and compiles (or loads from `Cache/Shaders`) about 30 ms of entries per frame. It then switches to `loading.rml` and reads the level on the editor's worker thread (`BeginLoadSceneFromFile`). Meshes and textures stream in time-boxed slices, and the warm-up frames run until the level renders smoothly. Reading and streaming fill the first 85 % of the bar and warm-up the rest; the bar never moves backwards. `StartGame` closes the screen before the intro or the game's own UI. The host fills `progress-fill` (width), `progress-percent`, `progress-status` and `progress-detail`, so a restyled screen must keep those ids. Both screens are opaque: the scene is held back until the game starts, so `verify-menus.exe` reports its transparent-margin check as failed on them by design.

Settings are saved to `settings.cfg` in the game's own folder (next to `game.cfg`; `Binaries/` for editor play sessions).

Anything the system cannot honour is corrected, the page shows the corrected value, and the status line says why. Stopping play restores every renderer and audio setting the session changed, so the editor never inherits a preset.

For another host, keep one controller instance, call `Attach(document)` after loading, `Detach()` before closing, and consume `TakePage()` outside event dispatch. Fill `Options` with hardware-dependent choices before attaching, `Load` saved values, and assign `Apply` to apply them (correcting in place as needed) and return a plain user-facing status string. All text, select inputs, sliders, buttons and dice remain native editable elements; there is no JavaScript requirement.

## Validation

`Tools/build-menu-preview.cmd` builds a software preview helper against the installed RmlUi SDK. `Tools/preview-menus.py` validates and renders all six screens at two sizes, plus the populated dice state. It checks native parsing, missing textures, visible button hit targets, margins, AA dependencies, slider labels, Reset, custom/invalid winning scores, host-supplied option lists, host corrections after Apply, and loading of saved values. `Tools/check-integration.cmd` performs C++ syntax checks of the modified implementation files. These checks do not launch the engine or constitute a DirectX playtest.

The original UI documentation follows; its historical verification notes refer to the earlier editor HUD/results implementation.

---

# Farkle UI

Transparent medieval game HUD for **Ptero Engine / RmlUi 6.3**. The reference's layout is recreated with parchment, oak, brass, and iron skins. Text, scores, pips, controls, and the scrollable log are native RmlUi elements. The tavern background is not included.

## Victory and Defeat screens

The two end-of-match references are implemented as separate, transparent documents:

| Document | Layout |
| --- | --- |
| `Farkle/victory.rml` | Wide three-column board, golden Victory heading, crown, laurels, match summary, and tankard ornament |
| `Farkle/defeat.rml` | Tall board, copper-red Defeat heading, paired scores, opponent crown, and inset summary |

Select either document in the UI Editor and press **Load**, or pass its relative path to `LoadDocument`. Both load `farkle.rcss` for the five existing fonts/base skins and `results.rcss` for the result layouts. The new ornament atlas is `Textures/results-ornaments.png`, with a real alpha channel. Only the boards and their contents are painted; there is no tavern image or full-screen dimmer.

The game host connects `rematch-button`, `main-menu-button`, and `continue-button` to game actions. These native controls have matching `data-action` values (`rematch`, `main-menu`, `continue`), keyboard focus styling, hover/pressed states, and support the `disabled` attribute. Gameplay lives in the existing C++ Game project.

Both screens expose `result-title`, `result-subtitle`, `result-player-name`, `result-opponent-name`, `result-player-score`, `result-opponent-score`, `result-best-roll`, `result-banked-total`, and `result-flavor`. Victory additionally exposes `result-bonus-label` and `result-bonus`; Defeat exposes `result-missed-chance`. All supplied scores and summaries are editable example content matching the references.

Titles use Grenze Gotisch; Match Summary uses Pirata One; buttons/names use IM FELL English SC; scores/statistics use EB Garamond; flavor lines use Fondamento.

The result previews are `Preview/victory-1600x900.png` and `Preview/defeat-1600x900.png`, with 1280×720 versions alongside them. They were rendered with the **existing** native preview executable, without compiling the engine or any helper. `Tools/preview-results.py` repeats this check using Python and Pillow. It adapts the old preview tool's HUD-specific probe IDs in a temporary copy only; the delivered document IDs remain unchanged. Native layout, font/texture loading, the three action hit targets, and transparent margins were checked at both resolutions. This is a document preview, not a full engine playtest.

## Load

Using the existing engine UI renderer:

```cpp
ui.LoadDocument("Farkle/farkle.rml");
```

Or directly from a RmlUi context:

```cpp
auto* document = context->LoadDocument("K:/Ptero-Engine/Data/UI/Farkle/farkle.rml");
if (document) document->Show();
```

The UI Editor's document picker recursively scans `Data/UI`, so select `Farkle/farkle.rml` and press **Load**. The engine and node graph's UI load action also accept the same relative path. **Reload** preserves it.

`farkle.rml` is the entry point; `farkle.rcss` contains layout, fonts, sprite definitions, and states. `Textures/farkle-atlas.png` is the shared RGBA skin atlas. The document and empty play area have no background fill. Preview PNGs also have real alpha; a viewer may display that transparency as black.

The layout targets landscape 16:9 displays and uses viewport-relative sizing. Native rendering was verified at 1280×720 and 1600×900. Parchment and wood panels use RmlUi ninepatch decorators, preserving their corners as they resize. No browser, JavaScript, SVG support, custom decorators, filters, or extra shaders are required.

## Fonts

The existing font files and their OFL licenses are preserved in `Fonts/`.

| Family | Use |
| --- | --- |
| Pirata One | Farkle title |
| Grenze Gotisch | Panel headings and dialog titles, including victory/defeat text |
| IM FELL English SC | Buttons, player labels, turn banner, tray headings |
| EB Garamond | Scores, scoring table, log, instructions, shortcut labels |
| Fondamento | Dialog flavor text |

Static TTF faces are used for predictable FreeType rendering. This bundled 6.3 build hands `@font-face` paths to the file interface verbatim rather than resolving them against the stylesheet, so the five font declarations are written relative to the Data root (`UI/Farkle/Fonts/...`). The engine resolves them against Data/ (or, in a packaged game, the UI package); run `Tools/verify-menus.exe` with `Data/` as the working directory. All texture references remain relative to the stylesheet.

## Connect to gameplay

The existing `Source/Game` project now drives this UI through the renderer's queued event adapter. It supplies scoring, opponent decisions, animated dice throws, camera travel, and live values. See [game setup and configuration](../../../Documentation/Farkle.md). The static document values remain preview examples; gameplay replaces them. The integration uses these IDs:

| Element | Action / value |
| --- | --- |
| `roll-button`, `bank-button`, `clear-button` | Roll, bank, clear selection |
| `help-button`, `pause-button`, `close-button` | Open help, open pause menu, close dialog |
| `die-1` … `die-6` | Select rolled dice; `data-index` is zero-based |
| `slot-1` … `slot-6` | Remove a selected die; `data-index` is zero-based |
| `player-name`, `opponent-name` | Player names |
| `player-score`, `opponent-score` | Total scores |
| `turn-score`, `banked-score`, `round-score` | Live counters |
| `turn-label`, `status-text` | Turn banner and empty-area message |
| `game-log` | Append `.log-row` elements with `.gain` score spans |

Example update:

```cpp
document->GetElementById("player-score")->SetInnerRML("2,500");
document->GetElementById("turn-label")->SetInnerRML("Opponent's Turn");
document->GetElementById("play-area")->SetClass("has-dice", true);
document->GetElementById("die-1")->SetClassNames("die face-5 selected");
document->GetElementById("bank-button")->SetAttribute("disabled", true);
// Re-enable with RemoveAttribute("disabled").
```

For a populated slot, replace `+` with a `span` carrying `die face-N`, containing the same seven pip spans as a rolled die. Use a span rather than nesting another button. `selected` raises and highlights a die; `held` dims it. Use the native `disabled` attribute when a die or control must not activate.

To display the optional dialog:

```cpp
document->GetElementById("dialog-title")->SetInnerRML("Victory");
document->GetElementById("dialog-text")->SetInnerRML("You reached the winning score.");
document->GetElementById("dialog-flavor")->SetInnerRML("Fortune favours the bold.");
document->GetElementById("dialog")->SetClass("hidden", false);
document->GetElementById("close-button")->Focus();
// Hide on close and restore focus to roll-button.
```

The overlay catches pointer input. The host should suspend game shortcuts and restrict keyboard focus to the dialog while it is open. The shell has no dimming background, preserving the requested transparent UI.

Keyboard hints are displayed as R / B / C / F1 / Esc. The game host binds these shortcuts and 1–6 selection keys while the separate game window has focus. F11 toggles fullscreen. Buttons participate in native tab navigation.

The scoring table uses 100 / 200 / 400 / 800 × face value, with the displayed exception that three ones score 1,000 and each extra one doubles that score. The match target is 3,000 with one final reply turn.

## Validation and previews

The existing previews below predate the C++ game integration and latest HUD changes. No build, test, debug session, or new preview render was run for the game implementation, as requested.

`Tools/verify.cmd` builds a small C++ software renderer against the project's actual RmlUi 6.3 and FreeType libraries. It loads the document, checks parser warnings and missing textures, tests native click delivery on the five main controls, checks screen bounds and transparent scene pixels, and writes RGBA PNGs. It does not launch or alter the engine. The script uses the locally installed MSVC path; update that path if using another machine.

- `Preview/farkle-1600x900.png` — idle screen, transparent
- `Preview/farkle-1280x720.png` — smaller viewport
- `Preview/farkle-dice.png` — all six native pip layouts and selected state
- `Preview/farkle-hover.png` — primary button hover
- `Preview/farkle-dialog.png` — help shell and Fondamento text

The previews use native RmlUi layout, font rasterization, and generated geometry with a simple software rasterizer. They validate the document without claiming a full DirectX engine playtest.

RmlUi references: [ninepatch decorators](https://mikke89.github.io/RmlUiDoc/pages/rcss/decorators/ninepatch.html), [sprite sheets](https://mikke89.github.io/RmlUiDoc/pages/rcss/sprite_sheets.html), [fonts](https://mikke89.github.io/RmlUiDoc/pages/rcss/fonts.html).
