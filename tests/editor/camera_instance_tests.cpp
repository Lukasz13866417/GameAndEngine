#include "../../examples/editor/editing_session.hpp"
#include "../../examples/editor/animation.hpp"
#include "../../examples/editor/play_camera.hpp"
#include "../../examples/editor/document_patch.hpp"
#include "../../examples/editor/keyframes.hpp"
#include "../../examples/editor/position_edits.hpp"
#include "../../examples/editor/effects.hpp"
#include "../../examples/editor/annotation_geometry.hpp"
#include "../../examples/editor/blueprint_gizmos.hpp"
#include "../../examples/editor/camera_glyph.hpp"
#include "../../examples/editor/selection.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>

namespace {
using namespace vng;
using namespace editor_example;
State scene() {
    content::vmesh::Document d;
    d.vertex_count = 3;
    d.vertex_fields = {{"position", {content::vmesh::ScalarType::Float32, 3},
                        std::vector<f32>{0,0,0, 1,0,0, 0,1,0}}};
    d.faces = {{0,1,2}};
    auto mesh = editor::EditableMesh::create(std::move(d)); REQUIRE(mesh);
    State state{.document = {.mesh = std::move(*mesh)}};
    state.viewport.selected_object = 1;
    return state;
}
bool close(const CameraPose& a, const CameraPose& b, f32 tolerance = 1e-3F) {
    return std::abs(a.yaw - b.yaw) < tolerance && std::abs(a.pitch - b.pitch) < tolerance &&
           std::abs(a.distance - b.distance) < tolerance && std::abs(a.zoom - b.zoom) < tolerance &&
           std::abs(a.target.x - b.target.x) < tolerance && std::abs(a.target.y - b.target.y) < tolerance &&
           std::abs(a.target.z - b.target.z) < tolerance;
}
u32 add_camera(State& state, const CameraPose& pose) {
    state.viewport.editor_camera = pose;
    auto created = instantiate(state, BlueprintId::camera);
    REQUIRE(created);
    return *created;
}
} // namespace

TEST_CASE("Camera pose conversions round trip through an instance transform", "[editor][camera]") {
    for (const CameraPose pose : {CameraPose{15, 12, 8, {}, 1}, CameraPose{-140, -30, 250, {3, -2, 40}, 2.5F},
                                  CameraPose{179, 80, .5F, {-1, 1, -1}, .2F}}) {
        SceneInstance camera{7, BlueprintId::camera, "Camera", CameraSettings{}, {}};
        place_camera(camera, pose);
        // Looking up is positive instance pitch; the orbit pose measures the eye's elevation instead.
        CHECK(camera.transform.rotation.x == Catch::Approx(-pose.pitch));
        CHECK(camera.transform.rotation.y == Catch::Approx(pose.yaw));
        CHECK(std::get<CameraSettings>(camera.settings).focus == pose.distance);
        CHECK(close(camera_pose(camera), pose));
    }
    SceneInstance mesh{1, BlueprintId::mesh, "Mesh", MeshSettings{}, {}};
    CHECK(camera_pose(mesh) == CameraPose{}); // Not a camera: the default pose, never a crash.
}

