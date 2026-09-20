#pragma once

#include <vng/ui/draw_list.hpp>
#include <vng/editor/preview.hpp>
#include <array>
#include <charconv>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace editor_example {
// UI presentation rate and a rolling worker-frame cost, not GPU/scanout timing.
// Only distinct presented preview images contribute worker duration samples.
class PreviewFps {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    enum class Activity { waiting, updating, idle, frozen };

    static constexpr std::size_t frame_window = 20;

    void presented(const vng::editor::preview::FrameInfo& info, Time now) {
        ui_.presented(now);
        if (!info.generation || !info.frame_id || info.generation < generation_) return;
        if (info.generation != generation_) {
            generation_ = info.generation;
            frame_ = 0;
            durations_ = {};
            sample_count_ = next_sample_ = 0;
            total_ms_ = 0;
            preview_label_dirty_ = true;
        }
        if (info.frame_id <= frame_) return;
        frame_ = info.frame_id;
        const auto& trace = info.interaction;
        if (!trace.render_started_ns || trace.readback_ready_ns <= trace.render_started_ns) return;
        const auto ms = static_cast<double>(trace.readback_ready_ns - trace.render_started_ns) / 1e6;
        total_ms_ += ms - durations_[next_sample_];
        durations_[next_sample_] = ms;
        next_sample_ = (next_sample_ + 1) % frame_window;
        if (sample_count_ < frame_window) ++sample_count_;
        preview_label_dirty_ = true;
    }

    // Keep duration history across pauses: no inter-frame/idle gaps are sampled.
    // Activity comes from scheduling state, not from guessing that a slow worker
    // is idle because no images arrived. UI FPS retains its timed sampling window.
    void update(Time now, Activity activity) {
        preview_label_dirty_ |= activity_ != activity;
        activity_ = activity;
        if (ui_.update(now)) ui_label_ = "UI: " + number(*ui_.rate) + " FPS";
        if (!preview_label_dirty_) return;
        preview_label_dirty_ = false;
        if (sample_count_) {
            preview_label_ = "Preview: " + number(total_ms_ / static_cast<double>(sample_count_)) + " ms";
            if (activity == Activity::idle) preview_label_ += " (idle)";
            else if (activity == Activity::frozen) preview_label_ += " (frozen)";
            else if (activity == Activity::waiting) preview_label_ += " (waiting)";
            return;
        }
        switch (activity) {
        case Activity::waiting: preview_label_ = "Preview: waiting"; break;
        case Activity::idle: preview_label_ = "Preview: idle"; break;
        case Activity::frozen: preview_label_ = "Preview: frozen"; break;
        case Activity::updating:
            preview_label_ = "Preview: measuring...";
            break;
        }
    }

    [[nodiscard]] std::string_view ui_label() const { return ui_label_; }
    [[nodiscard]] std::string_view preview_label() const { return preview_label_; }

    // Draw-only annotation: never captures pointer input or dirties the document.
    void append(vng::ui::DrawList& list, const vng::text::Font& font,
                vng::ui::Rect viewport) const {
        if (viewport.width <= 16 || viewport.height <= 16) return;
        const vng::ui::Rect box{viewport.x + 8, viewport.y + 8, 236, 44};
        list.commands.emplace_back(vng::ui::BoxDraw{box, viewport,
            {.015F, .02F, .03F, .88F}, {}, 4, 0});
        list.commands.emplace_back(vng::ui::TextDraw{ui_label_, {box.x + 6, box.y + 3},
            viewport, font, 14, {.92F, .96F, 1.F, 1.F}});
        list.commands.emplace_back(vng::ui::TextDraw{preview_label_, {box.x + 6, box.y + 23},
            viewport, font, 14, {.92F, .96F, 1.F, 1.F}});
    }

private:
    struct Rate {
        std::optional<Time> started;
        std::optional<double> rate;
        vng::u64 count{};
        void presented(Time now) {
            if (!started) started = now;
            else ++count;
        }
        bool update(Time now) {
            if (!started || now - *started < std::chrono::milliseconds(500)) return false;
            rate = static_cast<double>(count) / std::chrono::duration<double>(now - *started).count();
            count = 0;
            started = now;
            return true;
        }
    };
    static std::string number(double rate) {
        char digits[32];
        const auto converted = std::to_chars(digits, digits + sizeof digits, rate, std::chars_format::fixed, 1);
        return {digits, converted.ptr};
    }
    Rate ui_;
    std::array<double, frame_window> durations_{};
    std::size_t sample_count_{}, next_sample_{};
    double total_ms_{};
    Activity activity_{Activity::waiting};
    bool preview_label_dirty_{};
    vng::u64 generation_{}, frame_{};
    std::string ui_label_{"UI: measuring..."}, preview_label_{"Preview: waiting"};
};
} // namespace editor_example
