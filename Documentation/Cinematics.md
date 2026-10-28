# Ptero Sequencer and Ptero Graph — integration architecture

This is the plan both systems are built against, written after surveying the engine as it
stands (October 2026). It records what already exists, what each new piece reuses, and
which file owns what. Phases 1 and 2 (the cinematic runtime and the basic Sequencer) are
implemented; the later phases are described so that what is built now does not have to
be torn up for them.

## What the engine already has

| Need | Existing system | Where |
|---|---|---|
| Entities | `Entity` — a plain struct of `std::optional<...Component>`s, held in `Editor::mEntities` | `Renderer_DX12/Components.h` |
| Stable identity | `Entity::Id`, a persistent random 53-bit id assigned by `EnsureEntityIds` and saved in the level | `Renderer_DX12/EntityIds.h` |
| Transform | `TransformComponent` (metres, Euler radians, composed X→Y→Z, Z-up) | `Components.h` |
| Lights | `PointLightComponent` (point / spot / rect) | `Components.h` |
| Camera | **No camera entity.** One `EditorCamera` (position, pitch, yaw, vertical FOV) owned by `DX12SceneRenderer`; the game and the Node Graph drive it during play | `Ptero-Engine/EditorCamera.h` |
| Audio | FMOD Studio through `AudioManager` (events by name or path, positional emitters) | `Audio/AudioManager.h` |
| Animation | **None.** Meshes are static; there is no skeleton or clip system | — |
| Assets | Files under `Data/`, read through `DataFiles` so the packaged game reads `.ppak` archives transparently | `System/DataFiles.h` |
| Serialization | nlohmann::json everywhere (levels, materials, node graphs) | `SDKs/nlohmann` |
| Editor UI | **Qt, not ImGui.** `QtUi` is an ImGui-shaped immediate API that reconciles into retained Qt widgets; complex tools (the Node Graph) are written directly against Qt | `QtUi/QtUi.h`, `System/src/NodeGraphEditorWindow.cpp` |
| Undo | Level edits: whole-entity-list snapshots recorded from `Editor::MarkSceneChanged`. The Node Graph has its own `QUndoStack` | `Renderer_DX12/EditorUndo.cpp` |
| Visual scripting | **The Node Graph** — a Blueprint-model graph (exec pins, pulled data pins, events, flow control, variables, functions, comments) with a QtNodes editor and a Qt-free interpreter | `System/NodeGraph*.{h,cpp}` |
| Reflection | None general. The Node Graph has a 12-row string table of entity properties in `DX12SceneRenderer.cpp` | `NodeGraphUiHost::GetEntityProperty` |
| Game build | `Renderer_DX12` built with `PteroGameRuntime=true` swaps every Qt translation unit for a headless stub | `Renderer_DX12.vcxproj` |

## Decisions

**1. Ptero Graph evolves the Node Graph rather than replacing it.** The existing Node
Graph already separates editor (QtNodes) from runtime (`NodeGraphRuntime`, no Qt) and
from data (`NodeGraphDocument`), and Farkle ships on it. A second visual-scripting
system beside it would be exactly the duplicate subsystem the brief forbids. Phases 4–9
therefore grow it: typed pins in place of the loose Number/String/Bool kinds, a
registration API in place of the static catalogue table, UUID-addressed nodes and pins,
one `GraphInstance` per entity carrying its own variables, and the debugger. Existing
`.nodegraph` files must keep loading.

**2. The Sequencer is written directly against Qt.** `QtUi`'s draw list has no text and
no curves, and a timeline is a custom-painted control anyway. The window follows the
Node Graph window's pattern: a `QMainWindow` parented to the editor shell, no `Q_OBJECT`
(lambdas with context objects, so moc never runs on engine code), and a facade header
the rest of the editor calls. Both timeline panes are painted from one cached row model,
so only visible rows cost anything and hundreds of tracks stay cheap.

**3. One property registry, shared by both systems.** `PropertyRegistry` (System) maps a
stable path such as `PointLight.Intensity` to a type, display metadata and type-erased
accessors. Properties belong to a *domain* — `Entity` for anything on an entity,
`Environment` for engine-wide settings — and the code that owns the data registers them:
Renderer_DX12 registers the entity properties because it owns `Entity`. The Sequencer's
property tracks and the Node Graph's Get/Set Entity Property nodes resolve through the
same registry, so a weather system that registers `Weather.RainIntensity` appears in both
without either core changing. The Node Graph's old display names ("Light Intensity") are
kept as registry aliases so existing graphs still load.

