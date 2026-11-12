#include "slic3r/GUI/PrepareSelection.hpp"
#include "slic3r/GUI/PrepareGeometryPicker.hpp"
#include "libslic3r/GeometryRevision.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrepareGeometry.hpp"
#include <Eigen/Core>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <thread>
#include <variant>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::PrepareGeometry;
using Catch::Matchers::WithinAbs;
namespace {
indexed_triangle_set quad(float z = 0.f, float x0 = -.5f, float x1 = .5f, float y0 = -.5f, float y1 = .5f)
{
    indexed_triangle_set m;
    m.vertices = {{x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}};
    m.indices  = {{0, 1, 2}, {0, 2, 3}};
    return m;
}
PreparePickView view()
{
    PreparePickView result;
    result.projection_view(2, 2) = -1.;
    result.viewport              = {0, 0, 200, 200};
    return result;
}
std::shared_ptr<const PrepareSelectionItem> item(const GeometryRevision& revision)
{
    MeshGeometry geometry(quad());
    auto result       = std::make_shared<PrepareSelectionItem>();
    result->target    = {1, 2, 3, 0, revision.stamp()};
    result->geometry  = std::make_shared<const SurfaceRegion>(geometry.select_surface(0, Vec3d(.1, -.1, 0), {}));
    result->local_hit = {.1, -.1, 0};
    return result;
}
} // namespace
TEST_CASE("Mesh revision witnesses retire across mutation deletion and independent copies", "[PrepareSelection]")
{
    GeometryRevision::Stamp retained;
    {
        GeometryRevision source;
        retained = source.stamp();
        REQUIRE(retained.valid());
        GeometryRevision copy(source);
        source.touch();
        REQUIRE_FALSE(retained.valid());
        REQUIRE(copy.stamp().valid());
        retained = source.stamp();
    }
    REQUIRE_FALSE(retained.valid());
}
TEST_CASE("Prepare state publishes committed values and monotonic revisioned clears", "[PrepareSelection]")
{
    PrepareSelectionStore store;
    GeometryRevision mesh;
    auto empty = store.get();
    REQUIRE_FALSE(empty.item);
    REQUIRE(empty.selection_revision == 0);
    REQUIRE(store.publish(store.request(), item(mesh), PrepareSelectionReason::Picked));
    auto selected = store.get();
    REQUIRE(selected.item);
    REQUIRE(selected.selection_revision > empty.selection_revision);
    store.clear();
    auto cleared = store.get();
    REQUIRE_FALSE(cleared.item);
    REQUIRE(cleared.selection_revision > selected.selection_revision);
    store.clear();
    REQUIRE(store.get().selection_revision == cleared.selection_revision);
    mesh.touch();
    REQUIRE(selected.item->target.mesh.valid() == false);
    REQUIRE(std::get<std::shared_ptr<const SurfaceRegion>>(selected.item->geometry)->mesh->indices.size() == 2);
}
TEST_CASE("Pending jobs cannot publish after newer requests or document replacement", "[PrepareSelection]")
{
    PrepareSelectionStore store;
    GeometryRevision mesh;
    auto old     = store.request();
    auto current = store.request();
    REQUIRE_FALSE(store.publish(old, item(mesh), PrepareSelectionReason::Picked));
    REQUIRE(store.publish(current, item(mesh), PrepareSelectionReason::Picked));
    auto generation = store.get().document_generation;
    store.new_document();
    REQUIRE(store.get().document_generation != generation);
    REQUIRE_FALSE(store.publish(current, item(mesh), PrepareSelectionReason::Picked));
    REQUIRE_FALSE(store.get().item);
}
TEST_CASE("Mesh invalidation becomes visible to worker readers without GUI access", "[PrepareSelection]")
{
    PrepareSelectionStore store;
    GeometryRevision mesh;
    REQUIRE(store.publish(store.request(), item(mesh), PrepareSelectionReason::Picked));
    mesh.touch();
    PrepareSelectionState result;
    std::thread reader([&] { result = store.get(); });
    reader.join();
    REQUIRE_FALSE(result.item);
    REQUIRE(result.reason == PrepareSelectionReason::Invalidated);
}
TEST_CASE("Native observers are deferred isolated and safe to unsubscribe during delivery", "[PrepareSelection]")
{
    PrepareSelectionStore store;
    GeometryRevision mesh;
    int first = 0, second = 0, late = 0;
    size_t id = 0;
    id        = store.observe([&](const auto&) {
        ++first;
        store.unobserve(id);
        store.get();
        throw std::runtime_error("observer");
    });
    store.observe([&](const auto&) { ++second; });
    REQUIRE(store.publish(store.request(), item(mesh), PrepareSelectionReason::Picked));
    REQUIRE(first == 0);
    store.observe([&](const auto&) { ++late; });
    store.dispatch_pending();
    REQUIRE(first == 1);
    REQUIRE(second == 1);
    REQUIRE(late == 0);
    store.clear();
    store.dispatch_pending();
    REQUIRE(first == 1);
    REQUIRE(second == 2);
    REQUIRE(late == 1);
}
TEST_CASE("Settings preview cancel apply and Done preserve committed state", "[PrepareSelection]")
{
    PrepareSelectionStore store;
    GeometryRevision mesh;
    PrepareSelectionSession session(store);
    session.open();
    REQUIRE(session.accept(item(mesh)));
    auto committed               = store.get();
    auto settings                = session.settings();
    settings.surface.plane_angle = 2.;
    session.edit(settings);
    REQUIRE(session.pending_settings());
    REQUIRE(store.get().selection_revision == committed.selection_revision);
    session.cancel_settings();
    REQUIRE_FALSE(session.pending_settings());
    REQUIRE_THAT(session.settings().surface.plane_angle, WithinAbs(1., 1e-12));
    session.edit(settings);
    auto adjusted      = std::make_shared<PrepareSelectionItem>(*item(mesh));
    adjusted->settings = settings;
    REQUIRE(session.accept(adjusted, true));
    REQUIRE(store.get().reason == PrepareSelectionReason::SettingsApplied);
    settings.surface.plane_angle = 3.;
    session.edit(settings);
    session.done();
    REQUIRE_FALSE(session.picking());
    REQUIRE_FALSE(session.pending_settings());
    REQUIRE(store.get().item);
    REQUIRE_THAT(session.settings().surface.plane_angle, WithinAbs(2., 1e-12));
}
TEST_CASE("Escape cancels preview then clears selection then exits picking", "[PrepareSelection]")
{
    PrepareSelectionStore store;
    GeometryRevision mesh;
    PrepareSelectionSession session(store);
    session.open();
    REQUIRE(session.accept(item(mesh)));
    auto settings   = session.settings();
    settings.radius = 10.;
    session.edit(settings);
    REQUIRE_FALSE(session.escape());
    REQUIRE(store.get().item);
    REQUIRE_FALSE(session.escape());
    REQUIRE_FALSE(store.get().item);
    REQUIRE(session.escape());
    REQUIRE_FALSE(session.picking());
}
TEST_CASE("Switching selection kind restores committed settings before the next pick", "[PrepareSelection]")
{
    PrepareSelectionStore store;
    GeometryRevision mesh;
    PrepareSelectionSession session(store);
    REQUIRE(session.accept(item(mesh)));
    auto settings                = session.settings();
    settings.surface.plane_angle = 4.;
    session.edit(settings);
    settings.mode = PreparePickMode::Vertex;
    session.edit(settings);
    REQUIRE_FALSE(session.pending_settings());
    REQUIRE(session.settings().mode == PreparePickMode::Vertex);
    REQUIRE_THAT(session.settings().surface.plane_angle, WithinAbs(1., 1e-12));
    REQUIRE(store.get().item->settings.mode == PreparePickMode::Surface);
}
TEST_CASE("Placement updates preserve geometry and suspend singular instances", "[PrepareSelection]")
{
    PrepareSelectionStore store;
    GeometryRevision mesh;
    auto selected = item(mesh);
    REQUIRE(store.publish(store.request(), selected, PrepareSelectionReason::Picked));
    Transform3d transform = Transform3d::Identity();
    transform.translate(Vec3d(1, 2, 3));
    store.transform(transform);
    REQUIRE(store.get().reason == PrepareSelectionReason::TransformChanged);
    REQUIRE(store.get().item->geometry == selected->geometry);
    transform.scale(Vec3d(0, 1, 1));
    store.transform(transform);
    REQUIRE_FALSE(store.get().item);
}
TEST_CASE("Source picking resolves the nearest visible original surface across volumes", "[PrepareSelection]")
{
    auto geometry                      = std::make_shared<const MeshGeometry>(quad());
    std::vector<PreparePickPart> scene = {{std::make_shared<const PreparePickMesh>(geometry)}};
    auto result                        = prepare_pick_candidates(scene, 0, *geometry, view(), {110, 110}, {});
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].triangle == 0);
    auto near = std::make_shared<const indexed_triangle_set>(quad(.2f));
    scene.push_back({std::make_shared<const PreparePickMesh>(near)});
    REQUIRE(prepare_pick_candidates(scene, 0, *geometry, view(), {110, 110}, {}).empty());
    auto clipped   = view();
    clipped.planes = {{0, 0, -1, .1}};
    REQUIRE(prepare_pick_candidates(scene, 0, *geometry, clipped, {110, 110}, {}).size() == 1);
}
TEST_CASE("Open source surfaces obey explicit sidedness without odd-hit rejection", "[PrepareSelection]")
{
    auto geometry                      = std::make_shared<const MeshGeometry>(quad());
    std::vector<PreparePickPart> scene = {{std::make_shared<const PreparePickMesh>(geometry)}};
    auto camera                        = view();
    camera.projection_view(2, 2)       = 1.;
    REQUIRE(prepare_pick_candidates(scene, 0, *geometry, camera, {110, 110}, {}).empty());
    scene[0].two_sided = true;
    REQUIRE(prepare_pick_candidates(scene, 0, *geometry, camera, {110, 110}, {}).size() == 1);
}
TEST_CASE("Vertex snapping uses logical pixels and deterministic coincident source indices", "[PrepareSelection]")
{
    auto source = quad();
    source.vertices.push_back(source.vertices[0]);
    auto geometry                      = std::make_shared<const MeshGeometry>(source);
    std::vector<PreparePickPart> scene = {{std::make_shared<const PreparePickMesh>(geometry)}};
    PreparePickSettings settings;
    settings.mode      = PreparePickMode::Vertex;
    settings.radius    = 8.;
    double scale       = GENERATE(1., 1.5, 2.);
    auto camera        = view();
    camera.pixel_scale = scale;
    camera.viewport    = {0, 0, int(200 * scale), int(200 * scale)};
    auto inside        = prepare_pick_candidates(scene, 0, *geometry, camera, {(50 + 7.5) * scale, 150 * scale}, settings);
    REQUIRE(inside.size() == 2);
    REQUIRE(inside[0].vertex == 0);
    REQUIRE(inside[1].vertex == 4);
    REQUIRE(prepare_pick_candidates(scene, 0, *geometry, camera, {(50 + 8.5) * scale, 150 * scale}, settings).empty());
}
TEST_CASE("Snapped vertices and edges are tested for occlusion at their own positions", "[PrepareSelection]")
{
    auto geometry = std::make_shared<const MeshGeometry>(quad());
    auto mode     = GENERATE(PreparePickMode::Vertex, PreparePickMode::MeshEdge);
    float y0 = mode == PreparePickMode::Vertex ? -.52f : -.1f, y1 = mode == PreparePickMode::Vertex ? -.48f : .1f;
    auto occluder                      = std::make_shared<const indexed_triangle_set>(quad(.2f, -.52f, -.48f, y0, y1));
    std::vector<PreparePickPart> scene = {{std::make_shared<const PreparePickMesh>(geometry)},
                                          {std::make_shared<const PreparePickMesh>(occluder)}};
    Vec2d mouse(55, mode == PreparePickMode::Vertex ? 150 : 100);
    REQUIRE(prepare_pick_candidates(scene, 0, *geometry, view(), mouse, {}).size() == 1); // cursor ray is unobscured
    PreparePickSettings settings;
    settings.mode   = mode;
    settings.radius = 8.;
    REQUIRE(prepare_pick_candidates(scene, 0, *geometry, view(), mouse, settings).empty());
}
TEST_CASE("Feature candidate graphs prevent snapping to coplanar diagonals", "[PrepareSelection]")
{
    auto geometry                      = std::make_shared<const MeshGeometry>(quad());
    std::vector<PreparePickPart> scene = {{std::make_shared<const PreparePickMesh>(geometry)}};
    PreparePickSettings settings;
    settings.mode = PreparePickMode::FeatureChain;
    auto graph    = geometry->build_chain_graph(0.);
    REQUIRE(prepare_pick_candidates(scene, 0, *geometry, view(), {100, 100}, settings, {}, &graph).empty());
    settings.mode = PreparePickMode::MeshEdge;
    REQUIRE(prepare_pick_candidates(scene, 0, *geometry, view(), {100, 100}, settings).size() == 1);
}