TEST_CASE("Adding cameras places them at the editor view and only the first becomes active", "[editor][camera]") {
    auto state = scene();
    CHECK_FALSE(has_camera(state));
    CHECK(active_camera(state, 0) == nullptr);
    const CameraPose first_view{30, 10, 6, {1, 0, 0}, 1}, second_view{-60, 20, 12, {0, 2, 0}, 1.5F};
    const auto first = add_camera(state, first_view);
    const auto second = add_camera(state, second_view);
    REQUIRE(camera_settings(state, first)); REQUIRE(camera_settings(state, second));
    CHECK(camera_settings(state, first)->active);
    CHECK_FALSE(camera_settings(state, second)->active);
    CHECK(is_camera_instance(state, first)); CHECK_FALSE(is_camera_instance(state, 1));
    CHECK(find_instance(state, first)->name == "Camera " + std::to_string(first));
    CHECK(active_camera(state, 0)->id == first);
    CHECK(close(*evaluate_camera(state, 0), first_view));
    CHECK(close(camera_pose(*find_instance(state, second)), second_view));
    const auto catalog = blueprint_catalog(state);
    const auto entry = std::ranges::find(catalog, BlueprintId::camera, &Blueprint::id);
    REQUIRE(entry != catalog.end());
    CHECK(entry->kind == BlueprintKind::camera);
    CHECK(entry->name == "Camera");
    CHECK(view_instance(state, BlueprintKind::camera)->id == second); // The newest camera is selected.
    state.viewport.selected_object = 1;
    CHECK(view_instance(state, BlueprintKind::camera)->id == first); // Otherwise the first camera.
    // Camera settings are ordinary keyable properties.
    const auto properties = animation_properties(state);
    for (const auto property : {"zoom", "focus", "active", "visible"})
        CHECK(std::ranges::find(properties, timeline::Target{first, property}, &AnimationProperty::target) != properties.end());
    auto bytes = encode_scene(state); REQUIRE(bytes);
    CHECK(bytes->find("editor_project = 5;") != std::string::npos);
    auto restored = decode(*bytes); REQUIRE(restored);
    CHECK(restored->document.instances == state.document.instances);
    CHECK(close(*evaluate_camera(*restored, 0), first_view));
}

