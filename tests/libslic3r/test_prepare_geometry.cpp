#include "libslic3r/PrepareGeometry.hpp"
#include "libslic3r/Point.hpp"
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::PrepareGeometry;
using Catch::Matchers::WithinAbs;

namespace {
indexed_triangle_set cube()
{
    indexed_triangle_set m;
    m.vertices = {{0,0,0},{20,0,0},{20,20,0},{0,20,0},{0,0,20},{20,0,20},{20,20,20},{0,20,20}};
    m.indices = {{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
                 {1,2,6},{1,6,5},{2,3,7},{2,7,6},{3,0,4},{3,4,7}};
    return m;
}

indexed_triangle_set square()
{
    indexed_triangle_set m;
    m.vertices = {{0,0,0},{2,0,0},{2,2,0},{0,2,0}};
    m.indices = {{0,1,2},{0,2,3}};
    return m;
}

indexed_triangle_set cylinder()
{
    indexed_triangle_set m;
    constexpr int n = 64;
    for (int z = 0; z < 2; ++z)
        for (int i = 0; i < n; ++i) {
            double a = 2. * std::acos(-1.) * i / n;
            m.vertices.emplace_back(float(10. * std::cos(a)), float(10. * std::sin(a)), float(z * 20));
        }
    for (int i = 0; i < n; ++i) {
        int next = (i + 1) % n;
        m.indices.emplace_back(i,next,next+n);
        m.indices.emplace_back(i,next+n,i+n);
    }
    m.vertices.emplace_back(0,0,0);
    m.vertices.emplace_back(0,0,20);
    for (int i = 0; i < n; ++i) {
        int next = (i + 1) % n;
        m.indices.emplace_back(128,next,i);
        m.indices.emplace_back(129,i+n,next+n);
    }
    return m;
}

indexed_triangle_set sphere()
{
    indexed_triangle_set m;
    constexpr int n = 32, rings = 15;
    m.vertices.emplace_back(0,0,1);
    for (int row = 1; row <= rings; ++row) {
        double latitude = std::acos(-1.) * row / (rings + 1);
        for (int col = 0; col < n; ++col) {
            double longitude = 2. * std::acos(-1.) * col / n;
            m.vertices.emplace_back(float(std::sin(latitude) * std::cos(longitude)),
                                    float(std::sin(latitude) * std::sin(longitude)), float(std::cos(latitude)));
        }
    }
    m.vertices.emplace_back(0,0,-1);
    for (int col = 0; col < n; ++col) {
        int next = (col + 1) % n;
        m.indices.emplace_back(0,1+col,1+next);
        for (int row = 0; row < rings - 1; ++row) {
            int a = 1 + row*n + col, b = 1 + row*n + next;
            m.indices.emplace_back(a,a+n,b+n);
            m.indices.emplace_back(a,b+n,b);
        }
        int base = 1 + (rings - 1)*n;
        m.indices.emplace_back(1+rings*n,base+next,base+col);
    }
    return m;
}

indexed_triangle_set annulus()
{
    indexed_triangle_set m;
    m.vertices = {{-2,-2,0},{2,-2,0},{2,2,0},{-2,2,0},{-1,-1,0},{1,-1,0},{1,1,0},{-1,1,0}};
    for (int i = 0; i < 4; ++i) {
        int next = (i + 1) % 4;
        m.indices.emplace_back(i,next,next+4);
        m.indices.emplace_back(i,next+4,i+4);
    }
    return m;
}

Vec3d center(const indexed_triangle_set& m, size_t f)
{
    const auto& t = m.indices[f];
    return (m.vertices[t[0]].cast<double>() + m.vertices[t[1]].cast<double>() + m.vertices[t[2]].cast<double>()) / 3.;
}

SurfaceRegion select(const MeshGeometry& mesh, size_t seed = 0, SurfaceSettings settings = {})
{
    return mesh.select_surface(seed, center(*mesh.mesh(), seed), settings);
}
}

TEST_CASE("Planar selection preserves the cube face source triangles and boundary", "[PrepareGeometry]")
{
    MeshGeometry mesh(cube());
    auto r = select(mesh, 2);
    REQUIRE(r.triangles == std::vector<size_t>{2,3});
    REQUIRE_THAT(r.local.area, WithinAbs(400., 1e-10));
    REQUIRE_THAT((r.local.centroid - Vec3d(10,10,20)).norm(), WithinAbs(0., 1e-10));
    REQUIRE(r.boundary_complete);
    REQUIRE(r.boundaries.size() == 1);
    REQUIRE(r.boundaries[0].closed);
    REQUIRE(r.boundaries[0].vertices.size() == 4);
    REQUIRE(r.boundaries[0].loop_kind == LoopKind::Outer);
    REQUIRE(r.is_planar);
    SurfaceSettings s;
    s.mode = SurfaceMode::Triangle;
    REQUIRE(select(mesh, 2, s).triangles == std::vector<size_t>{2});
}

TEST_CASE("Region growth does not join coplanar islands or coincident source vertices", "[PrepareGeometry]")
{
    auto m = square();
    m.vertices.emplace_back(0,0,0);
    m.vertices.emplace_back(-2,0,0);
    m.vertices.emplace_back(0,-2,0);
    m.indices.emplace_back(4,5,6);
    m.indices.emplace_back(0,6,5); // vertex-only contact with the square
    MeshGeometry mesh(m);
    REQUIRE(select(mesh).triangles == std::vector<size_t>{0,1});
    REQUIRE(mesh.select_vertex(0).vertex != mesh.select_vertex(4).vertex);
    REQUIRE_THAT((mesh.select_vertex(0).position - mesh.select_vertex(4).position).norm(), WithinAbs(0., 1e-15));
}

TEST_CASE("Fixed seed-plane distance prevents growth from drifting across a shallow ramp", "[PrepareGeometry]")
{
    auto m = square();
    m.vertices.emplace_back(4,0,0.005f);
    m.vertices.emplace_back(4,2,0.005f);
    m.vertices.emplace_back(6,0,0.015f);
    m.vertices.emplace_back(6,2,0.015f);
    m.indices.insert(m.indices.end(), {{1,4,5},{1,5,2},{4,6,7},{4,7,5}});
    MeshGeometry mesh(m);
    SurfaceSettings s;
    s.plane_angle = 1.; s.plane_distance = 0.01;
    REQUIRE(select(mesh, 0, s).triangles == std::vector<size_t>{0,1,2,3});
    s.plane_distance = 0.02;
    REQUIRE(select(mesh, 0, s).triangles.size() == 6);
    s.plane_distance = 0.;
    REQUIRE(select(mesh, 0, s).triangles == std::vector<size_t>{0,1});
}

TEST_CASE("Plane distance equality includes only the documented numerical allowance", "[PrepareGeometry]")
{
    auto m = square();
    m.vertices[3].z() = 0.125f;
    MeshGeometry mesh(m);
    SurfaceSettings s;
    s.plane_angle = 45.; s.plane_distance = 0.125;
    REQUIRE(select(mesh, 0, s).triangles.size() == 2);
    s.plane_distance = 0.125 - 0.5e-9;
    REQUIRE(select(mesh, 0, s).triangles.size() == 2);
    s.plane_distance = 0.125 - 2e-9;
    REQUIRE(select(mesh, 0, s).triangles.size() == 1);
}

TEST_CASE("Curved growth wraps the cylinder and obeys seed and distance limits", "[PrepareGeometry]")
{
    MeshGeometry mesh(cylinder());
    SurfaceSettings s;
    s.mode = SurfaceMode::Curved; s.neighbor_angle = 10.;
    auto r = select(mesh, 0, s);
    REQUIRE(r.triangles.size() == 128);
    REQUIRE(r.triangles.back() == 127);
    REQUIRE_FALSE(r.is_planar);
    REQUIRE(r.boundary_complete);
    REQUIRE(r.boundaries.size() == 2);
    REQUIRE(r.boundaries[0].loop_kind == LoopKind::Unknown);
    REQUIRE_FALSE(r.local.mean_normal.has_value());
    s.neighbor_angle = 5.;
    REQUIRE(select(mesh, 0, s).triangles == std::vector<size_t>{0,1});
    s.neighbor_angle = 10.; s.seed_angle = 45.;
    REQUIRE(select(mesh, 0, s).triangles.size() < 128);
    s.seed_angle.reset(); s.distance = 4.;
    auto limited = select(mesh, 0, s);
    REQUIRE(limited.triangles == std::vector<size_t>{0}); // seed survives all-vertex distance test
    s.distance = 100.;
    REQUIRE(select(mesh, 0, s).triangles == r.triangles);
}

TEST_CASE("Curved selections report seed-plane planarity without changing membership", "[PrepareGeometry]")
{
    MeshGeometry mesh(square());
    SurfaceSettings s;
    s.mode = SurfaceMode::Curved;
    s.plane_angle = 0.; s.plane_distance = 0.;
    auto r = select(mesh, 0, s);
    REQUIRE(r.triangles.size() == 2);
    REQUIRE(r.is_planar);
}

TEST_CASE("A closed curved shell has complete empty boundaries and no mean normal", "[PrepareGeometry]")
{
    auto source = sphere();
    MeshGeometry mesh(source);
    SurfaceSettings s;
    s.mode = SurfaceMode::Curved; s.neighbor_angle = 30.;
    auto r = select(mesh, 0, s);
    REQUIRE(r.triangles.size() == source.indices.size());
    REQUIRE(r.boundaries.empty());
    REQUIRE(r.boundary_complete);
    REQUIRE_FALSE(r.local.mean_normal.has_value());
    s.seed_angle = 45.;
    REQUIRE(select(mesh, 0, s).triangles.size() < r.triangles.size());
    s.seed_angle.reset(); s.distance = 0.5;
    REQUIRE(select(mesh, 0, s).triangles.size() < r.triangles.size());
}

TEST_CASE("Increasing a single inclusion tolerance never shrinks surface membership", "[PrepareGeometry]")
{
    MeshGeometry mesh(cylinder());
    SurfaceSettings s;
    s.mode = SurfaceMode::Curved;
    const int control = GENERATE(0,1,2);
    std::vector<size_t> previous;
    for (double value : {0., 5., 10., 30., 90., 180.}) {
        if (control == 0) s.neighbor_angle = value;
        if (control == 1) s.seed_angle = value;
        if (control == 2) s.distance = value + 0.001;
        auto r = select(mesh, 0, s);
        REQUIRE(std::includes(r.triangles.begin(), r.triangles.end(), previous.begin(), previous.end()));
        previous = r.triangles;
    }
}

TEST_CASE("Triangle traversal order does not change the reachable curved component", "[PrepareGeometry]")
{
    auto source = sphere();
    MeshGeometry original(source);
    SurfaceSettings s;
    s.mode = SurfaceMode::Curved; s.neighbor_angle = 12.;
    const int limit = GENERATE(0,1,2);
    if (limit == 1) s.seed_angle = 45.;
    if (limit == 2) s.distance = 0.5;
    auto expected = select(original, 0, s).triangles;
    std::reverse(source.indices.begin(), source.indices.end());
    MeshGeometry reordered(source);
    auto actual = select(reordered, source.indices.size() - 1, s).triangles;
    for (size_t& f : actual) f = source.indices.size() - 1 - f;
    std::sort(actual.begin(), actual.end());
    REQUIRE(actual == expected);
    // Reachability is symmetric without seed/distance constraints, even when an
    // adjacency fails and another path through the mesh can reach that triangle.
    if (limit == 0)
        for (size_t f : expected) REQUIRE(select(original, f, s).triangles == expected);
}

TEST_CASE("Topology defects are barriers and retain diagnostic source edges", "[PrepareGeometry]")
{
    auto m = square();
    const int defect = GENERATE(0,1,2);
    if (defect == 0) m.indices[1] = {0,3,2}; // same-winding shared edge
    if (defect == 1) { m.vertices.emplace_back(1,0,1); m.indices.emplace_back(0,2,4); }
    if (defect == 2) m.vertices[3] = {1,1,0}; // degenerate neighbor
    MeshGeometry mesh(m);
    auto r = select(mesh);
    REQUIRE(r.triangles == std::vector<size_t>{0});
    REQUIRE_FALSE(r.boundary_complete);
    if (defect == 0) REQUIRE(r.diagnostics.inconsistent_winding);
    if (defect == 1) REQUIRE(r.diagnostics.nonmanifold);
    if (defect == 2) {
        REQUIRE(r.diagnostics.degenerate);
        REQUIRE_THROWS_AS(select(mesh, 1), std::invalid_argument);
    }
    auto edge = mesh.select_chain({0,2}, {});
    REQUIRE(edge.edges.size() == 1);
    REQUIRE_FALSE(edge.boundary_complete);
}

TEST_CASE("Mesh edges include diagonals but feature chains exclude coplanar diagonals", "[PrepareGeometry]")
{
    MeshGeometry mesh(cube());
    REQUIRE(mesh.select_edge({6,4}).seed_edge == EdgeKey{4,6});
    ChainSettings s;
    s.crease_angle = 0.; s.turn_angle = 180.;
    REQUIRE_THROWS_AS(mesh.select_chain({4,6}, s), std::invalid_argument);
    auto r = mesh.select_chain({4,5}, s);
    REQUIRE(r.edges.size() == 1); // all cube corners are junctions, even with a permissive turn
    REQUIRE(r.chain.vertices == std::vector<int>{4,5});
    REQUIRE_THAT(r.length, WithinAbs(20., 1e-12));
    REQUIRE_THROWS_AS(mesh.select_edge({0,6}), std::out_of_range);
}

TEST_CASE("Polygonal rim chains close deterministically and honor turn limits", "[PrepareGeometry]")
{
    MeshGeometry mesh(cylinder());
    ChainSettings s;
    s.crease_angle = 30.; s.turn_angle = 30.;
    auto a = mesh.select_chain({0,1}, s);
    auto b = mesh.select_chain({32,33}, s);
    REQUIRE(a.chain.closed);
    REQUIRE(a.edges.size() == 64);
    REQUIRE(a.chain.vertices.size() == 64);
    REQUIRE(a.chain.vertices == b.chain.vertices);
    REQUIRE(a.edges == b.edges);
    s.turn_angle = 1.;
    REQUIRE(mesh.select_chain({0,1}, s).edges.size() == 1);
}

TEST_CASE("Selected region boundaries provide an independent edge candidate graph", "[PrepareGeometry]")
{
    MeshGeometry mesh(cube());
    auto top = select(mesh, 2);
    ChainSettings s;
    s.turn_angle = 180.;
    auto edge = mesh.select_chain({4,5}, s, &top);
    REQUIRE(edge.chain.closed);
    REQUIRE(edge.edges.size() == 4);
    REQUIRE_THAT(edge.length, WithinAbs(80., 1e-10));
    REQUIRE_THROWS_AS(mesh.select_chain({0,1}, s, &top), std::invalid_argument);
}

TEST_CASE("A planar annulus reports distinct outer and inner source loops", "[PrepareGeometry]")
{
    MeshGeometry mesh(annulus());
    auto r = select(mesh);
    REQUIRE(r.triangles.size() == 8);
    REQUIRE_THAT(r.local.area, WithinAbs(12., 1e-10));
    REQUIRE_THAT(r.local.centroid.norm(), WithinAbs(0., 1e-10)); // centroid lies in the hole
    REQUIRE(r.boundary_complete);
    REQUIRE(r.boundaries.size() == 2);
    REQUIRE(std::count_if(r.boundaries.begin(), r.boundaries.end(), [](const Chain& c) { return c.loop_kind == LoopKind::Outer; }) == 1);
    REQUIRE(std::count_if(r.boundaries.begin(), r.boundaries.end(), [](const Chain& c) { return c.loop_kind == LoopKind::Inner; }) == 1);
    auto chain = mesh.select_chain({4,5}, {30.,180.}, &r);
    REQUIRE(chain.chain.closed);
    REQUIRE(chain.edges.size() == 4);
}

TEST_CASE("Open boundaries keep original vertices without welding or inventing caps", "[PrepareGeometry]")
{
    auto m = square();
    m.indices.pop_back();
    MeshGeometry mesh(m);
    auto r = select(mesh);
    REQUIRE(r.diagnostics.open_mesh);
    REQUIRE(r.boundary_complete);
    REQUIRE(r.boundaries[0].vertices.size() == 3);
    REQUIRE(r.boundaries[0].closed);
    REQUIRE(mesh.select_vertex(3).vertex == 3); // an unused source vertex remains addressable
    REQUIRE_THROWS_AS(mesh.select_vertex(4), std::out_of_range);
}

TEST_CASE("Surface and chain metrics use transformed geometry under affine placement", "[PrepareGeometry]")
{
    MeshGeometry mesh(square());
    auto r = select(mesh);
    auto edge = mesh.select_chain({0,1}, {30.,180.}, &r);
    Transform3d placement = Transform3d::Identity();
    placement.translate(Vec3d(10,20,30));
    placement.rotate(Eigen::AngleAxisd(0.37, Vec3d(1,2,3).normalized()));
    const bool mirrored = GENERATE(false, true);
    placement.scale(Vec3d(mirrored ? -2. : 2.,3.,0.5));
    const auto metrics = mesh.measure_surface(r, placement);
    REQUIRE_THAT(metrics.area, WithinAbs(24., 1e-10));
    REQUIRE_THAT((metrics.centroid - placement * Vec3d(1,1,0)).norm(), WithinAbs(0., 1e-10));
    const Vec3d a = placement * Vec3d(0,0,0), b = placement * Vec3d(2,0,0), c = placement * Vec3d(2,2,0);
    REQUIRE_THAT((metrics.seed_normal - (b-a).cross(c-a).normalized()).norm(), WithinAbs(0., 1e-12));
    REQUIRE_THAT(mesh.measure_length(edge, placement), WithinAbs(20., 1e-10));
    REQUIRE_THAT(r.local.area, WithinAbs(4., 1e-12));
    REQUIRE(r.triangles == std::vector<size_t>{0,1});
    placement.scale(Vec3d(0,1,1));
    REQUIRE_THROWS_AS(mesh.measure_surface(r, placement), std::invalid_argument);
    REQUIRE_THROWS_AS(mesh.measure_length(edge, placement), std::invalid_argument);
}

TEST_CASE("Snapshots survive source replacement and selections share immutable geometry", "[PrepareGeometry]")
{
    auto source = square();
    MeshGeometry mesh(source);
    auto r = select(mesh);
    auto v = mesh.select_vertex(0);
    source.vertices[0].x() = 42;
    source.clear();
    REQUIRE(r.mesh == v.mesh);
    REQUIRE(r.mesh == select(mesh).mesh);
    REQUIRE_THAT(r.mesh->vertices[0].x(), WithinAbs(0., 1e-10));
    MeshGeometry other(square());
    REQUIRE_THROWS_AS(other.measure_surface(r, Transform3d::Identity()), std::invalid_argument);
    REQUIRE_THROWS_AS(other.select_chain({0,1}, {}, &r), std::invalid_argument);
    auto retained = [] { MeshGeometry temporary(square()); return select(temporary); }();
    REQUIRE(retained.mesh->indices.size() == 2);
    REQUIRE_THAT(retained.local.area, WithinAbs(4.,1e-12));
}

TEST_CASE("Invalid settings and source geometry fail before producing selections", "[PrepareGeometry]")
{
    MeshGeometry mesh(square());
    for (double invalid : {-1., 181., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        SurfaceSettings s;
        s.neighbor_angle = invalid;
        REQUIRE_THROWS_AS(select(mesh, 0, s), std::invalid_argument);
        REQUIRE_THROWS_AS(mesh.select_chain({0,1}, {invalid,30.}), std::invalid_argument);
    }
    SurfaceSettings s;
    s.plane_angle = 46.; REQUIRE_THROWS_AS(s.validate(), std::invalid_argument);
    s = {}; s.plane_distance = 11.; REQUIRE_THROWS_AS(s.validate(), std::invalid_argument);
    s = {}; s.distance = 0.; REQUIRE_THROWS_AS(s.validate(), std::invalid_argument);
    s = {}; s.seed_angle = 181.; REQUIRE_THROWS_AS(s.validate(), std::invalid_argument);
    auto source = square(); source.indices[0][0] = -1;
    REQUIRE_THROWS_AS(MeshGeometry(source), std::invalid_argument);
    source = square(); source.vertices[0].x() = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_THROWS_AS(MeshGeometry(source), std::invalid_argument);
    REQUIRE_THROWS_AS(mesh.select_surface(0, Vec3d(100,100,0), {}), std::invalid_argument);
    REQUIRE_THROWS_AS(mesh.select_surface(2, Vec3d::Zero(), {}), std::out_of_range);
    MeshGeometry empty(indexed_triangle_set{});
    REQUIRE_THROWS_AS(empty.select_surface(0, Vec3d::Zero(), {}), std::out_of_range);
}

TEST_CASE("Cancellation discards interrupted analysis and query results", "[PrepareGeometry]")
{
    struct Cancelled {};
    const int stop = GENERATE(1,2,4);
    int calls = 0;
    Cancel cancel = [&] { if (++calls == stop) throw Cancelled{}; };
    REQUIRE_THROWS_AS(MeshGeometry(sphere(), cancel), Cancelled);
    MeshGeometry mesh(sphere());
    SurfaceSettings s; s.mode = SurfaceMode::Curved;
    calls = 0;
    REQUIRE_THROWS_AS(mesh.select_surface(0, center(*mesh.mesh(),0), s, cancel), Cancelled);
    REQUIRE(select(mesh, 0, s).boundary_complete);
    MeshGeometry rim(cylinder());
    calls = 0;
    REQUIRE_THROWS_AS(rim.select_chain({0,1}, {}, nullptr, cancel), Cancelled);
    REQUIRE(rim.select_chain({0,1}, {}).chain.closed);
}

TEST_CASE("Feature chains continue straight and stop at a sharp turn", "[PrepareGeometry]")
{
    indexed_triangle_set source;
    source.vertices = {{0,0,0},{1,0,0},{2,0,0},{2,1,0},{1,1,0},{0,1,0}};
    source.indices = {{0,1,4},{0,4,5},{1,2,3},{1,3,4}};
    MeshGeometry mesh(source);
    auto chain = mesh.select_chain({0,1}, {30.,0.});
    REQUIRE(chain.chain.vertices == std::vector<int>{0,1,2});
    REQUIRE_FALSE(chain.chain.closed);
    REQUIRE_THAT(chain.length, WithinAbs(2., 1e-12));
    REQUIRE(mesh.select_chain({0,1}, {30.,90.}).chain.closed);
    auto surface = select(mesh);
    REQUIRE(surface.boundary_complete);
    REQUIRE(surface.boundaries[0].vertices.size() == 6); // preserve collinear source vertices
    REQUIRE(surface.boundaries[0].loop_kind == LoopKind::Outer);
}

TEST_CASE("Pinched planar boundaries remain open at their junctions", "[PrepareGeometry]")
{
    indexed_triangle_set source;
    for (int y = 0; y <= 4; ++y)
        for (int x = 0; x <= 4; ++x) source.vertices.emplace_back(float(x),float(y),0.f);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            if ((x == 1 && y == 1) || (x == 2 && y == 2)) continue;
            int a = y*5+x;
            source.indices.emplace_back(a,a+1,a+6); source.indices.emplace_back(a,a+6,a+5);
        }
    MeshGeometry mesh(source);
    auto r = select(mesh);
    REQUIRE_THAT(r.local.area, WithinAbs(14., 1e-12));
    REQUIRE_FALSE(r.boundary_complete);
    REQUIRE(r.diagnostics.ambiguous_boundary);
    REQUIRE(std::any_of(r.boundaries.begin(), r.boundaries.end(), [](const Chain& c) { return !c.closed; }));
    for (const auto& chain : r.boundaries) REQUIRE(chain.loop_kind == LoopKind::Unknown);
}

TEST_CASE("A concave planar region retains its boundary and on-mesh seed", "[PrepareGeometry]")
{
    indexed_triangle_set source;
    source.vertices = {{0,0,0},{2,0,0},{2,1,0},{1,1,0},{1,2,0},{0,2,0}};
    source.indices = {{0,1,3},{1,2,3},{0,3,5},{3,4,5}};
    MeshGeometry mesh(source);
    auto r = select(mesh);
    REQUIRE(r.boundary_complete);
    REQUIRE(r.boundaries.size() == 1);
    REQUIRE(r.boundaries[0].vertices.size() == 6);
    REQUIRE(r.boundaries[0].loop_kind == LoopKind::Outer);
    REQUIRE_THAT(r.local.area, WithinAbs(3., 1e-12));
    REQUIRE_THAT((r.seed_hit - center(source,0)).norm(), WithinAbs(0.,1e-12));
}

TEST_CASE("Smooth transitions can spread across a neck unless an explicit limit stops them", "[PrepareGeometry]")
{
    indexed_triangle_set source;
    constexpr int n = 32, rows = 24;
    for (int row = 0; row <= rows; ++row) {
        double z = -2. + 4. * row / rows;
        double radius = 1. - 0.4 * std::cos(std::acos(-1.) * z / 2.);
        for (int col = 0; col < n; ++col) {
            double a = 2. * std::acos(-1.) * col / n;
            source.vertices.emplace_back(float(radius * std::cos(a)),float(radius * std::sin(a)),float(z));
        }
    }
    for (int row = 0; row < rows; ++row)
        for (int col = 0; col < n; ++col) {
            int a = row*n+col, b = row*n+(col+1)%n;
            source.indices.emplace_back(a,b,b+n); source.indices.emplace_back(a,b+n,a+n);
        }
    MeshGeometry mesh(source);
    SurfaceSettings s; s.mode = SurfaceMode::Curved; s.neighbor_angle = 30.;
    REQUIRE(select(mesh,0,s).triangles.size() == source.indices.size());
    s.distance = 1.;
    REQUIRE(select(mesh,0,s).triangles.size() < source.indices.size());
}

TEST_CASE("Plane angle equality and monotonic tolerances use the fixed seed", "[PrepareGeometry]")
{
    auto source = square(); source.vertices[3].z() = 0.125f;
    MeshGeometry mesh(source);
    Vec3d a = (source.vertices[2] - source.vertices[0]).cast<double>();
    Vec3d b = (source.vertices[3] - source.vertices[0]).cast<double>();
    Vec3d n = a.cross(b).normalized();
    double actual = std::atan2(Vec3d::UnitZ().cross(n).norm(), Vec3d::UnitZ().dot(n)) * 180. / std::acos(-1.);
    SurfaceSettings s; s.plane_distance = 1.; s.plane_angle = actual;
    REQUIRE(select(mesh,0,s).triangles.size() == 2);
    s.plane_angle = actual - 0.5e-10;
    REQUIRE(select(mesh,0,s).triangles.size() == 2);
    s.plane_angle = actual - 2e-10;
    REQUIRE(select(mesh,0,s).triangles.size() == 1);
    const bool distance = GENERATE(false,true);
    std::vector<size_t> previous;
    for (double value : {0.,0.01,0.1,0.2,1.,10.}) {
        s.plane_angle = distance ? 45. : value;
        s.plane_distance = distance ? value : 10.;
        auto r = select(mesh,0,s);
        REQUIRE(std::includes(r.triangles.begin(), r.triangles.end(), previous.begin(), previous.end()));
        previous = r.triangles;
    }
}

TEST_CASE("Float-rounded seed hits are reconstructed on the original triangle", "[PrepareGeometry]")
{
    MeshGeometry mesh(square());
    auto r = mesh.select_surface(0, Vec3d(1,0.5,1e-7), {});
    REQUIRE_THAT(r.seed_hit.z(), WithinAbs(0.,1e-15));
    REQUIRE_THROWS_AS(mesh.select_surface(0, Vec3d(1,0.5,0.01), {}), std::invalid_argument);
}

TEST_CASE("Concurrent region queries share the snapshot without modifying source geometry", "[PrepareGeometry]")
{
    MeshGeometry mesh(cylinder());
    SurfaceSettings settings; settings.mode = SurfaceMode::Curved;
    SurfaceRegion a, b;
    std::thread first([&] { a = select(mesh,0,settings); });
    std::thread second([&] { b = select(mesh,64,settings); });
    first.join(); second.join();
    REQUIRE(a.mesh == b.mesh);
    REQUIRE(a.triangles == b.triangles);
    REQUIRE(a.triangles.size() == 128);
}

TEST_CASE("Cached chain graphs outlive their analysis and reuse candidate topology", "[PrepareGeometry]")
{
    auto graph = [] { MeshGeometry mesh(cylinder()); return mesh.build_chain_graph(30.); }();
    auto a = graph.select_chain({0,1},30.);
    auto b = graph.select_chain({32,33},30.);
    REQUIRE(a.mesh == b.mesh);
    REQUIRE(a.chain.vertices == b.chain.vertices);
    REQUIRE(a.chain.closed);
    REQUIRE(graph.select_chain({0,1},1.).edges.size() == 1);
    REQUIRE_THROWS_AS(graph.select_chain({0,64},30.), std::invalid_argument);
}