TEST_CASE("Interactive transforms suspend publication and restore only valid source membership", "[PrepareSelection]")
{
    PrepareSelectionStore store;
    GeometryRevision mesh;
    auto original = item(mesh);
    REQUIRE(store.publish(store.request(), original, PrepareSelectionReason::Picked));
    const auto before = store.get().selection_revision;
    store.suspend();
    REQUIRE_FALSE(store.get().item);
    REQUIRE(store.get().reason == PrepareSelectionReason::Invalidated);
    Transform3d placement = Transform3d::Identity();
    placement.translate(Vec3d(4, 5, 6));
    store.resume(placement);
    REQUIRE(store.get().item);
    REQUIRE(store.get().selection_revision > before);
    REQUIRE(store.get().item->geometry == original->geometry);
    store.suspend();
    mesh.touch();
    store.resume(placement);
    REQUIRE_FALSE(store.get().item);
}

TEST_CASE("New seeds reject an unapplied preview and document changes cancel suspended transforms", "[PrepareSelection]")
{
    PrepareSelectionStore store;
    GeometryRevision mesh;
    PrepareSelectionSession session(store);
    REQUIRE(session.accept(item(mesh)));
    auto settings                = session.settings();
    settings.surface.plane_angle = 4.;
    session.edit(settings);
    auto preview      = std::make_shared<PrepareSelectionItem>(*item(mesh));
    preview->settings = settings;
    REQUIRE_FALSE(session.accept(preview));
    REQUIRE_THAT(session.settings().surface.plane_angle, WithinAbs(1., 1e-12));
    REQUIRE_THAT(store.get().item->settings.surface.plane_angle, WithinAbs(1., 1e-12));
    store.suspend();
    store.new_document();
    store.resume(Transform3d::Identity());
    REQUIRE_FALSE(store.get().item);
}

