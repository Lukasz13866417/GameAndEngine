#include "../../examples/editor/animation.hpp"
#include "../../examples/editor/scale_limits.hpp"
#include "../../examples/editor/scene_samples.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>

namespace {
using namespace vng;
using namespace editor_example;
using timeline::Interpolation;
State scene() {
    content::vmesh::Document document;
    document.vertex_count = 3;
    document.vertex_fields = {{"position",
                               {content::vmesh::ScalarType::Float32, 3},
                               std::vector<f32>{-1, -1, 0, 1, -1, 0, 0, 1, 0}}};
    document.faces = {{0, 1, 2}};
    auto mesh = editor::EditableMesh::create(std::move(document));
    REQUIRE(mesh);
    return {.document = {.mesh = std::move(*mesh)}};
}
std::string serialized(const State& state) {
    auto encoded = encode(state);
    INFO((encoded ? "Encoded scene" : encoded.error().message));
    REQUIRE(encoded);
    return *encoded;
}
std::string legacy_scene(const State& state) {
    auto source = serialized(state);
    const auto timeline = source.find("timeline = {");
    REQUIRE(timeline != std::string::npos);
    source.erase(timeline);
    return source;
}
std::string track(std::string_view object, std::string_view property, std::string_view keys) {
    return "{ object = " + std::string(object) + "; property = \"" + std::string(property) +
           "\"; label = \"Track\"; layer = \"Layer\"; keys = [" + std::string(keys) + "]; }";
}
std::string animated(std::string_view tracks, std::string_view duration = "10") {
    return legacy_scene(scene()) + "timeline = { duration = " + std::string(duration) +
           "; tracks = [" + std::string(tracks) + "]; };\n";
}
} // namespace

TEST_CASE("Animation property bridge enumerates stable typed properties for both entities",
          "[editor][animation]") {
    auto state = scene();
    const auto properties = animation_properties(state);
    REQUIRE(properties.size() == 16);
    const std::vector<std::string> names{"position", "rotation", "scale", "axis_scale", "brightness", "visible", "wireframe",
                                         "position", "rotation", "scale", "axis_scale", "radius", "displacement", "bloom", "white_spots", "visible"};
    for (std::size_t i = 0; i < properties.size(); ++i) {
        CHECK(properties[i].target.property == names[i]);
        CHECK(properties[i].target.object == (i < 7 ? 1 : 2));
        CHECK(properties[i].layer == (i < 7 ? "Mesh" : "Sun"));
        CHECK_FALSE(properties[i].label.empty());
        REQUIRE(key_property(state, properties[i].target, 2, properties[i].base_value));
    }
    CHECK(state.document.timeline.tracks().size() == 16);
    CHECK(validate_animation(state));
    CHECK(evaluate_scene(state, 1) == evaluate_scene(state, -1));
}

