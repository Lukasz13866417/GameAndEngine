#pragma once
#include <vng/editor/selection.hpp>
#include <vng/input/input.hpp>
#include <vng/ui/draw_list.hpp>
namespace editor_example {
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
