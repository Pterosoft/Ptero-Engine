# 11. Gameplay

## Play sessions

**Play** runs the open level as a game (see [2. The editor](02-The-Editor.md#play) for the
controls). What happens when you press it:

1. The level is copied. The game plays on the copy; when play stops, the copy is thrown away
   and the level is exactly as you left it. Nothing that moves, breaks or changes during play is
   ever saved.
2. The **physics** world is built: every mesh becomes static collision, terrain becomes a height
   field, entities with a Rigid Body join the simulation.
3. The **player character** spawns at the entity named `PlayerStart` (feet at its position,
   facing its local +Y), or at the editor camera if there is none.
4. The **player controller** chosen in Game Settings takes over (C++ or Node Graph).
5. The level's **Node Graph** starts: *On Game Start* fires, then *On Tick* every frame.
6. Audio emitters with *Auto Play* start.

Each frame, in order: the level graph and the controller graph tick, the character moves,
physics steps, and the frame is drawn.

## The player character

Ptero has one built-in first-person character: a capsule that walks, sprints, crouches, jumps
and falls, colliding with meshes, terrain and rigid bodies, with an eye camera on top that has
crouch blending, head bob, a landing dip and a field-of-view kick when sprinting. It can stand
on and push physics objects.

Something has to drive it: the **player controller**, chosen in **Windows → Game Settings... →
Player Controller**:

- **C++** (`Game.dll`, `FirstPersonCharacter`): the built-in controller, written in code.
- **Node Graph**: a `.nodegraph` file whose Character nodes drive the same character. Game
  Settings' **Export Node Graph Controller...** writes a node-graph copy of the C++ controller
  (`Data/Game/Controllers/FirstPersonController.nodegraph`) to start from and modify.

The controller decides *when* to move; the character decides *how*. The controller also owns the
character's configuration: speeds, acceleration, jump height, gravity, capsule size, field of
view, eye heights, head bob, look limits, mouse sensitivity, Invert Y, crouch/sprint toggling,
mouse capture. In a Node Graph controller these are the **Set Movement / Camera / Control
Settings** nodes run from *On Game Start*.

## Node Graph: visual scripting

**Windows → Node Graph...** opens the level's script. It works like Unreal's Blueprints:

- **White execution wires** carry "do this, then this". An event fires, and control flows
  along the white wires from node to node.
- **Coloured value wires** carry data (numbers, text, true/false, entities). A node pulls the
  values it needs when it runs.
- Any input left unconnected uses the value typed on the node itself.

The graph is saved **inside the level**. **File → Export Graph...** saves it as a `.nodegraph`
file, so several levels can share one script.

### Working in the canvas

| Input | Does |
| --- | --- |
| Right-drag | Pan |
| Mouse wheel | Zoom |
| Left-click / left-drag | Select / box-select; drag a node to move it |
| **Q** | Node list at the pointer, filtered as you type |
| **C** | Wrap the selection in a comment box (or drop an empty one) |
| Delete | Delete the selection |
| Drag from the Palette | Place that node |
| Drag from Variables | Choose Get or Set, then place it |
| Double-click a comment title | Edit its text and colour |

### Events: where scripts start

| Event | Fires |
| --- | --- |
| On Game Start / On Game Stop | When play begins / ends |
| On Tick | Every frame (outputs the frame time) |
| On Key Pressed / Released | A key, while the game window has focus |
| On UI Button Clicked | A button in the game UI (by its id) |
| On Landed, On Jumped | The player character |
| On Rigid Body Hit | Something starts touching a body, with the other entity, whether it was the player, and the impact speed (Min Impact Speed filters out settling) |
| On Video Finished | A non-looping video reached its end |

### What the nodes can do

| Category | Nodes |
| --- | --- |
| Flow | Branch, Sequence, Delay, Gate, Do Once, Do N, Flip Flop, For Loop, Switch |
| Logic | And, Or, Not, Compare, Select (pick one of two values), Text Equals, Multiplexer |
| Math | Add, Subtract, Multiply, Divide, Power, Clamp, Lerp, Min, Max, Modulo, Abs, Floor, Sin, Cos, Ease, Lerp Angle, Random Range / Integer / Chance; bitwise And / Or / Xor, Is Bit Set, Set Bit, Count Bits |
| Values, Variables | Number, Boolean, String, Append, To String, Format Number, Format Text; Get / Set Variable |
| Lists | Make List, Get Item, Set Item, Add Item, List Length, Count Item, Last Items, Join List |
| Entity | Entity (pick one), Find Entity, Is Entity Valid, Get Entity Name; Get / Set / Move By / Rotate By position, rotation and scale; **Move Entity To** and **Rotate Entity To** (animated over a duration with easing; Move has an arc height, Rotate can add extra turns); Get / Set Entity Property (light intensity, radius, colour and temperature, shadows, particles on/off and rate, rain on/off and intensity); Play / Stop Entity Audio |
| Physics | Add Impulse, Add Force, Set Velocity, Set Spin, Get Velocity, Raycast, Raycast From Camera |
| Character | Add Movement Input, Add Look Input, Jump, Stop Jumping, Crouch, Uncrouch, Set Sprinting, Get Character State / Location, Teleport Character, Get / Set Movement and Camera properties |
| Input | Is Key Down, Get Input Axis, Get Look Input, Get Mouse Delta, Set / Is Mouse Captured |
| Camera | Get Camera, Set Camera, **Move Camera To** (animated), Release Camera, Camera Look Point |
| UI | Show / Close / Reload UI Document, Set UI Visible, Set UI Input Enabled, Set UI Text, Set UI Property, Set UI Class, Set UI Element Visible, Set UI Element Enabled |
| Steam | Unlock Achievement, Is Achievement Unlocked, Show Achievement Progress, Clear Achievement, Set Stat, Add to Stat, Get Stat, Is Steam Running |
| Audio | Play Sound, Play Music, Play Music Playlist (shuffled, never repeats a track), Stop Music, Is Music Playing |
| Video | Play, Pause, Resume, Stop, Seek Video; Set Video Looping / Volume; Is Video Playing; Get Video Time / Duration |
| Game | Stop Game, Toggle Fullscreen, Is Standalone, Get Game Setting, Get Play Time, Print String |

Units: positions in metres, rotations and camera angles in **degrees**, forces in newtons,
impulses in newton-seconds.

### Referring to entities

Entity nodes have an **Entity** picker listing the level's entities by name; its ◎ button takes
whatever is selected in the viewport. The reference is the entity's permanent **id**, so it
survives renaming, reordering and saving. A reference to a deleted entity shows as `<missing id>`
and logs an error when it runs instead of quietly hitting the wrong thing.

### Latent nodes

**Delay**, **Move Entity To** and **Move Camera To** take time. They fire **Then** at once and
**Completed** when they finish. Firing one again restarts it.

**Set Camera** and **Move Camera To** *hold* the camera until **Release Camera**, overriding the
player's view (for cutscenes, a death camera, a menu backdrop).

### Variables

Declared in the Variables dock with a name, a type (Bool, Int, Float, Double, String, Text) and a
default. Set Variable converts to the declared type, so a variable never changes type.

### Functions and libraries

A **Function Entry** node declares a function; everything connected after it is the body,
**Return** ends it early, **Call Function** runs it. Functions take no parameters and return
nothing: pass data through variables (the same rule as Blueprint functions).

To reuse a graph: **File → Export Graph...** it, then in another graph **File → Import Graph as
Library...** with a name. Its functions become callable as `Name.Function`. The library is
copied in (not linked), and everything in it is prefixed with its name so its variables never
collide with yours. Re-import to pick up changes.

### Safety

A runaway graph cannot freeze the editor: each frame has a budget of 20 000 node executions and
a recursion limit, and a graph that hits them stops and logs why.

### Example: open a door by looking at it and pressing E

```
On Key Pressed (Key: E)
  → Raycast From Camera (Distance: 2)
  → Branch (Condition: Hit AND Get Entity Name(Hit Entity) Text Equals "Door")
  → Do Once
  → Move Entity To (Door, the open position, 1 s, Smoother)
```

[Tutorial 2](Tutorial-2-First-Person-Door.md) builds this step by step.

## Physics

Physics uses **Jolt** and only runs during play.

- Every **mesh** collides as static geometry (its generated collision hulls if it has them, its
  exact triangles otherwise). **Terrain** collides as a height field. Water and vegetation
  areas do not collide.
- Entities with a **Rigid Body** move: Dynamic bodies fall and get pushed; Kinematic ones are
  moved by scripts and push dynamic ones. See [4. Components](04-Components.md#rigid-body).
- The **player** is not a physics body; it moves itself with collision rays. A kinematic capsule
  follows it so it shoves dynamic bodies aside, and it can stand on them.
- Physics steps at a fixed **60 Hz** and the drawn positions are interpolated between steps, so
  motion is smooth at any frame rate.
- Collisions are reported to the graph through **On Rigid Body Hit** on the next frame.

A push or a shot: **Raycast From Camera**, then **Add Impulse** on the *Hit Entity*, along the hit
direction.

## Game UI

Game interfaces (HUDs, menus, settings screens) are **RmlUi** documents: HTML-like `.rml` files
styled with CSS-like `.rcss` files, in `Data/UI`. If you have written a web page, you can write
a Ptero UI.

- Show a document from the Node Graph (**Show UI Document**) or from game code.
- Change it at runtime by element **id**: Set UI Text, Set UI Property (any style property),
  Set UI Class, Set UI Element Visible. Each reports Success, so a typo in an id shows up.
- Buttons fire **On UI Button Clicked** with their id.
- Fonts come from `Data/Fonts`; images resolve relative to the document.

### The UI Editor

**Windows → UI Editor** previews a document at real game resolutions without playing:

1. Pick a document from the list (everything under `Data/UI`, including subfolders) and **Load**.
2. Edit the `.rml` / `.rcss` in any text editor, press **Reload**.
3. Tick **Interactive** and click it.

**Inspector** opens RmlUi's own debugger (element tree, computed styles, box model). **Trace
events** shows every click, hover, focus and key event reaching the document:

| You see | It means |
| --- | --- |
| `click button#start` | The click arrived. If nothing happened, the handler is missing. |
| `mouseover` but no `click` | Press and release landed on different elements, or something swallowed the press. |
| Nothing | The click never reached the document: something covers the element, or it is outside it. |

The **Element** section calls the same Set Text / Property / Class / Visible functions the graph
uses, to test them by hand.

While playing, the viewport's **Options → Game UI Inspector** and **Reload Game UI** do the same
for the live game UI.

## Audio

Sound is **FMOD Studio** (see [3. Assets](03-Assets-And-Importing.md#audio-fmod) for the bank
workflow).

- **Audio Emitter** components play an event from a position.
- **Play Sound** fires a one-shot event; **Play Music** plays on a single music channel that
  replaces what was playing; **Play Music Playlist** shuffles tracks without repeats.
- Events can be named by bare name; the engine finds the full path.
- **Spatial audio** uses Google's **Resonance Audio** through FMOD: a spatialised event carries a
  Resonance Source and Listener, set up in the FMOD project.
- The player's volume settings (overall, music, sound) scale everything at runtime.

## Video

The engine plays **WebM** video (VP8/VP9) with its Opus soundtrack, kept within about 20 ms of
the picture.

- **Preview**: double-click a `.webm` in the Asset Browser, or **Windows → Video Player...**.
  Space plays/pauses, L loops, M mutes, arrows skip and change volume, the timeline scrubs.
- **In game**: the Video nodes drive one full-screen video layer, drawn over the scene and under
  the game UI (so a "Skip" prompt can sit on top). **Play Video** takes a path or just a file
  name, a fit mode (Letterbox, Fill, Stretch) and a volume. **On Video Finished** fires when a
  non-looping video ends.

A typical intro:

```
On Game Start → Play Video ("Intro", Loop off, Letterbox)
On Video Finished → Show UI Document ("menu.rml")
```

The video's clock is the game's, so a paused game pauses the video.

## Writing gameplay in C++

For programmers, `Game.dll` is a C++ module the engine loads when play starts and unloads when
it stops, so you can rebuild it and press Play again without restarting the editor. The built-in
first-person controller lives there. This manual does not cover the C++ API.
