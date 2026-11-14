---
id: smoke
title: The app starts, loads a model, slices it and exits
priority: P0
timeout_minutes: 5
preconditions:
  - App installed and settings folders wiped as described in testbox.yml
  - Logged in to the desktop as the local user "claude"
test_data:
  - data/cube_20mm.stl
---

## Setup

None.

## Steps

| # | Action | Expected result | Screenshot? |
|---|---|---|---|
| 1 | Start the app with the launch command from `testbox.yml` and wait up to 90 seconds | A window whose title contains "OrcaSlicer" is open. On a clean start it shows the setup wizard | y |
| 2 | Go through the setup wizard with its default choices: press the forward button on each page (it may read **Next**, **Confirm** or **Finish**). If a page requires a printer, tick the first printer offered. Decline any offer to install a network plug-in or to log in | The wizard closes and the main window shows the **Prepare** tab with an empty build plate | y |
| 3 | Open **File > Import > Import 3MF/STL/STEP/SVG/OBJ/AMF...** and choose `data\cube_20mm.stl` | A cube stands on the build plate and its name is in the object list on the left | y |
| 4 | Press **Slice plate** (top right) and wait up to 60 seconds | The view switches to **Preview** and shows the cube as coloured layers. No error message is shown | y |
| 5 | Close the main window. If asked whether to save the project, choose not to save | The window closes and no OrcaSlicer process is left after 15 seconds | n |

## Pass criteria

All five steps pass, and the newest file in the log folder has no line containing `fatal`.

## Known flaky areas

- Step 1: the first start on a box without a GPU is slow, because the 3D view runs on
  software OpenGL. A blank window for up to a minute is not a failure.
- Step 2: the wizard's wording was not checked on screen. Any path through it that ends
  at the main window is acceptable; note the pages and buttons you saw.
- Step 4: if no printer was selected in the wizard, slicing is refused. Record that as a
  failure of step 2, not of step 4.

## Cleanup

Make sure no `orca-slicer.exe` process is running.
