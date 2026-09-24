#include "../../examples/editor/effects.hpp"
#include "../../examples/editor/animation.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>

namespace {
using namespace vng;
using namespace editor_example;
State scene(u32 selected) {
    content::vmesh::Document geometry;
    geometry.vertex_count = 3;
    geometry.vertex_fields = {{"position", {content::vmesh::ScalarType::Float32, 3},
                               std::vector<f32>{-1, -1, 0, 1, -1, 0, 0, 1, 0}}};
    geometry.faces = {{0, 1, 2}};
    auto mesh = editor::EditableMesh::create(std::move(geometry));
    REQUIRE(mesh);
    State result{.document = {.mesh = std::move(*mesh)}};
    result.viewport.selected_object = selected;
    return result;
}
struct Fixture {
    State state;
    ProjectControls controls;
    editor::Inspector inspector;
    explicit Fixture(u32 selected = 1)
        : state(scene(selected)), controls(state), inspector(selected, 1, state.document.revision) { describe(); }
    void describe() {
        inspector = editor::Inspector{state.viewport.selected_object, 1, state.document.revision};
        controls.describe_editor(inspector);
        REQUIRE(editor::validate(inspector.schema()));
    }
    const editor::Control& control(std::string_view key) const {
        const auto& values = inspector.schema().controls;
        const auto found = std::ranges::find(values, key, &editor::Control::key);
        REQUIRE(found != values.end());
        return *found;
    }
    editor::Value value(std::string_view group, std::string_view key) const {
        const auto& fields = control(group).fields;
        const auto found = std::ranges::find(fields, key, &editor::Field::key);
        REQUIRE(found != fields.end());
        return found->value;
    }
    std::vector<editor::NamedValue> full_patch(std::string_view group, std::string_view key, editor::Value value) const {
        std::vector<editor::NamedValue> patch;
        for (const auto& field : control(group).fields)
            patch.push_back({field.key, field.key == key ? value : field.value});
        return patch;
    }
    editor::Result<void> dispatch(std::string key, std::vector<editor::NamedValue> values,
                                  editor::Phase phase = editor::Phase::apply) {
        auto result = inspector.dispatch({inspector.schema().stamp, std::move(key), phase, std::move(values)});
        if (result) state.document.revision = inspector.schema().stamp.revision;
        return result;
    }
    SceneInstance evaluated() const { return evaluate_instance(state, *find_instance(state, state.viewport.selected_object), state.viewport.time); }
};
} // namespace

TEST_CASE("Instance transform Apply changes only selected placement and never shared geometry", "[editor][effects]") {
    Fixture f;
    auto duplicate = instantiate(f.state, BlueprintId::mesh);
    REQUIRE(duplicate);
    const auto other = find_instance(f.state, *duplicate)->transform;
    f.state.viewport.selected_object = 1;
    f.describe();
    const auto* positions = std::get<std::vector<f32>>(f.state.document.mesh.document().vertex_fields[0].values).data();
    REQUIRE(f.control("transform").label == "Instance transform");
    CHECK(f.control("transform").apply_label == "Apply instance transform");
    CHECK(f.control("model").apply_label == "Apply appearance");
    CHECK(f.control("model").fields.size() == 3);
    REQUIRE(f.dispatch("transform", {{"scale", 1.4F}, {"rotation", Vec3{0, 0, 45}}}));
    CHECK(find_instance(f.state, 1)->transform.scale == 1.4F);
    CHECK(find_instance(f.state, 1)->transform.rotation == Vec3{0, 0, 45});
    CHECK(find_instance(f.state, *duplicate)->transform == other);
    CHECK(f.state.document.timeline.tracks().empty());
    CHECK(std::get<std::vector<f32>>(f.state.document.mesh.document().vertex_fields[0].values).data() == positions);
    // Repeated dispatch without rebuilding still compares against the last
    // successfully displayed/submitted baseline, not the original factory value.
    REQUIRE(f.dispatch("transform", {{"scale", .7F}, {"rotation", Vec3{}}}));
    CHECK(find_instance(f.state, 1)->transform.scale == .7F);
    CHECK(find_instance(f.state, 1)->transform.rotation == Vec3{});
    REQUIRE(f.dispatch("model", {{"brightness", 2.F}}));
    CHECK(mesh_settings(f.state, 1)->brightness == 2.F);
    CHECK(find_instance(f.state, 1)->transform.scale == .7F);
}

