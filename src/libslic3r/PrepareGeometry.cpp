#include "PrepareGeometry.hpp"

#include "Point.hpp"
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <boost/geometry/algorithms/intersects.hpp>
#include <boost/geometry/algorithms/area.hpp>
#include <boost/geometry/algorithms/is_valid.hpp>
#include <boost/geometry/geometries/linestring.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/ring.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Slic3r::PrepareGeometry {
namespace {
// Angular comparisons allow 1e-10 degrees, distance comparisons 1e-9 local mm.
// Neither epsilon is a model-scale-dependent substitute for the user's tolerance.
constexpr double angle_epsilon = 1e-10;
constexpr double distance_epsilon = 1e-9;
constexpr double pi = 3.14159265358979323846;

void checkpoint(const Cancel& cancel, size_t iteration = 0)
{
    if (cancel && iteration % 1024 == 0) cancel();
}

void range(double value, double low, double high)
{
    if (!std::isfinite(value) || value < low || value > high)
        throw std::invalid_argument("Geometry selection setting is outside its supported range");
}

double angle(const Vec3d& a, const Vec3d& b)
{
    return std::atan2(a.cross(b).norm(), a.dot(b)) * 180. / pi;
}

EdgeKey canonical(EdgeKey key)
{
    if (key[0] > key[1]) std::swap(key[0], key[1]);
    return key;
}

void canonicalize(Chain& chain)
{
    if (chain.closed) {
        auto first = std::min_element(chain.vertices.begin(), chain.vertices.end());
        std::rotate(chain.vertices.begin(), first, chain.vertices.end());
        if (chain.vertices.size() > 2 && chain.vertices[1] > chain.vertices.back())
            std::reverse(chain.vertices.begin() + 1, chain.vertices.end());
    } else if (std::lexicographical_compare(chain.vertices.rbegin(), chain.vertices.rend(),
                                            chain.vertices.begin(), chain.vertices.end()))
        std::reverse(chain.vertices.begin(), chain.vertices.end());
}

using Graph = std::vector<std::vector<size_t>>;
Graph graph(const std::vector<EdgeKey>& edges, size_t vertices, const Cancel& cancel,
            const std::vector<EdgeKind>* kinds = nullptr)
{
    Graph result(vertices);
    for (size_t i = 0; i < edges.size(); ++i) {
        checkpoint(cancel, i);
        if (kinds && (*kinds)[i] != EdgeKind::Boundary && (*kinds)[i] != EdgeKind::Manifold) continue;
        result[edges[i][0]].push_back(i);
        result[edges[i][1]].push_back(i);
    }
    return result;
}

// Walk both ways in the ORIGINAL eligible graph. Used-edge removal must not turn a
// junction into a degree-two vertex and manufacture a branch on subsequent walks.
Chain trace(size_t seed, const std::vector<EdgeKey>& edges, const Graph& adjacency,
            const indexed_triangle_set& mesh, double turn_limit, const Cancel& cancel)
{
    Chain result;
    std::vector<int> left, right;
    const auto walk = [&](int from, int at, std::vector<int>& out) {
        size_t previous = seed;
        for (size_t step = 0; step < edges.size(); ++step) {
            checkpoint(cancel, step);
            if (adjacency[at].size() != 2) return false;
            size_t next = adjacency[at][0] == previous ? adjacency[at][1] : adjacency[at][0];
            int to = edges[next][0] == at ? edges[next][1] : edges[next][0];
            Vec3d incoming = mesh.vertices[at].cast<double>() - mesh.vertices[from].cast<double>();
            Vec3d outgoing = mesh.vertices[to].cast<double>() - mesh.vertices[at].cast<double>();
            if (incoming.squaredNorm() == 0. || outgoing.squaredNorm() == 0. ||
                angle(incoming, outgoing) > turn_limit + angle_epsilon) return false;
            if (next == seed) return true;
            out.push_back(to);
            previous = next;
            from = at;
            at = to;
        }
        return false;
    };
    const int a = edges[seed][0], b = edges[seed][1];
    result.closed = walk(a, b, right);
    if (result.closed) {
        result.vertices = {a, b};
        // The last appended vertex is the starting vertex; closure is stored separately.
        if (!right.empty() && right.back() == a) right.pop_back();
        result.vertices.insert(result.vertices.end(), right.begin(), right.end());
    } else {
        walk(b, a, left);
        result.vertices.assign(left.rbegin(), left.rend());
        result.vertices.push_back(a);
        result.vertices.push_back(b);
        result.vertices.insert(result.vertices.end(), right.begin(), right.end());
    }
    canonicalize(result);
    return result;
}

void valid_transform(const Transform3d& transform)
{
    if (!transform.matrix().allFinite() || transform.linear().determinant() == 0. ||
        !std::isfinite(transform.linear().determinant()) ||
        transform.matrix().row(3) != Eigen::RowVector4d(0., 0., 0., 1.))
        throw std::invalid_argument("Geometry selection requires an invertible affine transform");
}

template<class KindAt>
EdgeSelection edge_result(const std::shared_ptr<const indexed_triangle_set>& mesh, const Chain& chain,
                           EdgeKey seed, const Cancel& cancel, KindAt kind_at)
{
    EdgeSelection result;
    result.mesh = mesh;
    result.seed_edge = canonical(seed);
    result.chain = chain;
    const size_t segments = chain.closed ? chain.vertices.size() : chain.vertices.size() - 1;
    for (size_t i = 0; i < segments; ++i) {
        checkpoint(cancel, i);
        const int a = chain.vertices[i], b = chain.vertices[(i + 1) % chain.vertices.size()];
        EdgeKey key = canonical({a, b});
        result.edges.push_back(key);
        result.kinds.push_back(kind_at(key));
        result.length += (mesh->vertices[a].cast<double>() - mesh->vertices[b].cast<double>()).norm();
    }
    return result;
}
} // namespace

