#pragma once
#include "project.hpp"
#include <vng/editor/interaction_timing.hpp>
#include <vng/ui/ui.hpp>

namespace editor_example {
// Example UI only: the bounded recorder/trace protocol has no widget or GL dependency.
class TimingPanel {
public:
    explicit TimingPanel(vng::ui::Container host) : host_(host) {
        auto row=host_.row().padding(0).gap(8);
        capture_=row.checkbox("Capture timing").value(true).width(180);
        clear_=row.button("Clear").width(90);
        export_=row.button("Export JSON").width(150);
        text_=host_.text_area().height(250);
        host_.visible(false);
    }
    void visible(bool value) { visible_=value; host_.visible(value); }
    [[nodiscard]] bool visible() const { return visible_; }
    void fit_height(float panel_height) { text_.height(std::max(80.0F,panel_height-152.0F)); }
    std::optional<std::string> update(vng::editor::InteractionTimings& timings) {
        if (!visible_) return {};
        timings.enabled(capture_.value());
        if (clear_.clicked()) timings.clear();
        const auto now=vng::monotonic_ns();
        if (now>=refresh_at_) {
            text_.value(timings.summary());
            refresh_at_=now+100'000'000;
        }
        if (export_.clicked()) {
            auto path=std::filesystem::path{"interaction-timing-" + std::to_string(now) + ".json"};
            auto result=save_new(path,timings.json());
            return result ? "Exported " + std::filesystem::absolute(path).string() : result.error().message;
        }
        return {};
    }
private:
    vng::ui::Container host_;
    vng::ui::Checkbox capture_;
    vng::ui::Button clear_,export_;
    vng::ui::TextField text_;
    bool visible_{};
    vng::u64 refresh_at_{};
};
} // namespace editor_example
