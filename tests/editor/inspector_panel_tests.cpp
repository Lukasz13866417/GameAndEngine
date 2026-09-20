#include "../../examples/editor/inspector_panel.hpp"
#include "../../examples/editor/number_control.hpp"
#include "../../examples/editor/translation_tool.hpp"
#include "../../examples/editor/selection.hpp"
#include "../../examples/editor/animation.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <limits>
#include <optional>

namespace {
using namespace vng;
using input::EventKind;
using input::Key;
text::Font font() {
    auto value = text::Font::load(VNG_TEST_FONT_PATH);
    REQUIRE(value);
    return *value;
}
struct Fixture {
    ui::Screen screen{ui::dark_theme(font())};
    ui::Container host = screen.column().position({10, 10}).width(330).padding(8).gap(6);
    editor_example::InspectorPanel panel{host};
    input::Frame input{.logical_size = {1000, 1600}, .framebuffer = {1000, 1600}};
    void pump(std::initializer_list<input::Event> events = {}) {
        input.events = events;
        for (const auto& event : events)
            if (event.kind == EventKind::pointer_down || event.kind == EventKind::pointer_up ||
                event.kind == EventKind::pointer_move)
                input.pointer = event.position;
        auto result = screen.update(input, .016F);
        INFO((result ? std::string{} : result.error().message));
        REQUIRE(result);
    }
    Vec2 at(std::string_view caption) {
        auto list = screen.draw_list();
        REQUIRE(list);
        for (const auto& command : list->commands)
            if (const auto* text = std::get_if<ui::TextDraw>(&command);
                text && text->text == caption)
                return {text->position.x + 2, text->position.y + 8};
        FAIL("No inspector text: " << caption);
        return {};
    }
    bool has(std::string_view caption) {
        auto list = screen.draw_list();
        REQUIRE(list);
        return std::ranges::any_of(list->commands, [&](const auto& command) {
            const auto* text = std::get_if<ui::TextDraw>(&command);
            return text && text->text == caption;
        });
    }
    void click(std::string_view caption) {
        const auto p = at(caption);
        pump({{.kind = EventKind::pointer_down, .position = p},
              {.kind = EventKind::pointer_up, .position = p}});
    }
    void enter(std::string_view original, std::string replacement, bool submit = false) {
        click(original);
        input::Event select{
            .kind = EventKind::key_down, .key = Key::a, .modifiers = {.control = true}};
        input::Event text{.kind = EventKind::text, .text = std::move(replacement)};
        if (submit)
            pump({select, text, {.kind = EventKind::key_down, .key = Key::enter}});
        else
            pump({select, text});
    }
};
struct Settings {
    bool enabled{true};
    f32 emission{2}, unbounded{12.25F};
    i32 count{7};
    u32 seed{42};
    Vec3 offset{1.25F, 2.5F, 3.75F};
    std::string name{"Original"};
};
} // namespace

TEST_CASE("Editor numbers accept precise text reject invalid drafts and cancel with Escape",
          "[editor][ui][number]") {
    Fixture f;
    editor_example::NumberControl number{f.host, "Amount", -10, 10, 2};
    f.pump();
    f.enter("2", "3.125", false);
    number.poll();
    CHECK(number.value() == 2.0F);
    CHECK_FALSE(number.changedValue());
    REQUIRE(number.read());
    CHECK(*number.read() == 3.125F);
    f.pump({{.kind = EventKind::key_down, .key = Key::enter}});
    number.poll();
    CHECK(number.value() == 3.125F);
    CHECK(number.changedValue() == 3.125F);
    CHECK(number.changedValue() == 3.125F); // Non-consuming within this frame.
    CHECK(number.editCommitted());
    f.pump();
    number.poll();
    CHECK_FALSE(number.changedValue());
    CHECK_FALSE(number.editCommitted());

    for (const auto invalid : {"-", "nan", "inf", "1e99", "11", "-11", "3oops"}) {
        f.enter("3.125", invalid, true);
        number.poll();
        CHECK(number.value() == 3.125F);
        CHECK_FALSE(number.changedValue());
        CHECK_FALSE(number.editCommitted());
        CHECK_FALSE(number.read());
        CHECK_FALSE(number.status().empty());
        CHECK(f.has(invalid));
        f.pump({{.kind = EventKind::key_down, .key = Key::escape}});
        number.poll();
        CHECK(number.status().empty());
        CHECK(f.has("3.125"));
        CHECK_FALSE(number.editing());
    }
    f.enter("3.125", " +1.25e0 ", true);
    number.poll();
    CHECK(number.value() == 1.25F);
    CHECK(number.changedValue() == 1.25F);
}