TEST_CASE("The scene camera follows the active camera's keys and is absent without cameras", "[editor][camera]") {
    auto state = scene();
    const CameraPose start{0, 0, 10, {}, 1}, end{90, 0, 10, {}, 1};
    CHECK_FALSE(has_camera(state));
    CHECK_FALSE(evaluate_camera(state, 2)); // Without a camera the simulation has no view of its own.
    auto camera = ensure_camera(state, start, "Shot"); REQUIRE(camera);
    CHECK(find_instance(state, *camera)->name == "Shot");
    auto again = ensure_camera(state, end); REQUIRE(again);
    CHECK(*again == *camera); // An existing camera is kept, not moved.
    CHECK(close(*evaluate_camera(state, 2), start));
    REQUIRE(key_camera(state, *camera, 0, start));
    REQUIRE(key_camera(state, *camera, 4, end));
    const auto middle = evaluate_camera(state, 2);
    REQUIRE(middle);
    CHECK(middle->distance == Catch::Approx(10));
    // Interpolating the eye position moves along the chord, so the pivot distance is kept while the eye passes closer.
    const auto expected_eye = evaluate_transform(state, *find_instance(state, *camera), 2).position;
    CHECK(expected_eye.x == Catch::Approx(5)); CHECK(expected_eye.z == Catch::Approx(5));
    CHECK(middle->yaw == Catch::Approx(45));
    CHECK_FALSE(key_camera(state, 1, 2, end)); // Not a camera.
}
TEST_CASE("An orbiting camera is placed by its focus point and swings around it between keys", "[editor][camera]") {
    auto state = scene();
    state.document.timeline_duration = 10;
    const CameraPose start{0, 0, 10, {}, 1}, end{90, 0, 10, {4, 0, 0}, 1};
    const auto camera = add_camera(state, start);
    REQUIRE(key_camera(state, camera, 0, start));
    REQUIRE(key_camera(state, camera, 4, end));
    const auto eye = [&](f32 time) { return evaluate_transform(state, *find_instance(state, camera), time).position; };
    const auto near = [](Vec3 a, Vec3 b) { return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z) < 1e-4F; };
    const auto position_keys = [&] { return state.document.timeline.find({camera, "position"}); };
    CHECK_FALSE(camera_settings(state, camera)->orbit); // Straight lines by default.
    CHECK(eye(2).x == Catch::Approx(7)); CHECK(eye(2).z == Catch::Approx(5));

    // Switching keeps the camera where it is at every key: its position keys
    // become the focus points they look at.
    const auto first = eye(0), last = eye(4);
    REQUIRE(set_camera_orbit(state, camera, true));
    CHECK(camera_settings(state, camera)->orbit);
    CHECK(near(std::get<Vec3>(position_keys()->keys[1].value), {4, 0, 0}));
    CHECK(position_keys()->label == "Focus point");
    CHECK(near(eye(0), first)); CHECK(near(eye(4), last));
    // The focus point moves from the origin to (4, 0, 0) while the camera
    // turns a quarter around it, keeping its focus distance.
    CHECK(close(*evaluate_camera(state, 1), {22.5F, 0, 10, {1, 0, 0}, 1}));
    CHECK(close(*evaluate_camera(state, 2), {45, 0, 10, {2, 0, 0}, 1}));
    CHECK(eye(2).x == Catch::Approx(2 + 7.0710678)); CHECK(eye(2).z == Catch::Approx(7.0710678));
    // After the last key it keeps turning around the last focus point, and a
    // focus change dollies toward that point rather than moving it.
    REQUIRE(key_property(state, {camera, "rotation"}, 8, Vec3{0, 180, 0}));
    CHECK(close(*evaluate_camera(state, 8), {180, 0, 10, {4, 0, 0}, 1}));
    REQUIRE(key_property(state, {camera, "focus"}, 6, 4.F));
    CHECK(close(*evaluate_camera(state, 6), {135, 0, 4, {4, 0, 0}, 1}));

    // A keyframe added anywhere keeps the path: it stores the focus point there.
    std::vector<CameraPose> path;
    for (const f32 time : {1.F, 2.F, 3.F, 5.F}) path.push_back(*evaluate_camera(state, time));
    REQUIRE(add_keyframe(state, 3));
    std::size_t index{};
    for (const f32 time : {1.F, 2.F, 3.F, 5.F}) CHECK(close(*evaluate_camera(state, time), path[index++]));
    // Moving it in the viewport stores the focus point that shows its eye there.
    state.viewport.time = 3;
    state.viewport.paused = true;
    REQUIRE(apply_placed_position(state, camera, {20, 1, 2}));
    CHECK(near(eye(3), {20, 1, 2}));
    // Saving a view stores that view's target.
    REQUIRE(key_camera(state, camera, 5, {30, 10, 6, {1, 1, 1}, 1}));
    CHECK(close(*evaluate_camera(state, 5), {30, 10, 6, {1, 1, 1}, 1}));
    CHECK(near(std::get<Vec3>(std::ranges::find(position_keys()->keys, 5.F, &timeline::Keyframe::time)->value), {1, 1, 1}));
    // A held key holds the focus point until it.
    auto held = *position_keys();
    std::ranges::find(held.keys, 4.F, &timeline::Keyframe::time)->incoming = timeline::Interpolation::hold;
    const auto keyed_at_three = std::get<Vec3>(std::ranges::find(held.keys, 3.F, &timeline::Keyframe::time)->value);
    REQUIRE(state.document.timeline.replace_track(held));
    CHECK(near(evaluate_camera(state, 3.5F)->target, keyed_at_three));

    // Switching back keeps it where it is at every key, too.
    std::vector<Vec3> eyes;
    for (const auto& key : position_keys()->keys) eyes.push_back(eye(key.time));
    REQUIRE(set_camera_orbit(state, camera, false));
    CHECK(position_keys()->label == "Position");
    index = 0;
    for (const auto& key : position_keys()->keys) CHECK(near(eye(key.time), eyes[index++]));

    // Without position keys an orbiting camera turns around the point its
    // placement looks at.
    auto turning = scene();
    const auto still = add_camera(turning, {0, 0, 10, {1, 2, 3}, 1});
    REQUIRE(set_camera_orbit(turning, still, true));
    CHECK(near(find_instance(turning, still)->transform.position, {1, 2, 3}));
    REQUIRE(key_property(turning, {still, "rotation"}, 4, Vec3{-30, 90, 0}));
    CHECK(close(*evaluate_camera(turning, 2), {45, 15, 10, {1, 2, 3}, 1}));

    // The placement is saved and travels in worker patches; it is not keyable.
    REQUIRE(set_camera_orbit(state, camera, true));
    auto bytes = encode_scene(state); REQUIRE(bytes);
    auto restored = decode(*bytes); REQUIRE(restored);
    CHECK(camera_settings(*restored, camera)->orbit);
    CHECK(close(*evaluate_camera(*restored, 2), *evaluate_camera(state, 2)));
    auto worker = *restored;
    REQUIRE(set_camera_orbit(worker, camera, false));
    DocumentChanges toggled;
    toggled.properties.insert({camera, "orbit"});
    toggled.properties.insert({camera, "position"});
    ++restored->document.revision;
    const auto patch = capture_patch(restored->document.revision - 1, *restored, toggled); REQUIRE(patch);
    const auto encoded = encode_patch(*patch); REQUIRE(encoded);
    const auto decoded = decode_patch(*encoded); REQUIRE(decoded);
    REQUIRE(apply_patch(worker, *decoded));
    CHECK(camera_settings(worker, camera)->orbit);
    CHECK(close(*evaluate_camera(worker, 2), *evaluate_camera(*restored, 2)));
    CHECK_FALSE(key_property(state, {camera, "orbit"}, 3, false, timeline::Interpolation::hold));

    // The inspector switches it with the rest of the lens, all or nothing: a
    // focus point that would leave the scene undoes the whole edit.
    auto edge = scene();
    const auto outward = add_camera(edge, {0, 0, 10, {}, 1});
    find_instance(edge, outward)->transform = {{999999, 0, 0}, {0, -90, 0}, 1};
    camera_settings(edge, outward)->focus = 100;
    edge.viewport.paused = true;
    const auto lens = *camera_settings(edge, outward);
    auto next = lens;
    next.focus = 50;
    next.orbit = true;
    DocumentChanges edited;
    CHECK_FALSE(apply_lens(edge, edited, outward, lens, next));
    CHECK(*camera_settings(edge, outward) == lens);
    CHECK(edited.empty());
    next.orbit = false;
    REQUIRE(apply_lens(edge, edited, outward, lens, next));
    CHECK(camera_settings(edge, outward)->focus == 50);
}

