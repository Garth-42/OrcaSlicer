// Windows headers ahead of libslic3r, for the reason given in test_plugin_host_api.cpp.
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
    #include <CommCtrl.h>
#endif

#include <catch2/catch_all.hpp>

#include <libslic3r/LifecycleEvents.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleSelector.hpp>
#include <slic3r/GUI/SelectionSnapshot.hpp>

#include "python_test_support.hpp"

#include <pybind11/embed.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <thread>
#include <vector>

namespace py = pybind11;
using namespace Slic3r;
using namespace Slic3r::GUI;

namespace {

// A model with one object holding two cube parts (12 triangles each) and one instance.
struct TwoPartModel
{
    Model        model;
    ModelObject* object = nullptr;

    TwoPartModel()
    {
        object = model.add_object();
        object->add_volume(make_cube(10.0, 10.0, 10.0));
        object->add_volume(make_cube(5.0, 5.0, 5.0));
        object->add_instance();
    }

    SelectedFaces faces(size_t volume_idx, std::vector<int> triangles) const
    {
        const ModelVolume* volume = object->volumes[volume_idx];
        SelectedFaces      entry;
        entry.object_id      = object->id().id;
        entry.volume_id      = volume->id().id;
        entry.object_index   = 0;
        entry.volume_index   = int(volume_idx);
        entry.instance_index = 0;
        entry.triangles      = std::move(triangles);
        entry.mesh           = volume->get_mesh_shared_ptr();
        return entry;
    }
};

} // namespace

TEST_CASE("Selection store changes its revision only when the content changes", "[PluginSelection]")
{
    SelectionSnapshotStore store;
    TwoPartModel           scene;
    const auto             revision = store.revision();

    SelectedItem item;
    item.object_index   = 0;
    item.instance_index = 0;
    item.object_id      = scene.object->id().id;

    REQUIRE(store.set_items(SelectionSnapshotMode::Instance, {item}));
    CHECK(store.revision() == revision + 1);
    CHECK_FALSE(store.set_items(SelectionSnapshotMode::Instance, {item}));
    CHECK(store.revision() == revision + 1);
    CHECK(store.set_items(SelectionSnapshotMode::Volume, {item}));
    CHECK(store.set_items(SelectionSnapshotMode::Volume, {}));
    CHECK(store.snapshot().items.empty());

    REQUIRE(store.set_faces(scene.faces(0, {3, 1})));
    CHECK_FALSE(store.set_faces(scene.faces(0, {1, 3})));
    CHECK_FALSE(store.set_faces(scene.faces(1, {})));
    CHECK(store.set_faces(scene.faces(0, {1})));
    CHECK(store.set_faces(scene.faces(0, {})));
    CHECK_FALSE(store.clear_faces());
}

TEST_CASE("Selection store tracks changes of the face selection separately", "[PluginSelection]")
{
    SelectionSnapshotStore store;
    TwoPartModel           scene;
    const auto             faces_revision = store.faces_revision();

    SelectedItem item;
    item.object_index = 0;
    REQUIRE(store.set_items(SelectionSnapshotMode::Instance, {item}));
    CHECK(store.faces_revision() == faces_revision);

    REQUIRE(store.set_faces(scene.faces(0, {1})));
    CHECK(store.faces_revision() == faces_revision + 1);
    REQUIRE(store.clear_faces());
    CHECK(store.faces_revision() == faces_revision + 2);
}

TEST_CASE("Selection store normalizes the triangles of a face selection", "[PluginSelection]")
{
    SelectionSnapshotStore store;
    TwoPartModel           scene;

    // Unsorted, duplicated, and with indices outside the 12 triangles of a cube.
    REQUIRE(store.set_faces(scene.faces(1, {7, -1, 2, 7, 12, 0})));
    REQUIRE(store.set_faces(scene.faces(0, {5})));

    const SelectionSnapshot snapshot = store.snapshot();
    REQUIRE(snapshot.faces.size() == 2);
    CHECK(snapshot.faces[0].volume_index == 0);
    CHECK(snapshot.faces[0].triangles == std::vector<int>{5});
    CHECK(snapshot.faces[1].volume_index == 1);
    CHECK(snapshot.faces[1].triangles == std::vector<int>{0, 2, 7});
    CHECK(store.face_count() == 4);

    const ModelVolume* part = scene.object->volumes[1];
    CHECK(store.face_triangles(part->id().id, part->get_mesh_shared_ptr()) == std::vector<int>{0, 2, 7});
    CHECK(store.face_triangles(part->id().id, scene.object->volumes[0]->get_mesh_shared_ptr()).empty());

    CHECK(store.clear_object_faces(scene.object->id().id));
    CHECK(store.snapshot().faces.empty());
}