void SurfaceSettings::validate() const
{
    switch (mode) {
    case SurfaceMode::Triangle: case SurfaceMode::Planar: case SurfaceMode::Curved: break;
    default: throw std::invalid_argument("Unknown surface selection mode");
    }
    range(plane_angle, 0., 45.);
    range(plane_distance, 0., 10.);
    range(neighbor_angle, 0., 180.);
    if (seed_angle) range(*seed_angle, 0., 180.);
    if (distance) range(*distance, 0.001, 10000.);
}

void ChainSettings::validate() const
{
    range(crease_angle, 0., 180.);
    range(turn_angle, 0., 180.);
}

MeshGeometry::MeshGeometry(const indexed_triangle_set& source, const Cancel& cancel)
{
    checkpoint(cancel);
    // Copy incrementally so capture can be cancelled, and no mutable alias escapes.
    auto mesh = std::make_shared<indexed_triangle_set>();
    mesh->vertices.reserve(source.vertices.size());
    for (size_t i = 0; i < source.vertices.size(); ++i) {
        checkpoint(cancel, i);
        if (!source.vertices[i].allFinite()) throw std::invalid_argument("Non-finite source vertex");
        mesh->vertices.push_back(source.vertices[i]);
    }
    mesh->indices.reserve(source.indices.size());
    struct Record { EdgeKey key; Incidence incidence; };
    std::vector<Record> records;
    if (source.indices.size() > records.max_size() / 3) throw std::length_error("Source mesh is too large");
    records.reserve(3 * source.indices.size());
    m_normals.reserve(source.indices.size());
    m_areas.reserve(source.indices.size());
    m_face_edges.resize(source.indices.size());
    for (size_t f = 0; f < source.indices.size(); ++f) {
        checkpoint(cancel, f);
        const auto& triangle = source.indices[f];
        for (int v : triangle)
            if (v < 0 || size_t(v) >= mesh->vertices.size()) throw std::invalid_argument("Invalid source triangle index");
        mesh->indices.push_back(triangle);
        const Vec3d a = mesh->vertices[triangle[0]].cast<double>();
        Vec3d normal = (mesh->vertices[triangle[1]].cast<double>() - a).cross(
                       mesh->vertices[triangle[2]].cast<double>() - a);
        const double twice_area = normal.norm();
        m_areas.push_back(twice_area * 0.5);
        // Exact collinearity is degenerate. Do not discard small, otherwise valid source triangles.
        if (twice_area > 0.) normal /= twice_area;
        m_normals.push_back(normal);
        for (unsigned char s = 0; s < 3; ++s) {
            const int from = triangle[s], to = triangle[(s + 1) % 3];
            records.push_back({canonical({from, to}), {f, s, from < to}});
        }
    }
    size_t comparisons = 0;
    std::sort(records.begin(), records.end(), [&](const Record& a, const Record& b) {
        checkpoint(cancel, comparisons++);
        if (a.key != b.key) return a.key < b.key;
        return a.incidence.triangle != b.incidence.triangle ? a.incidence.triangle < b.incidence.triangle :
                                                            a.incidence.slot < b.incidence.slot;
    });
    m_incidence.reserve(records.size());
    for (size_t begin = 0; begin < records.size();) {
        checkpoint(cancel, begin);
        size_t end = begin + 1;
        while (end < records.size() && records[end].key == records[begin].key) { checkpoint(cancel, end); ++end; }
        EdgeKind kind = end - begin == 1 ? EdgeKind::Boundary : EdgeKind::Manifold;
        if (end - begin > 2) kind = EdgeKind::Nonmanifold;
        else if (end - begin == 2 && records[begin].incidence.forward == records[begin + 1].incidence.forward)
            kind = EdgeKind::InconsistentWinding;
        for (size_t i = begin; i < end; ++i) {
            const auto& incidence = records[i].incidence;
            if (m_areas[incidence.triangle] == 0. && kind != EdgeKind::Nonmanifold) kind = EdgeKind::Degenerate;
            m_face_edges[incidence.triangle][incidence.slot] = m_edges.size();
            m_incidence.push_back(incidence);
        }
        m_edges.push_back({records[begin].key, begin, end, kind});
        begin = end;
    }
    m_mesh = std::move(mesh);
    checkpoint(cancel);
}

