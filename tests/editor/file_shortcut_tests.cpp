#include "../../examples/editor/file_shortcuts.hpp"
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace vng;
using namespace input;
using editor_example::FileShortcut;
using editor_example::take_file_shortcut;
Event save_key(EventKind kind = EventKind::key_down, Modifiers modifiers = {.control = true},
               bool repeat = false) {
    return {.kind = kind, .key = Key::s, .modifiers = modifiers, .repeat = repeat};
}
void unchanged(const Frame& before, const Frame& after) {
    CHECK(before.logical_size == after.logical_size);
    CHECK(before.framebuffer == after.framebuffer);
    CHECK(before.pointer == after.pointer);
    CHECK(before.focused == after.focused);
    CHECK(before.overflow == after.overflow);
    CHECK(before.keys == after.keys);
    REQUIRE(before.events.size() == after.events.size());
    for (std::size_t i = 0; i < before.events.size(); ++i) {
        const auto& a = before.events[i];
        const auto& b = after.events[i];
        CHECK(a.kind == b.kind);
        CHECK(a.position == b.position);
        CHECK(a.scroll == b.scroll);
        CHECK(a.key == b.key);
        CHECK(a.button == b.button);
        CHECK(a.modifiers.shift == b.modifiers.shift);
        CHECK(a.modifiers.control == b.modifiers.control);
        CHECK(a.modifiers.alt == b.modifiers.alt);
        CHECK(a.modifiers.super == b.modifiers.super);
        CHECK(a.repeat == b.repeat);
        CHECK(a.text == b.text);
    }
}
} // namespace

TEST_CASE("Global Ctrl S routes Save and Shift Ctrl S routes Save As before UI key handling",
          "[editor][input]") {
    for (const bool shift : {false, true}) {
        const Modifiers modifiers{.shift = shift, .control = true};
        Frame frame{.logical_size = {900, 600}, .framebuffer = {1800, 1200}, .pointer = {250, 300}};
        frame.keys[static_cast<std::size_t>(Key::s)] = true;
        frame.events = {{.kind = EventKind::text, .text = "keep this"},
                        save_key(EventKind::key_down, modifiers),
                        save_key(EventKind::key_up, modifiers),
                        {.kind = EventKind::key_down, .key = Key::enter},
                        {.kind = EventKind::pointer_move, .position = {252, 300}}};
        const auto original = frame;
        CHECK(take_file_shortcut(frame, true) ==
              (shift ? FileShortcut::save_as : FileShortcut::save));
        auto expected = original;
        expected.events = {original.events[0], original.events[3], original.events[4]};
        unchanged(expected, frame);
        CHECK(original.events.size() == 5);
    }
}

TEST_CASE("File shortcuts consume repeat and release edges without requesting more saves",
          "[editor][input]") {
    Frame frame{.events = {save_key(), save_key(EventKind::key_down, {.control = true}, true),
                           save_key(EventKind::key_up)}};
    CHECK(take_file_shortcut(frame, true) == FileShortcut::save);
    CHECK(frame.events.empty());
    frame.events = {save_key(EventKind::key_down, {.control = true}, true),
                    save_key(EventKind::key_up)};
    CHECK(take_file_shortcut(frame, true) == FileShortcut::none);
    CHECK(frame.events.empty());
    frame.events = {save_key(EventKind::key_down, {.shift = true, .control = true}, true),
                    save_key(EventKind::key_up, {.shift = true, .control = true})};
    CHECK(take_file_shortcut(frame, true) == FileShortcut::none);
    CHECK(frame.events.empty());
    frame.events = {save_key(), save_key(EventKind::key_up),
                    save_key(EventKind::key_down, {.shift = true, .control = true})};
    CHECK(take_file_shortcut(frame, true) == FileShortcut::save_as);
    CHECK(frame.events.empty());
    frame.events = {save_key(EventKind::key_down, {.shift = true, .control = true}), save_key(),
                    save_key(),
                    save_key(EventKind::key_down, {.shift = true, .control = true}, true)};
    CHECK(take_file_shortcut(frame, true) == FileShortcut::save);
    CHECK(frame.events.empty());
}

TEST_CASE("Unmodified S and alternate modifier chords remain available to UI handling",
          "[editor][input]") {
    for (const auto modifiers :
         {Modifiers{}, Modifiers{.shift = true}, Modifiers{.control = true, .alt = true},
          Modifiers{.control = true, .super = true},
          Modifiers{.shift = true, .control = true, .alt = true},
          Modifiers{.shift = true, .control = true, .super = true}}) {
        Frame frame{.events = {save_key(EventKind::key_down, modifiers),
                               save_key(EventKind::key_up, modifiers)}};
        const auto original = frame;
        CHECK(take_file_shortcut(frame, true) == FileShortcut::none);
        unchanged(original, frame);
    }
    // Text is a separate committed input stream, never reconstructed from S.
    Frame text{
        .events = {
            {.kind = EventKind::text, .key = Key::s, .modifiers = {.control = true}, .text = "s"}}};
    const auto original = text;
    CHECK(take_file_shortcut(text, true) == FileShortcut::none);
    unchanged(original, text);
}

TEST_CASE("Disabled file shortcuts and uncertain focus preserve the complete input frame",
          "[editor][input]") {
    Frame frame{.events = {save_key(), save_key(EventKind::key_up)}};
    const auto original = frame;
    CHECK(take_file_shortcut(frame, false) == FileShortcut::none);
    unchanged(original, frame);
    for (const bool focused : {false, true}) {
        Frame uncertain{.focused = focused,
                        .overflow = focused,
                        .events = {save_key(), save_key(EventKind::key_up)}};
        const auto before = uncertain;
        CHECK(take_file_shortcut(uncertain, true) == FileShortcut::none);
        unchanged(before, uncertain);
    }
    Frame regained{
        .events = {{.kind = EventKind::focus_lost}, save_key(), {.kind = EventKind::focus_gained}}};
    const auto before = regained;
    CHECK(take_file_shortcut(regained, true) == FileShortcut::none);
    unchanged(before, regained);
}

TEST_CASE("Global file shortcut remains eligible across same-batch text field focus acquisition",
          "[editor][input]") {
    Frame frame{.events = {{.kind = EventKind::pointer_down, .position = {800, 500}, .button = 0},
                           save_key()}};
    CHECK(take_file_shortcut(frame, true) == FileShortcut::save);
    REQUIRE(frame.events.size() == 1);
    CHECK(frame.events[0].kind == EventKind::pointer_down);
}
