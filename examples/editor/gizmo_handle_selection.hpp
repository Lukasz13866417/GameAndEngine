#pragma once
#include <vng/input/input.hpp>
#include <vng/ui/draw_list.hpp>
#include <algorithm>
#include <cmath>
#include <optional>
#include <span>

namespace editor_example {
// Handle focus is not pointer capture or a document edit. Only a completed
// viewport click dismisses it; UI presses and camera drags leave it alone.
class GizmoHandleSelection {
public:
    std::optional<vng::u32> axis() const { return axis_; }
    void select(vng::u32 axis) { axis_=axis; press_.reset(); }
    void clear() { axis_.reset(); press_.reset(); }
    bool update(const vng::input::Event& event, std::span<const vng::input::Event> available,
                vng::ui::Rect viewport) {
        using namespace vng;
        const bool unhandled=std::ranges::any_of(available,[&](const auto& e) {
            return e.kind==event.kind && e.button==event.button && e.position==event.position;
        });
        if(event.kind==input::EventKind::focus_lost || event.kind==input::EventKind::scroll)press_.reset();
        if(event.kind==input::EventKind::pointer_down) {
            press_.reset();
            if(event.button<=1 && unhandled && viewport.contains(event.position) && !event.modifiers.control &&
               !event.modifiers.alt && !event.modifiers.super) {press_=event.position;button_=event.button;}
        }
        if(press_ && (event.kind==input::EventKind::pointer_move || event.kind==input::EventKind::pointer_up) &&
           (!std::isfinite(event.position.x) || !std::isfinite(event.position.y) ||
            std::hypot(event.position.x-press_->x,event.position.y-press_->y)>=5))press_.reset();
        if(event.kind==input::EventKind::pointer_up && event.button==button_) {
            const bool clicked=press_.has_value() && unhandled && viewport.contains(event.position);
            press_.reset();
            if(clicked)axis_.reset();
            return clicked;
        }
        return false;
    }
private:
    std::optional<vng::u32> axis_;
    std::optional<vng::Vec2> press_;
    vng::u32 button_{};
};
}
