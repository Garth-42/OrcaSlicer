---
id: 02-slice-and-export-gcode
title: Slice a model and export the G-code to a file
priority: P1
timeout_minutes: 15
preconditions:
  - Setup wizard already completed in this settings folder (run plan 01 or smoke first, and do not wipe)
test_data:
  - data/cube_20mm.stl
---

## Setup

Create the folder `C:\TestRuns\{run_id}\out` and make sure it is empty.

## Steps

| # | Action | Expected result | Screenshot? |
|---|---|---|---|
| 1 | Start the app | The main window opens on the **Prepare** tab | n |
| 2 | Import `data\cube_20mm.stl` through **File > Import > Import 3MF/STL/STEP/SVG/OBJ/AMF...** | One cube is on the plate | n |
| 3 | Press **Slice plate** and wait up to 60 seconds | The **Preview** tab is shown with the cube drawn as layers, and a legend on the left lists line types with times or lengths | y |
| 4 | Drag the vertical slider at the right edge of the preview down to about the middle | Only the lower half of the cube's layers is drawn | y |
| 5 | Look for an estimated print time and filament usage on screen | Both are shown and are greater than zero. Note the values | y |
| 6 | Open the arrow beside the **Print plate** button (top right) and choose **Export G-code file**, then press that button | A save dialog opens with a file name ending in `.gcode` | y |
| 7 | Save as `C:\TestRuns\{run_id}\out\cube.gcode` | The dialog closes. A notification says the export finished | y |
| 8 | Check the file on disk | `cube.gcode` exists, is larger than 20 KB, and its text contains lines starting with `G1` | n |
| 9 | Go back to **Prepare**, select the cube, and change **Layer height** in the left side bar to `0.28` | The value is accepted, and the **Slice plate** button is available again | y |
| 10 | Press **Slice plate** | The preview is rebuilt with visibly fewer, thicker layers than in step 3 | y |

## Pass criteria

All steps pass, and the newest log file has no line containing `error` that mentions slicing or export.

## Known flaky areas

- Step 6: the export choice may already be the button's label, or may be reached through
  **File > Export > Export G-code...** instead. Either route is fine; note which one existed.
- Step 9: **Layer height** is under the **Quality** group of the process settings. If the
  side bar shows only a few settings, switch on **Advanced** first.

## Cleanup

Close the app without saving. Keep `out\cube.gcode` with the run's results.
