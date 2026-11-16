---
id: 05-plugin-selection-api
title: A plugin reads the selection, sets the face selection, and the selection survives deleting a modifier
priority: P1
timeout_minutes: 25
preconditions:
  - Setup wizard already completed in this settings folder, with a printer that is not a belt printer
  - The build contains the Face selection tool (build-info.json names a branch starting with feature/plugin-selection-api)
test_data:
  - data/orca_selection_plugin_any.py
---

## Setup

Start the app and maximize the window. Right-click the plate, choose **Add Primitive**, then
**Cylinder**, and accept the size dialog. Click the cylinder so it is selected.

## Steps

| # | Action | Expected result | Screenshot? |
|---|---|---|---|
| 1 | Open **File > Plugins**, choose **Install local plugin** and pick `data\orca_selection_plugin_any.py`. Approve any prompt | "Selection Inspector" is in the list of installed plugins | y |
| 2 | Expand the "Selection Inspector" row and press its run button (a triangle icon). Move the Plugins dialog out of the way | A panel titled **Selection Inspector** is docked beside the 3D view, with sections Target, Objects, Faces, Events, a row of buttons, and Output | y |
| 3 | Click the cylinder, then press **Run self-test** in the panel | Output starts with `SELF-TEST: N passed, 0 failed, M skipped`. Copy that line and every line starting with FAIL into the report | y |
| 4 | Click an empty part of the plate, then click the cylinder again | Objects shows `nothing selected`, then one line naming object 0 with "whole object" | n |
| 5 | With the Face selection tool closed, press **Select first 10** | Output says `Set 10 triangle(s) on object 0 ...`; Faces shows a total of 10 | y |
| 6 | Open the Face selection tool on the cylinder | 10 triangles are orange and the tool panel says "10 triangles selected" | y |
| 7 | With the tool still open, press **Select upward** in the plugin panel | The orange area changes at once to the flat top of the cylinder. The count in the tool panel equals the Faces total in the plugin panel | y |
| 8 | Press **Grow** | The orange area spreads from the top onto the upper rim of the side, and both counts rise together | y |
| 9 | With the **Triangle** tool, hold Shift and click one orange triangle | Both counts drop by 1 | n |
| 10 | Press **Done** in the tool panel. Note the Faces total | The highlight is gone; the Faces total is unchanged | n |
| 11 | Right-click the cylinder, choose **Add Modifier**, then **Cube**; confirm any dialog. Click the cylinder itself again | The object list shows the cylinder with a modifier part under it. The Faces total is unchanged | y |
| 12 | In the object list, click the modifier part and press Delete | The modifier is gone and the cylinder has a single part again | n |
| 13 | Look at Faces in the plugin panel, then open the Face selection tool on the cylinder | The Faces total is the same as in step 10, and the same triangles are orange | y |
| 14 | Press **Done**. Right-click the cylinder, choose **Modify**, then **Simplify Model**, and apply any reduction | Faces shows `no faces selected` (the mesh changed, so the selection is dropped) | y |
| 15 | Press **Clear faces**, then delete the cylinder | Faces shows `no faces selected`; the app keeps running | n |

## Pass criteria

All steps pass, the self-test line in step 3 reports 0 failed, and the selection in step 13
is the one from step 10.

## Known flaky areas

- Steps 1 and 2: the Plugins dialog was only seen on macOS. If the menu path or the run
  button differ, use the closest match and note it.
- Step 3: a SKIP line about "no modifier" is expected here, because the cylinder has no
  modifier yet.
- Steps 6 to 10: hold the mouse button about a third of a second on buttons inside the
  3D view, and find the Face selection button by the panel it opens.
- Step 12: if Delete removes the whole cylinder, undo once and use the modifier's
  right-click menu instead.

## Cleanup

Close the app without saving. Attach the plugin panel's Output text from step 3.
