---
id: 04-face-selection
title: The Face selection tool picks faces without changing the project or the undo history
priority: P1
timeout_minutes: 20
preconditions:
  - Setup wizard already completed in this settings folder
  - The build contains the Face selection tool (build-info.json names a branch starting with feature/plugin-selection-api)
  - The printer chosen in the setup wizard is not a belt printer
test_data: []
---

## Setup

Create the folder `C:\TestRuns\{run_id}\out` if it does not exist.

## Steps

| # | Action | Expected result | Screenshot? |
|---|---|---|---|
| 1 | Start the app and maximize the window. Right-click the plate, choose **Add Primitive**, then **Cylinder**; accept the size dialog. Add a **Cube** the same way | Two objects stand side by side on the plate | n |
| 2 | Save the project as `C:\TestRuns\{run_id}\out\faces.3mf` | The title shows no unsaved marker | n |
| 3 | Click the cylinder. In the row of tool buttons above the 3D view, click the button right after the three paint tools (support, seam, fuzzy skin) and before the star-shaped Painted Modifier button. If another tool's panel opens, close it and try the next button | A small panel opens with four tool icons, a line "0 triangles selected" and buttons **Clear**, **Clear all**, **Done** | y |
| 4 | Choose the **Triangle** tool icon and click once on the flat top of the cylinder | One triangle turns orange; the panel says "1 triangle selected" | y |
| 5 | Hold Shift and click the same triangle | Nothing is orange; the panel says "0 triangles selected" | n |
| 6 | Choose the **Fill** tool icon and click the curved side of the cylinder. Note the count in the panel | The whole curved side is orange; the flat top is not | y |
| 7 | Press **Done** | The panel closes and the orange highlight is gone | n |
| 8 | Look at the window title | Still no unsaved marker | y |
| 9 | Open the Face selection tool again on the cylinder | The curved side is orange again, with the same count as in step 6 | y |
| 10 | Press **Done**. Press Ctrl+Z once | The cube disappears (the last real change is undone). The Face selection panel does **not** open | y |
| 11 | Press Ctrl+Z again | One more earlier change is undone (the cylinder disappears, or moves back if it was moved during setup), or nothing changes. The Face selection panel does **not** open | y |
| 12 | Press Ctrl+Y until nothing changes any more | Cylinder and cube are back. The panel is still closed. The title shows no unsaved marker | y |
| 13 | Click the cylinder and open the Face selection tool. Leave it open. In the object list, click the cylinder's name, wait a second, click the name again slowly, type `renamed`, press Enter. Close the tool | The name is `renamed` | n |
| 14 | Rename the cylinder once more, to `renamed2`. Press Ctrl+Z twice | The name goes back to `renamed`, then to the original. The Face selection panel does not open at either step | y |
| 15 | Rename the cylinder to `again`. Open the Face selection tool on it and select a few triangles. Press Ctrl+Z once | The name goes back one step and the Face selection panel closes | y |
| 16 | Open the support painting tool from the same button row, paint a short stroke on the cylinder, then press Ctrl+Z once | The stroke is removed (paint tools still have their own undo) | y |

## Pass criteria

All steps pass. In particular the Face selection panel never opens as the result of
Ctrl+Z or Ctrl+Y (steps 10 to 15).

## Known flaky areas

- Step 1: if the cube lands on top of the cylinder, the printer is a belt printer. Undo
  the cube, drag the cylinder aside, add the cube again, and expect step 11 to undo that move.
- Step 3: tooltips do not show in screenshots, and the button row changes when an object
  has a modifier. Go by the panel that opens: Face selection's has **Clear all** and **Done**
  and a line counting triangles.
- Steps 13 to 15: a rename does not put the unsaved marker in the title. That is not
  part of this plan.
- Steps 4 to 7: these buttons are drawn inside the 3D view. Hold the mouse button about a
  quarter of a second; a faster click can be missed. If **Done** does not react, click
  the tool's button in the row again, and note that it happened.
- Steps 10 to 15: press the shortcut with the mouse over the 3D view, so the 3D view has
  keyboard focus.

## Cleanup

Close the app without saving. Collect the newest log file; its lines containing
`Undo / Redo snapshot taken` belong in the report.
