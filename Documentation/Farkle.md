# Farkle game implementation

Code is in the existing `Source/Game` project. **No build, test, or debug run was performed for these changes**, as requested. Rebuild QtUi, Renderer_DX12 (including its DX12 context), and Game. The shared Game API remains version 2.

## Play and level

The editor Play button now runs Game.dll **inside the existing engine process**. It creates a maximized game window and switches presentation at a GPU frame boundary. The existing device, renderer, shader state, and asset cache stay alive; Play does not launch another executable or repeat engine startup. Each window retains a swap chain, but only the active one is rendered. The inactive editor viewport retains its last frame. The native viewport windows are never reparented.

If Farkle is open, Play uses the edited entities, including unsaved changes, and keeps an in-memory backup of the editor scene. Mesh references reuse loaded assets. If a different level is open, Play temporarily loads `Data/Levels/Farkle.json` into the same renderer and restores the previous scene afterwards. The editor's filename, dirty state, selection, undo history, camera, and scene settings are preserved. No temporary play JSON or changes to the authored level are written. The game requires `Table` and `Dice1`…`Dice6`; names with spaces such as `Dice 6` also work.

Editor controls are temporarily disabled while the game owns rendering, and its panels remain in place. Close the game window or use **Exit to Editor** to restore the scene and return rendering to the original viewport. Escape pauses; F11 or the Fullscreen button toggles borderless fullscreen. The retained game window and swap chain are reused on the next Play. Game startup failures restore the editor and appear in the Play tooltip.

The optional explicit `--game <level-path>` startup mode remains available, but the editor's Play button does not use it.

## Game and controls

- Player vs. computer; 3,000-point target; the other player gets one final reply. Ties continue with alternating sudden-death turns.
- Single 1 = 100, single 5 = 50. Three ones = 1,000; other triples = 100 × face. Each additional matching die doubles the triple score. Straight = 1,500; three pairs = 750.
- All selected dice must score. Combinations cannot span separate throws. Scoring all six gives hot dice. Farkling loses the entire unbanked turn.
- R rolls, B banks, C clears. Click dice controls or press 1–6 to select; click selected slots to deselect. F1 opens help, Escape pauses, F11 toggles fullscreen. Native button focus supports Tab and Enter/Space.
- Rematch starts another match. Main Menu and Continue return to the menu; New Match starts again. No campaign/progression system is implied by Continue.

The UI now receives live scores, dice values/selection, action availability, turn messages, log entries, and result statistics. The former cosmetic victory bonus is replaced by the match target. Game callbacks are queued and processed during Update, so loading another RML document never destroys the DOM during a click event. `menu.rml` and `transition.rml` accompany the existing HUD/Victory/Defeat screens.

## Camera and throw configuration

Edit **`Source/Game/FarkleConfig.h`**:

- Table camera: position `(10.81, -1.61, 17.87)`, pitch/yaw `(-81.4°, -88.5°)`.
- Result camera: position `(20.05, -18.40, 10.54)`, pitch/yaw `(-6.1°, -40.4°)`.
- Travel lasts 2.4 seconds with smooth position interpolation and shortest-path yaw interpolation. Results appear on arrival. Rematch smoothly returns to the table. The transition document is empty/transparent: no fade or black overlay.
- Dice rolls animate entity position and XYZ rotation: pull back, arc forward while tumbling, then two diminishing settling bounces. Held dice stay still. Final poses exactly match the chosen face configuration. This is a controlled throw animation, not rigid-body collision simulation.
- The authored positions are the dice resting locations. Place them on the tabletop in the level. Throw distance, height, bounce height, duration, and individual face rotations are centralized in the configuration header.

**Required calibration:** the six `FaceRotations` entries are provisional orientations because the imported mesh's face directions were not supplied. Set entry 0 to show face 1 upward, through entry 5 for face 6. Euler values are radians in the engine's X→Y→Z composition. Until calibrated, the visible model pips may differ from the authoritative game/UI result. No claim of in-engine verification is made.

The result and HUD fonts retain their existing local font configuration. If relocating the project, use the previously supplied font-path relocation script.