size_t MeshGeometry::find_edge(EdgeKey key) const
{
    key = canonical(key);
    auto it = std::lower_bound(m_edges.begin(), m_edges.end(), key,
                               [](const Edge& edge, const EdgeKey& value) { return edge.key < value; });
    if (it == m_edges.end() || it->key != key) throw std::out_of_range("Source edge does not exist");
    return size_t(it - m_edges.begin());
}

std::vector<EdgeKey> MeshGeometry::source_edges() const
{
    std::vector<EdgeKey> result;
    result.reserve(m_edges.size());
    for (const auto& edge : m_edges) result.push_back(edge.key);
    return result;
}

bool MeshGeometry::on_seed_plane(size_t face, size_t seed, const SurfaceSettings& settings) const
{
    if (angle(m_normals[seed], m_normals[face]) > settings.plane_angle + angle_epsilon) return false;
    const Vec3d origin = m_mesh->vertices[m_mesh->indices[seed][0]].cast<double>();
    for (int v : m_mesh->indices[face])
        if (std::abs(m_normals[seed].dot(m_mesh->vertices[v].cast<double>() - origin)) >
            settings.plane_distance + distance_epsilon) return false;
    return true;
}

SurfaceRegion MeshGeometry::select_surface(size_t seed, const Vec3d& hit, const SurfaceSettings& settings,
                                           const Cancel& cancel) const
{
    settings.validate();
    checkpoint(cancel);
    if (seed >= m_areas.size()) throw std::out_of_range("Source triangle does not exist");
    if (m_areas[seed] == 0.) throw std::invalid_argument("Degenerate triangle cannot seed a region");
    if (!hit.allFinite()) throw std::invalid_argument("Non-finite seed hit");
    // The caller supplies a ray/snap hit; reject an off-triangle point rather than
    // recording a supposed on-mesh seed that was never on the source geometry.
    const auto& t = m_mesh->indices[seed];
    const Vec3d a = m_mesh->vertices[t[0]].cast<double>();
    const Vec3d ab = m_mesh->vertices[t[1]].cast<double>() - a;
    const Vec3d ac = m_mesh->vertices[t[2]].cast<double>() - a;
    const Vec3d ap = hit - a;
    const Vec3d cross = ab.cross(ac);
    double u = ap.cross(ac).dot(cross) / cross.squaredNorm();
    double v = ab.cross(ap).dot(cross) / cross.squaredNorm();
    // Raycasters may return float hit coordinates. Accept their rounding slack,
    // then reconstruct an on-triangle point; detection tolerances remain separate.
    constexpr double hit_slack = 8. * std::numeric_limits<float>::epsilon();
    const double hit_distance = hit_slack * std::max({a.norm(), ab.norm(), ac.norm(), 1.});
    if (std::abs(ap.dot(m_normals[seed])) > hit_distance || u < -hit_slack || v < -hit_slack || u + v > 1. + hit_slack)
        throw std::invalid_argument("Seed hit is outside its source triangle");
    u = std::clamp(u, 0., 1.);
    v = std::clamp(v, 0., 1. - u);
    SurfaceRegion result;
    result.mesh = m_mesh;
    result.settings = settings;
    result.seed_triangle = seed;
    result.seed_hit = a + u * ab + v * ac;
    std::vector<unsigned char> included(m_areas.size(), 0);
    std::vector<size_t> queue{seed};
    included[seed] = 1;
    for (size_t i = 0; i < queue.size() && settings.mode != SurfaceMode::Triangle; ++i) {
        checkpoint(cancel, i);
        const size_t current = queue[i];
        for (size_t edge_id : m_face_edges[current]) {
            const auto& edge = m_edges[edge_id];
            if (edge.kind != EdgeKind::Manifold) continue;
            const size_t next = m_incidence[edge.begin].triangle == current ? m_incidence[edge.begin + 1].triangle :
                                                                           m_incidence[edge.begin].triangle;
            if (included[next]) continue;
            if (settings.mode == SurfaceMode::Planar && !on_seed_plane(next, seed, settings)) continue;
            if (settings.mode == SurfaceMode::Curved) {
                if (angle(m_normals[current], m_normals[next]) > settings.neighbor_angle + angle_epsilon) continue;
                if (settings.seed_angle && angle(m_normals[seed], m_normals[next]) > *settings.seed_angle + angle_epsilon) continue;
            }
            if (settings.distance) {
                bool within = true;
                for (int vertex : m_mesh->indices[next])
                    if ((m_mesh->vertices[vertex].cast<double>() - result.seed_hit).norm() > *settings.distance + distance_epsilon)
                        within = false;
                if (!within) continue;
            }
            included[next] = 1; // Never mark failed adjacency globally: another path may pass.
            queue.push_back(next);
        }
    }
    result.is_planar = true;
    for (size_t f = 0; f < included.size(); ++f) {
        checkpoint(cancel, f);
        if (included[f]) {
            result.triangles.push_back(f);
            if (!on_seed_plane(f, seed, settings)) result.is_planar = false;
        }
    }
    result.local = measure_surface(result, Transform3d::Identity(), cancel);
    extract_boundaries(result, cancel);
    checkpoint(cancel);
    return result;
}