TEST_CASE("Scene camera keys interpolate shots and hold cuts without touching the private view",
          "[editor][animation][camera]") {
    auto state = scene();
    const auto inspection = state.viewport.editor_camera;
    const auto* geometry = std::get<std::vector<f32>>(
        state.document.mesh.document().vertex_fields[0].values).data();
    const CameraPose opening{0, 10, 12, {1, 2, 3}, 1}, follow{30, 20, 8, {3, 4, 5}, 3}, cut{-40, 5, 9, {8, 9, 10}, 1};
    const auto id = ensure_camera(state, opening);
    REQUIRE(id);
    REQUIRE(key_camera(state, *id, 0, opening));
    REQUIRE(key_camera(state, *id, 4, follow));
    const auto middle = evaluate_camera(state, 2);
    REQUIRE(middle);
    CHECK(middle->yaw == Catch::Approx(15));
    CHECK(middle->pitch == Catch::Approx(15));
    CHECK(middle->distance == Catch::Approx(10));
    CHECK(middle->zoom == Catch::Approx(2));
    const auto* camera_instance = find_instance(state, *id);
    const auto eye = [&](f32 time) { return evaluate_transform(state, *camera_instance, time).position; };
    for (unsigned c = 0; c < 3; ++c) CHECK(eye(2)[c] == Catch::Approx((eye(0)[c] + eye(4)[c]) * .5F));
    REQUIRE(key_camera(state, *id, 6, cut, Interpolation::hold));
    CHECK(evaluate_camera(state, 5.99F)->yaw == Catch::Approx(follow.yaw));
    CHECK(evaluate_camera(state, 6)->yaw == Catch::Approx(cut.yaw));
    for (unsigned c = 0; c < 3; ++c) CHECK(evaluate_camera(state, 6)->target[c] == Catch::Approx(cut.target[c]).margin(1e-4));
    // The editor preview always looks through the private view, never the scene camera.
    state.viewport.time = 2;
    const auto preview = camera(state).snapshot({800, 600});
    const auto expected = camera(inspection, ViewMode::scene).snapshot({800, 600});
    REQUIRE(preview); REQUIRE(expected);
    CHECK(preview->position == expected->position);
    CHECK(state.viewport.editor_camera == inspection);
    CHECK(state.document.revision == 1);
    CHECK(std::get<std::vector<f32>>(state.document.mesh.document().vertex_fields[0].values).data() == geometry);
    const auto decoded = decode(serialized(state));
    REQUIRE(decoded);
    CHECK(evaluate_camera(*decoded, 2) == middle);
}

TEST_CASE("Editing an animated camera is atomic and preserves an authored camera cut",
          "[editor][animation][camera]") {
    auto state = scene();
    const auto id = ensure_camera(state, {0, 0, 8, {}});
    REQUIRE(id);
    REQUIRE(key_camera(state, *id, 0, {0, 0, 8, {}}));
    REQUIRE(key_camera(state, *id, 5, {70, 12, 10, {1, 2, 3}}, Interpolation::hold));
    // Re-keying the cut with the default interpolation keeps it a cut.
    REQUIRE(key_camera(state, *id, 5, {80, 12, 10, {1, 2, 3}}));
    CHECK(evaluate_camera(state, 4.99F)->yaw == Catch::Approx(0));
    CHECK(evaluate_camera(state, 5)->yaw == Catch::Approx(80));
    const auto before = state.document.timeline;
    CHECK_FALSE(key_camera(state, *id, 7, {25, 12, camera_min_distance * .5F, {}}));
    CHECK(state.document.timeline == before);
    CHECK_FALSE(key_property(state, {*id, "focus"}, 1, Vec3{}));
    CHECK_FALSE(key_property(state, {*id, "zoom"}, 1, camera_max_zoom * 2));
    CHECK_FALSE(key_property(state, {*id, "position"}, 1, Vec3{camera_target_limit * 4, 0, 0}));
    CHECK(state.document.timeline == before);
}

