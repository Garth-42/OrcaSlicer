---
id: 05-plugin-selection-api
title: A plugin reads the selection, sets the face selection, and the selection survives deleting a modifier
priority: P1
timeout_minutes: 25
preconditions:
  - Setup wizard already completed in this settings folder, with a printer that is not a belt printer
  - The build contains the Face selection tool (build-info.json names a branch starting with feature/plugin-selection-api)
  - Desktop at least 900 pixels high (see display in testbox.yml)
test_data:
  - data/orca_selection_plugin_any.py
---

## Setup

Start the app and maximize the window. Right-click the plate, choose **Add Primitive**, then
**Cylinder**, and accept the size dialog. Click the cylinder so it is selected.

## Steps

| # | Action | Expected result | Screenshot? |
|---|---|---|---|
| 1 | Open the **Plugins** dialog. On Windows and Linux it is in the menu behind the icon right of **File** in the title bar; on macOS it is under **File**. If the dialog's top edge is off-screen, press Alt+Space and choose **Maximize** | The Plugins dialog is open and its **Install plugin** button is visible | y |
| 2 | Open the menu of the **Install plugin** split button, choose **Install local plugin** and pick `data\orca_selection_plugin_any.py`. Approve any prompt | "Selection Inspector" is listed with source Local, marked Inactive, and the status line says it was installed | y |
| 3 | On the "Selection Inspector" row, tick **Activate** | The row expands and shows a **Script** entry with a run button (a triangle) | n |
| 4 | Press the run button, then close the Plugins dialog | A panel titled **Selection Inspector** is docked at the right edge of the window, with sections Target, Objects, Faces, Events, a row of buttons, and Output. It stays after the dialog closes | y |
| 5 | Click the cylinder, then press **Run self-test** in the panel | Output starts with `SELF-TEST: N passed, 0 failed, 1 skipped`; the skipped check says the target object has no modifier. Copy the summary line and any line starting with FAIL into the report | y |
| 6 | Click an empty part of the plate, then click the cylinder again | Objects shows `nothing selected`, then one line naming object 0 with "whole object" | n |
| 7 | With the Face selection tool closed, press **Select first 10** | Output says `Set 10 triangle(s) on object 0 ...`; Faces shows a total of 10 | y |
| 8 | Open the Face selection tool on the cylinder | 10 triangles are orange and the tool panel says "10 triangles selected" | y |
| 9 | With the tool still open, press **Select upward** in the plugin panel | The flat top of the cylinder turns orange at once. The count in the tool panel equals the Faces total in the plugin panel | y |
| 10 | Press **Grow** | The orange area grows from the top onto the side (on a cylinder whose side triangles run its full height, the whole side). Both counts rise together and stay equal | y |
| 11 | With the **Triangle** tool, hold Shift and click one orange triangle | Both counts drop by 1 | n |
| 12 | Press **Done** in the tool panel. Note the Faces total | The highlight is gone; the Faces total is unchanged | n |
| 13 | Right-click the cylinder, choose **Add Modifier**, then **Cube**; confirm any dialog. Click the cylinder itself again | The object list shows the cylinder with a modifier part under it. The Faces total is unchanged | y |
| 14 | In the object list, click the modifier part and press Delete | The modifier is gone and the cylinder has a single part again | n |
| 15 | Look at Faces in the plugin panel, then open the Face selection tool on the cylinder | The Faces total is the same as in step 12, and the same triangles are orange | y |
| 16 | Press **Done**. Right-click the cylinder, choose **Modify**, then **Simplify Model**, and apply any reduction | Faces shows `no faces selected` (the mesh changed, so the selection is dropped) | y |
| 17 | Press **Clear faces**, then delete the cylinder | Faces shows `no faces selected`; the app keeps running | n |

## Pass criteria

All steps pass, the self-test in step 5 reports 0 failed, and the selection in step 15 is
the one from step 12.

## Known flaky areas

- Step 1: the Plugins dialog opens at 900 x 820. On a screen 768 pixels high its title bar
  is off-screen and it cannot be dragged; maximize it as described.
- Step 2: if the plugin is still installed from an earlier run in this settings folder,
  the install asks whether to overwrite it. Accept.
- Output shows the result of the last button pressed in the plugin panel only. It does not
  change when the selection is edited in the 3D view or the model changes, so judge steps
  11 to 17 by Faces, not by Output.
- Steps 8 to 12: hold the mouse button about a third of a second on buttons inside the 3D
  view. With the plugin panel open the tool row is cramped; find Face selection by the
  panel it opens (see the README).
- Step 14: if Delete removes the whole cylinder, undo once and use the modifier's
  right-click menu instead.
- Reference values from the first run, with a 25 mm cylinder of 704 triangles: self-test
  14 passed, 0 failed, 1 skipped; Select upward 176; Grow 528; Simplify at medium detail
  left 66 triangles. Other numbers are fine as long as the two panels agree.

## Cleanup

Close the app without saving (Ctrl+Q). Attach the plugin panel's Output text from step 5.
The plugin stays installed and active in this settings folder.