SurfaceMetrics MeshGeometry::measure_surface(const SurfaceRegion& region, const Transform3d& transform,
                                             const Cancel& cancel) const
{
    if (region.mesh != m_mesh) throw std::invalid_argument("Region belongs to another mesh snapshot");
    valid_transform(transform);
    if (region.seed_triangle >= m_areas.size()) throw std::out_of_range("Invalid seed triangle");
    SurfaceMetrics result;
    Vec3d area_normal = Vec3d::Zero();
    for (size_t i = 0; i < region.triangles.size(); ++i) {
        checkpoint(cancel, i);
        const size_t f = region.triangles[i];
        if (f >= m_areas.size()) throw std::out_of_range("Invalid region triangle");
        const auto& t = m_mesh->indices[f];
        const Vec3d a = transform * m_mesh->vertices[t[0]].cast<double>();
        const Vec3d b = transform * m_mesh->vertices[t[1]].cast<double>();
        const Vec3d c = transform * m_mesh->vertices[t[2]].cast<double>();
        const Vec3d normal = (b - a).cross(c - a);
        const double area = normal.norm() * 0.5;
        if (!std::isfinite(area) || !normal.allFinite()) throw std::invalid_argument("Non-finite transformed geometry");
        result.area += area;
        if (result.area > 0.) result.centroid += (area / result.area) * ((a / 3. + b / 3. + c / 3.) - result.centroid);
        area_normal += normal;
        if (f == region.seed_triangle && area > 0.) result.seed_normal = normal / (2. * area);
    }
    if (!std::isfinite(result.area) || !result.centroid.allFinite() || !area_normal.allFinite())
        throw std::invalid_argument("Non-finite transformed measurements");
    // Relative to total area: a closed shell's cancelling sum has no universal normal.
    if (area_normal.norm() > 2. * result.area * 1e-12) result.mean_normal = area_normal.normalized();
    checkpoint(cancel);
    return result;
}