TEST_CASE("Selection store drops face selections that no longer match the model", "[PluginSelection]")
{
    SelectionSnapshotStore store;
    TwoPartModel           scene;
    REQUIRE(store.set_faces(scene.faces(0, {1, 2})));
    REQUIRE(store.set_faces(scene.faces(1, {4})));

    SECTION("an unchanged model keeps everything")
    {
        CHECK_FALSE(store.refresh_faces(scene.model));
        CHECK(store.snapshot().faces.size() == 2);
    }

    SECTION("a replaced mesh loses its selection")
    {
        scene.object->volumes[0]->set_mesh(make_cube(1.0, 1.0, 1.0));
        CHECK(store.refresh_faces(scene.model));
        const SelectionSnapshot snapshot = store.snapshot();
        REQUIRE(snapshot.faces.size() == 1);
        CHECK(snapshot.faces[0].volume_id == scene.object->volumes[1]->id().id);
    }

    SECTION("a deleted part loses its selection and the others are reindexed")
    {
        // A third part, so that two remain and the survivors keep their ids.
        scene.object->add_volume(make_cube(2.0, 2.0, 2.0));
        scene.object->delete_volume(0);
        CHECK(store.refresh_faces(scene.model));
        const SelectionSnapshot snapshot = store.snapshot();
        REQUIRE(snapshot.faces.size() == 1);
        CHECK(snapshot.faces[0].volume_index == 0);
        CHECK(snapshot.faces[0].triangles == std::vector<int>{4});
    }

    SECTION("the last part left keeps its selection under the new id the model gives it")
    {
        const size_t old_id = scene.object->volumes[1]->id().id;
        scene.object->delete_volume(0);
        REQUIRE(scene.object->volumes[0]->id().id != old_id);
        CHECK(store.refresh_faces(scene.model));
        const SelectionSnapshot snapshot = store.snapshot();
        REQUIRE(snapshot.faces.size() == 1);
        CHECK(snapshot.faces[0].volume_id == scene.object->volumes[0]->id().id);
        CHECK(snapshot.faces[0].volume_index == 0);
        CHECK(snapshot.faces[0].triangles == std::vector<int>{4});
        CHECK(store.face_triangles(scene.object->volumes[0]->id().id, scene.object->volumes[0]->get_mesh_shared_ptr()) ==
              std::vector<int>{4});
        CHECK_FALSE(store.refresh_faces(scene.model));
    }

    SECTION("the last part left loses its selection when its mesh was replaced as well")
    {
        scene.object->delete_volume(0);
        scene.object->volumes[0]->set_mesh(make_cube(1.0, 1.0, 1.0));
        CHECK(store.refresh_faces(scene.model));
        CHECK(store.snapshot().faces.empty());
    }

    SECTION("an emptied model loses everything")
    {
        scene.model.clear_objects();
        CHECK(store.refresh_faces(scene.model));
        CHECK(store.snapshot().faces.empty());
        CHECK_FALSE(store.refresh_faces(scene.model));
    }
}

TEST_CASE("Selection store can be read while it is written", "[PluginSelection]")
{
    SelectionSnapshotStore store;
    TwoPartModel           scene;

    std::thread writer([&] {
        for (int i = 0; i < 2000; ++i)
            store.set_faces(scene.faces(0, {i % 12, (i + 1) % 12}));
    });
    for (int i = 0; i < 2000; ++i) {
        const SelectionSnapshot snapshot = store.snapshot();
        if (!snapshot.faces.empty())
            CHECK(snapshot.faces[0].triangles.size() == 2);
    }
    writer.join();
}

TEST_CASE("Plugin host API exposes the Prepare tab selection to Python", "[PluginSelection][Python]")
{
    py::module_ orca = import_orca_module();
    py::object  host = orca.attr("host");
    for (const char* name : {"selection", "Selection", "SelectedItem", "SelectedFaces", "SelectionMode", "set_face_selection",
                             "clear_face_selection"}) {
        CAPTURE(name);
        REQUIRE(py::hasattr(host, name));
    }
    CHECK(py::hasattr(orca.attr("LifecycleEvent"), "SelectionChanged"));
    CHECK(lifecycle_event_to_string(LifecycleEvent::SelectionChanged) == "SelectionChanged");

    SelectionSnapshotStore& store = SelectionSnapshotStore::instance();
    TwoPartModel            scene;
    const ModelVolume*      part = scene.object->volumes[1];

    SelectedItem item;
    item.object_index   = 0;
    item.instance_index = 0;
    item.volume_index   = 1;
    item.object_id      = scene.object->id().id;
    item.volume_id      = part->id().id;
    store.set_items(SelectionSnapshotMode::Volume, {item});
    store.clear_faces();
    store.set_faces(scene.faces(1, {9, 4}));

    py::object selection = host.attr("selection")();
    CHECK(selection.attr("revision").cast<std::uint64_t>() == store.revision());
    CHECK(selection.attr("mode").cast<SelectionSnapshotMode>() == SelectionSnapshotMode::Volume);
    CHECK_FALSE(selection.attr("is_empty")().cast<bool>());
    CHECK(selection.attr("has_faces")().cast<bool>());

    py::list items = selection.attr("items")();
    REQUIRE(py::len(items) == 1);
    CHECK(items[0].attr("object_index").cast<int>() == 0);
    CHECK(items[0].attr("instance_index").cast<int>() == 0);
    CHECK(items[0].attr("volume_index").cast<int>() == 1);
    CHECK(items[0].attr("volume_id").cast<size_t>() == part->id().id);

    py::list faces = selection.attr("faces")();
    REQUIRE(py::len(faces) == 1);
    CHECK(faces[0].attr("object_index").cast<int>() == 0);
    CHECK(faces[0].attr("volume_index").cast<int>() == 1);
    CHECK(faces[0].attr("volume_id").cast<size_t>() == part->id().id);
    CHECK(faces[0].attr("count").cast<size_t>() == 2);
    CHECK(faces[0].attr("triangle_indices")().cast<std::vector<int>>() == std::vector<int>{4, 9});

    // The snapshot is a copy: a later change of the store does not reach it.
    store.clear_faces();
    store.set_items(SelectionSnapshotMode::Instance, {});
    CHECK(faces[0].attr("count").cast<size_t>() == 2);
    CHECK(py::len(selection.attr("items")()) == 1);
    CHECK(host.attr("selection")().attr("is_empty")().cast<bool>());

    // Setting the face selection needs the model of the running app, which a test does not have.
    CHECK_THROWS_AS(host.attr("set_face_selection")(0, 0, py::list()), py::error_already_set);
    CHECK_THROWS_AS(host.attr("clear_face_selection")(), py::error_already_set);

    bool have_numpy = false;
    try {
        py::module_::import("numpy");
        have_numpy = true;
    } catch (const py::error_already_set&) {
    }
    if (have_numpy) {
        py::object triangles = faces[0].attr("triangles")();
        CHECK(triangles.attr("dtype").attr("name").cast<std::string>() == "int32");
        CHECK(triangles.attr("tolist")().cast<std::vector<int>>() == std::vector<int>{4, 9});
    } else {
        CHECK_THROWS_AS(faces[0].attr("triangles")(), py::error_already_set);
    }
}