TEST_CASE("Editor numbers keep focused and unsent drafts during external synchronization",
          "[editor][ui][number]") {
    Fixture f;
    editor_example::NumberControl number{f.host, "Amount", 0, 10, 2};
    auto other = f.host.button("Other");
    f.pump();
    f.enter("2", "3.");
    number.poll();
    number.value(4);
    CHECK(f.has("3."));
    CHECK(number.value() == 4.0F);
    f.click("Other");
    number.poll();
    number.value(5);
    CHECK(f.has("3.")); // Blurring alone does not apply or erase an edit.
    f.click("3.");
    f.pump({{.kind = EventKind::key_down, .key = Key::escape}});
    number.poll();
    CHECK(f.has("5")); // Escape restores the newest authoritative value.
    CHECK(number.value() == 5.0F);
    CHECK_FALSE(number.changedValue());
    number.value(6);
    CHECK(f.has("6"));
    f.enter("6", "7.");
    number.poll();
    number.reset(8);
    CHECK(f.has("8"));
    CHECK_FALSE(number.changedValue());
    CHECK(other.valid());
}

TEST_CASE("Inspector bounded numbers retain Apply semantics and preserve incomplete drafts",
          "[editor][ui][number]") {
    Fixture f;
    Settings settings;
    editor::Inspector inspector{{1, 2, 3}};
    inspector.edit("settings", settings, "Settings")
        .slider("emission", &Settings::emission, 0, 10, "Emission")
        .apply("Apply settings", [&](const auto& next) { settings = next; });
    f.panel.show(inspector.schema());
    f.pump();
    f.enter("2", "4.125", true);
    CHECK(f.panel.poll().empty()); // Enter accepts a field, not the Apply group.
    CHECK(settings.emission == 2.0F);
    f.click("Apply settings");
    auto events = f.panel.poll();
    REQUIRE(events.size() == 1);
    REQUIRE(inspector.dispatch(events.front()));
    CHECK(settings.emission == 4.125F);
    f.panel.show(inspector.schema());
    f.pump();
    f.enter("4.125", "-");
    CHECK(f.panel.poll().empty());
    auto ack = inspector.schema();
    ++ack.stamp.revision;
    ack.controls[0].fields[0].value = 6.0F;
    f.panel.show(ack);
    CHECK(f.has("-"));
    f.click("Apply settings");
    CHECK(f.panel.poll().empty());
    CHECK_FALSE(f.panel.status().empty());
    f.enter("-", "10.125");
    f.click("Apply settings");
    CHECK(f.panel.poll().empty());
    CHECK(f.panel.status().find("between") != std::string_view::npos);
    f.enter("10.125", "nan");
    f.click("Apply settings");
    CHECK(f.panel.poll().empty());
    f.enter("nan", "5.875"); // Apply also accepts valid text without prior Enter.
    f.click("Apply settings");
    events = f.panel.poll();
    REQUIRE(events.size() == 1);
    CHECK(std::get<f32>(events.front().values.front().value) == 5.875F);
    CHECK(events.front().stamp == ack.stamp);
}
TEST_CASE("Local scale updates can precede an inspector schema with larger limits",
          "[editor][ui][number][scale-limits]") {
    Fixture f;
    Settings settings;settings.emission=1;
    editor::Inspector inspector{{1,2,3}};
    inspector.edit("transform",settings,"Instance transform")
        .slider("scale",&Settings::emission,.05F,3.F,"Scale")
        .apply("Apply transform",[](const auto&){});
    f.panel.show(inspector.schema());f.pump();
    REQUIRE_NOTHROW(f.panel.reset_number("transform","scale",10.F));
    CHECK(f.has("10"));
    CHECK(f.panel.poll().empty()); // Display synchronization is not an edit.
    f.click("Apply transform");
    CHECK(f.panel.poll().empty()); // The old schema must not accept an out-of-range edit.
    auto fresh=inspector.schema();++fresh.stamp.revision;
    fresh.controls[0].fields[0].maximum=25.F;
    fresh.controls[0].fields[0].value=10.F;
    f.panel.show(fresh);f.pump();
    CHECK(f.has("10"));
    f.click("Apply transform");
    const auto events=f.panel.poll();REQUIRE(events.size()==1);
    CHECK(events[0].stamp==fresh.stamp);
    CHECK(std::get<f32>(events[0].values[0].value)==10.F);
}

