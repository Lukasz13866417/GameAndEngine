#include "../../examples/editor/timeline_panel.hpp"
#include "../../examples/editor/editing_session.hpp"
#include "../../examples/editor/animation.hpp"
#include "../../examples/editor/edit_clipboard.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
#include <iostream>
#include "../../examples/editor/editor_layout.hpp"
#include "../../examples/editor/sidebar_sizing.hpp"
#include "../../examples/editor/box_selection.hpp"

namespace {
using namespace vng;
using namespace editor_example;
using input::EventKind;
using input::Key;
text::Font font() {
    auto value = text::Font::load(VNG_TEST_FONT_PATH);
    REQUIRE(value);
    return *value;
}
State state() {
    content::vmesh::Document d;
    d.vertex_count = 3;
    d.vertex_fields = {{"position",
                        {content::vmesh::ScalarType::Float32, 3},
                        std::vector<f32>{0, 0, 0, 1, 0, 0, 0, 1, 0}}};
    d.faces = {{0, 1, 2}};
    auto mesh = editor::EditableMesh::create(std::move(d));
    REQUIRE(mesh);
    return {.document = {.mesh = std::move(*mesh)}};
}
KeyframeValue property_value(const TimelineAction& action, const timeline::Target& target) {
    const auto value = std::ranges::find(action.values, target, &KeyframeValue::target);
    REQUIRE(value != action.values.end());
    return *value;
}
struct Fixture {
    ui::Screen screen{ui::dark_theme(font())};
    ui::Container host = screen.column().position({20, 1030}).width(1760).height(160);
    ui::Container list = screen.column().position({20, 30}).width(300).height(970);
    ui::Container inspector = screen.column().position({360, 30}).width(760).height(970);
    ui::Container actions = screen.row().position({1150, 30}).width(440).height(36);
    TimelinePanel panel{host, list, inspector, actions, screen.column()};
    input::Frame input{.logical_size = {1800, 1200}, .framebuffer = {1800, 1200}};
    std::vector<input::Event> unhandled;
    State source = state();
    Fixture() {
        source.viewport.time = 3;
        const std::array<u64, 3> selected{1, 2, camera_animation_object};
        panel.selected_objects(selected);
        pump();
        panel.show(source);
        pump();
    }
    void pump(std::initializer_list<input::Event> events = {}) {
        panel.layout_menu(input.logical_size);
        input.events = events;
        for (const auto& event : events)
            if (event.kind == EventKind::pointer_down || event.kind == EventKind::pointer_up ||
                event.kind == EventKind::pointer_move)
                input.pointer = event.position;
        auto result = screen.update(input, .016F);
        REQUIRE(result);
        unhandled = std::move(result->events);
    }
    std::optional<TimelineAction> poll() { return panel.poll(unhandled, input.events); }
    ui::DrawList draw() {
        auto list = screen.draw_list();
        REQUIRE(list);
        return std::move(*list);
    }
    Vec2 at(std::string_view text, std::size_t occurrence = 0) {
        const auto list = draw();
        for (const auto& command : list.commands)
            if (const auto* entry = std::get_if<ui::TextDraw>(&command);
                entry && entry->text == text) {
                if (occurrence) {
                    --occurrence;
                    continue;
                }
                return {entry->position.x + 3, entry->position.y + 8};
            }
        FAIL("No timeline text: " << text);
        return {};
    }
    bool has(std::string_view text) {
        const auto list = draw();
        return std::ranges::any_of(list.commands, [&](const auto& command) {
            const auto* entry = std::get_if<ui::TextDraw>(&command);
            return entry && entry->text == text;
        });
    }
    Vec2 checkbox_in_row(std::string_view property, std::string_view caption) {
        const auto inspected = screen.inspect();
        REQUIRE(inspected);
        for (const auto& label : inspected->widgets) {
            if (label.role != ui::WidgetRole::label || label.label != property || !label.visible)
                continue;
            for (const auto& widget : inspected->widgets)
                if (widget.parent == label.parent && widget.role == ui::WidgetRole::checkbox &&
                    widget.label == caption && widget.visible)
                    return {widget.clip.x + widget.clip.width * .5F,
                            widget.clip.y + widget.clip.height * .5F};
        }
        FAIL("No " << caption << " checkbox in property row " << property);
        return {};
    }
    Vec2 field_after(std::string_view label) {
        const auto list = draw();
        bool seen{};
        for (const auto& command : list.commands) {
            if (const auto* text = std::get_if<ui::TextDraw>(&command); text && text->text == label)
                seen = true;
            else if (seen)
                if (const auto* box = std::get_if<ui::BoxDraw>(&command))
                    return {box->rect.x + 15, box->rect.y + box->rect.height * .5F};
        }
        FAIL("No field after " << label);
        return {};
    }
    ui::Rect canvas() {
        const auto list = draw();
        for (const auto& command : list.commands)
            if (const auto* image = std::get_if<ui::ImageDraw>(&command))
                return image->rect;
        FAIL("No timeline canvas");
        return {};
    }
    void click(Vec2 position,input::Modifiers modifiers={}) {
        pump({{.kind = EventKind::pointer_down, .position = position,.modifiers=modifiers},
              {.kind = EventKind::pointer_up, .position = position,.modifiers=modifiers}});
    }
    void click(std::string_view text) { click(at(text)); }
    void toggle_group(std::string_view identifier) {
        const auto tree = screen.inspect(); REQUIRE(tree);
        const auto label = std::ranges::find_if(tree->widgets, [&](const auto& widget) {
            return widget.visible && widget.role == ui::WidgetRole::button && widget.text == identifier;
        });
        REQUIRE(label != tree->widgets.end());
        const auto toggle = std::ranges::find_if(tree->widgets, [&](const auto& widget) {
            return widget.parent == label->parent && widget.role == ui::WidgetRole::button && widget.id != label->id;
        });
        REQUIRE(toggle != tree->widgets.end());
        CHECK(toggle->clip.height == Catch::Approx(toggle->bounds.height));
        CHECK(toggle->clip.width == Catch::Approx(toggle->bounds.width));
        click(Vec2{toggle->clip.x + toggle->clip.width * .5F, toggle->clip.y + toggle->clip.height * .5F});
        CHECK_FALSE(poll());
        pump();
    }
    void edit(Vec2 position, std::string text, bool submit = false) {
        click(position);
        const input::Event select{
            .kind = EventKind::key_down, .key = Key::a, .modifiers = {.control = true}};
        const input::Event input{.kind = EventKind::text, .text = std::move(text)};
        if (submit)
            pump({select, input, {.kind = EventKind::key_down, .key = Key::enter}});
        else
            pump({select, input});
    }
    void choose(std::string_view dropdown, std::string_view option) {
        click(dropdown);
        CHECK_FALSE(poll());
        // Menus paint last; an object heading can have the same text.
        auto position = at(option);
        for (const auto& command : draw().commands)
            if (const auto* text = std::get_if<ui::TextDraw>(&command);
                text && text->text == option)
                position = {text->position.x + 3, text->position.y + 8};
        click(position);
        CHECK_FALSE(poll());
    }
    void add() {
        click("Add keyframe");
        const auto action = poll();
        REQUIRE(action);
        applied(*action);
    }
    void applied(TimelineAction action) {
        if (action.kind == TimelineAction::Kind::add) {
            REQUIRE(add_keyframe(source, action.time));
            source.viewport.time = action.time;
        } else if (action.kind == TimelineAction::Kind::edit) {
            REQUIRE(update_keyframe(source, action.time, action.destination, action.name,
                                    action.values));
            source.viewport.time = action.destination;
        } else if (action.kind == TimelineAction::Kind::erase)
            REQUIRE(erase_keyframe(source, action.time));
        else if (action.kind == TimelineAction::Kind::apply_range)
            REQUIRE(apply_keyframe_range(source, action.time, action.destination, action.changes));
        else if (action.kind == TimelineAction::Kind::seek)
            source.viewport.time = action.time;
        ++source.document.revision;
        panel.show(source);
        pump();
    }
};
} // namespace

