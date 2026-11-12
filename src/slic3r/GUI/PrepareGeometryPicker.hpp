#pragma once
#include "PrepareSelection.hpp"
#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrepareGeometry.hpp"
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace Slic3r::GUI {

// Owned immutable source geometry, never a borrowed GLVolume/raycaster.
struct PreparePickMesh
{
    explicit PreparePickMesh(std::shared_ptr<const indexed_triangle_set> source, const PrepareGeometry::Cancel& cancel = {});
    explicit PreparePickMesh(std::shared_ptr<const PrepareGeometry::MeshGeometry> analysis, const PrepareGeometry::Cancel& cancel = {});
    std::shared_ptr<const indexed_triangle_set> mesh;
    std::vector<PrepareGeometry::EdgeKey> edges;
    AABBTreeIndirect::Tree3f tree;
    double ray_epsilon = 1e-6;
};
struct PreparePickPart
{
    std::shared_ptr<const PreparePickMesh> mesh;
    Transform3d placement = Transform3d::Identity();
    bool two_sided        = false;
};
struct PreparePickView
{
    Matrix4d projection_view = Matrix4d::Identity();
    std::array<int, 4> viewport{0, 0, 1, 1};
    double pixel_scale = 1.;
    // World-space eligibility halfspaces, n.p + d >= 0. Includes the actual bed policy.
    std::vector<Vec4d> planes;
    bool same_view(const PreparePickView& other) const;
};
struct PreparePickCandidate
{
    size_t triangle = 0;
    std::optional<size_t> vertex;
    std::optional<PrepareGeometry::EdgeKey> edge;
    Vec3d local_hit        = Vec3d::Zero();
    double screen_distance = 0., depth = 0.;
};
// Pixel coordinates are physical, top-left origin, exactly as canvas/ImGui use.
// Snapped features are visibility-tested at THEIR projected position, not the mouse ray.
std::vector<PreparePickCandidate> prepare_pick_candidates(const std::vector<PreparePickPart>& scene,
                                                          size_t target,
                                                          const PrepareGeometry::MeshGeometry& geometry,
                                                          const PreparePickView& view,
                                                          const Vec2d& pixel,
                                                          const PreparePickSettings& settings,
                                                          const PrepareGeometry::Cancel& cancel          = {},
                                                          const PrepareGeometry::ChainGraph* chain_graph = nullptr);

} // namespace Slic3r::GUI
