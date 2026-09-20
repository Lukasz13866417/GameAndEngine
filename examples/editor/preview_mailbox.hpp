#pragma once

#include <vng/editor/preview.hpp>
#include <optional>
#include <utility>

namespace editor_example {
// Pixels and authored-state acknowledgements arrive on independent transports.
// Keep at most one completed image until its revision is known to the UI.
// A paused worker need not render again just because its image arrived first.
class PreviewMailbox {
public:
    void clear() noexcept { next_.reset(); }

    void offer(vng::editor::preview::PreviewFrame frame, vng::u64 active_generation) {
        if (frame.info.generation != active_generation) return;
        if (!next_ || next_->info.generation != active_generation ||
            frame.info.frame_id > next_->info.frame_id)
            next_ = std::move(frame);
    }

    [[nodiscard]] std::optional<vng::editor::preview::PreviewFrame>
    take(vng::u64 active_generation, vng::u64 minimum_revision,
         vng::u64 displayed_revision, vng::u64 authored_revision) {
        if (!next_) return {};
        const auto& info = next_->info;
        if (info.generation != active_generation || info.scene_revision < minimum_revision ||
            info.scene_revision < displayed_revision) {
            next_.reset();
            return {};
        }
        if (info.scene_revision > authored_revision) return {}; // Await its state ACK.
        return std::exchange(next_, std::nullopt);
    }
private:
    std::optional<vng::editor::preview::PreviewFrame> next_;
};
} // namespace editor_example
