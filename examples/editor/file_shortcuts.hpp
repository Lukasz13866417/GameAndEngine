#pragma once

#include <algorithm>
#include <vng/input/input.hpp>

namespace editor_example {
enum class FileShortcut { none, save, save_as };

// Global Linux editor shortcuts, routed before UI keyboard handling even when
// a text field owns focus. Work on a copy: ordinary text and unrelated input
// remain intact, while Ctrl+S edges/repeats cannot also reach UI key handling.
// Only non-repeat downs request an action; the last request in a batch wins.
[[nodiscard]] inline FileShortcut take_file_shortcut(vng::input::Frame& frame, bool enabled) {
    using namespace vng::input;
    if (!enabled || !frame.focused || frame.overflow ||
        std::ranges::any_of(frame.events,
                            [](const auto& event) { return event.kind == EventKind::focus_lost; }))
        return FileShortcut::none;
    auto requested = FileShortcut::none;
    std::erase_if(frame.events, [&](const auto& event) {
        const auto& modifiers = event.modifiers;
        const bool save = event.key == Key::s &&
                          (event.kind == EventKind::key_down || event.kind == EventKind::key_up) &&
                          modifiers.control && !modifiers.alt && !modifiers.super;
        if (save && event.kind == EventKind::key_down && !event.repeat)
            requested = modifiers.shift ? FileShortcut::save_as : FileShortcut::save;
        return save;
    });
    return requested;
}
} // namespace editor_example
