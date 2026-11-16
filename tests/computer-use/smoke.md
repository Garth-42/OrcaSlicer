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
| 1 | Start the app with the launch command from `testbox.yml` and wait up to 90 seconds | The main window is open. On a clean start a "Setup Wizard" dialog reading "Welcome to Orca Slicer" is in front of it | y |
| 2 | Press **Get Started** on the "Setup Wizard" page, then go through the wizard with its default choices, pressing the forward button on each page. When it offers printers, tick one **without** "Belt" in its name (a belt printer drops every new object on the same spot). Decline any offer to install a network plug-in or to log in | The wizard closes and the main window shows the **Prepare** tab with an empty build plate | y |
| 3 | Open **File > Import > Import 3MF/STL/STEP/SVG/OBJ/AMF...** and choose `data\cube_20mm.stl` | A cube stands on the build plate and its name is in the object list on the left | y |
| 4 | Press **Slice plate** (top right) and wait up to 60 seconds | The view switches to **Preview** and shows the cube as coloured layers. No error message is shown | y |
| 5 | Close the main window. If asked whether to save the project, choose not to save | The window closes and no OrcaSlicer process is left after 15 seconds | n |

## Pass criteria

All five steps pass, and the newest file in the log folder has no line containing `fatal`.

## Known flaky areas

- Step 1: the first start on a box without a GPU is slow, because the 3D view runs on
  software OpenGL. A blank window for up to a minute is not a failure.
- Step 2: only the wizard's first page is known. Any path through the rest that ends at
  the main window with a non-belt printer is acceptable; note the pages and buttons you saw.
- Step 4: if no printer was selected in the wizard, slicing is refused. Record that as a
  failure of step 2, not of step 4.

## Cleanup

Make sure no `orca-slicer.exe` process is running.