TEST_CASE("Scale Apply edits the visible timeline value instead of an overridden base", "[editor][effects]") {
    Fixture f;
    f.state.viewport.time = 3;
    f.state.document.keyframe_names[3] = "Editable pose";
    REQUIRE(f.state.document.timeline.set({1, "scale"}, {0, .5F, timeline::Interpolation::hold}, "Size", "Ship"));
    REQUIRE(f.state.document.timeline.set({1, "rotation"}, {0, Vec3{0, 0, 90}, timeline::Interpolation::hold}));
    const auto base = find_instance(f.state, 1)->transform;
    const auto rotation = *f.state.document.timeline.find({1, "rotation"});
    f.describe();
    CHECK(std::get<f32>(f.value("transform", "scale")) == .5F);
    CHECK(std::get<Vec3>(f.value("transform", "rotation")) == Vec3{0, 0, 90});
    REQUIRE(f.dispatch("transform", f.full_patch("transform", "scale", 1.2F)));
    CHECK(f.evaluated().transform.scale == 1.2F);
    CHECK(find_instance(f.state, 1)->transform == base);
    CHECK(*f.state.document.timeline.find({1, "rotation"}) == rotation);
    const auto* track = f.state.document.timeline.find({1, "scale"});
    REQUIRE(track);
    REQUIRE(track->keys.size() == 2);
    CHECK(track->keys.back().time == f.state.viewport.time);
    CHECK(track->keys.back().incoming == timeline::Interpolation::linear);
    CHECK(track->label == "Size");
    CHECK(track->layer == "Ship");
}

TEST_CASE("Native controls report only committed targets and drain their change information", "[editor][effects][patch]") {
    Fixture f;
    CHECK(f.controls.take_changes().empty());
    REQUIRE(f.dispatch("transform", {{"scale", 1.4F}, {"rotation", Vec3{0, 0, 45}}}));
    auto changes = f.controls.take_changes();
    CHECK_FALSE(changes.full);
    CHECK(changes.vertices.empty());
    CHECK(changes.properties == std::set<timeline::Target, TargetLess>{{1, "scale"}, {1, "rotation"}});
    CHECK(f.controls.take_changes().empty());
    REQUIRE(f.dispatch("transform", {{"scale", 1.4F}, {"rotation", Vec3{0, 0, 45}}}));
    CHECK(f.controls.take_changes().empty());
    CHECK_FALSE(f.dispatch("transform", {{"scale", 1.2F}, {"rotation", Vec3{0, 400, 0}}}));
    CHECK(f.controls.take_changes().empty());
    CHECK(instance_transform(f.state, 1)->scale == 1.4F);
    REQUIRE(f.dispatch("model", {{"wireframe", true}}));
    CHECK(f.controls.take_changes().properties == std::set<timeline::Target, TargetLess>{{1, "wireframe"}});
}

TEST_CASE("Instance Apply preserves an existing scale key's incoming interpolation", "[editor][effects]") {
    Fixture f;
    f.state.viewport.time = 3;
    REQUIRE(f.state.document.timeline.set({1, "scale"}, {0, .5F, timeline::Interpolation::hold}));
    REQUIRE(f.state.document.timeline.set({1, "scale"}, {f.state.viewport.time, .8F, timeline::Interpolation::hold}));
    f.describe();
    REQUIRE(f.dispatch("transform", {{"scale", 1.1F}}));
    const auto* track = f.state.document.timeline.find({1, "scale"});
    REQUIRE(track->keys.size() == 2);
    CHECK(track->keys.back().incoming == timeline::Interpolation::hold);
    CHECK(std::get<f32>(*f.state.document.timeline.sample({1, "scale"}, 2.99F)) == .5F);
    CHECK(std::get<f32>(*f.state.document.timeline.sample({1, "scale"}, 3.F)) == 1.1F);
}

