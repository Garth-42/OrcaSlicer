#ifndef slic3r_GLGizmoFaceSelect_hpp_
#define slic3r_GLGizmoFaceSelect_hpp_

#include "GLGizmoPainterBase.hpp"

#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <wx/string.h>

namespace Slic3r::GUI {

// Picks whole mesh triangles and publishes them to the SelectionSnapshotStore, where plugins read
// them (docs/HLSD/plugin-selection-api.md). It reuses the painting tools of GLGizmoPainterBase,
// but unlike the paint gizmos it never changes the model: the selection is not saved with the
// project, is not part of undo/redo and does not affect slicing.
class GLGizmoFaceSelect : public GLGizmoPainterBase
{
public:
    GLGizmoFaceSelect(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

    void render_painter_gizmo() override;

protected:
    void        on_render_input_window(float x, float y, float bottom_limit) override;
    std::string on_get_name() const override;

    wxString handle_snapshot_action_name(bool shift_down, Button button_down) const override;

    // Nothing in the model changes, so neither entering, leaving nor a stroke is an undo step, and
    // a snapshot something else takes while the gizmo is open does not record it as open.
    bool wants_enter_leave_snapshots() const override { return false; }
    bool is_saved_in_snapshots() const override { return false; }
    bool stroke_takes_snapshot() const override { return false; }

    EnforcerBlockerType get_left_button_state_type() const override { return EnforcerBlockerType::ENFORCER; }
    EnforcerBlockerType get_right_button_state_type() const override { return EnforcerBlockerType::NONE; }

private:
    bool on_init() override;

    // Publishes the selectors' triangles to the store / seeds the selectors from the store.
    void update_model_object() override;
    void update_from_model_object(bool first_update) override;

    void             on_opening() override {}
    void             on_shutdown() override;
    PainterGizmoType get_painter_type() const override { return PainterGizmoType::FACE_SELECT; }

    wchar_t m_current_tool = 0;
    // Triangles selected on the edited object, for the panel.
    size_t  m_selected_count = 0;
    // The store's face revision the selectors match. A plugin may set the face selection while
    // the gizmo is open; the selectors are then rebuilt from the store.
    std::uint64_t m_faces_revision = 0;

    std::map<std::string, wxString>            m_desc;
    std::vector<std::pair<wxString, wxString>> m_shortcuts_brush;
    std::vector<std::pair<wxString, wxString>> m_shortcuts_triangle;
    std::vector<std::pair<wxString, wxString>> m_shortcuts_smart_fill;
};

} // namespace Slic3r::GUI

#endif // slic3r_GLGizmoFaceSelect_hpp_
