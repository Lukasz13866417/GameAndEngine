#include "../../examples/editor/view_selector.hpp"
#include <vng/ui/inspection.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <set>

namespace {
using namespace vng;
using namespace editor_example;
using Role = ui::WidgetRole;

State source(std::string name = "Cube") {
    content::vmesh::Document document;
    if (!name.empty()) document.metadata["name"] = std::move(name);
    document.vertex_count = 3;
    document.vertex_fields = {{"position", {content::vmesh::ScalarType::Float32, 3},
                               std::vector<f32>{0, 0, 0, 1, 0, 0, 0, 1, 0}}};
    document.faces = {{0, 1, 2}};
    auto mesh = editor::EditableMesh::create(std::move(document));
    REQUIRE(mesh);
    return {.document = {.mesh = std::move(*mesh)}};
}
BlueprintId add_asset(State& state, std::string name) {
    const auto id = static_cast<BlueprintId>(state.document.next_blueprint_id++);
    state.document.mesh_assets.push_back({id, std::move(name), state.document.mesh, {}});
    return id;
}
std::string label(const State& state, BlueprintId id) {
    const auto choices = view_choices(state);
    const auto found = std::ranges::find_if(choices, [&](const auto& item) {
        return item.value == ViewSelection{ViewMode::mesh, id};
    });
    REQUIRE(found != choices.end());
    return found->label;
}
text::Font font() {
    auto result = text::Font::load(VNG_TEST_FONT_PATH);
    REQUIRE(result);
    return *result;
}
struct Fixture {
    State state;
    ui::Screen screen{ui::dark_theme(font())};
    ui::Container host = screen.column().position({20, 20}).width(400).height(40);
    ViewSelector selector{host, state};
    input::Frame frame{.logical_size = {800, 600}, .framebuffer = {800, 600}};
    explicit Fixture(State initial = source()) : state(std::move(initial)) { pump(); }
    void pump(std::initializer_list<input::Event> events = {}) {
        frame.events = events;
        for (const auto& event : events)
            if (event.kind == input::EventKind::pointer_down || event.kind == input::EventKind::pointer_up)
                frame.pointer = event.position;
        REQUIRE(screen.update(frame, .016F));
    }
    ui::Inspection tree() const {
        const auto inspected = screen.inspect();
        REQUIRE(inspected);
        return *inspected;
    }
    ui::WidgetSnapshot find(Role role, std::string_view caption = {}) const {
        const auto inspected = tree();
        const auto found = std::ranges::find_if(inspected.widgets, [&](const auto& item) {
            return item.role == role && (caption.empty() || item.label == caption) && item.visible;
        });
        REQUIRE(found != inspected.widgets.end());
        return *found;
    }
    bool has_option(std::string_view caption) const {
        const auto inspected = tree();
        return std::ranges::any_of(inspected.widgets, [&](const auto& item) {
            return item.role == Role::option && item.visible && item.label == caption;
        });
    }
    void click(const ui::WidgetSnapshot& node) {
        const Vec2 point{node.bounds.x + node.bounds.width * .5F, node.bounds.y + node.bounds.height * .5F};
        pump({{.kind = input::EventKind::pointer_down, .position = point},
              {.kind = input::EventKind::pointer_up, .position = point}});
    }
    void open() { click(find(Role::dropdown, "View")); }
    void show() { selector.show(state); pump(); }
    std::size_t dropdown_count() const {
        const auto inspected = tree();
        return static_cast<std::size_t>(std::ranges::count(inspected.widgets, Role::dropdown, &ui::WidgetSnapshot::role));
    }
};
} // namespace

TEST_CASE("View destinations directly identify every mesh including orphan blueprints",
          "[editor][ui][view-selector]") {
    auto state = source("Original cube");
    const auto ship = add_asset(state, "Spaceship");
    const auto choices = view_choices(state);
    REQUIRE(choices.size() == 4);
    CHECK(choices.front().value == ViewSelection{ViewMode::scene});
    CHECK(choices.front().label == "Scene");
    CHECK(choices.back().value == ViewSelection{ViewMode::sun});
    CHECK(choices.back().label == "Sun effect");
    CHECK(label(state, BlueprintId::mesh) == "Mesh: Original cube");
    CHECK(label(state, ship) == "Mesh: Spaceship");
    CHECK(std::ranges::none_of(choices, [](const auto& choice) { return choice.label == "Blueprint mesh"; }));
    CHECK(std::ranges::none_of(state.document.instances, [&](const auto& instance) { return instance.blueprint == ship; }));
    CHECK(label(source(""), BlueprintId::mesh) == "Mesh: Mesh");
    CHECK(label(source(std::string(257, 'x')), BlueprintId::mesh) == "Mesh: Mesh");
    CHECK(label(source("Cube\nA\tB"), BlueprintId::mesh) == "Mesh: Cube A B");
    CHECK(label(source("Sześcian"), BlueprintId::mesh) == "Mesh: Sześcian");
}

