# Plugin selection API - High Level Design

## Purpose and scope

Python plugins can read the model graph through `orca.host`, but they cannot tell what
the user is pointing at in the Prepare tab. This feature lets a plugin see:

1. which objects, instances and parts are selected,
2. which mesh triangles the user picked with a new **Face selection** tool,
3. which triangles the user painted with the existing paint tools (supports, seam,
   multi-material, fuzzy skin),

and be told when 1 or 2 change.

Everything exposed is read-only. Plugins cannot set, clear or highlight a selection.
The face selection is transient working state: it is not written to the 3MF, is not part
of undo/redo, and does not affect slicing.

Out of scope: writing selections from a plugin; edge/vertex selection; sub-triangle face
selection; showing the face selection while the tool is closed; the Assemble view, the
Preview tab and the Design (CAD) tab; texture-displacement paint data; SLA.

## Terms

- **Object selection**: what `GUI::Selection` holds in the Prepare canvas (whole
  instances, or individual parts/modifiers).
- **Face selection**: a set of original-mesh triangle indices per model part, picked with
  the Face selection gizmo. Indices refer to `ModelVolume.mesh().triangles()` rows.
- **Paint layer**: one of the four `FacetsAnnotation` members of a `ModelVolume`. Paint
  can cover part of a triangle, so it is reported as sub-triangle geometry plus the source
  triangle of each piece.

## Design

### 1. Selection store (`src/slic3r/GUI/SelectionSnapshot.{hpp,cpp}`)

A small class with no wx or OpenGL dependency, so it is unit-testable and usable from the
plugin layer. There is one instance per process (`SelectionSnapshotStore::instance()`), as
there is one `Plater`.

- Holds the last published object selection (a list of items) and the face selection
  (map keyed by `ModelVolume` `ObjectID`).
- All access goes through one mutex. Writers are on the UI thread; readers may be on any
  thread, because Script plugins run on a worker. Readers receive a value copy
  (`SelectionSnapshot`), never a reference into live GUI state.
- A monotonically increasing `revision` is bumped only when content actually changes, so
  redundant writes produce no event.

Face entries record, next to the triangle indices: object and volume `ObjectID`, object
and volume index, the instance that was active when picking, and the identity of the mesh
they index into (mesh pointer and triangle count).

Invalidation, all on the UI thread:

- An entry is dropped when its volume no longer exists or its mesh identity changed
  (simplify, cut, repair, mesh boolean, reload from disk). Indices are refreshed when
  objects or volumes are reordered. This check runs when the store is published (below)
  and when the gizmo opens.
- New project, project open and project close need no separate handling: the parts are
  gone, so the same check drops their entries.

### 2. Face selection gizmo (`src/slic3r/GUI/Gizmos/GLGizmoFaceSelect.{hpp,cpp}`)

A `GLGizmoPainterBase` subclass, modeled on `GLGizmoFuzzySkin`, so brush, raycast,
clipping plane and rendering are reused rather than duplicated.

- Tools: circle brush, sphere brush, single triangle, smart fill with an angle threshold.
  Left button adds, Shift + left button removes, matching the other paint tools.
- Triangle splitting is disabled (`m_triangle_splitting_enabled = false`), so the result
  is always whole original triangles. A brush selects every triangle it touches.
- The gizmo keeps its `TriangleSelector`s as the editing state. `update_model_object()`
  writes the selected triangle indices to the store instead of a `FacetsAnnotation`;
  `update_from_model_object()` seeds the selectors from the store. The model is never
  modified, so nothing is rescheduled for slicing and the project is not marked dirty.
- No undo/redo entries are created. `GLGizmoPainterBase` takes snapshots at stroke end
  and on enter/leave; a small virtual opt-out is added to the base class, defaulting to
  the current behavior so the existing paint gizmos are unchanged.
- Panel: tool buttons, brush size or smart-fill angle, selected triangle count, **Clear**
  (this object), **Clear all** (every object), **Done**.
- Activation rules are inherited: FFF printer, one object instance selected. Selecting on
  several objects is done by switching objects; entries accumulate in the store.
