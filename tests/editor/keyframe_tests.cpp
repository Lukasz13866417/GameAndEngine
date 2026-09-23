#include "../../examples/editor/keyframes.hpp"
#include "../../examples/editor/editing_session.hpp"
#include "../../examples/editor/edits.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <algorithm>
#include <limits>
#include <chrono>
#include <iostream>

namespace {
using namespace vng;
using namespace editor_example;
State scene() {
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
std::string serialized(const State& state) {
    auto data = encode(state);
    REQUIRE(data);
    return *data;
}
} // namespace

TEST_CASE("Range edits preserve other fields components interpolation and timestamp names", "[editor][keyframe][range]") {
    auto state = scene();
    for (auto time : {0.F, 2.F, 4.F, 6.F}) {
        REQUIRE(key_property(state, {1, "position"}, time, Vec3{time, time + 10, time + 20}));
        REQUIRE(key_property(state, {1, "scale"}, time, time * .25F + 1));
        state.document.keyframe_names[time] = "Key " + std::to_string(time);
    }
    const auto before = state;
    KeyframeValue old{{1, "position"}, Vec3{2, 12, 22}, timeline::Interpolation::linear, true};
    auto next = old; next.value = Vec3{99, 12, 22};
    auto change = keyframe_change(old, next); REQUIRE(change);
    CHECK(change->components == std::array{true, false, false});
    CHECK_FALSE(change->incoming);
    const std::array changes{*change};
    REQUIRE(apply_keyframe_range(state, 2, 4, changes));
    CHECK(state.document.timeline.sample({1, "position"}, 0) == before.document.timeline.sample({1, "position"}, 0));
    CHECK(std::get<Vec3>(*state.document.timeline.sample({1, "position"}, 2)) == Vec3{99, 12, 22});
    CHECK(std::get<Vec3>(*state.document.timeline.sample({1, "position"}, 4)) == Vec3{99, 14, 24});
    CHECK(state.document.timeline.sample({1, "position"}, 6) == before.document.timeline.sample({1, "position"}, 6));
    CHECK(*state.document.timeline.find({1, "scale"}) == *before.document.timeline.find({1, "scale"}));
    CHECK(state.document.keyframe_names == before.document.keyframe_names);
    CHECK(state.document.mesh.document() == before.document.mesh.document());
}

TEST_CASE("Range Blend and Key changes sample each destination before any edits", "[editor][keyframe][range]") {
    auto state = scene();
    REQUIRE(key_property(state, {1, "position"}, 0, Vec3{0, 0, 0}));
    REQUIRE(key_property(state, {1, "position"}, 6, Vec3{6, 12, 18}));
    state.document.keyframe_names[2] = "Sparse A";
    state.document.keyframe_names[4] = "Sparse B";
    const auto before = state;
    SECTION("Only interpolation changes, not values") {
        const std::array changes{KeyframeChange{.target = {1, "position"}, .incoming = timeline::Interpolation::hold}};
        REQUIRE(apply_keyframe_range(state, 2, 6, changes));
        for (auto time : {2.F, 4.F, 6.F}) {
            CHECK(state.document.timeline.sample({1, "position"}, time) == before.document.timeline.sample({1, "position"}, time));
            const auto* track = state.document.timeline.find({1, "position"}); REQUIRE(track);
            CHECK(std::ranges::find(track->keys, time, &timeline::Keyframe::time)->incoming == timeline::Interpolation::hold);
        }
    }
    SECTION("An axis edit preserves original interpolated axes in later destinations") {
        const std::array changes{KeyframeChange{.target = {1, "position"}, .value = Vec3{50, 0, 0}, .components = {true, false, false}}};
        REQUIRE(apply_keyframe_range(state, 2, 4, changes));
        CHECK(std::get<Vec3>(*state.document.timeline.sample({1, "position"}, 2)) == Vec3{50, 4, 6});
        CHECK(std::get<Vec3>(*state.document.timeline.sample({1, "position"}, 4)) == Vec3{50, 8, 12});
    }
    SECTION("Keying alone preserves destination poses") {
        const std::array changes{KeyframeChange{.target = {1, "position"}, .keyed = true}};
        REQUIRE(apply_keyframe_range(state, 2, 4, changes));
        for (auto time : {2.F, 4.F}) CHECK(state.document.timeline.sample({1, "position"}, time) == before.document.timeline.sample({1, "position"}, time));
    }
    SECTION("Unkeying a missing track does not create a baseline") {
        const std::array changes{KeyframeChange{.target = {1, "scale"}, .keyed = false}};
        REQUIRE(apply_keyframe_range(state, 2, 4, changes));
        CHECK_FALSE(state.document.timeline.find({1, "scale"}));
    }
}

TEST_CASE("Range edit validation is atomic including the permanent initial pose", "[editor][keyframe][range]") {
    auto state = scene();
    REQUIRE(add_keyframe(state, 2)); REQUIRE(add_keyframe(state, 4));
    const auto before = serialized(state);
    const KeyframeChange scale{.target = {1, "scale"}, .value = 2.F};
    SECTION("Invalid property after valid edit") {
        const std::array changes{scale, KeyframeChange{.target = {1, "missing"}, .value = 3.F}};
        CHECK_FALSE(apply_keyframe_range(state, 0, 4, changes));
    }
    SECTION("Zero cannot be unkeyed") {
        const std::array changes{scale, KeyframeChange{.target = {1, "position"}, .keyed = false}};
        CHECK_FALSE(apply_keyframe_range(state, 0, 4, changes));
    }
    SECTION("Value validation") {
        const std::array changes{KeyframeChange{.target = {1, "scale"}, .value = -1.F}};
        CHECK_FALSE(apply_keyframe_range(state, 2, 4, changes));
    }
    SECTION("Invalid and empty ranges") {
        const std::array changes{scale};
        for (const auto& range : {std::pair{4.F, 2.F}, std::pair{-1.F, 2.F}, std::pair{1.F, 1.5F}, std::pair{0.F, std::numeric_limits<f32>::infinity()}})
            CHECK_FALSE(apply_keyframe_range(state, range.first, range.second, changes));
        CHECK_FALSE(apply_keyframe_range(state, 0, 4, {}));
    }
    CHECK(serialized(state) == before);
}

TEST_CASE("Range edits are one session transaction with undo and redo", "[editor][keyframe][range][session]") {
    auto initial = scene();
    REQUIRE(add_keyframe(initial, 2)); REQUIRE(add_keyframe(initial, 4));
    EditingSession session{initial};
    const std::array changes{KeyframeChange{.target = {1, "scale"}, .value = 3.F}};
    CHECK_FALSE(session.apply_keyframe_range(0, 4, changes)); // Explicit selection required.
    session.select_keyframe(0);
    REQUIRE(session.apply_keyframe_range(0, 4, changes));
    const auto after = session.state();
    for (auto time : {0.F, 2.F, 4.F}) CHECK(std::get<f32>(*after.document.timeline.sample({1, "scale"}, time)) == 3.F);
    REQUIRE(session.undo());
    CHECK_FALSE(session.can_undo());
    CHECK(session.state().document.timeline.tracks().size() == initial.document.timeline.tracks().size());
    CHECK(std::ranges::equal(session.state().document.timeline.tracks(), initial.document.timeline.tracks()));
    REQUIRE(session.redo());
    CHECK(std::ranges::equal(session.state().document.timeline.tracks(), after.document.timeline.tracks()));
}

TEST_CASE("The initial pose is permanent without migrating sparse animation tracks",
          "[editor][keyframe][initial]") {
    auto state = scene();
    REQUIRE(state.document.timeline.set({1, "scale"}, {4, 2.F, timeline::Interpolation::linear}));
    const auto original = serialized(state);
    CHECK(keyframe_times(state) == std::vector<f32>{0, 4});
    CHECK(is_keyframe(state, 0));
    REQUIRE(add_keyframe(state, 0));
    CHECK(serialized(state) == original);
    CHECK_FALSE(erase_keyframe(state, 0));
    CHECK_FALSE(move_keyframe(state, 0, 1));
    auto fields = keyframe_values(state, 0);
    CHECK(std::ranges::all_of(fields, &KeyframeValue::keyed));
    auto scale = *std::ranges::find(fields, timeline::Target{1, "scale"}, &KeyframeValue::target);
    scale.keyed = false;
    CHECK_FALSE(edit_keyframe(state, 0, std::span{&scale, 1}));
    CHECK(serialized(state) == original);
    scale.keyed = true;
    scale.value = .75F;
    REQUIRE(edit_keyframe(state, 0, std::span{&scale, 1}));
    CHECK(evaluate_scene(state, 3.99F).model_transform.scale == .75F);
    CHECK(evaluate_scene(state, 4).model_transform.scale == 2.F);
    REQUIRE(state.document.timeline.tracks().size() == 1);
    CHECK(state.document.timeline.tracks()[0].keys.back().incoming == timeline::Interpolation::hold);
    EditingSession editing{state}; editing.select_keyframe(editing.state().viewport.time);
    REQUIRE_FALSE(*editing.erase_keyframes(std::array<f32, 1>{0}));
    CHECK_FALSE(editing.can_undo());
    CHECK_FALSE(editing.dirty());
}

TEST_CASE(
    "New scene keyframes capture the interpolated playhead without changing the animation",
    "[editor][keyframe]") {
    auto state = scene();
    const auto original = state;
    REQUIRE(key_property(state, {1, "position"}, 6, Vec3{3.7F, 2, 0}));
    REQUIRE(key_property(state, {2, "radius"}, 8, 2.F, timeline::Interpolation::hold));
    const auto previous = evaluate_scene(state, 3);
    const auto previous_camera = evaluate_camera(state, 3);
    std::vector<SceneValues> samples;
    for (f32 time=0; time<=10; time+=.5F) samples.push_back(evaluate_scene(state,time));
    REQUIRE(add_keyframe(state, 3));
    CHECK(keyframe_times(state) == std::vector<f32>{0, 3, 6, 8});
    CHECK(state.document.keyframe_names.contains(3));
    const auto values = keyframe_values(state, 3);
    REQUIRE(values.size() == 16);
    CHECK(std::ranges::all_of(values, [](const auto& field) { return field.keyed; }));
    CHECK(std::get<Vec3>(values[0].value) == previous.model_transform.position);
    const auto visible = std::ranges::find_if(values, [](const auto& field) {
        return field.target.object == 1 && field.target.property == "visible";
    });
    REQUIRE(visible != values.end());
    CHECK(visible->incoming == timeline::Interpolation::hold);
    CHECK(evaluate_scene(state, 3) == previous);
    CHECK(evaluate_camera(state, 3) == previous_camera);
    std::size_t index{};
    for (f32 time=0; time<=10; time+=.5F, ++index) {
        const auto value = evaluate_scene(state,time);
        CHECK(value.model_transform.position.x == Catch::Approx(samples[index].model_transform.position.x));
        CHECK(value.model_transform.position.y == Catch::Approx(samples[index].model_transform.position.y));
        CHECK(value.sun == samples[index].sun);
    }
    const auto radius = state.document.timeline.find({2,"radius"});
    REQUIRE(radius);
    CHECK(std::ranges::find(radius->keys,3.F,&timeline::Keyframe::time)->incoming == timeline::Interpolation::hold);
    const auto encoded = serialized(state);
    REQUIRE(add_keyframe(state, 3));
    CHECK(serialized(state) == encoded);
    CHECK((*editor_example::mesh_settings(state, 1)) == (*editor_example::mesh_settings(original, 1)));
    CHECK((*editor_example::sun_settings(state, 2)) == (*editor_example::sun_settings(original, 2)));
    CHECK(state.document.mesh.document() == original.document.mesh.document());
    CHECK(state.document.revision == original.document.revision);
}

TEST_CASE("Keyframe snapshots include sparse tracks camera and authored initial state",
          "[editor][keyframe]") {
    auto state = scene();
    const auto camera_id = ensure_camera(state, {0, 0, 8, {}});
    REQUIRE(camera_id);
    const auto initial = evaluate_scene(state, 0);
    // Imported sparse tracks can have no key at zero. Inserting before their
    // first timestamp uses the same authored fallback as normal evaluation.
    REQUIRE(state.document.timeline.set({1,"position"}, {4, Vec3{8,2,1}}));
    REQUIRE(add_keyframe(state, 2));
    CHECK(evaluate_scene(state, 2) == initial);
    REQUIRE(key_camera(state, *camera_id, 4, {.yaw=30, .pitch=20, .distance=8, .target={1,2,3}}));
    REQUIRE(key_property(state, {2,"radius"}, 8, 3.F));
    state.document.keyframe_names[5] = "Sparse named pose";
    const auto preceding = evaluate_scene(state, 6);
    const auto camera = evaluate_camera(state, 6);
    REQUIRE(add_keyframe(state, 6));
    CHECK(evaluate_scene(state, 6) == preceding);
    CHECK(evaluate_camera(state, 6) == camera);
    const auto after_last = evaluate_scene(state, 9);
    const auto last_camera = evaluate_camera(state, 9);
    REQUIRE(add_keyframe(state, 9));
    CHECK(evaluate_scene(state, 9) == after_last);
    CHECK(evaluate_camera(state, 9) == last_camera);
    const auto before = state.document.timeline;
    const std::array<PropertyKey,2> invalid_batch{{
        {{1,"position"},Vec3{99,0,0},timeline::Interpolation::linear,true},
        {{2,"radius"},-1.F,timeline::Interpolation::linear,true}}};
    CHECK_FALSE(edit_property_keys(state, 6, invalid_batch));
    CHECK(state.document.timeline == before);
}

TEST_CASE("Fleet key insertion and camera saves batch timeline work", "[editor][keyframe][performance]") {
    auto state = scene();
    const auto prototype = state.document.instances.front();
    for (u32 id=3; id<=105; ++id) {
        auto instance = prototype;
        instance.id = id;
        instance.name = "Ship " + std::to_string(id);
        state.document.instances.push_back(std::move(instance));
    }
    state.document.next_instance_id = 106;
    const auto camera = ensure_camera(state, {0, 0, 8, {}});
    REQUIRE(camera);
    const auto properties = animation_properties(state);
    for (const auto& property : properties)
        for (f32 time : {0.F,2.F,4.F,6.F,8.F,10.F})
            REQUIRE(state.document.timeline.set(property.target, {time,property.base_value}));
    EditingSession editing{std::move(state)}; editing.select_keyframe(editing.state().viewport.time);
    REQUIRE(editing.timeline_track_limit(1024));
    const auto original = editing.state().document.timeline;
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(editing.add_keyframe(3));
    const auto inserted = std::chrono::steady_clock::now();
    REQUIRE(editing.undo());
    CHECK(editing.state().document.timeline == original);
    editing.select_keyframe(editing.state().viewport.time);
    auto pose = *evaluate_camera(editing.state(), editing.state().viewport.time);
    constexpr int saves = 32; // Each save is one history entry; stay inside the undo depth.
    const auto camera_start = std::chrono::steady_clock::now();
    for (int i=0; i<saves; ++i) {
        pose.yaw += 1.F;
        REQUIRE(editing.set_camera(*camera, pose));
    }
    const auto camera_end = std::chrono::steady_clock::now();
    for (int i=0; i<saves; ++i) REQUIRE(editing.undo());
    CHECK(editing.state().document.timeline == original);
    const auto insert_ms = std::chrono::duration<double,std::milli>(inserted-start).count();
    const auto camera_ms = std::chrono::duration<double,std::milli>(camera_end-camera_start).count()/saves;
    std::cout << "Fleet (" << properties.size() << " properties): insert " << insert_ms
              << " ms, camera update " << camera_ms << " ms\n";
    CHECK(insert_ms < 1000); // Generous CI ceilings: catch repeated whole-timeline work.
    CHECK(camera_ms < 50);
}

TEST_CASE("Scene keyframe edits rename retime and remove values as an atomic group",
          "[editor][keyframe]") {
    auto state = scene();
    REQUIRE(add_keyframe(state, 2));
    REQUIRE(add_keyframe(state, 5));
    auto values = keyframe_values(state, 2);
    values[0].value = Vec3{9, 8, 7};
    values[0].incoming = timeline::Interpolation::hold;
    values[1].keyed = false;
    const auto original = serialized(state);
    CHECK_FALSE(update_keyframe(state, 2, 5, "Collision", values));
    CHECK(serialized(state) == original);
    CHECK_FALSE(update_keyframe(state, 2, 4, std::string(257, 'x'), values));
    CHECK_FALSE(update_keyframe(state, 2, 4, "Line\nbreak", values));
    CHECK_FALSE(update_keyframe(state, 2, 4, std::string("\xc0\xaf", 2), values));
    CHECK_FALSE(update_keyframe(state, 2, 4, std::string("\xed\xa0\x80", 3), values));
    CHECK(serialized(state) == original);
    REQUIRE(update_keyframe(state, 2, 4, "Approach / α", values));
    CHECK(keyframe_times(state) == std::vector<f32>{0, 4, 5});
    CHECK(keyframe_name(state, 4) == "Approach / α");
    CHECK_FALSE(state.document.keyframe_names.contains(2));
    const auto keys = keyframe_values(state, 4);
    CHECK(std::get<Vec3>(keys[0].value) == Vec3{9, 8, 7});
    CHECK(keys[0].incoming == timeline::Interpolation::hold);
    CHECK_FALSE(keys[1].keyed);
    REQUIRE(erase_keyframe(state, 4));
    CHECK(keyframe_times(state) == std::vector<f32>{0, 5});
    CHECK_FALSE(state.document.keyframe_names.contains(4));
}

TEST_CASE("Empty named keyframes remain selectable and survive persistence and undo",
          "[editor][keyframe]") {
    auto initial = scene();
    REQUIRE(add_keyframe(initial, 3));
    EditingSession editing{std::move(initial)}; editing.select_keyframe(editing.state().viewport.time);
    const auto& state = editing.state();
    const auto original = state;
    auto values = keyframe_values(state, 3);
    for (auto& field : values)
        field.keyed = false;
    REQUIRE(editing.update_keyframe(3, 4, "A \"quiet\" moment", values));
    CHECK(keyframe_times(state) == std::vector<f32>{0, 4});
    CHECK(std::ranges::none_of(keyframe_values(state, 4), [](const auto& f) { return f.keyed; }));
    const auto encoded = serialized(state);
    auto loaded = decode(encoded);
    REQUIRE(loaded);
    CHECK(loaded->document.timeline == state.document.timeline);
    CHECK(loaded->document.keyframe_names == state.document.keyframe_names);
    CHECK(serialized(*loaded) == encoded);
    REQUIRE(editing.undo());
    CHECK(state.document.timeline == original.document.timeline);
    CHECK(state.document.keyframe_names == original.document.keyframe_names);
    REQUIRE(editing.redo());
    CHECK(serialized(state).find("quiet") != std::string::npos);
    CHECK(state.document.keyframe_names == loaded->document.keyframe_names);
    REQUIRE(editing.erase_keyframes(std::array<f32,2>{4,0}));
    CHECK(keyframe_times(state) == std::vector<f32>{0});
    for (const auto& track : state.document.timeline.tracks())
        CHECK(track.keys.size() == 1);
}

TEST_CASE("Keyframe validation rejects invalid targets times types and malformed metadata",
          "[editor][keyframe]") {
    auto state = scene();
    REQUIRE(add_keyframe(state, 3));
    const auto original = serialized(state);
    for (auto time : {-1.F, 11.F, std::numeric_limits<f32>::infinity(),
                      std::numeric_limits<f32>::quiet_NaN()}) {
        CHECK_FALSE(add_keyframe(state, time));
        CHECK_FALSE(move_keyframe(state, 3, time));
        CHECK_FALSE(erase_keyframe(state, time));
    }
    auto fields = keyframe_values(state, 3);
    fields[0].value = true;
    CHECK_FALSE(edit_keyframe(state, 3, fields));
    fields = keyframe_values(state, 3);
    fields[0].target.object = 99;
    CHECK_FALSE(edit_keyframe(state, 3, fields));
    fields = keyframe_values(state, 3);
    fields.push_back(fields.front());
    CHECK_FALSE(edit_keyframe(state, 3, fields));
    CHECK(serialized(state) == original);
    auto invalid = state;
    invalid.document.keyframe_names[11] = "Outside";
    CHECK_FALSE(encode(invalid));
    invalid = state;
    invalid.document.timeline_duration = 2;
    CHECK_FALSE(encode(invalid));
    auto document = original;
    const auto start = document.find("keyframes = [");
    REQUIRE(start != std::string::npos);
    const auto end = document.find("];", start);
    REQUIRE(end != std::string::npos);
    document.replace(start, end + 2 - start,
                     "keyframes = [{time=2;name=\"A\";},{time=2;name=\"B\";}];");
    CHECK_FALSE(decode(document));
    document = original;
    document.erase(start, end + 2 - start);
    auto legacy = decode(document);
    REQUIRE(legacy);
    CHECK(legacy->document.keyframe_names.empty());
    CHECK(keyframe_times(*legacy) == std::vector<f32>{0, 3});
}
