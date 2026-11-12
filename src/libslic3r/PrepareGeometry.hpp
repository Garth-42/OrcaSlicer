#pragma once

#include "Point.hpp"
#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include <admesh/stl.h>

namespace Slic3r::PrepareGeometry {

// Cancellation callbacks may throw. An interrupted operation never returns a partial result.
using Cancel = std::function<void()>;
using EdgeKey = std::array<int, 2>;
enum class SurfaceMode { Triangle, Planar, Curved };
enum class EdgeKind { Boundary, Manifold, Nonmanifold, InconsistentWinding, Degenerate };
enum class LoopKind { Unknown, Outer, Inner };

struct SurfaceSettings {
    SurfaceMode mode = SurfaceMode::Planar;
    double plane_angle = 1.;
    double plane_distance = 0.01;
    double neighbor_angle = 30.;
    std::optional<double> seed_angle;
    std::optional<double> distance;
    void validate() const;
};

struct ChainSettings {
    double crease_angle = 30.;
    double turn_angle = 30.;
    void validate() const;
};

struct Diagnostics {
    bool open_mesh = false;
    bool degenerate = false;
    bool nonmanifold = false;
    bool inconsistent_winding = false;
    bool ambiguous_boundary = false;
};

struct Chain {
    // A closed chain lists its first vertex once; the closing edge is implicit.
    std::vector<int> vertices;
    bool closed = false;
    LoopKind loop_kind = LoopKind::Unknown;
};

struct SurfaceMetrics {
    double area = 0.;
    Vec3d centroid = Vec3d::Zero();
    Vec3d seed_normal = Vec3d::Zero();
    std::optional<Vec3d> mean_normal;
};

struct SurfaceRegion {
    std::shared_ptr<const indexed_triangle_set> mesh;
    SurfaceSettings settings;
    size_t seed_triangle = 0;
    Vec3d seed_hit = Vec3d::Zero();
    std::vector<size_t> triangles; // sorted, unique source indices
    SurfaceMetrics local;
    bool is_planar = false; // fixed seed-plane diagnostic in all three modes
    bool boundary_complete = true;
    std::vector<Chain> boundaries;
    Diagnostics diagnostics;
};

struct EdgeSelection {
    std::shared_ptr<const indexed_triangle_set> mesh;
    EdgeKey seed_edge{};
    std::vector<EdgeKey> edges; // travel order for chains; each key is canonical
    std::vector<EdgeKind> kinds;
    Chain chain;
    double length = 0.;
    bool boundary_complete = true;
};

struct VertexSelection {
    std::shared_ptr<const indexed_triangle_set> mesh;
    size_t vertex = 0;
    Vec3d position = Vec3d::Zero();
};

// Cache per mesh + crease settings (or selected boundary). Seed/turn changes do
// not rebuild this graph. Its immutable data can be queried from several workers.
class ChainGraph {
public:
    bool contains(EdgeKey edge) const;
    EdgeSelection select_chain(EdgeKey seed, double turn_angle, const Cancel& cancel = {}) const;
private:
    friend class MeshGeometry;
    std::shared_ptr<const indexed_triangle_set> m_mesh;
    std::vector<EdgeKey> m_edges;
    std::vector<EdgeKind> m_kinds;
    std::vector<std::vector<size_t>> m_adjacency;
    bool m_boundary_complete = true;
};

// Construct once per mesh generation on immutable input, then share for concurrent reads.
// The constructor detaches source arrays; it never modifies ModelVolume or painting state.
class MeshGeometry {
public:
    explicit MeshGeometry(const indexed_triangle_set& source, const Cancel& cancel = {});
    const std::shared_ptr<const indexed_triangle_set>& mesh() const { return m_mesh; }
    std::vector<EdgeKey> source_edges() const;
    SurfaceRegion select_surface(size_t seed, const Vec3d& hit, const SurfaceSettings& settings,
                                 const Cancel& cancel = {}) const;
    VertexSelection select_vertex(size_t vertex) const;
    EdgeSelection select_edge(EdgeKey edge) const;
    ChainGraph build_chain_graph(double crease_angle, const SurfaceRegion* boundary = nullptr,
                                 const Cancel& cancel = {}) const;
    // Null boundary: source boundary/crease graph. Otherwise use this region's boundary graph.
    EdgeSelection select_chain(EdgeKey seed, const ChainSettings& settings,
                               const SurfaceRegion* boundary = nullptr, const Cancel& cancel = {}) const;
    SurfaceMetrics measure_surface(const SurfaceRegion& region, const Transform3d& transform,
                                   const Cancel& cancel = {}) const;
    double measure_length(const EdgeSelection& selection, const Transform3d& transform,
                          const Cancel& cancel = {}) const;

private:
    struct Incidence { size_t triangle; unsigned char slot; bool forward; };
    struct Edge { EdgeKey key; size_t begin, end; EdgeKind kind; };
    std::shared_ptr<const indexed_triangle_set> m_mesh;
    std::vector<Vec3d> m_normals;
    std::vector<double> m_areas;
    std::vector<std::array<size_t, 3>> m_face_edges;
    std::vector<Incidence> m_incidence;
    std::vector<Edge> m_edges;

    size_t find_edge(EdgeKey key) const;
    bool on_seed_plane(size_t face, size_t seed, const SurfaceSettings& settings) const;
    void extract_boundaries(SurfaceRegion& region, const Cancel& cancel) const;
};

} // namespace Slic3r::PrepareGeometry
