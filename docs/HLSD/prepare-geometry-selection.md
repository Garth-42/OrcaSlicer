# Prepare geometry selection

`libslic3r/PrepareGeometry` analyzes source mesh regions, edges and vertices without requiring wxWidgets, OpenGL, Python or CAD. The Prepare canvas owns a native selection store and an explicit geometry-selection gizmo. Viewport picking and interaction use the geometry core without modifying the model, painting, slicing configuration or undo history.

## Source identity and ownership

`MeshGeometry` captures a detached, immutable indexed mesh. Triangle and vertex indices address the supplied arrays after import; source edge identity is the unordered pair of endpoint vertex indices. Coincident positions retain separate identities. Analysis never welds, repairs, subdivides or paints the mesh. The capture contains vertices and triangle indices, not the source's per-face painting properties.

Construction reads a source protected from concurrent mutation and captures detached arrays. Model-volume initialization and scale helpers also detach shared allocations before writing them. Subsequent queries use only immutable data and independent scratch storage, so they can run concurrently. Regions, edge selections, vertices and cached chain graphs retain the snapshot after the analysis or live model is destroyed.

Capture and analysis are scoped to one mesh generation. Model IDs, document generations, instances and placement matrices are supplied by the owner; array indices do not establish validity after a model edit. Native result structs are values. A consumer publishing immutable selections must retain them through a const owning handle and expose only read-only bindings.

## Topology and region membership

The analysis stores sorted source edges, complete edge incidence and triangle-to-edge references. Full incidence is necessary because a single-neighbor table cannot distinguish all nonmanifold cases. Source boundary edges, nonmanifold edges, inconsistent winding and edges incident to degenerate triangles are distinct. Growth crosses only consistently oriented manifold adjacencies. Degenerate triangles cannot seed a region. Degeneracy means zero geometric area; small valid triangles are retained.

`SurfaceSettings` validates every supplied numeric control, including controls inactive in the selected mode. Angles use degrees, lengths use local mesh millimeters. Triangle mode returns the source seed. Planar growth compares every candidate normal and vertex against the fixed seed plane, preventing accumulated drift. Curved growth compares neighboring oriented normals, optionally limits candidate normals against the fixed seed normal, and optionally limits every candidate vertex by Euclidean distance from the seed hit. The valid seed is included even when the distance limit would exclude one of its vertices. An adjacency that fails an angle test does not exclude a triangle reachable by another path.

Angular comparisons allow `1e-10` degrees, distance comparisons `1e-9` local mm. These numerical allowances do not grow with mesh scale. Settings retain the documented ranges and defaults in the header. Detection operates on original local geometry regardless of instance placement.

A supplied seed hit must fall on its source triangle within float-raycast rounding slack: eight float machine epsilons in barycentric coordinates and that slack times the largest of 1 mm, seed-origin magnitude and seed-edge lengths for plane distance. Accepted hits are reconstructed from clamped barycentric coordinates on the original triangle. This pick-rounding allowance is separate from region detection tolerances. Non-finite inputs and invalid source indices are rejected.

Each region reports a sorted unique triangle list, its seed, captured settings, local measurements and a fixed-seed-plane planarity diagnostic. Planarity uses the captured plane angle and distance controls in every mode, including Curved and Triangle, without changing membership.

## Boundaries and chains

A region boundary consists of source edges incident to exactly one selected triangle. Boundary graphs are traced through degree-two vertices and stop at junctions. Open chains and closed loops are explicit; closed loops list the start vertex once. Broken or ambiguous topology produces diagnostics and an incomplete boundary rather than a fabricated closing segment. An entirely selected closed manifold shell has a complete empty boundary.

Boundary directions follow selected source triangle winding. Closed loops start at their smallest source vertex. Outer/inner classification requires complete planar boundaries: contours are projected onto an orthonormal seed-plane frame, validated with Boost.Geometry, and checked for mutual intersections. Signed area along the seed normal distinguishes outer and inner loops. Invalid projected boundaries retain source chains with unknown classification and an ambiguity diagnostic. Curved boundaries remain unclassified. Collinear source vertices are preserved, including those in long straight runs.

`build_chain_graph` constructs an immutable candidate graph for a crease angle or a selected region's boundary. Consumers cache it by mesh snapshot, crease settings and boundary identity. Repeated seed or turn-limit queries reuse its topology. The convenience `MeshGeometry::select_chain` constructs a graph for a single query; hover consumers should use the cached graph instead.

