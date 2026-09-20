#include <vng/ui/ui.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <set>

namespace {
using namespace vng;
using input::EventKind;
using input::Key;
text::Font font() {
    auto f = text::Font::load(VNG_TEST_FONT_PATH);
    REQUIRE(f);
    return *f;
}
struct Fixture {
    ui::Screen screen{ui::dark_theme(font())};
    ui::Container panel = screen.column().position({20, 20}).width(250).padding(8).gap(6);
    input::Frame input{.logical_size = {800, 600}, .framebuffer = {800, 600}};
    ui::UpdateResult pump(std::initializer_list<input::Event> events = {},
                          input::Clipboard* clipboard = nullptr) {
        input.events = events;
        for (const auto& e : events)
            if (e.kind == EventKind::pointer_move || e.kind == EventKind::pointer_down ||
                e.kind == EventKind::pointer_up)
                input.pointer = e.position;
        auto result = screen.update(input, .016F, clipboard);
        INFO((result ? "updated" : result.error().message));
        REQUIRE(result);
        return *result;
    }
    template <class W> Vec2 center(W w) {
        pump();
        auto r = w.bounds();
        return {r.x + r.width * .5F, r.y + r.height * .5F};
    }
    template <class W> void click(W w) {
        const auto p = center(w);
        pump({mouse(EventKind::pointer_down, p), mouse(EventKind::pointer_up, p)});
    }
    static input::Event mouse(EventKind k, Vec2 p) {
        input::Event e;
        e.kind = k;
        e.position = p;
        return e;
    }
    static input::Event key(Key k, bool control = false, bool shift = false,
                            EventKind kind = EventKind::key_down) {
        input::Event e;
        e.kind = kind;
        e.key = k;
        e.modifiers.control = control;
        e.modifiers.shift = shift;
        return e;
    }
    static input::Event text(std::string s) {
        input::Event e;
        e.kind = EventKind::text;
        e.text = std::move(s);
        return e;
    }
    ui::TextDraw drawn_text(std::string_view value) {
        const auto list = screen.draw_list();
        REQUIRE(list);
        for (const auto& command : list->commands)
            if (const auto* text = std::get_if<ui::TextDraw>(&command);
                text && text->text == value)
                return *text;
        FAIL("Missing UI text: " << value);
        return {};
    }
};
const ui::WidgetSnapshot& widget(const ui::Inspection& inspection, ui::WidgetRole role,
                                std::string_view label) {
    const auto found = std::ranges::find_if(inspection.widgets, [&](const auto& value) {
        return value.role == role && value.label == label;
    });
    INFO("Missing inspected widget: " << label);
    REQUIRE(found != inspection.widgets.end());
    return *found;
}
bool same_rect(ui::Rect a, ui::Rect b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

TEST_CASE("Screens support more than the old 65536 live-widget budget", "[ui][capacity]") {
    Fixture f;
    auto parent = f.panel.column();
    ui::Label last;
    for(std::size_t i=0;i<70000;++i)last=parent.label("value");
    CHECK(last.valid());
    parent.remove();
    CHECK_FALSE(last.valid());
    auto reused=f.panel.label("still usable"); CHECK(reused.valid());
    CHECK(ui::max_widgets == 262144);
}

TEST_CASE("Selected tab buttons remain clickable and expose persistent selection", "[ui][tabs]") {
    Fixture f;
    auto tab=f.panel.button("Scene").selected(true);
    f.click(tab);
    REQUIRE(tab.clicked());
    auto inspection=f.screen.inspect();
    REQUIRE(inspection);
    CHECK(widget(*inspection,ui::WidgetRole::button,"Scene").selected);
    const auto id=widget(*inspection,ui::WidgetRole::button,"Scene").id;
    tab.selected(false);
    f.pump();
    inspection=f.screen.inspect();
    REQUIRE(inspection);
    CHECK_FALSE(widget(*inspection,ui::WidgetRole::button,"Scene").selected);
    CHECK(widget(*inspection,ui::WidgetRole::button,"Scene").id==id);
}
TEST_CASE("Adopted UI groups preserve values and identities under later-created parents", "[ui][layout]") {
    Fixture f;
    auto group=f.panel.column().padding(0);
    auto field=group.text_input("Retained text").value("Draft");
    auto action=group.button("Retained action").selected(true);
    f.pump();
    auto before=f.screen.inspect(); REQUIRE(before);
    const auto id=widget(*before,ui::WidgetRole::button,"Retained action").id;
    auto destination=f.screen.column().position({350,20}).width(250).padding(0);
    destination.adopt(group);
    f.click(action);
    CHECK(action.clicked());
    CHECK(field.getText()=="Draft");
    CHECK(action.bounds().x>=350.F);
    auto after=f.screen.inspect(); REQUIRE(after);
    CHECK(widget(*after,ui::WidgetRole::button,"Retained action").id==id);
    CHECK(widget(*after,ui::WidgetRole::button,"Retained action").selected);
    destination.enabled(false);
    after=f.screen.inspect(); REQUIRE(after);
    CHECK_FALSE(widget(*after,ui::WidgetRole::button,"Retained action").enabled);
    destination.enabled(true).visible(false);
    after=f.screen.inspect(); REQUIRE(after);
    CHECK(widget(*after,ui::WidgetRole::button,"Retained action").enabled);
    CHECK_FALSE(widget(*after,ui::WidgetRole::button,"Retained action").in_layout);
    f.panel.adopt(group);
    f.click(action);
    CHECK(action.clicked());
    CHECK(action.bounds().x<350.F);
}
TEST_CASE("UI adoption rejects cycles roots and foreign screens without changing the tree", "[ui][layout]") {
    Fixture f;
    auto group=f.panel.column();
    auto child=group.column();
    auto action=child.button("Still here");
    ui::Screen foreign{ui::dark_theme(font())};
    CHECK_THROWS_AS(child.adopt(group),std::invalid_argument);
    CHECK_THROWS_AS(group.adopt(group),std::invalid_argument);
    CHECK_THROWS_AS(group.adopt(f.screen.root()),std::invalid_argument);
    CHECK_THROWS_AS(foreign.root().adopt(group),std::invalid_argument);
    f.click(action);
    CHECK(action.clicked());
}
TEST_CASE("Moving a captured UI group cancels its gesture and does not activate it on release", "[ui][layout]") {
    Fixture f;
    auto group=f.panel.column();
    auto action=group.button("Move me");
    auto destination=f.screen.column().position({350,20}).width(250);
    const auto point=f.center(action);
    f.pump({Fixture::mouse(EventKind::pointer_down,point)});
    destination.adopt(group);
    f.pump({Fixture::mouse(EventKind::pointer_up,point)});
    CHECK_FALSE(action.clicked());
    f.click(action);
    CHECK(action.clicked());
}
TEST_CASE("UI adoption checks nesting for the entire subtree before changing ownership", "[ui][layout]") {
    Fixture f;
    auto group=f.panel.column();
    auto action=group.column().button("Still shallow");
    auto destination=f.screen.column().visible(false);
    for(unsigned i=1;i<126;++i) destination=destination.column();
    CHECK_THROWS_AS(destination.adopt(group),std::length_error);
    f.click(action);
    CHECK(action.clicked());
    // A leaf group may still be placed at the maximum supported depth.
    auto shallow=f.screen.root().column();
    shallow.button("Depth 128");
    CHECK_NOTHROW(destination.adopt(shallow));
}
ui::WidgetSnapshot scrollbar(Fixture& f, ui::Container owner) {
    const auto inspection = f.screen.inspect();
    REQUIRE(inspection);
    const auto bounds = owner.bounds();
    const auto parent = std::ranges::find_if(inspection->widgets, [&](const auto& entry) {
        return entry.role == ui::WidgetRole::column && same_rect(entry.bounds, bounds);
    });
    REQUIRE(parent != inspection->widgets.end());
    const auto bar = std::ranges::find_if(inspection->widgets, [&](const auto& entry) {
        return entry.role == ui::WidgetRole::scrollbar && entry.parent == parent->id;
    });
    REQUIRE(bar != inspection->widgets.end());
    return *bar;
}
Vec2 middle(ui::Rect r) { return {r.x + r.width * .5F, r.y + r.height * .5F}; }
} // namespace

TEST_CASE("UI scrollbars are proportional navigation controls with a reserved gutter",
          "[ui][scrollbar][inspection]") {
    Fixture f;
    f.panel.height(100);
    auto first = f.panel.button("First");
    for (int i = 0; i < 7; ++i) f.panel.button("Item");
    f.pump();
    const auto bar = scrollbar(f, f.panel);
    REQUIRE(bar.thumb);
    CHECK(bar.label == "Vertical scroll");
    CHECK(bar.visible);
    CHECK(bar.enabled);
    CHECK(bar.number == 0.F);
    CHECK(bar.minimum == 0.F);
    CHECK(bar.maximum == f.panel.scroll_limit());
    CHECK(bar.bounds.width == 14.F);
    CHECK(bar.bounds.height == 84.F);
    CHECK(bar.thumb->height == 24.F); // minimum-sized target for this long list
    CHECK(first.bounds().x + first.bounds().width + 4.F == bar.bounds.x);
    CHECK(bar.thumb->y == bar.bounds.y);
    const auto list = f.screen.draw_list();
    REQUIRE(list);
    CHECK(std::ranges::any_of(list->commands, [&](const auto& command) {
        const auto* box = std::get_if<ui::BoxDraw>(&command);
        return box && same_rect(box->rect, bar.bounds);
    }));

    f.panel.scroll(f.panel.scroll_limit());
    f.pump();
    const auto bottom = scrollbar(f, f.panel);
    CHECK(bottom.id == bar.id);
    CHECK(bottom.number == bottom.maximum);
    CHECK(bottom.thumb->y + bottom.thumb->height == bottom.bounds.y + bottom.bounds.height);
    f.panel.height(150);
    f.pump();
    const auto taller = scrollbar(f, f.panel);
    CHECK(taller.id == bar.id);
    CHECK(taller.thumb->height > bar.thumb->height);
    CHECK(f.panel.scroll() <= f.panel.scroll_limit());
}

TEST_CASE("UI scrollbar modes do not reserve space for nonoverflowing automatic columns",
          "[ui][scrollbar][layout]") {
    Fixture f;
    f.panel.height(100);
    auto first = f.panel.button("Only item");
    f.pump();
    CHECK(f.panel.scroll_limit() == 0.F);
    CHECK(first.bounds().width == 234.F);
    auto inspection = f.screen.inspect();
    REQUIRE(inspection);
    CHECK(std::ranges::none_of(inspection->widgets, [](const auto& entry) {
        return entry.role == ui::WidgetRole::scrollbar;
    }));
    f.panel.scrollbar(ui::ScrollBar::always);
    auto bar = scrollbar(f, f.panel);
    CHECK(bar.visible);
    CHECK_FALSE(bar.enabled);
    CHECK(same_rect(*bar.thumb, bar.bounds));
    CHECK(first.bounds().width == 216.F);
    f.pump({Fixture::mouse(EventKind::pointer_down, middle(bar.bounds)),
            Fixture::mouse(EventKind::pointer_up, middle(bar.bounds))});
    CHECK(f.panel.scroll() == 0.F);
    CHECK_FALSE(first.clicked());

    f.panel.scrollbar(ui::ScrollBar::hidden);
    for (int i = 0; i < 6; ++i) f.panel.button("Overflow");
    f.pump();
    CHECK(first.bounds().width == 234.F);
    inspection = f.screen.inspect();
    REQUIRE(inspection);
    CHECK(std::ranges::none_of(inspection->widgets, [](const auto& entry) {
        return entry.role == ui::WidgetRole::scrollbar;
    }));
    f.pump({{.kind = EventKind::scroll, .position = middle(first.bounds()), .scroll = {0, -1}}});
    CHECK(f.panel.scroll() == 36.F); // hidden means wheel-only, not nonscrollable
    REQUIRE_THROWS_AS(f.panel.row().scrollbar(ui::ScrollBar::always), std::invalid_argument);
}

TEST_CASE("UI scrollbar thumb and track gestures own their entire pointer interaction",
          "[ui][scrollbar][input]") {
    Fixture f;
    f.panel.height(160);
    auto first = f.panel.button("First");
    std::vector<ui::Button> buttons;
    for (int i = 0; i < 12; ++i) buttons.push_back(f.panel.button("Item"));
    auto behind = f.screen.root().button("Elsewhere").position({350, 300}).width(160);
    f.pump();
    auto bar = scrollbar(f, f.panel);
    const auto start = middle(*bar.thumb);
    auto result = f.pump({Fixture::mouse(EventKind::pointer_down, start)});
    CHECK(result.events.empty());
    CHECK(scrollbar(f, f.panel).pressed);
    const Vec2 outside{430, 340};
    result = f.pump({Fixture::mouse(EventKind::pointer_move, outside),
                     Fixture::mouse(EventKind::pointer_up, outside)});
    CHECK(result.events.empty());
    CHECK(f.panel.scroll() == f.panel.scroll_limit());
    CHECK_FALSE(scrollbar(f, f.panel).pressed);
    CHECK_FALSE(first.clicked());
    CHECK_FALSE(behind.clicked());
    for (const auto& button : buttons) CHECK_FALSE(button.clicked());

    // A track click seeks with the thumb centered on the pointer.
    bar = scrollbar(f, f.panel);
    const Vec2 upper{bar.bounds.x + 4, bar.bounds.y + 10};
    f.pump({Fixture::mouse(EventKind::pointer_down, upper),
            Fixture::mouse(EventKind::pointer_up, upper)});
    CHECK(f.panel.scroll() == 0.F);
    f.click(first);
    CHECK(first.clicked()); // capture did not leak into the next gesture
}

TEST_CASE("UI nested scrollbars drag independently and wheel bubbles only at an edge",
          "[ui][scrollbar][input]") {
    Fixture f;
    f.panel.height(180);
    auto nested = f.panel.column().height(110).padding(4).gap(4);
    auto first = nested.button("Nested first");
    for (int i = 0; i < 8; ++i) nested.button("Nested item");
    f.panel.label("Parent tail").height(400);
    f.pump();
    const auto parent_bar = scrollbar(f, f.panel);
    auto child_bar = scrollbar(f, nested);
    CHECK(parent_bar.id != child_bar.id);
    CHECK(parent_bar.parent != child_bar.parent);
    CHECK(child_bar.bounds.x + child_bar.bounds.width < parent_bar.bounds.x);
    auto point = middle(first.bounds());
    f.pump({{.kind = EventKind::scroll, .position = point, .scroll = {0, -1}}});
    CHECK(nested.scroll() == 36.F);
    CHECK(f.panel.scroll() == 0.F);
    nested.scroll(nested.scroll_limit());
    f.pump();
    point = {child_bar.bounds.x - 10, child_bar.bounds.y + 15};
    f.pump({{.kind = EventKind::scroll, .position = point, .scroll = {0, -1}}});
    CHECK(nested.scroll() == nested.scroll_limit());
    CHECK(f.panel.scroll() == 36.F);
    // Up still belongs to the inner list while it has room to move.
    point.y = parent_bar.bounds.y + 10;
    f.pump({{.kind = EventKind::scroll, .position = point, .scroll = {0, 1}}});
    CHECK(nested.scroll() == nested.scroll_limit() - 36.F);
    CHECK(f.panel.scroll() == 36.F);

    f.panel.scroll(0);
    nested.scroll(0);
    f.pump();
    child_bar = scrollbar(f, nested);
    f.pump({Fixture::mouse(EventKind::pointer_down, middle(*child_bar.thumb)),
            Fixture::mouse(EventKind::pointer_move,
                           {child_bar.bounds.x + 5, child_bar.bounds.y + child_bar.bounds.height}),
            Fixture::mouse(EventKind::pointer_up,
                           {child_bar.bounds.x + 5, child_bar.bounds.y + child_bar.bounds.height})});
    CHECK(nested.scroll() == nested.scroll_limit());
    CHECK(f.panel.scroll() == 0.F);
    f.pump({Fixture::mouse(EventKind::pointer_down, middle(*parent_bar.thumb)),
            Fixture::mouse(EventKind::pointer_move,
                           {parent_bar.bounds.x + 5, parent_bar.bounds.y + 55}),
            Fixture::mouse(EventKind::pointer_up,
                           {parent_bar.bounds.x + 5, parent_bar.bounds.y + 55})});
    CHECK(f.panel.scroll() > 0.F);
    CHECK(nested.scroll() == nested.scroll_limit());
}

TEST_CASE("Fractional scrollbar thumb endpoints do not swallow the next wheel event", "[ui][scrollbar][regression]") {
    Fixture f;
    f.panel.height(516).padding(12).gap(6);
    f.panel.label("Header").height(36);
    auto nested = f.panel.column().height(76).padding(0).gap(4).scrollbar(ui::ScrollBar::always);
    for (int i=0;i<3;++i) nested.button("Item").height(36);
    f.panel.label("Tail").height(650); f.pump();
    const auto bar=scrollbar(f,nested);
    const auto start=middle(*bar.thumb);
    const Vec2 end{start.x,bar.bounds.y+bar.bounds.height-bar.thumb->height*.5F};
    f.pump({Fixture::mouse(EventKind::pointer_down,start),Fixture::mouse(EventKind::pointer_move,end),
        Fixture::mouse(EventKind::pointer_up,end)});
    REQUIRE(nested.scroll()==nested.scroll_limit());
    f.pump({{.kind=EventKind::scroll,.position={bar.bounds.x-12,bar.bounds.y+bar.bounds.height*.5F},.scroll={0,-1}}});
    CHECK(f.panel.scroll()>0);
}

TEST_CASE("UI scrollbar capture cancels safely on Escape loss removal hiding and resizing",
          "[ui][scrollbar][input]") {
    Fixture f;
    f.panel.height(140);
    auto button = f.panel.button("First");
    for (int i = 0; i < 10; ++i) f.panel.button("Item");
    f.pump();
    f.panel.scroll(20);
    auto bar = scrollbar(f, f.panel);
    f.pump({Fixture::mouse(EventKind::pointer_down, middle(*bar.thumb)),
            Fixture::mouse(EventKind::pointer_move,
                           {bar.bounds.x + 5, bar.bounds.y + 70})});
    REQUIRE(f.panel.scroll() > 20.F);
    SECTION("Escape restores the scroll position without clicking a newly revealed row") {
        auto result = f.pump({Fixture::key(Key::escape)});
        CHECK(result.events.empty());
        CHECK(f.panel.scroll() == 20.F);
        CHECK_FALSE(scrollbar(f, f.panel).pressed);
        result = f.pump({Fixture::mouse(EventKind::pointer_up, {40, 40})});
        CHECK(result.events.empty());
        CHECK_FALSE(button.clicked());
    }
    SECTION("Focus loss releases the gesture") {
        f.pump({{.kind = EventKind::focus_lost}});
        CHECK(f.panel.scroll() == 20.F);
        CHECK_FALSE(scrollbar(f, f.panel).pressed);
        f.pump({{.kind = EventKind::focus_gained},
                Fixture::mouse(EventKind::pointer_up, {40, 40})});
        CHECK_FALSE(button.clicked());
    }
    SECTION("Hiding an ancestor cancels capture") {
        f.panel.visible(false);
        CHECK(f.panel.scroll() == 20.F);
        const auto inspection = f.screen.inspect();
        REQUIRE(inspection);
        const auto hidden = std::ranges::find(inspection->widgets, bar.id, &ui::WidgetSnapshot::id);
        REQUIRE(hidden != inspection->widgets.end());
        CHECK_FALSE(hidden->visible);
        CHECK_FALSE(hidden->pressed);
        f.panel.visible(true);
        f.pump({Fixture::mouse(EventKind::pointer_up, {40, 40})});
        CHECK_FALSE(button.clicked());
    }
    SECTION("Disabling prevents both drag and wheel from changing scroll") {
        f.panel.enabled(false);
        CHECK(f.panel.scroll() == 20.F);
        f.pump({Fixture::mouse(EventKind::pointer_move, {bar.bounds.x + 5, 500}),
                Fixture::mouse(EventKind::pointer_up, {bar.bounds.x + 5, 500}),
                {.kind = EventKind::scroll, .position = {40, 40}, .scroll = {0, -1}}});
        CHECK(f.panel.scroll() == 20.F);
        CHECK_FALSE(scrollbar(f, f.panel).enabled);
    }
    SECTION("Growing to fit removes the bar and clamps the cancelled gesture") {
        f.panel.height(540);
        f.pump({Fixture::mouse(EventKind::pointer_up, {40, 40})});
        CHECK(f.panel.scroll_limit() == 0.F);
        CHECK(f.panel.scroll() == 0.F);
        CHECK_FALSE(button.clicked());
    }
    SECTION("Removing the owner cannot leave a stale capture") {
        f.panel.remove();
        auto result = f.pump({Fixture::mouse(EventKind::pointer_up, {40, 40})});
        CHECK(result.events.empty());
        CHECK_FALSE(result.capturesPointer);
        CHECK_FALSE(button.valid());
    }
}

TEST_CASE("UI distinguishes ordinary widget focus from exclusive shortcut ownership", "[ui][input]") {
    Fixture f;
    auto button=f.panel.button("Action");
    auto field=f.panel.text_input("Value");
    auto choices=f.panel.dropdown<int>("Choice",{{1,"One"},{2,"Two"}});
    button.focus();auto result=f.pump();
    CHECK(result.capturesKeyboard);CHECK_FALSE(result.capturesShortcuts);
    field.focus();result=f.pump();CHECK(result.capturesShortcuts);
    choices.focus();result=f.pump();CHECK_FALSE(result.capturesShortcuts);
    f.click(choices);result=f.pump();CHECK(result.capturesShortcuts);
    f.pump({Fixture::key(Key::escape)});result=f.pump();CHECK_FALSE(result.capturesShortcuts);
}

TEST_CASE("UI scrollbar drags isolate keyboard shortcuts text and pending activations",
          "[ui][scrollbar][input]") {
    Fixture f;
    f.panel.height(150);
    auto field = f.panel.text_input("Draft").value("keep this text");
    auto button = f.panel.button("Apply");
    for (int i = 0; i < 10; ++i) f.panel.button("Item");
    f.pump();
    SECTION("No focused widget still owns Delete and keyboard state during scrolling") {
        const auto bar = scrollbar(f, f.panel);
        f.pump({Fixture::mouse(EventKind::pointer_down, middle(*bar.thumb))});
        f.input.keys[static_cast<std::size_t>(Key::del)] = true;
        const auto result = f.pump({Fixture::key(Key::del), Fixture::key(Key::enter),
            Fixture::key(Key::tab), Fixture::text("ignored"),
            Fixture::key(Key::enter, false, false, EventKind::key_up),
            Fixture::key(Key::tab, false, false, EventKind::key_up),
            Fixture::key(Key::del, false, false, EventKind::key_up)});
        CHECK(result.events.empty());
        CHECK(result.capturesKeyboard);
        CHECK_FALSE(result.keyDown(Key::del));
        CHECK_FALSE(button.clicked());
        CHECK_FALSE(field.isFocused());
        CHECK_FALSE(button.isFocused());
        CHECK(field.getText() == "keep this text");
        CHECK(f.panel.scroll() == 0.F);
        CHECK(scrollbar(f, f.panel).pressed);
        f.input.keys[static_cast<std::size_t>(Key::del)] = false;
        const auto released = f.pump({Fixture::mouse(EventKind::pointer_up, middle(*bar.thumb))});
        CHECK_FALSE(released.capturesKeyboard);
        // Ordinary editor shortcuts are available again after the gesture.
        CHECK(f.pump({Fixture::key(Key::del)}).events.size() == 1);
    }
    SECTION("Retained text focus does not edit submit or Tab-reveal during dragging") {
        field.focus();
        const auto bar = scrollbar(f, f.panel);
        f.pump({Fixture::mouse(EventKind::pointer_down, middle(*bar.thumb)),
                Fixture::mouse(EventKind::pointer_move,
                               {bar.bounds.x + 5, bar.bounds.y + 80})});
        const auto position = f.panel.scroll();
        REQUIRE(position > 0.F);
        const auto result = f.pump({Fixture::key(Key::del), Fixture::text("ignored"),
            Fixture::key(Key::enter), Fixture::key(Key::tab),
            Fixture::key(Key::enter, false, false, EventKind::key_up),
            Fixture::key(Key::tab, false, false, EventKind::key_up)});
        CHECK(result.events.empty());
        CHECK(result.capturesKeyboard);
        CHECK(field.isFocused());
        CHECK_FALSE(button.isFocused());
        CHECK(field.getText() == "keep this text");
        CHECK_FALSE(field.changedText());
        CHECK_FALSE(field.submittedText());
        CHECK(f.panel.scroll() == position);
        CHECK(scrollbar(f, f.panel).pressed);
        f.pump({Fixture::key(Key::escape)});
        CHECK(f.panel.scroll() == 0.F);
        CHECK(field.isFocused());
        CHECK_FALSE(field.editCancelled()); // Escape cancelled the scrollbar, not this draft
        CHECK_FALSE(scrollbar(f, f.panel).pressed);
    }
    SECTION("A pending Enter activation cannot fire during or after the scrollbar gesture") {
        button.focus();
        f.pump({Fixture::key(Key::enter)});
        REQUIRE(button.isPressed());
        const auto bar = scrollbar(f, f.panel);
        const auto result = f.pump({Fixture::mouse(EventKind::pointer_down, middle(*bar.thumb)),
            Fixture::key(Key::enter, false, false, EventKind::key_up)});
        CHECK(result.events.empty());
        CHECK_FALSE(button.clicked());
        f.pump({Fixture::mouse(EventKind::pointer_up, middle(*bar.thumb)),
                Fixture::key(Key::enter, false, false, EventKind::key_up)});
        CHECK_FALSE(button.clicked());
        f.pump({Fixture::key(Key::enter), Fixture::key(Key::enter, false, false, EventKind::key_up)});
        CHECK(button.clicked());
    }
}

TEST_CASE("UI inspection exposes owned semantic values and stable hierarchy IDs",
          "[ui][inspection]") {
    Fixture f;
    auto label = f.panel.label("Current status");
    auto row = f.panel.row().padding(0);
    auto button = row.button("Run");
    auto text = f.panel.text_input("Distance").placeholder("Enter distance").value("12.5");
    auto area = f.panel.text_area("Notes").value("Line one\nLine two").height(60);
    auto checkbox = f.panel.checkbox("Depth").value(true);
    auto slider = f.panel.slider("Exposure", 0, 10).value(2.5F);
    auto dropdown = f.panel.dropdown<int>("Mode", {{1, "Mesh"}, {2, "Scene"}}).value(2);
    auto image = f.panel.image().height(50);
    CHECK_FALSE(f.screen.inspect());
    f.pump();
    auto inspected = f.screen.inspect();
    REQUIRE(inspected);
    using Role = ui::WidgetRole;
    const auto& root_node = widget(*inspected, Role::root, "");
    const auto& panel_node = widget(*inspected, Role::column, "");
    const auto& row_node = widget(*inspected, Role::row, "");
    const auto& button_node = widget(*inspected, Role::button, "Run");
    CHECK(root_node.parent == 0);
    CHECK(root_node.id != 0);
    CHECK(panel_node.parent == root_node.id);
    CHECK(row_node.parent == panel_node.id);
    CHECK(button_node.parent == row_node.id);
    CHECK(button_node.text == "Run");
    CHECK(button_node.visible);
    CHECK(button_node.enabled);
    CHECK(same_rect(button_node.bounds, button.bounds()));
    const auto& input_node = widget(*inspected, Role::text_field, "Distance");
    CHECK(input_node.parent == panel_node.id);
    CHECK(input_node.text == "12.5");
    CHECK(input_node.placeholder == "Enter distance");
    CHECK_FALSE(input_node.number);
    CHECK(widget(*inspected, Role::text_area, "Notes").text == "Line one\nLine two");
    CHECK(widget(*inspected, Role::checkbox, "Depth").checked == true);
    CHECK(widget(*inspected, Role::slider, "Exposure").number == 2.5F);
    CHECK(widget(*inspected, Role::slider, "Exposure").minimum == 0.F);
    CHECK(widget(*inspected, Role::slider, "Exposure").maximum == 10.F);
    CHECK(widget(*inspected, Role::dropdown, "Mode").text == "Scene");
    CHECK_FALSE(widget(*inspected, Role::dropdown, "Mode").expanded);
    CHECK_FALSE(widget(*inspected, Role::image, "").number);
    CHECK(label.text() == "Current status");
    CHECK(inspected->logical_size == Vec2{800, 600});
    CHECK(inspected->framebuffer == Extent2D{800, 600});

    const auto original_button_id = button_node.id;
    const auto original_input_id = input_node.id;
    button.text("Execute");
    label.text("Ready");
    text.value("7.25");
    auto current = f.screen.inspect();
    REQUIRE(current);
    CHECK(widget(*current, Role::button, "Execute").id == original_button_id);
    CHECK(widget(*current, Role::text_field, "Distance").id == original_input_id);
    CHECK(widget(*current, Role::text_field, "Distance").text == "7.25");
    CHECK(label.text() == "Ready");
    // Strings in the old snapshot remain owned even after live values change.
    CHECK(widget(*inspected, Role::button, "Run").text == "Run");
    CHECK(widget(*inspected, Role::text_field, "Distance").text == "12.5");
    auto copied = *current;
    copied.widgets.clear();
    row.remove();
    auto replacement = f.panel.button("Replacement");
    current = f.screen.inspect();
    REQUIRE(current);
    CHECK(std::ranges::none_of(current->widgets, [&](const auto& value) {
        return value.id == original_button_id || value.id == row_node.id;
    }));
    CHECK(widget(*current, Role::button, "Replacement").id != original_button_id);
    CHECK(widget(*current, Role::text_field, "Distance").id == original_input_id);
}

TEST_CASE("UI inspection distinguishes hidden ancestors from clipped and disabled widgets",
          "[ui][inspection][layout]") {
    Fixture f;
    f.input.framebuffer = {1600, 1200};
    f.panel.height(96);
    auto first = f.panel.button("First").height(40);
    auto second = f.panel.button("Second").height(40);
    auto third = f.panel.button("Third").height(40);
    f.pump();
    auto inspection = f.screen.inspect();
    REQUIRE(inspection);
    CHECK(inspection->logical_size == Vec2{800, 600});
    CHECK(inspection->framebuffer == Extent2D{1600, 1200});
    const auto& a = widget(*inspection, ui::WidgetRole::button, "First");
    const auto& b = widget(*inspection, ui::WidgetRole::button, "Second");
    const auto& c = widget(*inspection, ui::WidgetRole::button, "Third");
    CHECK(a.bounds.x == 28.F);
    CHECK(a.bounds.y == 28.F); // logical coordinates, not framebuffer pixels
    CHECK(same_rect(a.bounds, a.clip));
    CHECK(b.bounds.height == 40.F);
    CHECK(b.clip.height == 34.F);
    CHECK(b.in_layout);
    CHECK(b.visible);
    CHECK(c.in_layout);
    CHECK_FALSE(c.visible);
    CHECK(c.clip.height == 0.F);
    CHECK(c.bounds.height == 40.F);

    f.panel.enabled(false);
    inspection = f.screen.inspect();
    REQUIRE(inspection);
    CHECK_FALSE(widget(*inspection, ui::WidgetRole::button, "First").enabled);
    CHECK(widget(*inspection, ui::WidgetRole::button, "First").visible);
    f.panel.enabled(true).visible(false);
    inspection = f.screen.inspect();
    REQUIRE(inspection);
    for (const auto& entry : inspection->widgets) {
        if (entry.role == ui::WidgetRole::root) continue;
        CHECK_FALSE(entry.in_layout);
        CHECK_FALSE(entry.visible);
        CHECK(entry.enabled); // enabled and visible are independent
        CHECK(same_rect(entry.bounds, {}));
        CHECK(same_rect(entry.clip, {}));
    }
    f.panel.visible(true);
    second.visible(false);
    inspection = f.screen.inspect();
    REQUIRE(inspection);
    CHECK_FALSE(widget(*inspection, ui::WidgetRole::button, "Second").in_layout);
    CHECK(widget(*inspection, ui::WidgetRole::button, "Third").visible);
}

TEST_CASE("UI inspection does not consume activation or focus and snapshots survive the screen",
          "[ui][inspection]") {
    ui::Inspection retained;
    {
        Fixture f;
        auto button = f.panel.button("Run");
        auto field = f.panel.text_input("Name");
        const auto point = f.center(button);
        f.pump({Fixture::mouse(EventKind::pointer_down, point)});
        auto inspection = f.screen.inspect();
        REQUIRE(inspection);
        CHECK(widget(*inspection, ui::WidgetRole::button, "Run").pressed);
        CHECK(button.isPressed());
        f.pump({Fixture::mouse(EventKind::pointer_up, point)});
        REQUIRE(button.clicked());
        for (int i = 0; i < 2; ++i) {
            inspection = f.screen.inspect();
            REQUIRE(inspection);
            CHECK(widget(*inspection, ui::WidgetRole::button, "Run").focused);
            CHECK_FALSE(widget(*inspection, ui::WidgetRole::button, "Run").pressed);
            CHECK(button.clicked());
        }
        field.focus();
        f.pump({Fixture::text("draft")});
        REQUIRE(field.changedText() == "draft");
        inspection = f.screen.inspect();
        REQUIRE(inspection);
        CHECK(widget(*inspection, ui::WidgetRole::text_field, "Name").focused);
        CHECK(field.changedText() == "draft");
        retained = std::move(*inspection);
        auto moved = std::move(f.screen);
        CHECK_FALSE(f.screen.inspect());
        CHECK(moved.inspect());
    }
    CHECK(widget(retained, ui::WidgetRole::text_field, "Name").text == "draft");
}

TEST_CASE("UI inspection exposes clickable dropdown options outside parent clips and through scrolling",
          "[ui][inspection][popup]") {
    Fixture f;
    f.panel.height(60);
    auto dropdown = f.panel.dropdown<int>("Mode", {{0, "Zero"}, {1, "One"}, {2, "Two"},
        {3, "Three"}, {4, "Four"}, {5, "Five"}, {6, "Six"}, {7, "Seven"},
        {8, "Eight"}, {9, "Nine"}});
    f.click(dropdown);
    auto inspection = f.screen.inspect();
    REQUIRE(inspection);
    const auto& popup = widget(*inspection, ui::WidgetRole::dropdown, "Mode");
    CHECK(popup.expanded);
    const auto& zero = widget(*inspection, ui::WidgetRole::option, "Zero");
    const auto& nine = widget(*inspection, ui::WidgetRole::option, "Nine");
    const auto& six = widget(*inspection, ui::WidgetRole::option, "Six");
    CHECK(zero.parent == popup.id);
    CHECK(zero.selected);
    CHECK(zero.option_index == 0);
    CHECK_FALSE(nine.in_layout);
    CHECK_FALSE(nine.visible);
    CHECK(same_rect(nine.bounds, {}));
    CHECK(six.visible);
    CHECK(six.bounds.y > f.panel.bounds().y + f.panel.bounds().height);
    const auto nine_id = nine.id;
    const Vec2 six_point{six.clip.x + six.clip.width * .5F, six.clip.y + six.clip.height * .5F};
    f.pump({Fixture::mouse(EventKind::pointer_down, six_point),
            Fixture::mouse(EventKind::pointer_up, six_point)});
    CHECK(dropdown.changedValue() == 6);
    inspection = f.screen.inspect();
    REQUIRE(inspection);
    CHECK_FALSE(widget(*inspection, ui::WidgetRole::dropdown, "Mode").expanded);
    CHECK(std::ranges::none_of(inspection->widgets, [](const auto& entry) {
        return entry.role == ui::WidgetRole::option;
    }));

    f.click(dropdown);
    f.pump({{.kind = EventKind::scroll, .position = six_point, .scroll = {0, -2}}});
    inspection = f.screen.inspect();
    REQUIRE(inspection);
    const auto& last = widget(*inspection, ui::WidgetRole::option, "Nine");
    CHECK(last.id == nine_id);
    CHECK(last.option_index == 9);
    CHECK(last.visible);
    CHECK_FALSE(widget(*inspection, ui::WidgetRole::option, "Zero").in_layout);
    const Vec2 last_point{last.clip.x + last.clip.width * .5F, last.clip.y + last.clip.height * .5F};
    f.pump({Fixture::mouse(EventKind::pointer_down, last_point),
            Fixture::mouse(EventKind::pointer_up, last_point)});
    CHECK(dropdown.changedValue() == 9);
    f.click(dropdown);
    inspection = f.screen.inspect();
    REQUIRE(inspection);
    CHECK(widget(*inspection, ui::WidgetRole::option, "Nine").selected);
    f.panel.visible(false);
    inspection = f.screen.inspect();
    REQUIRE(inspection);
    CHECK(std::ranges::none_of(inspection->widgets, [](const auto& entry) {
        return entry.role == ui::WidgetRole::option;
    }));
}

TEST_CASE("UI inspected popup rows report clipping on a short viewport", "[ui][inspection][popup]") {
    Fixture f;
    f.input.logical_size = {800, 160};
    f.input.framebuffer = {800, 160};
    auto dropdown = f.panel.dropdown<int>("Mode", {{0, "Zero"}, {1, "One"}, {2, "Two"},
                                                 {3, "Three"}, {4, "Four"}});
    f.click(dropdown);
    const auto inspection = f.screen.inspect();
    REQUIRE(inspection);
    const auto& first = widget(*inspection, ui::WidgetRole::option, "Zero");
    const auto& last = widget(*inspection, ui::WidgetRole::option, "Four");
    CHECK(first.visible);
    CHECK(last.in_layout);
    CHECK_FALSE(last.visible);
    for (const auto& entry : inspection->widgets) {
        if (entry.role != ui::WidgetRole::option || !entry.visible) continue;
        CHECK(entry.clip.y >= 0.F);
        CHECK(entry.clip.y + entry.clip.height <= 160.F);
        CHECK(entry.clip.height <= entry.bounds.height);
    }
}

TEST_CASE("Compact dropdowns expand their popup to fit long option labels",
          "[ui][inspection][popup][layout]") {
    Fixture f;
    const std::string caption = "Blueprint mesh / Deep-space explorer";
    auto dropdown = f.panel.dropdown<int>("View", {{0, "Scene"}, {1, caption}}).width(96);
    f.click(dropdown);
    auto inspected = f.screen.inspect();
    REQUIRE(inspected);
    const auto& choice = widget(*inspected, ui::WidgetRole::option, caption);
    CHECK(choice.bounds.width > dropdown.bounds().width);
    CHECK(choice.bounds.x == dropdown.bounds().x);
    const auto drawn = f.drawn_text(caption);
    const auto measured = drawn.font.measure(caption, static_cast<u32>(drawn.size));
    REQUIRE(measured);
    CHECK(drawn.position.x + measured->width <= drawn.clip.x + drawn.clip.width);
    const Vec2 point{choice.bounds.x + choice.bounds.width - 8,
                     choice.bounds.y + choice.bounds.height * .5F};
    CHECK_FALSE(dropdown.bounds().contains(point));
    // Popup hit testing must beat unrelated content outside the closed control.
    auto under = f.screen.root().button("Under popup").position({point.x - 20, point.y - 15})
                     .width(80).height(30);
    f.pump({Fixture::mouse(EventKind::pointer_down, point),
            Fixture::mouse(EventKind::pointer_up, point)});
    CHECK(dropdown.changedValue() == 1);
    CHECK_FALSE(under.clicked());
    CHECK(dropdown.bounds().width == 96.F); // no toolbar layout mutation
}

TEST_CASE("Dropdown popup fitting tracks theme metrics and remains inside resized screens",
          "[ui][inspection][popup][layout]") {
    Fixture f;
    auto theme = ui::dark_theme_values(font());
    f.screen.set_theme(std::make_shared<ui::BasicTheme>(theme));
    f.panel.position({720, 20}).width(80);
    const std::string caption = "Blueprint mesh / Deep-space explorer";
    auto dropdown = f.panel.dropdown<int>("View", {{0, "Scene"}, {1, caption}}).width(64);
    f.click(dropdown);
    auto inspected = f.screen.inspect();
    REQUIRE(inspected);
    auto choice = widget(*inspected, ui::WidgetRole::option, caption);
    const auto id = choice.id;
    const auto measured = theme.font.measure(caption, 18);
    REQUIRE(measured);
    CHECK(choice.bounds.width == measured->width + 2 * theme.padding);
    CHECK(choice.bounds.x >= 0.F);
    CHECK(choice.bounds.x + choice.bounds.width == 800.F);
    CHECK(choice.bounds.x < dropdown.bounds().x);

    // Padding does not require reshaping the cached labels. A different font
    // size does, and both must update an already open popup without reopening it.
    theme.padding = 18;
    f.screen.set_theme(std::make_shared<ui::BasicTheme>(theme));
    f.pump();
    inspected = f.screen.inspect();
    REQUIRE(inspected);
    choice = widget(*inspected, ui::WidgetRole::option, caption);
    CHECK(choice.id == id);
    CHECK(choice.bounds.width == measured->width + 36.F);
    const auto old_width = choice.bounds.width;
    theme.font_size = 28;
    theme.control_height = 44;
    f.screen.set_theme(std::make_shared<ui::BasicTheme>(theme));
    f.pump();
    inspected = f.screen.inspect();
    REQUIRE(inspected);
    choice = widget(*inspected, ui::WidgetRole::option, caption);
    CHECK(choice.id == id);
    CHECK(choice.bounds.width > old_width);
    CHECK(choice.bounds.x >= 0.F);
    CHECK(choice.bounds.x + choice.bounds.width <= 800.F);

    f.input.logical_size = {260, 140};
    f.input.framebuffer = {520, 280};
    f.pump(); // The old owner is now offscreen; popup placement remains bounded.
    inspected = f.screen.inspect();
    REQUIRE(inspected);
    choice = widget(*inspected, ui::WidgetRole::option, caption);
    CHECK(choice.id == id);
    CHECK(choice.bounds.width == 260.F);
    CHECK(choice.bounds.x == 0.F);
    for (const auto& option : inspected->widgets) {
        if (option.role != ui::WidgetRole::option || !option.visible) continue;
        CHECK(option.clip.x >= 0.F);
        CHECK(option.clip.y >= 0.F);
        CHECK(option.clip.x + option.clip.width <= 260.F);
        CHECK(option.clip.y + option.clip.height <= 140.F);
    }
}

TEST_CASE("Dropdown popup width includes long options outside the current scroll window",
          "[ui][inspection][popup][input]") {
    Fixture f;
    std::vector<ui::Choice<int>> choices;
    for (int i = 0; i < 11; ++i) choices.push_back({i, "Mesh " + std::to_string(i)});
    const std::string caption = "Blueprint mesh / Imported orbital patrol spacecraft";
    choices.push_back({11, caption});
    auto dropdown = f.panel.dropdown<int>("View", choices).width(100);
    f.click(dropdown);
    auto inspected = f.screen.inspect();
    REQUIRE(inspected);
    const auto first = widget(*inspected, ui::WidgetRole::option, "Mesh 0");
    CHECK(first.bounds.width > dropdown.bounds().width);
    CHECK_FALSE(widget(*inspected, ui::WidgetRole::option, caption).visible);
    const Vec2 point{first.bounds.x + first.bounds.width - 8,
                     first.bounds.y + first.bounds.height * .5F};
    f.pump({{.kind = EventKind::scroll, .position = point, .scroll = {0, -10}}});
    inspected = f.screen.inspect();
    REQUIRE(inspected);
    const auto last = widget(*inspected, ui::WidgetRole::option, caption);
    CHECK(last.visible);
    CHECK(last.bounds.width == first.bounds.width);
    const Vec2 last_point{last.bounds.x + last.bounds.width - 8,
                          last.bounds.y + last.bounds.height * .5F};
    f.pump({Fixture::mouse(EventKind::pointer_down, last_point),
            Fixture::mouse(EventKind::pointer_up, last_point)});
    CHECK(dropdown.changedValue() == 11);
}

TEST_CASE("UI repeated unchanged panel settings preserve geometry and input", "[ui][layout]") {
    Fixture f;
    std::vector<ui::Button> buttons;
    for (u32 y = 0; y < 20; ++y) {
        auto row = f.panel.row().padding(0).gap(2);
        for (u32 x = 0; x < 8; ++x)
            buttons.push_back(row.button("Panel control"));
    }
    f.pump();
    const auto before = buttons.front().bounds();
    const auto start = std::chrono::steady_clock::now();
    for (u32 i = 0; i < 10; ++i) {
        f.panel.position({20, 20}).width(250).padding(8).gap(6);
        for (auto& button : buttons)
            button.visible(true).enabled(true);
        f.pump();
    }
    const auto milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count() /
        10;
    std::cout << "UI retained 160-control panel: " << milliseconds << " ms/update\n";
    const auto after = buttons.front().bounds();
    CHECK(after.x == before.x);
    CHECK(after.y == before.y);
    CHECK(after.width == before.width);
    CHECK(after.height == before.height);
    f.click(buttons.front());
    CHECK(buttons.front().clicked());
}

TEST_CASE("UI natural widths track label padding font size and theme changes", "[ui][layout]") {
    Fixture f;
    const auto face = font();
    auto theme = ui::dark_theme_values(face);
    f.screen.set_theme(std::make_shared<ui::BasicTheme>(theme));
    auto row = f.panel.row().padding(0).gap(0);
    auto button = row.button("Wide label sample");
    f.pump();
    const auto expected = [&](std::string_view text, f32 padding, u32 size) {
        auto metrics = face.measure(text, size);
        REQUIRE(metrics);
        return std::max(100.0F, metrics->width + padding * 2 + 24);
    };
    CHECK(button.bounds().width == expected("Wide label sample", theme.padding, 18));
    button.text("Much wider changed label sample");
    CHECK(button.bounds().width == expected("Much wider changed label sample", theme.padding, 18));
    theme.padding = 3;
    f.screen.set_theme(std::make_shared<ui::BasicTheme>(theme));
    CHECK(button.bounds().width == expected("Much wider changed label sample", 3, 18));
    theme.font_size = 30;
    f.screen.set_theme(std::make_shared<ui::BasicTheme>(theme));
    CHECK(button.bounds().width == expected("Much wider changed label sample", 3, 30));
    button.text("A");
    CHECK(button.bounds().width == 100.0F);
}

TEST_CASE("UI idempotent visibility and enabled setters do not relayout the tree", "[ui][layout]") {
    struct Theme final : ui::Theme {
        ui::ThemeValues style{ui::dark_theme_values(font())};
        mutable u32 accesses{};
        const ui::ThemeValues& values() const noexcept override {
            ++accesses;
            return style;
        }
    };
    auto theme = std::make_shared<Theme>();
    ui::Screen screen{theme};
    auto panel = screen.column().position({10, 10}).width(400).padding(10).gap(8);
    auto button = panel.button("Unchanged button");
    REQUIRE(screen.update({.logical_size = {800, 600}, .framebuffer = {800, 600}}, 0));
    theme->accesses = 0;
    for (u32 i = 0; i < 100; ++i) {
        panel.position({10, 10}).width(400).padding(10).gap(8);
        button.visible(true).enabled(true);
        (void)button.bounds();
    }
    CHECK(theme->accesses == 0);
    button.enabled(false);
    CHECK(theme->accesses == 0); // Changed flags also defer layout now.
    (void)button.bounds();
    CHECK(theme->accesses > 0);
}

TEST_CASE("Changing fixed-width UI text does not relayout unrelated panels", "[ui][layout][performance]") {
    struct Theme final : ui::Theme {
        ui::ThemeValues style{ui::dark_theme_values(font())};
        mutable u32 accesses{};
        const ui::ThemeValues& values() const noexcept override { ++accesses; return style; }
    };
    auto theme = std::make_shared<Theme>();
    ui::Screen screen{theme};
    auto panel = screen.column().width(400).height(300);
    auto status = panel.label("Status");
    auto field = panel.row().text_input().width(120);
    for (int i=0; i<500; ++i) panel.button("Offscreen " + std::to_string(i)).height(34);
    REQUIRE(screen.update({.logical_size={800,600}, .framebuffer={800,600}}, 0));
    const auto bounds = status.bounds();
    theme->accesses = 0;
    status.text("Camera update 42");
    field.value("123.456");
    CHECK(same_rect(status.bounds(), bounds));
    CHECK(theme->accesses == 0);
    const auto list = screen.draw_list();
    REQUIRE(list);
    CHECK(list->commands.size() < 80); // Clipped controls never reach the renderer.
}

TEST_CASE("Reveal scrolls nested panels and keeps offscreen controls out of drawing",
          "[ui][scroll][layout]") {
    Fixture f;
    f.panel.height(180);
    f.panel.label("Above").height(220);
    auto inner = f.panel.column().height(120).padding(4);
    for (int i=0; i<50; ++i) inner.button("Offscreen " + std::to_string(i)).height(34);
    auto target = inner.text_input("Target").height(34).value("Selected instance");
    f.pump();
    target.reveal();
    target.focus();
    f.pump();
    const auto inspection = f.screen.inspect();
    REQUIRE(inspection);
    const auto& entry = widget(*inspection, ui::WidgetRole::text_field, "Target");
    CHECK(entry.visible);
    CHECK(entry.focused);
    CHECK(entry.clip.height == entry.bounds.height);
    CHECK(f.panel.scroll() > 0);
    CHECK(inner.scroll() > 0);
    const auto list = f.screen.draw_list();
    REQUIRE(list);
    CHECK(list->commands.size() < 40);
    const auto before = f.panel.scroll();
    target.reveal();
    CHECK(f.panel.scroll() == before);
}

TEST_CASE("Focused ordinary controls pass document clipboard and history shortcuts through", "[ui][input]") {
    Fixture f;
    auto button = f.panel.button("Add keyframe");
    auto checkbox = f.panel.checkbox("Key");
    auto field = f.panel.text_input("Value");
    f.pump();
    for (auto key : {Key::c,Key::v,Key::z,Key::y}) {
        button.focus();
        CHECK(f.pump({Fixture::key(key,true)}).events.size() == 1);
        checkbox.focus();
        CHECK(f.pump({Fixture::key(key,true)}).events.size() == 1);
        CHECK(checkbox.isFocused());
        field.focus();
        CHECK(f.pump({Fixture::key(key,true)}).events.empty());
    }
}

TEST_CASE("Unused ordinary-control keys bubble while text input and popups remain exclusive", "[ui][input]") {
    Fixture f;
    auto button=f.panel.button("Undo");
    auto slider=f.panel.slider("Value",0,1).value(.5F);
    auto text=f.panel.text_input("Name");
    auto dropdown=f.panel.dropdown<int>("Mode",{{0,"First"},{1,"Second"}});
    f.click(button);
    REQUIRE(f.pump({Fixture::key(Key::g)}).unhandled().size()==1);
    CHECK_FALSE(button.clicked());
    f.click(slider);
    REQUIRE(f.pump({Fixture::key(Key::r)}).unhandled().size()==1);
    f.click(text);
    CHECK(f.pump({Fixture::key(Key::s)}).unhandled().empty());
    f.click(dropdown);
    CHECK(f.pump({Fixture::key(Key::g)}).unhandled().empty());
}

TEST_CASE("UI polling separates held state from non-consuming activation", "[ui]") {
    Fixture f;
    auto button = f.panel.button("Restart");
    const auto p = f.center(button);
    f.pump({Fixture::mouse(EventKind::pointer_down, p)});
    CHECK(button.isPressed());
    CHECK(button.isHovered());
    CHECK(f.panel.isHovered());
    CHECK_FALSE(button.clicked());
    f.pump({Fixture::mouse(EventKind::pointer_up, p)});
    CHECK_FALSE(button.isPressed());
    CHECK(button.clicked());
    CHECK(button.clicked());
    REQUIRE(f.screen.draw_list());
    CHECK(button.clicked());
    f.pump();
    CHECK_FALSE(button.clicked());
    f.pump({Fixture::mouse(EventKind::pointer_down, p),
            Fixture::mouse(EventKind::pointer_move, {700, 500})});
    CHECK(button.isPressed());
    CHECK_FALSE(button.isHovered());
    auto released = f.pump({Fixture::mouse(EventKind::pointer_up, {700, 500})});
    CHECK_FALSE(button.clicked());
    CHECK(released.unhandled().empty());
    button.focus();
    f.pump({Fixture::key(Key::space)});
    CHECK(button.isPressed());
    f.pump({Fixture::key(Key::space, false, false, EventKind::key_up)});
    CHECK(button.clicked());
}
TEST_CASE("Reapplying an unchanged slider range preserves a live drag", "[ui][slider][regression]") {
    Fixture f;
    auto slider = f.panel.slider("Zoom", 1, 10).value(2);
    const auto start = f.center(slider);
    f.pump({Fixture::mouse(EventKind::pointer_down, start)});
    const auto changed = slider.changedValue();
    REQUIRE(changed);
    slider.range(1, 10);
    CHECK(slider.isPressed());
    CHECK(slider.changedValue() == changed);
    CHECK_FALSE(slider.editCancelled());
    f.pump({Fixture::mouse(EventKind::pointer_move, {start.x + 20, start.y})});
    CHECK(slider.value() > *changed);
    f.pump({Fixture::mouse(EventKind::pointer_up, {start.x + 20, start.y})});
    CHECK(slider.editCommitted());
    f.pump({Fixture::mouse(EventKind::pointer_down, start)});
    slider.range(1, 20);
    CHECK_FALSE(slider.isPressed());
    CHECK(slider.editCancelled()); // A genuinely different range still cancels.
}

TEST_CASE("Splitters drag relatively while their own border moves and keep capture outside the panel", "[ui][splitter]") {
    Fixture f;
    auto upper = f.panel.column().height(120).padding(0);
    auto split = f.panel.splitter("Resize panes").range(40, 300).value(120);
    auto lower = f.panel.column().height(200).padding(0);
    const auto start = f.center(split);
    auto result = f.pump({f.mouse(EventKind::pointer_down, start)});
    CHECK(result.events.empty()); CHECK(result.capturesPointer); CHECK(result.capturesShortcuts);
    CHECK(split.editStarted()); CHECK(split.value() == 120.F); CHECK_FALSE(split.changedValue());
    f.pump({f.mouse(EventKind::pointer_move, {start.x, start.y+25})});
    CHECK(split.value() == 145.F);
    upper.height(split.value()); lower.height(320-split.value());
    split.range(40, 300); // Repeated layout synchronization must not end capture.
    f.pump({f.mouse(EventKind::pointer_move, {start.x, start.y+55})});
    CHECK(split.isPressed()); CHECK(split.value() == 175.F);
    upper.height(split.value());
    result = f.pump({f.mouse(EventKind::pointer_up, {700, start.y+90})});
    CHECK(split.value() == 210.F); CHECK(split.editCommitted()); CHECK_FALSE(split.isPressed());
    CHECK(result.events.empty());
    const auto tree = f.screen.inspect(); REQUIRE(tree);
    const auto& inspected = widget(*tree,ui::WidgetRole::splitter,"Resize panes");
    CHECK(inspected.number == 210.F); CHECK(inspected.minimum == 40.F); CHECK(inspected.maximum == 300.F);
    CHECK(inspected.bounds.height == 10.F);
    const auto draw = f.screen.draw_list(); REQUIRE(draw);
    CHECK(std::ranges::none_of(draw->commands, [](const auto& command) {
        const auto* text = std::get_if<ui::TextDraw>(&command);
        return text && text->text.find("Resize panes") != std::string::npos;
    })); // This is a thin grip, not a numeric slider or captioned button.
}

TEST_CASE("Splitters clamp cancel and support horizontal and keyboard resizing", "[ui][splitter]") {
    for (const auto axis : {ui::SplitAxis::x, ui::SplitAxis::y}) {
        Fixture f;
        auto split = f.panel.splitter("Resize", axis).range(50, 250).value(100);
        const auto start = f.center(split);
        const auto end = axis == ui::SplitAxis::x ? Vec2{start.x+800,start.y} : Vec2{start.x,start.y+800};
        for (const auto cancel : {EventKind::key_down, EventKind::focus_lost}) {
            f.pump({f.mouse(EventKind::pointer_down,start),f.mouse(EventKind::pointer_move,end)});
            CHECK(split.value() == 250.F); CHECK(split.isPressed());
            const auto result = f.pump({input::Event{.kind=cancel,.key=Key::escape}});
            CHECK(split.value() == 100.F); CHECK(split.editCancelled()); CHECK(split.changedValue());
            CHECK_FALSE(split.isPressed());
            f.pump();
        }
        split.focus();
        f.pump({f.key(Key::down)}); CHECK(split.value() == 101.F); CHECK(split.editCommitted());
        f.pump({f.key(Key::up,false,true)}); CHECK(split.value() == 91.F);
        f.pump({f.key(Key::home)}); CHECK(split.value() == 50.F);
        f.pump({f.key(Key::end)}); CHECK(split.value() == 250.F);
        f.pump({f.mouse(EventKind::pointer_down,start),f.mouse(EventKind::pointer_move,end)});
        split.visible(false); CHECK_FALSE(split.isPressed()); CHECK(split.editCancelled());
        CHECK_THROWS_AS(split.range(4,4),std::invalid_argument);
        CHECK_THROWS_AS(split.value(std::numeric_limits<f32>::quiet_NaN()),std::invalid_argument);
    }
}

TEST_CASE("Slider motion batching preserves release cancellation and unhandled motion",
          "[ui][slider][regression]") {
    Fixture f;
    auto slider = f.panel.slider("Amount", 0, 10).value(2);
    const auto start = f.center(slider);
    const Vec2 right{start.x+200,start.y}, outside{700,500};
    SECTION("Release commits its own position, not a later uncaptured sample") {
        const auto result = f.pump({Fixture::mouse(EventKind::pointer_down,start),
            Fixture::mouse(EventKind::pointer_move,right),
            Fixture::mouse(EventKind::pointer_move,start),
            Fixture::mouse(EventKind::pointer_up,right),
            Fixture::mouse(EventKind::pointer_move,outside),
            Fixture::mouse(EventKind::pointer_move,{710,500})});
        CHECK(slider.value()==10.F);
        CHECK(slider.editCommitted());
        CHECK_FALSE(slider.isPressed());
        REQUIRE(result.unhandled().size()==2);
        CHECK(result.unhandled()[0].position==outside);
        CHECK(result.unhandled()[1].position==Vec2{710,500});
    }
    SECTION("Escape remains an ordering barrier inside a motion burst") {
        const auto result = f.pump({Fixture::mouse(EventKind::pointer_down,start),
            Fixture::mouse(EventKind::pointer_move,right),
            Fixture::mouse(EventKind::pointer_move,start),
            Fixture::key(Key::escape),
            Fixture::mouse(EventKind::pointer_move,outside),
            Fixture::mouse(EventKind::pointer_move,{710,500})});
        CHECK(slider.value()==2.F);
        CHECK(slider.editCancelled());
        CHECK_FALSE(slider.isPressed());
        REQUIRE(result.unhandled().size()==2);
    }
}

TEST_CASE("Captured scrollbar motion lays out the latest sample, not every queued sample",
          "[ui][scrollbar][performance]") {
    struct Theme final : ui::Theme {
        ui::ThemeValues style{ui::dark_theme_values(font())};
        mutable u32 accesses{};
        const ui::ThemeValues& values() const noexcept override { ++accesses; return style; }
    };
    auto theme = std::make_shared<Theme>();
    ui::Screen screen{theme};
    auto panel = screen.column().width(400).height(500).scrollbar(ui::ScrollBar::always);
    for (int i = 0; i < 1000; ++i) panel.button("Item").height(32);
    input::Frame frame{.logical_size={800,600}, .framebuffer={800,600}};
    REQUIRE(screen.update(frame, 0));
    const auto inspected = screen.inspect();
    REQUIRE(inspected);
    const auto& bar = widget(*inspected, ui::WidgetRole::scrollbar, "Vertical scroll");
    REQUIRE(bar.thumb);
    const Vec2 start{bar.thumb->x + 3, bar.thumb->y + 3};
    frame.pointer = start;
    frame.events = {Fixture::mouse(EventKind::pointer_down, start)};
    REQUIRE(screen.update(frame, 0));
    frame.pointer.y += 1;
    frame.events = {Fixture::mouse(EventKind::pointer_move, frame.pointer)};
    theme->accesses = 0;
    REQUIRE(screen.update(frame, 0));
    const auto single_sample_cost = theme->accesses;
    REQUIRE(single_sample_cost > 0);
    frame.events.clear();
    for (int i = 0; i < 128; ++i) {
        frame.pointer.y += 1;
        frame.events.push_back(Fixture::mouse(EventKind::pointer_move, frame.pointer));
    }
    theme->accesses = 0;
    REQUIRE(screen.update(frame, 0));
    const auto burst_cost = theme->accesses;
    INFO("Single sample: " << single_sample_cost << "; burst: " << burst_cost);
    CHECK(burst_cost < single_sample_cost * 3);
    const auto travel = bar.bounds.height - bar.thumb->height;
    CHECK(std::abs(panel.scroll() - 129.F / travel * panel.scroll_limit()) < .1F);
    frame.events = {Fixture::mouse(EventKind::pointer_up, frame.pointer)};
    REQUIRE(screen.update(frame, 0));
}

TEST_CASE("UI sliders report reversible pointer gestures and keyboard commits", "[ui]") {
    Fixture f;
    auto slider = f.panel.slider("Emission", 0, 10).value(2).step(.5F);
    const auto center = f.center(slider);
    f.pump({Fixture::mouse(EventKind::pointer_down, center)});
    CHECK(slider.editStarted());
    CHECK(slider.editStarted());
    REQUIRE(slider.changedValue());
    CHECK(slider.value() == 5.0F);
    CHECK(slider.isPressed());
    CHECK_FALSE(slider.editCommitted());
    f.pump({Fixture::mouse(EventKind::pointer_move, {700, center.y})});
    CHECK(slider.value() == 10.0F);
    CHECK_FALSE(slider.editStarted());
    f.pump({Fixture::key(Key::escape)});
    CHECK(slider.value() == 2.0F);
    CHECK(slider.editCancelled());
    CHECK(slider.changedValue());
    CHECK_FALSE(slider.isPressed());
    CHECK_FALSE(slider.editCommitted());
    CHECK(f.pump({Fixture::mouse(EventKind::pointer_up, {700, center.y})}).unhandled().empty());
    CHECK_FALSE(slider.editCancelled());
    f.pump({Fixture::mouse(EventKind::pointer_down, center),
            Fixture::mouse(EventKind::pointer_up, {700, center.y})});
    CHECK(slider.value() == 10.0F);
    CHECK(slider.editStarted());
    CHECK(slider.editCommitted());
    REQUIRE(f.screen.draw_list());
    CHECK(slider.editCommitted());
    f.pump({Fixture::key(Key::left)});
    CHECK(slider.value() == 9.5F);
    CHECK(slider.editStarted());
    CHECK(slider.editCommitted());
    f.pump({Fixture::key(Key::home)});
    CHECK(slider.value() == 0.0F);
    f.pump({Fixture::key(Key::right, false, true)});
    CHECK(slider.value() == 5.0F);
    f.pump({Fixture::mouse(EventKind::pointer_down, {700, center.y})});
    slider.value(100);
    CHECK(slider.value() == 10.0F);
    CHECK_FALSE(slider.changedValue());
    slider.value(3);
    f.pump({Fixture::mouse(EventKind::pointer_down, center),
            input::Event{.kind = EventKind::focus_lost}});
    CHECK(slider.editCancelled());
    CHECK(slider.value() == 3.0F);
    CHECK_THROWS_AS(slider.range(1, 1), std::invalid_argument);
    CHECK_THROWS_AS(slider.range(10, 0), std::invalid_argument);
    CHECK_THROWS_AS(slider.value(std::numeric_limits<float>::quiet_NaN()), std::invalid_argument);
    CHECK_THROWS_AS(slider.step(-1), std::invalid_argument);
}
TEST_CASE("UI image views own immutable snapshots but pass viewport input through", "[ui]") {
    Fixture f;
    auto image = f.panel.image().height(100);
    auto pixels = std::make_shared<gfx::ImageData>(
        gfx::ImageData{{1, 1}, {std::byte{128}, std::byte{128}, std::byte{128}, std::byte{255}}});
    image.image(pixels, 7);
    auto position = f.center(image);
    auto update = f.pump({Fixture::mouse(EventKind::pointer_down, position),
                          Fixture::mouse(EventKind::pointer_up, position)});
    CHECK(update.unhandled().size() == 2);
    CHECK_FALSE(update.capturesPointer);
    CHECK_FALSE(update.capturesKeyboard);
    CHECK(image.isHovered());
    CHECK_FALSE(image.isPressed());
    image.focus();
    CHECK_FALSE(image.isFocused());
    auto before = f.screen.draw_list();
    REQUIRE(before);
    const auto* draw = std::get_if<ui::ImageDraw>(&before->commands.back());
    REQUIRE(draw);
    CHECK(draw->pixels == pixels);
    CHECK(draw->revision == 7);
    auto updated = std::make_shared<gfx::ImageData>(*pixels);
    updated->pixels[0] = std::byte{255};
    image.image(updated);
    auto after = f.screen.draw_list();
    REQUIRE(after);
    const auto& next = std::get<ui::ImageDraw>(after->commands.back());
    CHECK(next.revision == 8);
    CHECK(next.identity == draw->identity);
    CHECK(next.pixels != draw->pixels);
    CHECK(draw->pixels->pixels[0] == std::byte{128});
    const std::weak_ptr<const gfx::ImageData> retained = updated;
    updated.reset();
    after = ui::DrawList{};
    image.remove();
    CHECK(retained.expired());
    CHECK(draw->pixels->extent == Extent2D{1, 1});
    auto invalid = std::make_shared<gfx::ImageData>();
    CHECK_THROWS_AS(f.panel.image().image(invalid), std::invalid_argument);
}
TEST_CASE("UI focus disabled ancestors removal and lost focus cancel interaction", "[ui]") {
    Fixture f;
    auto a = f.panel.button("A");
    auto b = f.panel.button("B");
    f.pump();
    f.pump({Fixture::key(Key::tab)});
    CHECK(a.isFocused());
    f.pump({Fixture::key(Key::tab)});
    CHECK(b.isFocused());
    f.pump({Fixture::key(Key::tab, false, true)});
    CHECK(a.isFocused());
    const auto p = f.center(a);
    f.pump({Fixture::mouse(EventKind::pointer_down, p)});
    f.panel.enabled(false);
    CHECK_FALSE(a.isPressed());
    CHECK_FALSE(a.isFocused());
    f.pump({Fixture::mouse(EventKind::pointer_up, p)});
    CHECK_FALSE(a.clicked());
    f.panel.enabled(true);
    f.click(a);
    CHECK(a.clicked());
    f.pump({Fixture::mouse(EventKind::pointer_down, p), input::Event{.kind = EventKind::focus_lost},
            Fixture::mouse(EventKind::pointer_up, p)});
    CHECK_FALSE(a.clicked());
    CHECK_FALSE(a.isPressed());
    CHECK_FALSE(a.isFocused());
    f.panel.remove();
    CHECK_FALSE(a.valid());
    CHECK_FALSE(b.valid());
    CHECK_FALSE(a.clicked());
    CHECK_THROWS_AS(a.text("invalid"), std::logic_error);
    ui::Button expired;
    {
        ui::Screen other{ui::dark_theme(font())};
        expired = other.column().button("gone");
    }
    CHECK_FALSE(expired.valid());
    CHECK_FALSE(expired.isHovered());
}
TEST_CASE("UI slots survive more than a lifetime of widget creation without reviving old handles", "[ui][storage]") {
    Fixture f;
    auto old=f.panel.button("original");f.pump();
    const auto before=f.screen.inspect();REQUIRE(before);
    const auto old_id=widget(*before,ui::WidgetRole::button,"original").id;
    old.remove();
    REQUIRE_NOTHROW([&] {
        for(unsigned i=0;i<70000;++i) {
            auto temporary=f.panel.button("temporary");
            if(i%4096==0)f.pump();
            temporary.remove();
        }
    }());
    auto replacement=f.panel.button("replacement");f.pump();
    CHECK_FALSE(old.valid());CHECK_FALSE(old.clicked());CHECK_FALSE(old.isHovered());
    CHECK_THROWS_AS(old.text("wrong widget"),std::logic_error);
    CHECK_THROWS_AS(old.remove(),std::logic_error);
    auto after=f.screen.inspect();REQUIRE(after);CHECK(after->widgets.size()==3);
    CHECK(widget(*after,ui::WidgetRole::button,"replacement").id!=old_id);
    f.click(replacement);CHECK(replacement.clicked());CHECK_FALSE(old.clicked());
}
TEST_CASE("Recycled UI slots retain creation order and ancestor enabled state", "[ui][storage]") {
    Fixture f;
    auto low=f.panel.button("old low slot");
    auto high=f.panel.column();
    low.remove();high.remove();
    // LIFO reuse: parent gets the higher slot, child gets the lower one.
    auto parent=f.panel.column().enabled(false);
    auto child=parent.button("new child");f.pump();
    auto inspection=f.screen.inspect();REQUIRE(inspection);
    REQUIRE(inspection->widgets.size()==4);
    const auto& p=inspection->widgets[2];const auto& c=inspection->widgets[3];
    CHECK(p.role==ui::WidgetRole::column);CHECK(c.label=="new child");
    CHECK(c.parent==p.id);CHECK_FALSE(c.enabled);
    CHECK_THROWS_AS(high.button("stale parent"),std::logic_error);
    parent.enabled(true);parent.visible(false);
    inspection=f.screen.inspect();REQUIRE(inspection);
    CHECK(widget(*inspection,ui::WidgetRole::button,"new child").enabled);
    CHECK_FALSE(widget(*inspection,ui::WidgetRole::button,"new child").in_layout);
    parent.visible(true);f.click(child);CHECK(child.clicked());
    parent.remove();CHECK_FALSE(parent.valid());CHECK_FALSE(child.valid());
    auto again=f.panel.button("reuse subtree");f.click(again);CHECK(again.clicked());
    CHECK_THROWS_AS(child.focus(),std::logic_error);
}
TEST_CASE("Widget reuse cannot inherit pointer capture keyboard focus or dropdown popup", "[ui][storage]") {
    Fixture f;
    auto button=f.panel.button("old button");const auto point=f.center(button);
    f.pump({Fixture::mouse(EventKind::pointer_down,point)});REQUIRE(button.isPressed());
    button.remove();auto next=f.panel.button("new button");
    f.pump({Fixture::mouse(EventKind::pointer_up,point)});
    CHECK_FALSE(next.clicked());CHECK_FALSE(next.isFocused());CHECK_FALSE(next.isPressed());
    next.focus();f.pump({Fixture::key(Key::space)});REQUIRE(next.isPressed());
    next.remove();auto field=f.panel.text_input("replacement input");
    f.pump({Fixture::key(Key::space,false,false,EventKind::key_up),Fixture::text("stale")});
    CHECK_FALSE(field.isFocused());CHECK(field.getText().empty());
    field.remove();
    auto choices=f.panel.dropdown<int>("Old choices",{{0,"Zero"},{1,"One"}});
    f.click(choices);auto inspection=f.screen.inspect();REQUIRE(inspection);
    REQUIRE(widget(*inspection,ui::WidgetRole::dropdown,"Old choices").expanded);
    const auto old_option=widget(*inspection,ui::WidgetRole::option,"Zero").id;
    choices.remove();
    auto other=f.panel.dropdown<int>("New choices",{{0,"Zero"},{1,"One"}});f.pump();
    inspection=f.screen.inspect();REQUIRE(inspection);
    CHECK_FALSE(widget(*inspection,ui::WidgetRole::dropdown,"New choices").expanded);
    CHECK(std::ranges::none_of(inspection->widgets,[](const auto& w){return w.role==ui::WidgetRole::option;}));
    f.click(other);inspection=f.screen.inspect();REQUIRE(inspection);
    const auto& option=widget(*inspection,ui::WidgetRole::option,"Zero");
    CHECK(option.id!=old_option);
    CHECK(option.parent==widget(*inspection,ui::WidgetRole::dropdown,"New choices").id);
    std::set<u64> ids;for(const auto& w:inspection->widgets)CHECK(ids.insert(w.id).second);
    CHECK_FALSE(choices.valid());CHECK_THROWS_AS(choices.value(1),std::logic_error);
}
TEST_CASE("Removing a large UI subtree releases its resources and leaves live traversal usable", "[ui][storage]") {
    Fixture f;
    auto subtree=f.panel.column();
    std::vector<ui::Button> handles;
    for(unsigned i=0;i<4096;++i)handles.push_back(subtree.button("temporary"));
    subtree.remove();
    for(const auto& handle:handles)CHECK_FALSE(handle.valid());
    f.screen.set_theme(ui::dark_theme(font()));
    f.input.logical_size={900,700};f.input.framebuffer={900,700};
    auto remaining=f.panel.button("remaining");f.pump();
    auto inspection=f.screen.inspect();REQUIRE(inspection);CHECK(inspection->widgets.size()==3);
    REQUIRE(f.screen.draw_list());f.click(remaining);CHECK(remaining.clicked());
}
TEST_CASE("Deferred visibility updates cancel interaction even when reenabled before layout",
          "[ui][regression]") {
    Fixture f;
    auto button = f.panel.button("Press");
    const auto point = f.center(button);
    f.pump({Fixture::mouse(EventKind::pointer_move, point),
            Fixture::mouse(EventKind::pointer_down, point)});
    REQUIRE(button.isPressed());
    REQUIRE(button.isFocused());
    SECTION("hide then show") {
        f.panel.visible(false);
        CHECK_FALSE(button.isHovered());
        CHECK_FALSE(button.isFocused());
        CHECK_FALSE(button.isPressed());
        f.panel.visible(true);
    }
    SECTION("disable then enable") {
        f.panel.enabled(false);
        CHECK_FALSE(button.isFocused());
        CHECK_FALSE(button.isPressed());
        f.panel.enabled(true);
    }
    CHECK_FALSE(button.isFocused());
    CHECK_FALSE(button.isPressed());
    auto result = f.pump({Fixture::mouse(EventKind::pointer_up, point)});
    CHECK(result.events.empty());
    CHECK_FALSE(button.clicked());
    f.click(button);
    CHECK(button.clicked());
}
TEST_CASE("UI values report false and empty changes rather than truthiness", "[ui]") {
    Fixture f;
    auto check = f.panel.checkbox("Enabled");
    f.click(check);
    REQUIRE(check.changedValue());
    CHECK(*check.changedValue());
    CHECK(check.changedValue());
    f.click(check);
    REQUIRE(check.changedValue());
    CHECK_FALSE(*check.changedValue());
    f.pump();
    CHECK_FALSE(check.changedValue());
    check.value(true);
    CHECK_FALSE(check.changedValue());
    auto field = f.panel.text_input("Name").value("Kestrel");
    f.pump();
    field.focus();
    f.pump({Fixture::key(Key::a, true), Fixture::key(Key::backspace)});
    REQUIRE(field.changedText());
    CHECK(field.changedText()->empty());
    CHECK(field.getText().empty());
    f.pump({Fixture::key(Key::enter)});
    REQUIRE(field.submittedText());
    CHECK(field.submittedText()->empty());
    f.pump();
    CHECK_FALSE(field.changedText());
    CHECK_FALSE(field.submittedText());
}
TEST_CASE("UI Escape reports text cancellation without choosing draft policy", "[ui][text]") {
    Fixture f;
    auto field = f.panel.text_input("Amount").value("1.");
    f.pump();
    field.focus();
    const auto result = f.pump({Fixture::key(Key::escape)});
    CHECK(result.unhandled().empty());
    CHECK(field.editCancelled());
    CHECK_FALSE(field.isFocused());
    CHECK(field.getText() == "1.");
    f.pump();
    CHECK_FALSE(field.editCancelled());
}

TEST_CASE("Text fields reveal complete paths when first layout or resizing makes room",
          "[ui][text][layout][regression]") {
    Fixture f;
    f.panel.width(110);
    const std::string path = "/home/luke/Desktop/GameAndEngine/build/examples/assets";
    auto field = f.panel.text_input("Path").value(path);
    field.focus(); // A modal may open before its final full-width layout exists.
    SECTION("Final width arrives before the first frame") {
        f.panel.width(760);
        f.pump();
    }
    SECTION("An already visible focused field grows") {
        f.pump();
        const auto narrow = f.drawn_text(path);
        CHECK(narrow.position.x < narrow.clip.x);
        f.panel.width(760);
        f.pump();
    }
    SECTION("An unfocused field grows after keeping its earlier scroll position") {
        auto other = f.panel.button("Other");
        f.pump();
        const auto narrow = f.drawn_text(path);
        CHECK(narrow.position.x < narrow.clip.x);
        other.focus();
        f.panel.width(760);
        f.pump();
    }
    const auto full = f.drawn_text(path);
    CHECK(std::abs(full.position.x - full.clip.x) < .001F);
    const auto measured = full.font.measure(path, static_cast<u32>(full.size));
    REQUIRE(measured);
    CHECK(full.position.x + measured->width <= full.clip.x + full.clip.width);
    CHECK(field.getText() == path);
    CHECK_FALSE(field.changedText());
    CHECK_FALSE(field.submittedText());
}

TEST_CASE("Text field resizing keeps caret visible without changing the selection",
          "[ui][text][layout][regression]") {
    Fixture f;
    f.panel.width(760);
    const std::string original = "A sufficiently long filename for narrow input fields.vmesh";
    auto field = f.panel.text_input("Path").value(original);
    f.pump();
    field.focus();
    f.panel.width(110);
    f.pump();
    const auto narrow = f.drawn_text(original);
    CHECK(narrow.position.x < narrow.clip.x);
    const auto draws = f.screen.draw_list();
    REQUIRE(draws);
    bool caret_visible{};
    for (const auto& command : draws->commands)
        if (const auto* box = std::get_if<ui::BoxDraw>(&command); box && box->rect.width == 1.5F)
            caret_visible |= box->rect.x >= box->clip.x &&
                             box->rect.x + box->rect.width <= box->clip.x + box->clip.width;
    CHECK(caret_visible);
    f.pump({Fixture::key(Key::left, false, true), Fixture::key(Key::left, false, true)});
    CHECK_FALSE(field.changedText());
    f.panel.width(760);
    f.pump();
    const auto full = f.drawn_text(original);
    CHECK(std::abs(full.position.x - full.clip.x) < .001F);
    CHECK(field.isFocused());
    CHECK_FALSE(field.changedText());
    f.pump({Fixture::text("XX")});
    CHECK(field.getText() == original.substr(0, original.size() - 2) + "XX");
    f.pump({Fixture::key(Key::z, true)});
    CHECK(field.getText() == original);
}

TEST_CASE("Growing a multiline text area removes obsolete vertical scrolling",
          "[ui][text][layout][regression]") {
    Fixture f;
    const std::string original = "First line\nSecond line\nThird line\nLast line";
    auto field = f.panel.text_area("Notes").height(44).value(original);
    f.pump();
    field.focus();
    const auto short_view = f.drawn_text(original);
    CHECK(short_view.position.y < short_view.clip.y);
    field.height(180);
    f.pump();
    const auto full = f.drawn_text(original);
    CHECK(std::abs(full.position.y - full.clip.y) < .001F);
    CHECK_FALSE(field.changedText());
    f.pump({Fixture::key(Key::backspace)});
    CHECK(field.getText() == original.substr(0, original.size() - 1));
}

TEST_CASE("UI Delete reaches selection shortcuts except during text gestures and popups", "[ui]") {
    Fixture f;
    auto button = f.panel.button("Selected keyframe");
    auto slider = f.panel.slider("Amount", 0, 10);
    auto field = f.panel.text_input().value("12");
    auto choice = f.panel.dropdown<int>("Choice", {{0, "First"}, {1, "Second"}});
    f.pump();
    button.focus();
    auto result = f.pump({Fixture::key(Key::del)});
    REQUIRE(result.unhandled().size() == 1);
    CHECK(result.unhandled()[0].key == Key::del);
    slider.focus();
    result = f.pump({Fixture::key(Key::del)});
    CHECK(result.unhandled().size() == 1);
    CHECK(f.pump({Fixture::key(Key::del, true)}).unhandled().empty());
    field.focus();
    result = f.pump({Fixture::key(Key::a, true), Fixture::key(Key::del)});
    CHECK(result.unhandled().empty());
    CHECK(field.getText().empty());
    f.click(choice);
    CHECK(f.pump({Fixture::key(Key::del)}).unhandled().empty());
    f.pump({Fixture::key(Key::escape)});
    const auto point = f.center(slider);
    f.pump({Fixture::mouse(EventKind::pointer_down, point)});
    CHECK(slider.isPressed());
    CHECK(f.pump({Fixture::key(Key::del)}).unhandled().empty());
}
TEST_CASE("UI text editing uses Unicode graphemes clipboard selection undo and submission",
          "[ui][text]") {
    Fixture f;
    auto field = f.panel.text_input("Name").value("Aé👩‍🚀é");
    f.pump();
    field.focus();
    f.pump({Fixture::key(Key::backspace)});
    CHECK(field.getText() == "Aé👩‍🚀");
    f.pump({Fixture::key(Key::backspace)});
    CHECK(field.getText() == "Aé");
    f.pump({Fixture::key(Key::z, true)});
    CHECK(field.getText() == "Aé👩‍🚀");
    f.pump({Fixture::key(Key::y, true)});
    CHECK(field.getText() == "Aé");
    f.pump({Fixture::key(Key::left), Fixture::key(Key::del)});
    CHECK(field.getText() == "A");
    struct Clipboard : input::Clipboard {
        std::string text;
        std::string read() override { return text; }
        void write(std::string_view s) override { text = s; }
    } clipboard;
    f.pump({Fixture::key(Key::a, true), Fixture::key(Key::x, true)}, &clipboard);
    CHECK(clipboard.text == "A");
    CHECK(field.getText().empty());
    clipboard.text = "Kestrel\nII";
    f.pump({Fixture::key(Key::v, true)}, &clipboard);
    CHECK(field.getText() == "Kestrel II");
    auto text_area = f.panel.text_area("Notes");
    f.pump();
    text_area.focus();
    f.pump({Fixture::text("alpha"), Fixture::key(Key::enter), Fixture::text("beta")});
    CHECK(text_area.getText() == "alpha\nbeta");
    CHECK_FALSE(text_area.submittedText());
    f.pump({Fixture::key(Key::enter, true)});
    REQUIRE(text_area.submittedText());
    CHECK_THROWS_AS(field.value(std::string("\xff", 1)), std::invalid_argument);
    const auto result = f.pump({Fixture::key(Key::w), Fixture::text("w")});
    CHECK(result.capturesKeyboard);
    CHECK(result.unhandled().empty());
}
TEST_CASE("UI typed dropdown popups escape panel clips and prevent click through", "[ui]") {
    enum class Quality { low, high };
    Fixture f;
    auto select =
        f.panel.dropdown<Quality>("Quality", {{Quality::low, "Low"}, {Quality::high, "High"}});
    f.panel.height(52);
    auto under = f.screen.column().position({20, 76}).width(250).padding(0).button("under popup");
    f.click(select);
    auto draws = f.screen.draw_list();
    REQUIRE(draws);
    const auto r = select.bounds();
    const Vec2 second{r.x + 20, r.y + r.height + 36 + 10};
    f.pump({Fixture::mouse(EventKind::pointer_down, second),
            Fixture::mouse(EventKind::pointer_up, second)});
    REQUIRE(select.changedValue());
    CHECK(*select.changedValue() == Quality::high);
    CHECK_FALSE(under.clicked());
    select.focus();
    f.pump({Fixture::key(Key::enter), Fixture::key(Key::up), Fixture::key(Key::enter)});
    REQUIRE(select.changedValue());
    CHECK(select.value() == Quality::low);
    f.pump();
    CHECK_FALSE(select.changedValue());
    f.click(select);
    const auto u = under.bounds();
    const Vec2 outside{700, 500};
    f.pump({Fixture::mouse(EventKind::pointer_down, outside),
            Fixture::mouse(EventKind::pointer_up, outside)});
    CHECK_FALSE(under.clicked());
    CHECK_FALSE(select.changedValue());
    CHECK(u.width > 0.0F);
}
TEST_CASE("UI layout scrolling themes owned draw lists and display scale", "[ui]") {
    Fixture f;
    f.panel.height(100);
    auto first = f.panel.button("first");
    for (int i = 0; i < 8; ++i)
        f.panel.button("another");
    auto last = f.panel.button("last");
    f.pump();
    const auto initial = first.bounds();
    f.panel.scroll(1000);
    f.pump();
    CHECK(last.bounds().y < 120.0F);
    CHECK(first.bounds().y < initial.y);
    auto row = f.screen.row().position({300, 20}).padding(0).gap(10);
    auto a = row.button("a").width(80);
    auto b = row.button("b").width(90);
    f.pump();
    CHECK(b.bounds().x == a.bounds().x + 90.0F);
    auto label = row.label("owned");
    auto saved = f.screen.draw_list();
    REQUIRE(saved);
    label.text("replacement");
    bool owns_old{};
    for (const auto& c : saved->commands)
        if (auto t = std::get_if<ui::TextDraw>(&c))
            owns_old = owns_old || t->text == "owned";
    CHECK(owns_old);
    auto moved = std::move(f.screen);
    CHECK(a.valid());
    f.input.framebuffer = {1600, 1200};
    REQUIRE(moved.update(f.input, 0));
    auto scaled = moved.draw_list();
    REQUIRE(scaled);
    CHECK(scaled->framebuffer == Extent2D{1600, 1200});
    CHECK(a.bounds().x == 300.0F);
    auto style = ui::dark_theme_values(font());
    style.control_height = 50;
    moved.set_theme(std::make_shared<ui::BasicTheme>(style));
    REQUIRE(moved.update(f.input, 0));
    CHECK(a.bounds().height == 50.0F);
    f.input.overflow = true;
    CHECK_FALSE(moved.update(f.input, 0));
    CHECK_FALSE(a.isPressed());
    CHECK_FALSE(moved.update(f.input, std::numeric_limits<float>::quiet_NaN()));
}
TEST_CASE("UI dropdowns copy dynamically supplied choices", "[ui]") {
    Fixture f;
    std::vector<ui::Choice<std::string>> choices{{"mesh", "Mesh"}, {"effects", "Effects"}};
    auto select =
        f.panel.dropdown<std::string>("Layer", std::span<const ui::Choice<std::string>>{choices});
    choices.clear();
    choices.shrink_to_fit();
    select.value("effects");
    REQUIRE(select.value() == "effects");
    f.pump();
    select.focus();
    f.pump({Fixture::key(Key::enter), Fixture::key(Key::up), Fixture::key(Key::enter)});
    REQUIRE(select.changedValue() == std::optional<std::string>{"mesh"});
    REQUIRE_THROWS_AS(
        f.panel.dropdown<std::string>("Empty", std::span<const ui::Choice<std::string>>{}),
        std::invalid_argument);
}

TEST_CASE("UI focus reveals scrolled widgets and hidden ancestors clear hover", "[ui]") {
    Fixture f;
    f.panel.height(100);
    auto a = f.panel.button("first");
    for (int i = 0; i < 5; ++i)
        f.panel.button("middle");
    auto b = f.panel.button("last");
    f.pump();
    CHECK_FALSE(f.screen.root().isPressed());
    CHECK_FALSE(f.screen.root().isFocused());
    b.focus();
    CHECK(b.bounds().y + b.bounds().height <= 112.0F);
    CHECK(b.isFocused());
    a.focus();
    CHECK(a.bounds().y == 28.0F);
    f.pump({Fixture::mouse(EventKind::pointer_move, {40, 40})});
    CHECK(a.isHovered());
    f.panel.visible(false);
    CHECK_FALSE(a.isHovered());
    CHECK_FALSE(a.isFocused());
    f.panel.visible(true);
    f.input.focused = false;
    auto result = f.pump();
    CHECK_FALSE(result.capturesPointer);
    CHECK_FALSE(result.capturesKeyboard);
    f.input.focused = true;
    auto invalid =
        Fixture::mouse(EventKind::pointer_move, {std::numeric_limits<float>::quiet_NaN(), 0});
    f.input.events = {invalid};
    CHECK_FALSE(f.screen.update(f.input, 0));
}

TEST_CASE("UI processes committed text before a later focus loss in the same input queue", "[ui]") {
    Fixture f;
    auto field = f.panel.text_input().value("A");
    f.pump();
    field.focus();
    f.input.focused = false; // final window snapshot, after all queued events
    auto result =
        f.pump({Fixture::text("é"), {.kind = EventKind::focus_lost}, Fixture::text("ignored")});
    CHECK(field.getText() == "Aé");
    REQUIRE(field.changedText());
    CHECK_FALSE(field.isFocused());
    CHECK_FALSE(result.capturesKeyboard);
    CHECK_FALSE(result.capturesPointer);
}
