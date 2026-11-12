#pragma once
#include "GLGizmoBase.hpp"
#include <memory>
#include <string>
#include <wx/event.h>

namespace Slic3r::GUI {
class GLGizmoPrepareSelection final : public GLGizmoBase
{
public:
    GLGizmoPrepareSelection(GLCanvas3D& parent, unsigned int sprite);
    ~GLGizmoPrepareSelection() override;
    bool on_mouse(const wxMouseEvent& event) override;
    void data_changed(bool serializing) override;
    bool render_follows_cursor() const override { return m_state == On; }
    void invalidate();
    void cancel_gesture();
    bool escape();
    void tick();
    void render_retained_indicator();

protected:
    bool on_init() override;
    std::string on_get_name() const override;
    bool on_is_activable() const override;
    bool on_is_selectable() const override;
    void on_set_state() override;
    void on_render() override;
    void on_render_input_window(float x, float y, float bottom) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace Slic3r::GUI
