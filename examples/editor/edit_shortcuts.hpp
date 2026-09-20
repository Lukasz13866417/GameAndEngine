#pragma once
#include <vng/input/input.hpp>
#include <algorithm>
#include <span>
#include <vector>

namespace editor_example {
enum class EditShortcut { none, copy, paste, undo, redo };
// Text editing gets first refusal. Only UI-unhandled, non-repeating key edges
// reach the document; never apply a shortcut to a same-batch new selection.
// Preserve the whole ordered burst: a quick Ctrl+C, Ctrl+V can share a UI tick.
[[nodiscard]] inline std::vector<EditShortcut> edit_shortcuts(std::span<const vng::input::Event> unhandled,
                                                std::span<const vng::input::Event> raw, bool enabled) {
    using namespace vng::input;
    if(!enabled || std::ranges::any_of(raw,[](const auto& e) {
        return e.kind==EventKind::focus_lost || e.kind==EventKind::pointer_down;
    })) return {};
    std::vector<EditShortcut> result;
    for(const auto& e:unhandled) {
        const auto m=e.modifiers;
        if(e.kind!=EventKind::key_down || e.repeat || !m.control || m.alt || m.super) continue;
        if(e.key==Key::z) result.push_back(m.shift ? EditShortcut::redo : EditShortcut::undo);
        if(e.key==Key::y && !m.shift) result.push_back(EditShortcut::redo);
        if(e.key==Key::c && !m.shift) result.push_back(EditShortcut::copy);
        if(e.key==Key::v && !m.shift) result.push_back(EditShortcut::paste);
    }
    return result;
}
}