TEST_CASE("Number synchronization preserves finite values without widening edit validation",
          "[editor][ui][number][scale-limits]") {
    Fixture f;editor_example::NumberControl number{f.host,"Scale",.05F,3.F,1.F};f.pump();
    REQUIRE_NOTHROW(number.value(12.F));CHECK(number.value()==12.F);CHECK(f.has("12"));
    CHECK_FALSE(number.read());CHECK_FALSE(number.changedValue());
    f.enter("12","2",true);number.poll();
    CHECK(number.value()==2.F);REQUIRE(number.read());CHECK(*number.read()==2.F);
    REQUIRE_NOTHROW(number.reset(20.F));CHECK(f.has("20"));
    REQUIRE_NOTHROW(number.reset(1.F));REQUIRE(number.read());CHECK(*number.read()==1.F);
    CHECK_THROWS_AS(number.reset(std::numeric_limits<f32>::quiet_NaN()),std::invalid_argument);
}

TEST_CASE("Inspector live numeric controls submit only valid Enter commits",
          "[editor][ui][number]") {
    Fixture f;
    Settings settings;
    editor::Inspector inspector{{1, 2, 3}};
    inspector.edit("settings", settings)
        .slider("emission", &Settings::emission, 0, 10, "Emission")
        .live([](const auto&) {});
    f.panel.show(inspector.schema());
    f.pump();
    f.enter("2", "nan", true);
    CHECK(f.panel.poll().empty());
    CHECK_FALSE(f.panel.status().empty());
    f.enter("nan", "6.125");
    CHECK(f.panel.poll().empty());
    f.pump({{.kind = EventKind::key_down, .key = Key::enter}});
    auto events = f.panel.poll();
    REQUIRE(events.size() == 1);
    CHECK(std::get<f32>(events.front().values.front().value) == 6.125F);
    CHECK(f.panel.status().empty());
}

TEST_CASE("Acknowledged typed Scale Apply becomes a clean baseline for later revisions",
          "[editor][ui][number][regression]") {
    Fixture f;
    struct ScaleSettings { f32 scale{.7F}; } model;
    editor::Inspector inspector{{1, 2, 3}};
    inspector.edit("model", model)
        .slider("scale", &ScaleSettings::scale, .05F, 3.F, "Scale")
        .apply("Apply model", [&](const auto& next) { model = next; });
    f.panel.show(inspector.schema());
    f.pump();
    f.enter("0.7", "1.5"); // Deliberately click Apply without pressing Enter first.
    f.click("Apply model");
    auto events = f.panel.poll();
    REQUIRE(events.size() == 1);
    CHECK(events.front().control == "model");
    CHECK(events.front().values.front().key == "scale");
    CHECK(std::get<f32>(events.front().values.front().value) == 1.5F);
    REQUIRE(inspector.dispatch(events.front()));
    CHECK(model.scale == 1.5F);
    f.panel.show(inspector.schema());
    f.pump();
    CHECK(f.has("1.5"));

    // A later worker revision (for example Reset or external edits) must update
    // both the text and slider. The already-acknowledged text is not a draft.
    auto external = inspector.schema();
    ++external.stamp.revision;
    external.controls.front().fields.front().value = .8F;
    f.panel.show(external);
    f.pump();
    CHECK(f.has("0.8"));
    f.click("Apply model");
    events = f.panel.poll();
    REQUIRE(events.size() == 1);
    CHECK(std::get<f32>(events.front().values.front().value) == .8F);
}

TEST_CASE("Numeric Apply acknowledgments preserve a newer typed draft",
          "[editor][ui][number][regression]") {
    Fixture f;
    Settings settings;
    editor::Inspector inspector{{1, 2, 3}};
    inspector.edit("settings", settings)
        .slider("emission", &Settings::emission, 0.F, 10.F, "Emission")
        .apply("Apply settings", [&](const auto& next) { settings = next; });
    f.panel.show(inspector.schema());
    f.pump();
    f.enter("2", "4.125");
    f.click("Apply settings");
    const auto events = f.panel.poll();
    REQUIRE(events.size() == 1);
    REQUIRE(inspector.dispatch(events.front()));

    std::string draft;
    SECTION("unfinished invalid number") { draft = "-"; }
    SECTION("different valid number") { draft = "6.25"; }
    SECTION("unfinished number matching the old baseline") { draft = "2."; }
    f.enter("4.125", draft);
    CHECK(f.panel.poll().empty());
    f.panel.show(inspector.schema());
    f.pump();
    CHECK(f.has(draft));
}

