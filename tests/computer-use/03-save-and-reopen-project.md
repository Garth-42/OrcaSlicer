---
id: 03-save-and-reopen-project
title: Save a project, reopen it, and find it unchanged
priority: P1
timeout_minutes: 15
preconditions:
  - Setup wizard already completed in this settings folder
test_data:
  - data/cube_20mm.stl
---

## Setup

Create the folder `C:\TestRuns\{run_id}\out` if it does not exist.

## Steps

| # | Action | Expected result | Screenshot? |
|---|---|---|---|
| 1 | Start the app and import `data\cube_20mm.stl` | One cube is on the plate | n |
| 2 | Double-click the object's name in the object list, type `saved_cube`, press Enter | The object list shows `saved_cube` | n |
| 3 | Right-click an empty part of the plate, choose **Add Primitive**, then **Cylinder**. Accept the size dialog if one appears | A cylinder is added; the object list shows two objects | y |
| 4 | Look at the window title | It shows a marker for unsaved changes (an asterisk) | y |
| 5 | Choose **File > Save Project as...** and save as `C:\TestRuns\{run_id}\out\two_objects.3mf` | The dialog closes; the title shows `two_objects` without the unsaved marker | y |
| 6 | Choose **File > New Project** | The plate is empty and the object list is empty. No prompt to save appears | y |
| 7 | Choose **File > Open Project...** and open `two_objects.3mf` | Both objects are back in the same places, and one is named `saved_cube` | y |
| 8 | Press **Slice plate** | The preview shows both objects as layers | y |
| 9 | Go back to **Prepare**, click the cylinder and press Delete. Then close the main window | A prompt asks whether to save the changes | y |
| 10 | Choose not to save. Start the app again and open `two_objects.3mf` | Both objects are present | n |

## Pass criteria

All steps pass. The file `two_objects.3mf` exists and is larger than 5 KB.

## Known flaky areas

- Step 3: the first time a primitive is added, a size dialog and an information dialog
  can appear. Confirm them with their default values.
- Step 7: opening a project may ask how to load it or whether to keep current settings.
  Take the default.

## Cleanup

Close the app. Keep `out\two_objects.3mf` with the run's results.