void MeshGeometry::extract_boundaries(SurfaceRegion& region, const Cancel& cancel) const
{
    std::vector<unsigned char> selected(m_areas.size(), 0);
    for (size_t i = 0; i < region.triangles.size(); ++i) { checkpoint(cancel, i); selected[region.triangles[i]] = 1; }
    std::vector<EdgeKey> edges;
    std::vector<bool> forward;
    for (size_t id = 0; id < m_edges.size(); ++id) {
        checkpoint(cancel, id);
        const auto& edge = m_edges[id];
        size_t count = 0;
        bool direction = false;
        for (size_t i = edge.begin; i < edge.end; ++i) {
            checkpoint(cancel, i);
            if (selected[m_incidence[i].triangle]) { ++count; direction = m_incidence[i].forward; }
        }
        if (!count) continue;
        switch (edge.kind) {
        case EdgeKind::Boundary: region.diagnostics.open_mesh = true; break;
        case EdgeKind::Nonmanifold: region.diagnostics.nonmanifold = true; region.boundary_complete = false; break;
        case EdgeKind::InconsistentWinding: region.diagnostics.inconsistent_winding = true; region.boundary_complete = false; break;
        case EdgeKind::Degenerate: region.diagnostics.degenerate = true; region.boundary_complete = false; break;
        case EdgeKind::Manifold: break;
        }
        if (count == 1) { edges.push_back(edge.key); forward.push_back(direction); }
        else if (edge.kind != EdgeKind::Manifold) region.diagnostics.ambiguous_boundary = true;
    }
    auto adjacency = graph(edges, m_mesh->vertices.size(), cancel);
    for (size_t v = 0; v < adjacency.size(); ++v) {
        checkpoint(cancel, v);
        if (!adjacency[v].empty() && adjacency[v].size() != 2) {
            region.diagnostics.ambiguous_boundary = true;
            region.boundary_complete = false;
        }
    }
    std::vector<unsigned char> visited(edges.size(), 0);
    // Start at endpoints/junctions before cycles, so each ambiguous branch appears once.
    std::vector<size_t> order;
    for (size_t i = 0; i < edges.size(); ++i) {
        checkpoint(cancel, i);
        if (adjacency[edges[i][0]].size() != 2 || adjacency[edges[i][1]].size() != 2) order.push_back(i);
    }
    for (size_t i = 0; i < edges.size(); ++i) { checkpoint(cancel, i); order.push_back(i); }
    for (size_t id : order) {
        checkpoint(cancel, id);
        if (visited[id]) continue;
        auto chain = trace(id, edges, adjacency, *m_mesh, 180., cancel);
        const size_t segments = chain.closed ? chain.vertices.size() : chain.vertices.size() - 1;
        for (size_t i = 0; i < segments; ++i) {
            checkpoint(cancel, i);
            auto key = canonical({chain.vertices[i], chain.vertices[(i + 1) % chain.vertices.size()]});
            auto edge = std::lower_bound(edges.begin(), edges.end(), key);
            visited[size_t(edge - edges.begin())] = 1;
        }
        // Orient to source winding using the first source edge, then rotate a closed
        // loop to its smallest source vertex without changing its direction.
        EdgeKey first = canonical({chain.vertices[0], chain.vertices[1]});
        size_t edge_id = size_t(std::lower_bound(edges.begin(), edges.end(), first) - edges.begin());
        if ((chain.vertices[0] < chain.vertices[1]) != forward[edge_id])
            std::reverse(chain.vertices.begin(), chain.vertices.end());
        if (chain.closed)
            std::rotate(chain.vertices.begin(), std::min_element(chain.vertices.begin(), chain.vertices.end()), chain.vertices.end());
        region.boundaries.push_back(std::move(chain));
    }
    if (region.is_planar && region.boundary_complete) {
        // Validate projected boundaries before assigning outer/inner labels. A
        // topologically closed source chain may still cross or overlap itself.
        namespace bg = boost::geometry;
        using LineString = bg::model::linestring<bg::model::d2::point_xy<double>>;
        std::vector<LineString> loops;
        const Vec3d origin = m_mesh->vertices[m_mesh->indices[region.seed_triangle][0]].cast<double>();
        const Vec3d u_axis = m_normals[region.seed_triangle].unitOrthogonal();
        const Vec3d v_axis = m_normals[region.seed_triangle].cross(u_axis);
        for (const auto& chain : region.boundaries) {
            checkpoint(cancel);
            LineString loop;
            for (size_t i = 0; i < chain.vertices.size(); ++i) {
                checkpoint(cancel, i);
                const Vec3d p = m_mesh->vertices[chain.vertices[i]].cast<double>() - origin;
                loop.emplace_back(p.dot(u_axis), p.dot(v_axis));
            }
            if (chain.closed && !loop.empty()) loop.push_back(loop.front());
            bg::model::ring<bg::model::d2::point_xy<double>, false> ring(loop.begin(), loop.end());
            if (bg::area(ring) < 0.) std::reverse(ring.begin(), ring.end());
            bool simple = bg::is_valid(ring);
            for (const auto& other : loops) { checkpoint(cancel); if (bg::intersects(loop, other)) simple = false; }
            if (!simple) {
                region.diagnostics.ambiguous_boundary = true;
                region.boundary_complete = false;
                return;
            }
            loops.push_back(std::move(loop));
        }
        for (auto& chain : region.boundaries) {
            if (!chain.closed) continue;
            Vec3d sum = Vec3d::Zero();
            const Vec3d origin = m_mesh->vertices[chain.vertices[0]].cast<double>();
            for (size_t i = 0; i < chain.vertices.size(); ++i) {
                checkpoint(cancel, i);
                Vec3d a = m_mesh->vertices[chain.vertices[i]].cast<double>() - origin;
                Vec3d b = m_mesh->vertices[chain.vertices[(i + 1) % chain.vertices.size()]].cast<double>() - origin;
                sum += a.cross(b);
            }
            const double signed_area = sum.dot(m_normals[region.seed_triangle]);
            if (signed_area != 0.) chain.loop_kind = signed_area > 0. ? LoopKind::Outer : LoopKind::Inner;
        }
    }
}