TEST_CASE("Inspector panel submits complete typed drafts and displays parse failures",
          "[editor][ui]") {
    Fixture f;
    Settings settings;
    editor::Inspector inspector{{1, 2, 3}};
    auto edit = inspector.edit("settings", settings, "Settings");
    edit.toggle("enabled", &Settings::enabled, "Enabled");
    edit.slider("emission", &Settings::emission, 0, 10, "Emission");
    edit.field("unbounded", &Settings::unbounded, "Unbounded");
    edit.field("count", &Settings::count, "Count");
    edit.field("seed", &Settings::seed, "Seed");
    edit.field("offset", &Settings::offset, "Offset");
    edit.field("name", &Settings::name, "Name");
    edit.apply("Apply settings", [&](const Settings& next) { settings = next; });
    f.panel.show(inspector.schema());
    REQUIRE(f.panel.status().empty());
    f.pump();
    f.enter("Original", "Draft");
    CHECK(f.panel.poll().empty());
    f.click("Enabled");
    CHECK(f.panel.poll().empty());
    f.enter("12.25", "13.5");
    f.enter("7", "-4");
    f.enter("42", "43");
    f.enter("2.5", "4.5");
    f.click("Apply settings");
    auto events = f.panel.poll();
    REQUIRE(events.size() == 1);
    CHECK(events[0].stamp == editor::Stamp{1, 2, 3});
    CHECK(events[0].phase == editor::Phase::apply);
    CHECK(events[0].values.size() == 7);
    REQUIRE(inspector.dispatch(events[0]));
    CHECK(settings.name == "Draft");
    CHECK_FALSE(settings.enabled);
    CHECK(settings.unbounded == 13.5F);
    CHECK(settings.count == -4);
    CHECK(settings.seed == 43);
    CHECK(settings.offset == Vec3{1.25F, 4.5F, 3.75F});
    f.panel.show(inspector.schema());
    f.pump();
    f.enter("13.5", "nan");
    f.click("Apply settings");
    CHECK(f.panel.poll().empty());
    CHECK_FALSE(f.panel.status().empty());
    f.enter("nan", "17.5");
    f.click("Apply settings");
    events = f.panel.poll();
    REQUIRE(events.size() == 1);
    CHECK(f.panel.status().empty());
    CHECK(events[0].stamp.revision == 4);
}

TEST_CASE("Inspector panel retains unsent drafts on acknowledgements and only sends slider commits",
          "[editor][ui]") {
    Fixture f;
    Settings settings;
    editor::Inspector inspector{{4, 9, 0}};
    auto staged = inspector.edit("name", settings, "Name settings");
    staged.field("name", &Settings::name, "Name");
    staged.apply("Apply name", [](const auto&) {});
    auto live = inspector.edit("light", settings, "Light");
    live.slider("emission", &Settings::emission, 0, 10, "Emission");
    live.live([](const auto&) {});
    f.panel.show(inspector.schema());
    f.pump();
    f.enter("Original", "Unsent");
    auto ack = inspector.schema();
    ++ack.stamp.revision;
    ack.controls[1].fields[0].value = 3.0F;
    f.panel.show(ack);
    CHECK(f.has("Unsent"));
    CHECK(f.has("Emission: 3"));
    const auto p = f.at("Emission: 3");
    f.pump({{.kind = EventKind::pointer_down, .position = p},
            {.kind = EventKind::pointer_move, .position = {250, p.y}}});
    CHECK(f.panel.poll().empty());
    f.pump({{.kind = EventKind::pointer_up, .position = {250, p.y}}});
    auto events = f.panel.poll();
    REQUIRE(events.size() == 1);
    CHECK(events[0].control == "light");
    CHECK(events[0].phase == editor::Phase::apply);
    CHECK(events[0].stamp.revision == 1);
    f.panel.show(ack); // A duplicate pre-ack description must not erase pending draft identity.
    ++ack.stamp.revision;
    ack.controls[1].fields[0].value = 4.0F; // Worker may normalize accepted values.
    f.panel.show(ack);
    CHECK(f.has("Emission: 4"));
    CHECK(f.has("Unsent"));
    f.click("Apply name");
    events = f.panel.poll();
    REQUIRE(events.size() == 1);
    CHECK(std::get<std::string>(events[0].values[0].value) == "Unsent");
}

