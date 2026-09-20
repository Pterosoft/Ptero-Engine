# Qt editor

The editor uses Qt Widgets from `Source/SDKs/qt`. DX12 renders directly into a native child viewport; additional native surfaces display the G-buffer debug textures. Qt paints viewport gizmos and placement icons. Neither editor project compiles or links Dear ImGui or ImGuizmo.

The interface keeps the top toolbar, central viewport, Components/Console/Level Explorer on the left, and Properties/Resource Debug/Audio Manager on the right. Auxiliary editors remain separate windows. The default Fusion palette uses neutral charcoal backgrounds, the system UI font at the size the system asks for - the same face Windows draws in a title bar, so the panels match the chrome around them - and the existing Pterosoft orange (`#c73d0d`) for selection and focus. Playfair Display remains the HUD font, loaded by `RmlUiRenderer` for the documents under `Data/UI`; that is game content, not editor chrome. Qt saves docking arrangements separately from the legacy `imgui.ini` files.

## Build

The supplied Qt directory is a source checkout, not an installed binary SDK. Run this from the repository root in PowerShell:

```powershell
./Source/QtUi/BuildQt.ps1 -Configuration Debug
```

Then build `Source/Ptero-Engine/Ptero-Engine.slnx` with Visual Studio, Debug/x64. For Release/x64, first run the same script with `-Configuration Release`. Both configurations install to `Source/SDKs/qt/install`; separate build directories live under `Cache`. The old CMake cache in the supplied source directory is not used.

`Source/QtUi/Qt.props` selects matching Qt debug/release libraries and copies the runtime DLLs and Windows platform plugin next to the renderer. A prebuilt compatible SDK can be selected with the MSBuild property `PteroQtRoot`.

## UI bindings

Numeric fields render as a slider plus a spin box whenever the caller supplies a finite range, which is what every `SliderFloat`/`SliderInt` and every bounded `DragFloat`/`DragInt` does. The spin box remains authoritative so the slider's resolution never becomes the precision a setting is stored at. Unbounded fields — transforms, and anything routed through `InputFloat` — keep the plain spin box, since there is no span for a track to represent.

`Source/QtUi/QtUi.cpp` reconciles the editor's control declarations with persistent Qt widgets and actions. Signals store pending values in widget-owned state. The next editor update consumes them synchronously, so callbacks never capture pointers to temporary setting values or entities invalidated by scene changes. Existing scene, material, terrain, import, audio, and render-setting operations remain in their original modules.

`QtViewportRenderer.cpp` samples engine descriptor handles into native DX12 swap chains. Engine headers use the toolkit-independent `UiTextureID` type. Qt input focus gates camera movement; editor selection and manual gizmos use Qt's viewport coordinates. Shutdown waits for the GPU before releasing renderer resources and destroying native windows.

Every non-docked panel is a `QDialog` with an ordinary window frame, carrying the full caption hints, so it has minimize, maximize and close boxes and responds to a double-click on the title bar. It is deliberately **not** a `Qt::Tool` window: a tool window gets `WS_EX_TOOLWINDOW`, and Windows draws nothing but a close box on that caption whatever style bits Qt asks for, which is why these panels were stuck at the size they opened at however the maximize hint was set.

Each one is owned by the shell, so it stays above the editor, and Windows gives an owned window no taskbar button - which is why minimize used to be left off the frame, since a minimized panel would have been somewhere the user could not get it back from. `QtUi` sets `WS_EX_APPWINDOW` on the native handle before the first show, so the button is there and minimize behaves as it does anywhere else. Switching a panel off and on again in the Windows menu also restores it, for a panel that was minimized and then closed.

Per-frame auto-sizing and repositioning both skip a window the user has maximized, minimized or made fullscreen, so nothing fights the choice.

A floating dock panel - Components, Properties, Console - needs the same correction, and needs it repeatedly. Qt makes a torn-off dock a `Qt::Tool` window with a close box and nothing else, and it rebuilds those flags from scratch whenever it touches the dock's window state, which includes every drag of an already-floating panel and not just the moment it leaves the shell. `QtUi` therefore re-asserts the ordinary window flags each frame rather than connecting once to `topLevelChanged`, and never while a mouse button is down: `setWindowFlags` destroys and recreates the native window, which would drop a drag in progress.

Every framed window opens at the size of what it holds. The body inside the scroll area is measured over the first few frames after the window appears - size hints only settle once the children exist and the style has polished them - and the window is resized to that, clamped to 90% of the screen with room left for whichever scroll bar the clamp brings on. A window is measured again each time it is reopened, because what it holds is not necessarily what it held when the user last closed it. A size passed to `SetNextWindowSize` is kept as the floor rather than the answer, so a panel that deliberately opens roomy keeps its room while anything larger still grows to fit; the flat 720x640 fallback that used to apply to everything without `AlwaysAutoResize` is gone, and that flag now describes what every framed window does anyway.

The undecorated HUD overlays (the statistics readout and the loading notice) keep their frameless, unfocusable flags and are not maximizable, which is correct - they are not windows the user manages.

`QtUiSmoke.cpp` exercises queued property edits and menu commands, panel reopening, native window ownership, resizing, font loading, and palette defaults without needing a DX12 device.

## Console

The Console panel, the session log under `Logs/`, and the cvar system that binds a name to every renderer setting are documented separately in [Console.md](Console.md).

## UI Editor

The RmlUi document viewer — preview, inspector and event trace — is documented in [UiEditor.md](UiEditor.md).