TEST_CASE("Toolbar range action applies only drafts and stays open on errors", "[editor][ui][timeline][range]") {
    Fixture f;
    f.add();
    REQUIRE(add_keyframe(f.source, 6));
    ++f.source.document.revision; f.panel.show(f.source); f.pump();
    f.click("Apply to keyframes"); CHECK_FALSE(f.poll()); f.pump();
    REQUIRE(f.panel.menu_open()); CHECK(f.has("2 keyframes in range"));
    f.click("Apply range"); CHECK_FALSE(f.poll());
    CHECK(f.has("Edit a keyframe property before applying to a range."));
    f.click("Cancel"); CHECK_FALSE(f.poll()); CHECK_FALSE(f.panel.menu_open());
    f.edit(f.at("0.7"), "2.5"); CHECK_FALSE(f.poll());
    f.click("Apply to keyframes"); CHECK_FALSE(f.poll()); f.pump();
    f.click("Apply range"); const auto action = f.poll(); REQUIRE(action);
    CHECK(action->kind == TimelineAction::Kind::apply_range);
    CHECK(action->time == 3); CHECK(action->destination == 6);
    REQUIRE(action->changes.size() == 1);
    CHECK(action->changes[0].target == timeline::Target{1, "scale"});
    CHECK(std::get<f32>(*action->changes[0].value) == 2.5F);
    f.panel.error("Test rejection"); f.pump();
    CHECK(f.panel.menu_open()); CHECK(f.has("Test rejection"));
    f.click("Apply range"); const auto retry = f.poll(); REQUIRE(retry);
    f.applied(*retry);
    CHECK_FALSE(f.panel.menu_open());
    for (auto time : {3.F, 6.F}) CHECK(std::get<f32>(*f.source.document.timeline.sample({1, "scale"}, time)) == 2.5F);
}

TEST_CASE("Range picker uses multi-selection endpoints and Escape preserves drafts", "[editor][ui][timeline][range]") {
    Fixture f;
    for (auto time : {2.F, 4.F, 6.F}) REQUIRE(add_keyframe(f.source, time));
    f.source.viewport.time = 4;
    const std::array times{2.F, 4.F}; f.panel.select_keyframes(times);
    ++f.source.document.revision; f.panel.show(f.source); f.pump();
    f.edit(f.at("0.7"), "3"); CHECK_FALSE(f.poll());
    f.click("Apply to keyframes"); CHECK_FALSE(f.poll()); f.pump();
    CHECK(f.has("2 keyframes in range"));
    f.pump({{.kind = EventKind::key_down, .key = Key::escape}}); CHECK_FALSE(f.poll());
    CHECK_FALSE(f.panel.menu_open());
    f.click("Apply to keyframes"); CHECK_FALSE(f.poll()); f.pump();
    f.edit(f.field_after("From (s)"), "5"); CHECK_FALSE(f.poll());
    f.click("Apply range"); CHECK_FALSE(f.poll()); CHECK(f.panel.menu_open());
    f.edit(f.field_after("From (s)"), "2"); CHECK_FALSE(f.poll());
    f.click("Apply range"); const auto action = f.poll(); REQUIRE(action);
    CHECK(action->time == 2); CHECK(action->destination == 4);
    REQUIRE(action->changes.size() == 1); CHECK(std::get<f32>(*action->changes[0].value) == 3.F);
}

TEST_CASE("Keyframe identifiers request shared scene selection without changing keys or drafts",
          "[editor][ui][timeline][selection]") {
    Fixture f;
    f.panel.selected_objects({});
    f.source.viewport.time = 0;
    f.panel.show(f.source); f.pump(); f.add();
    const auto revision = f.source.document.revision;
    const auto animation = f.source.document.timeline;
    f.edit(f.field_after("Name"), "Draft name");
    CHECK_FALSE(f.poll());
    for (const auto modifiers : {input::Modifiers{}, input::Modifiers{.control=true}, input::Modifiers{.shift=true}}) {
        f.click(f.at("Mesh / Mesh"), modifiers);
        const auto action = f.poll(); REQUIRE(action);
        CHECK(action->kind == TimelineAction::Kind::select_object);
        CHECK(action->object == 1);
        CHECK(action->selection_mode == (modifiers.control ? editor::SelectionMode::toggle :
              modifiers.shift ? editor::SelectionMode::range : editor::SelectionMode::replace));
        CHECK(action->values.empty());
        CHECK(f.panel.selected_keyframe() == 0.F);
    }
    // The application confirms selection: no second scene selection model is
    // stored in the panel, and selecting elsewhere drives the same highlights.
    f.panel.selected_objects(std::array<u64, 1>{1}); f.pump();
    const auto tree = f.screen.inspect(); REQUIRE(tree);
    for (const auto& widget : tree->widgets) {
        if (widget.role != ui::WidgetRole::button) continue;
        if (widget.text == "Mesh / Mesh") {
            CHECK(widget.selected);
            const auto header = std::ranges::find(tree->widgets, widget.parent, &ui::WidgetSnapshot::id);
            REQUIRE(header != tree->widgets.end());
            CHECK(widget.bounds.x + widget.bounds.width == Catch::Approx(header->bounds.x + header->bounds.width));
        }
        if (widget.text == "Sun / effect / Sun") CHECK_FALSE(widget.selected);
    }
    CHECK(f.has("Brightness")); CHECK_FALSE(f.has("Radius"));
    f.toggle_group("Mesh / Mesh"); // Expansion emits no scene-selection intent.
    f.click("Apply to keyframe");
    const auto apply = f.poll(); REQUIRE(apply);
    CHECK(apply->kind == TimelineAction::Kind::edit);
    CHECK(apply->name == "Draft name");
    CHECK(f.source.document.revision == revision);
    CHECK(f.source.document.timeline == animation);
}

TEST_CASE("Keyframe objects are identifier-only groups with manual and selection-driven expansion",
          "[editor][ui][timeline][collapse]") {
    Fixture f;
    f.panel.selected_objects({});
    f.source.viewport.time = 0;
    f.panel.show(f.source); f.pump(); f.add();
    const auto revision = f.source.document.revision;
    const auto animation = f.source.document.timeline;
    const auto tree = f.screen.inspect(); REQUIRE(tree);
    std::vector<u64> ids;
    for (const auto& widget : tree->widgets) ids.push_back(widget.id);
    CHECK(f.has("Mesh / Mesh")); CHECK(f.has("Sun / effect / Sun"));
    CHECK(f.has("Animation camera"));
    CHECK_FALSE(f.has("Position")); CHECK_FALSE(f.has("Radius")); CHECK_FALSE(f.has("Key"));

    f.panel.selected_objects(std::array<u64, 1>{1}); f.pump();
    CHECK(f.has("Position")); CHECK(f.has("Brightness")); CHECK_FALSE(f.has("Radius"));
    f.toggle_group("Mesh / Mesh");
    CHECK_FALSE(f.has("Brightness")); // Manual collapse of the selected object.
    f.panel.show(f.source); f.panel.selected_objects(std::array<u64, 1>{1}); f.pump();
    CHECK_FALSE(f.has("Brightness")); // Ordinary refresh must not force it open.
    f.panel.selected_objects(std::array<u64, 1>{2}); f.pump();
    CHECK_FALSE(f.has("Brightness")); CHECK(f.has("Radius"));
    f.panel.selected_objects({}); f.pump();
    CHECK_FALSE(f.has("Position")); CHECK_FALSE(f.has("Radius"));

    f.toggle_group("Mesh / Mesh");
    CHECK(f.has("Brightness"));
    f.panel.selected_objects(std::array<u64, 1>{2}); f.pump();
    CHECK(f.has("Brightness")); CHECK(f.has("Radius"));
    f.panel.selected_objects({}); f.pump();
    CHECK(f.has("Brightness")); CHECK_FALSE(f.has("Radius")); // Unrelated manual choice survives.
    f.panel.selected_objects(std::array<u64, 1>{1}); f.pump();
    f.panel.selected_objects({}); f.pump();
    CHECK_FALSE(f.has("Brightness")); // Leaving selection resets that object's override.
    f.panel.selected_objects(std::array<u64, 2>{1, 2}); f.pump();
    CHECK(f.has("Brightness")); CHECK(f.has("Radius"));
    f.panel.selected_objects({}); f.pump();
    const auto after = f.screen.inspect(); REQUIRE(after);
    std::vector<u64> after_ids;
    for (const auto& widget : after->widgets) after_ids.push_back(widget.id);
    CHECK(ids == after_ids);
    CHECK(f.source.document.revision == revision);
    CHECK(f.source.document.timeline == animation);
    f.click("Apply to keyframe"); const auto action = f.poll(); REQUIRE(action);
    CHECK(action->values.empty());

    f.toggle_group("Mesh / Mesh"); CHECK(f.has("Brightness"));
    f.panel.reset(); f.panel.select_keyframe(0); f.panel.show(f.source); f.pump();
    CHECK_FALSE(f.has("Brightness")); // A different scene never inherits expansion overrides.
}