TEST_CASE("Inspector panel adapts actions gizmos and rejects malformed descriptions",
          "[editor][ui]") {
    Fixture f;
    editor::Inspector inspector{{8, 1, 0}};
    inspector.action("regenerate", [] {}, "Regenerate");
    inspector.translation_gizmo("origin", Vec3{11, 22, 33}, [](Vec3) {}, "Origin");
    f.panel.show(inspector.schema());
    f.pump();
    f.click("Regenerate");
    auto events = f.panel.poll();
    REQUIRE(events.size() == 1);
    CHECK(events[0].phase == editor::Phase::activate);
    CHECK(events[0].values.empty());
    f.enter("22", "24");
    CHECK(f.panel.poll().empty());
    f.click("Apply Origin");
    events = f.panel.poll();
    REQUIRE(events.size() == 1);
    CHECK(events[0].control == "origin");
    CHECK(events[0].phase == editor::Phase::apply);
    CHECK(events[0].values == std::vector<editor::NamedValue>{{"position", Vec3{11, 24, 33}}});
    auto invalid = inspector.schema();
    invalid.controls[0].key.clear();
    f.panel.show(invalid);
    CHECK_FALSE(f.panel.status().empty());
    CHECK(f.has("Regenerate"));
    f.panel.clear();
    CHECK(f.panel.poll().empty());
    CHECK(f.panel.status().empty());
    CHECK_FALSE(f.has("Regenerate"));
    Settings notes;
    notes.name = "Line one\nLine two";
    editor::Inspector text_inspector{{9, 1, 0}};
    auto edit = text_inspector.edit("notes", notes);
    edit.field("name", &Settings::name, "Notes");
    edit.apply("Store notes", [](const auto&) {});
    f.panel.show(text_inspector.schema());
    REQUIRE(f.panel.status().empty());
    f.pump();
    f.enter("Line one\nLine two", "Changed\nSecond");
    f.click("Store notes");
    events = f.panel.poll();
    REQUIRE(events.size() == 1);
    CHECK(std::get<std::string>(events[0].values[0].value) == "Changed\nSecond");
    auto unsupported = text_inspector.schema();
    unsupported.controls[0].fields[0].value = std::string("A\tB");
    f.panel.show(unsupported);
    CHECK_FALSE(f.panel.status().empty());
}

TEST_CASE("Translation gizmos project world axes and commit raw pointer releases once",
          "[editor][ui]") {
    gfx::Camera camera;
    camera.set_position({0, 0, 10}).look_at({0, 0, 0}).set_orthographic({.vertical_height = 10});
    const auto snapshot = camera.snapshot({640, 480});
    REQUIRE(snapshot);
    editor::Inspector inspector{{3, 4, 5}};
    inspector.translation_gizmo("position", Vec3{}, [](Vec3) {});
    constexpr ui::Rect viewport{100, 100, 640, 480};
    editor_example::TranslationTool tool;
    CHECK_FALSE(tool.update(inspector.schema(), *snapshot, viewport, {}, {}, true));
    CHECK_FALSE(tool.preview_position());
    ui::DrawList draw{{840, 680}, {840, 680}, {}};
    tool.append(draw);
    CHECK_FALSE(draw.commands.empty());
    // Orthographic projection has 48 logical pixels/world unit. Z points
    // directly at the camera, so only X/right and Y/up should have handles.
    bool red{}, green{}, blue{};
    for (const auto& command : draw.commands) {
        const auto& box = std::get<ui::BoxDraw>(command);
        red |= box.color.x == 1 && box.color.y < .1F;
        green |= box.color.y == 1 && box.color.x < .1F;
        blue |= box.color.z == 1 && box.color.x == .1F;
    }
    CHECK(red);
    CHECK(green);
    CHECK_FALSE(blue);
    const std::array down{input::Event{.kind = EventKind::pointer_down, .position = {450, 340}}};
    CHECK_FALSE(tool.update(inspector.schema(), *snapshot, viewport, {}, down, true));
    CHECK_FALSE(tool.dragging()); // UI-consumed down cannot start a viewport tool.
    CHECK_FALSE(tool.update(inspector.schema(), *snapshot, viewport, down, down, true));
    CHECK(tool.dragging());
    REQUIRE(tool.preview_position());
    CHECK(*tool.preview_position() == Vec3{});
    CHECK(tool.handledPointer());
    const std::array move{input::Event{.kind = EventKind::pointer_move, .position = {498, 340}}};
    CHECK_FALSE(tool.update(inspector.schema(), *snapshot, viewport, {}, move, true));
    CHECK(tool.dragging());
    const auto preview = tool.preview_position();
    REQUIRE(preview);
    CHECK(std::abs(preview->x - 1) < 1e-5F);
    // Observation/copying neither advances the gesture nor consumes its value.
    const auto copied_preview = preview;
    CHECK(copied_preview == tool.preview_position());
    CHECK(tool.handledPointer());
    const std::array release{input::Event{.kind = EventKind::pointer_up, .position = {498, 340}}};
    auto event = tool.update(inspector.schema(), *snapshot, viewport, {}, release, true);
    REQUIRE(event);
    CHECK(event->stamp == inspector.schema().stamp);
    CHECK(event->control == "position");
    CHECK(event->phase == editor::Phase::apply);
    REQUIRE(event->values.size() == 1);
    CHECK(event->values[0].key == "position");
    const auto translated = std::get<Vec3>(event->values[0].value);
    CHECK(std::abs(translated.x - 1) < 1e-5F);
    CHECK(translated.y == 0.0F);
    CHECK(translated.z == 0.0F);
    CHECK_FALSE(tool.dragging());
    CHECK_FALSE(tool.preview_position());
    CHECK(tool.handledPointer());
    CHECK_FALSE(tool.update(inspector.schema(), *snapshot, viewport, {}, release, true));
    CHECK_FALSE(tool.handledPointer());
}