**4. A camera component.** Cinematics need cameras that are objects in the level.
`CameraComponent` joins `Entity` with focal length, sensor size, near clip and a
procedural shake; field of view is derived from focal length and sensor height, and both
are animatable. A camera faces its local +Y, the same convention as `PlayerStart`, so
`pitch = Rotation.x` and `yaw = -Rotation.z`. Aperture and focus distance are *not*
exposed yet: the renderer has no depth-of-field pass, and an animatable control with no
effect would be pretending.

**5. The runtime never touches the editor or the renderer directly.** The cinematic
runtime lives in `System/` and talks to the world through `CinematicHost` (resolve a
binding to an object, set the cut camera) — the same pattern as `NodeGraphHost` — so it
is unit-tested without an engine and runs unchanged in the packaged game.

**6. Undo is a command stack per sequence.** A Sequencer transaction records only what it
touched — the bindings (with their tracks) and the master tracks it changed — before
and after, never the whole editor. Moving a key in a 2 000-key sequence stores one
binding twice, not the level. The Sequencer window owns the stack, as the Node Graph
window owns its own.

**7. Evaluation is driven by time changes, not every frame.** The Sequencer evaluates
when the playhead moves or the sequence is edited, so an entity edited in the viewport
keeps its edit until the next scrub, and Auto Key can see the difference.

## Runtime (Phase 1) — `Source/System/include/System/Cinematics/`, `Source/System/src/Cinematics/`

| File | Contents |
|---|---|
| `Guid.h/.cpp` (`System/`) | 128-bit random UUIDs, text form `xxxxxxxx-xxxx-...` |
| `PropertyRegistry.h/.cpp` | `PropertyType`, `PropertyValue`, `PropertyDescriptor`, `PropertyRegistry` |
| `CinematicCurve.h/.cpp` | `Keyframe`, `FloatChannel` (all interpolation modes, auto tangents, binary search with a cached segment hint), `BoolChannel`, `IntChannel` |
| `SequenceTrack.h/.cpp` | `SequenceTrack` base, `PropertyTrack`, `CameraCutTrack`, `EventTrack`, and the track-type factory used by serialization |
| `CinematicSequence.h/.cpp` | `CinematicSequence`, `SequenceBinding`, `FrameRate`, JSON load/save |
| `SequencePlayer.h/.cpp` | `SequencePlayer` (play/pause/stop/seek/loop/rate, event firing across loop wraps, restore state), `CinematicHost`, `CinematicSystem` |

Entity properties are registered in `Renderer_DX12/EntityProperties.cpp`; the host that
applies sequences to the level and the camera is `Renderer_DX12/CinematicWorldHost.*`.

Sequences are JSON assets under `Data/Cinematics/*.sequence`. A binding stores the
entity's persistent id and its name (the name is a fallback when a sequence is used in
another level); nothing stores a pointer.

## Editor (Phase 2) — `Source/Editor/Sequencer/`

| File | Contents |
|---|---|
| `SequencerEditor.h` | Facade the editor calls: show, open, tick, Auto Key notification, camera lock |
| `SequencerWindow.cpp` | The window, toolbar, undo stack, preview player |
| `SequencerTimeline.h/.cpp` | Row model, track tree pane, timeline pane |
| `SequencerEditorHeadless.cpp` | Stand-in compiled into the packaged game's renderer |

Hooked from: Windows → Cinematic Editor, double-clicking a `.sequence` in the asset
browser, `Editor::MarkSceneChanged` (Auto Key), level save and play start (restore the
pre-animation state first), and `DX12SceneRenderer`'s camera update (lock and pilot).

## Using Ptero Sequencer

**Windows → Cinematic Editor** opens it as a dock along the bottom of the editor; double-
clicking a `.sequence` in the Asset Browser opens that sequence.

- **Add → Add Selected Entities** binds what is selected in the level. **Add Entity...**
  picks one by name; **Create Camera from View** drops a camera where the viewport looks
  from and binds it. The track list groups bindings into Cameras, Actors, Lights and
  Environment.
- The **+** on a binding row (or its context menu) adds a track for any animatable
  registered property, keyed with the current value at the playhead. The **◀ ◆ ▶**
  buttons on a track row jump to the previous key, key the row, and jump to the next key.
- Keys: click, Ctrl-click to toggle, Shift-click or drag a box to add. Drag to move (the
  grabbed key snaps to frames, the rest keep their spacing). Right-click for
  interpolation, tangents, copy, duplicate, scale and delete; double-click to edit a
  key's exact time and value (a colour track offers a colour picker).