TEST_CASE("First property keys seed baseline zero while preserving authored mesh and scene values",
          "[editor][animation]") {
    auto state = scene();
    const auto original = state;
    const auto* vertices =
        std::get<std::vector<f32>>(state.document.mesh.document().vertex_fields[0].values).data();
    REQUIRE(key_property(state, {1, "position"}, 5, Vec3{3.7F, 2, 0}));
    const auto* position = state.document.timeline.find({1, "position"});
    REQUIRE(position);
    REQUIRE(position->keys.size() == 2);
    CHECK(position->keys[0].time == 0.F);
    CHECK(std::get<Vec3>(position->keys[0].value) == find_instance(original, 1)->transform.position);
    CHECK(evaluate_scene(state, 0).model_transform.position == find_instance(original, 1)->transform.position);
    const auto halfway = evaluate_scene(state, 2.5F).model_transform.position;
    CHECK(halfway.x == Catch::Approx(2.7F));
    CHECK(halfway.y == Catch::Approx(1));
    CHECK(evaluate_scene(state, 5).model_transform.position == Vec3{3.7F, 2, 0});
    CHECK(evaluate_scene(state, 20).model_transform.position == Vec3{3.7F, 2, 0});
    CHECK((*editor_example::mesh_settings(state, 1)) == (*editor_example::mesh_settings(original, 1)));
    CHECK((*editor_example::sun_settings(state, 2)) == (*editor_example::sun_settings(original, 2)));
    CHECK(state.document.mesh.document() == original.document.mesh.document());
    CHECK(std::get<std::vector<f32>>(state.document.mesh.document().vertex_fields[0].values).data() ==
          vertices);
    CHECK(state.viewport.time == original.viewport.time);
    CHECK(state.document.revision == original.document.revision);
    REQUIRE(key_property(state, {1, "position"}, 5, Vec3{4, 4, 4}));
    REQUIRE(state.document.timeline.find({1, "position"})->keys.size() == 2);
    CHECK(evaluate_scene(state, 0).model_transform.position == find_instance(original, 1)->transform.position);
    CHECK(evaluate_scene(state, 5).model_transform.position == Vec3{4, 4, 4});
}

TEST_CASE("Animation evaluation supports every current setting and held boolean keys",
          "[editor][animation]") {
    auto state = scene();
    const auto base = evaluate_scene(state, -1);
    REQUIRE(key_property(state, {1, "position"}, 0, Vec3{2, 3, 4}));
    REQUIRE(key_property(state, {1, "scale"}, 0, 2.F));
    REQUIRE(key_property(state, {1, "rotation"}, 0, Vec3{0, 0, 90}));
    REQUIRE(key_property(state, {1, "brightness"}, 0, 3.F));
    REQUIRE(key_property(state, {1, "visible"}, 0, false));
    REQUIRE(key_property(state, {1, "wireframe"}, 0, true));
    REQUIRE(key_property(state, {2, "position"}, 0, Vec3{-3, 2, 1}));
    REQUIRE(key_property(state, {2, "rotation"}, 0, Vec3{90, 0, 0}));
    REQUIRE(key_property(state, {2, "scale"}, 0, .5F));
    REQUIRE(key_property(state, {2, "radius"}, 0, 2.F));
    REQUIRE(key_property(state, {2, "displacement"}, 0, .5F));
    REQUIRE(key_property(state, {2, "bloom"}, 0, .7F));
    REQUIRE(key_property(state, {2, "white_spots"}, 0, true));
    REQUIRE(key_property(state, {2, "visible"}, 0, false));
    const auto result = evaluate_scene(state, 4);
    CHECK(result.model == MeshSettings{3, false, true});
    CHECK(result.model_transform == InstanceTransform{{2, 3, 4}, {0, 0, 90}, 2});
    CHECK(result.sun == SunSettings{2, .5F, .7F, true, false});
    CHECK(result.sun_transform == InstanceTransform{{-3, 2, 1}, {90, 0, 0}, .5F});
    CHECK(evaluate_scene(state, -1) == base);
    REQUIRE(key_property(state, {1, "visible"}, 5, true, Interpolation::linear));
    CHECK(state.document.timeline.find({1, "visible"})->keys.back().incoming == Interpolation::hold);
    CHECK_FALSE(evaluate_scene(state, 4.999F).model.visible);
    CHECK(evaluate_scene(state, 5).model.visible);
    CHECK(evaluate_scene(state, -1) == base);
    CHECK(evaluate_scene(state, std::numeric_limits<f32>::quiet_NaN()) == base);
}