TEST_CASE("Translation previews use the latest sample and preserve ordered terminal events",
          "[editor][ui][regression]") {
    gfx::Camera camera;
    camera.set_position({0, 0, 10}).look_at({0, 0, 0}).set_orthographic({.vertical_height = 10});
    const auto snapshot = camera.snapshot({640, 480});
    REQUIRE(snapshot);
    editor::Inspector inspector{{3, 4, 5}};
    inspector.translation_gizmo("position", Vec3{}, [](Vec3) {});
    constexpr ui::Rect viewport{100, 100, 640, 480};
    editor_example::TranslationTool tool;
    const input::Event down{.kind = EventKind::pointer_down, .position = {450, 340}};
    const input::Event first_move{.kind = EventKind::pointer_move, .position = {474, 340}};
    const input::Event last_move{.kind = EventKind::pointer_move, .position = {498, 340}};

    SECTION("down and moves in one frame expose the latest preview without an Apply") {
        const std::array events{down, first_move, last_move};
        CHECK_FALSE(tool.update(inspector.schema(), *snapshot, viewport, events, events, true));
        REQUIRE(tool.preview_position());
        CHECK(std::abs(tool.preview_position()->x - 1) < 1e-5F);
        CHECK(tool.preview_position()->y == 0.F);
        CHECK(tool.preview_position()->z == 0.F);
        // Empty input is inert and keeps the current preview available.
        const auto preview = tool.preview_position();
        CHECK_FALSE(tool.update(inspector.schema(), *snapshot, viewport, {}, {}, true));
        CHECK(tool.preview_position() == preview);

        const std::array moves{
            input::Event{.kind = EventKind::pointer_move, .position = {522, 340}},
            input::Event{.kind = EventKind::pointer_move, .position = {546, 340}}};
        CHECK_FALSE(tool.update(inspector.schema(), *snapshot, viewport, {}, moves, true));
        REQUIRE(tool.preview_position());
        CHECK(std::abs(tool.preview_position()->x - 2) < 1e-5F);
        CHECK(preview != tool.preview_position());
    }
    SECTION("release has its exact position even after a coalesced move batch") {
        const std::array events{
            down, first_move, last_move,
            input::Event{.kind = EventKind::pointer_up, .position = {546, 340}},
            input::Event{.kind = EventKind::pointer_move, .position = {594, 340}}};
        const auto applied =
            tool.update(inspector.schema(), *snapshot, viewport, events, events, true);
        REQUIRE(applied);
        CHECK(applied->phase == editor::Phase::apply);
        CHECK(std::abs(std::get<Vec3>(applied->values[0].value).x - 2) < 1e-5F);
        CHECK_FALSE(tool.dragging());
        CHECK_FALSE(tool.preview_position());
    }
    SECTION("Escape after moves cancels instead of exposing or applying a stale preview") {
        const std::array events{
            down, first_move, last_move,
            input::Event{.kind = EventKind::key_down, .key = Key::escape},
            input::Event{.kind = EventKind::pointer_up, .position = {546, 340}}};
        CHECK_FALSE(tool.update(inspector.schema(), *snapshot, viewport, events, events, true));
        CHECK_FALSE(tool.dragging());
        CHECK_FALSE(tool.preview_position());
        CHECK(tool.handledPointer());
        const std::array restart{down};
        CHECK_FALSE(tool.update(inspector.schema(), *snapshot, viewport, restart, restart, true));
        REQUIRE(tool.preview_position());
        CHECK(*tool.preview_position() == Vec3{});
    }
    SECTION("an invalid final pointer sample does not discard the last valid preview") {
        const std::array events{
            down, first_move, last_move,
            input::Event{.kind = EventKind::pointer_move,
                         .position = {std::numeric_limits<f32>::quiet_NaN(), 340}}};
        CHECK_FALSE(tool.update(inspector.schema(), *snapshot, viewport, events, events, true));
        REQUIRE(tool.preview_position());
        CHECK(std::abs(tool.preview_position()->x - 1) < 1e-5F);
        tool.cancel();
        CHECK_FALSE(tool.preview_position());
    }
}