- Registered in `GLGizmosManager` (`EType::FaceSelect`, placed after `FuzzySkin`; the
  manager's painter-gizmo dispatch lists are extended), with a new toolbar icon in light
  and dark variants. It has no keyboard shortcut in this version.
- The highlight is visible only while the gizmo is open. The selection itself persists
  after the gizmo closes so a plugin run from the Plugins dialog still sees it.

### 3. Publishing and the change event

A new `LifecycleEvent::SelectionChanged` is appended as the last value of the enum (also in
`lifecycle_event_to_string` and the Python enum), so existing values do not move.

- **Faces**: published at the end of each stroke and on Clear / Clear all, not per mouse
  move.
- **Objects**: `GUI::Selection` advances a revision counter whenever its content may have
  changed, whether from the 3D view or the object list. The Prepare canvas compares it in
  its idle handler and publishes, so a bulk operation produces one event.
- The event is fired on the UI thread only when the store's revision changed. Context:
  `source` is `"objects"` or `"faces"`, `index` is the new revision; other fields are
  empty. The payload carries no selection data: handlers call `orca.host.selection()`.

### 4. Python API (`src/slic3r/plugin/host/PluginHostSelection.cpp`)

Registered from `PluginHost::RegisterBindings` after `register_model`.

```python
sel = orca.host.selection()        # immutable snapshot; callable from any thread
sel.revision                       # int
sel.is_empty()                     # no objects/parts selected
sel.mode                           # orca.host.SelectionMode.Instance | .Volume

for item in sel.items():           # object selection
    item.object_index              # index into model.objects()
    item.instance_index            # -1 when not tied to one instance
    item.volume_index              # -1 when the whole instance is selected
    item.object_id, item.volume_id # stable ObjectID values (volume_id 0 when whole instance)

for faces in sel.faces():          # face selection, one entry per part with >= 1 triangle
    faces.object_index, faces.volume_index, faces.instance_index
    faces.object_id, faces.volume_id
    faces.count                    # number of selected triangles
    faces.triangle_indices()       # sorted list[int]; no numpy needed
    faces.triangles()              # sorted (N,) int32 ndarray copy; requires numpy

sel.has_faces()                    # any entry
```

Indices index the current model and are valid at snapshot time. A plugin resolves them
with the existing graph, for example `orca.host.model().object(i).volume(j).mesh()`; the
snapshot does not hand out model references, so it stays safe to keep.

Paint layers are read from the volume:

```python
vol.painted_states(orca.host.PaintLayer.Support)      # list[int] of states in use
pf = vol.painted_facets(orca.host.PaintLayer.Support, state=1)
pf.vertices()            # (K, 3) float32, volume-local coordinates; requires numpy
pf.triangles()           # (M, 3) int32 rows into vertices()
pf.source_triangles()    # (M,) int32: original mesh triangle of each piece
pf.source_triangle_indices()  # the same as list[int]; no numpy needed
pf.count                 # M
```

`PaintLayer` is `Support`, `Seam`, `MultiMaterial`, `FuzzySkin`. `state` is the raw
`EnforcerBlockerType` value: for Support and Seam 1 = enforcer, 2 = blocker; for FuzzySkin
1; for MultiMaterial the 1-based filament number. `painted_facets` returns a value copy
built with `TriangleSelector::get_facets_strict`. These follow the threading rules of the
existing `ModelVolume` bindings.

No new plugin permission is introduced: the data is of the same kind as the mesh geometry
plugins can already read.

### 5. Compatibility

- No change to the 3MF format, presets or profiles.
- With the Face selection gizmo unused and no plugin loaded, behavior is unchanged: the
  store is written but nothing observes it, and the lifecycle hook is a no-op.
- Existing paint gizmos keep their undo/redo and model-update behavior.
- `LifecycleEvent` gains one value; existing values keep their order.

## Files

| Area | Files |
| --- | --- |
| Store | `src/slic3r/GUI/SelectionSnapshot.{hpp,cpp}` (new) |
| Gizmo | `Gizmos/GLGizmoFaceSelect.{hpp,cpp}` (new), `GLGizmoPainterBase.{hpp,cpp}` (snapshot opt-out), `GLGizmosManager.{hpp,cpp}`, `GLCanvas3D.cpp`, `resources/images/toolbar_face_select*.svg`, `src/slic3r/CMakeLists.txt` |
| Event | `libslic3r/LifecycleEvents.hpp`, `plugin/PythonPluginBridge.cpp`, `Selection.{hpp,cpp}` (revision), publishing in `GLCanvas3D.{hpp,cpp}` |
| Bindings | `plugin/host/PluginHostSelection.cpp` (new), `PluginHostBindings.hpp`, `PluginHost.cpp`, `PluginHostModel.cpp` (paint layers) |
| Tests | `tests/slic3rutils/test_plugin_selection.cpp` (new) |

## Build order

1. Store and its unit tests.
2. Python bindings for the snapshot and the paint layers, with host API tests.
3. `SelectionChanged` event and object-selection publishing.
4. Face selection gizmo, manager registration, icon, shortcut.
5. Example plugin (not written yet).

## Verification

Automated (Catch2, `tests/slic3rutils`):

- Store: add/remove/replace/clear, revision changes only on real change, pruning on
  missing volume and on changed mesh identity, index refresh, concurrent read while
  writing.
- Bindings: types and functions exist; a snapshot built in C++ reads back correctly in
  Python (indices, sorted order, dtype, read-only arrays); `painted_states` and
  `painted_facets` on a volume painted in the test, with each piece mapped to its source
  triangle.
- Event: `SelectionChanged` is exposed to Python and has a string name.

Manual, in the running app:

- Each tool adds and removes whole triangles; count matches `sel.faces()`.
- Selection survives closing the gizmo and switching objects; is dropped after Simplify
  or Cut; is cleared by New Project.
- Using the tool does not mark the project dirty, add undo entries, or invalidate a
  finished slice. Undo/redo and results of the four paint gizmos are unchanged.
- The example plugin prints the object and face selection and reacts to the event.