TEST_CASE("Authored fallback and incoming interpolation retain timeline core semantics",
          "[editor][animation]") {
    auto state = scene();
    REQUIRE(
        state.document.timeline.set({1, "brightness"}, {3, 2.F, Interpolation::hold}, "Glow", "Lighting"));
    CHECK(evaluate_scene(state, 2).model.brightness == (*editor_example::mesh_settings(state, 1)).brightness);
    REQUIRE(key_property(state, {1, "brightness"}, 6, 4.F, Interpolation::hold));
    CHECK(evaluate_scene(state, 5.999F).model.brightness == 2.F);
    CHECK(evaluate_scene(state, 6).model.brightness == 4.F);
    REQUIRE(key_property(state, {1, "brightness"}, 8, 2.F, Interpolation::linear));
    CHECK(evaluate_scene(state, 7).model.brightness == 3.F);
    CHECK(state.document.timeline.find({1, "brightness"})->label == "Glow");
    CHECK(state.document.timeline.find({1, "brightness"})->layer == "Lighting");
}

TEST_CASE("Invalid property keys fail transactionally with editor-specific ranges and time limits",
          "[editor][animation]") {
    auto state = scene();
    REQUIRE(key_property(state, {1, "scale"}, 4, 1.F));
    const auto original = state.document.timeline;
    for (const auto& target : {timeline::Target{0, "position"}, timeline::Target{3, "position"},
                               timeline::Target{1, "radius"}, timeline::Target{2, "camera"}})
        CHECK_FALSE(key_property(state, target, 1, 1.F));
    for (const auto time :
         {-1.F, 10.1F, std::numeric_limits<f32>::infinity(), std::numeric_limits<f32>::quiet_NaN()})
        CHECK_FALSE(key_property(state, {1, "scale"}, time, 1.F));
    for (const auto& value :
         {timeline::Value{0.F}, timeline::Value{max_instance_scale+1.F}, timeline::Value{i32{1}},
          timeline::Value{true}, timeline::Value{std::numeric_limits<f32>::quiet_NaN()}})
        CHECK_FALSE(key_property(state, {1, "scale"}, 1, value));
    CHECK_FALSE(key_property(state, {1, "position"}, 1, Vec3{scene_coordinate_limit + 1, 0, 0}));
    CHECK_FALSE(key_property(state, {1, "rotation"}, 1, Vec3{0, 361, 0}));
    CHECK_FALSE(key_property(state, {2, "scale"}, 1, .01F));
    CHECK_FALSE(key_property(state, {2, "bloom"}, 1, 1.1F));
    CHECK_FALSE(key_property(state, {2, "radius"}, 1, .01F));
    CHECK_FALSE(key_property(state, {2, "displacement"}, 1, 1.01F));
    CHECK_FALSE(key_property(state, {1, "visible"}, 1, false, static_cast<Interpolation>(17)));
    CHECK(state.document.timeline == original);
    state.document.timeline_duration = 3;
    CHECK_FALSE(validate_animation(state));
    CHECK_FALSE(key_property(state, {2, "bloom"}, 1, .5F));
    CHECK(state.document.timeline == original);
}

TEST_CASE("Timeline duration and bypassed editor constraints are validated before serialization",
          "[editor][animation]") {
    auto state = scene();
    for (const auto duration : {0.F, .099F, 86401.F, std::numeric_limits<f32>::infinity()}) {
        state.document.timeline_duration = duration;
        CHECK_FALSE(validate_animation(state));
        CHECK_FALSE(encode(state));
    }
    for (const auto duration : {.1F, 86400.F}) {
        state.document.timeline_duration = duration;
        CHECK(validate_animation(state));
        REQUIRE(key_property(state, {2, "bloom"}, duration, .5F));
    }
    state.document.timeline_duration = 86400;
    REQUIRE(state.document.timeline.set({1, "scale"}, {0, max_instance_scale+1.F, Interpolation::linear}));
    CHECK_FALSE(validate_animation(state));
    CHECK_FALSE(encode(state));
    REQUIRE(state.document.timeline.erase({1, "scale"}, 0));
    REQUIRE(state.document.timeline.set({9, "unknown"}, {0, false, Interpolation::hold}));
    CHECK_FALSE(validate_animation(state));
}

