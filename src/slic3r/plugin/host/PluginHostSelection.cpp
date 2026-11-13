#include "PluginHostBindings.hpp"

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "slic3r/GUI/SelectionSnapshot.hpp"
#include "slic3r/plugin/PluginBindingUtils.hpp"

#include <cstdint>

namespace py = pybind11;

namespace Slic3r {

// What is selected in the Prepare tab, see docs/HLSD/plugin-selection-api.md. Everything bound
// here is a value copy taken from the SelectionSnapshotStore, so a plugin may read it from any
// thread and keep it: it never refers back into the GUI.
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
}

} // namespace Slic3r
