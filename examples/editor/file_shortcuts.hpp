#pragma once

#include <algorithm>
#include <vng/input/input.hpp>

namespace editor_example {
enum class FileShortcut { none, save, save_as, open };

// Global Linux editor shortcuts, routed before UI keyboard handling even when
// a text field owns focus. Work on a copy: ordinary text and unrelated input
// remain intact, while Ctrl+S / Ctrl+O edges/repeats cannot also reach UI key
// handling. Only non-repeat downs request an action; the last request in a
// batch wins. Ctrl+Shift+O is not a file shortcut and stays available to UI.
[[nodiscard]] inline FileShortcut take_file_shortcut(vng::input::Frame& frame, bool enabled) {
    using namespace vng::input;
    if (!enabled || !frame.focused || frame.overflow ||
        std::ranges::any_of(frame.events,
                            [](const auto& event) { return event.kind == EventKind::focus_lost; }))
        return FileShortcut::none;
    auto requested = FileShortcut::none;
    std::erase_if(frame.events, [&](const auto& event) {
        const auto& modifiers = event.modifiers;
        const bool edge = event.kind == EventKind::key_down || event.kind == EventKind::key_up;
        const bool chord = edge && modifiers.control && !modifiers.alt && !modifiers.super;
        const bool save = chord && event.key == Key::s;
        const bool open = chord && event.key == Key::o && !modifiers.shift;
        if (event.kind == EventKind::key_down && !event.repeat) {
            if (save)
                requested = modifiers.shift ? FileShortcut::save_as : FileShortcut::save;
            else if (open)
                requested = FileShortcut::open;
        }
        return save || open;
    });
    return requested;
}
} // namespace editor_example