TEST_CASE("Readable timeline serialization round trips deterministically and retains old scenes",
          "[editor][animation]") {
    auto state = scene();
    state.document.timeline_duration = 12;
    REQUIRE(key_property(state, {2, "white_spots"}, 7, true));
    REQUIRE(key_property(state, {1, "position"}, 4, Vec3{4, -2, 1}, Interpolation::linear));
    REQUIRE(key_property(state, {1, "position"}, 9, Vec3{-3, 1, 4}, Interpolation::hold));
    const auto source = serialized(state);
    CHECK(source.find("timeline = {") != std::string::npos);
    CHECK(source.find("property = \"position\"") != std::string::npos);
    CHECK(source.find("incoming = \"linear\"") != std::string::npos);
    auto decoded = decode(source);
    INFO((decoded ? "Decoded timeline" : decoded.error().message));
    REQUIRE(decoded);
    CHECK(decoded->document.timeline == state.document.timeline);
    CHECK(decoded->document.timeline_duration == state.document.timeline_duration);
    CHECK((*editor_example::mesh_settings(*decoded, 1)) == (*editor_example::mesh_settings(state, 1)));
    CHECK((*editor_example::sun_settings(*decoded, 2)) == (*editor_example::sun_settings(state, 2)));
    CHECK(decoded->document.mesh.document() == state.document.mesh.document());
    CHECK(serialized(*decoded) == source);
    for (const auto time : {0.F, 2.F, 6.F, 8.F, 12.F})
        CHECK(evaluate_scene(*decoded, time) == evaluate_scene(state, time));
    auto legacy = scene();
    legacy.viewport.time = 123; // old editor playheads extend the default ruler
    legacy.document.timeline_duration = 123;
    auto old = decode(legacy_scene(legacy));
    REQUIRE(old);
    CHECK(old->document.timeline.tracks().empty());
    CHECK(old->document.timeline_duration == 123.F);
    CHECK(old->viewport.time == 123.F);
}

TEST_CASE(
    "Timeline scene reader rejects unknown targets malformed values and invalid interpolation",
    "[editor][animation]") {
    constexpr std::string_view key = "{ time = 2; value = 1; incoming = \"linear\"; }";
    for (const auto& bad_track :
         {track("0", "scale", key), track("3", "scale", key), track("1", "radius", key),
          track("1", "scale", "{ time = 2; value = true; incoming = \"linear\"; }"),
          track("1", "scale", "{ time = 2; value = 1000001; incoming = \"linear\"; }"),
          track("1", "scale", "{ time = 11; value = 1; incoming = \"linear\"; }"),
          track("1", "scale", "{ time = 2; value = 1; incoming = \"cubic\"; }"),
          track("1", "visible", "{ time = 2; value = false; incoming = \"linear\"; }"),
          track("1", "position", "{ time = 2; value = [0,0]; incoming = \"linear\"; }"),
          track("1", "position", "{ time = 2; value = [0,1000001,0]; incoming = \"hold\"; }"),
          track("2", "bloom", ""),
          track("1", "scale", std::string(key) + "," + std::string(key))}) {
        INFO(bad_track);
        CHECK_FALSE(decode(animated(bad_track)));
    }
    CHECK_FALSE(decode(animated(track("1", "scale", key) + "," + track("1", "scale", key))));
    CHECK_FALSE(decode(animated(track("1", "scale", key), "0")));
    CHECK_FALSE(decode(animated(track("1", "scale", key), "86401")));
    CHECK(decode(animated(track("1", "scale", key))));
}

