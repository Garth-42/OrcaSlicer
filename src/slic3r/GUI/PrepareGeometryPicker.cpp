#include "PrepareGeometryPicker.hpp"
#include "PrepareSelection.hpp"
#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrepareGeometry.hpp"
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <igl/Hit.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Slic3r::GUI {
namespace {
void check(const PrepareGeometry::Cancel& cancel, size_t i = 0)
{
    if (cancel && i % 256 == 0)
        cancel();
}
bool eligible(const Vec3d& p, const PreparePickView& view)
{
    for (const auto& plane : view.planes)
        if (plane.head<3>().dot(p) + plane.w() < 0.)
            return false;
    return true;
}
std::optional<Vec3d> project(const Vec3d& p, const PreparePickView& view)
{
    Vec4d clip = view.projection_view * Vec4d(p.x(), p.y(), p.z(), 1.);
    if (!clip.allFinite() || clip.w() <= 0.)
        return {};
    Vec3d ndc = clip.head<3>() / clip.w();
    if (ndc.z() < -1. - 1e-10 || ndc.z() > 1. + 1e-10)
        return {};
    return Vec3d(view.viewport[0] + (ndc.x() + 1.) * 0.5 * view.viewport[2], view.viewport[1] + (1. - ndc.y()) * 0.5 * view.viewport[3],
                 ndc.z());
}
struct Hit
{
    size_t part, triangle;
    Vec3d local;
    double distance;
};
// Trim an original edge to the visible portion before finding its closest
// screen point. The returned parameters still refer to that same source edge.
bool clip_edge(Vec3d& a, Vec3d& b, const Transform3d& placement, const PreparePickView& view)
{
    double begin = 0., end = 1.;
    auto trim = [&](double da, double db) {
        if (da < 0. && db < 0.)
            return false;
        if (da < 0.)
            begin = std::max(begin, da / (da - db));
        if (db < 0.)
            end = std::min(end, da / (da - db));
        return begin <= end;
    };
    const Vec3d wa = placement * a, wb = placement * b;
    for (const auto& plane : view.planes)
        if (!trim(plane.head<3>().dot(wa) + plane.w(), plane.head<3>().dot(wb) + plane.w()))
            return false;
    const Vec4d ca = view.projection_view * wa.homogeneous(), cb = view.projection_view * wb.homogeneous();
    if (!trim(ca.w() + ca.z(), cb.w() + cb.z()) || !trim(ca.w() - ca.z(), cb.w() - cb.z()))
        return false;
    const Vec3d start = a, delta = b - a;
    a = start + begin * delta;
    b = start + end * delta;
    return true;
}
std::optional<Hit> nearest(const std::vector<PreparePickPart>& scene,
                           const PreparePickView& view,
                           const Matrix4d& inverse_view,
                           const Vec2d& pixel,
                           const PrepareGeometry::Cancel& cancel)
{
    const double x = 2. * (pixel.x() - view.viewport[0]) / view.viewport[2] - 1.;
    const double y = 1. - 2. * (pixel.y() - view.viewport[1]) / view.viewport[3];
    Vec4d near = inverse_view * Vec4d(x, y, -1., 1.), far = inverse_view * Vec4d(x, y, 1., 1.);
    if (near.w() == 0. || far.w() == 0.)
        return {};
    Vec3d origin = near.head<3>() / near.w(), direction = (far.head<3>() / far.w() - origin).normalized();
    std::optional<Hit> best;
    for (size_t i = 0; i < scene.size(); ++i) {
        check(cancel);
        const auto& part = scene[i];
        if (!part.mesh || !part.placement.matrix().allFinite() || part.placement.linear().determinant() == 0.)
            continue;
        Transform3d inverse = part.placement.inverse();
        Vec3d local_origin = inverse * origin, local_direction = inverse.linear() * direction;
        std::vector<igl::Hit<float>> hits;
        AABBTreeIndirect::intersect_ray_all_hits(part.mesh->mesh->vertices, part.mesh->mesh->indices, part.mesh->tree, local_origin,
                                                 local_direction, hits, part.mesh->ray_epsilon);
        for (size_t h = 0; h < hits.size(); ++h) {
            check(cancel, h);
            const auto& hit = hits[h];
            if (!std::isfinite(hit.t) || hit.t < 0. || hit.id < 0)
                continue;
            const auto& triangle = part.mesh->mesh->indices[size_t(hit.id)];
            const auto& vertices = part.mesh->mesh->vertices;
            const Vec3d a        = vertices[triangle[0]].cast<double>();
            const Vec3d normal   = (vertices[triangle[1]].cast<double>() - a).cross(vertices[triangle[2]].cast<double>() - a);
            // Local sidedness matches the renderer's mirrored front-face correction.
            if (!part.two_sided && normal.dot(local_direction) >= 0.)
                continue;
            Vec3d local = local_origin + double(hit.t) * local_direction, world = part.placement * local;
            if (!eligible(world, view))
                continue;
            auto projected = project(world, view);
            if (!projected)
                continue;
            double distance = (world - origin).dot(direction);
            if (!best || distance < best->distance ||
                (distance == best->distance && std::make_pair(i, size_t(hit.id)) < std::make_pair(best->part, best->triangle)))
                best = Hit{i, size_t(hit.id), local, distance};
        }
    }
    return best;
}
} // namespace

PreparePickMesh::PreparePickMesh(std::shared_ptr<const indexed_triangle_set> source, const PrepareGeometry::Cancel& cancel)
    : mesh(std::move(source))
{
    check(cancel);
    if (!mesh)
        throw std::invalid_argument("Missing pick mesh");
    double scale = 1.;
    for (size_t i = 0; i < mesh->vertices.size(); ++i) {
        check(cancel, i);
        if (!mesh->vertices[i].allFinite())
            throw std::invalid_argument("Non-finite pick vertex");
        scale = std::max(scale, double(mesh->vertices[i].cwiseAbs().maxCoeff()));
    }
    double total = 0.;
    for (size_t i = 0; i < mesh->indices.size(); ++i) {
        check(cancel, i);
        const auto& t = mesh->indices[i];
        for (int v : t)
            if (v < 0 || size_t(v) >= mesh->vertices.size())
                throw std::invalid_argument("Invalid pick mesh index");
        for (int e = 0; e < 3; ++e)
            total += (mesh->vertices[t[e]].cast<double>() - mesh->vertices[t[(e + 1) % 3]].cast<double>()).norm();
    }
    if (!mesh->indices.empty()) {
        double average = total / (3. * mesh->indices.size());
        if (average > 0.)
            ray_epsilon *= average * average;
    }
    // Pad only BVH slabs: an axis-aligned ray exactly on a source vertex must
    // not encounter 0*infinity in a zero-width slab. Triangle tests stay exact.
    tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(mesh->vertices, mesh->indices,
                                                                       float(8. * std::numeric_limits<float>::epsilon() * scale));
    check(cancel);
}
bool PreparePickView::same_view(const PreparePickView& other) const
{
    if (projection_view != other.projection_view || viewport != other.viewport || pixel_scale != other.pixel_scale ||
        planes.size() != other.planes.size())
        return false;
    for (size_t i = 0; i < planes.size(); ++i)
        if (planes[i] != other.planes[i])
            return false;
    return true;
}
PreparePickMesh::PreparePickMesh(std::shared_ptr<const PrepareGeometry::MeshGeometry> analysis, const PrepareGeometry::Cancel& cancel)
    : PreparePickMesh(analysis->mesh(), cancel)
{
    edges = analysis->source_edges();
    check(cancel);
}
std::vector<PreparePickCandidate> prepare_pick_candidates(const std::vector<PreparePickPart>& scene,
                                                          size_t target,
                                                          const PrepareGeometry::MeshGeometry& geometry,
                                                          const PreparePickView& view,
                                                          const Vec2d& pixel,
                                                          const PreparePickSettings& settings,
                                                          const PrepareGeometry::Cancel& cancel,
                                                          const PrepareGeometry::ChainGraph* chain_graph)
{
    settings.validate();
    check(cancel);
    if (target >= scene.size() || !scene[target].mesh || scene[target].mesh->mesh != geometry.mesh())
        throw std::invalid_argument("Pick target and analysis snapshot differ");
    if (!view.projection_view.allFinite() || view.projection_view.determinant() == 0. || view.viewport[2] <= 0 || view.viewport[3] <= 0 ||
        !std::isfinite(view.pixel_scale) || view.pixel_scale <= 0. || !pixel.allFinite())
        throw std::invalid_argument("Invalid pick view");
    const Matrix4d inverse = view.projection_view.inverse();
    const auto& part       = scene[target];
    if (!part.placement.matrix().allFinite() || part.placement.linear().determinant() == 0.)
        throw std::invalid_argument("Singular pick placement");
    if (settings.mode == PreparePickMode::Surface) {
        auto hit = nearest(scene, view, inverse, pixel, cancel);
        if (!hit || hit->part != target)
            return {};
        return {{hit->triangle, {}, {}, hit->local, 0., hit->distance}};
    }
    const double radius = settings.radius * view.pixel_scale;
    std::vector<PreparePickCandidate> candidates;
    const auto& source = *geometry.mesh();
    if (settings.mode == PreparePickMode::Vertex) {
        for (size_t i = 0; i < source.vertices.size(); ++i) {
            check(cancel, i);
            Vec3d local = source.vertices[i].cast<double>(), world = part.placement * local;
            auto screen = project(world, view);
            if (!screen || !eligible(world, view))
                continue;
            double distance = (screen->head<2>() - pixel).norm();
            if (distance <= radius)
                candidates.push_back({0, i, {}, local, distance, screen->z()});
        }
    } else {
        if ((settings.mode == PreparePickMode::FeatureChain || settings.mode == PreparePickMode::BoundaryChain) && !chain_graph)
            throw std::invalid_argument("Missing feature candidate graph");
        const auto& edges = part.mesh->edges;
        for (size_t i = 0; i < edges.size(); ++i) {
            check(cancel, i);
            const auto& edge = edges[i];
            if (chain_graph && !chain_graph->contains(edge))
                continue;
            Vec3d a = source.vertices[edge[0]].cast<double>(), b = source.vertices[edge[1]].cast<double>();
            if (!clip_edge(a, b, part.placement, view))
                continue;
            auto pa = project(part.placement * a, view), pb = project(part.placement * b, view);
            if (!pa || !pb)
                continue;
            Vec2d delta = pb->head<2>() - pa->head<2>();
            double t    = delta.squaredNorm() > 0. ? std::clamp((pixel - pa->head<2>()).dot(delta) / delta.squaredNorm(), 0., 1.) : 0.;
            // Interpolate in homogeneous coordinates so perspective snapping returns
            // an actual closest projected source segment point, not an affine guess.
            double wa   = (view.projection_view * (part.placement * a).homogeneous()).w();
            double wb   = (view.projection_view * (part.placement * b).homogeneous()).w();
            double u    = t * wa / ((1. - t) * wb + t * wa);
            Vec3d local = (1. - u) * a + u * b, world = part.placement * local;
            auto p = project(world, view);
            if (!p || !eligible(world, view))
                continue;
            double distance = (p->head<2>() - pixel).norm();
            if (distance <= radius)
                candidates.push_back({0, {}, edge, local, distance, p->z()});
        }
    }
    size_t comparisons = 0;
    std::sort(candidates.begin(), candidates.end(), [&](const auto& a, const auto& b) {
        check(cancel, comparisons++);
        if (a.screen_distance != b.screen_distance)
            return a.screen_distance < b.screen_distance;
        if (a.depth != b.depth)
            return a.depth < b.depth;
        return a.vertex ? a.vertex < b.vertex : a.edge < b.edge;
    });
    std::vector<PreparePickCandidate> visible;
    for (size_t i = 0; i < candidates.size(); ++i) {
        check(cancel, i);
        auto candidate    = candidates[i];
        const Vec3d world = part.placement * candidate.local_hit;
        auto screen       = project(world, view);
        auto hit          = nearest(scene, view, inverse, screen->head<2>(), cancel);
        if (!hit || hit->part != target)
            continue;
        const double error = (part.placement * hit->local - world).norm();
        if (error > 8. * std::numeric_limits<float>::epsilon() * std::max(1., hit->distance))
            continue;
        candidate.triangle = hit->triangle;
        visible.push_back(std::move(candidate));
    }
    check(cancel);
    return visible;
}
} // namespace Slic3r::GUI
