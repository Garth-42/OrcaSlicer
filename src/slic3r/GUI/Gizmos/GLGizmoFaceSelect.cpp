#include "GLGizmoFaceSelect.hpp"

#include "libslic3r/Color.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/SelectionSnapshot.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "GLGizmoUtils.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <glad/gl.h>
#include <imgui.h>
#include <memory>
#include <wx/busycursor.h>

namespace Slic3r::GUI {

namespace {
// The state a selected triangle has inside the gizmo's TriangleSelectors.
constexpr EnforcerBlockerType SELECTED = EnforcerBlockerType::ENFORCER;
const ColorRGBA               SELECTED_COLOR{1.f, 0.6f, 0.1f, 1.f};
} // namespace

GLGizmoFaceSelect::GLGizmoFaceSelect(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoPainterBase(parent, icon_filename, sprite_id), m_current_tool(ImGui::TriangleButtonIcon)
{
    // A selection is a set of whole mesh triangles: a brush takes every triangle it touches.
    m_triangle_splitting_enabled = false;
}

void GLGizmoFaceSelect::on_shutdown()
{
    m_parent.use_slope(false);
    m_parent.toggle_model_objects_visibility(true);
}

std::string GLGizmoFaceSelect::on_get_name() const
{
    return _u8L("Face selection");
}

bool GLGizmoFaceSelect::on_init()
{
    const wxString ctrl  = GUI::shortkey_ctrl_prefix();
    const wxString shift = GUI::shortkey_shift_prefix();

    m_desc["clear"]            = _L("Clear");
    m_desc["clear_all"]        = _L("Clear all");
    m_desc["tool_type"]        = _L("Tool type");
    m_desc["cursor_size"]      = _L("Brush size");
    m_desc["smart_fill_angle"] = _L("Smart fill angle");
    m_desc["select"]           = _L("Select faces");
    m_desc["deselect"]         = _L("Deselect faces");

    const std::pair<wxString, wxString> select   = {_L("Left mouse button"), m_desc["select"]};
    const std::pair<wxString, wxString> deselect = {shift + _L("Left mouse button"), m_desc["deselect"]};

    m_shortcuts_brush      = {select, deselect, {ctrl + _L("Mouse wheel"), m_desc["cursor_size"]}};
    m_shortcuts_triangle   = {select, deselect};
    m_shortcuts_smart_fill = {select, deselect, {ctrl + _L("Mouse wheel"), m_desc["smart_fill_angle"]}};
    return true;
}

void GLGizmoFaceSelect::render_painter_gizmo()
{
    const Selection& selection = m_parent.get_selection();

    if (!is_painting() && !m_triangle_selectors.empty() && SelectionSnapshotStore::instance().faces_revision() != m_faces_revision)
        update_from_model_object(false);

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    render_triangles(selection);
    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();
    render_cursor();

    glsafe(::glDisable(GL_BLEND));
}

void GLGizmoFaceSelect::on_render_input_window(float x, float y, float bottom_limit)
{
    const ModelObject* mo = m_c->selection_info()->model_object();
    if (!mo)
        return;

    float scale = m_parent.get_scale();
#ifdef WIN32
    scale *= (float) get_dpi_for_window(wxGetApp().GetTopWindow()) / (float) DPI_DEFAULT;
#endif // WIN32

    y = std::min(y, bottom_limit - m_imgui->scaled(22.f));
#if BBS_TOOLBAR_ON_TOP
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
#else
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 1.0f, 0.0f);
#endif

    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * m_parent.get_gizmos_manager().get_layout_scale()));
    GizmoImguiBegin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    const float space_size         = m_imgui->get_style_scaling() * 8;
    const float sliders_left_width = std::max(m_imgui->calc_text_size(m_desc.at("smart_fill_angle")).x,
                                              m_imgui->calc_text_size(m_desc.at("cursor_size")).x) + m_imgui->scaled(1.5f);
    const float slider_icon_width  = m_imgui->get_slider_icon_size().x;
    const float empty_button_width = m_imgui->calc_button_size("").x;
    const float sliders_width      = m_imgui->scaled(7.0f);
    const float drag_left_width    = ImGui::GetStyle().WindowPadding.x + sliders_width - space_size;
    const float max_tooltip_width  = ImGui::GetFontSize() * 20.0f;

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("tool_type"));

    const std::array<wchar_t, 4> tool_ids = {ImGui::CircleButtonIcon, ImGui::SphereButtonIcon, ImGui::TriangleButtonIcon, ImGui::FillButtonIcon};
    const std::array<wchar_t, 4> icons    = m_is_dark_mode ?
        std::array<wchar_t, 4>{ImGui::CircleButtonDarkIcon, ImGui::SphereButtonDarkIcon, ImGui::TriangleButtonDarkIcon, ImGui::FillButtonDarkIcon} :
        tool_ids;
    const std::array<wxString, 4> tool_tips = {_L("Circle"), _L("Sphere"), _L("Triangle"), _L("Fill")};

    auto unselect_seed_fill = [this]() {
        for (auto& triangle_selector : m_triangle_selectors) {
            triangle_selector->seed_fill_unselect_all_triangles();
            triangle_selector->request_update_render_data();
        }
    };

    for (size_t i = 0; i < tool_ids.size(); ++i) {
        if (i != 0)
            ImGui::SameLine((empty_button_width + m_imgui->scaled(1.75f)) * i + m_imgui->scaled(1.5f));

        const bool is_active = m_current_tool == tool_ids[i];
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.f * scale, 4.f * scale));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        ImGui::PushStyleColor(ImGuiCol_Button, is_active ? ImVec4(0.f, .59f, .53f, .25f) : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, is_active ? ImVec4(0.f, .59f, .53f, .25f) : ImVec4(.6f, .6f, .6f, .2f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, is_active ? ImVec4(0.f, .59f, .53f, .30f) : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, is_active ? ImGuiWrapper::COL_ORCA : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_BorderActive, is_active ? ImGuiWrapper::COL_ORCA : ImVec4(0, 0, 0, 0));
        const bool btn_clicked = m_imgui->glyph_button(icons[i], ImVec2(16.f * scale, 16.f * scale));
        ImGui::PopStyleColor(6);
        ImGui::PopStyleVar(3);

        if (btn_clicked && !is_active) {
            m_current_tool = tool_ids[i];
            unselect_seed_fill();
        }
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(tool_tips[i], max_tooltip_width);
    }

    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));

    const std::vector<std::pair<wxString, wxString>>* shortcuts = &m_shortcuts_triangle;
    if (m_current_tool == ImGui::CircleButtonIcon || m_current_tool == ImGui::SphereButtonIcon) {
        m_cursor_type = m_current_tool == ImGui::CircleButtonIcon ? TriangleSelector::CursorType::CIRCLE : TriangleSelector::CursorType::SPHERE;
        m_tool_type   = ToolType::BRUSH;
        shortcuts     = &m_shortcuts_brush;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("cursor_size"));
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        m_imgui->bbl_slider_float_style("##cursor_radius", &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width + sliders_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##cursor_radius_input", &m_cursor_radius, 0.05f, 0.0f, 0.0f, "%.2f");
    } else if (m_current_tool == ImGui::TriangleButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_tool_type   = ToolType::BRUSH;
    } else {
        assert(m_current_tool == ImGui::FillButtonIcon);
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_tool_type   = ToolType::SMART_FILL;
        shortcuts     = &m_shortcuts_smart_fill;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("smart_fill_angle"));
        const std::string format_str = std::string("%.f") + I18N::translate_utf8("°", "Face angle threshold,"
                                                                                       "placed after the number with no whitespace in between.");
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        if (m_imgui->bbl_slider_float_style("##smart_fill_angle", &m_smart_fill_angle, SmartFillAngleMin, SmartFillAngleMax, format_str.data(), 1.0f, true))
            unselect_seed_fill();
        ImGui::SameLine(drag_left_width + sliders_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##smart_fill_angle_input", &m_smart_fill_angle, 0.05f, 0.0f, 0.0f, "%.2f");
    }

    ImGui::Separator();
    m_imgui->text(format_wxstr(_L_PLURAL("%1% triangle selected", "%1% triangles selected", m_selected_count), m_selected_count));
    ImGui::Separator();

    GLGizmoUtils::render_tooltip_button(m_imgui, m_parent, *shortcuts, x, y);

    SelectionSnapshotStore& store = SelectionSnapshotStore::instance();
    auto reset_selectors = [this]() {
        for (auto& triangle_selector : m_triangle_selectors) {
            triangle_selector->reset();
            triangle_selector->request_update_render_data(true);
        }
    };

    ImGui::SameLine();
    m_imgui->disabled_begin(m_selected_count == 0);
    if (m_imgui->button(m_desc.at("clear"))) {
        reset_selectors();
        update_model_object();
        m_parent.set_as_dirty();
    }
    m_imgui->disabled_end();

    ImGui::SameLine();
    m_imgui->disabled_begin(store.face_count() == 0);
    if (m_imgui->button(m_desc.at("clear_all"))) {
        reset_selectors();
        m_selected_count = 0;
        if (store.clear_faces())
            fire_selection_changed("faces", store.revision());
        m_faces_revision = store.faces_revision();
        m_parent.set_as_dirty();
    }
    m_imgui->disabled_end();

    ImGui::SameLine();
    GLGizmoUtils::begin_right_aligned_buttons({_L("Done")});
    if (m_imgui->button(_L("Done")))
        m_parent.reset_all_gizmos();

    ImGui::PopStyleVar(1); // ImGuiStyleVar_FramePadding
    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoFaceSelect::update_model_object()
{
    const ModelObject*     mo  = m_c->selection_info()->model_object();
    const ModelObjectPtrs& mos = wxGetApp().model().objects;
    const auto             it  = std::find(mos.begin(), mos.end(), mo);
    if (it == mos.end())
        return;

    SelectionSnapshotStore& store   = SelectionSnapshotStore::instance();
    bool                    changed = false;
    int                     idx     = -1;
    m_selected_count = 0;
    for (size_t volume_idx = 0; volume_idx < mo->volumes.size(); ++volume_idx) {
        const ModelVolume* mv = mo->volumes[volume_idx];
        if (!mv->is_model_part())
            continue;

        ++idx;
        SelectedFaces entry;
        entry.object_id      = mo->id().id;
        entry.volume_id      = mv->id().id;
        entry.object_index   = int(it - mos.begin());
        entry.volume_index   = int(volume_idx);
        entry.instance_index = m_parent.get_selection().get_instance_idx();
        entry.mesh           = mv->get_mesh_shared_ptr();
        // Triangles are never split here, so each selected piece is one mesh triangle.
        m_triangle_selectors[idx]->get_facets_strict(SELECTED, &entry.triangles);
        std::sort(entry.triangles.begin(), entry.triangles.end());
        entry.triangles.erase(std::unique(entry.triangles.begin(), entry.triangles.end()), entry.triangles.end());
        m_selected_count += entry.triangles.size();
        changed |= store.set_faces(std::move(entry));
    }

    m_faces_revision = store.faces_revision();
    if (changed)
        fire_selection_changed("faces", store.revision());
}

void GLGizmoFaceSelect::update_from_model_object(bool first_update)
{
    wxBusyCursor wait;

    const ModelObject* mo = m_c->selection_info()->model_object();
    m_triangle_selectors.clear();
    m_selected_count = 0;

    const std::vector<ColorRGBA>  ebt_colors = {GLVolume::NEUTRAL_COLOR, SELECTED_COLOR, TriangleSelectorGUI::blockers_color};
    const SelectionSnapshotStore& store      = SelectionSnapshotStore::instance();
    m_faces_revision = store.faces_revision();
    for (const ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;

        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(mv->mesh(), ebt_colors));
        const std::vector<int> triangles = store.face_triangles(mv->id().id, mv->get_mesh_shared_ptr());
        for (int triangle : triangles)
            m_triangle_selectors.back()->set_facet(triangle, SELECTED);
        m_selected_count += triangles.size();
        m_triangle_selectors.back()->request_update_render_data();
    }
}

wxString GLGizmoFaceSelect::handle_snapshot_action_name(bool shift_down, GLGizmoPainterBase::Button button_down) const
{
    return shift_down ? _L("Deselect faces") : _L("Select faces");
}

} // namespace Slic3r::GUI
