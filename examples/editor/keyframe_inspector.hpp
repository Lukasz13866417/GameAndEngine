#pragma once
#include "timeline_panel.hpp"

namespace editor_example {
class KeyframeInspector final {
public:
    explicit KeyframeInspector(vng::ui::Container);
    ~KeyframeInspector();
    void show(std::span<const AnimationProperty>, const vng::timeline::Timeline&,
              std::optional<vng::f32> time, bool reset = false);
    [[nodiscard]] std::optional<TimelineAction> poll();
    void error(std::string_view);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace editor_example
