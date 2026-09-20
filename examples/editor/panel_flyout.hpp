#pragma once
#include <vng/ui/ui.hpp>
#include <algorithm>

namespace editor_example {
// A second, retained UI view, not a native window and not a new document owner.
class PanelFlyout {
public:
    PanelFlyout(vng::ui::Container host, std::string_view title, std::string_view close_text)
        : host_(host) {
        host_.padding(12).gap(6).visible(false);
        host_.label(title).height(28);
        close_ = host_.button(close_text).height(32);
        body_ = host_.column().padding(0).gap(4).scrollbar(vng::ui::ScrollBar::always);
    }
    vng::ui::Container body() const { return body_; }
    bool opened() const { return opened_; }
    bool contains(vng::Vec2 point) const { return opened_ && host_.bounds().contains(point); }
    void open() { opened_ = true; host_.visible(true); }
    void close() { opened_ = false; host_.visible(false); }
    void enabled(bool value) { host_.enabled(value); }
    void layout(vng::Vec2 size, vng::ui::Rect anchor) {
        const auto width = std::min(500.F, std::max(100.F, size.x - 16));
        const auto y = std::min(anchor.y + anchor.height + 4, std::max(8.F, size.y - 160));
        const auto height = std::max(100.F, std::min(720.F, size.y - y - 12));
        host_.position({std::clamp(anchor.x, 8.F, std::max(8.F, size.x - width - 8)), y})
            .width(width).height(height);
        body_.height(std::max(20.F, height - 96));
    }
    void poll(std::span<const vng::input::Event> events, vng::ui::Rect opener, bool allowed) {
        if (!opened_) return;
        if (!allowed || close_.clicked() || std::ranges::any_of(events, [&](const auto& event) {
            using namespace vng::input;
            return (event.kind == EventKind::key_down && event.key == Key::escape) ||
                (event.kind == EventKind::pointer_down && !host_.bounds().contains(event.position) &&
                 !opener.contains(event.position));
        })) close();
    }
private:
    vng::ui::Container host_, body_;
    vng::ui::Button close_;
    bool opened_{};
};
}