TEST_CASE("Translation gestures cancel on lifecycle changes and clamp edits", "[editor][ui]") {
    gfx::Camera camera;
    camera.set_position({0, 0, 10}).look_at({0, 0, 0}).set_orthographic({.vertical_height = 10});
    const auto snapshot = camera.snapshot({640, 480});
    REQUIRE(snapshot);
    editor::Inspector inspector{{3, 4, 5}};
    inspector.translation_gizmo("origin", Vec3{}, [](Vec3) {});
    constexpr ui::Rect viewport{100, 100, 640, 480};
    editor_example::TranslationTool tool;
    const std::array down{input::Event{.kind = EventKind::pointer_down, .position = {450, 340}}};
    const std::array escape{input::Event{.kind = EventKind::key_down, .key = Key::escape}};
    const std::array lost{input::Event{.kind = EventKind::focus_lost}};
    const auto start = [&] {
        CHECK_FALSE(tool.update(inspector.schema(), *snapshot, viewport, down, down, true));
        REQUIRE(tool.dragging());
    };
    start();
    CHECK_FALSE(tool.update(inspector.schema(), *snapshot, viewport, {}, escape, true));
    CHECK_FALSE(tool.dragging());
    CHECK(tool.handledPointer());
    start();
    CHECK_FALSE(tool.update(inspector.schema(), *snapshot, viewport, {}, lost, true));
    CHECK_FALSE(tool.dragging());
    start();
    auto changed = inspector.schema();
    ++changed.stamp.generation;
    CHECK_FALSE(tool.update(changed, *snapshot, viewport, {}, {}, true));
    CHECK_FALSE(tool.dragging());
    start();
    CHECK_FALSE(tool.update(inspector.schema(), *snapshot, viewport, {}, {}, false));
    CHECK_FALSE(tool.dragging());
    ui::DrawList draw{{840, 680}, {840, 680}, {}};
    tool.append(draw);
    CHECK(draw.commands.empty());
    start();
    const std::array release{
        input::Event{.kind = EventKind::pointer_up, .position = {100000000, 340}}};
    auto event = tool.update(inspector.schema(), *snapshot, viewport, {}, release, true);
    REQUIRE(event);
    CHECK(std::get<Vec3>(event->values[0].value) == Vec3{editor_example::scene_coordinate_limit, 0, 0});
    const std::array rapid{down[0], input::Event{.kind = EventKind::key_down, .key = Key::escape}};
    CHECK_FALSE(tool.update(inspector.schema(), *snapshot, viewport, rapid, rapid, true));
    CHECK_FALSE(tool.dragging());
    CHECK(tool.handledPointer());
    tool.cancel();
    draw.commands.clear();
    tool.append(draw);
    CHECK(draw.commands.empty());
}

TEST_CASE("Perspective axis dragging follows projected points rather than a constant screen scale",
          "[editor][ui]") {
    gfx::Camera camera;
    camera.set_position({4, 3, 8}).look_at({0, 0, 0});
    const auto snapshot = camera.snapshot({640, 480});
    REQUIRE(snapshot);
    editor::Inspector inspector{{3, 4, 5}};
    inspector.translation_gizmo("origin", Vec3{}, [](Vec3) {});
    constexpr ui::Rect viewport{100, 100, 640, 480};
    editor_example::TranslationTool tool;
    CHECK_FALSE(tool.update(inspector.schema(), *snapshot, viewport, {}, {}, true));
    ui::DrawList draw{{840, 680}, {840, 680}, {}};
    tool.append(draw);
    std::optional<Vec2> endpoint;
    for (const auto& command : draw.commands) {
        const auto& box = std::get<ui::BoxDraw>(command);
        if (box.color.x == 1 && box.color.y < .1F && box.rect.width == 10)
            endpoint = Vec2{box.rect.x + 5, box.rect.y + 5};
    }
    REQUIRE(endpoint);
    const auto project = [&](f32 x) {
        const auto& m = snapshot->view_projection;
        const auto w = m[0][3] * x + m[3][3];
        return Vec2{viewport.x + ((m[0][0] * x + m[3][0]) / w + 1) * .5F * viewport.width,
                    viewport.y + (1 - (m[0][1] * x + m[3][1]) / w) * .5F * viewport.height};
    };
    const auto a = project(0), b = project(1);
    const Vec2 pointer{endpoint->x + b.x - a.x, endpoint->y + b.y - a.y};
    const std::array events{input::Event{.kind = EventKind::pointer_down, .position = *endpoint},
                            input::Event{.kind = EventKind::pointer_up, .position = pointer}};
    const auto event = tool.update(inspector.schema(), *snapshot, viewport, events, events, true);
    REQUIRE(event);
    const auto result = std::get<Vec3>(event->values[0].value);
    CHECK(std::abs(result.x - 1) < 1e-4F);
    CHECK(result.y == 0.0F);
    CHECK(result.z == 0.0F);
}