TEST_CASE("Edge snapping finds the nearest eligible portion of an original clipped edge", "[PrepareSelection]")
{
    auto geometry                      = std::make_shared<const MeshGeometry>(quad());
    std::vector<PreparePickPart> scene = {{std::make_shared<const PreparePickMesh>(geometry)}};
    auto camera                        = view();
    camera.planes                      = {{0, 1, 0, 0}};
    PreparePickSettings settings;
    settings.mode   = PreparePickMode::MeshEdge;
    auto candidates = prepare_pick_candidates(scene, 0, *geometry, camera, {50, 105}, settings);
    REQUIRE(candidates.size() == 1);
    REQUIRE(candidates.front().edge == EdgeKey{0, 3});
    REQUIRE_THAT(candidates.front().local_hit.y(), WithinAbs(0., 1e-10));
    REQUIRE_THAT(candidates.front().screen_distance, WithinAbs(5., 1e-10));
}

TEST_CASE("Perspective snapping preserves source coordinates under mirrored nonuniform placement", "[PrepareSelection]")
{
    auto geometry = std::make_shared<const MeshGeometry>(quad());
    PreparePickPart part{std::make_shared<const PreparePickMesh>(geometry)};
    part.placement.translate(Vec3d(0, 0, -2));
    part.placement.scale(Vec3d(-2, 1, 3));
    std::vector<PreparePickPart> scene = {part};
    auto camera                        = view();
    camera.projection_view.setZero();
    camera.projection_view(0, 0) = camera.projection_view(1, 1) = 1.;
    camera.projection_view(2, 2)                                = -11. / 9.;
    camera.projection_view(2, 3)                                = -20. / 9.;
    camera.projection_view(3, 2)                                = -1.;
    PreparePickSettings settings;
    settings.mode   = PreparePickMode::Vertex;
    auto candidates = prepare_pick_candidates(scene, 0, *geometry, camera, {153, 125}, settings);
    REQUIRE(candidates.size() == 1);
    REQUIRE(candidates.front().vertex == 0);
    REQUIRE_THAT((candidates.front().local_hit - Vec3d(-.5, -.5, 0)).norm(), WithinAbs(0., 1e-10));
    scene.front().placement.scale(Vec3d(0, 1, 1));
    REQUIRE_THROWS_AS(prepare_pick_candidates(scene, 0, *geometry, camera, {153, 125}, settings), std::invalid_argument);
}

