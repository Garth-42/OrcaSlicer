#include "PluginHostBindings.hpp"

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "PluginHostUi.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/SelectionSnapshot.hpp"
#include "slic3r/plugin/PluginBindingUtils.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>
#include <wx/app.h>

namespace py = pybind11;

namespace Slic3r {
namespace {

GUI::Plater& require_plater()
{
    GUI::Plater* plater = wxTheApp != nullptr ? GUI::wxGetApp().plater() : nullptr;
    if (plater == nullptr)
        throw std::runtime_error("Plater is not available");
    return *plater;
}

// Tells plugins and the Prepare view that a plugin changed the face selection. UI thread.
void face_selection_set_by_plugin(GUI::Plater& plater)
{
    GUI::fire_selection_changed("plugin", GUI::SelectionSnapshotStore::instance().revision());
    if (GUI::GLCanvas3D* canvas = plater.get_view3D_canvas3D())
        canvas->set_as_dirty();
}

// The model is owned by the UI thread, so the work is done there; the caller waits for it.
size_t set_face_selection(int object_index, int volume_index, std::vector<int> triangles, int instance_index)
{
    size_t count = 0;
    PluginHostUi::run_on_ui_thread([&] {
        GUI::Plater& plater = require_plater();
        const Model& model  = plater.model();
        if (object_index < 0 || object_index >= int(model.objects.size()))
            throw std::out_of_range("object index out of range");
        const ModelObject* object = model.objects[object_index];
        if (volume_index < 0 || volume_index >= int(object->volumes.size()))
            throw std::out_of_range("volume index out of range");
        if (instance_index < 0 || instance_index >= int(object->instances.size()))
            throw std::out_of_range("instance index out of range");
        const ModelVolume* volume = object->volumes[volume_index];
        if (!volume->is_model_part())
            throw std::invalid_argument("faces can only be selected on a model part");
        const int triangle_count = int(volume->mesh().its.indices.size());
        for (int triangle : triangles)
            if (triangle < 0 || triangle >= triangle_count)
                throw std::out_of_range("triangle index out of range");

        GUI::SelectedFaces entry;
        entry.object_id      = object->id().id;
        entry.volume_id      = volume->id().id;
        entry.object_index   = object_index;
        entry.volume_index   = volume_index;
        entry.instance_index = instance_index;
        entry.triangles      = std::move(triangles);
        entry.mesh           = volume->get_mesh_shared_ptr();

        GUI::SelectionSnapshotStore& store   = GUI::SelectionSnapshotStore::instance();
        const bool                   changed = store.set_faces(std::move(entry));
        count = store.face_triangles(volume->id().id, volume->get_mesh_shared_ptr()).size();
        if (changed)
            face_selection_set_by_plugin(plater);
    });
    return count;
}

void clear_face_selection()
{
    PluginHostUi::run_on_ui_thread([] {
        GUI::Plater& plater = require_plater();
        if (GUI::SelectionSnapshotStore::instance().clear_faces())
            face_selection_set_by_plugin(plater);
    });
}

} // namespace

// What is selected in the Prepare tab, see docs/HLSD/plugin-selection-api.md. What a plugin reads
// is a value copy taken from the SelectionSnapshotStore, so it may be read from any thread and
// kept: it never refers back into the GUI. Of the selection, a plugin can change the faces only.
void host_bindings::register_selection(py::module_& host)
{
    using namespace GUI;

    py::enum_<SelectionSnapshotMode>(host, "SelectionMode")
        .value("Instance", SelectionSnapshotMode::Instance)
        .value("Volume", SelectionSnapshotMode::Volume);

    py::class_<SelectedItem>(host, "SelectedItem",
        "One selected instance, or one selected part of an instance when volume_index is not -1.")
        .def_readonly("object_index", &SelectedItem::object_index)
        .def_readonly("instance_index", &SelectedItem::instance_index)
        .def_readonly("volume_index", &SelectedItem::volume_index)
        .def_readonly("object_id", &SelectedItem::object_id)
        .def_readonly("volume_id", &SelectedItem::volume_id);

    py::class_<SelectedFaces>(host, "SelectedFaces",
        "The triangles picked on one model part with the Face selection tool.")
        .def_readonly("object_index", &SelectedFaces::object_index)
        .def_readonly("volume_index", &SelectedFaces::volume_index)
        .def_readonly("instance_index", &SelectedFaces::instance_index)
        .def_readonly("object_id", &SelectedFaces::object_id)
        .def_readonly("volume_id", &SelectedFaces::volume_id)
        .def_property_readonly("count", [](const SelectedFaces& faces) { return faces.triangles.size(); })
        .def("triangle_indices", [](const SelectedFaces& faces) { return faces.triangles; },
             "Sorted indices of the selected triangles, rows of the part's mesh.triangles().")
        .def("triangles", [](const SelectedFaces& faces) {
            return with_numpy([&] {
                return py::object(py::array_t<std::int32_t>(py::ssize_t(faces.triangles.size()), faces.triangles.data()));
            });
        }, "The same indices as a sorted (N,) int32 ndarray (copy). Requires numpy.");

    py::class_<SelectionSnapshot>(host, "Selection",
        "Immutable snapshot of the Prepare tab selection.")
        .def_readonly("revision", &SelectionSnapshot::revision)
        .def_readonly("mode", &SelectionSnapshot::mode)
        .def("is_empty", [](const SelectionSnapshot& selection) { return selection.items.empty(); })
        .def("items", [](const SelectionSnapshot& selection) { return selection.items; },
             "The selected instances or parts.")
        .def("has_faces", [](const SelectionSnapshot& selection) { return !selection.faces.empty(); })
        .def("faces", [](const SelectionSnapshot& selection) { return selection.faces; },
             "One SelectedFaces per part with selected triangles, ordered by object and volume index.");

    host.def("selection", []() { return SelectionSnapshotStore::instance().snapshot(); },
             "Snapshot of the objects, parts and faces selected in the Prepare tab. Callable from any thread.");
    host.def("set_face_selection", &set_face_selection, py::arg("object_index"), py::arg("volume_index"), py::arg("triangles"),
             py::arg("instance_index") = 0,
             "Replace the selected triangles of one model part; an empty list deselects them. Returns the number "
             "selected. Fires SelectionChanged with source 'plugin' when the selection changed.");
    host.def("clear_face_selection", &clear_face_selection, "Deselect the faces of every part.");
}

} // namespace Slic3r