TEST_CASE("Keyframe inspector shows changed groups and temporarily selected instances",
          "[editor][ui][timeline][differences]") {
    Fixture f;
    f.panel.selected_objects({});
    f.add();
    CHECK_FALSE(f.has("Mesh / Mesh"));
    CHECK_FALSE(f.has("Sun / effect / Sun"));
    const auto revision = f.source.document.revision;
    f.panel.selected_objects(std::array<u64,1>{1});
    f.panel.focus_object(1);
    f.pump();
    CHECK(f.has("Mesh / Mesh")); CHECK(f.has("Position")); CHECK(f.has("Brightness"));
    f.panel.selected_objects({}); f.pump();
    CHECK_FALSE(f.has("Mesh / Mesh"));
    CHECK(f.source.document.revision == revision);
    f.panel.selected_objects(std::array<u64,1>{1}); f.pump();
    f.edit(f.at("1.7"), "2.75"); CHECK_FALSE(f.poll());
    f.panel.selected_objects({}); f.pump();
    CHECK(f.has("Mesh / Mesh")); // Unsubmitted changes pin their instance, not its expanded fields.
    CHECK_FALSE(f.has("Brightness"));
    f.click("Apply to keyframe");
    const auto edit = f.poll(); REQUIRE(edit);
    REQUIRE(edit->values.size() == 1);
    f.applied(*edit);
    CHECK(f.has("Mesh / Mesh")); CHECK_FALSE(f.has("Sun / effect / Sun"));
    REQUIRE(add_keyframe(f.source, 5));
    ++f.source.document.revision;
    f.source.viewport.time = 5;
    f.panel.select_keyframe(5); f.panel.show(f.source); f.pump();
    CHECK_FALSE(f.has("Mesh / Mesh")); // Compared with the preceding key, not time zero.
    f.source.viewport.time = 0;
    f.panel.select_keyframe(0); f.panel.show(f.source); f.pump();
    CHECK(f.has("Mesh / Mesh")); CHECK(f.has("Sun / effect / Sun"));
    const auto inspection = f.screen.inspect(); REQUIRE(inspection);
    for (const auto& widget : inspection->widgets)
        if ((widget.role == ui::WidgetRole::checkbox && widget.label == "Key") ||
            (widget.role == ui::WidgetRole::button && widget.text == "Delete keyframe"))
            CHECK_FALSE(widget.enabled);
}

TEST_CASE("Reverting an unselected inspector draft hides the unchanged instance",
          "[editor][ui][timeline][differences]") {
    Fixture f;
    f.panel.selected_objects(std::array<u64,1>{1});
    f.add();
    f.edit(f.at("1.7"), "oops"); CHECK_FALSE(f.poll());
    f.panel.selected_objects({}); f.pump();
    CHECK(f.has("Mesh / Mesh"));
    f.toggle_group("Mesh / Mesh");
    f.edit(f.at("oops"), "1.7"); CHECK_FALSE(f.poll());
    f.pump();
    CHECK_FALSE(f.has("Mesh / Mesh"));
    CHECK_FALSE(f.has("Sun / effect / Sun"));
}

TEST_CASE("Scene keyframes are added at the cursor and immediately inspected",
          "[editor][ui][timeline]") {
    Fixture f;
    CHECK_FALSE(f.panel.selected_keyframe());
    f.edit(f.field_after("Time"), "4.5", true);
    const auto seek = f.poll();
    REQUIRE(seek);
    CHECK(seek->kind == TimelineAction::Kind::seek);
    f.applied(*seek);
    CHECK(f.source.document.timeline.tracks().empty());
    f.add();
    CHECK(f.panel.selected_keyframe() == 4.5F);
    CHECK(f.has("> 4.5 s | Keyframe"));
    CHECK(f.has("0 s | Keyframe"));
    CHECK(f.has("KEYFRAME / 4.5 seconds"));
    CHECK(f.has("Mesh / Mesh"));
    CHECK(f.has("Sun / effect / Sun"));
    CHECK(f.source.document.timeline.tracks().size() == 21);
    const auto before = f.source.document.timeline;
    f.add();
    CHECK(f.source.document.timeline == before);
    CHECK(f.panel.selected_keyframe() == 4.5F);
}

TEST_CASE("Timeline reuses keyframe buttons across unrelated scene revisions",
          "[editor][ui][timeline]") {
    Fixture f;
    REQUIRE(add_keyframe(f.source, 2));
    ++f.source.document.revision;
    f.panel.show(f.source);
    f.pump();
    const auto position = f.at("2 s | Keyframe");
    f.pump({{.kind = EventKind::pointer_down, .position = position}});
    // A worker/editor update between the press and release must not replace
    // the retained button and cancel the user's pending selection.
    ++f.source.document.revision;
    (*editor_example::mesh_settings(f.source, 1)).brightness += .1F;
    f.panel.show(f.source);
    f.pump({{.kind = EventKind::pointer_up, .position = position}});
    const auto action = f.poll();
    REQUIRE(action);
    CHECK(action->kind == TimelineAction::Kind::seek);
    CHECK(action->time == 2.0F);
    f.applied(*action);
    f.source.document.keyframe_names[2] = "Close pass";
    ++f.source.document.revision;
    f.panel.show(f.source);
    f.pump();
    CHECK(f.has("> 2 s | Close pass"));
}

TEST_CASE("Opening at time zero does not select the initial keyframe", "[editor][ui][timeline][startup]") {
    Fixture f;
    f.panel.reset();
    f.source.viewport.time = 0;
    REQUIRE(f.source.document.timeline.set({1,"scale"},{0,1.F}));
    ++f.source.document.revision;
    f.panel.show(f.source); f.pump();
    CHECK_FALSE(f.panel.selected_keyframe()); CHECK(f.panel.selected_keyframes().empty());
    CHECK(f.has("KEYFRAMES / 0 selected")); CHECK(f.has("KEYFRAME / none selected"));
    CHECK_FALSE(f.poll());
    f.panel.show(f.source); f.pump();
    CHECK_FALSE(f.panel.selected_keyframe());
    f.click("0 s | Keyframe"); REQUIRE(f.poll());
    CHECK(f.panel.selected_keyframe() == 0.F);
}

TEST_CASE("Private camera revisions cause no timeline reconstruction or value resampling",
          "[editor][ui][timeline][viewport]") {
    Fixture f;
    f.add();
    const auto before = f.panel.statistics();
    const auto revision = f.source.document.revision;
    for (int i = 0; i < 100; ++i) {
        ++f.source.viewport.sequence;
        f.source.viewport.editor_camera.yaw = static_cast<float>(i);
        f.source.viewport.editor_camera.distance = 8 + static_cast<float>(i) * .1F;
        f.panel.show(f.source);
        f.pump();
    }
    CHECK(f.panel.statistics() == before);
    CHECK(f.source.document.revision == revision);
}

