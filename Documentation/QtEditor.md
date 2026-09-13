# Qt editor

The editor uses Qt Widgets from `Source/SDKs/qt`. DX12 renders directly into a native child viewport; additional native surfaces display the G-buffer debug textures. Qt paints viewport gizmos and placement icons. Neither editor project compiles or links Dear ImGui or ImGuizmo.

The interface keeps the top toolbar, central viewport, Components/Console/Level Explorer on the left, and Properties/Resource Debug/Audio Manager on the right. Auxiliary editors remain separate windows. The default Fusion palette uses neutral charcoal backgrounds, Playfair Display Regular at 18 pixels from `Data/Fonts`, and the existing Pterosoft orange (`#c73d0d`) for selection and focus. Qt saves docking arrangements separately from the legacy `imgui.ini` files.

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

Every non-docked panel is a `QDialog` with a `Qt::Tool` frame, created with the full caption hints so it has a maximize box and responds to a double-click on the title bar - the UI Editor and the G-Buffer debug view are the ones that need the room, but a fixed-size tool window is a nuisance everywhere. Minimize is deliberately absent: a tool window has no taskbar button, so a minimized one would be somewhere the user could not get it back from. Per-frame auto-sizing and repositioning both skip a window the user has maximized, so nothing fights the choice.

The undecorated HUD overlays (the statistics readout and the loading notice) keep their frameless, unfocusable flags and are not maximizable, which is correct - they are not windows the user manages.

`QtUiSmoke.cpp` exercises queued property edits and menu commands, panel reopening, native window ownership, resizing, font loading, and palette defaults without needing a DX12 device.

## Console

The Console panel, the session log under `Logs/`, and the cvar system that binds a name to every renderer setting are documented separately in [Console.md](Console.md).

## UI Editor

The RmlUi document viewer — preview, inspector and event trace — is documented in [UiEditor.md](UiEditor.md).