- Camera Cuts: right-click or double-click the row to cut to a bound camera; drag a
  block's edges - the line between two cuts moves both.
- Events: double-click the Events row to add a marker; its name is what scripts listen for.
- Ruler: drag to scrub; drag the green/red handles to set the playback range;
  Ctrl-drag to mark a selection range.
- Keyboard (while the Sequencer has focus): Space play/pause, Shift+Space play from
  start, Left/Right step a frame, Home/End, S add key, Delete, Ctrl+C/V/D, Ctrl+Z/Y,
  A frame all, F frame selection, C Camera View. The wheel zooms the timeline about the
  cursor; Shift+wheel or middle-drag pans; Ctrl+wheel (or the wheel over the track list)
  scrolls the tracks.
- **Auto Key**: with it on, moving a bound entity (gizmo, Properties, piloting) keys the
  changed channels at the playhead, creating a Transform track if there is none.
- **Camera View** (toolbar, C, or Viewport → Lock to Cinematic Camera) looks through the
  cut at the playhead, matted to the render's aspect ratio; when the viewport is narrower
  than the render the field of view is widened so the frame inside the mattes is exactly
  what Render produces. **Pilot Camera** (camera Properties, a camera binding's menu, or
  Viewport → Pilot Selected Camera) flies a camera entity with the viewport controls.
  While piloting, the view follows the camera when the sequence moves it, and only
  writes back when you fly - so scrubbing with Auto Key on never keys the camera.
- **Render** writes the sequence to a PNG or JPEG image sequence, or an H.264 MP4 through
  the ffmpeg in `Binaries/ffmpeg`. The viewport's scene target switches to the render
  resolution for the duration, the scene clock advances exactly one frame per image
  (`DX12SceneRenderer::SetFixedFrameDelta`), and warm-up frames at the first pose let TAA,
  exposure and GI settle. Each image is read back two engine frames after its pose was
  set, which is the renderer's latency. Settings are saved in the sequence's metadata.
  MP4 has no audio until sequences have audio tracks; EXR is not offered because the
  final image is display-referred.

Previewing writes animated values into the level's entities. They are put back when the
Sequencer is closed, around every level save and for the length of a play session, so
the level is always saved and played as authored.

## The first working demo

`Data/Levels/CinematicDemo.level` holds Camera_A, Camera_B, PointLight and Cube (a 1 m
cube, `Data/Geometry/Basic/Cube.ptero`) on a floor at dusk. `Data/Cinematics/Intro.sequence`
is the brief's timeline: Camera_A pushes in over 0-3 s, the cube rises over 1-4 s,
"LightOn" fires at 2 s, the cut goes to Camera_B at 3 s, Camera_B turns toward the cube
over 3-5 s, the light ramps 0 → 5000 lm over 4-5 s, "IntroFinished" fires at 5 s.
Open the level, double-click the sequence, turn on the camera lock and press Play.

The graph half of the demo (Event BeginPlay → Play Sequence, On Sequence Event → Set
Light Color) needs the cinematic graph nodes of phase 8. Until then a game starts
sequences from code: `DX12SceneRenderer::GetCinematics().Play("Cinematics/Intro.sequence")`.

## Tests

`Source/Tests/Cinematics/Build.cmd` compiles the runtime and the Sequencer's document
model into a console program and runs it: curves and every interpolation mode, the
hinted key search against a brute-force reference, serialization round trips, playback,
looping, reverse play, event firing, restore state, undo/redo, key moves with snapping,
camera-cut editing, Auto Key, and the demo assets played end to end against the real
level file.

## Phases

1. **Core runtime** — done.
2. **Basic Sequencer** — done: timeline, playhead, playback, entity bindings, transform
   and property tracks, keys, camera component, camera cuts, lock and pilot, Auto Key.
3. Curve editor, animation clips (needs an animation system first), audio sections
   (FMOD events), material parameters, visibility (needs a visibility flag the renderers
   honour), subsequences. (Render Sequence was brought forward and is done.)
4–9. Ptero Graph, as decision 1 describes, then the cinematic nodes and the debugger.

Known gaps in what is done:

- Entities cannot be dragged from the Level Explorer into the Sequencer: QtUi has no drag
  source. Add Selected and Add Entity cover it.
- Camera roll (Rotation.y) is stored and animated but not shown: the renderer's view has
  no roll.
- Aperture and focus distance are absent until there is a depth-of-field pass.
