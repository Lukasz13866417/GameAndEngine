#pragma once
#include <vng/editor/selection.hpp>
#include <vng/input/input.hpp>
#include <vng/input/routing.hpp>
#include <vng/ui/draw_list.hpp>
#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>
namespace editor_example {
// Selection normally starts immediately. With a focused gizmo handle, wait for
// an outside click to complete before changing the selected object/components.
// Crossing the box-selection threshold still starts a drag from the original
// press; a release over UI or a lost focus never invents a viewport click.
class SelectionInput {
public:
    void cancel() { press_.reset(); }
    std::span<const vng::input::Event> route(std::span<const vng::input::Event> raw,
        std::span<const vng::input::Event> unhandled,vng::ui::Rect viewport,bool defer,bool enabled) {
        using namespace vng;
        events_.clear();available_.reset(unhandled);
        if(!enabled)press_.reset();
        const auto flush=[&] {events_.push_back(*press_);available_.include(*press_);press_.reset();};
        for(const auto& e:raw) {
            if(e.kind==input::EventKind::focus_lost ||
               (e.kind==input::EventKind::key_down && e.key==input::Key::escape))press_.reset();
            if(enabled && defer && e.kind==input::EventKind::pointer_down && e.button==0 &&
               viewport.contains(e.position) && available(e)) {press_=e;continue;}
            if(press_ && e.kind==input::EventKind::pointer_move &&
               std::hypot(e.position.x-press_->position.x,e.position.y-press_->position.y)>=5)flush();
            if(press_ && e.kind==input::EventKind::pointer_up && e.button==0) {
                if(available(e) && viewport.contains(e.position))flush();else press_.reset();
            }
            events_.push_back(e);
        }
        return events_;
    }
    bool available(const vng::input::Event& e) const {
        return available_.contains(e);
    }
private:
    std::optional<vng::input::Event> press_;
    std::vector<vng::input::Event> events_;
    vng::input::AvailableEvents available_{{}};
};
inline vng::editor::SelectionMode click_selection(vng::input::Modifiers m,bool list=false) {
    using Mode=vng::editor::SelectionMode;
    return m.control?Mode::toggle:m.shift?(list?Mode::range:Mode::add):Mode::replace;
}
inline vng::editor::SelectionMode box_selection(vng::input::Modifiers m) {
    using Mode=vng::editor::SelectionMode;
    return m.control?Mode::remove:m.shift?Mode::add:Mode::replace;
}
inline vng::input::Modifiers click_modifiers(std::span<const vng::input::Event> events,vng::ui::Rect bounds) {
    for(auto i=events.rbegin();i!=events.rend();++i)
        if(i->kind==vng::input::EventKind::pointer_up && i->button==0 && bounds.contains(i->position)) return i->modifiers;
    return {};
}
}