TEST_CASE("Fleet-sized keyframe inspection reuses controls across repeated selections",
          "[editor][ui][timeline][performance][regression]") {
    Fixture f;
    f.source.document.instances.clear();
    for (u32 id = 1; id <= 105; ++id) {
        f.source.document.instances.push_back(
            {id, BlueprintId::mesh, "Ship " + std::to_string(id), MeshSettings{}, {}});
        REQUIRE(f.source.document.timeline.set({id, "scale"}, {0, 1.F}));
        REQUIRE(f.source.document.timeline.set({id, "scale"}, {5, 2.F, timeline::Interpolation::linear}));
    }
    ++f.source.document.revision;
    f.panel.show(f.source);
    f.pump();
    const auto revision = f.source.document.revision;
    const auto animation = f.source.document.timeline;
    const auto begin = std::chrono::steady_clock::now();
    f.click("0 s | Keyframe");
    REQUIRE(f.poll());
    REQUIRE(f.panel.selected_keyframe() == 0.F);
    f.pump();
    const auto first_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - begin).count();
    const auto initial = f.screen.inspect();
    REQUIRE(initial);
    std::vector<u64> widget_ids;
    for (const auto& widget : initial->widgets) widget_ids.push_back(widget.id);
    double worst_ms{};
    for (int seek = 0; seek < 24; ++seek) {
        const auto time = seek % 2 ? 0.F : 5.F;
        const auto start = std::chrono::steady_clock::now();
        f.click(time == 0 ? "0 s | Keyframe" : "5 s | Keyframe");
        const auto action = f.poll();
        REQUIRE(action);
        CHECK(action->kind == TimelineAction::Kind::seek);
        CHECK(f.panel.selected_keyframe() == time);
        f.source.viewport.time = action->time; // Seeking changes no authored revision.
        f.panel.show(f.source);
        f.pump();
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        worst_ms = std::max(worst_ms, elapsed);
        const auto inspection = f.screen.inspect();
        REQUIRE(inspection);
        std::vector<u64> ids;
        for (const auto& widget : inspection->widgets) ids.push_back(widget.id);
        REQUIRE(ids == widget_ids); // No new widgets, tombstones or ID exhaustion.
    }
    std::cout << "105-instance keyframe selection: first " << first_ms
              << " ms, worst repeated " << worst_ms << " ms\n";
    CHECK(f.source.document.revision == revision);
    CHECK(f.source.document.timeline == animation);
    f.click("Apply to keyframe");
    const auto edit = f.poll();
    REQUIRE(edit);
    CHECK(edit->values.empty()); // Applying an unchanged pose emits no property edits.
}

TEST_CASE("Idle keyframe inspectors do not refilter or resize all retained property rows",
          "[editor][ui][timeline][performance]") {
    Fixture f;
    for (u32 id=3; id<203; ++id)
        f.source.document.instances.push_back({id, BlueprintId::mesh, "Ship", MeshSettings{}, {}});
    ++f.source.document.revision;
    f.source.viewport.time=0;
    f.panel.select_keyframe(0);
    f.panel.show(f.source);
    f.pump();
    (void)f.poll();
    f.panel.show(f.source); // Settle the inspector's scrollbar gutter.
    const auto before=f.panel.statistics();
    for(int i=0; i<30; ++i) {
        ++f.source.viewport.sequence;
        f.source.viewport.editor_camera.yaw+=1;
        f.panel.show(f.source);
        f.pump();
        CHECK_FALSE(f.poll());
    }
    CHECK(f.panel.statistics()==before);
    f.inspector.width(900);
    f.panel.show(f.source);
    CHECK(f.panel.statistics().row_layout_updates==before.row_layout_updates+1);
    CHECK(f.panel.statistics().row_visibility_updates==before.row_visibility_updates);
}

TEST_CASE("Removing a selected keyframe discards drafts for the vanished target",
          "[editor][ui][timeline][regression]") {
    Fixture f;
    EditingSession editing{f.source}; editing.select_keyframe(editing.state().viewport.time);
    REQUIRE(editing.add_keyframe(3));
    f.add();
    REQUIRE(f.panel.selected_keyframe() == 3.F);
    f.edit(f.field_after("Name"), "Unsent draft");
    CHECK_FALSE(f.poll());
    REQUIRE(f.has("Unsent draft"));

    SECTION("Undo the addition while the field is focused") {
        REQUIRE(editing.undo());
        f.source = editing.state();
        f.panel.show(f.source);
        f.pump();
    }
    SECTION("Delete from its inspector despite an unsent draft") {
        f.click("Delete keyframe");
        const auto action = f.poll();
        REQUIRE(action);
        REQUIRE(action->kind == TimelineAction::Kind::erase);
        f.applied(*action);
    }
    CHECK_FALSE(f.panel.selected_keyframe());
    CHECK(f.has("KEYFRAME / none selected"));
    CHECK_FALSE(f.has("Unsent draft"));
    CHECK(f.has("Apply to keyframe")); // Pinned toolbar action stays visible but disabled.
    f.click("Apply to keyframe"); CHECK_FALSE(f.poll());
    CHECK_FALSE(f.has("Delete keyframe"));
    f.pump({{.kind = EventKind::key_down, .key = Key::enter}});
    CHECK_FALSE(f.poll()); // Stale field focus cannot recreate the deleted key.
    f.add();
    CHECK(f.panel.selected_keyframe() == 3.F);
    CHECK(f.has("Apply to keyframe"));
    CHECK_FALSE(f.has("Unsent draft"));
}

TEST_CASE("List and markers select groups but markers never drag them", "[editor][ui][timeline]") {
    Fixture f;
    REQUIRE(add_keyframe(f.source, 2));
    f.source.document.keyframe_names[2] = "Fly past";
    ++f.source.document.revision;
    f.panel.show(f.source);
    f.pump();
    f.click("2 s | Fly past");
    auto action = f.poll();
    REQUIRE(action);
    f.applied(*action);
    CHECK(f.panel.selected_keyframe() == 2.F);
    const auto before = f.source.document.timeline;
    auto canvas = f.canvas();
    f.pump({{.kind = EventKind::pointer_down, .position = {canvas.x, canvas.y + 40}}});
    action = f.poll();
    REQUIRE(action);
    CHECK(action->time == 0.F);
    CHECK_FALSE(f.panel.dragging());
    f.applied(*action);
    f.pump({{.kind = EventKind::pointer_move, .position = {canvas.x + 350, canvas.y + 40}},
            {.kind = EventKind::pointer_up, .position = {canvas.x + 350, canvas.y + 40}}});
    CHECK_FALSE(f.poll());
    CHECK(f.panel.selected_keyframe() == 0.F);
    CHECK(f.source.document.timeline == before);
}

TEST_CASE("Inspector edits name timestamp and per-value interpolation atomically",
          "[editor][ui][timeline]") {
    Fixture f;
    f.add();
    f.edit(f.field_after("Name"), "Arrival");
    CHECK_FALSE(f.poll());
    f.edit(f.field_after("Timestamp (s)"), "6");
    CHECK_FALSE(f.poll());
    f.edit(f.at("1.7"), "2.75");
    CHECK_FALSE(f.poll());
    f.click(f.checkbox_in_row("Position", "Blend"));
    CHECK_FALSE(f.poll());
    f.click(f.checkbox_in_row("Scale", "Key"));
    CHECK_FALSE(f.poll());
    f.click("Apply to keyframe");
    auto action = f.poll();
    REQUIRE(action);
    CHECK(action->kind == TimelineAction::Kind::edit);
    CHECK(action->time == 3.F);
    CHECK(action->destination == 6.F);
    CHECK(action->name == "Arrival");
    REQUIRE(action->values.size() == 2);
    const auto& position = property_value(*action, {1, "position"});
    CHECK(std::get<Vec3>(position.value).x == 2.75F);
    CHECK(position.incoming == timeline::Interpolation::hold);
    CHECK_FALSE(property_value(*action, {1, "scale"}).keyed);
    CHECK(f.source.document.keyframe_names.at(3).empty());
    f.applied(*action);
    CHECK(f.panel.selected_keyframe() == 6.F);
    CHECK(f.has("> 6 s | Arrival"));
    CHECK(f.source.document.timeline.find({1, "position"})->keys.back().time == 6.F);
    CHECK(f.source.document.timeline.find({1, "scale"})->keys.size() == 1);
    f.click("Delete keyframe");
    action = f.poll();
    REQUIRE(action);
    f.applied(*action);
    CHECK_FALSE(f.panel.selected_keyframe());
    CHECK(keyframe_times(f.source) == std::vector<f32>{0});
}

