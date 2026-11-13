#pragma once

// What the user has selected in the Prepare tab, published for readers outside the GUI thread
// (chiefly Python plugins, see docs/HLSD/plugin-selection-api.md). The store holds value copies
// only: the object selection as last published by the Prepare canvas, and the face selection
// made with the Face selection gizmo. It has no wx or OpenGL dependency.

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace Slic3r {

class Model;
class TriangleMesh;

namespace GUI {

enum class SelectionSnapshotMode { Instance, Volume };

// One selected instance (volume_index == -1) or one selected part of an instance.
struct SelectedItem
{
    int    object_index   = -1;
    int    instance_index = -1;
    int    volume_index   = -1;
    size_t object_id      = 0;
    size_t volume_id      = 0;

    bool operator==(const SelectedItem& rhs) const
    {
        return object_index == rhs.object_index && instance_index == rhs.instance_index && volume_index == rhs.volume_index &&
               object_id == rhs.object_id && volume_id == rhs.volume_id;
    }
};

// The triangles picked on one model part: sorted, unique indices into the part's mesh.
struct SelectedFaces
{
    size_t           object_id      = 0;
    size_t           volume_id      = 0;
    int              object_index   = -1;
    int              volume_index   = -1;
    // The instance that was active while picking. The triangles belong to the mesh, which all
    // instances share.
    int              instance_index = -1;
    std::vector<int> triangles;
    // The mesh the indices refer to. A part whose mesh was replaced loses its face selection.
    std::weak_ptr<const TriangleMesh> mesh;
};

struct SelectionSnapshot
{
    std::uint64_t              revision = 0;
    SelectionSnapshotMode      mode     = SelectionSnapshotMode::Instance;
    std::vector<SelectedItem>  items;
    std::vector<SelectedFaces> faces; // ordered by (object_index, volume_index)
};

class SelectionSnapshotStore
{
public:
    // One Plater exists per process, so does one store.
    static SelectionSnapshotStore& instance();

    // Every mutator returns true, and advances the revision, only when the content changed.

    bool set_items(SelectionSnapshotMode mode, std::vector<SelectedItem> items);

    // Replaces the face selection of entry.volume_id. Triangles are sorted and deduplicated, and
    // indices outside the mesh are dropped; an entry left without triangles is removed.
    bool set_faces(SelectedFaces entry);
    bool clear_faces();
    bool clear_object_faces(size_t object_id);

    // Drops entries whose part is gone or whose mesh was replaced, and refreshes the indices of
    // the others. Reads the model, so call it from the thread that owns the model.
    bool refresh_faces(const Model& model);

    // The triangles selected on a part, empty when none or when `mesh` is not the mesh they were
    // picked on.
    std::vector<int> face_triangles(size_t volume_id, const std::shared_ptr<const TriangleMesh>& mesh) const;
    size_t           face_count() const;

    SelectionSnapshot snapshot() const;
    std::uint64_t     revision() const;

private:
    mutable std::mutex              m_mutex;
    std::uint64_t                   m_revision = 0;
    SelectionSnapshotMode           m_mode     = SelectionSnapshotMode::Instance;
    std::vector<SelectedItem>       m_items;
    std::map<size_t, SelectedFaces> m_faces; // by volume id
};

// Fires LifecycleEvent::SelectionChanged. `source` is "objects" or "faces".
void fire_selection_changed(const char* source, std::uint64_t revision);

} // namespace GUI
} // namespace Slic3r