TEST_CASE("Only one camera can be active at a time and the session keys the switch", "[editor][camera][session]") {
    auto initial = scene();
    const auto first = add_camera(initial, {0, 0, 10, {}, 1});
    const auto second = add_camera(initial, {90, 0, 10, {}, 1});
    initial.document.keyframe_names = {{0, "start"}, {5, "cut"}};
    REQUIRE(encode_scene(initial));
    auto conflicting = initial;
    REQUIRE(key_property(conflicting, {second, "active"}, 5, true, timeline::Interpolation::hold));
    CHECK_FALSE(encode_scene(conflicting)); // Two active cameras at t=5.
    EditingSession session{initial};
    session.viewport().time = 5;
    session.select_keyframe(5);
    REQUIRE(session.can_edit_scene_pose());
    auto switched = session.set_active_camera(second);
    REQUIRE(switched); CHECK(*switched);
    const auto& state = session.state();
    CHECK(active_camera(state, 0)->id == first);
    CHECK(active_camera(state, 5)->id == second);
    CHECK(active_camera(state, 9)->id == second);
    CHECK(evaluate_camera(state, 5)->yaw == Catch::Approx(90));
    CHECK(session.dirty());
    auto again = session.set_active_camera(second); REQUIRE(again); CHECK_FALSE(*again);
    REQUIRE(session.undo());
    CHECK(active_camera(session.state(), 5)->id == first);
    REQUIRE(session.redo());
    CHECK(active_camera(session.state(), 5)->id == second);
    session.select_keyframe(std::nullopt);
    CHECK_FALSE(session.set_active_camera(first));
    CHECK_FALSE(session.set_active_camera(1)); // Meshes cannot be cameras.
}

