#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"
#include "GLGizmoPrepareSelection.hpp"
#include "GLGizmoBase.hpp"
#include "slic3r/GUI/PrepareSelection.hpp"
#include "slic3r/GUI/PrepareGeometryPicker.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/Jobs/BoostThreadWorker.hpp"
#include "slic3r/GUI/Jobs/Job.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/GeometryRevision.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrepareGeometry.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Color.hpp"
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <glad/gl.h>
#include <imgui.h>
#include <wx/timer.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <type_traits>

namespace Slic3r::GUI {
namespace {
namespace PG = PrepareGeometry;
struct Cancelled
{};
struct Overlay
{
    GLModel::Geometry data;
    Transform3d placement = Transform3d::Identity();
};
// Clip only the display overlay. These interpolated vertices never become selectable source IDs.
Overlay overlay(const PrepareSelectionItem& item, const PreparePickView& view, bool wire, const PG::Cancel& cancel)
{
    Overlay out;
    out.placement                 = item.local_to_world;
    out.data.format.vertex_layout = GLModel::Geometry::EVertexLayout::P3;
    out.data.format.type          = GLModel::Geometry::EPrimitiveType::Lines;
    auto add                      = [&](const Vec3d& p) {
        out.data.add_vertex(Vec3f(p.cast<float>()));
        out.data.add_index(unsigned(out.data.vertices_count() - 1));
    };
    auto segment = [&](Vec3d a, Vec3d b) {
        for (const auto& plane : view.planes) {
            const double da = plane.head<3>().dot(item.local_to_world * a) + plane.w();
            const double db = plane.head<3>().dot(item.local_to_world * b) + plane.w();
            if (da < 0. && db < 0.)
                return;
            if ((da < 0.) != (db < 0.)) {
                Vec3d p = a + (da / (da - db)) * (b - a);
                if (da < 0.)
                    a = p;
                else
                    b = p;
            }
        }
        if (wire && std::holds_alternative<std::shared_ptr<const PG::EdgeSelection>>(item.geometry)) {
            // A dashed preview stays distinguishable from the committed solid edge without colour.
            constexpr int pieces = 12;
            for (int i = 0; i < pieces; i += 2) {
                add(a + (double(i) / pieces) * (b - a));
                add(a + (double(i + 1) / pieces) * (b - a));
            }
        } else {
            add(a);
            add(b);
        }
    };
    std::visit(
        [&](const auto& selected) {
            using Value      = typename std::decay_t<decltype(selected)>::element_type;
            const auto& mesh = *selected->mesh;
            if constexpr (std::is_same_v<Value, const PG::SurfaceRegion>) {
                out.data.format.type = wire ? GLModel::Geometry::EPrimitiveType::Lines : GLModel::Geometry::EPrimitiveType::Triangles;
                for (size_t i = 0; i < selected->triangles.size(); ++i) {
                    if (cancel && i % 1024 == 0)
                        cancel();
                    const auto& t = mesh.indices[selected->triangles[i]];
                    if (wire) {
                        for (int s = 0; s < 3; ++s)
                            segment(mesh.vertices[t[s]].template cast<double>(), mesh.vertices[t[(s + 1) % 3]].template cast<double>());
                        continue;
                    }
                    std::vector<Vec3d> polygon;
                    for (int v : t)
                        polygon.push_back(mesh.vertices[v].template cast<double>());
                    for (const auto& plane : view.planes) {
                        std::vector<Vec3d> clipped;
                        for (size_t k = 0; k < polygon.size(); ++k) {
                            const auto& a = polygon[k];
                            const auto& b = polygon[(k + 1) % polygon.size()];
                            double da     = plane.head<3>().dot(item.local_to_world * a) + plane.w(),
                                   db     = plane.head<3>().dot(item.local_to_world * b) + plane.w();
                            if (da >= 0.)
                                clipped.push_back(a);
                            if ((da < 0.) != (db < 0.))
                                clipped.push_back(a + (da / (da - db)) * (b - a));
                        }
                        polygon = std::move(clipped);
                    }
                    for (size_t k = 1; k + 1 < polygon.size(); ++k) {
                        add(polygon[0]);
                        add(polygon[k]);
                        add(polygon[k + 1]);
                    }
                }
            } else if constexpr (std::is_same_v<Value, const PG::EdgeSelection>) {
                for (size_t i = 0; i < selected->edges.size(); ++i) {
                    if (cancel && i % 1024 == 0)
                        cancel();
                    const auto& e = selected->edges[i];
                    segment(mesh.vertices[e[0]].template cast<double>(), mesh.vertices[e[1]].template cast<double>());
                }
            } else {
                out.data.format.type = GLModel::Geometry::EPrimitiveType::Points;
                bool visible         = true;
                for (const auto& p : view.planes)
                    if (p.head<3>().dot(item.local_to_world * selected->position) + p.w() < 0.)
                        visible = false;
                if (visible)
                    add(selected->position);
            }
        },
        item.geometry);
    if (cancel)
        cancel();
    return out;
}
struct PollTimer final : wxTimer
{
    std::function<void()> poll;
    void Notify() override
    {
        try {
            if (poll)
                poll();
        } catch (...) {
            Stop();
        }
    }
};
} // namespace

struct GLGizmoPrepareSelection::Impl
{
    GLGizmoPrepareSelection& g;
    PrepareSelectionStore& store;
    PrepareSelectionSession session;
    std::unique_ptr<BoostThreadWorker> worker;
    PollTimer timer;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<bool>> cancelled;
    bool busy = false, retain = false, refresh = true, overlay_needed = false, scene_dirty = true;
    PreparePickGesture gesture;
    int job_kind = 0; // 1 source preparation, 2 view-dependent query, 3 committed display
    std::optional<Vec2d> click;
    PreparePickView click_view;
    Vec2d last_pixel      = Vec2d(-1, -1);
    std::uint64_t context = 0, document = 0, scene_revision = 0;
    PrepareTarget target;
    PreparePickView last_view;
    std::vector<std::array<std::uint64_t, 3>> signature;
    std::uint64_t chosen_part = 0;
    struct Part
    {
        const indexed_triangle_set* identity = nullptr;
        GeometryRevision::Stamp stamp;
        std::shared_ptr<const TriangleMesh> owner;
        std::shared_ptr<const indexed_triangle_set> captured;
        PreparePickPart pick;
        std::shared_ptr<const PG::MeshGeometry> geometry;
    };
    std::vector<Part> ready;
    struct SceneMember
    {
        const GLVolume* volume           = nullptr; // UI-only identity; never dereferenced by a worker
        const indexed_triangle_set* mesh = nullptr;
        GeometryRevision::Stamp stamp;
        Transform3d placement = Transform3d::Identity();
    };
    std::vector<SceneMember> scene_members;
    size_t target_part = 0;
    std::shared_ptr<const PG::ChainGraph> graph;
    std::shared_ptr<const PG::SurfaceRegion> boundary;
    std::vector<PreparePickCandidate> candidates;
    size_t candidate = 0;
    std::shared_ptr<const PrepareSelectionItem> preview;
    std::shared_ptr<const PrepareSelectionItem> displayed;
    Overlay committed_data, preview_data, candidates_data;
    bool commit_upload = false, preview_upload = false, candidates_upload = false, hidden_by_clipping = false;
    GLModel committed_gl, preview_gl, candidates_gl;
    std::string status;

