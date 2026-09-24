# Node Graph

Node Graph is the engine's visual scripting: a Blueprint-style graph of white execution
connections and coloured value connections that runs for the duration of a play session.
It is built on the bundled QtNodes SDK (`Source/SDKs/nodeeditor`) and lives in the System
project, next to the other engine modules.

Open it from **Windows → Node Graph...** in the editor.

## Working in the canvas

| Input | Does |
| --- | --- |
| **Right button drag** | Pans the view. It never raises a menu. |
| **Left button** | Selects. On empty canvas it drags a rubber-band selection; on a node it moves the node. |
| **Mouse wheel** | Zooms. |
| **Q** | Opens the node list where the pointer is, filtered as you type. |
| **C** | Wraps the selected nodes in a comment box, or drops an empty one under the pointer. |
| **Delete** | Removes the selection, nodes and comment boxes together. |
| **Drag from the Palette** | Drops that node where you release. Double-clicking adds it at the centre instead. |
| **Drag from Variables** | Asks whether to get or set, then drops the node already pointed at that variable. |
| **Double-click a comment title** | Edits its text and colour, or deletes it. |

Both the palette dock and the Q list are built by the same function, so they cannot drift
apart. The keys are handled by the canvas rather than declared as menu shortcuts: a
single-key window shortcut would fire even while a node's text field had focus, and a
Print String node has every right to contain a `q`. The **Insert** menu lists both with
their key so they stay discoverable.

Comment boxes are drawn behind the nodes and only their title bar and resize grip are part
of the item's shape, so a rubber-band selection drags straight across one instead of being
swallowed by it. Dragging a box by its title carries every node whose centre was inside it
when the drag started. Click a title bar to select the box, then Delete — comment removal
hangs off `GraphicsView::deleteSelectionAction()` rather than a key handler, because the
view binds Delete as a QAction shortcut and Qt dispatches shortcuts before a widget's key
events ever run.

## Where the code lives

| File | Role |
| --- | --- |
| `Source/System/include/System/NodeGraphCatalog.h`, `src/NodeGraphCatalog.cpp` | The single table of node types: ports, categories, inline parameters. |
| `Source/System/include/System/NodeGraphDocument.h`, `src/NodeGraphDocument.cpp` | The saved form of a graph, and its JSON reader/writer. |
| `Source/System/include/System/NodeGraphRuntime.h`, `src/NodeGraphRuntime.cpp` | The interpreter, plus the `NodeGraphHost` interface it reaches the engine through. |
| `Source/System/include/System/NodeGraphEditor.h` | Qt-free facade the rest of the editor calls. |
| `Source/System/src/NodeGraphEditorWindow.cpp` | The Qt window, built on QtNodes. |
| `Source/QtUi/QtNodes.props` | Compiles the QtNodes SDK into the importing project, standing in for CMake's AUTOMOC/AUTORCC. |

The catalogue, document and runtime are free of Qt and of any renderer type, so
`System.vcxproj` builds them like any other System module. `NodeGraphEditorWindow.cpp` is
the exception: it needs Qt, so only `Renderer_DX12.vcxproj` compiles it, which is the DLL
that owns the `QApplication`. `Ptero-Engine.vcxproj` compiles the catalogue and document
because it links `SceneSerializer.cpp` directly.

Adding a node type means adding a row to the catalogue and a case to the runtime. The
editor needs no new code: ports, palette entry, inline editors and serialisation all come
out of the table.

## Execution model

QtNodes draws a data-flow graph, but a graph *runs* the Blueprint way:

- **Exec pins** (white) push control forward. An exec output fires exactly one target; an
  exec input may be fired by many sources. Graphs may contain loops - `PteroGraphModel`
  re-enables them, since a Gate or For Loop is pointless without.
- **Value pins** are pulled on demand by whichever node needs them. Values are re-read
  every time a node executes, and memoised only while that one node gathers its inputs.
- Every node parameter whose key matches an input pin acts as that pin's default: a
  connected pin wins, an unconnected one falls back to what was typed on the node body.
- Types are checked when connecting but converted when running, so a number reaching a
  string pin is stringified rather than refused. `any` pins accept anything.

Two guards keep a bad graph from taking the editor down: a per-step budget of 20 000 node
executions, and a recursion depth cap. The depth cap is the one that matters - execution
is recursive, so a self-feeding Gate would blow the stack long before it ran out of steps.
Both log to the runtime's bounded log rather than failing silently.

`Stop Game` only raises a flag; `DX12SceneRenderer::UpdateGameCamera` polls it after
`Tick`. Tearing the runtime down from inside its own execution would destroy the state the
current step is still walking.

## Node set