TEST_CASE("Plugin host API exposes painted triangles to Python", "[PluginSelection][Python]")
{
    py::object host = import_orca_module().attr("host");
    REQUIRE(py::hasattr(host, "PaintLayer"));
    REQUIRE(py::hasattr(host, "PaintedFacets"));
    py::object layers = host.attr("PaintLayer");

    TwoPartModel scene;
    ModelVolume* part = scene.object->volumes[0];
    {
        TriangleSelector supports(part->mesh());
        supports.set_facet(2, EnforcerBlockerType::ENFORCER);
        supports.set_facet(5, EnforcerBlockerType::BLOCKER);
        supports.set_facet(6, EnforcerBlockerType::BLOCKER);
        part->supported_facets.set(supports);

        TriangleSelector fuzzy(part->mesh());
        fuzzy.set_facet(11, EnforcerBlockerType::FUZZY_SKIN);
        part->fuzzy_skin_facets.set(fuzzy);
    }

    py::object py_part = py::cast(part, py::return_value_policy::reference);
    CHECK(py_part.attr("painted_states")(layers.attr("Support")).cast<std::vector<int>>() == std::vector<int>{1, 2});
    CHECK(py_part.attr("painted_states")(layers.attr("FuzzySkin")).cast<std::vector<int>>() == std::vector<int>{1});
    CHECK(py_part.attr("painted_states")(layers.attr("Seam")).cast<std::vector<int>>().empty());

    py::object enforcers = py_part.attr("painted_facets")(layers.attr("Support"));
    CHECK(enforcers.attr("count").cast<size_t>() == 1);
    CHECK(enforcers.attr("source_triangle_indices")().cast<std::vector<int>>() == std::vector<int>{2});

    py::object blockers = py_part.attr("painted_facets")(layers.attr("Support"), py::arg("state") = 2);
    CHECK(blockers.attr("count").cast<size_t>() == 2);
    CHECK(blockers.attr("source_triangle_indices")().cast<std::vector<int>>() == std::vector<int>{5, 6});

    CHECK(py_part.attr("painted_facets")(layers.attr("FuzzySkin")).attr("source_triangle_indices")().cast<std::vector<int>>() ==
          std::vector<int>{11});
    CHECK(py_part.attr("painted_facets")(layers.attr("Seam")).attr("count").cast<size_t>() == 0);
    CHECK(py_part.attr("painted_facets")(layers.attr("MultiMaterial"), 3).attr("count").cast<size_t>() == 0);
    CHECK_THROWS_AS(py_part.attr("painted_facets")(layers.attr("Support"), 0), py::error_already_set);

    bool have_numpy = false;
    try {
        py::module_::import("numpy");
        have_numpy = true;
    } catch (const py::error_already_set&) {
    }
    if (have_numpy) {
        // Each piece is one triangle of the cube here, so it has the vertices of its source.
        py::object triangles = blockers.attr("triangles")();
        CHECK(triangles.attr("shape").cast<std::pair<size_t, size_t>>() == std::make_pair(size_t(2), size_t(3)));
        CHECK_FALSE(triangles.attr("flags").attr("writeable").cast<bool>());
        CHECK(blockers.attr("vertices")().attr("shape").cast<std::pair<size_t, size_t>>().second == 3);
        CHECK(blockers.attr("source_triangles")().attr("tolist")().cast<std::vector<int>>() == std::vector<int>{5, 6});
    }
}