    explicit Impl(GLGizmoPrepareSelection& gizmo) : g(gizmo), store(g.m_parent.prepare_selection()), session(store)
    {
        timer.poll = [this] { tick(); };
    }
    ~Impl()
    {
        alive->store(false);
        timer.Stop();
        cancel();
        worker.reset();
    }
    void cancel()
    {
        if (cancelled)
            cancelled->store(true);
    }
    void dirty()
    {
        g.m_parent.set_as_dirty();
        g.m_parent.request_extra_frame();
    }
    void invalidate()
    {
        cancel();
        ++context;
        session.invalidate();
        preview.reset();
        boundary.reset();
        graph.reset();
        candidates.clear();
        ready.clear();
        scene_members.clear();
        displayed.reset();
        target      = {};
        refresh     = true;
        scene_dirty = true;
        gesture.cancel();
        click.reset();
        status.clear();
        dirty();
    }
    void tick()
    {
        // Resolve placements/visibility before draining a completion: numeric
        // moves need not recreate GL volumes or advance reload_scene's revision.
        if (g.m_state == On || store.get().item)
            sync();
        if (worker)
            worker->process_events();
        if (target.volume && !target.mesh.valid())
            invalidate();
        store.dispatch_pending();
        if (worker && worker->is_idle())
            timer.Stop();
    }
    template<class Process, class Finish> void launch(Process process, Finish finish, int kind = 1)
    {
        if (busy)
            return;
        busy      = true;
        job_kind  = kind;
        cancelled = std::make_shared<std::atomic<bool>>(false);
        auto flag = cancelled, guard = alive;
        auto work = [flag, process = std::move(process)](Job::Ctl& ctl) mutable {
            PG::Cancel check = [&] {
                if (flag->load() || ctl.was_canceled())
                    throw Cancelled{};
            };
            process(check);
        };
        auto final = [this, guard, flag, finish = std::move(finish)](bool was_cancelled, std::exception_ptr& error) mutable {
            if (!guard->load()) {
                error = nullptr;
                return;
            }
            busy = false;
            if (error) {
                try {
                    std::rethrow_exception(error);
                } catch (const Cancelled&) {
                } catch (...) {
                    status = _u8L("Geometry selection could not be calculated for this mesh.");
                }
                error = nullptr;
            } else if (!was_cancelled && !flag->load()) {
                try {
                    finish();
                } catch (...) {
                    status = _u8L("Geometry selection could not be calculated for this mesh.");
                }
            }
            dirty();
        };
        if (!queue_job(*worker, std::move(work), std::move(final))) {
            busy   = false;
            status = _u8L("Geometry selection is busy. Try again.");
        }
        timer.Start(16);
    }
    std::vector<const GLVolume*> parts() const
    {
        std::vector<const GLVolume*> result;
        const Model* model = g.m_parent.get_model();
        if (!model)
            return result;
        const auto& selection = g.m_parent.get_selection();
        if (!selection.is_from_single_instance())
            return result;
        const auto& volumes = g.m_parent.get_volumes().volumes;
        for (unsigned id : selection.get_volume_idxs()) {
            if (id >= volumes.size())
                continue;
            const auto* v = volumes[id];
            if (!v || !v->is_active || v->is_modifier || v->is_wipe_tower || v->object_idx() < 0 ||
                size_t(v->object_idx()) >= model->objects.size())
                continue;
            const auto* object = model->objects[v->object_idx()];
            if (v->volume_idx() < 0 || size_t(v->volume_idx()) >= object->volumes.size() || v->instance_idx() < 0 ||
                size_t(v->instance_idx()) >= object->instances.size())
                continue;
            auto* plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
            if (plate && plate->contain_instance(v->object_idx(), v->instance_idx()) && object->volumes[v->volume_idx()]->is_model_part())
                result.push_back(v);
        }
        return result;
    }
    PrepareTarget identity(const GLVolume& v) const
    {
        const auto* object = g.m_parent.get_model()->objects[v.object_idx()];
        const auto* volume = object->volumes[v.volume_idx()];
        return {object->id().id, volume->id().id, object->instances[v.instance_idx()]->id().id,
                wxGetApp().plater()->get_partplate_list().get_curr_plate_index(), volume->geometry_stamp()};
    }
    PreparePickView view() const
    {
        const auto& camera = wxGetApp().plater()->get_camera();
        PreparePickView result;
        result.projection_view = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
        result.viewport        = camera.get_viewport();
        result.pixel_scale     = g.m_parent.get_scale();
        result.planes          = g.m_parent.prepare_pick_planes();
        return result;
    }
    bool sync()
    {
        auto available = parts();
        std::vector<std::array<std::uint64_t, 3>> current;
        for (const auto* v : available) {
            auto id = identity(*v);
            current.push_back({id.object, id.volume, id.instance});
        }
        const auto state = store.get();
        if (document != state.document_generation || current != signature) {
            invalidate();
            document  = state.document_generation;
            signature = current;
        }
        if (available.empty())
            return false;
        auto selected = std::find_if(available.begin(), available.end(), [&](const auto* v) { return identity(*v).volume == chosen_part; });
        const auto* v = selected == available.end() ? available.front() : *selected;
        auto id       = identity(*v);
        if (!target.same_source(id)) {
            invalidate();
            target      = id;
            chosen_part = id.volume;
        }
        auto live = store.get().item;
        if (live)
            store.transform(v->world_matrix());
        std::vector<SceneMember> current_scene;
        const auto* model = g.m_parent.get_model();
        for (const auto* volume : g.m_parent.get_volumes().volumes) {
            if (!volume || !volume->is_active || volume->is_extrusion_path)
                continue;
            SceneMember member;
            member.volume    = volume;
            member.placement = volume->world_matrix();
            if (volume->object_idx() >= 0 && size_t(volume->object_idx()) < model->objects.size()) {
                const auto* object = model->objects[volume->object_idx()];
                if (volume->volume_idx() >= 0 && size_t(volume->volume_idx()) < object->volumes.size()) {
                    const auto* source = object->volumes[volume->volume_idx()];
                    member.mesh        = &source->mesh().its;
                    member.stamp       = source->geometry_stamp();
                }
            }
            if (!member.mesh && volume->mesh_raycaster)
                member.mesh = volume->mesh_raycaster->get_aabb_mesh().get_triangle_mesh();
            if (member.mesh)
                current_scene.push_back(std::move(member));
        }
        const bool same_scene = current_scene.size() == scene_members.size() &&
                                std::equal(current_scene.begin(), current_scene.end(), scene_members.begin(),
                                           [](const auto& a, const auto& b) {
                                               return a.volume == b.volume && a.mesh == b.mesh && a.stamp.same_source(b.stamp) &&
                                                      a.placement.matrix() == b.placement.matrix();
                                           });
        if (!same_scene || scene_revision != g.m_parent.prepare_scene_revision()) {
            cancel();
            store.request();
            click.reset();
            gesture.cancel();
            preview.reset();
            candidates.clear();
            scene_dirty    = true;
            refresh        = true;
            scene_revision = g.m_parent.prepare_scene_revision();
            scene_members  = std::move(current_scene);
        }
        return true;
    }
    void prepare()
    {
        std::vector<Part> captured;
        size_t target_index = 0;
        bool found_target   = false;
        const Model* model  = g.m_parent.get_model();
        for (const auto* v : g.m_parent.get_volumes().volumes) {
            if (!v || !v->is_active || v->is_extrusion_path)
                continue;
            Part p;
            p.pick.placement = v->world_matrix();
            if (v->object_idx() >= 0 && size_t(v->object_idx()) < model->objects.size()) {
                const auto* object = model->objects[v->object_idx()];
                if (v->volume_idx() >= 0 && size_t(v->volume_idx()) < object->volumes.size()) {
                    auto* volume = object->volumes[v->volume_idx()];
                    p.owner      = volume->mesh_ptr();
                    p.identity   = &p.owner->its;
                    p.stamp      = volume->geometry_stamp();
                    if (volume->id().id == target.volume && v->instance_idx() >= 0 &&
                        size_t(v->instance_idx()) < object->instances.size() &&
                        object->instances[v->instance_idx()]->id().id == target.instance) {
                        target_index = captured.size();
                        found_target = true;
                    }
                }
            }
            if (!p.identity && v->mesh_raycaster)
                p.identity = v->mesh_raycaster->get_aabb_mesh().get_triangle_mesh();
            if (!p.identity)
                continue;
            auto cached = std::find_if(ready.begin(), ready.end(), [&](const Part& old) {
                return p.owner && old.identity == p.identity && old.stamp.same_source(p.stamp);
            });
            if (cached != ready.end()) {
                p.pick.mesh = cached->pick.mesh;
                p.geometry  = cached->geometry;
            } else
                p.captured = std::make_shared<const indexed_triangle_set>(*p.identity); // UI capture, no borrowed live arrays on worker
            captured.push_back(std::move(p));
        }
        if (captured.empty() || !found_target)
            return;
        auto result           = std::make_shared<std::vector<Part>>();
        const auto generation = context;
        launch(
            [captured = std::move(captured), result, target_index](const PG::Cancel& check) mutable {
                for (size_t i = 0; i < captured.size(); ++i) {
                    check();
                    auto& p = captured[i];
                    if (i == target_index) {
                        if (!p.geometry) {
                            p.geometry  = std::make_shared<const PG::MeshGeometry>(p.captured ? *p.captured : *p.pick.mesh->mesh, check);
                            p.pick.mesh = std::make_shared<const PreparePickMesh>(p.geometry, check);
                        }
                    } else if (!p.pick.mesh) {
                        auto shared = std::find_if(result->begin(), result->end(),
                                                   [&](const Part& old) { return old.identity == p.identity; });
                        p.pick.mesh = shared == result->end() ? std::make_shared<const PreparePickMesh>(p.captured, check) :
                                                                shared->pick.mesh;
                    }
                    p.captured.reset();
                    result->push_back(std::move(p));
                }
            },
            [this, result, generation, target_index] {
                if (generation == context && target.mesh.valid()) {
                    ready       = std::move(*result);
                    target_part = target_index;
                    refresh     = true;
                    scene_dirty = false;
                }
            });
    }
    static std::shared_ptr<const PrepareSelectionItem> selection(const PrepareTarget& target,
                                                                 const PreparePickCandidate& seed,
                                                                 const PreparePickSettings& settings,
                                                                 const std::shared_ptr<const PG::MeshGeometry>& geometry,
                                                                 const Transform3d& placement,
                                                                 const std::shared_ptr<const PG::ChainGraph>& chain,
                                                                 const PG::Cancel& check)
    {
        auto item            = std::make_shared<PrepareSelectionItem>();
        item->target         = target;
        item->settings       = settings;
        item->local_to_world = placement;
        item->local_hit      = seed.local_hit;
        if (settings.mode == PreparePickMode::Surface) {
            auto region     = geometry->select_surface(seed.triangle, seed.local_hit, settings.surface, check);
            item->local_hit = region.seed_hit;
            item->geometry  = std::make_shared<const PG::SurfaceRegion>(std::move(region));
        } else if (settings.mode == PreparePickMode::Vertex) {
            item->geometry = std::make_shared<const PG::VertexSelection>(geometry->select_vertex(*seed.vertex));
        } else if (settings.mode == PreparePickMode::MeshEdge) {
            item->geometry = std::make_shared<const PG::EdgeSelection>(geometry->select_edge(*seed.edge));
        } else {
            item->geometry = std::make_shared<const PG::EdgeSelection>(chain->select_chain(*seed.edge, settings.chain.turn_angle, check));
        }
        return item;
    }
    void query(const PreparePickView& camera, const Vec2d& pixel)
    {
        if (ready.empty() || target_part >= ready.size())
            return;
        auto geometry = ready[target_part].geometry;
        if (!geometry)
            return;
        std::vector<PreparePickPart> scene;
        for (const auto& p : ready)
            scene.push_back(p.pick);
        auto settings          = session.settings();
        auto fixed             = store.get().item;
        auto retained_boundary = boundary;
        auto existing_graph    = graph;
        struct Result
        {
            std::vector<PreparePickCandidate> candidates;
            std::shared_ptr<const PrepareSelectionItem> item;
            Overlay display, nearby;
            std::shared_ptr<const PG::ChainGraph> graph;
        };
        auto result                     = std::make_shared<Result>();
        const auto captured_target      = target;
        const auto captured_target_part = target_part;
        const auto ticket               = store.request();
        const auto generation           = context;
        const bool settings_preview     = session.pending_settings();
        const bool commit_intent        = click.has_value();
        launch(
            [result, scene = std::move(scene), geometry, camera, pixel, settings, fixed, retained_boundary, existing_graph, captured_target,
             captured_target_part, settings_preview](const PG::Cancel& check) {
                auto chain = existing_graph;
                if (settings.mode == PreparePickMode::FeatureChain || settings.mode == PreparePickMode::BoundaryChain) {
                    if (!chain)
                        chain = std::make_shared<const PG::ChainGraph>(
                            geometry->build_chain_graph(settings.chain.crease_angle,
                                                        settings.mode == PreparePickMode::BoundaryChain ? retained_boundary.get() : nullptr,
                                                        check));
                }
                result->graph = chain;
                if (settings_preview && fixed) {
                    PreparePickCandidate seed;
                    seed.local_hit = fixed->local_hit;
                    std::visit(
                        [&](const auto& value) {
                            using V = typename std::decay_t<decltype(value)>::element_type;
                            if constexpr (std::is_same_v<V, const PG::SurfaceRegion>)
                                seed.triangle = value->seed_triangle;
                            else if constexpr (std::is_same_v<V, const PG::EdgeSelection>)
                                seed.edge = value->seed_edge;
                            else
                                seed.vertex = value->vertex;
                        },
                        fixed->geometry);
                    result->candidates = {seed};
                } else
                    result->candidates = prepare_pick_candidates(scene, captured_target_part, *geometry, camera, pixel, settings, check,
                                                                 chain.get());
                if (result->candidates.empty())
                    return;
                result->nearby.placement                 = scene[captured_target_part].placement;
                result->nearby.data.format.vertex_layout = GLModel::Geometry::EVertexLayout::P3;
                result->nearby.data.format.type          = GLModel::Geometry::EPrimitiveType::Points;
                if (settings.mode == PreparePickMode::Vertex)
                    for (const auto& candidate : result->candidates) {
                        result->nearby.data.add_vertex(Vec3f(candidate.local_hit.cast<float>()));
                        result->nearby.data.add_index(unsigned(result->nearby.data.vertices_count() - 1));
                    }
                result->item = selection(captured_target, result->candidates[0], settings, geometry, scene[captured_target_part].placement,
                                         chain, check);
                result->display = overlay(*result->item, camera, true, check);
            },
            [this, result, camera, pixel, ticket, generation, commit_intent] {
                if (generation != context || !store.current(ticket) || !target.mesh.valid() || !camera.same_view(view()) ||
                    (!commit_intent && !session.pending_settings() && pixel != g.m_parent.get_local_mouse_position()))
                    return;
                candidates        = std::move(result->candidates);
                candidate         = 0;
                preview           = result->item;
                graph             = result->graph;
                preview_data      = std::move(result->display);
                preview_upload    = true;
                refresh           = false;
                candidates_data   = std::move(result->nearby);
                candidates_upload = true;
                if (commit_intent) {
                    click.reset();
                    if (preview)
                        commit();
                    else
                        session.clear();
                }
            },
            2);
    }
    void committed_overlay(const PreparePickView& camera)
    {
        auto item = store.get().item;
        if (!item)
            return;
        auto result           = std::make_shared<Overlay>();
        const auto generation = context;
        launch([result, item, camera](const PG::Cancel& check) { *result = overlay(*item, camera, false, check); },
               [this, result, item, camera, generation] {
                   if (generation == context && store.get().item == item && camera.same_view(view())) {
                       hidden_by_clipping = result->data.is_empty();
                       committed_data     = std::move(*result);
                       commit_upload      = true;
                       displayed          = item;
                       overlay_needed     = false;
                   }
               },
               3);
    }
    void draw(GLModel& model, Overlay& data, bool upload, const ColorRGBA& color, float size)
    {
        if (upload) {
            model.reset();
            if (!data.data.is_empty())
                model.init_from(std::move(data.data));
        }
        if (!model.is_initialized())
            return;
        auto* shader = wxGetApp().get_shader("flat");
        if (!shader)
            return;
        auto* previous = wxGetApp().get_current_shader();
        if (previous)
            previous->stop_using();
        shader->start_using();
        const auto& camera = wxGetApp().plater()->get_camera();
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        shader->set_uniform("view_model_matrix", camera.get_view_matrix() * data.placement);
        const bool blend = glIsEnabled(GL_BLEND), cull = glIsEnabled(GL_CULL_FACE), offset = glIsEnabled(GL_POLYGON_OFFSET_FILL),
                   depth = glIsEnabled(GL_DEPTH_TEST);
        GLfloat point, line, offset_factor, offset_units;
        glGetFloatv(GL_POINT_SIZE, &point);
        glGetFloatv(GL_LINE_WIDTH, &line);
        glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &offset_factor);
        glGetFloatv(GL_POLYGON_OFFSET_UNITS, &offset_units);
        GLint src_rgb, dst_rgb, src_alpha, dst_alpha;
        GLint depth_func;
        GLboolean depth_mask;
        glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
        glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_alpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_alpha);
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.f, -1.f);
        glPointSize(size);
        glLineWidth(std::min(2.f, size));
        model.set_color(color);
        model.render();
        glPointSize(point);
        glLineWidth(line);
        if (!blend)
            glDisable(GL_BLEND);
        if (cull)
            glEnable(GL_CULL_FACE);
        if (!offset)
            glDisable(GL_POLYGON_OFFSET_FILL);
        if (!depth)
            glDisable(GL_DEPTH_TEST);
        glDepthFunc(depth_func);
        glDepthMask(depth_mask);
        glPolygonOffset(offset_factor, offset_units);
        glBlendFuncSeparate(src_rgb, dst_rgb, src_alpha, dst_alpha);
        shader->stop_using();
        if (previous)
            previous->start_using();
    }
    void render()
    {
        if (g.m_state == Off && !store.get().item) {
            committed_gl.reset();
            preview_gl.reset();
            candidates_gl.reset();
            displayed.reset();
            return;
        }
        if (!sync())
            return;
        auto camera             = view();
        const auto pixel        = g.m_parent.get_local_mouse_position();
        const bool view_changed = !camera.same_view(last_view);
        if (view_changed || pixel != last_pixel) {
            if (job_kind == 2 && (!click || view_changed))
                cancel();
            if (camera.planes != last_view.planes) {
                if (job_kind == 3)
                    cancel();
                displayed.reset();
            }
            if (view_changed && click && !click_view.same_view(camera)) {
                click.reset();
                store.request();
            }
            preview.reset();
            refresh    = true;
            last_view  = camera;
            last_pixel = pixel;
        }
        auto item = store.get().item;
        if (item != displayed)
            overlay_needed = bool(item);
        if (!busy) {
            if (overlay_needed)
                committed_overlay(camera);
            else if (g.m_state == On && status.empty() && (ready.empty() || scene_dirty))
                prepare();
            else if (g.m_state == On && refresh)
                query(camera, click ? *click : pixel);
        }
        if (item && item == displayed)
            draw(committed_gl, committed_data, commit_upload, ColorRGBA(0.05f, .75f, .85f, .4f), 9.f * g.m_parent.get_scale());
        if (preview && g.m_state == On)
            draw(candidates_gl, candidates_data, candidates_upload, ColorRGBA(.95f, .95f, .95f, 1.f), 3.f * g.m_parent.get_scale());
        if (preview && g.m_state == On)
            draw(preview_gl, preview_data, preview_upload, ColorRGBA(1.f, .5f, .1f, 1.f), 6.f * g.m_parent.get_scale());
        commit_upload     = false;
        preview_upload    = false;
        candidates_upload = false;
    }
    void changed(PreparePickSettings settings)
    {
        try {
            session.edit(std::move(settings));
            click.reset();
            cancel();
            preview.reset();
            graph.reset();
            refresh = true;
            status.clear();
        } catch (const std::exception&) {
            status = _u8L("Enter a finite value within the displayed range.");
        }
        dirty();
    }
    void commit(bool apply = false)
    {
        if (!preview)
            return;
        if (session.accept(preview, apply)) {
            overlay_needed = true;
            displayed.reset();
            refresh = false;
            status.clear();
        }
        dirty();
    }
    void ui(float x, float y, float bottom, bool retained)
    {
        if (retained && !store.get().item)
            return;
        const float scale = g.m_parent.get_scale(), width = g.m_parent.get_canvas_size().get_width();
        ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(300.f * scale, width), 0),
                                            ImVec2(std::max(1.f, width - 16.f * scale), std::max(1.f, bottom - y - 8.f * scale)));
        g.GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always);
        ImGuiWrapper::push_toolbar_style(scale);
        g.GizmoImguiBegin(_u8L("Select geometry"),
                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
        auto selected = store.get().item;
        if (retained) {
            ImGui::TextUnformatted(_u8L("Geometry selection retained").c_str());
            if (g.m_imgui->button(_L("Clear"))) {
                session.clear();
                click.reset();
                gesture.cancel();
                preview.reset();
            }
            ImGui::SameLine();
            if (g.m_imgui->button(_L("Select geometry")))
                g.m_parent.get_gizmos_manager().open_gizmo(GLGizmosManager::GeometrySelection);
        } else {
            auto options = parts();
            if (options.size() > 1) {
                const auto* model = g.m_parent.get_model();
                std::string current;
                for (auto* v : options)
                    if (identity(*v).volume == chosen_part)
                        current = model->objects[v->object_idx()]->volumes[v->volume_idx()]->name;
                if (ImGui::BeginCombo(_u8L("Part").c_str(), current.c_str())) {
                    for (auto* v : options) {
                        auto id   = identity(*v);
                        auto name = model->objects[v->object_idx()]->volumes[v->volume_idx()]->name;
                        if (ImGui::Selectable(name.c_str(), id.volume == chosen_part)) {
                            invalidate();
                            chosen_part = id.volume;
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            auto settings                          = session.settings();
            const std::array<std::string, 5> names = {_u8L("Surface"), _u8L("Mesh edge"), _u8L("Feature chain"), _u8L("Region boundary"),
                                                      _u8L("Vertex")};
            if (ImGui::BeginCombo(_u8L("Selection kind").c_str(), names[size_t(settings.mode)].c_str())) {
                for (size_t i = 0; i < names.size(); ++i) {
                    if (i == 3 && !boundary)
                        continue;
                    if (ImGui::Selectable(names[i].c_str(), size_t(settings.mode) == i)) {
                        settings.mode = PreparePickMode(i);
                        changed(settings);
                    }
                }
                ImGui::EndCombo();
            }
            settings = session.settings();
            if (settings.mode == PreparePickMode::Surface) {
                const std::array<std::string, 3> modes = {_u8L("Triangle"), _u8L("Planar region"), _u8L("Curved region")};
                if (ImGui::BeginCombo(_u8L("Surface mode").c_str(), modes[size_t(settings.surface.mode)].c_str())) {
                    for (size_t i = 0; i < modes.size(); ++i)
                        if (ImGui::Selectable(modes[i].c_str(), size_t(settings.surface.mode) == i)) {
                            settings.surface.mode = PG::SurfaceMode(i);
                            changed(settings);
                        }
                    ImGui::EndCombo();
                }
                settings = session.settings();
                if (settings.surface.mode == PG::SurfaceMode::Planar) {
                    if (ImGui::InputDouble(_u8L("Plane angle (0–45°)").c_str(), &settings.surface.plane_angle))
                        changed(settings);
                    if (ImGui::InputDouble(_u8L("Local plane distance (0–10 mm)").c_str(), &settings.surface.plane_distance))
                        changed(settings);
                } else if (settings.surface.mode == PG::SurfaceMode::Curved) {
                    if (ImGui::InputDouble(_u8L("Neighbor angle (0–180°)").c_str(), &settings.surface.neighbor_angle))
                        changed(settings);
                    bool limit = bool(settings.surface.seed_angle);
                    if (ImGui::Checkbox(_u8L("Limit deviation from seed").c_str(), &limit)) {
                        settings.surface.seed_angle = limit ? std::optional<double>(90.) : std::nullopt;
                        changed(settings);
                    }
                    if (settings.surface.seed_angle &&
                        ImGui::InputDouble(_u8L("Seed angle (0–180°)").c_str(), &*settings.surface.seed_angle))
                        changed(settings);
                }
                if (settings.surface.mode != PG::SurfaceMode::Triangle) {
                    bool limit = bool(settings.surface.distance);
                    if (ImGui::Checkbox(_u8L("Limit distance from pick").c_str(), &limit)) {
                        settings.surface.distance = limit ? std::optional<double>(10.) : std::nullopt;
                        changed(settings);
                    }
                    if (settings.surface.distance &&
                        ImGui::InputDouble(_u8L("Local distance (0.001–10000 mm)").c_str(), &*settings.surface.distance))
                        changed(settings);
                    ImGui::TextWrapped("%s", _u8L("Region membership can include hidden triangles around the back of the surface.").c_str());
                }
            } else {
                if (ImGui::InputDouble(_u8L("Pick radius (2–20 logical pixels)").c_str(), &settings.radius))
                    changed(settings);
                if (settings.mode == PreparePickMode::FeatureChain &&
                    ImGui::InputDouble(_u8L("Crease angle (0–180°)").c_str(), &settings.chain.crease_angle))
                    changed(settings);
                if ((settings.mode == PreparePickMode::FeatureChain || settings.mode == PreparePickMode::BoundaryChain) &&
                    ImGui::InputDouble(_u8L("Chain turn limit (0–180°)").c_str(), &settings.chain.turn_angle))
                    changed(settings);
            }
            if (selected && std::holds_alternative<std::shared_ptr<const PG::SurfaceRegion>>(selected->geometry) &&
                g.m_imgui->button(_L("Pick boundary edge"))) {
                session.cancel_settings();
                boundary      = std::get<std::shared_ptr<const PG::SurfaceRegion>>(selected->geometry);
                settings      = session.settings();
                settings.mode = PreparePickMode::BoundaryChain;
                changed(settings);
            }
            if (g.m_imgui->button(_L("Reset defaults"))) {
                auto defaults         = PreparePickSettings{};
                defaults.mode         = session.settings().mode;
                defaults.surface.mode = session.settings().surface.mode;
                changed(defaults);
            }
            if (session.pending_settings()) {
                ImGui::TextUnformatted(_u8L("Settings preview").c_str());
                g.m_imgui->disabled_begin(!preview || busy);
                if (g.m_imgui->button(_L("Apply")))
                    commit(true);
                g.m_imgui->disabled_end();
                ImGui::SameLine();
                if (g.m_imgui->button(_L("Cancel"))) {
                    session.cancel_settings();
                    cancel();
                    preview.reset();
                    graph.reset();
                    refresh = true;
                }
            }
            if (candidates.size() > 1 && !session.pending_settings() && !busy && g.m_imgui->button(_L("Next candidate"))) {
                candidate                  = (candidate + 1) % candidates.size();
                auto geometry              = ready[target_part].geometry;
                const auto seed            = candidates[candidate];
                const auto settings        = session.settings();
                const auto captured_target = target;
                const auto placement       = ready[target_part].pick.placement;
                const auto camera          = last_view;
                const auto chain           = graph;
                const auto ticket          = store.request();
                auto next                  = std::make_shared<std::pair<std::shared_ptr<const PrepareSelectionItem>, Overlay>>();
                launch(
                    [next, seed, settings, geometry, captured_target, placement, chain, camera](const PG::Cancel& check) {
                        next->first  = selection(captured_target, seed, settings, geometry, placement, chain, check);
                        next->second = overlay(*next->first, camera, true, check);
                    },
                    [this, next, ticket, camera] {
                        if (store.current(ticket) && camera.same_view(view())) {
                            preview        = next->first;
                            preview_data   = std::move(next->second);
                            preview_upload = true;
                            refresh        = false;
                        }
                    },
                    2);
            }
            if (g.m_imgui->button(_L("Clear"))) {
                session.clear();
                cancel();
                click.reset();
                gesture.cancel();
                preview.reset();
                refresh = true;
            }
            ImGui::SameLine();
            if (g.m_imgui->button(_L("Done"))) {
                retain = true;
                click.reset();
                session.done();
                g.m_parent.get_gizmos_manager().reset_all_states();
            }
        }
        if (busy)
            ImGui::TextUnformatted(_u8L("Calculating geometry…").c_str());
        if (!status.empty())
            ImGui::TextWrapped("%s", status.c_str());
        auto summary = [&](const std::shared_ptr<const PrepareSelectionItem>& shown) {
            std::visit(
                [&](const auto& value) {
                    using V = typename std::decay_t<decltype(value)>::element_type;
                    if constexpr (std::is_same_v<V, const PG::SurfaceRegion>) {
                        g.m_imgui->text(
                            format_wxstr(_L_PLURAL("%1% triangle", "%1% triangles", value->triangles.size()), value->triangles.size()));
                        g.m_imgui->text(format_wxstr(_L("Local area: %1% mm²"), value->local.area));
                        if (!value->boundary_complete)
                            ImGui::TextWrapped("%s", _u8L("Boundary topology is incomplete or ambiguous.").c_str());
                    } else if constexpr (std::is_same_v<V, const PG::EdgeSelection>) {
                        g.m_imgui->text(format_wxstr(_L_PLURAL("%1% edge", "%1% edges", value->edges.size()), value->edges.size()));
                        g.m_imgui->text(format_wxstr(_L("Local length: %1% mm"), value->length));
                    } else
                        g.m_imgui->text(format_wxstr(_L("Source vertex index: %1%"), value->vertex));
                },
                shown->geometry);
        };
        if (preview && !retained) {
            ImGui::TextUnformatted(_u8L("Preview (outline)").c_str());
            summary(preview);
        }
        if (selected) {
            ImGui::TextUnformatted(_u8L("Selected").c_str());
            summary(selected);
        }
        if (selected && selected == displayed && hidden_by_clipping)
            ImGui::TextWrapped("%s", _u8L("The selection is retained but hidden by clipping.").c_str());
        g.GizmoImguiEnd();
        ImGuiWrapper::pop_toolbar_style();
    }
};

GLGizmoPrepareSelection::GLGizmoPrepareSelection(GLCanvas3D& parent, unsigned int sprite)
    : GLGizmoBase(parent, "toolbar_select_geometry.svg", sprite), m_impl(std::make_unique<Impl>(*this))
{}
GLGizmoPrepareSelection::~GLGizmoPrepareSelection() = default;
bool GLGizmoPrepareSelection::on_init()
{
    m_impl->worker = std::make_unique<BoostThreadWorker>(nullptr, "prepare_geometry");
    return true;
}
std::string GLGizmoPrepareSelection::on_get_name() const { return _u8L("Select geometry"); }
bool GLGizmoPrepareSelection::on_is_selectable() const { return m_parent.get_canvas_type() == GLCanvas3D::CanvasView3D; }
bool GLGizmoPrepareSelection::on_is_activable() const { return on_is_selectable() && !m_impl->parts().empty(); }
void GLGizmoPrepareSelection::on_set_state()
{
    if (m_state == On) {
        m_impl->session.open();
        m_impl->refresh = true;
        m_impl->retain  = false;
    } else {
        m_impl->cancel();
        m_impl->session.done();
        if (!m_impl->retain)
            m_impl->invalidate();
        m_impl->retain = false;
    }
}
void GLGizmoPrepareSelection::invalidate() { m_impl->invalidate(); }
void GLGizmoPrepareSelection::cancel_gesture()
{
    m_impl->gesture.cancel();
    m_impl->click.reset();
    m_impl->cancel();
}
void GLGizmoPrepareSelection::data_changed(bool serializing)
{
    if (serializing)
        invalidate();
    else
        m_impl->sync();
}
void GLGizmoPrepareSelection::tick() { m_impl->tick(); }
bool GLGizmoPrepareSelection::escape()
{
    m_impl->cancel();
    m_impl->click.reset();
    m_impl->gesture.cancel();
    m_impl->preview.reset();
    m_impl->refresh = true;
    return m_impl->session.escape();
}
void GLGizmoPrepareSelection::on_render() { m_impl->render(); }
void GLGizmoPrepareSelection::on_render_input_window(float x, float y, float bottom) { m_impl->ui(x, y, bottom, false); }
void GLGizmoPrepareSelection::render_retained_indicator()
{
    if (m_state == Off)
        m_impl->ui(12.f * m_parent.get_scale(), 60.f * m_parent.get_scale(), float(m_parent.get_canvas_size().get_height()), true);
}
bool GLGizmoPrepareSelection::on_mouse(const wxMouseEvent& event)
{
    auto& p = *m_impl;
    const Vec2d pixel(event.GetX(), event.GetY());
    if (event.Leaving()) {
        cancel_gesture();
        return false;
    }
    if (event.RightDown() || event.MiddleDown() || event.HasAnyModifiers()) {
        cancel_gesture();
        return false;
    }
    if ((event.LeftDown() || event.LeftDClick()) && !event.HasAnyModifiers()) {
        if (p.session.pending_settings()) {
            p.session.cancel_settings();
            p.preview.reset();
            p.graph.reset();
            p.refresh = true;
        }
        p.click.reset();
        if (p.job_kind == 2)
            p.cancel();
        p.store.request();
        p.gesture.begin(pixel, m_parent.get_scale());
        return true;
    }
    if (event.Dragging()) {
        p.gesture.move(pixel);
        return false;
    }
    if (event.LeftUp() && p.gesture.pressed()) {
        p.gesture.move(pixel);
        if (!p.gesture.release())
            return false;
        if (p.preview && !p.busy && p.last_view.same_view(p.view()) && pixel == p.last_pixel)
            p.commit();
        else {
            p.click      = pixel;
            p.click_view = p.view();
            p.refresh    = true;
            if (p.job_kind == 2)
                p.cancel();
            p.dirty();
        }
        return true;
    }
    return false;
}
} // namespace Slic3r::GUI
