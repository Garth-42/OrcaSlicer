# Plugin selection API - High Level Design

## Purpose and scope

Python plugins can read the model graph through `orca.host`, but that alone does not tell
them what the user is pointing at in the Prepare tab. The selection API lets a plugin see:

1. which objects, instances and parts are selected,
2. which mesh triangles are picked with the **Face selection** tool,
3. which triangles are painted with the paint tools (supports, seam, multi-material,
   fuzzy skin),

be told when 1 or 2 change, and replace 2 with a set of triangles it computed.

The split of responsibilities is deliberate. OrcaSlicer owns what a plugin cannot do:
picking in the 3D view, drawing the highlight, holding the selection and keeping it valid.
Deciding which triangles belong together (a hole, a cylinder, everything facing down) is
plugin work: the plugin computes the set from the mesh arrays and hands it back for
display and touch-up.

The face selection is transient working state. It is not written to the 3MF, is not part
of undo/redo, and does not affect slicing. A plugin cannot change the object selection or
the paint layers through this API.

Not covered: edge and vertex selection, sub-triangle face selection, showing the face
selection while the tool is closed, the Assemble view, the Preview tab, the Design tab,
texture-displacement paint data, and SLA.

## Terms

- **Object selection**: what `GUI::Selection` holds in the Prepare canvas (whole
  instances, or individual parts and modifiers).
- **Face selection**: a set of original-mesh triangle indices per model part. Indices are
  rows of `ModelVolume.mesh().triangles()`.
- **Paint layer**: one of the four `FacetsAnnotation` members of a `ModelVolume`. Paint
  can cover a part of a triangle, so it is reported as split geometry plus the source
  triangle of each piece.

## Selection store

`SelectionSnapshotStore` (`src/slic3r/GUI/SelectionSnapshot.hpp`) holds value copies of
the object selection and the face selection. It has no wx or OpenGL dependency. There is
one instance per process, as there is one `Plater`.

- All access goes through one mutex. Writers run on the UI thread; readers may be on any
  thread, because Script plugins run on a worker. A reader receives a copy
  (`SelectionSnapshot`), never a reference into GUI state.
- `revision` advances only when content changes, so a redundant write produces no event.
  `faces_revision` advances with the face selection alone.
- A face entry records the part and object ids and indices, the instance that was active
  when it was made, and the mesh its indices refer to (a `weak_ptr`).

A face entry is dropped when its part no longer exists or its mesh was replaced (simplify,
cut, repair, mesh boolean, reload from disk, a new or reopened project). Entries are
matched by part id. `ModelObject::delete_volume` gives the last part left in an object a
new id; its entry is then found by its mesh and moved to the new id, unless two entries
of that object refer to the same mesh, in which case both are dropped. The indices of
the remaining entries are refreshed when objects or parts are reordered. This check,
`refresh_faces`, runs whenever the Prepare canvas reloads its scene.

## Face selection gizmo

`GLGizmoFaceSelect` is a `GLGizmoPainterBase` subclass, so brush, raycast, clipping plane
and rendering are shared with the paint gizmos.

- Tools: circle brush, sphere brush, single triangle, and smart fill with an angle
  threshold. The left button selects; Shift with the left button, or the right button,
  deselects.
- Triangle splitting is off, so the result is always whole mesh triangles. A brush takes
  every triangle it touches.
- The gizmo's `TriangleSelector`s are the editing state. At the end of a stroke the gizmo
  writes the selected indices to the store; when it opens, or when the store's
  `faces_revision` moves under it, it rebuilds the selectors from the store. That is how a
  selection set by a plugin shows up while the tool is open.
- The model is never modified: nothing is rescheduled for slicing, the project is not
  marked dirty, and no undo snapshot is taken. `GLGizmoPainterBase::stroke_takes_snapshot`
  exists for this; it defaults to the behavior of the paint gizmos.
- Undo snapshots record which gizmo is open, and something other than the gizmo can take
  one while it is open (an object list action, another gizmo being entered, Undo capturing
  the current state). `GLGizmoBase::is_saved_in_snapshots` is false for this gizmo, so
  such a snapshot records no open gizmo and Undo / Redo never reopens the tool. Undo or
  Redo while the tool is open applies to the model and closes the tool; the face
  selection stays in the store unless the mesh it refers to is gone.
