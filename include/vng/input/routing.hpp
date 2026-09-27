#pragma once

#include <vng/input/input.hpp>
#include <algorithm>
#include <span>
#include <unordered_set>
#include <vector>

namespace vng::input {
// Owned by the window/input coordinator, not by a child tool. Call before
// copying input to UI surfaces. One sequence can identify multiple windows.
class EventSequence {
public:
    void identify(std::span<Event> events) {
        for (auto& event : events) {
            if (++next_ == 0) ++next_;
            event.routing_id = next_;
        }
    }
private:
    u64 next_{};
};

// A synchronous membership snapshot of which occurrences a preceding child
// left unhandled. No global bus, owning callbacks or search for sibling nodes.
// Production events have identity, so membership is expected constant time.
class AvailableEvents {
public:
    explicit AvailableEvents(std::span<const Event> events) { reset(events); }
    void reset(std::span<const Event> events) {
        ids_.clear(); anonymous_.clear();
        ids_.reserve(events.size());
        for (const auto& event : events) {
            if (event.routing_id) ids_.insert(event.routing_id);
            else anonymous_.push_back(event);
        }
    }
    [[nodiscard]] bool contains(const Event& event) const {
        if (event.routing_id) return ids_.contains(event.routing_id);
        // Headless/standalone callers may still construct untagged input. Do not
        // use this path for routed application events: identical occurrences
        // can only be distinguished once the parent has assigned identity.
        return std::ranges::find(anonymous_, event) != anonymous_.end();
    }
    void include(const Event& event) {
        if (event.routing_id) ids_.insert(event.routing_id);
        else anonymous_.push_back(event);
    }
private:
    std::unordered_set<u64> ids_;
    std::vector<Event> anonymous_;
};
} // namespace vng::input
