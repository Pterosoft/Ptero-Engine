# 12. Cinematics: Ptero Sequencer

The **Sequencer** animates entities over time on a timeline: cameras moving and cutting, lights
fading, objects rising, events firing at set moments. Use it for cutscenes, trailers, flythroughs
and rendered videos.

Open it with **Windows → Cinematic Editor...** (a dock along the bottom of the editor), or by
double-clicking a `.sequence` file in the Asset Browser. Sequences are saved in
`Data/Cinematics`.

## Concepts

| Term | Meaning |
| --- | --- |
| **Sequence** | One timeline, saved as a `.sequence` file. It has a frame rate and a playback range. |
| **Binding** | An entity the sequence animates. It is stored by the entity's permanent id (with its name as a fallback), so renaming the entity doesn't break it. The track list groups bindings into Cameras, Actors, Lights and Environment. |
| **Track** | One animated property of a binding: its transform, a light's intensity or colour, a camera's focal length, and so on. Any property the engine registers as animatable can have a track. |
| **Key** | A value at a time. Between keys the value is interpolated. |
| **Camera Cuts track** | Which camera the shot looks through at each moment. |
| **Event track** | Named markers that fire at set times, for scripts to react to. |

## Building a shot

1. **Cameras.** Double-click **Camera** in the Components panel to drop a camera where the
   viewport is looking from, or use **Add → Create Camera from View** in the Sequencer, which also
   binds it. Set its focal length on the Camera component.
2. **Bind what moves.** Select entities and **Add → Add Selected Entities**, or **Add Entity...**
   to pick by name.
3. **Add tracks.** The **+** on a binding row (or its right-click menu) adds a track for any
   animatable property, keyed with the current value at the playhead.
4. **Key it.** Move the playhead, change the value, key it. The **◀ ◆ ▶** buttons on a track jump
   to the previous key, add a key here, and jump to the next key.
5. **Cut between cameras.** Right-click or double-click the Camera Cuts row to cut to a bound
   camera at the playhead. Drag a cut's edge to retime it; the line between two cuts moves both.
6. **Events.** Double-click the Events row to add a marker. Its name is what scripts listen for.

### Auto Key

With **Auto Key** on, moving a bound entity (with the gizmo, in Properties, or by piloting) keys
the changed values at the playhead, creating a Transform track if there is none. The fastest way
to block out motion: move the playhead, move the thing, repeat.

### Looking through the camera

- **Camera View** (toolbar, **C**, or **Viewport → Lock to Cinematic Camera**) shows the shot
  through whichever camera the cut track has at the playhead, with letterbox mattes matching the
  render's aspect ratio, so the framing is exactly what Render will produce.
- **Pilot Camera** (camera's Properties, a camera binding's menu, or **Viewport → Pilot Selected
  Camera**) flies a camera entity with the normal viewport controls. The view follows the
  sequence when it moves the camera, and only writes back when *you* fly, so scrubbing with Auto
  Key on never accidentally keys the camera.

## Editing keys

| Action | How |
| --- | --- |
| Select | Click; Ctrl-click toggles; Shift-click or drag a box adds |
| Move | Drag. The grabbed key snaps to frames; the others keep their spacing. |
| Exact time and value | Double-click a key (colour tracks get a colour picker) |
| Interpolation, tangents | Right-click: interpolation mode; tangents Auto, Unified or Broken |
| Copy, duplicate, scale, delete | Right-click, or Ctrl+C / Ctrl+V / Ctrl+D / Delete |

### The ruler

Drag in the ruler to scrub. Drag the **green** and **red** handles to set the playback start and
end. **Ctrl**-drag marks a selection range (right-click to set the playback range to it).

### Keyboard (while the Sequencer has focus)

| Key | Does |
| --- | --- |
| Space / Shift+Space | Play-pause / play from the start |
| Left / Right | Step one frame |
| Home / End | Jump to start / end |
| S | Add a key |
| A / F | Frame all / frame the selection |
| C | Camera View |
| Ctrl+Z / Ctrl+Y | Undo / redo (the Sequencer has its own undo history) |
| Wheel | Zoom the timeline around the cursor |
| Shift+wheel, middle-drag | Pan |
| Ctrl+wheel (or wheel over the track list) | Scroll the tracks |

## Previewing never changes your level

Scrubbing and playing write animated values into the level's entities so you can see them. The
original values are put back when the Sequencer closes, around every level save and for the
length of a play session. The level is always saved and played as you authored it.

## Rendering to images or video

**Render** (on the toolbar) writes the sequence to a **PNG** or **JPEG** image sequence, or an
**H.264 MP4** (MP4 needs ffmpeg in `Binaries/ffmpeg`).

- The renderer switches to the render resolution for the duration.
- Time advances exactly one frame per image, however long each frame takes to render, so
  heavy settings (path tracing, high-quality GI) render correctly.
- Warm-up frames at the first pose let TAA, exposure and GI settle before the first image.
- Render settings are saved in the sequence.

MP4 output has no audio yet.

## Playing a sequence in the game

There is no Node Graph node for playing a sequence yet; a game starts sequences from C++ code.
The camera cut track drives the game camera while a sequence plays, and its event markers fire
as the playhead passes them.

## Demo

`Data/Levels/CinematicDemo.level` with `Data/Cinematics/Intro.sequence`: Camera_A pushes in over
0–3 s while a cube rises, "LightOn" fires at 2 s, the shot cuts to Camera_B at 3 s and turns
towards the cube, the light ramps up to 5000 lm, and "IntroFinished" fires at 5 s. Open the
level, double-click the sequence, turn on Camera View and press Play.

## Current limitations

- Entities cannot be dragged from the Level Explorer into the Sequencer; use Add Selected.
- Camera roll is stored and animated but not shown.
- No depth of field (no aperture or focus distance) yet.
- No audio tracks, animation clips or subsequences yet.