TEST_CASE("CPU transforms and vertex projection use animated position scale and visibility",
          "[editor][animation]") {
    auto state = scene();
    state.viewport.selected_object = 1;
    find_instance(state, 1)->transform.position = {};
    find_instance(state, 1)->transform.scale = 1;
    state.viewport.editor_camera.yaw = state.viewport.editor_camera.pitch = 0;
    REQUIRE(key_property(state, {1, "position"}, 4, Vec3{2, 0, 0}));
    REQUIRE(key_property(state, {1, "scale"}, 4, 2.F));
    REQUIRE(key_property(state, {2, "position"}, 4, Vec3{-4, 1, 0}));
    state.viewport.time = 4;
    CHECK(mesh_transform(state)[3].x == 2.F);
    CHECK(mesh_transform(state)[0].x == 2.F);
    CHECK(mesh_transform(state, 0)[3].x == 0.F);
    CHECK(sun_position(state) == Vec3{-4, 1, 0});
    CHECK(sun_position(state, 0) == find_instance(state, 2)->transform.position);
    const auto animated_vertex = project_vertex(state, 0, {640, 480});
    REQUIRE(animated_vertex);
    CHECK(animated_vertex->x == Catch::Approx(.5F));
    REQUIRE(key_property(state, {1, "visible"}, 4, false));
    CHECK_FALSE(project_vertex(state, 0, {640, 480}));
    for (const auto& vertex : project_vertices(state, {640, 480}))
        CHECK_FALSE(vertex);
    CHECK((*editor_example::mesh_settings(state, 1)).visible);
    state.viewport.mode = ViewMode::mesh;
    CHECK(mesh_transform(state)[3].x == 0.F); // isolated mesh view remains centered
    state.viewport.mode = ViewMode::sun;
    CHECK(sun_position(state) == Vec3{});
}
TEST_CASE("Runtime scene samples reuse camera redraws and notice every data dependency", "[editor][animation][cache]") {
    using namespace editor_example;
    using namespace vng;
    auto scene = ::scene();
    SceneSamples cache;
    const auto count = scene.document.instances.size();
    auto samples = cache.sample(scene, 0);
    REQUIRE(samples.size() == count);
    CHECK(cache.evaluations() == count);
    ++scene.viewport.sequence;
    scene.viewport.editor_camera.yaw += 30;
    scene.viewport.selected_object = 2;
    scene.viewport.mode = ViewMode::sun;
    ++scene.document.revision; // Unrelated geometry/world edits need no resampling either.
    samples = cache.sample(scene, 0);
    CHECK(cache.evaluations() == count);
    scene.document.instances[0].transform.scale = 2; // Even direct authoring is observed.
    samples = cache.sample(scene, 0);
    CHECK(cache.evaluations() == count + 1);
    CHECK(samples[0].transform.scale == 2);
    REQUIRE(scene.document.timeline.set({scene.document.instances[0].id,"scale"},{0,3.F}));
    samples = cache.sample(scene, 0);
    CHECK(samples[0].transform.scale == 3);
    auto fork = scene;
    REQUIRE(fork.document.timeline.set({fork.document.instances[0].id,"scale"},{0,4.F}));
    scene.document.timeline = fork.document.timeline;
    samples = cache.sample(scene, 0);
    CHECK(samples[0].transform.scale == 4);
    REQUIRE(scene.document.timeline.set({scene.document.instances[0].id,"scale"},
        {2,6.F,timeline::Interpolation::linear}));
    samples = cache.sample(scene, 1);
    CHECK(samples[0].transform.scale == 5);
    const auto previous = cache.evaluations();
    (void)cache.sample(scene, 1); CHECK(cache.evaluations() == previous);
    std::swap(scene.document.instances[0],scene.document.instances[1]);
    samples = cache.sample(scene, 1);
    for(std::size_t i=0;i<samples.size();++i)CHECK(samples[i] == evaluate_instance(scene,scene.document.instances[i],1));
    scene.document.instances.erase(scene.document.instances.begin());
    samples = cache.sample(scene, 1); REQUIRE(samples.size() == 1);
    CHECK(samples[0].transform.scale == 5);
    scene.document.instances.clear(); CHECK(cache.sample(scene,1).empty());
}