TEST_CASE("Saving the editor view authors a camera at the selected keyframe", "[editor][camera][session]") {
    auto initial = scene();
    const auto camera = add_camera(initial, {0, 0, 10, {}, 1});
    initial.document.keyframe_names = {{0, "start"}, {5, "look"}};
    EditingSession session{initial};
    const CameraPose view{40, -15, 3, {2, 1, 0}, 1.8F};
    session.viewport().editor_camera = view;
    CHECK_FALSE(session.set_camera(camera, view)); // No keyframe selected.
    session.viewport().time = 5;
    session.select_keyframe(5);
    auto saved = session.set_camera(camera, view);
    REQUIRE(saved); CHECK(*saved);
    const auto& state = session.state();
    CHECK(close(*evaluate_camera(state, 5), view));
    CHECK(close(*evaluate_camera(state, 0), {0, 0, 10, {}, 1})); // The keyed value starts at the cut, not before.
    for (const auto property : {"position", "rotation", "zoom", "focus"})
        CHECK(state.document.timeline.find({camera, property}));
    auto repeat = session.set_camera(camera, view); REQUIRE(repeat); CHECK_FALSE(*repeat);
    REQUIRE(session.undo());
    CHECK(close(*evaluate_camera(session.state(), 5), {0, 0, 10, {}, 1}));
    REQUIRE(session.redo());
    CHECK(close(*evaluate_camera(session.state(), 5), view));
    CHECK_FALSE(session.set_camera(1, view));
}

TEST_CASE("A camera is picked by clicking its drawn body", "[editor][camera][selection]") {
    auto state = scene();
    const auto camera = add_camera(state, {0, 0, 10, {}, 1}); // Eye at z=10 looking down -Z.
    // Stand at the origin looking +Z, so the camera body sits in the middle of the view.
    state.viewport.editor_camera = {180, 0, 10, {0, 0, 10}, 1};
    const Extent2D extent{800, 600};
    CHECK(pick_object(state, {.5F, .5F}, extent) == camera);
    CHECK_FALSE(pick_object(state, {.02F, .02F}, extent));
    camera_settings(state, camera)->visible = false;
    CHECK_FALSE(pick_object(state, {.5F, .5F}, extent)); // Hidden glyph, nothing to click.
    camera_settings(state, camera)->visible = true;
    state.viewport.selected_object = camera;
    state.viewport.gizmo_only = true;
    CHECK_FALSE(pick_object(state, {.5F, .5F}, extent)); // Gizmo-only hides the active surface.
}

TEST_CASE("Cameras move, turn and slide along their look direction, and draw a frustum glyph", "[editor][camera]") {
    auto state = scene();
    const auto camera = add_camera(state, {0, 0, 10, {}, 1});
    const auto manipulation = blueprint_manipulation(state, BlueprintId::camera);
    CHECK(manipulation.surface == ManipulationSurface::object);
    for (const auto mode : {GizmoMode::move, GizmoMode::rotate, GizmoMode::free_rotate, GizmoMode::forward})
        CHECK(std::ranges::find(manipulation.gizmos, mode) != manipulation.gizmos.end());
    CHECK(std::ranges::find(manipulation.gizmos, GizmoMode::scale) == manipulation.gizmos.end());
    const auto axes = blueprint_translation_axes(state, evaluate_instance(state, *find_instance(state, camera), 0));
    REQUIRE(axes.size() == 1);
    CHECK(axes.front().label == "Forward / back");
    CHECK(axes.front().direction.z == Catch::Approx(-1)); // Yaw zero looks down -Z.
    auto lines = scene_annotation_lines(state, 0);
    CHECK(lines.size() == camera_glyph_line_count); // Body, lens, reels, frustum, up tick and look line.
    CHECK(scene_annotation_triangles(state, 0).size() == camera_glyph_triangle_count); // Solid body, lens and reels.
    CHECK(scene_annotation_triangles(state, 0).front().color.x < .6F); // Fills are a darker tone than the wire.
    const auto eye = find_instance(state, camera)->transform.position;
    CHECK(std::ranges::count_if(lines, [&](const SceneLine& line) { return line.from == eye; }) == 4);
    state.viewport.mode = ViewMode::mesh;
    CHECK(scene_annotation_lines(state, 0).empty());
    state.viewport.mode = ViewMode::scene;
    camera_settings(state, camera)->visible = false;
    CHECK(scene_annotation_lines(state, 0).empty());
    CHECK(scene_annotation_triangles(state, 0).empty());
}

