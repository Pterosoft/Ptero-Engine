# Tutorial 2: a first-person level with a door

Time: about 30 minutes. You will build a room the player can walk around, put a door in it, and
write your first Node Graph script: look at the door, press **E**, and it slides open.

Do [Tutorial 1](Tutorial-1-First-Level.md) first; this one assumes you can place, move and scale
entities and assign materials.

## 1. Build a room

Start with **File → New**, then build the room from cubes (`Data/Geometry/Basic/Cube.ptero`, a
1 m cube whose pivot is its centre). Use these transforms as a starting point:

| Entity | Position (X, Y, Z) | Scale (X, Y, Z) |
| --- | --- | --- |
| `Floor` | 0, 0, -0.05 | 10, 10, 0.1 |
| `Wall_Back` | 0, 5, 1.5 | 10, 0.2, 3 |
| `Wall_Left` | -5, 0, 1.5 | 0.2, 10, 3 |
| `Wall_Right` | 5, 0, 1.5 | 0.2, 10, 3 |
| `Wall_Front_L` | -3, -5, 1.5 | 4, 0.2, 3 |
| `Wall_Front_R` | 3, -5, 1.5 | 4, 0.2, 3 |
| `Wall_Front_Top` | 0, -5, 2.5 | 2, 0.2, 1 |
| `Ceiling` | 0, 0, 3.05 | 10, 10, 0.1 |
| **`Door`** | 0, -5, 1 | 2, 0.1, 2 |

The front wall has a 2 m wide, 2 m high gap in the middle, and the `Door` fills it. Naming the
door exactly `Door` matters: the script will look for that name.

Give the walls and the door materials (Tutorial 1, step 3). A wood-coloured door with Roughness
0.6 reads well.

> **Tip:** build one wall, then copy and paste it (Ctrl+C, Ctrl+V) and type the new numbers into
> Transform. Typing values is quicker and more exact than dragging for architecture.

## 2. Light it

- Leave **Time of Day** on and set it to about `16` so sunlight comes in when the door opens.
- Add a **Rect Light** inside the room, near the ceiling: Position `0, 0, 2.9`, **Width** `1`,
  **Height** `1`, **Intensity** `2000` lm. Unrotated, a rect light shines straight down.
- Check **Windows → Graphics Settings... → RTGI** is enabled (on a GPU without ray tracing,
  voxel GI steps in automatically).

## 3. Place the player start

Stand the editor camera inside the room at head height, looking at the door. Double-click
**Camera** in the Components panel and rename the new camera `PlayerStart`.

Press **Play** to check. Walk with **W A S D**, look with the mouse, **Space** to jump, **Shift**
to sprint, **Ctrl** or **C** to crouch. The walls and the closed door are solid: every mesh
collides. Press **~** to stop.

## 4. Write the script

Open **Windows → Node Graph...**. This is the level's script; it is saved inside the level.

Navigating the graph: right-drag pans, the wheel zooms, **Q** opens the node list at the pointer
(type to filter), **Delete** removes the selection. Connect pins by dragging from one to another:
white pins carry execution ("then do this"), coloured pins carry values.

Add these nodes with **Q** and connect them:

1. **On Key Pressed**. On its body, set **Key** to `E`.
2. **Raycast From Camera**. Set **Distance** to `2` (metres). Connect On Key Pressed's white
   output to its white input. It reports what the player is looking at within 2 m.
3. **Get Entity Name**. Connect Raycast From Camera's **Hit Entity** to its Entity input.
4. **Text Equals**. Connect Get Entity Name's name output to **A**; type `Door` into **B**.
5. **And**. Connect Raycast From Camera's **Hit** to **A**, and Text Equals' result to **B**.
6. **Branch**. Connect Raycast From Camera's white **Then** to Branch's white input, and the
   And's result to **Condition**.
7. **Do Once**. Connect Branch's **True** to it, so the door only opens once.
8. **Move Entity To**. Connect Do Once's output to it. On its body:
   - **Entity**: pick `Door` from the list (or select the door in the viewport and press ◎);
   - **X, Y, Z**: `2, -5, 1`, which slides the door 2 m sideways, into the wall;
   - **Duration** `1` second, **Easing** `Smoother`.

Read it left to right: *when E is pressed, look 2 m ahead; if we hit something called Door, and
haven't already, slide the door open over one second.*

## 5. Test it

Press **Play**, walk to the door, look at it and press **E**. The door slides into the wall and
the evening light comes in. Stop with **~**: the door is closed again, because play never
changes the level.

If nothing happens:

- Check the door entity is named exactly `Door` (Text Equals compares the text exactly).
- Stand closer: the ray only reaches 2 m.
- Add a **Print String** after the Branch's **False** output and watch the **Console**
  (**Windows → Console**) while you press E. If it prints, the ray is missing the door.
- The Node Graph logs errors to the Console too, such as an entity that no longer exists.

## 6. Make it nicer (optional)

- **Sound**: after Move Entity To's **Then**, add **Play Sound** with the name of an event from
  your FMOD banks (see **Audio → Audio Manager...** for the list).
- **A light that switches on**: add **Set Entity Property** after the door opens, pick the rect
  light, property *Light Intensity*, value `5000`.
- **Close it again**: replace Do Once with **Flip Flop**; send **A** to the open move and **B**
  to a second Move Entity To back to `0, -5, 1`.
- **A pickup**: place a small cube named `Key`. Copy the script, change the name to `Key`, and
  have Move Entity To send the key away (for example to `0, 0, -100`) with **Arc Height** `0.5`
  so it hops, then **Set Variable** `HasKey` to true. Gate the door's Branch on `HasKey` with
  another And, and the door needs the key first.

## 7. Save

**File → Save** (Ctrl+S) as `Data/Levels/MyDoor.level`. The script is saved inside the level.
To reuse it in other levels, **File → Export Graph...** in the Node Graph window.

## What you learned

- The player character needs no setup; `PlayerStart` decides where it appears.
- Scripts start from **events** (On Key Pressed) and run along white execution wires.
- Values flow along coloured wires; unconnected inputs use the values typed on the node.
- **Raycast From Camera** is how a script knows what the player is looking at.
- Latent nodes like **Move Entity To** animate over time.

Next: [11. Gameplay](11-Gameplay.md) lists every node, and covers physics, game UI, audio and
video.