VertexSelection MeshGeometry::select_vertex(size_t vertex) const
{
    if (vertex >= m_mesh->vertices.size()) throw std::out_of_range("Source vertex does not exist");
    return {m_mesh, vertex, m_mesh->vertices[vertex].cast<double>()};
}

EdgeSelection MeshGeometry::select_edge(EdgeKey key) const
{
    const auto& edge = m_edges[find_edge(key)];
    Chain chain;
    chain.vertices = {edge.key[0], edge.key[1]};
    return edge_result(m_mesh, chain, edge.key, {}, [&](EdgeKey k) { return m_edges[find_edge(k)].kind; });
}

ChainGraph MeshGeometry::build_chain_graph(double crease_angle, const SurfaceRegion* boundary, const Cancel& cancel) const
{
    range(crease_angle, 0., 180.);
    checkpoint(cancel);
    if (boundary && boundary->mesh != m_mesh) throw std::invalid_argument("Boundary belongs to another mesh snapshot");
    std::vector<unsigned char> allowed(m_edges.size(), 0);
    if (boundary) {
        for (const auto& chain : boundary->boundaries) {
            size_t segments = chain.closed ? chain.vertices.size() : chain.vertices.empty() ? 0 : chain.vertices.size() - 1;
            for (size_t i = 0; i < segments; ++i) {
                checkpoint(cancel, i);
                allowed[find_edge({chain.vertices[i], chain.vertices[(i + 1) % chain.vertices.size()]})] = 1;
            }
        }
    } else {
        for (size_t id = 0; id < m_edges.size(); ++id) {
            checkpoint(cancel, id);
            const auto& edge = m_edges[id];
            if (edge.kind == EdgeKind::Boundary) allowed[id] = 1;
            else if (edge.kind == EdgeKind::Manifold) {
                double crease = angle(m_normals[m_incidence[edge.begin].triangle], m_normals[m_incidence[edge.begin + 1].triangle]);
                if (crease > angle_epsilon && crease + angle_epsilon >= crease_angle) allowed[id] = 1;
            } else allowed[id] = 1; // defective edges remain individually inspectable
        }
    }
    ChainGraph result;
    result.m_mesh = m_mesh;
    if (boundary) result.m_boundary_complete = boundary->boundary_complete;
    for (size_t id = 0; id < allowed.size(); ++id) {
        checkpoint(cancel, id);
        if (allowed[id]) {
            result.m_edges.push_back(m_edges[id].key);
            result.m_kinds.push_back(m_edges[id].kind);
        }
    }
    result.m_adjacency = graph(result.m_edges, m_mesh->vertices.size(), cancel, &result.m_kinds);
    checkpoint(cancel);
    return result;
}