TEST_CASE("Sparse inspector exposes evaluated context and filters objects",
          "[editor][ui][timeline]") {
    Fixture f;
    REQUIRE(key_property(f.source, {1, "scale"}, 2, 1.25F));
    REQUIRE(key_property(f.source, {2, "radius"}, 4, 2.F));
    ++f.source.document.revision;
    f.panel.show(f.source);
    f.pump();
    f.click("2 s | Keyframe");
    auto action = f.poll();
    REQUIRE(action);
    f.applied(*action);
    f.click("Apply to keyframe");
    action = f.poll();
    REQUIRE(action);
    CHECK(action->values.empty());
    const TimelineAction context{.values = keyframe_values(f.source, 2)};
    CHECK_FALSE(property_value(context, {1, "position"}).keyed);
    const auto& scale = property_value(context, {1, "scale"});
    CHECK(scale.keyed);
    CHECK(std::get<f32>(scale.value) == 1.25F);
    const auto& radius = property_value(context, {2, "radius"});
    CHECK_FALSE(radius.keyed);
    CHECK(std::get<f32>(radius.value) == Catch::Approx(1.55F));
    CHECK(property_value(context, {1, "visible"}).incoming == timeline::Interpolation::hold);
    CHECK(f.has("1.25"));
    f.applied(*action);
    const auto before = f.source.document.timeline;
    f.choose("Object: All objects", "Sun");
    CHECK_FALSE(f.has("Brightness"));
    CHECK(f.has("Radius"));
    CHECK(f.source.document.timeline == before);
}

TEST_CASE("Layer filter changes markers without muting animation", "[editor][ui][timeline]") {
    Fixture f;
    REQUIRE(key_property(f.source, {1, "scale"}, 2, 1.25F));
    REQUIRE(key_property(f.source, {2, "radius"}, 4, 2.F));
    ++f.source.document.revision;
    f.panel.show(f.source);
    f.pump();
    const auto before = f.source.document.timeline;
    f.choose("Layer: All layers", "Sun");
    CHECK(f.has("Layer: Sun"));
    CHECK_FALSE(f.has("2 s | Keyframe"));
    CHECK(f.has("4 s | Keyframe"));
    CHECK(f.source.document.timeline == before);
    CHECK(evaluate_scene(f.source, 4).model_transform.scale == 1.25F);
}

TEST_CASE("Timeline object filters follow live blueprint instances after deletion",
          "[editor][ui][timeline][instances]") {
    Fixture f;
    REQUIRE(erase_instance(f.source, 1));
    auto created = instantiate(f.source, BlueprintId::mesh);
    REQUIRE(created);
    find_instance(f.source, *created)->name = "Second cube";
    f.panel.selected_objects(std::array<u64, 2>{*created, 2});
    ++f.source.document.revision;
    f.panel.show(f.source);
    f.pump();
    f.add();
    f.choose("Object: All objects", "Second cube");
    CHECK(f.has("Brightness"));
    CHECK_FALSE(f.has("Radius"));
    REQUIRE(erase_instance(f.source, *created));
    ++f.source.document.revision;
    f.panel.show(f.source);
    f.pump();
    CHECK(f.has("Object: All objects"));
    CHECK_FALSE(f.has("Second cube"));
    CHECK_FALSE(f.has("Brightness"));
    CHECK(f.has("Radius"));
}

TEST_CASE("Timeline ruler scrubs from unhandled input and releases outside panel",
          "[editor][ui][timeline]") {
    Fixture f;
    const auto canvas = f.canvas();
    const Vec2 down{canvas.x + canvas.width * .25F, canvas.y + 5};
    f.pump({{.kind = EventKind::pointer_down, .position = down}});
    CHECK_FALSE(f.panel.poll({}, f.input.events));
    CHECK_FALSE(f.panel.dragging());
    auto action = f.poll();
    REQUIRE(action);
    CHECK(action->time == Catch::Approx(2.5));
    CHECK(f.panel.dragging());
    f.applied(*action);
    REQUIRE(f.panel.dragging());
    f.pump({{.kind = EventKind::pointer_move, .position = {canvas.x + canvas.width * .6F, 50}},
            {.kind = EventKind::pointer_up, .position = {canvas.x + canvas.width + 50, 50}}});
    action = f.panel.poll({}, f.input.events);
    REQUIRE(action);
    CHECK(action->time == 10.F);
    CHECK_FALSE(f.panel.dragging());
    f.pump({{.kind = EventKind::pointer_down, .position = down}});
    REQUIRE(f.poll());
    f.pump({{.kind = EventKind::focus_lost}});
    CHECK_FALSE(f.poll());
    CHECK_FALSE(f.panel.dragging());
}

TEST_CASE("Drafts survive playback but remain read-only away from their keyframe",
          "[editor][ui][timeline]") {
    Fixture f;
    f.add();
    f.edit(f.at("1.7"), "9");
    CHECK_FALSE(f.poll());
    ++f.source.document.revision;
    f.source.viewport.time = 4;
    f.panel.show(f.source);
    f.pump({{.kind = EventKind::text, .text = "8"}});
    ++f.source.document.revision;
    f.source.viewport.time = 5;
    f.panel.show(f.source);
    CHECK(f.has("9"));
    CHECK_FALSE(f.has("1.79"));
    CHECK_FALSE(f.poll());
    f.click("Apply to keyframe");
    CHECK_FALSE(f.poll());
    f.source.viewport.time = 3;
    f.panel.show(f.source);
    f.pump();
    f.click("Apply to keyframe");
    const auto action = f.poll();
    REQUIRE(action);
    CHECK(std::get<Vec3>(action->values[0].value).x == 9.F);
    CHECK(action->time == 3.F);
    CHECK(action->destination == 3.F);
    f.panel.error("Another keyframe already exists at that time");
    CHECK(f.has("9"));
    f.edit(f.field_after("Timestamp (s)"), "nan");
    CHECK_FALSE(f.poll());
    f.click("Apply to keyframe");
    CHECK_FALSE(f.poll());
    CHECK(f.has("Timestamp must lie inside the timeline duration."));
    f.panel.reset();
    ++f.source.document.revision;
    f.panel.show(f.source);
    f.pump();
    CHECK_FALSE(f.panel.selected_keyframe());
    CHECK_FALSE(f.has("9"));
}

TEST_CASE("Slider capture survives acknowledgements until release", "[editor][ui][timeline]") {
    Fixture f;
    const auto start = f.at("Playhead: 3");
    f.pump({{.kind = EventKind::pointer_down, .position = start}});
    const auto first = f.poll();
    REQUIRE(first);
    REQUIRE(f.panel.dragging());
    f.applied(*first);
    REQUIRE(f.panel.dragging());
    f.pump({{.kind = EventKind::pointer_move, .position = {start.x + 90, start.y}}});
    const auto moved = f.poll();
    REQUIRE(moved);
    CHECK(moved->time > first->time);
    f.applied(*moved);
    f.pump({{.kind = EventKind::pointer_up, .position = {start.x + 100, start.y}}});
    (void)f.poll();
    CHECK_FALSE(f.panel.dragging());
}