TEST_CASE("An animated mesh exposes a draggable gizmo at its sampled position",
          "[editor][ui][selection][regression]") {
    using namespace editor_example;
    content::vmesh::Document document;
    document.vertex_count = 3;
    document.vertex_fields = {{"position", {content::vmesh::ScalarType::Float32, 3},
                               std::vector<f32>{-1, -1, 0, 1, -1, 0, 0, 1, 0}}};
    document.faces = {{0, 1, 2}};
    auto mesh = editor::EditableMesh::create(std::move(document));
    REQUIRE(mesh);
    State state{.document = {.mesh = std::move(*mesh)}};
    state.viewport.selected_object = 1;
    instance_transform(state, 1)->position = {};
    state.viewport.time = 2;
    REQUIRE(key_property(state, {1, "position"}, 2, Vec3{1, 1, 0}));

    editor::Inspector inspector{{1, 4, state.document.revision}};
    inspector.translation_gizmo("position", instance_transform(state, 1)->position, [](Vec3) {});
    const auto schema = selection_gizmo_schema(state, inspector.schema());
    gfx::Camera camera;
    camera.set_position({0, 0, 10}).look_at({0, 0, 0}).set_orthographic({.vertical_height = 10});
    const auto snapshot = camera.snapshot({640, 480});
    REQUIRE(snapshot);
    constexpr ui::Rect viewport{100, 100, 640, 480};
    TranslationTool tool;
    CHECK_FALSE(tool.update(schema, *snapshot, viewport, {}, {}, true));
    ui::DrawList draw{{840, 680}, {840, 680}, {}};
    tool.append(draw);
    REQUIRE_FALSE(draw.commands.empty());
    std::optional<Vec2> red_tip;
    for (const auto& command : draw.commands) {
        const auto& box = std::get<ui::BoxDraw>(command);
        if (box.color.x == 1 && box.color.y < .1F && box.rect.width == 10)
            red_tip = Vec2{box.rect.x + 5, box.rect.y + 5};
    }
    REQUIRE(red_tip);
    // The sampled (1,1,0) origin is (468,292), not the base (420,340).
    CHECK(std::abs(red_tip->x - 513) < .001F);
    CHECK(std::abs(red_tip->y - 292) < .001F);
    const std::array events{
        input::Event{.kind = EventKind::pointer_down, .position = *red_tip},
        input::Event{.kind = EventKind::pointer_up,
                     .position = {red_tip->x + 48, red_tip->y}}};
    auto event = tool.update(schema, *snapshot, viewport, events, events, true);
    REQUIRE(event);
    CHECK(tool.handledPointer());
    auto applied = apply_animated_translation(state, *event);
    REQUIRE(applied);
    CHECK(*applied);
    CHECK(evaluate_scene(state, 2).model_transform.position == Vec3{2, 1, 0});
    CHECK(instance_transform(state, 1)->position == Vec3{});
    CHECK(state.document.revision == schema.stamp.revision);
}

TEST_CASE("Inspector XYZ fields reflow after overflow adds a scrollbar and after resizing") {
    Fixture f;
    f.host.height(260);
    editor::Inspector inspector{{1, 1, 1}};
    struct Values { Vec3 rotation{}; } values;
    inspector.edit("transform", values).field("rotation", &Values::rotation).apply("Apply", [](const Values&) {});
    for (int i = 0; i < 8; ++i)
        inspector.action("extra" + std::to_string(i), [] {}, "Extra action");
    f.panel.show(inspector.schema());
    for (const auto width : {330.F, 240.F, 420.F}) {
        f.host.width(width);
        f.pump();
        (void)f.panel.poll();
        f.pump();
        const auto tree = f.screen.inspect();
        REQUIRE(tree);
        int components{};
        bool scrollbar{};
        for (const auto& widget : tree->widgets) {
            if (widget.role == ui::WidgetRole::scrollbar && widget.visible) scrollbar = true;
            if (widget.role != ui::WidgetRole::text_field || !widget.visible) continue;
            ++components;
            CHECK(widget.bounds.width > 30.F);
            CHECK(widget.clip.width >= widget.bounds.width - .1F);
        }
        CHECK(scrollbar);
        CHECK(components == 3);
    }
}
