#pragma once
#include <atomic>
#include <cstdint>
#include <memory>

namespace Slic3r {

// Transient lifetime/revision witness, deliberately absent from model serialization.
// Copies of a ModelVolume (including slicing snapshots) get independent witnesses.
class GeometryRevision
{
    struct State
    {
        std::atomic<std::uint64_t> revision{1};
        std::atomic<bool> alive{true};
    };

public:
    class Stamp
    {
    public:
        bool valid() const { return m_state && m_state->alive.load() && m_state->revision.load() == m_revision; }
        std::uint64_t revision() const { return m_revision; }
        bool same_source(const Stamp& other) const { return m_state == other.m_state && m_revision == other.m_revision; }

    private:
        friend class GeometryRevision;
        std::shared_ptr<const State> m_state;
        std::uint64_t m_revision = 0;
    };
    GeometryRevision() = default;
    GeometryRevision(const GeometryRevision&) {} // a copy is a new model-volume lifetime
    GeometryRevision& operator=(const GeometryRevision&)
    {
        touch();
        return *this;
    }
    ~GeometryRevision() { m_state->alive.store(false); }
    void touch() { ++m_state->revision; }
    Stamp stamp() const
    {
        Stamp result;
        result.m_state    = m_state;
        result.m_revision = m_state->revision.load();
        return result;
    }

private:
    std::shared_ptr<State> m_state = std::make_shared<State>();
};
} // namespace Slic3r
