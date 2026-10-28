# 2. The editor

- [Layout](#layout)
- [Moving around the viewport](#moving-around-the-viewport)
- [Placing things](#placing-things)
- [Selecting and transforming](#selecting-and-transforming)
- [Undo and redo](#undo-and-redo)
- [Levels](#levels)
- [Play](#play)
- [The viewport's menus](#the-viewports-menus)
- [View modes](#view-modes)
- [Viewport cameras](#viewport-cameras)
- [Viewport resolution and screenshots](#viewport-resolution-and-screenshots)
- [Level Explorer](#level-explorer)
- [Properties](#properties)
- [Editor Settings and styles](#editor-settings-and-styles)
- [Renderer Statistics](#renderer-statistics)
- [Resource Debug](#resource-debug)
- [Console](#console)
- [The main menu, item by item](#the-main-menu-item-by-item)
- [Keyboard shortcuts](#keyboard-shortcuts)

## Layout

![The editor](<../../Screenshots/screenshot_20260925_191159_editor.png>)

| Area | What it is |
| --- | --- |
| **Main menu bar** (top) | File, Edit, View, Play, Audio, Windows, Release, Help |
| **Toolbar** (below it) | The tools **Select**, **Move**, **Rotate**, **Scale**; the **Wireframe** and **Proxy** display toggles; and **Play** (the gamepad button; right-click it to choose where play runs) |
| **Viewport** (centre) | The rendered level. It has its own menu bar: **Viewport**, **Options**, **View Mode** and **Fullscreen** |
| **Left dock** | **Components** (things you can place), with **Console** and **Level Explorer** as tabs beside it |
| **Right dock** | **Properties** (the selected entity), plus **Resource Debug** and **Audio Manager** |
| Separate windows | Material Editor, Node Graph, Time of Day, Graphics Settings and the other tools open as their own windows |

### Docking and floating panels

- **Drag a panel by its title** to move it: drop it on another dock area, onto another panel
  to make tabs, or anywhere else to float it as its own window.
- Floating panels and tool windows are ordinary windows with minimise, maximise and close
  buttons; double-click the title bar to maximise. They stay above the editor and have their own
  taskbar button.
- Each tool window opens sized to fit its contents (up to 90% of the screen) and is measured
  again every time you reopen it.
- **Windows** in the main menu shows or hides every panel and tool window. A panel you closed or
  lost comes back from there.

The layout is saved when the editor closes and restored at the next start.

### Resetting the layout

There is no reset command in the editor yet. To go back to the default layout, close the editor
and delete the saved layout, which Windows keeps in the registry under
`HKEY_CURRENT_USER\Software\Pterosoft\Ptero Editor` (value `layout/qt-v1`). From a command
prompt:

```bat
reg delete "HKCU\Software\Pterosoft\Ptero Editor\layout" /f
```

The next start uses the default arrangement. (The same registry key also holds your recent
levels and editor style, which are not affected by this command.)

## Moving around the viewport

| Input | Does |
| --- | --- |
| Hold **right mouse button** + move the mouse | Look around |
| Hold right mouse + **W** / **S** | Fly forward / back |
| Hold right mouse + **A** / **D** | Fly left / right |
| Hold right mouse + **E** / **Q** | Fly straight up / down (world Z) |
| Hold right mouse + **mouse wheel** | Fly faster or slower: each notch is 15%, from 0.05 to 200 m/s |
| **F11** | Fullscreen viewport (and back) |

The flying speed is also a slider: **Windows → Scene Settings... → Camera Speed**. Set it low
(1–2 m/s) for detailed interiors and high (50+ m/s) for terrain.

The movement keys only work while the editor window has keyboard focus, so typing in another
program never moves the camera, and they are ignored while you type in a text field.

**Getting to an entity.** There is no "focus on selection" key yet. To find an entity, select it
in the **Level Explorer**, read its **Position** in Properties, and fly there; or, for a camera,
use **Viewport → Pilot Selected Camera** to jump into it.

## Placing things

The **Components** panel lists everything you can place: **Geometry**, **Point Light**, **Spot
Light**, **Rect Light**, **Camera**, **Audio Emitter**, **Decal**, **Rain**, **Particle System**,
**Vegetation Area**, **Terrain** and **Water**.

1. Click an item. It stays highlighted ("armed"). Click it again to disarm.
2. Click in the viewport. The entity is created on the ground plane (height 0) under the cursor,
   and selected.
3. Set it up in **Properties**.

Special cases:

- **Geometry** creates an entity with an empty Mesh. Choose the model with **Select Geometry
  (.ptero)** in its Mesh section.
- **Camera**: double-click the item to drop a camera exactly where the viewport is looking from.
- **Terrain** first opens a dialog for the heightmap (see [9. Outdoor worlds](09-Outdoor-Worlds.md#terrain)).

## Selecting and transforming

### Selecting

| Input | Does |
| --- | --- |
| Left-click an entity in the viewport | Select it |
| Left-drag over empty space | Box-select everything inside |
| **Ctrl**+click | Add an entity to the selection, or remove it |
| **Ctrl**+drag | Add the boxed entities to the selection |
| Click in the **Level Explorer** (Ctrl+click to add) | Select from the list |

### The gizmo

The keys are **1–4**, as the toolbar tooltips show. (Q, W, E and R are not gizmo keys: W, E
and Q fly the camera.)

| Key / button | Gizmo |
| --- | --- |
| **1** / Select | No gizmo: clicks only select |
| **2** / Move | Arrows for X (red), Y (green), Z (blue), plus plane handles to move along two axes at once |
| **3** / Rotate | Rings around each axis |
| **4** / Scale | Per-axis handles; the plane handles together scale uniformly |

- The handle under the cursor lights up before you click, so you can see what you are about to
  grab. What lights up is exactly what the click takes.
- The gizmo stays the same size on screen however far away the entity is.
- With **several entities selected**, the gizmo moves the whole group: positions follow the
  primary (last-selected) entity, rotation swings the group around the primary's pivot, and
  scale is applied to each entity.

For exact values, type them into the **Transform** section of Properties instead. Rotation is in
degrees.

### Snapping

In the viewport's **Options** menu:

| Option | Meaning |
| --- | --- |
| **Movement Snap** + *Move Step* | Moves in steps of this many metres (for example 0.5 or 1 for modular kits). |
| **Rotation Snap** + *Rotate Step* | Rotates in steps of this many degrees (15 or 90 are common). |

### Copy, paste and delete

| Keys | Does |
| --- | --- |
| **Ctrl+C** | Copy the selected entities (all their components) |
| **Ctrl+V** | Paste. The copies land **exactly on top of the originals** and become the selection, ready to move with the gizmo. |
| **Delete** | Delete the selected entities |

Pasted entities get new ids, so scripts and sequences that referred to the originals keep
referring to the originals.

## Undo and redo

| Keys | Does |
| --- | --- |
| **Ctrl+Z** | Undo |
| **Ctrl+Y** or **Ctrl+Shift+Z** | Redo |

**What undo covers:** everything in the level: creating, deleting, copying entities; their
transforms; every component property; and the selection. The rule is simple: *if a change marks
the level as unsaved, it can be undone.*

**What it does not cover:**

| Not undoable | Instead |
| --- | --- |
| Render settings (Graphics Settings, Time of Day, DPLE, ...) | Each section has a **Revert All** or reset button |
| Node Graph edits | The Node Graph window has its own undo (Ctrl+Z inside it) |
| Sequencer edits | The Sequencer has its own undo |
| Material files, particle effects | Saved as separate files; reopen the file to discard changes |
| Terrain sculpting | **Revert Sculpting** on the Terrain component throws away all sculpting |

Things to know:

- Edits less than 0.4 seconds apart merge into one step, so one gizmo drag is one undo. Two quick
  separate edits can therefore merge too.
- The history holds **64 steps**.
- The history is **cleared** when you open or create a level, so you cannot undo into a
  different level.
- Shortcuts are ignored while a text field has focus, so Ctrl+Z in a text box undoes the typing,
  not the level.

## Levels

| Command | Shortcut | Notes |
| --- | --- | --- |
| **File → New** | Ctrl+N | An empty level |
| **File → Open...** | Ctrl+O | Opens `.level` files and older `.json` levels |
| **File → Recent Levels** | | The last ten levels opened; **Clear Recent Levels** empties the list |
| **File → Save** | Ctrl+S | Saves to the level's file. A level that has never been saved asks for a name, like Save As. |
| **File → Save As...** | | Save under a new name. Choose `.level` (the current format) or the legacy `.json` format. |

Save levels inside `Data/Levels` so the packaged game can find them.

**Unsaved changes.** The editor keeps track of whether the level has changed since it was last
saved. **New**, **Open** (from the menu, the shortcut or Recent Levels) and **closing the
editor** all ask *"The current level has unsaved changes. Do you want to save it before
continuing?"*:

- **Yes**: save, then continue (cancelled if you cancel the save dialog);
- **No**: discard the changes and continue;
- **Cancel**: go back to editing, nothing happens.

Closing the editor's window asks before anything shuts down, so Cancel really does carry on.

What is saved in a level, and what isn't, is listed in
[1. How Ptero works](01-How-Ptero-Works.md#levels-entities-and-components).

## Play

Press **Play** on the toolbar (or **Play → Play**) to run the level as a game. Where it runs is
set in the **Play** menu, or by right-clicking the Play button (it can't be changed while a
session is running):

- **Play In Viewport**: inside the editor's viewport. While playing, the keyboard belongs to the
  game: the editor's single-key shortcuts (1–4, Delete) are switched off so the game can use
  those keys.
- **Play In New Window**: a separate game window.

While playing:

- the mouse is captured for looking; **Escape** releases it, and clicking the game view takes it
  back;
- with the mouse released, holding the right button still looks around;
- **~** (the key under Escape, on any keyboard layout) or **Play → Stop** ends the session.

When play stops, the level is exactly as it was. See [11. Gameplay](11-Gameplay.md).

## The viewport's menus

**Viewport**

| Item | Does |
| --- | --- |
| Resolution... | Render at a fixed resolution ([below](#viewport-resolution-and-screenshots)) |
| Screenshot... | Save the current frame ([below](#viewport-resolution-and-screenshots)) |
| Lock to Cinematic Camera | Look through the Sequencer's current shot ([below](#viewport-cameras)) |
| Pilot Selected Camera / Stop Piloting Camera | Fly a camera entity ([below](#viewport-cameras)) |

**Options**

| Item | Does |
| --- | --- |
| Renderer Statistics | The statistics overlay ([below](#renderer-statistics)) |
| Placement Icons & Light Shapes | Show or hide entity icons and the light-reach wireframes |
| Grid | The ground grid (one square = one metre) |
| Game UI | Show the game's interface over the viewport. Underneath, a line reports whether the UI is ready and which document is loaded. |
| Game UI Inspector | RmlUi's debugger for the live game UI |
| Reload Game UI | Re-read the UI documents from disk after editing them |
| Movement Snap, Move Step, Rotation Snap, Rotate Step | Snapping |

**View Mode**: see below. **Fullscreen** (F11): the viewport fills the window.

## View modes

**View Mode** in the viewport menu bar changes what the viewport shows. Everything except *Lit*
is a diagnostic view.

| View | Shows |
| --- | --- |
| **Lit** | The normal, final image. Picking it switches every other view off. |
| **G-Buffer → All Buffers** | Every G-Buffer channel at once, as a labelled grid: base colour, normals, roughness, metallic, depth, motion and more |
| **G-Buffer → *one channel*** | One channel, full screen. Use it to check a material's textures are what you expect (a roughness map imported as colour, a normal map the wrong way round). |
| **Global Illumination** | RTGI, Voxel GI, Radiance Cascades and Path Tracer views: the raw bounce light, its brightness, reflections only, voxel contents |
| **Ambient Occlusion** | RTAO, GTAO and DPLE views |
| **Reflections** | SSR contribution and confidence |
| **Subsurface Scattering** | Scattered light only, or the mask of subsurface surfaces |
| **Shadows** | Virtual shadow map levels and sun visibility; point-light shadows |
| **Volumetrics** | Fog scattering and transmittance; cloud views |
| **Anti-Aliasing** | Edges and blend weights (SMAA), edge classification and coverage (Analytic Edge AA, AAR) |
| **Virtual Geometry** | Clusters, instances, LOD levels |

A system's debug view only shows while that system is running. The list shows them all, so you
never have to switch a system on just to find its view.

Related display toggles:

- **Placement Icons & Light Shapes** (Options): each light draws its reach in its own colour:
  three circles for a point light's radius (a fainter pair for its source size), a cone for a
  spot light, the emitting rectangle and a reach line for a rect light. Turning icons off hides
  these too, for an unobstructed look at the render.
- **Grid** (Options).
- **Fullscreen** (F11, or Fullscreen in the viewport menu bar).
- **Wireframe** (toolbar): draws the scene's geometry as wireframe, to see triangle density.
- **Proxy** (toolbar): draws every mesh's generated **collision hulls** as green wireframes, to
  check what physics will collide with. Meshes without generated collisions show nothing.

## Viewport cameras

The viewport normally has its own free camera. It can also borrow a **Camera entity**:

**Pilot Selected Camera** (Viewport menu, or the camera's Properties): select a camera entity
and choose this. The viewport jumps into the camera, and flying the viewport now moves the
camera entity. It is the easiest way to frame a shot: fly until it looks right, then **Stop
Piloting Camera**. The camera keeps the position. With the Sequencer open, the view follows the
camera when the sequence animates it, and only writes back when you actually fly, so scrubbing
the timeline never moves the camera by accident.

**Lock to Cinematic Camera** (Viewport menu, the Sequencer's **Camera View** button, or **C** in
the Sequencer): the viewport looks through whichever camera the open sequence's Camera Cuts
track has at the playhead, with letterbox mattes matching the render's aspect ratio. Turning it
off returns to exactly where your free camera was. See [12. Cinematics](12-Cinematics.md).

## Viewport resolution and screenshots

### Resolution

**Viewport → Resolution...** sets the resolution the scene renders at:

- Presets: **1280 × 720**, **1600 × 900**, **1920 × 1080**, **2560 × 1440**, **3840 × 2160**;
  **Window Resolution** takes the current window size; or type a width and height.
- **Apply** fixes the render at that size. The viewport shows it scaled to fit the panel.
- **Fit to Viewport** goes back to following the panel's size.

Use a fixed resolution to test performance at a target resolution, or before taking a
screenshot at an exact size.

### Screenshots

**Viewport → Screenshot...** opens a small window showing the output folder and the current
render resolution. **Capture** saves the frame as a PNG.

- Files go to the **`Screenshots`** folder in the project root (next to `Data`), named by date and
  time: `screenshot_20261025_143012.png`.
- The image is the rendered scene at the render resolution, not the editor window: panels,
  gizmos and the statistics overlay are not in it. For a 4K screenshot, set **Viewport →
  Resolution...** to 3840 × 2160 first.
- The grid and the light wireframes are drawn into the scene, so turn off **Grid** and
  **Placement Icons & Light Shapes** for a clean image.
- For a final-quality still, switch on the path tracer's **Reference** mode, wait for it to
  converge, then capture. See [10. Image quality](10-Image-Quality.md#path-tracer).
- For animated captures, render a sequence from the Sequencer instead.

## Level Explorer

The **Level Explorer** (left dock, a tab beside Components) lists every entity in the level, in
the order they were created.

| Action | How |
| --- | --- |
| Select | Click a name. The viewport selection and Properties follow. |
| Multi-select | Ctrl+click |
| Rename | Select, then edit **Name** at the top of Properties |
| Delete, copy, paste | Select, then Delete / Ctrl+C / Ctrl+V |

Names do not have to be unique; scripts and sequences refer to entities by a hidden permanent id,
not by name. Two names do matter: `PlayerStart` (where the player spawns) and names a script
compares against (like `Door` in [Tutorial 2](Tutorial-2-First-Person-Door.md)).

The list is flat: there are no folders, groups or parenting yet, and no search box. Good naming is
the way to stay organised: prefix entities by area or type (`Kitchen_Table`, `Light_Hall_01`,
`Cam_Intro`) so related ones read together.

## Properties

**Properties** shows the selected entity: its **Name**, **Transform**, and a section per
component (Mesh, Point Light, Camera, Decal, ...), each described in
[4. Components](04-Components.md). Rigid bodies are added and removed in the **Physics** section.
With nothing selected it says *No entity selected*.

Numeric fields with a range are a slider plus a box: drag the slider for a rough value, or type
into the box for an exact one. The typed value is what is stored, so the slider's steps never
limit precision.

## Editor Settings and styles

**Edit → Editor Settings...** chooses the editor's look.

| Control | Does |
| --- | --- |
| **Editor style** | Pick a style. Built in: **Ptero Dark**, **Ptero Light**, **Midnight Blue**, **Midnight Brass**, **Frozen**. The choice is remembered. |
| **Reload** | Re-read the current style file. (Saving the file reloads it automatically.) |
| **Rescan Folder** | Pick up `.style` files added since the list was read. |
| **Open Styles Folder** | Opens `Data/Styles` in Explorer. |
| **New style: Name + Duplicate** | Copies the current style to a new `.style` file and switches to it. |

A style is a small text file (JSON) of named colours (window, panel, header, button, text,
accent, borders, scrollbars, ...) and icon colours. Edit it in any text editor; the editor
updates as soon as you save. `Data/Styles/README.md` lists every setting.

The other editor preferences live elsewhere: camera speed, grid, statistics and icons in
**Windows → Scene Settings...**; snapping in the viewport's **Options** menu.

## Renderer Statistics

**Options → Renderer Statistics** (or **Show Renderer Statistics** in Scene Settings) draws a
translucent overlay in the corner of the viewport, updated four times a second:

| Line | Meaning |
| --- | --- |
| **FPS** | Frames per second, averaged over a quarter of a second |
| **Latency** | How long the last frame took, in milliseconds (16.7 ms = 60 FPS, 33.3 ms = 30 FPS) |
| **Editor UI** | Time spent building and updating the editor's panels this frame. If this is high, close tool windows you aren't using. |
| **CPU / GPU / RAM Usage** | How busy the processor and graphics card are, and memory used |
| **Camera Position / Rotation** | Where the editor camera is, in metres and degrees. Handy for typing exact positions into Properties. |
| **Viewport** | The render resolution, and how often the viewport has been resized |
| **Render / Outside / Tail** | Time the frame spent rendering, outside the renderer, and finishing up |
| **Scene / UiBuild / QtEvents** | Scene update, editor UI build and Qt event handling |
| **Pump / Audio / msgs** | Windows message handling and audio update |
| **Qt** / **Events** | Editor UI repaint counters, for diagnosing a slow editor |

How to read it:

- **Latency high, GPU Usage near 100%**: the GPU is the limit. Open **Resource Debug** to see
  which render passes cost most; lower settings or use an upscaler.
- **Latency high, GPU Usage low**: the CPU or the editor is the limit. Check **Editor UI**; large
  open tool windows cost time every frame.
- The last four lines are for engine developers; you can ignore them.

## Resource Debug

**Windows → Resource Debug** (right dock) shows where the frame's time goes:

- **Renderer Elements**: a table of every render pass (shadows, G-Buffer, GI, fog, post-processing
  and so on) with its **GPU ms** and **CPU ms** this frame, plus the totals.
- **Engine elements**: a rough CPU / GPU / RAM share per engine system.

Sort out performance here first: the pass with the biggest GPU time is the setting to look at.
[10. Image quality](10-Image-Quality.md#performance-checklist) has a checklist.

## Console

**Windows → Console** (left dock, a tab beside Components) shows everything the engine logs and
accepts commands.

### Reading the log

Each line has a level (Trace, Debug, Info, Warning, Error, Fatal) and a short category tag
(`Renderer`, `Editor`, `Meshes`, `Assets`, `UI`, `RTGI`, `Console`, `Watchdog`, ...). Warnings and
errors stand out; filter by category to follow one system.

The same lines are written to a file in `Logs/` (`Ptero_<date>_<time>.txt`). The file is written
as it goes, so it survives a crash; the 30 newest are kept. Type `logpath` to see the current one.

### Commands and settings

Every renderer setting is a **cvar** (console variable) with a dotted name. Typing in the
Console changes the very same value the settings windows do.

```
rtgi.maxbounces              show the value, its range and its default
rtgi.maxbounces 3            set it
list fog                     every setting whose name or description mentions fog
find shadow                  the same as list
help taa.enabled             describe one setting
reset rtgi.maxbounces        back to its default
reset all                    every setting back to its default
```

As you type, matching settings drop down with their descriptions. Matching finds the text
anywhere in the name, so `specular` finds `rtgi.specular.enabled`. Arrow keys move through the
list; Enter or Tab inserts the name.

- On/off settings accept `true/false`, `1/0`, `on/off`, `yes/no`.
- Choice settings accept the label or its number.
- A value outside the allowed range is **rejected, not clamped**, so a typo never silently does
  something else.

| Command | Does |
| --- | --- |
| `help` | List the commands |
| `help <name>` | Describe one setting |
| `list [text]`, `find <text>` | Settings whose name or description contains the text |
| `reset <name>`, `reset all` | Back to the default |
| `dumpcvars`, `cvarlist` | Every setting and its current value |
| `log <level>` | How much detail to log: `trace`, `debug`, `info`, `warning`, `error`, `fatal` |
| `logpath` | Where this session's log file is |
| `clear` | Empty the Console (the log file keeps everything) |
| `echo <text>` | Print text |

The full list of settings, with what each does, its values and its default, is in the
[CVar reference](CVar-Reference.md).

## The main menu, item by item

**File**: New, Open, Recent Levels, Save, Save As, **Compile Shaders** (recompile everything in
`Data/Shaders`), Exit.

**Edit**: Undo, Redo, Copy, Paste, Delete, **Editor Settings...**.

**View**: **G-Buffer / RT Debug...** (a window with live views of the G-Buffer and ray-tracing
buffers); **Virtual Geometry → Virtualize All Meshes**, its debug view, and **Freeze Culling**
(see [4. Components](04-Components.md#virtualized-geometry)).

**Play**: Play, Stop, Play In Viewport, Play In New Window.

**Audio**: **Audio Manager...** (loaded FMOD banks, failures and every event; click one to
audition it), **Stop All**.

**Windows**

| Window | Use it to... | Manual page |
| --- | --- | --- |
| Scene Settings | camera speed, grid, statistics, icons | this page |
| Material Editor | create and edit materials | [5](05-Materials.md) |
| Particle Editor | author `.particle` effects | [4](04-Components.md#particle-system) |
| Node Graph | script the level | [11](11-Gameplay.md) |
| Cinematic Editor | the Sequencer | [12](12-Cinematics.md) |
| Components, Level Explorer, Properties, Resource Debug, Console | the dock panels | this page |
| UI Editor | preview and debug game UI documents | [11](11-Gameplay.md#game-ui) |
| Terrain Tool | sculpt and paint terrain | [9](09-Outdoor-Worlds.md#terrain) |
| Asset Browser | import and manage files in `Data/` | [3](03-Assets-And-Importing.md) |
| Video Player | preview videos | [11](11-Gameplay.md#video) |
| Time of Day | sun, sky, moon, stars, exposure, wind | [6](06-Lighting.md) |
| Graphics Settings | every renderer setting for this level | [7](07-Global-Illumination.md), [8](08-Shadows.md), [10](10-Image-Quality.md) |
| DPLE | the lighting enhancer | [7](07-Global-Illumination.md#dple) |
| Game Settings | player controller, startup level, Steam | [13](13-Shipping.md) |

**Release**: **Build Game...**, **Extract Package...** ([13. Shipping](13-Shipping.md)).

**Help**: **About Ptero Editor**, including every third-party library the engine uses.

## Keyboard shortcuts

### Editor

| Keys | Does |
| --- | --- |
| Ctrl+N | New level |
| Ctrl+O | Open level |
| Ctrl+S | Save level (Save As the first time) |
| Ctrl+Z | Undo |
| Ctrl+Y, Ctrl+Shift+Z | Redo |
| Ctrl+C / Ctrl+V | Copy / paste the selected entities |
| Delete | Delete the selection |
| 1 / 2 / 3 / 4 | No gizmo / Move / Rotate / Scale |
| F11 | Fullscreen viewport |
| Ctrl+click, Ctrl+drag | Add to the selection |

All of these are ignored while a text field has focus.

### Viewport camera (hold the right mouse button)

| Keys | Does |
| --- | --- |
| Mouse | Look |
| W A S D | Fly forward, left, back, right |
| E / Q | Up / down |
| Wheel | Faster / slower |

### Play session

| Keys | Does |
| --- | --- |
| W A S D, mouse | Walk, look |
| Space | Jump (release early for a lower hop) |
| Shift | Sprint |
| Ctrl or C | Crouch |
| Escape | Release the mouse (click the view to capture it again) |
| ~ | Stop playing |

These are the built-in first-person controller's keys; a Node Graph controller can change them.

### Other windows

- **Node Graph**: Q node list, C comment box, Delete, right-drag pan, wheel zoom
  ([11. Gameplay](11-Gameplay.md#working-in-the-canvas)).
- **Sequencer**: Space play, S key, C camera view and more
  ([12. Cinematics](12-Cinematics.md#keyboard-while-the-sequencer-has-focus)).
- **Video Player**: Space, L, M, arrows, Home, Ctrl+O, Esc
  ([11. Gameplay](11-Gameplay.md#video)).
