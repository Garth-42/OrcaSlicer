# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Selection Inspector"
# description = "Shows the objects and faces selected in the Prepare tab, sets the face selection from Python, and self-tests the selection API."
# author = "OrcaSlicer"
# version = "0.0.1"
# ///
"""Selection Inspector -- orca.host.selection() and friends.

Run it from the Plugins dialog. It opens a panel docked beside the 3D view that follows the
Prepare tab selection live:

  Objects   the selected objects or parts                       orca.host.selection().items()
  Faces     the triangles picked with the Face selection tool   orca.host.selection().faces()
  Events    how many SelectionChanged events arrived, by source  on_lifecycle_event()

and can change the face selection, which is how a plugin hands a computed selection (a hole, a
curved face, ...) back to the user for touch-up in the Face selection tool:

  Select first 10    orca.host.set_face_selection(object, part, [0..9])
  Select upward      every triangle whose normal points up in the part's own coordinates
  Grow               adds the triangles that share a vertex with the selected ones
  Clear faces        orca.host.clear_face_selection()

"Target" is the part these buttons act on: the first selected part, or the first part of the
first selected object, or the first part on the plate when nothing is selected.

"Paint layers" lists what is painted on the target with the support, seam, multi-material and
fuzzy skin tools. "Run self-test" exercises the API on the target and prints one PASS / FAIL /
SKIP line per check; it puts the face selection back the way it found it.

No numpy needed. The mesh walks are plain Python, so they stop at MAX_TRIANGLES.
"""
import orca

MAX_TRIANGLES = 200000
SOURCES = ("objects", "faces", "plugin")

PAGE = """
<style>
  body { margin: 0; padding: 12px; font-size: 13px; }
  h3 { margin: 14px 0 4px; font-size: 13px; }
  h3:first-child { margin-top: 0; }
  .actions { display: flex; flex-wrap: wrap; gap: 6px; margin: 8px 0; }
  .actions button.quiet { background: transparent; color: var(--orca-fg); border-color: var(--orca-border); }
  pre { margin: 0; padding: 6px 8px; white-space: pre-wrap; word-break: break-word; font-size: 12px;
        border: 1px solid var(--orca-border); border-radius: 6px; }
  #status { margin-top: 8px; color: var(--orca-muted); font-size: 12px; }
</style>

<h3>Target</h3>
<pre id="target">...</pre>
<h3>Objects</h3>
<pre id="objects">...</pre>
<h3>Faces</h3>
<pre id="faces">...</pre>
<h3>Events</h3>
<pre id="events">...</pre>

<div class="actions">
  <button type="button" data-command="first10">Select first 10</button>
  <button type="button" data-command="upward">Select upward</button>
  <button type="button" data-command="grow">Grow</button>
  <button type="button" data-command="clear" class="quiet">Clear faces</button>
</div>
<div class="actions">
  <button type="button" data-command="paint" class="quiet">Paint layers</button>
  <button type="button" data-command="selftest">Run self-test</button>
  <button type="button" data-command="reset_events" class="quiet">Reset event counts</button>
</div>

<h3>Output</h3>
<pre id="output">Nothing run yet.</pre>
<p id="status">Waiting for the plugin...</p>

<script>
(function () {
  function show(id, value) { document.getElementById(id).textContent = value; }

  orca.onMessage(function (message) {
    if (!message) return;
    if (message.command === "state") {
      show("target", message.target);
      show("objects", message.objects);
      show("faces", message.faces);
      show("events", message.events);
      show("status", "Revision " + message.revision + ", updated " + new Date().toLocaleTimeString());
    } else if (message.command === "output") {
      show("output", message.text);
    }
  });

  Array.prototype.forEach.call(document.querySelectorAll("button[data-command]"), function (button) {
    button.addEventListener("click", function () {
      orca.postMessage({ command: button.dataset.command });
    });
  });

  orca.postMessage({ command: "refresh" });
})();
</script>
"""


def object_label(model, object_index):
    if 0 <= object_index < model.object_count():
        return f"object {object_index} '{model.object(object_index).name or '(unnamed)'}'"
    return f"object {object_index}"


def brief(indices, limit=12):
    """'0, 1, 2, ... (40 total)' for a list of triangle indices."""
    shown = ", ".join(str(i) for i in indices[:limit])
    return shown if len(indices) <= limit else f"{shown}, ... ({len(indices)} total)"


def find_target(model, selection):
    """(object_index, volume_index) of the part the buttons act on, or None on an empty plate."""
    candidates = [(item.object_index, item.volume_index) for item in selection.items()]
    candidates += [(i, -1) for i in range(model.object_count())]
    for object_index, volume_index in candidates:
        if not 0 <= object_index < model.object_count():
            continue
        volumes = model.object(object_index).volumes()
        if 0 <= volume_index < len(volumes) and volumes[volume_index].is_model_part():
            return object_index, volume_index
        for index, volume in enumerate(volumes):
            if volume.is_model_part():
                return object_index, index
    return None


def selected_on(selection, target):
    for faces in selection.faces():
        if (faces.object_index, faces.volume_index) == target:
            return faces.triangle_indices()
    return []