TEST_CASE("Pick gestures reject drags that return to their start and canceled releases at every scale", "[PrepareSelection]")
{
    const double scale = GENERATE(1., 1.5, 2.);
    PreparePickGesture gesture;
    gesture.begin({10, 20}, scale);
    gesture.move({10 + 2.9 * scale, 20});
    REQUIRE(gesture.release());
    REQUIRE_FALSE(gesture.release());
    gesture.begin({10, 20}, scale);
    gesture.move({10 + 3.1 * scale, 20});
    gesture.move({10, 20});
    REQUIRE_FALSE(gesture.release());
    gesture.begin({10, 20}, scale);
    gesture.cancel();
    REQUIRE_FALSE(gesture.release());
}

TEST_CASE("Every geometry kind retains detached membership through transforms and invalidation", "[PrepareSelection]")
{
    const int kind = GENERATE(0, 1, 2);
    GeometryRevision source;
    MeshGeometry geometry(quad());
    auto picked = std::make_shared<PrepareSelectionItem>(*item(source));
    if (kind == 1) {
        picked->settings.mode = PreparePickMode::MeshEdge;
        picked->geometry      = std::make_shared<const EdgeSelection>(geometry.select_edge({0, 1}));
    } else if (kind == 2) {
        picked->settings.mode = PreparePickMode::Vertex;
        picked->geometry      = std::make_shared<const VertexSelection>(geometry.select_vertex(0));
    }
    PrepareSelectionStore store;
    std::vector<PrepareSelectionState> observed;
    store.observe([&](const auto& state) { observed.push_back(state); });
    REQUIRE(store.publish(store.request(), picked, PrepareSelectionReason::Picked));
    auto retained = store.get().item;
    store.suspend();
    REQUIRE_FALSE(store.get().item);
    Transform3d placement = Transform3d::Identity();
    placement.translate(Vec3d(12, -3, 5));
    store.resume(placement);
    REQUIRE(store.get().item->geometry == retained->geometry);
    REQUIRE(store.get().item->target.same_source(retained->target));
    source.touch();
    REQUIRE_FALSE(store.get().item);
    REQUIRE(store.get().reason == PrepareSelectionReason::Invalidated);
    store.dispatch_pending();
    REQUIRE(observed.size() == 4);
    for (size_t i = 1; i < observed.size(); ++i)
        REQUIRE(observed[i].selection_revision > observed[i - 1].selection_revision);
    REQUIRE(std::visit([](const auto& selection) { return selection->mesh->vertices.size(); }, retained->geometry) == 4);
}

TEST_CASE("Nested observer dispatch preserves revision order", "[PrepareSelection]")
{
    GeometryRevision mesh;
    PrepareSelectionStore store;
    std::vector<PrepareSelectionState> delivered;
    store.observe([&](const auto& state) {
        delivered.push_back(state);
        if (state.reason == PrepareSelectionReason::Picked) {
            store.clear();
            store.dispatch_pending();
        }
    });
    REQUIRE(store.publish(store.request(), item(mesh), PrepareSelectionReason::Picked));
    Transform3d placement = Transform3d::Identity();
    placement.translate(Vec3d(2, 0, 0));
    store.transform(placement);
    store.dispatch_pending();
    REQUIRE(delivered.size() == 2);
    store.dispatch_pending();
    REQUIRE(delivered.size() == 3);
    REQUIRE(delivered[0].reason == PrepareSelectionReason::Picked);
    REQUIRE(delivered[1].reason == PrepareSelectionReason::TransformChanged);
    REQUIRE(delivered[2].reason == PrepareSelectionReason::Cleared);
    REQUIRE(delivered[0].selection_revision < delivered[1].selection_revision);
    REQUIRE(delivered[1].selection_revision < delivered[2].selection_revision);
}