TEST_CASE("Appearance Apply leaves untouched animated group fields and transforms alone", "[editor][effects]") {
    Fixture f;
    REQUIRE(f.state.document.timeline.set({1, "brightness"}, {0, 2.F, timeline::Interpolation::hold}));
    REQUIRE(f.state.document.timeline.set({1, "visible"}, {0, false, timeline::Interpolation::hold}));
    const auto before = f.state.document.timeline;
    const auto transform = find_instance(f.state, 1)->transform;
    f.describe();
    REQUIRE(f.dispatch("model", f.full_patch("model", "wireframe", true)));
    CHECK(mesh_settings(f.state, 1)->wireframe);
    CHECK(mesh_settings(f.state, 1)->brightness == 1.F);
    CHECK(mesh_settings(f.state, 1)->visible);
    CHECK(f.state.document.timeline == before);
    CHECK(find_instance(f.state, 1)->transform == transform);
}

TEST_CASE("Invalid transform groups roll back earlier base and key mutations atomically", "[editor][effects]") {
    Fixture f;
    SECTION("unkeyed scale staged first") {}
    SECTION("animated scale staged first") {
        REQUIRE(f.state.document.timeline.set({1, "scale"}, {0, .5F, timeline::Interpolation::hold}));
    }
    f.describe();
    const auto instance = *find_instance(f.state, 1);
    const auto keys = f.state.document.timeline;
    const auto stamp = f.inspector.schema().stamp;
    CHECK_FALSE(f.dispatch("transform", {{"scale", 1.3F}, {"rotation", Vec3{0, 361, 0}}}));
    CHECK(*find_instance(f.state, 1) == instance);
    CHECK(f.state.document.timeline == keys);
    CHECK(f.inspector.schema().stamp == stamp);
}

TEST_CASE("Playback prevents both keyed and unkeyed instance property edits", "[editor][effects]") {
    Fixture f;
    REQUIRE(f.state.document.timeline.set({1, "scale"}, {0, .5F, timeline::Interpolation::hold}));
    f.state.viewport.paused = false;
    f.describe();
    const auto instance = *find_instance(f.state, 1);
    const auto keys = f.state.document.timeline;
    CHECK_FALSE(f.dispatch("transform", {{"scale", 1.3F}}));
    CHECK(*find_instance(f.state, 1) == instance);
    CHECK(f.state.document.timeline == keys);
    CHECK_FALSE(f.dispatch("transform", f.full_patch("transform", "rotation", Vec3{0, 0, 25})));
    CHECK(*find_instance(f.state, 1) == instance);
    CHECK(f.state.document.timeline == keys);
}

TEST_CASE("Sun surface Apply uses the same changed-only animation semantics", "[editor][effects]") {
    Fixture f{2};
    REQUIRE(f.state.document.timeline.set({2, "bloom"}, {0, .4F, timeline::Interpolation::hold}));
    REQUIRE(f.state.document.timeline.set({2, "radius"}, {0, 2.F, timeline::Interpolation::hold}));
    const auto radius = *f.state.document.timeline.find({2, "radius"});
    const auto base = *sun_settings(f.state, 2);
    f.describe();
    REQUIRE(f.dispatch("surface", f.full_patch("surface", "bloom", .7F)));
    CHECK(std::get<SunSettings>(f.evaluated().settings).bloom == .7F);
    CHECK(sun_settings(f.state, 2)->bloom == base.bloom);
    CHECK(*f.state.document.timeline.find({2, "radius"}) == radius);
    REQUIRE(f.dispatch("transform", {{"scale", 1.5F}, {"rotation", Vec3{0, 45, 0}}}));
    CHECK(find_instance(f.state, 2)->transform.scale == 1.5F);
    CHECK(find_instance(f.state, 2)->transform.rotation == Vec3{0, 45, 0});
    CHECK(sun_settings(f.state, 2)->radius == base.radius);
}

