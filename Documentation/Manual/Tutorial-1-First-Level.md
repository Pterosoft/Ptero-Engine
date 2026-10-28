# Tutorial 1: your first level

Time: about 20 minutes. You will build a small scene (a floor, a few objects, a lamp), set the
time of day, walk around it with Play, and save it.

You will use: the Components panel, Properties, the gizmo, the Material Editor, the Time of Day
window and Play. Every step names the menu or panel, so you never have to hunt.

## 1. Start a new level

**File → New** (Ctrl+N). The viewport shows an empty world with the ground grid at height 0
and, with the time of day on, a sky.

Get used to moving around before adding anything:

- Hold the **right mouse button** and move the mouse to look around.
- Keep holding it and press **W A S D** to fly, **E** / **Q** to go up and down.
- Still holding it, roll the **mouse wheel** to fly faster or slower.

## 2. Add a floor

1. In the **Components** panel (left), click **Geometry**. It stays highlighted.
2. Click on the grid in the viewport. An entity called `Geometry_1` appears there, selected.
   It has an empty Mesh component, so there is nothing to see yet.
3. In **Properties** (right), open the **Mesh** section and click **Select Geometry (.ptero)**.
   Choose `Data/Geometry/Basic/Plane.fbx.ptero`.
4. In the **Transform** section, set **Scale** to `20, 20, 1` to make a large floor. (Check the
   size against the grid and adjust; one grid square is one metre.)
5. At the top of Properties, rename it `Floor`.

## 3. Give the floor a material

1. **Windows → Material Editor...**, **Single Material** tab, **New**.
2. Set a **Base Colour** tint (a mid grey or a warm stone colour) and **Roughness** around 0.7.
   Leave **Metallic** at 0: a floor is not metal. (Metallic defaults to 1, which turns a plain
   surface into dark metal.)
3. If you have textures, use **Browse...** next to each slot (Base Colour, Normal, Roughness).
4. **Save As...** into `Data/Materials`, for example `MyFloor.json`.
5. Select the floor, and in its Mesh section click **Select Material** and choose `MyFloor.json`.

## 4. Add some objects

1. **Components → Geometry**, click near the middle of the floor.
2. **Select Geometry (.ptero)** → `Data/Geometry/Basic/Cube.ptero`. It is a 1 m cube.
3. Lift it onto the floor: in Transform set **Position Z** to `0.5` (half its height), or press
   **2** for the Move gizmo and drag the blue (Z) arrow.
4. Make a few more: **Ctrl+C**, **Ctrl+V**, then move each copy. Try **3** (Rotate) and **4**
   (Scale) on them. For neat placement, turn on **Options → Movement Snap** in the viewport's
   menu bar.
5. Give them a material the same way as the floor. Try a second material with **Metallic 1**
   and **Roughness 0.2** to see polished metal.

If you have your own models, import them with **Windows → Asset Browser... → Import...** and
pick the `.fbx`; it arrives with its base colour textures, ready to place. See
[3. Assets](03-Assets-And-Importing.md#models-fbx).

## 5. Add a light

1. **Components → Point Light**, click next to the cubes.
2. In Properties, set **Position Z** to about `2` so it hangs above them.
3. In the light's section set **Intensity** to `800` lumens (a 60 W bulb), **Radius** to `8`,
   and tick **Use Temperature** with `2700` K for a warm bulb.

The viewport draws the light's reach as three circles. You will see the light more clearly in
the evening; that is the next step.

## 6. Set the time of day

1. **Windows → Time of Day...**
2. Drag **Time Of Day** to `18.5`. The sun sets, the sky turns orange, and the exposure adjusts
   automatically.
3. Keep going to `21`. The sky darkens, the moon comes up, and your point light becomes the main
   light. Notice the bounce light it throws onto the floor and cubes: that is global
   illumination.
4. Pick the time you like. Try **Turbidity** 4–5 for a hazier sky.

If the scene looks noisy for a moment after a big change, that is the GI settling; it clears in
a second.

## 7. Choose where the player starts (optional)

Without a start point, the player appears wherever the editor camera is. For a fixed start,
double-click **Camera** in the Components panel to drop a camera where you are looking from, and
rename it `PlayerStart`. The player spawns at its position, facing the way it looks.

## 8. Play

Press **Play** on the toolbar (the gamepad button).

- Move with **W A S D**, look with the mouse, jump with **Space**.
- Walk up to the cubes: they are solid. Every mesh collides automatically.
- **Escape** frees the mouse; click the view to look again.
- Press **~** (the key under Escape) to stop.

Everything returns to how it was when you pressed Play.

## 9. Save

**File → Save** (Ctrl+S). The first save asks for a name: save into `Data/Levels` as
`MyFirstLevel.level`. The level remembers its entities, its time of day and its graphics
settings, and the camera position you saved from.

## What you learned

- Entities are placed from **Components** and set up in **Properties**.
- A **Mesh** needs geometry and a material; materials are physically based (colour, roughness,
  metallic).
- Lights use real units; the **time of day** drives the sun, sky and exposure.
- **Play** tests the level as a game, and never changes it.

Next: [Tutorial 2: a first-person level with a door](Tutorial-2-First-Person-Door.md).