| Category | Nodes |
| --- | --- |
| Events | On Game Start, On Game Stop, On Tick, On Video Finished, On Key Pressed, On Key Released, On UI Button Clicked |
| Functions | Function Entry, Call Function, Return |
| Flow | Branch, Delay, Gate, Sequence, Do Once, Do N, Flip Flop, For Loop, Switch |
| Logic | Multiplexer, And, Or, Not, Compare |
| Math | Add, Subtract, Multiply, Divide, Clamp, Lerp, Random Range, Random Integer, Random Chance, Min, Max, Modulo, Abs, Floor, Sin, Cos, Ease, Lerp Angle |
| Values | Number, Boolean, String, Append, To String, Format Number |
| Variables | Get Variable, Set Variable |
| UI | Show/Close/Reload UI Document, Set UI Visible, Set UI Input Enabled, Set UI Text, Set UI Property, Set UI Class, Set UI Element Visible |
| Video | Play Video, Pause Video, Resume Video, Stop Video, Seek Video, Set Video Looping, Set Video Volume, Is Video Playing, Get Video Time, Get Video Duration |
| Input | Is Key Down |
| Entity | Entity, Find Entity, Is Entity Valid, Get Entity Name, Get/Set Entity Position, Move Entity By, Get/Set Entity Rotation, Rotate Entity By, Get/Set Entity Scale, Move Entity To, Get/Set Entity Property, Play/Stop Entity Audio |
| Camera | Get Camera, Set Camera, Move Camera To, Release Camera, Camera Look Point |
| Audio | Play Sound, Play Music, Play Music Playlist, Stop Music, Is Music Playing |
| Game | Stop Game, Toggle Fullscreen, Is Standalone, Get Play Time, Print String |

The UI nodes drive `RmlUiRenderer` through `DX12SceneRenderer::NodeGraphUiHost`. Element
nodes address elements by their RML `id` and report failure on a `Success` pin, so a typo
shows up rather than doing nothing quietly.

The video nodes drive one full-screen video layer (`VideoLayer` in `Source/Video`) through
the same host. **Play Video** takes a path inside `Data` (`.webm` optional) or just the file
name, a Fit of Letterbox, Fill or Stretch, and a Volume; it reports on `Success` and logs
why when it fails. The video's Opus soundtrack plays with it, in step with the picture. The video sits over the scene and under the game UI, so a menu or a "skip" prompt can
be drawn on top of it. Its clock is the game's frame time. A video that ends without looping
hides itself and fires **On Video Finished** in the same frame; **Stop Video** hides it
without firing. See `Documentation/Video.md`.

Variables are declared in the window's Variables dock with a name, a C++ type and a
default:

| Type | Stored as | Notes |
| --- | --- | --- |
| `Bool` | true/false | |
| `Int` | whole number | Writes truncate, as the same expression would in C++. |
| `Float` | single precision | The default for a new variable, and what pre-existing `number` variables migrate to. |
| `Double` | double precision | |
| `String` | text | |
| `Text` | text | Identical to `String` at runtime; the editor offers a multi-line box. |

Pins stay deliberately coarser than this list — every numeric type travels on a `Number`
pin and both string types on a `String` pin — so adding a type never invalidates an
existing connection.

`Set Variable` coerces to the declared type, so a variable never changes shape depending
on which branch wrote it. The Variables dock runs a typed default through the *runtime's*
own parser, so the editor and the interpreter cannot disagree about what a default means:
typing `3.7` into an `Int` stores `3`, and `hello` stores `0`.

Many of these were lifted out of the Farkle game module (`Source/Game`) as the parts of
it that any game needs, rather than Farkle's own rules: the thousands-separated score text is
Format Number, the quintic ease on the camera travel and the dice throw is Ease's *Smoother*
curve, the throw itself is Move Entity To with an Arc Height, the table-to-result camera flight
is Move Camera To, "where do the dice land" is Camera Look Point, and the shuffled soundtrack
that never repeats a track is Play Music Playlist. Farkle still runs on its own C++; nothing in
it was switched over to the graph.

### Entities

Entity nodes address an entity by its **id**, a number saved with each entity in the level
(`"Id"` beside `"NameComponent"`). Names repeat and list positions shift, so neither is a safe
reference; the id survives renames, reordering and save/load. Levels saved before ids existed
get them when loaded, and keep them from the next save on. Duplicating or pasting an entity
gives the copy a new id and leaves the original's alone (`EnsureEntityIds` in
`Source/Renderer_DX12/EntityIds.h`).

Every entity node has an **Entity** picker on its body. It lists the level's entities by name,
re-read each time it drops down, and its ◎ button takes whatever is selected in the Outliner
or the viewport. The picker is the default for the node's blue **Entity** pin, so the common
case needs nothing wired; connect an **Entity** or **Find Entity** node when the target varies.
A reference to an entity that has since been deleted shows as `<missing id>` and logs when it
runs, rather than quietly pointing somewhere else.