TEST_CASE("Blueprint inspection cannot accidentally edit scene instance placement", "[editor][effects]") {
    Fixture f;
    f.state.viewport.mode = ViewMode::mesh;
    f.describe();
    CHECK(f.inspector.schema().controls.empty());
    f.state.viewport.mode = ViewMode::sun;
    f.state.viewport.selected_object = 2;
    f.describe();
    CHECK(std::ranges::none_of(f.inspector.schema().controls, [](const auto& control) {
        return control.key == "transform" || control.key == "position";
    }));
    CHECK(f.control("surface").apply_label == "Apply surface");
}

TEST_CASE("Animated surface edits reject playback without partially applying other appearance fields", "[editor][effects]") {
    Fixture f{2};
    REQUIRE(f.state.document.timeline.set({2, "bloom"}, {0, .4F, timeline::Interpolation::hold}));
    f.state.viewport.paused = false;
    f.describe();
    const auto before = *find_instance(f.state, 2);
    const auto keys = f.state.document.timeline;
    // Radius is an earlier, unkeyed field in the callback. The later animated
    // bloom rejection must roll that already-staged radius edit back as well.
    CHECK_FALSE(f.dispatch("surface", {{"radius", 2.F}, {"bloom", .7F}}));
    CHECK(*find_instance(f.state, 2) == before);
    CHECK(f.state.document.timeline == keys);
    f.state.viewport.paused = true;
    f.state.viewport.mode = ViewMode::sun;
    f.describe();
    CHECK_FALSE(f.dispatch("surface", {{"bloom", .7F}}));
    CHECK(*find_instance(f.state, 2) == before);
    CHECK(f.state.document.timeline == keys);
}

TEST_CASE("The inspector's position gizmo puts an orbiting camera's eye where it is dropped", "[editor][effects][camera]") {
    Fixture f;
    f.state.viewport.editor_camera = {0, 0, 10, {}, 1};
    const auto camera = instantiate(f.state, BlueprintId::camera);
    REQUIRE(camera);
    REQUIRE(set_camera_orbit(f.state, *camera, true));
    f.state.viewport.selected_object = *camera;
    f.describe();
    REQUIRE(f.dispatch("position", {}, editor::Phase::begin));
    REQUIRE(f.dispatch("position", {{"position", Vec3{3, 1, 12}}}, editor::Phase::commit));
    const auto eye = f.evaluated().transform.position;
    CHECK(eye.x == Catch::Approx(3)); CHECK(eye.y == Catch::Approx(1)); CHECK(eye.z == Catch::Approx(12));
    const auto stored = find_instance(f.state, *camera)->transform.position; // Its focus point.
    CHECK(stored.x == Catch::Approx(3)); CHECK(stored.y == Catch::Approx(1)); CHECK(stored.z == Catch::Approx(2));
}

TEST_CASE("Native position gesture cancellation restores its exact pre-gesture animation track", "[editor][effects]") {
    Fixture f;
    REQUIRE(key_property(f.state, {1, "position"}, 6, Vec3{4, 0, 0}));
    f.describe();
    const auto before = f.state.document.timeline;
    const auto base = find_instance(f.state, 1)->transform;
    REQUIRE(f.dispatch("position", {}, editor::Phase::begin));
    REQUIRE(f.dispatch("position", {{"position", Vec3{3, 2, 1}}}, editor::Phase::update));
    CHECK(f.state.document.timeline != before);
    REQUIRE(f.dispatch("position", {}, editor::Phase::cancel));
    CHECK(f.state.document.timeline == before);
    CHECK(find_instance(f.state, 1)->transform == base);
}
