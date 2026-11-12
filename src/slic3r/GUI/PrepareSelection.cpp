#include "PrepareSelection.hpp"
#include "libslic3r/Point.hpp"
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace Slic3r::GUI {
namespace {
std::atomic<std::uint64_t> documents{0}, revisions{0};
struct DispatchGuard
{
    std::mutex& mutex;
    bool& active;
    ~DispatchGuard()
    {
        std::lock_guard<std::mutex> lock(mutex);
        active = false;
    }
};
} // namespace
void PreparePickGesture::begin(const Vec2d& pixel, double pixel_scale)
{
    m_pressed   = pixel.allFinite() && std::isfinite(pixel_scale) && pixel_scale > 0.;
    m_dragged   = false;
    m_start     = pixel;
    m_threshold = 3. * pixel_scale;
}
void PreparePickGesture::move(const Vec2d& pixel)
{
    if (m_pressed && (!pixel.allFinite() || (pixel - m_start).norm() > m_threshold))
        m_dragged = true;
}
bool PreparePickGesture::release()
{
    const bool click = m_pressed && !m_dragged;
    m_pressed        = false;
    return click;
}
void PreparePickSettings::validate() const
{
    surface.validate();
    chain.validate();
    if (!std::isfinite(radius) || radius < 2. || radius > 20.)
        throw std::invalid_argument("Invalid pick radius");
    switch (mode) {
    case PreparePickMode::Surface:
    case PreparePickMode::MeshEdge:
    case PreparePickMode::FeatureChain:
    case PreparePickMode::BoundaryChain:
    case PreparePickMode::Vertex: break;
    default: throw std::invalid_argument("Invalid pick mode");
    }
}
bool PrepareTarget::same_source(const PrepareTarget& other) const
{
    return object == other.object && volume == other.volume && instance == other.instance && plate == other.plate &&
           mesh.same_source(other.mesh);
}
bool PreparePickSettings::operator==(const PreparePickSettings& other) const
{
    return mode == other.mode && radius == other.radius && surface.mode == other.surface.mode &&
           surface.plane_angle == other.surface.plane_angle && surface.plane_distance == other.surface.plane_distance &&
           surface.neighbor_angle == other.surface.neighbor_angle && surface.seed_angle == other.surface.seed_angle &&
           surface.distance == other.surface.distance && chain.crease_angle == other.chain.crease_angle &&
           chain.turn_angle == other.chain.turn_angle;
}
PrepareSelectionStore::PrepareSelectionStore() { m_state.document_generation = ++documents; }
void PrepareSelectionStore::notify_locked()
{
    m_state.selection_revision = ++revisions;
    if (m_observers.empty())
        return;
    Notification n;
    n.state = m_state;
    for (const auto& observer : m_observers)
        n.observers.push_back(observer.first);
    m_pending.push_back(std::move(n));
}
void PrepareSelectionStore::validate_locked()
{
    if (m_state.item && !m_state.item->target.mesh.valid()) {
        ++m_request;
        m_state.item.reset();
        m_state.reason = PrepareSelectionReason::Invalidated;
        notify_locked();
    }
}
PrepareSelectionState PrepareSelectionStore::get()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    validate_locked();
    return m_state;
}
PrepareSelectionStore::Ticket PrepareSelectionStore::request()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    validate_locked();
    return {m_state.document_generation, ++m_request};
}
bool PrepareSelectionStore::current(Ticket ticket) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return ticket.document == m_state.document_generation && ticket.request == m_request;
}
bool PrepareSelectionStore::publish(Ticket ticket, std::shared_ptr<const PrepareSelectionItem> item, PrepareSelectionReason reason)
{
    if (item) {
        item->settings.validate();
        if (!item->local_hit.allFinite() || !item->local_to_world.matrix().allFinite() ||
            item->local_to_world.linear().determinant() == 0. || !std::visit([](const auto& value) { return bool(value); }, item->geometry))
            return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    validate_locked();
    if (ticket.document != m_state.document_generation || ticket.request != m_request || !item || !item->target.mesh.valid())
        return false;
    m_suspended.reset();
    m_state.item   = std::move(item);
    m_state.reason = reason;
    notify_locked();
    return true;
}
void PrepareSelectionStore::clear(PrepareSelectionReason reason)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_request;
    m_suspended.reset();
    if (!m_state.item)
        return;
    m_state.item.reset();
    m_state.reason = reason;
    notify_locked();
}
void PrepareSelectionStore::new_document()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_request;
    m_suspended.reset();
    m_state.document_generation = ++documents;
    m_state.item.reset();
    m_state.reason = PrepareSelectionReason::Invalidated;
    notify_locked();
}
void PrepareSelectionStore::transform(const Transform3d& matrix)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    validate_locked();
    if (!m_state.item || m_state.item->local_to_world.matrix() == matrix.matrix())
        return;
    ++m_request;
    if (!matrix.matrix().allFinite() || matrix.linear().determinant() == 0.) {
        m_state.item.reset();
        m_state.reason = PrepareSelectionReason::Invalidated;
    } else {
        auto item            = std::make_shared<PrepareSelectionItem>(*m_state.item);
        item->local_to_world = matrix;
        m_state.item         = std::move(item);
        m_state.reason       = PrepareSelectionReason::TransformChanged;
    }
    notify_locked();
}
void PrepareSelectionStore::suspend()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    validate_locked();
    if (!m_state.item)
        return;
    ++m_request;
    m_suspended = m_state.item;
    m_state.item.reset();
    m_state.reason = PrepareSelectionReason::Invalidated;
    notify_locked();
}
std::shared_ptr<const PrepareSelectionItem> PrepareSelectionStore::suspended() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_suspended;
}
void PrepareSelectionStore::resume(const Transform3d& matrix)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_suspended)
        return;
    auto held = std::move(m_suspended);
    if (!held->target.mesh.valid() || !matrix.matrix().allFinite() || matrix.linear().determinant() == 0.)
        return;
    auto restored            = std::make_shared<PrepareSelectionItem>(*held);
    restored->local_to_world = matrix;
    ++m_request;
    m_state.item   = std::move(restored);
    m_state.reason = PrepareSelectionReason::TransformChanged;
    notify_locked();
}
std::size_t PrepareSelectionStore::observe(Observer observer)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto id = ++m_next_observer;
    m_observers.emplace(id, std::move(observer));
    return id;
}
void PrepareSelectionStore::unobserve(std::size_t id)
{
    Observer released;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_observers.find(id);
        if (it != m_observers.end()) {
            released = std::move(it->second);
            m_observers.erase(it);
        }
    }
}
void PrepareSelectionStore::dispatch_pending()
{
    std::vector<Notification> pending;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_dispatching)
            return;
        validate_locked();
        pending.swap(m_pending);
        m_dispatching = true;
    }
    // A callback may run a nested event loop. Keep newer queued revisions behind
    // this batch, including when a callback explicitly requests another dispatch.
    DispatchGuard dispatch{m_mutex, m_dispatching};
    for (const auto& n : pending)
        for (auto id : n.observers) {
            Observer callback;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_observers.find(id);
                if (it != m_observers.end())
                    callback = it->second;
            }
            if (callback) {
                try {
                    callback(n.state);
                } catch (...) { /* Isolate native observers; plugin adapters log under their owner. */
                }
            }
        }
}
void PrepareSelectionSession::edit(PreparePickSettings settings)
{
    settings.validate();
    if (settings.mode != m_settings.mode || settings.surface.mode != m_settings.surface.mode) {
        const auto mode         = settings.mode;
        const auto surface_mode = settings.surface.mode;
        cancel_settings();
        m_settings.mode         = mode;
        m_settings.surface.mode = surface_mode;
        m_committed_settings    = m_settings;
        m_store.request();
        return;
    }
    m_settings = std::move(settings);
    auto item  = m_store.get().item;
    m_pending  = item && item->settings.mode == m_settings.mode && item->settings.surface.mode == m_settings.surface.mode;
    if (!m_pending)
        m_committed_settings = m_settings;
    m_store.request(); // cancel old preview work, never publish a settings hover
}
void PrepareSelectionSession::cancel_settings()
{
    if (m_pending) {
        m_settings = m_committed_settings;
        m_pending  = false;
        m_store.request();
    }
}
bool PrepareSelectionSession::accept(std::shared_ptr<const PrepareSelectionItem> item, bool settings_apply)
{
    if (!settings_apply)
        cancel_settings();
    if (!item || !(item->settings == m_settings) || (settings_apply && !m_pending))
        return false;
    auto ticket = m_store.request();
    if (!m_store.publish(ticket, std::move(item), settings_apply ? PrepareSelectionReason::SettingsApplied : PrepareSelectionReason::Picked))
        return false;
    m_committed_settings = m_settings;
    m_pending            = false;
    return true;
}
void PrepareSelectionSession::done()
{
    cancel_settings();
    m_store.request();
    m_picking = false;
}
bool PrepareSelectionSession::escape()
{
    if (m_pending) {
        cancel_settings();
        return false;
    }
    if (m_store.get().item) {
        clear();
        return false;
    }
    m_picking = false;
    m_store.request();
    return true;
}
void PrepareSelectionSession::clear()
{
    cancel_settings();
    m_store.clear();
}
void PrepareSelectionSession::invalidate()
{
    cancel_settings();
    m_store.clear(PrepareSelectionReason::Invalidated);
}
} // namespace Slic3r::GUI