TEST_CASE("View labels remain unique when asset names duplicate or imitate ID suffixes",
          "[editor][ui][view-selector]") {
    auto state = source("Same");
    const auto a = add_asset(state, "Same");
    const auto b = add_asset(state, "Same #1");
    const auto c = add_asset(state, "Same #1 #1");
    const auto choices = view_choices(state);
    std::set<std::string> labels;
    std::set<std::pair<ViewMode, BlueprintId>> targets;
    for (const auto& choice : choices) {
        CHECK(labels.insert(choice.label).second);
        CHECK(targets.emplace(choice.value.mode, choice.value.blueprint).second);
    }
    CHECK(label(state, BlueprintId::mesh).starts_with("Mesh: Same #1"));
    CHECK(label(state, a).starts_with("Mesh: Same #3"));
    CHECK(label(state, b) == "Mesh: Same #1");
    CHECK(label(state, c) == "Mesh: Same #1 #1");
    state.viewport.selected_object = 0;
    state.viewport.editor_camera.yaw += 12;
    ++state.document.revision;
    const auto again = view_choices(state);
    CHECK(std::ranges::equal(choices, again, [](const auto& left, const auto& right) {
        return left.value == right.value && left.label == right.label;
    }));
}

TEST_CASE("Top View selects cube while a ship instance is selected without mutating scene data",
          "[editor][ui][view-selector]") {
    auto state = source();
    const auto asset = add_asset(state, "Ship");
    const auto instance = instantiate(state, asset);
    REQUIRE(instance);
    Fixture f{std::move(state)};
    const auto original = encode(f.state);
    REQUIRE(original);
    f.open();
    REQUIRE(f.has_option("Mesh: Cube"));
    REQUIRE(f.has_option("Mesh: Ship"));
    f.click(f.find(Role::option, "Mesh: Cube"));
    const auto selection = f.selector.changedValue();
    REQUIRE(selection);
    CHECK(*selection == ViewSelection{ViewMode::mesh, BlueprintId::mesh});
    CHECK(*encode(f.state) == *original);
    CHECK(f.state.viewport.selected_object == *instance);
    REQUIRE(inspect_mesh(f.state, selection->blueprint));
    f.show();
    CHECK(f.find(Role::dropdown, "View").text == "Mesh: Cube");
    CHECK_FALSE(f.selector.changedValue()); // Reflecting authoring state is not another user edit.
    CHECK(f.state.viewport.selected_object == *instance);
    CHECK(editable_mesh(f.state) == &f.state.document.mesh);
    CHECK(f.dropdown_count() == 1);
}

TEST_CASE("View redraws preserve widget and open popup across camera revisions and geometry edits",
          "[editor][ui][view-selector]") {
    Fixture f;
    const auto id = f.find(Role::dropdown, "View").id;
    f.open();
    REQUIRE(f.find(Role::dropdown, "View").expanded);
    f.state.viewport.editor_camera.yaw += 10;
    f.state.document.revision += 1;
    REQUIRE(f.state.document.mesh.set_position(0, {0, .2F, 0}));
    f.show();
    CHECK(f.find(Role::dropdown, "View").id == id);
    CHECK(f.find(Role::dropdown, "View").expanded);
    CHECK(f.has_option("Mesh: Cube"));
    CHECK_FALSE(f.selector.changedValue());
    CHECK(f.dropdown_count() == 1);
}

TEST_CASE("View catalog rebuilds for import Undo Redo and loaded documents but not instance deletion",
          "[editor][ui][view-selector]") {
    Fixture f;
    const auto original = f.state;
    const auto original_id = f.find(Role::dropdown, "View").id;
    const auto asset = add_asset(f.state, "Ship");
    const auto instance = instantiate(f.state, asset);
    REQUIRE(instance);
    REQUIRE(inspect_mesh(f.state, asset));
    f.show();
    const auto imported_id = f.find(Role::dropdown, "View").id;
    CHECK(imported_id != original_id);
    CHECK(f.find(Role::dropdown, "View").text == "Mesh: Ship");
    CHECK(f.dropdown_count() == 1);
    REQUIRE(erase_instance(f.state, *instance));
    f.show();
    CHECK(f.find(Role::dropdown, "View").id == imported_id);
    CHECK(f.find(Role::dropdown, "View").text == "Mesh: Ship");
    f.open();
    CHECK(f.has_option("Mesh: Ship"));
    const auto retained = f.state;

    f.state = original; // An undo restores a complete prior authored state.
    f.show();
    CHECK(f.find(Role::dropdown, "View").id != imported_id);
    CHECK_FALSE(f.find(Role::dropdown, "View").expanded);
    f.open();
    CHECK_FALSE(f.has_option("Mesh: Ship"));
    CHECK(f.has_option("Mesh: Cube"));
    f.state = retained;
    f.show();
    CHECK(f.find(Role::dropdown, "View").text == "Mesh: Ship");
    CHECK(f.dropdown_count() == 1);
    const auto saved = encode(f.state);
    REQUIRE(saved);
    auto loaded = decode(*saved);
    REQUIRE(loaded);
    loaded->document.mesh_assets.front().name = "Loaded ship";
    f.state = std::move(*loaded);
    f.show();
    CHECK(f.find(Role::dropdown, "View").text == "Mesh: Loaded ship");
    f.open();
    CHECK(f.has_option("Mesh: Loaded ship"));
    CHECK_FALSE(f.has_option("Mesh: Ship"));
    CHECK(f.dropdown_count() == 1);
}

TEST_CASE("Disabled View selector does not accept pointer edits",
          "[editor][ui][view-selector]") {
    Fixture f;
    f.selector.enabled(false);
    f.pump();
    const auto control = f.find(Role::dropdown, "View");
    CHECK_FALSE(control.enabled);
    f.click(control);
    CHECK_FALSE(f.selector.changedValue());
    CHECK_FALSE(f.find(Role::dropdown, "View").expanded);
    f.selector.enabled(true);
    f.pump();
    f.open();
    CHECK(f.find(Role::dropdown, "View").expanded);
}
