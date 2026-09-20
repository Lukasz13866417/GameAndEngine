#pragma once
#include <vng/input/input.hpp>
#include <optional>

namespace editor_example {
// Unmodified arrows belong to viewport tools; Ctrl+arrows remain gizmo cycling.
inline std::optional<vng::Vec2> transform_arrow(const vng::input::Event& event, float amount = 1.F) {
    using namespace vng;
    if(event.kind!=input::EventKind::key_down || event.modifiers.control ||
       event.modifiers.alt || event.modifiers.super)return {};
    const float step=amount*(event.modifiers.shift?.1F:1.F);
    switch(event.key) {
    case input::Key::left:return Vec2{-step,0};
    case input::Key::right:return Vec2{step,0};
    case input::Key::up:return Vec2{0,-step};
    case input::Key::down:return Vec2{0,step};
    default:return {};
    }
}
inline float rotation_arrow(vng::Vec2 arrow) {return -arrow.x-arrow.y;}
}