TEST_CASE("Timeline drawing stays below menus and inspector labels fit", "[editor][ui][timeline]") {
    Fixture f;
    // Include the camera's four extra rows without clipping a final row at
    // the viewport edge; scrolling clips partially visible labels by design.
    f.input.logical_size = {1800, 1500};
    f.input.framebuffer = {1800, 1500};
    f.host.position({20, 1330});
    f.inspector.width(660).height(1270);
    f.pump();
    f.panel.show(f.source);
    f.pump();
    f.add();
    auto list = f.draw();
    for (const auto& command : list.commands) {
        const auto* text = std::get_if<ui::TextDraw>(&command);
        if (!text || text->text.empty())
            continue;
        INFO(text->text);
        const auto measured = text->font.measure(text->text, static_cast<u32>(text->size));
        REQUIRE(measured);
        CHECK(measured->width <= text->clip.x + text->clip.width - text->position.x + 1);
        CHECK(text->position.y + measured->height <= text->clip.y + text->clip.height + 1);
    }
    f.click("Layer: All layers");
    list = f.draw();
    f.panel.append(list);
    std::optional<std::size_t> marker, popup;
    for (std::size_t i = 0; i < list.commands.size(); ++i) {
        if (const auto* box = std::get_if<ui::BoxDraw>(&list.commands[i]);
            box && box->color == Vec4{1, .42F, .08F, 1})
            marker = i;
        if (const auto* text = std::get_if<ui::TextDraw>(&list.commands[i]);
            text && text->text == "All layers")
            popup = i;
        CHECK_FALSE(std::holds_alternative<ui::ImageDraw>(list.commands[i]));
    }
    REQUIRE(marker);
    REQUIRE(popup);
    CHECK(*marker < *popup);
}

TEST_CASE("Keyframe inspector and long keyframe list scroll inside their own panels",
          "[editor][ui][timeline]") {
    Fixture f;
    f.inspector.height(480);
    f.list.height(220);
    for (int i = 0; i <= 30; ++i)
        f.source.document.keyframe_names[static_cast<f32>(i) * .25F] = "Marker";
    ++f.source.document.revision;
    f.pump();
    f.panel.show(f.source);
    f.pump();
    f.add();
    CHECK_FALSE(f.has("White spots"));
    f.pump({{.kind = EventKind::scroll, .position = {1000, 450}, .scroll = {0, -30}}});
    CHECK_FALSE(f.poll());
    CHECK(f.has("White spots"));
    f.pump({{.kind = EventKind::scroll, .position = {200, 160}, .scroll = {0, -30}}});
    CHECK_FALSE(f.poll());
    CHECK(f.has("7.5 s | Marker"));
    f.click("7.5 s | Marker");
    const auto selected = f.poll();
    REQUIRE(selected);
    CHECK(selected->time == 7.5F);
    f.applied(*selected);
    CHECK(f.panel.selected_keyframe() == 7.5F);
}

TEST_CASE("Keyframe inspector controls fit actual content widths with and without a scrollbar",
          "[editor][ui][timeline][regression][layout]") {
    Fixture f;
    f.input.logical_size = {1800, 1500};
    f.input.framebuffer = {1800, 1500};
    f.host.position({20, 1330});
    f.pump();
    f.add();
    std::optional<u64> delete_id;
    for (const auto size : {Vec2{660, 1270}, Vec2{660, 440}, Vec2{560, 440},
                            Vec2{520, 440}, Vec2{760, 1270}}) {
        INFO("Inspector size " << size.x << " x " << size.y);
        f.inspector.width(size.x).height(size.y).scroll(0);
        f.pump();
        f.panel.show(f.source);
        f.pump();
        const auto inspected = f.screen.inspect();
        REQUIRE(inspected);
        const auto host = std::ranges::find_if(inspected->widgets, [&](const auto& widget) {
            return widget.role == ui::WidgetRole::column && widget.bounds.x == 360.F &&
                   widget.bounds.y == 30.F && widget.bounds.width == size.x &&
                   widget.bounds.height == size.y;
        });
        REQUIRE(host != inspected->widgets.end());
        const auto belongs_to_inspector = [&](const ui::WidgetSnapshot& widget) {
            for (auto id = widget.parent; id;) {
                if (id == host->id) return true;
                const auto parent = std::ranges::find(inspected->widgets, id,
                                                      &ui::WidgetSnapshot::id);
                if (parent == inspected->widgets.end()) return false;
                id = parent->parent;
            }
            return false;
        };
        std::size_t checked{};
        for (const auto& widget : inspected->widgets) {
            using Role = ui::WidgetRole;
            if (!widget.in_layout || !belongs_to_inspector(widget) ||
                (widget.role != Role::button && widget.role != Role::text_field &&
                 widget.role != Role::checkbox && widget.role != Role::dropdown)) continue;
            INFO("Control " << widget.id << ": " << widget.label);
            CHECK(widget.bounds.width > 0.F);
            CHECK(widget.clip.width == Catch::Approx(widget.bounds.width).margin(.01F));
            CHECK(widget.bounds.x >= host->bounds.x);
            CHECK(widget.bounds.x + widget.bounds.width <= host->bounds.x + size.x);
            ++checked;
        }
        CHECK(checked > 40);
        const auto erase = std::ranges::find_if(inspected->widgets, [](const auto& widget) {
            return widget.role == ui::WidgetRole::button && widget.label == "Delete keyframe";
        });
        REQUIRE(erase != inspected->widgets.end());
        CHECK(erase->visible);
        CHECK(erase->bounds.width == 172.F); // full button affordance, not squeezed to fit
        CHECK(erase->clip.height == erase->bounds.height);
        if (delete_id) CHECK(erase->id == *delete_id);
        else delete_id = erase->id;
        const auto bar = std::ranges::find_if(inspected->widgets, [&](const auto& widget) {
            return widget.role == ui::WidgetRole::scrollbar && widget.parent == host->id;
        });
        if (size.y < 500) {
            REQUIRE(bar != inspected->widgets.end());
            CHECK(bar->visible);
            CHECK(*bar->maximum > 0.F);
        } else CHECK(bar == inspected->widgets.end());
    }
    // Regression: the complete button, including its rightmost pixels, responds.
    const auto inspected = f.screen.inspect();
    REQUIRE(inspected);
    const auto erase = std::ranges::find_if(inspected->widgets, [](const auto& widget) {
        return widget.role == ui::WidgetRole::button && widget.label == "Delete keyframe";
    });
    REQUIRE(erase != inspected->widgets.end());
    f.click({erase->bounds.x + erase->bounds.width - 2,
             erase->bounds.y + erase->bounds.height * .5F});
    const auto action = f.poll();
    REQUIRE(action);
    CHECK(action->kind == TimelineAction::Kind::erase);
}

TEST_CASE("Detached controls share one tabbed page and retain a compact timeline", "[editor][ui][layout]") {
    for (const auto size : {Vec2{1200,800},Vec2{1600,1000},Vec2{2560,1440}}) {
        const auto l=editor_controls_layout(size,false);
        CHECK(l.scene.x==l.inspector.x);
        CHECK(l.scene.width==l.inspector.width);
        CHECK(l.inspector.x==16);
        CHECK(l.inspector.x+l.inspector.width==Catch::Approx(size.x-16));
        CHECK(l.timeline.height==96);
        CHECK(l.inspector.y+l.inspector.height<l.timeline.y);
        CHECK(l.keyframes.y+l.keyframes.height<l.timeline.y);
        CHECK(l.timeline.y+l.timeline.height<l.files.y);
        CHECK(l.viewport.width==0);
    }
}