Feature candidates include source open boundaries and manifold edges meeting the crease threshold. Coplanar diagonals are excluded even at zero threshold. Defective edges remain individually inspectable but cannot connect a chain. Traversal grows both ways through degree-two vertices, stopping at endpoints, junctions or an excessive travel-tangent turn. Straight continuation is zero degrees. Closed chains rotate to their smallest source vertex and choose the lexicographically smaller direction, independent of the picked edge. These are polygonal chains, with no curve fitting or invented vertices. Explicit mesh-edge selection can select triangulation diagonals.

## Measurements and execution

Local area and centroid are triangle-area weighted. A region's seed normal follows source triangle winding; its mean normal is optional and omitted when the area-normal sum is at most `1e-12` of the total unsigned area-normal magnitude. A centroid can lie outside its region or inside a hole. The reconstructed seed hit is the on-mesh reference point.

World measurements require an explicit finite, invertible affine transform. Area and winding normals are computed from transformed triangle vertices, and length from transformed source endpoints, preserving nonuniform scale, shear and mirror behavior. World coordinates mean model placement, not G-code coordinates. Singular transforms and non-finite measurements are rejected.

Analysis and long queries accept a throwing cancellation callback. Checkpoints occur during capture, edge sorting, growth, measurements, graph construction and traversal; an exception unwinds the unfinished value. Callbacks run on the querying thread. Callers must check request/document/mesh/view generations again before publishing a completed result. Library contour validation is bracketed by checkpoints but does not accept an internal interruption callback. Geometry analysis does not publish selections, schedule work, modify the live model or create undo entries.

## Native selection and lifetime

`PrepareSelectionStore` belongs to the canvas and outlives its gizmos. It publishes one immutable tagged surface, edge or vertex item, or an empty state. An item captures object, volume, instance and plate identity, a mesh-revision witness, local hit, detection settings and local-to-world placement. Document generations and selection revisions increase independently of serialized model IDs. They are transient and do not change project files or profiles.

`GeometryRevision` retires stamps before mesh replacement, initialization writes and model-volume destruction. A copied volume has an independent witness even when its source allocation is shared. Deserialization retires the previous witness revision; undo does not restore selection validity. Workers and retained snapshots own detached arrays, never borrowed model or GL-volume objects.

Reads and request checks are protected by a mutex. Reading a stale live item produces a revisioned invalidation. Native observer notifications capture state values and are delivered on the UI thread, outside locks and input handlers. Unsubscription and exceptions are isolated; a nested dispatch leaves newer revisions queued until the current batch finishes. A retained old item remains readable after live invalidation.

Ordinary selection changes, part/instance changes, hiding the target, plate/tab switches, conflicting editing gizmos, repair, deletion, reload, undo and document replacement clear live geometry selection. Numeric placement updates preserve membership and publish a fresh transform. Direct interactive moves temporarily publish an empty state and restore membership only when the same selected source, instance and plate survive. Clearing or replacing the document cancels that restoration.

## Picking, workers and display

`PrepareGeometryPicker` uses the repository's source-triangle BVH. It finds the nearest eligible original surface across active scene volumes, with source winding and placement taken into account. Clipping halfspaces and the renderer's build-volume floor determine eligibility. Open meshes do not require even ray-hit parity. Display-only clip intersections never acquire source IDs.

Edge and vertex candidates are ranked by projected distance, depth and source identity. The radius is expressed in logical pixels and scaled once for the physical viewport. Perspective edge interpolation returns an actual source-edge point. Clipped edges are trimmed for the screen-distance query while retaining their original endpoint IDs. Each candidate is visibility-tested with a ray at its own projected position, so an unobstructed cursor ray cannot select a hidden nearby feature. Coincident source candidates remain separate and can be cycled explicitly.

The gizmo captures source arrays on the UI thread, then builds immutable analysis/BVH caches and computes queries through an owned `Job` worker. A short UI timer drains worker results. Jobs carry cancellation flags, request/context stamps, captured placement and camera/viewport/DPI/clip state. Before draining completions, UI synchronization compares active sources, visibility and placement matrices, including numeric moves that do not rebuild GL volumes. Completion checks captured values before accepting view-dependent results; teardown disables finalizers before joining the worker. Geometry and candidate graphs are cached by source generation and candidate settings. Workers never call wx or OpenGL.

Hover results remain previews. Numeric edits recompute from the committed original seed; Apply publishes the complete preview, while Cancel, a new mode/seed and Done discard pending settings. Drag and capture-lost gestures never commit a click. Escape first cancels pending settings, then clears the item, then exits. Done retains the item, highlight and a visible Clear/reopen indicator.

Overlay geometry is prepared on the worker and uploaded only with the render context current. Preview surfaces are outlines, preview edges are dashed, and nearby vertex dots are smaller than the preview and committed point. Committed surfaces use a translucent fill. Overlays obey scene depth and clipping, restore modified GL state, and indicate when clipping hides the retained selection completely.
