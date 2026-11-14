#include "SelectionSnapshot.hpp"

#include "libslic3r/LifecycleEvents.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <utility>

namespace Slic3r::GUI {

SelectionSnapshotStore& SelectionSnapshotStore::instance()
{
    static SelectionSnapshotStore store;
    return store;
}

bool SelectionSnapshotStore::set_items(SelectionSnapshotMode mode, std::vector<SelectedItem> items)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (mode == m_mode && items == m_items)
        return false;
    m_mode  = mode;
    m_items = std::move(items);
    ++m_revision;
    return true;
}

bool SelectionSnapshotStore::set_faces(SelectedFaces entry)
{
    std::vector<int>& triangles = entry.triangles;
    std::sort(triangles.begin(), triangles.end());
    triangles.erase(std::unique(triangles.begin(), triangles.end()), triangles.end());
    const std::shared_ptr<const TriangleMesh> mesh  = entry.mesh.lock();
    const int                                 count = mesh ? int(mesh->its.indices.size()) : 0;
    triangles.erase(std::remove_if(triangles.begin(), triangles.end(), [count](int idx) { return idx < 0 || idx >= count; }),
                    triangles.end());

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_faces.find(entry.volume_id);
    if (triangles.empty()) {
        if (it == m_faces.end())
            return false;
        m_faces.erase(it);
    } else if (it == m_faces.end()) {
        m_faces.emplace(entry.volume_id, std::move(entry));
    } else {
        const bool same = it->second.triangles == triangles && it->second.mesh.lock() == mesh &&
                          it->second.object_index == entry.object_index && it->second.volume_index == entry.volume_index &&
                          it->second.instance_index == entry.instance_index && it->second.object_id == entry.object_id;
        if (same)
            return false;
        it->second = std::move(entry);
    }
    ++m_revision;
    ++m_faces_revision;
    return true;
}

bool SelectionSnapshotStore::clear_faces()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_faces.empty())
        return false;
    m_faces.clear();
    ++m_revision;
    ++m_faces_revision;
    return true;
}

bool SelectionSnapshotStore::clear_object_faces(size_t object_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const size_t before = m_faces.size();
    for (auto it = m_faces.begin(); it != m_faces.end();)
        it = it->second.object_id == object_id ? m_faces.erase(it) : std::next(it);
    if (m_faces.size() == before)
        return false;
    ++m_revision;
    ++m_faces_revision;
    return true;
}

bool SelectionSnapshotStore::refresh_faces(const Model& model)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_faces.empty())
        return false;

    bool                            changed = false;
    std::map<size_t, SelectedFaces> kept;
    for (size_t object_idx = 0; object_idx < model.objects.size(); ++object_idx) {
        const ModelObject* object = model.objects[object_idx];
        for (size_t volume_idx = 0; volume_idx < object->volumes.size(); ++volume_idx) {
            const ModelVolume* volume = object->volumes[volume_idx];
            auto               it     = m_faces.find(volume->id().id);
            if (it == m_faces.end() && object->volumes.size() == 1) {
                // ModelObject::delete_volume gives the last part left in an object a new id. Its
                // entry is then the only one of this object that still refers to the part's mesh.
                auto renamed = m_faces.end();
                bool unique  = true;
                for (auto other = m_faces.begin(); other != m_faces.end(); ++other)
                    if (other->second.object_id == object->id().id && other->second.mesh.lock() == volume->get_mesh_shared_ptr()) {
                        unique  = renamed == m_faces.end();
                        renamed = other;
                    }
                if (renamed != m_faces.end() && unique) {
                    renamed->second.volume_id = volume->id().id;
                    changed                   = true;
                    it                        = renamed;
                }
            }
            if (it == m_faces.end() || it->second.mesh.lock() != volume->get_mesh_shared_ptr())
                continue;
            SelectedFaces& entry = it->second;
            changed |= entry.object_index != int(object_idx) || entry.volume_index != int(volume_idx) ||
                       entry.object_id != object->id().id;
            entry.object_index = int(object_idx);
            entry.volume_index = int(volume_idx);
            entry.object_id    = object->id().id;
            if (entry.instance_index >= int(object->instances.size())) {
                entry.instance_index = object->instances.empty() ? -1 : 0;
                changed              = true;
            }
            kept.emplace(entry.volume_id, std::move(entry));
        }
    }
    changed |= kept.size() != m_faces.size();
    m_faces = std::move(kept);
    if (changed) {
        ++m_revision;
        ++m_faces_revision;
    }
    return changed;
}

std::vector<int> SelectionSnapshotStore::face_triangles(size_t volume_id, const std::shared_ptr<const TriangleMesh>& mesh) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_faces.find(volume_id);
    if (it == m_faces.end() || it->second.mesh.lock() != mesh)
        return {};
    return it->second.triangles;
}

size_t SelectionSnapshotStore::face_count() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t count = 0;
    for (const auto& [id, entry] : m_faces)
        count += entry.triangles.size();
    return count;
}

SelectionSnapshot SelectionSnapshotStore::snapshot() const
{
    SelectionSnapshot out;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        out.revision = m_revision;
        out.mode     = m_mode;
        out.items    = m_items;
        out.faces.reserve(m_faces.size());
        for (const auto& [id, entry] : m_faces)
            out.faces.push_back(entry);
    }
    std::sort(out.faces.begin(), out.faces.end(), [](const SelectedFaces& a, const SelectedFaces& b) {
        return std::make_pair(a.object_index, a.volume_index) < std::make_pair(b.object_index, b.volume_index);
    });
    return out;
}

std::uint64_t SelectionSnapshotStore::revision() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_revision;
}

std::uint64_t SelectionSnapshotStore::faces_revision() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_faces_revision;
}

void fire_selection_changed(const char* source, std::uint64_t revision)
{
    LifecycleEventContext ctx;
    ctx.source = source;
    ctx.index  = int(revision);
    fire_lifecycle_event(LifecycleEvent::SelectionChanged, ctx);
}

} // namespace Slic3r::GUI
