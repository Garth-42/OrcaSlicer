#pragma once
#include "libslic3r/GeometryRevision.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrepareGeometry.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <variant>
#include <vector>

namespace Slic3r::GUI {

enum class PreparePickMode { Surface, MeshEdge, FeatureChain, BoundaryChain, Vertex };
enum class PrepareSelectionReason { Picked, SettingsApplied, TransformChanged, Cleared, Invalidated };
struct PreparePickSettings
{
    PreparePickMode mode = PreparePickMode::Surface;
    PrepareGeometry::SurfaceSettings surface;
    PrepareGeometry::ChainSettings chain;
    double radius = 8.; // logical pixels
    void validate() const;
    bool operator==(const PreparePickSettings& other) const;
};
struct PrepareTarget
{
    std::uint64_t object = 0, volume = 0, instance = 0;
    int plate = -1;
    GeometryRevision::Stamp mesh;
    bool same_source(const PrepareTarget& other) const;
};
using PrepareGeometryItem = std::variant<std::shared_ptr<const PrepareGeometry::SurfaceRegion>,
                                         std::shared_ptr<const PrepareGeometry::EdgeSelection>,
                                         std::shared_ptr<const PrepareGeometry::VertexSelection>>;
struct PrepareSelectionItem
{
    PrepareTarget target;
    Transform3d local_to_world = Transform3d::Identity();
    Vec3d local_hit            = Vec3d::Zero();
    PreparePickSettings settings;
    PrepareGeometryItem geometry;
};
struct PrepareSelectionState
{
    std::uint64_t document_generation = 0, selection_revision = 0;
    PrepareSelectionReason reason = PrepareSelectionReason::Cleared;
    std::shared_ptr<const PrepareSelectionItem> item;
};

// Camera drags and canceled/capture-lost gestures can never become pick clicks.
class PreparePickGesture
{
public:
    void begin(const Vec2d& pixel, double pixel_scale);
    void move(const Vec2d& pixel);
    bool release();
    void cancel() { m_pressed = false; }
    bool pressed() const { return m_pressed; }

private:
    Vec2d m_start      = Vec2d::Zero();
    double m_threshold = 3.;
    bool m_pressed = false, m_dragged = false;
};

// UI mutates/dispatches; workers may read or test request validity without a UI round trip.
// Notifications are queued values and delivered explicitly outside mouse handlers and locks.
class PrepareSelectionStore
{
public:
    struct Ticket
    { std::uint64_t document, request; };
    using Observer = std::function<void(const PrepareSelectionState&)>;
    PrepareSelectionStore();
    PrepareSelectionState get();
    Ticket request();
    bool current(Ticket ticket) const;
    bool publish(Ticket ticket, std::shared_ptr<const PrepareSelectionItem> item, PrepareSelectionReason reason);
    void clear(PrepareSelectionReason reason = PrepareSelectionReason::Cleared);
    void new_document();
    void transform(const Transform3d& matrix);
    void suspend();
    std::shared_ptr<const PrepareSelectionItem> suspended() const;
    void resume(const Transform3d& matrix);
    std::size_t observe(Observer observer);
    void unobserve(std::size_t id);
    void dispatch_pending();

private:
    struct Notification
    {
        PrepareSelectionState state;
        std::vector<std::size_t> observers;
    };
    mutable std::mutex m_mutex;
    PrepareSelectionState m_state;
    std::shared_ptr<const PrepareSelectionItem> m_suspended;
    std::uint64_t m_request     = 0;
    std::size_t m_next_observer = 0;
    std::map<std::size_t, Observer> m_observers;
    std::vector<Notification> m_pending;
    bool m_dispatching = false;
    void notify_locked();
    void validate_locked();
};

// Pure interaction policy shared by the native gizmo and headless interaction tests.
class PrepareSelectionSession
{
public:
    explicit PrepareSelectionSession(PrepareSelectionStore& store) : m_store(store) {}
    const PreparePickSettings& settings() const { return m_settings; }
    bool pending_settings() const { return m_pending; }
    bool picking() const { return m_picking; }
    void open() { m_picking = true; }
    void edit(PreparePickSettings settings);
    void cancel_settings();
    bool accept(std::shared_ptr<const PrepareSelectionItem> item, bool settings_apply = false);
    void done();
    // Returns true when the tool should exit. Text controls handle Escape before calling this.
    bool escape();
    void clear();
    void invalidate();

private:
    PrepareSelectionStore& m_store;
    PreparePickSettings m_settings, m_committed_settings;
    bool m_pending = false, m_picking = false;
};
} // namespace Slic3r::GUI