- Activation follows the paint gizmos: FFF printer, one object instance selected. Faces
  on several objects are selected by switching objects; entries accumulate in the store.
- The highlight is drawn only while the gizmo is open. The selection itself outlives the
  gizmo, so a plugin run from the Plugins dialog still sees it.
- The gizmo has a toolbar button and no keyboard shortcut.

## Change event

`LifecycleEvent::SelectionChanged` is fired on the UI thread when the store's content
changed. It is the last value of the enum, so the values of the other events are as they
were. Its context carries `source` and `index` (the new store revision), and no selection
data: a handler calls `orca.host.selection()`.

| `source` | Meaning | When |
| --- | --- | --- |
| `objects` | Object selection changed | `GUI::Selection` advances a revision counter whenever its content may have changed, from the 3D view or the object list. The Prepare canvas compares it in its idle handler, so a bulk operation yields one event. |
| `faces` | Face selection changed by the user, or entries were dropped by `refresh_faces` | End of a stroke, Clear, Clear all, scene reload. Not per mouse move. |
| `plugin` | Face selection set by a plugin | `set_face_selection`, `clear_face_selection` |

A plugin that sets the face selection from its own `SelectionChanged` handler should
ignore the `plugin` source, or it will answer its own change.

## Python API

```python
sel = orca.host.selection()        # immutable snapshot; callable from any thread
sel.revision                       # int
sel.is_empty()                     # no objects or parts selected
sel.mode                           # orca.host.SelectionMode.Instance | .Volume

for item in sel.items():           # object selection
    item.object_index              # index into model.objects()
    item.instance_index
    item.volume_index              # -1 when the whole instance is selected
    item.object_id, item.volume_id # ObjectID values (volume_id 0 for a whole instance)

sel.has_faces()
for faces in sel.faces():          # one entry per part with selected triangles
    faces.object_index, faces.volume_index, faces.instance_index
    faces.object_id, faces.volume_id
    faces.count
    faces.triangle_indices()       # sorted list[int]
    faces.triangles()              # sorted (N,) int32 ndarray copy; requires numpy

# Replace the selected triangles of one part; an empty list deselects them.
orca.host.set_face_selection(object_index, volume_index, triangles, instance_index=0)
orca.host.clear_face_selection()   # every part
```

Indices are valid at snapshot time. A plugin resolves them through the model graph, for
example `orca.host.model().object(i).volume(j).mesh()`. The snapshot hands out no model
references, so it is safe to keep.

`set_face_selection` and `clear_face_selection` read the model, so they run on the UI
thread and block the caller until done. `set_face_selection` raises `IndexError` for an
index outside the model or the mesh and `ValueError` for a volume that is not a model
part. It returns the number of triangles selected on the part.

Paint layers are read from the volume:

```python
vol.painted_states(orca.host.PaintLayer.Support)      # list[int] of states in use
pf = vol.painted_facets(orca.host.PaintLayer.Support, state=1)
pf.count                       # M, the number of painted pieces
pf.vertices()                  # (K, 3) float32, volume-local coordinates; requires numpy
pf.triangles()                 # (M, 3) int32 rows into vertices(); requires numpy
pf.source_triangles()          # (M,) int32: mesh triangle of each piece; requires numpy
pf.source_triangle_indices()   # the same as list[int]
```

`PaintLayer` is `Support`, `Seam`, `MultiMaterial` or `FuzzySkin`. `state` is the
`EnforcerBlockerType` value: for Support and Seam 1 is the enforcer and 2 the blocker, for
FuzzySkin it is 1, for MultiMaterial it is the 1-based filament number. `painted_facets`
returns a copy and follows the threading rules of the other `ModelVolume` bindings.

`sandboxes/orca_selection_plugin_any.py` is a worked example: a docked panel that follows
the selection, sets the face selection from Python, and self-tests these calls.

The API needs no plugin permission: what it reads is of the same kind as the mesh geometry
a plugin can already read, and what it writes is transient view state.

## Compatibility

- The 3MF format, presets and profiles are untouched.
- With the Face selection gizmo unused and no plugin loaded, behavior is unchanged: the
  store is written, nothing observes it, and firing the event is a no-op.
- The paint gizmos keep their undo/redo and model-update behavior.