TEST_CASE("Editor layout expands with the decorated window", "[editor][ui][layout]") {
    auto small = editor_layout({1600, 1000}, false), big = editor_layout({2560, 1440}, false);
    CHECK(big.viewport.width > small.viewport.width);
    CHECK(big.viewport.height > small.viewport.height);
    CHECK(editor_list_height(small) >= 60.F);
    CHECK(editor_list_height(small) <= 208.F);
    CHECK(editor_list_height(big) == 208.F);
    CHECK(big.keyframes.height <= 160.F);
    CHECK(big.scene.height > big.keyframes.height * 3);
    for (auto size : {Vec2{1360, 900}, Vec2{1600, 1000}, Vec2{2560, 1440}}) {
        for (bool vertices : {false, true}) {
            const auto l = editor_layout(size, vertices);
            CHECK(l.scene.y + l.scene.height < l.keyframes.y);
            CHECK(l.scene.x == l.inspector.x);
            CHECK(l.scene.width == l.inspector.width);
            CHECK(l.viewport.x == 16);
            CHECK(l.keyframes.y + l.keyframes.height < l.timeline.y);
            CHECK(l.viewport.x + l.viewport.width < l.inspector.x);
            CHECK(l.inspector.y + l.inspector.height < l.timeline.y);
            CHECK(l.timeline.y + l.timeline.height < l.files.y);
            CHECK(l.status.y + l.status.height <= size.y);
        }
    }
}

TEST_CASE("Sidebar divider proportions survive resizing and never collapse either pane", "[editor][ui][layout][splitter]") {
    SidebarSizing sizing;
    auto initial = editor_layout({1600,1000},false);
    const auto original = initial;
    sizing.apply(initial);
    CHECK(initial.scene.height == original.scene.height);
    const auto initial_sections=sizing.sections(initial);
    for(std::size_t i=0;i<initial_sections.size();++i) CHECK(initial_sections[i]>=SidebarSizing::minimum[i]);
    sizing.resize_panels(SidebarSizing::panels(initial).total*.65F,initial);
    sizing.apply(initial);
    sizing.resize_section(0,sizing.divider(0,initial).maximum,initial);
    for (const auto size : {Vec2{1600,1000}, Vec2{2560,1440}, Vec2{800,500}})
        for (const bool detached : {false,true}) {
            auto layout = detached ? editor_controls_layout(size,false) : editor_layout(size,false);
            const auto viewport = layout.viewport;
            sizing.apply(layout);
            const auto panels = SidebarSizing::panels(layout);
            CHECK(layout.scene.height == Catch::Approx(panels.clamp(panels.total*.65F)));
            CHECK(sizing.sections(layout)[1] == Catch::Approx(SidebarSizing::minimum[1]));
            CHECK(layout.scene.height >= panels.minimum);
            CHECK(layout.keyframes.height > 0);
            CHECK(layout.scene.y + layout.scene.height + 12 == Catch::Approx(layout.keyframes.y));
            CHECK(layout.keyframes.y + layout.keyframes.height == Catch::Approx(layout.inspector.y+layout.inspector.height));
            CHECK(layout.viewport.width == viewport.width); CHECK(layout.viewport.height == viewport.height);
            for (const auto extreme : {-1e6F,1e6F}) {
                auto clamped = sizing;
                clamped.resize_panels(extreme,layout); clamped.apply(layout);
                CHECK(layout.scene.height >= panels.minimum); CHECK(layout.scene.height <= panels.maximum);
                for(std::size_t i=0;i<SidebarSizing::section_count-1;++i) {
                    const auto before=clamped.sections(layout);
                    const auto bounds=clamped.divider(i,layout);
                    clamped.resize_section(i,extreme,layout);
                    const auto after=clamped.sections(layout);
                    CHECK(after[i] >= bounds.minimum-.001F); CHECK(after[i] <= bounds.maximum+.001F);
                    CHECK(after[i]+after[i+1] == Catch::Approx(before[i]+before[i+1]));
                    for(std::size_t j=0;j<after.size();++j) if(j!=i&&j!=i+1) CHECK(after[j]==Catch::Approx(before[j]));
                }
            }
        }
}

TEST_CASE("Right sidebar reclaims fixed-aspect playback space without a left column",
          "[editor][ui][layout][regression]") {
    for (const auto size : {Vec2{1600,1000},Vec2{2560,1440},Vec2{3440,1440},Vec2{5120,1440}})
        for (const auto scale : {.75F,1.F,1.25F,1.5F})
            for (const auto aspect : {4.F/3,16.F/9,9.F/16})
                for (const bool vertices : {false,true}) {
                    const Vec2 logical{size.x/scale,size.y/scale};
                    const auto layout=editor_layout(logical,vertices,aspect);
                    const auto base_right=std::clamp(logical.x*.24F,480.F,600.F);
                    const auto available=logical.x-base_right-44;
                    const auto fitted=std::min(available,layout.viewport.height*aspect);
                    const auto extra=available-fitted;
                    CHECK(layout.viewport.width==Catch::Approx(fitted).margin(.001));
                    CHECK(layout.scene.width==layout.inspector.width);
                    CHECK(layout.keyframes.width==layout.scene.width);
                    CHECK(layout.inspector.width==Catch::Approx(base_right+extra));
                    CHECK(layout.tabs.width==layout.inspector.width);
                    CHECK(layout.scene.x==layout.inspector.x);
                    CHECK(layout.viewport.x+layout.viewport.width+12==Catch::Approx(layout.inspector.x));
                    CHECK(layout.caption.x==layout.viewport.x);
                    CHECK(layout.caption.width==layout.viewport.width);
                    CHECK(layout.viewport.x==16);
                    CHECK(layout.inspector.x+layout.inspector.width==Catch::Approx(logical.x-16));
                }
}

TEST_CASE("Editable preview fills the space left of the tabbed sidebar", "[editor][ui][layout]") {
    for (const auto size : {Vec2{1000,700},Vec2{1600,1000},Vec2{2560,1440}}) {
        const auto l=editor_layout(size,false);
        CHECK(l.viewport.width==Catch::Approx(size.x-l.inspector.width-44));
        CHECK(l.viewport.x+l.viewport.width+12==Catch::Approx(l.inspector.x));
        CHECK(l.tabs.x==l.inspector.x);
        CHECK(l.scene.y==l.inspector.y);
        CHECK(l.keyframes.y+l.keyframes.height==Catch::Approx(l.inspector.y+l.inspector.height));
    }
}

TEST_CASE("Pasted keyframe becomes selectable in its dedicated inspector", "[editor][ui][clipboard]") {
    Fixture f;
    REQUIRE(key_property(f.source,{1,"scale"},1,2.F));
    f.source.document.keyframe_names[1]="Arrival";
    EditClipboard clipboard;
    REQUIRE(clipboard.copy_keyframe(f.source,1));
    f.source.viewport.time=5;
    const auto pasted=clipboard.paste(f.source); REQUIRE(pasted); REQUIRE(pasted->keyframe);
    ++f.source.document.revision;
    f.panel.show(f.source);
    f.panel.select_keyframe(*pasted->keyframe);
    f.panel.show(f.source);
    CHECK(f.panel.selected_keyframe()==5.F);
    CHECK_FALSE(f.draw().commands.empty());
}

TEST_CASE("Selecting another keyframe discards the previous unsent draft",
          "[editor][ui][timeline][regression]") {
    Fixture f;
    REQUIRE(key_property(f.source, {1, "scale"}, 1, 1.5F));
    REQUIRE(key_property(f.source, {1, "scale"}, 5, 2.5F));
    ++f.source.document.revision;
    f.source.viewport.time = 1;
    f.panel.select_keyframe(1);
    f.panel.show(f.source);
    f.pump();
    f.edit(f.field_after("Name"), "Unsent name");
    CHECK_FALSE(f.poll());
    f.source.viewport.time = 5;
    f.panel.select_keyframe(5);
    f.panel.show(f.source);
    f.pump();
    CHECK(f.panel.selected_keyframe() == 5.F);
    CHECK_FALSE(f.has("Unsent name"));
    CHECK(f.has("KEYFRAME / 5 seconds"));
    f.click("Apply to keyframe");
    const auto edit = f.poll();
    REQUIRE(edit);
    CHECK(edit->time == 5.F);
    CHECK(edit->values.empty());
    CHECK(f.has("2.5"));
}

