#pragma once

#include <algorithm>
#include <vng/input/input.hpp>

namespace editor_example {
// Control is side-independent (left or right Ctrl). Do not steal text-editing
// word navigation or keys arriving in the same batch as a focus-changing click.
[[nodiscard]] inline int take_gizmo_cycle(vng::input::Frame& frame, bool enabled) {
    using namespace vng::input;
    if (!enabled || !frame.focused || frame.overflow ||
        std::ranges::any_of(frame.events, [](const auto& event) {
            return (event.kind == EventKind::pointer_down && event.button == 0) ||
                   event.kind == EventKind::focus_lost;
        })) return 0;
    int steps{};
    std::erase_if(frame.events, [&](const auto& event) {
        const auto& m = event.modifiers;
        const bool handled = (event.key == Key::left || event.key == Key::right) &&
            (event.kind == EventKind::key_down || event.kind == EventKind::key_up) &&
            m.control && !m.shift && !m.alt && !m.super;
        if (handled && event.kind == EventKind::key_down && !event.repeat)
            steps += event.key == Key::right ? 1 : -1;
        return handled;
    });
    return steps;
}
// Route an unmodified Tab to the viewport before UI focus traversal sees it.
// The caller supplies viewport eligibility and prior UI keyboard ownership.
// Work on a copy of the input frame: other tools still need the original events.
// All handled Tab down/up events are removed, but only non-repeat downs toggle.
[[nodiscard]] inline bool take_viewport_tab(vng::input::Frame& frame, bool enabled) {
    using namespace vng::input;
    if (!enabled || !frame.focused || frame.overflow ||
        std::ranges::any_of(frame.events, [](const auto& event) {
            // A same-batch click can acquire a text field or popup's focus;
            // previous-frame keyboard ownership cannot safely describe it.
            return (event.kind == EventKind::pointer_down && event.button == 0) ||
                   event.kind == EventKind::focus_lost;
        }))
        return false;
    bool toggle{};
    std::erase_if(frame.events, [&](const auto& event) {
        const auto& modifiers = event.modifiers;
        const bool tab = event.key == Key::tab &&
                         (event.kind == EventKind::key_down || event.kind == EventKind::key_up) &&
                         !modifiers.shift && !modifiers.control && !modifiers.alt &&
                         !modifiers.super;
        if (tab && event.kind == EventKind::key_down && !event.repeat)
            toggle = !toggle;
        return tab;
    });
    return toggle;
}
} // namespace editor_example