def upward_triangles(mesh):
    """Triangles whose normal is within about 25 degrees of +Z in the mesh's own coordinates."""
    out = []
    for index in range(min(mesh.triangle_count(), MAX_TRIANGLES)):
        a, b, c = (mesh.vertex(v) for v in mesh.triangle(index))
        ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
        vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
        nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        length = (nx * nx + ny * ny + nz * nz) ** 0.5
        if length > 0 and nz / length > 0.9:
            out.append(index)
    return out


def grown(mesh, selected):
    """The selected triangles plus every triangle sharing a vertex with one of them."""
    count = min(mesh.triangle_count(), MAX_TRIANGLES)
    corners = [mesh.triangle(index) for index in range(count)]
    touched = {vertex for index in selected if index < count for vertex in corners[index]}
    return [index for index in range(count) if any(vertex in touched for vertex in corners[index])]


class SelectionInspector(orca.script.ScriptPluginCapabilityBase):
    panel = None
    events = dict.fromkeys(SOURCES, 0)
    last_event = "none yet"
    worker_read = "not run yet"

    def get_name(self):
        return "Selection Inspector"

    def execute(self):
        # execute() runs on the Script worker, not the UI thread: reading the selection here is
        # the check that the snapshot can be taken from any thread.
        try:
            self.worker_read = f"ok (revision {orca.host.selection().revision})"
        except Exception as error:  # noqa: BLE001 - reported in the panel
            self.worker_read = f"FAILED: {error}"

        if self.panel is not None and self.panel.is_open():
            self.panel.show()
            return orca.ExecutionResult.success("Selection Inspector is already open.")
        self.panel = orca.host.ui.create_dock_panel(
            html=PAGE,
            title="Selection Inspector",
            width=380,
            height=640,
            on_message=self.on_message,
            on_close=self.on_close,
            dock="right",
        )
        return orca.ExecutionResult.success("Selection Inspector opened.")

    def on_close(self):
        self.panel = None

    # Called on the UI thread for every lifecycle event; only selection changes matter here.
    def on_lifecycle_event(self, event, ctx):
        if event != orca.LifecycleEvent.SelectionChanged:
            return
        self.events[ctx.source] = self.events.get(ctx.source, 0) + 1
        self.last_event = f"{ctx.source} (revision {ctx.index})"
        self.send_state()

    # Called on the UI thread when the page posts.
    def on_message(self, message):
        command = (message or {}).get("command")
        try:
            if command == "reset_events":
                self.events = dict.fromkeys(SOURCES, 0)
                self.last_event = "none yet"
            elif command == "paint":
                self.output(self.paint_report())
            elif command == "selftest":
                self.output(self.self_test())
            elif command in ("first10", "upward", "grow", "clear"):
                self.output(self.change_faces(command))
        except Exception as error:  # noqa: BLE001 - shown in the panel instead of lost
            self.output(f"ERROR: {type(error).__name__}: {error}")
        self.send_state()

    def output(self, text):
        if self.panel is not None:
            self.panel.post({"command": "output", "text": text})

    def send_state(self):
        if self.panel is None:
            return
        model = orca.host.model()
        selection = orca.host.selection()
        target = find_target(model, selection)

        objects = [
            object_label(model, item.object_index) + f", instance {item.instance_index}" +
            (f", part {item.volume_index}" if item.volume_index >= 0 else ", whole object")
            for item in selection.items()
        ]
        faces = [
            f"{object_label(model, entry.object_index)}, part {entry.volume_index}: {entry.count} triangle(s)\n"
            f"  {brief(entry.triangle_indices())}"
            for entry in selection.faces()
        ]
        total = sum(entry.count for entry in selection.faces())
        self.panel.post({
            "command": "state",
            "revision": selection.revision,
            "target": "empty plate" if target is None else f"{object_label(model, target[0])}, part {target[1]}",
            "objects": f"Mode: {selection.mode.name}\n" + ("\n".join(objects) if objects else "nothing selected"),
            "faces": f"Total: {total} triangle(s)\n" + ("\n".join(faces) if faces else "no faces selected"),
            "events": "\n".join(f"{source}: {self.events.get(source, 0)}" for source in SOURCES) +
                      f"\nlast: {self.last_event}\nread from Script worker: {self.worker_read}",
        })

    def change_faces(self, command):
        if command == "clear":
            orca.host.clear_face_selection()
            return "Cleared the face selection of every part."
        model = orca.host.model()
        selection = orca.host.selection()
        target = find_target(model, selection)
        if target is None:
            return "Nothing to select on: the plate is empty."
        mesh = model.object(target[0]).volume(target[1]).mesh()
        if command == "first10":
            triangles = list(range(min(10, mesh.triangle_count())))
        elif command == "upward":
            triangles = upward_triangles(mesh)
        else:
            current = selected_on(selection, target)
            if not current:
                return "Grow needs a face selection on the target first."
            triangles = grown(mesh, current)
        count = orca.host.set_face_selection(target[0], target[1], triangles)
        return f"Set {count} triangle(s) on {object_label(model, target[0])}, part {target[1]}."

    def paint_report(self):
        model = orca.host.model()
        target = find_target(model, orca.host.selection())
        if target is None:
            return "Nothing to report: the plate is empty."
        volume = model.object(target[0]).volume(target[1])
        lines = [f"Paint on {object_label(model, target[0])}, part {target[1]}:"]
        for name in ("Support", "Seam", "MultiMaterial", "FuzzySkin"):
            layer = getattr(orca.host.PaintLayer, name)
            states = volume.painted_states(layer)
            if not states:
                lines.append(f"{name}: not painted")
                continue
            for state in states:
                facets = volume.painted_facets(layer, state)
                sources = sorted(set(facets.source_triangle_indices()))
                lines.append(f"{name} state {state}: {facets.count} piece(s) on {len(sources)} triangle(s)\n"
                             f"  {brief(sources)}")
        return "\n".join(lines)

    def self_test(self):
        model = orca.host.model()
        before = orca.host.selection()
        target = find_target(model, before)
        if target is None:
            return "SELF-TEST: cannot run, the plate is empty."
        obj, part = target
        mesh = model.object(obj).volume(part).mesh()
        if mesh.triangle_count() < 3:
            return "SELF-TEST: cannot run, the target has fewer than 3 triangles."

        results = []
        faces_before = [entry.triangle_indices() for entry in before.faces()]

        def check(name, passed, detail=""):
            results.append(("PASS" if passed else "FAIL", name + (f" ({detail})" if detail and not passed else "")))

        def raises(kind, call):
            try:
                call()
            except kind:
                return True
            except Exception:  # noqa: BLE001 - a different error is a failed check
                return False
            return False

        def plugin_events():
            return self.events.get("plugin", 0)

        # Start from a known state: no faces selected on the target.
        orca.host.set_face_selection(obj, part, [])

        events = plugin_events()
        count = orca.host.set_face_selection(obj, part, [0, 1, 2])
        check("set_face_selection returns the number selected", count == 3, f"got {count}")
        got = selected_on(orca.host.selection(), target)
        check("selection() reports the triangles that were set", got == [0, 1, 2], f"got {got}")
        check("a change fires one SelectionChanged with source 'plugin'", plugin_events() == events + 1,
              f"{plugin_events() - events} event(s)")

        events = plugin_events()
        revision = orca.host.selection().revision
        orca.host.set_face_selection(obj, part, [2, 1, 0, 1])
        check("setting the same triangles again fires no event", plugin_events() == events,
              f"{plugin_events() - events} event(s)")
        check("setting the same triangles again keeps the revision", orca.host.selection().revision == revision)

        orca.host.set_face_selection(obj, part, [2, 2, 0])
        got = selected_on(orca.host.selection(), target)
        check("triangles are stored sorted and without duplicates", got == [0, 2], f"got {got}")

        check("a triangle index outside the mesh raises IndexError",
              raises(IndexError, lambda: orca.host.set_face_selection(obj, part, [mesh.triangle_count()])))
        check("a negative triangle index raises IndexError",
              raises(IndexError, lambda: orca.host.set_face_selection(obj, part, [-1])))
        check("an object index outside the model raises IndexError",
              raises(IndexError, lambda: orca.host.set_face_selection(model.object_count(), 0, [0])))
        got = selected_on(orca.host.selection(), target)
        check("a rejected call leaves the selection as it was", got == [0, 2], f"got {got}")

        modifier = next((index for index, volume in enumerate(model.object(obj).volumes()) if not volume.is_model_part()), None)
        if modifier is None:
            results.append(("SKIP", "a volume that is not a model part raises ValueError (the target object has no modifier)"))
        else:
            check("a volume that is not a model part raises ValueError",
                  raises(ValueError, lambda: orca.host.set_face_selection(obj, modifier, [0])))

        orca.host.set_face_selection(obj, part, [])
        check("an empty list deselects the part", selected_on(orca.host.selection(), target) == [])

        orca.host.set_face_selection(obj, part, [0])
        orca.host.clear_face_selection()
        check("clear_face_selection deselects every part", not orca.host.selection().has_faces())

        check("a snapshot taken earlier is unchanged by later calls",
              [entry.triangle_indices() for entry in before.faces()] == faces_before)
        check("selection() can be read from the Script worker", self.worker_read.startswith("ok"), self.worker_read)

        # Put back what was selected before the test.
        for entry in before.faces():
            orca.host.set_face_selection(entry.object_index, entry.volume_index, entry.triangle_indices(),
                                         max(entry.instance_index, 0))

        tally = {status: sum(1 for result in results if result[0] == status) for status in ("PASS", "FAIL", "SKIP")}
        summary = f"SELF-TEST: {tally['PASS']} passed, {tally['FAIL']} failed, {tally['SKIP']} skipped"
        return "\n".join([summary] + [f"{status}  {name}" for status, name in results])


@orca.plugin
class SelectionInspectorPlugin(orca.base):
    def register_capabilities(self):
        orca.register_capability(SelectionInspector)