TEST_CASE("Timeline supports range and toggle selections without rebuilding the document", "[editor][ui][multiselect]") {
    Fixture f;
    for(f32 time:{1.F,2.F,3.F}) REQUIRE(key_property(f.source,{1,"scale"},time,1.F));
    ++f.source.document.revision;f.panel.show(f.source);f.pump();
    const auto revision=f.source.document.revision;
    const auto refreshes=f.panel.statistics().document_refreshes;
    f.click("1 s | Keyframe");REQUIRE(f.poll());f.pump();
    f.click(f.at("3 s | Keyframe"),{.shift=true});REQUIRE(f.poll());f.pump();
    CHECK(f.panel.selected_keyframes().size()==3);CHECK(f.panel.selected_keyframe()==3.F);
    CHECK(f.has("KEYFRAMES / 3 selected"));
    CHECK(f.has("+ 1 s | Keyframe"));CHECK(f.has("+ 2 s | Keyframe"));
    f.click(f.at("+ 2 s | Keyframe"),{.control=true});REQUIRE(f.poll());f.pump();
    CHECK(f.panel.selected_keyframes().size()==2);CHECK(f.panel.selected_keyframe()==3.F);
    const auto bounds=f.canvas();
    const Vec2 marker{bounds.x+bounds.width*.3F,bounds.y+40};
    f.click(marker,{.control=true});REQUIRE(f.poll());f.pump();
    CHECK(f.panel.selected_keyframes().size()==1);CHECK(f.panel.selected_keyframe()==1.F);
    f.click(f.at("3 s | Keyframe"),{.control=true});REQUIRE(f.poll());f.pump();
    f.click("Delete selected keys");const auto action=f.poll();REQUIRE(action);
    CHECK(action->kind==TimelineAction::Kind::erase);CHECK(action->selection==std::vector<f32>{1,3});
    CHECK(f.source.document.revision==revision);
    CHECK(f.panel.statistics().document_refreshes==refreshes);
    REQUIRE(erase_keyframe(f.source,3));++f.source.document.revision;f.panel.show(f.source);
    CHECK(f.panel.selected_keyframe()==1.F);CHECK(f.panel.selected_keyframes().size()==1);
    f.panel.clear_selection();CHECK(f.panel.selected_keyframes().empty());
    f.pump();CHECK(f.has("KEYFRAMES / 0 selected"));
}

TEST_CASE("Timeline marker ranges and list toggles share selection across input frames",
          "[editor][ui][timeline][multiselect]") {
    Fixture f;
    for (f32 time : {1.F, 2.F, 3.F, 4.F})
        REQUIRE(key_property(f.source, {1, "scale"}, time, 1.F));
    ++f.source.document.revision;
    f.panel.show(f.source);
    f.pump();
    const auto revision = f.source.document.revision;
    const auto refreshes = f.panel.statistics().document_refreshes;
    const auto select = [&](Vec2 point, input::Modifiers modifiers) {
        // Real clicks usually span frames. Refreshing the panel between the
        // press and release must preserve the selection and range anchor.
        f.pump({{.kind=EventKind::pointer_down, .position=point, .modifiers=modifiers}});
        if (auto action = f.poll()) f.source.viewport.time = action->time;
        f.panel.show(f.source);
        f.pump({{.kind=EventKind::pointer_up, .position=point, .modifiers=modifiers}});
        if (auto action = f.poll()) f.source.viewport.time = action->time;
        f.panel.show(f.source);
        f.pump();
    };
    const auto marker = [&](f32 time) {
        const auto bounds = f.canvas();
        return Vec2{bounds.x + bounds.width * time / f.source.document.timeline_duration,
                    bounds.y + 40};
    };
    select(marker(4), {});
    select(marker(2), {.shift=true});
    CHECK(std::vector<f32>(f.panel.selected_keyframes().begin(), f.panel.selected_keyframes().end()) ==
          std::vector<f32>{2, 3, 4});
    CHECK(f.panel.selected_keyframe() == 2.F);
    // Shrink, then extend the same range: the anchor is still at 4 seconds.
    select(marker(3), {.shift=true});
    CHECK(f.panel.selected_keyframes().size() == 2);
    select(marker(1), {.shift=true});
    CHECK(f.panel.selected_keyframes().size() == 4);
    select(f.at("+ 3 s | Keyframe"), {.control=true});
    CHECK(f.panel.selected_keyframes().size() == 3);
    CHECK(f.has("KEYFRAMES / 3 selected"));
    select(marker(1), {.control=true});
    CHECK(f.panel.selected_keyframe() == 4.F);
    f.click("Delete selected keys");
    const auto action = f.poll();
    REQUIRE(action);
    CHECK(action->kind == TimelineAction::Kind::erase);
    CHECK(action->selection == std::vector<f32>{2, 4});
    CHECK(f.source.document.revision == revision);
    CHECK(f.panel.statistics().document_refreshes == refreshes);
}

TEST_CASE("Selecting an instance reveals its keyframe entry without losing the draft",
          "[editor][ui][timeline]") {
    Fixture f;
    const auto prototype = f.source.document.instances.front();
    for (u32 id=3; id<=30; ++id) {
        auto instance = prototype;
        instance.id = id;
        instance.name = "Fleet ship " + std::to_string(id);
        f.source.document.instances.push_back(std::move(instance));
    }
    ++f.source.document.revision;
    f.panel.show(f.source);
    f.pump();
    f.add();
    const auto selected = f.panel.selected_keyframe();
    const auto revision = f.source.document.revision;
    f.edit(f.field_after("Name"), "Unsaved draft");
    CHECK_FALSE(f.poll());
    f.panel.focus_object(30);
    f.pump();
    CHECK(f.panel.selected_keyframe() == selected);
    CHECK(f.source.document.revision == revision);
    CHECK(f.has("Mesh / Fleet ship 30"));
    const auto inspection = f.screen.inspect();
    REQUIRE(inspection);
    CHECK(std::ranges::any_of(inspection->widgets, [](const auto& widget) {
        return widget.focused && widget.visible && widget.label == "Key";
    }));
    f.inspector.scroll(0);
    f.pump();
    f.click("Apply to keyframe");
    const auto action = f.poll();
    REQUIRE(action);
    CHECK(action->name == "Unsaved draft");
}

TEST_CASE("Box capture handles threshold, outside release, reverse rectangles and cancellation", "[editor][ui][multiselect]") {
    BoxSelection box;constexpr ui::Rect viewport{10,20,100,80};
    box.begin({30,40},viewport,{});
    CHECK_FALSE(box.update({.kind=EventKind::pointer_move,.position={31,41}}));CHECK_FALSE(box.dragging());
    CHECK_FALSE(box.update({.kind=EventKind::pointer_up,.position={31,41}}));CHECK_FALSE(box.active());
    box.begin({70,70},viewport,{.shift=true});
    CHECK_FALSE(box.update({.kind=EventKind::pointer_move,.position={-50,-50}}));CHECK(box.dragging());
    ui::DrawList list;box.append(list);CHECK(list.commands.size()==1);
    const auto result=box.update({.kind=EventKind::pointer_up,.position={-50,-50}});REQUIRE(result);
    CHECK(result->rect.x==10.F);CHECK(result->rect.y==20.F);CHECK(result->rect.width==60.F);CHECK(result->rect.height==50.F);
    CHECK(result->mode==editor::SelectionMode::add);
    box.begin({30,40},viewport,{.control=true});
    CHECK_FALSE(box.update({.kind=EventKind::focus_lost}));CHECK_FALSE(box.active());
    box.begin({30,40},viewport,{});
    CHECK_FALSE(box.update({.kind=EventKind::key_down,.key=Key::escape}));CHECK_FALSE(box.active());
}