EdgeSelection ChainGraph::select_chain(EdgeKey seed, double turn_angle, const Cancel& cancel) const
{
    range(turn_angle, 0., 180.);
    checkpoint(cancel);
    seed = canonical(seed);
    auto it = std::lower_bound(m_edges.begin(), m_edges.end(), seed);
    if (it == m_edges.end() || *it != seed) throw std::invalid_argument("Seed edge is not in the candidate graph");
    const size_t id = size_t(it - m_edges.begin());
    const bool defective = m_kinds[id] != EdgeKind::Boundary && m_kinds[id] != EdgeKind::Manifold;
    Chain chain;
    if (defective) chain.vertices = {seed[0],seed[1]};
    else chain = trace(id, m_edges, m_adjacency, *m_mesh, turn_angle, cancel);
    auto result = edge_result(m_mesh, chain, seed, cancel, [&](EdgeKey key) {
        return m_kinds[size_t(std::lower_bound(m_edges.begin(), m_edges.end(), key) - m_edges.begin())];
    });
    result.boundary_complete = m_boundary_complete && !defective;
    checkpoint(cancel);
    return result;
}

bool ChainGraph::contains(EdgeKey edge) const
{
    return std::binary_search(m_edges.begin(), m_edges.end(), canonical(edge));
}

EdgeSelection MeshGeometry::select_chain(EdgeKey seed, const ChainSettings& settings,
                                         const SurfaceRegion* boundary, const Cancel& cancel) const
{
    settings.validate();
    return build_chain_graph(settings.crease_angle, boundary, cancel).select_chain(seed, settings.turn_angle, cancel);
}

double MeshGeometry::measure_length(const EdgeSelection& selection, const Transform3d& transform, const Cancel& cancel) const
{
    if (selection.mesh != m_mesh) throw std::invalid_argument("Selection belongs to another mesh snapshot");
    valid_transform(transform);
    double length = 0.;
    for (size_t i = 0; i < selection.edges.size(); ++i) {
        checkpoint(cancel, i);
        const auto& key = m_edges[find_edge(selection.edges[i])].key;
        const Vec3d a = transform * m_mesh->vertices[key[0]].cast<double>();
        const Vec3d b = transform * m_mesh->vertices[key[1]].cast<double>();
        length += (b - a).norm();
    }
    if (!std::isfinite(length)) throw std::invalid_argument("Non-finite transformed length");
    checkpoint(cancel);
    return length;
}

} // namespace Slic3r::PrepareGeometry
