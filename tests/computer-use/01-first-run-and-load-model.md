---
id: 01-first-run-and-load-model
title: First start, setup wizard, loading and arranging a model
priority: P1
timeout_minutes: 15
preconditions:
  - App installed and settings folders wiped as described in testbox.yml
test_data:
  - data/cube_20mm.stl
---

## Setup

None.

## Steps

| # | Action | Expected result | Screenshot? |
|---|---|---|---|
| 1 | Start the app with the launch command from `testbox.yml` | The setup wizard is shown | y |
| 2 | Page by page, note the title of each wizard page and press its forward button. Tick the first printer offered if a page requires one. Decline plug-in installation and login | Each page advances. The wizard ends at the main window | y, each page |
| 3 | Look at the top of the window | Tabs **Prepare** and **Preview** are present, and **Prepare** is active | y |
| 4 | Look at the left side bar | It shows a **Printer** section with a printer name and a **Filament** section with at least one filament | y |
| 5 | Open **File > Import > Import 3MF/STL/STEP/SVG/OBJ/AMF...** and choose `data\cube_20mm.stl` | One cube is on the plate; the object list shows one object | y |
| 6 | Click the cube in the 3D view | The cube is highlighted and a row of tool buttons above the 3D view becomes available | y |
| 7 | Press Ctrl+C, then Ctrl+V | A second cube appears on the plate; the object list shows two objects | n |
| 8 | Click an empty part of the plate, then press `A` | Both cubes are rearranged on the plate without overlapping | y |
| 9 | Drag with the left mouse button on an empty part of the 3D view, then turn the mouse wheel | The view rotates, then zooms. The cubes stay solid and shaded | y |
| 10 | Click one cube and press Delete | One cube is left; the object list shows one object | n |
| 11 | Press Ctrl+Z | The deleted cube is back | n |
| 12 | Close the app, start it again with the same launch command | The main window opens directly, without the setup wizard, with the same printer as before | y |

## Pass criteria

All steps pass and no error dialog appeared.

## Known flaky areas

- Step 2: wizard wording and page count were not checked on screen.
- Steps 6 to 9: with software OpenGL the 3D view can lag by a second or two. Wait for the
  picture to settle before judging.
- Step 8: `A` only arranges when the 3D view has keyboard focus. Click the plate first.

## Cleanup

Close the app without saving.
