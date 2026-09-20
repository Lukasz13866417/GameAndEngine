#pragma once

#include "project.hpp"

#include <vng/core/monotonic_clock.hpp>
#include <vng/editor/preview.hpp>
#include <vng/input/input.hpp>
#include <vng/ui/draw_list.hpp>
#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <ostream>
#include <span>
#include <string_view>
#include <utility>

namespace editor_example {
// Opt-in receipt/routing/presentation evidence, independent of camera behavior.
// No files, formatting or frame history are produced when disabled.
class ScrollTrace {
public:
    explicit ScrollTrace(std::ostream* output = nullptr) : output_(output) {}
    static bool requested() {
        const auto* value = std::getenv("VNG_TRACE_SCROLL");
        return value && std::string_view(value) == "1";
    }
    [[nodiscard]] bool enabled() const { return output_ != nullptr; }

    void input(std::span<const vng::input::Event> raw,
               std::span<const vng::input::Event> unhandled, vng::ui::Rect viewport,
               bool dragging, const CameraPose& before, const CameraPose& target, vng::u64 sequence,
               std::initializer_list<std::pair<std::string_view, bool>> blockers = {}) {
        if (!output_) return;
        const bool blocked = std::ranges::any_of(blockers, [](const auto& b) { return b.second; });
        bool routed = false;
        for (const auto& event : raw) {
            if (event.kind != vng::input::EventKind::scroll) continue;
            const bool available = std::ranges::any_of(unhandled, [&](const auto& other) {
                return other.kind == event.kind && other.position == event.position &&
                       other.scroll == event.scroll && other.received_ns == event.received_ns;
            });
            const bool inside = viewport.contains(event.position);
            const auto reason = !inside ? "outside-image" : !available ? "consumed-by-ui" :
                blocked ? "blocked" : dragging ? "pointer-drag-active" :
                event.scroll.y == 0 ? "no-vertical-delta" :
                target == before ? "target-unchanged" : "target-updated";
            *output_ << "[scroll/ui] event=" << ++events_ << " received_ns=" << event.received_ns
                     << " dy=" << event.scroll.y << " at=" << event.position.x << ',' << event.position.y
                     << " reason=" << reason << " zoom=" << before.zoom << "->" << target.zoom
                     << " target=" << target.target.x << ',' << target.target.y << ',' << target.target.z
                     << " view=" << sequence;
            for (const auto& [name, active] : blockers)
                if (active) *output_ << " block=" << name;
            *output_ << '\n';
            routed = routed || (inside && available && !blocked && !dragging && target != before);
        }
        if (routed) {
            pending_ = sequence;
            first_ = false;
            requested_ns_ = vng::monotonic_ns();
        }
    }

    void submitted(vng::u64 generation, vng::u64 sequence, bool queued) {
        if (output_ && pending_ && sequence >= pending_)
            *output_ << "[scroll/transport] generation=" << generation << " view=" << sequence
                     << (queued ? " queued\n" : " failed\n");
    }
    // Called after presentation, not just on receipt of an offscreen image.
    void presented(const vng::editor::preview::FrameInfo& frame) {
        if (!output_ || !pending_ || !frame.has_view || frame.view_sequence < pending_ ||
            (frame.generation == generation_ && frame.frame_id == frame_)) return;
        generation_ = frame.generation;
        frame_ = frame.frame_id;
        if (!first_ || frame.settled) {
            *output_ << "[scroll/present] generation=" << frame.generation << " frame=" << frame.frame_id
                     << " view=" << frame.view_sequence << " distance=" << frame.view_camera[2]
                     << " settled=" << frame.settled << " zoom=" << frame.view_camera[6]
                     << " target=" << frame.view_camera[3] << ',' << frame.view_camera[4] << ',' << frame.view_camera[5]
                     << " since_ui_ms="
                     << double(vng::monotonic_ns() - requested_ns_) / 1e6 << '\n';
            first_ = true;
        }
        if (frame.settled) pending_ = 0;
    }
private:
    std::ostream* output_{};
    vng::u64 events_{}, pending_{}, requested_ns_{}, generation_{}, frame_{};
    bool first_{};
};
} // namespace editor_example
