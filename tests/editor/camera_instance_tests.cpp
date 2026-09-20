#include "../../examples/editor/editing_session.hpp"
#include "../../examples/editor/animation.hpp"
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
    CHECK(close(evaluate_camera(state, 0), first_view));
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
    CHECK(bytes->find("editor_project = 4;") != std::string::npos);
    auto restored = decode(*bytes); REQUIRE(restored);
    CHECK(restored->document.instances == state.document.instances);
    CHECK(close(evaluate_camera(*restored, 0), first_view));
}

TEST_CASE("The scene camera follows the active camera's keys and falls back without cameras", "[editor][camera]") {
    auto state = scene();
    const CameraPose start{0, 0, 10, {}, 1}, end{90, 0, 10, {}, 1};
    REQUIRE(key_camera(state, 0, start)); // The legacy document shot still evaluates without cameras.
    REQUIRE(key_camera(state, 4, end));
    CHECK_FALSE(has_camera(state));
    CHECK(has_camera_animation(state));
    CHECK(evaluate_camera(state, 2).yaw == Catch::Approx(45));
    auto camera = ensure_camera(state, start, "Shot"); REQUIRE(camera);
    CHECK(find_instance(state, *camera)->name == "Shot");
    auto again = ensure_camera(state, end); REQUIRE(again);
    CHECK(*again == *camera); // An existing camera is kept, not moved.
    CHECK(close(evaluate_camera(state, 2), start)); // Instance without keys: the legacy tracks no longer apply.
    REQUIRE(key_camera(state, *camera, 0, start));
    REQUIRE(key_camera(state, *camera, 4, end));
    CHECK(has_camera_animation(state));
    const auto middle = evaluate_camera(state, 2);
    CHECK(middle.distance == Catch::Approx(10));
    // Interpolating the eye position moves along the chord, so the pivot distance is kept while the eye passes closer.
    const auto expected_eye = evaluate_transform(state, *find_instance(state, *camera), 2).position;
    CHECK(expected_eye.x == Catch::Approx(5)); CHECK(expected_eye.z == Catch::Approx(5));
    CHECK(middle.yaw == Catch::Approx(45));
    CHECK_FALSE(key_camera(state, 1, 2, end)); // Not a camera.
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
    CHECK(evaluate_camera(state, 5).yaw == Catch::Approx(90));
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
    CHECK(close(evaluate_camera(state, 5), view));
    CHECK(close(evaluate_camera(state, 0), {0, 0, 10, {}, 1})); // The keyed value starts at the cut, not before.
    for (const auto property : {"position", "rotation", "zoom", "focus"})
        CHECK(state.document.timeline.find({camera, property}));
    auto repeat = session.set_camera(camera, view); REQUIRE(repeat); CHECK_FALSE(*repeat);
    REQUIRE(session.undo());
    CHECK(close(evaluate_camera(session.state(), 5), {0, 0, 10, {}, 1}));
    REQUIRE(session.redo());
    CHECK(close(evaluate_camera(session.state(), 5), view));
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
    const auto eye = find_instance(state, camera)->transform.position;
    CHECK(std::ranges::count_if(lines, [&](const SceneLine& line) { return line.from == eye; }) == 4);
    state.viewport.mode = ViewMode::mesh;
    CHECK(scene_annotation_lines(state, 0).empty());
    state.viewport.mode = ViewMode::scene;
    camera_settings(state, camera)->visible = false;
    CHECK(scene_annotation_lines(state, 0).empty());
}