Positions are in metres, rotations and camera angles in **degrees** (the engine stores
radians; the nodes convert). **Get/Set Entity Property** reaches component fields that are not
transforms: light intensity, radius, colour and temperature, shadow casting, particle
enable and spawn rate, rain enable and intensity. Booleans travel as 1 and 0, and **Success** is
false when the entity lacks that component. Entities cannot be hidden yet - there is no
visibility flag the renderer honours - so a graph that wants something gone moves it away,
as Farkle does with its dice.

The graph edits the play session's copy of the level, so every change snaps back when play
stops.

### Latent nodes, camera and audio

**Move Entity To** and **Move Camera To** fire **Then** immediately and **Completed** on
arrival, like Delay. Firing one again restarts it from wherever it has got to; a newer move of
the same entity (or any camera node) cancels the one in flight without firing its Completed.

The game module writes its own camera every frame before the graph runs. **Set Camera** and
**Move Camera To** therefore *hold* the camera: the runtime re-applies the pose at the end of
every tick until **Release Camera** hands it back. **Get Camera** reports the held pose while
there is one.

**On Key Pressed/Released** and **Is Key Down** only see keys while the game window has focus.
**On UI Button Clicked** has its own queue in `RmlUiRenderer`, separate from the game module's,
so both can react to the same button. Random nodes are reseeded every play session.

## Functions and libraries

A **Function Entry** node *is* the declaration of a function — there is no separate list to
keep in sync. Anything reachable from its `Then` pin is the body, a **Return** ends it
early, and a **Call Function** node runs it and continues once it has finished. The
Functions dock lists everything callable; double-clicking an entry drops a Call node.

Functions take no parameters and return no values: pass data through variables. Note also
that a latent node inside a body — a `Delay` — does not suspend the caller, which resumes
as soon as the rest of the body has run. Both are the same restrictions Blueprint puts on
its own functions, for the same reason.

**Reusing a graph in another one:**

1. Author the reusable graph — say a player controller with `FunctionA` and `FunctionB` —
   and **File → Export Graph...**.
2. In the level graph, **File → Import Graph as Library...**, pick the file, and give it a
   name (defaults to the file name).
3. Its functions are now callable as `PlayerController.FunctionA` and appear in the
   Functions dock and in every Call node's picker.

Import **copies** the library in rather than referencing it by path, so a level stays one
self-contained file with no link to go stale between the level and its scripts. The cost
is that re-importing is how an updated library gets picked up.

Everything the library names — its functions *and* its variables — is prefixed, and the
references inside it are rewritten to match, so a library with a `Speed` variable does not
collide with the level's own `Speed`, and importing the same library twice is safe. Node
ids are renumbered past whatever the document already uses, the copy is offset clear of
the existing nodes, and it arrives wrapped in a comment box named after the library.

A function that calls itself is stopped by the same recursion depth guard that catches a
looping exec chain, so it truncates and logs rather than overflowing the stack.

## Storage

A graph is one JSON object: `Variables`, `Nodes` (id, type, position, params),
`Connections` (node id + port index at each end) and `Comments` (text, rect, colour).
Comments are written only when there are any, and the runtime never reads them.

It is saved **inside the level** under a `"NodeGraph"` key. Levels written before visual
scripting existed, and levels with an empty graph, write nothing there and round-trip
unchanged.

**File → Export Graph...** writes the same object to a standalone `.nodegraph` file, and
**File → Load Graph...** replaces the open graph with one read back from disk. Both dialogs
accept `.json` as well as `.nodegraph` — a graph is plain JSON, so one written by hand or
by a tool opens exactly like an exported one; the extension only decides what the dialog
offers first. **File → Import Graph as Library...** merges instead of replacing (above). A
level may also carry a *path* string instead of an object under `"NodeGraph"`, resolved
relative to the level file; that is how several levels can share one script.

Edits bump `NodeGraphEditor::Revision()`. `Editor::HasUnsavedChanges` compares it against
the value from the last save, so a graph edit marks the level dirty like any other change.

## Building

`Source/QtUi/QtNodes.props` runs `moc` over the ten QtNodes headers that declare
`Q_OBJECT` or `Q_NAMESPACE`, runs `rcc` over `resources.qrc`, and adds the SDK's
translation units to the importing project. The generated sources land in
`$(IntDir)QtNodesGenerated`.

Engine-side node code deliberately avoids `Q_OBJECT` - every signal connection in
`NodeGraphEditorWindow.cpp` uses a lambda and a context object - so moc never has to run
over anything outside the SDK.

The SDK's units opt out of `/sdl` and the noisier warnings, but keep `/permissive-` and
C++20: Qt 6.11 static-asserts on conformance mode, and every Qt inline function these
units share with the rest of the DLL has to be compiled the same way or the two copies
violate the ODR.
