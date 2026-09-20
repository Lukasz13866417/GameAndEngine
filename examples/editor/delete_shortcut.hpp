#pragma once
#include <vng/input/input.hpp>
#include <algorithm>
#include <span>

namespace editor_example {
// Only examine UI-unhandled events: Delete inside a text field edits text.
// A same-batch pointer press may change selection after the key event, so
// conservatively wait for the next distinct Delete press in that case.
[[nodiscard]] inline bool delete_pressed(std::span<const vng::input::Event> unhandled,
                                         std::span<const vng::input::Event> raw,
                                         bool enabled) {
    using namespace vng::input;
    if (!enabled || std::ranges::any_of(raw, [](const auto& event) {
            return event.kind == EventKind::focus_lost ||
                   (event.kind == EventKind::pointer_down && event.button == 0);
        })) return false;
    return std::ranges::any_of(unhandled, [](const auto& event) {
        const auto& m = event.modifiers;
        return event.kind == EventKind::key_down && event.key == Key::del && !event.repeat &&
               !m.control && !m.shift && !m.alt && !m.super;
    });
}
} // namespace editor_example
