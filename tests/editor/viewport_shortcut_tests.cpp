#include "../../examples/editor/viewport_shortcuts.hpp"
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace vng;
using namespace input;
using editor_example::take_viewport_tab;
using editor_example::take_gizmo_cycle;
Event tab(EventKind kind = EventKind::key_down, Modifiers modifiers = {}, bool repeat = false) {
    return {.kind = kind, .key = Key::tab, .modifiers = modifiers, .repeat = repeat};
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

TEST_CASE("Gizmo cycling consumes Ctrl arrows but preserves text-navigation ownership", "[editor][input][gizmo]") {
    const Event right{.kind=EventKind::key_down,.key=Key::right,.modifiers={.control=true}};
    auto left=right;left.key=Key::left;
    auto release=right;release.kind=EventKind::key_up;
    auto repeat=right;repeat.repeat=true;
    Frame frame{.events={right,repeat,release,left,right}};
    CHECK(take_gizmo_cycle(frame,true)==1);
    CHECK(frame.events.empty());
    frame.events={left};CHECK(take_gizmo_cycle(frame,true)==-1);
    frame.events={right};const auto original=frame;
    CHECK(take_gizmo_cycle(frame,false)==0);unchanged(original,frame);
    for(auto modifiers:{Modifiers{},Modifiers{.shift=true,.control=true},
                        Modifiers{.control=true,.alt=true},Modifiers{.control=true,.super=true}}) {
        auto event=right;event.modifiers=modifiers;frame.events={event};
        const auto before=frame;CHECK(take_gizmo_cycle(frame,true)==0);unchanged(before,frame);
    }
    for(auto kind:{EventKind::pointer_down,EventKind::focus_lost}) {
        frame.events={right,{.kind=kind,.button=0}};const auto before=frame;
        CHECK(take_gizmo_cycle(frame,true)==0);unchanged(before,frame);
    }
    frame.events={right};frame.focused=false;CHECK(take_gizmo_cycle(frame,true)==0);
    frame.focused=true;frame.overflow=true;CHECK(take_gizmo_cycle(frame,true)==0);
}

TEST_CASE("Viewport Tab is consumed before UI focus traversal without touching unrelated input",
          "[editor][input]") {
    Frame frame{.logical_size = {900, 600}, .framebuffer = {1800, 1200}, .pointer = {300, 200}};
    frame.keys[static_cast<std::size_t>(Key::tab)] = true;
    frame.events = {{.kind = EventKind::pointer_move, .position = {301, 200}},
                    tab(),
                    tab(EventKind::key_up),
                    {.kind = EventKind::text, .text = "text"}};
    const auto original = frame;
    CHECK(take_viewport_tab(frame, true));
    REQUIRE(frame.events.size() == 2);
    auto expected = original;
    expected.events = {original.events.front(), original.events.back()};
    unchanged(expected, frame);
    CHECK(original.events.size() == 4); // caller can retain the original raw frame
}

TEST_CASE("Ineligible viewport Tab remains available to ordinary UI keyboard handling",
          "[editor][input]") {
    Frame frame{.events = {tab(), tab(EventKind::key_up)}};
    const auto original = frame;
    CHECK_FALSE(take_viewport_tab(frame, false));
    unchanged(original, frame);
    for (const auto modifiers :
         {Modifiers{.shift = true}, Modifiers{.control = true}, Modifiers{.alt = true},
          Modifiers{.super = true},
          Modifiers{.shift = true, .control = true, .alt = true, .super = true}}) {
        frame.events = {tab(EventKind::key_down, modifiers), tab(EventKind::key_up, modifiers)};
        const auto before = frame;
        CHECK_FALSE(take_viewport_tab(frame, true));
        unchanged(before, frame);
    }
}

TEST_CASE("Viewport Tab handles key repeat and paired edges without repeated toggles",
          "[editor][input]") {
    Frame frame{.events = {tab(), tab(EventKind::key_down, {}, true),
                           tab(EventKind::key_down, {}, true), tab(EventKind::key_up)}};
    CHECK(take_viewport_tab(frame, true));
    CHECK(frame.events.empty());
    frame.events = {tab(EventKind::key_down, {}, true), tab(EventKind::key_up)};
    CHECK_FALSE(take_viewport_tab(frame, true));
    CHECK(frame.events.empty());
    frame.events = {tab(), tab(EventKind::key_up), tab(), tab(EventKind::key_up)};
    CHECK_FALSE(take_viewport_tab(frame, true));
    CHECK(frame.events.empty());
    frame.events = {tab(), tab(EventKind::key_up), tab(), tab(EventKind::key_up), tab()};
    CHECK(take_viewport_tab(frame, true));
    CHECK(frame.events.empty());
}

TEST_CASE("Viewport Tab does not steal same-batch focus acquisition or uncertain focus state",
          "[editor][input]") {
    for (const auto kind : {EventKind::pointer_down, EventKind::focus_lost}) {
        Frame frame{.events = {tab(), {.kind = kind, .button = 0}, tab(EventKind::key_up)}};
        const auto before = frame;
        CHECK_FALSE(take_viewport_tab(frame, true));
        unchanged(before, frame);
    }
    Frame unfocused{.focused = false, .events = {tab()}};
    const auto before_focus = unfocused;
    CHECK_FALSE(take_viewport_tab(unfocused, true));
    unchanged(before_focus, unfocused);
    Frame overflow{.overflow = true, .events = {tab()}};
    const auto before_overflow = overflow;
    CHECK_FALSE(take_viewport_tab(overflow, true));
    unchanged(before_overflow, overflow);
    // Final focus may be regained; the batch still crosses an ownership boundary.
    Frame regained{
        .events = {{.kind = EventKind::focus_lost}, {.kind = EventKind::focus_gained}, tab()}};
    const auto before_regained = regained;
    CHECK_FALSE(take_viewport_tab(regained, true));
    unchanged(before_regained, regained);
}