## Shortcuts and unsaved changes

| Shortcut | Command |
|---|---|
| `Ctrl+N` | New level |
| `Ctrl+O` | Open level |
| `Ctrl+S` | Save level (Save As when it has no path yet) |
| `Ctrl+Z` | Undo |
| `Ctrl+Y`, `Ctrl+Shift+Z` | Redo |
| `Ctrl+C` / `Ctrl+V` | Copy / paste the selected entity |
| `Delete` | Delete the selection |
| `1` `2` `3` `4` | No gizmo / translate / rotate / scale |
| `F11` | Fullscreen viewport |

All of them are suppressed while a text field has focus, so `Ctrl+Z` in the Console or a name field is the field's own undo.

Every File command — the menu items and the shortcuts alike — goes through `Editor`, rather than the menu duplicating the logic. That is not tidiness: `File → Open...` used to call the loader directly and could discard an edited level without asking, while `Ctrl+O`, which is the same command, asked. Anything that replaces the level now asks the same question in the same place.

The question is asked in three situations: New, Open, and closing the editor. Closing was the gap — the window's own close button tore the session down without a word. It now asks first, on `WM_CLOSE`, before any shutdown work begins, so **Cancel** simply carries on rendering.

### Undo

Undo is snapshot-based: a step is a copy of the entity list and the selection. Entities hold `shared_ptr` handles to their meshes, so a snapshot copies component values and shares the geometry — it is strings and POD, not megabytes.

The recording point is `Editor::MarkSceneChanged()`, which is what every mutation already called to light the unsaved-changes flag. That is what makes the coverage complete without auditing 183 call sites: **if an edit marks the level dirty, it is undoable**, and if it does not, it was never going to be saved either.

Two consequences worth knowing:

- Edits closer together than 400 ms become one step. Without that, a single gizmo drag would fill the whole history with sixty indistinguishable frames of itself and push the state worth returning to off the end. It also means two quick separate edits can merge.
- The history holds 64 steps and is cleared whenever the level is replaced — a load, a new level, a reset. Undoing across that boundary would restore entities from a level you are no longer editing.

Undo covers the **level**: entities, their components, their transforms and the selection. It does not cover render settings (those are cvars and live in the settings structs) or the node graph (which has its own editing session and its own revision counter).

## Light shape gizmos

A light's reach and cone are invisible until something is lit by them, which makes placing one guesswork — you move it, wait for the GI to converge, and infer from the result where it actually pointed. The viewport draws the shape the renderer will use:

| Type | Drawn as |
|---|---|
| Point | Three world-aligned circles at `Radius`, which read as a sphere from any angle. A second, fainter pair shows `SourceRadius` when it is non-zero, since that is what softens the shadow. |
| Spot | A cone: a cap circle at `Radius` with radius `Radius · tan(outer/2)`, four edges from the apex, and a fainter inner-cone circle when the inner angle differs from the outer. |
| Rect | The emitting quad in the light's local XY plane, a reach line along its facing direction (both ways when two-sided), and faint `Radius` circles. |

They use the light's own colour, brighter and finer-tessellated for the selected light.

The geometry is derived exactly the way `DX12SceneRenderer` derives the GPU light record — local −Z is the direction, local +X and +Y span the rectangle, cone angles are halved, and **entity scale is ignored because the renderer ignores it too**. A gizmo that disagreed with the shading would be worse than none.

This is projected line work on the QtUi overlay, like the placement icons and the terrain bounds, not a GPU pass — so it stays out of the G-Buffer and out of every temporal history that reads it. A wireframe only the editor can see must not be something TAA or the GI denoiser has to forget.

The gizmos share the placement-icon switch (`Viewport → Placement Icons & Light Shapes`): turning the icons off means asking for an unobstructed view of the render, which a cage of light wireframes would defeat just as much. That switch is also the escape hatch if a scene has enough lights for the overlay painter to feel them.

## Transform gizmo

### Sizing

The gizmo keeps a roughly constant size on screen however far away the entity is. It used to do that **per axis**: project one world unit along the axis, measure the pixels that covered, and extrapolate to the length that would cover 72 of them.

Perspective is not linear, so that extrapolation is only valid when the measured unit is small on screen *and* roughly perpendicular to the view. Point the camera down an axis and the projected unit shrinks towards nothing; the extrapolation then divides by it and asks for an axis thousands of units long, which projects somewhere absurd and gets clamped to the viewport edge. That was the gizmo exploding into lines across the screen — and because the rotate rings took their radii from the same three lengths, the rings exploded with it.

`Editor::ComputeGizmoWorldScale` replaces all three with one length derived from the depth the perspective divide actually uses:

```
worldPerPixel = 2 · viewZ · tan(fovY / 2) / viewportHeight
```

It cannot blow up, and axes pointing away from the camera now foreshorten instead of compensating — which is what they should do, and what makes the gizmo readable end-on. It returns zero when the pivot is behind the near plane, and both the drawing and the hit tests then do nothing for that frame.

Both paths call that one function, so **what is clicked is always what is drawn**. They previously each carried their own copy of the extrapolation.

### Axis highlighting

The handle under the cursor is worked out every frame, not only when the mouse goes down, and the drawing lights it up: the axis in use goes near-white and thicker, the others dim back. That applies to hover as well as to an active drag, so you can see what you are about to grab before you grab it.

One pick chain feeds both the highlight and the click, so the highlight can never point at a different handle than the click takes.

In Scale mode the three plane handles are one control — uniform scale — so they highlight together, which is what they actually are.