TEST_CASE("Independent Play follows the scene camera until navigated, and any camera edit hands it back", "[editor][camera][play]") {
    auto state = scene();
    state.document.timeline_duration = 30;
    const auto camera = add_camera(state, {0, 0, 10, {}, 1});
    REQUIRE(key_camera(state, camera, 0, {0, 0, 10, {}, 1}));
    REQUIRE(key_camera(state, camera, 10, {60, 10, 10, {}, 1}));
    PlayCamera play;
    play.start(state, 2);
    CHECK_FALSE(play.held());
    CHECK(play.view(state, 2) == *evaluate_camera(state, 2));
    CHECK(play.view(state, 5) == *evaluate_camera(state, 5)); // It follows the animation.

    const CameraPose looked{-45, 20, 3, {1, 1, 1}, 1};
    play.navigated(looked);
    CHECK(play.held());
    CHECK(play.view(state, 6) == looked);

    auto before = PlayCamera::cameras(state);
    instance_transform(state, 1)->position = {5, 0, 0}; // Not a camera.
    CHECK_FALSE(play.authored(before, state));
    CHECK(play.view(state, 6) == looked);
    before = PlayCamera::cameras(state);
    find_instance(state, camera)->name = "Renamed"; // None of these changes what the camera sees.
    camera_settings(state, camera)->visible = false;
    find_instance(state, camera)->transform.rotation.z = 25;
    CHECK_FALSE(play.authored(before, state));
    CHECK(play.held());

    // Switching the path between keys changes what the camera sees.
    before = PlayCamera::cameras(state);
    camera_settings(state, camera)->orbit = true;
    CHECK(play.authored(before, state));
    CHECK_FALSE(play.held());
    play.navigated(looked);

    // A key far from the current playhead still counts as a camera edit.
    before = PlayCamera::cameras(state);
    REQUIRE(key_camera(state, camera, 25, {90, 0, 10, {}, 1}));
    CHECK(play.authored(before, state));
    CHECK_FALSE(play.held());
    CHECK(play.view(state, 6) == *evaluate_camera(state, 6));

    // Removing the camera keeps the view Play was showing.
    const auto shown = play.view(state, 6);
    before = PlayCamera::cameras(state);
    REQUIRE(erase_instance(state, camera));
    CHECK(play.authored(before, state));
    CHECK(play.held());
    CHECK(play.view(state, 7) == shown);
}

TEST_CASE("Independent Play of a scene without a camera holds the view it started with", "[editor][camera][play]") {
    auto state = scene();
    state.viewport.editor_camera = {10, 5, 8, {}, 1};
    PlayCamera play;
    play.start(state, 0);
    CHECK(play.held());
    const auto started = state.viewport.editor_camera;
    state.viewport.editor_camera = {-80, 30, 2, {4, 4, 4}, 1}; // Editor navigation during Play.
    CHECK(play.view(state, 1) == started);
    const auto before = PlayCamera::cameras(state);
    const auto camera = add_camera(state, {30, 0, 12, {}, 1});
    CHECK(play.authored(before, state)); // Adding a camera hands the view to it.
    CHECK(play.view(state, 1) == *evaluate_camera(state, 1));
    CHECK(find_instance(state, camera));
}

TEST_CASE("The editor can look through a camera only when its pivot is within the view range", "[editor][camera]") {
    auto state = scene();
    const auto camera = add_camera(state, {30, 10, 8, {1, 2, 3}, 2});
    const auto near_pose = look_through(state, *find_instance(state, camera), 0);
    REQUIRE(near_pose);
    CHECK(close(*near_pose, {30, 10, 8, {1, 2, 3}, 2}));
    auto* far = find_instance(state, camera);
    far->transform = {{-600000, 0, 0}, {0, 90, 0}, 1};
    camera_settings(state, camera)->focus = 600000; // Pivot at x = -1.2e6.
    CHECK_FALSE(look_through(state, *far, 0));
    CHECK(evaluate_camera(state, 0)); // Play can still render through it.
}
