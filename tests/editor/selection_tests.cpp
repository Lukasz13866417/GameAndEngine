#include "../../examples/editor/selection.hpp"
#include "../../examples/editor/animation.hpp"
#include <catch2/catch_test_macros.hpp>

#include <limits>

namespace {
using namespace vng;
using namespace editor_example;
constexpr Extent2D extent{800, 600};
State scene(std::vector<f32> positions = {-1, -1, 0, 1, -1, 0, 0, 1, 0},
            std::vector<gfx::TriangleFace> faces = {{0, 1, 2}}) {
    content::vmesh::Document document;
    document.vertex_count = positions.size() / 3;
    document.vertex_fields = {
        {"position", {content::vmesh::ScalarType::Float32, 3}, std::move(positions)}};
    document.faces = std::move(faces);
    auto mesh = editor::EditableMesh::create(std::move(document));
    REQUIRE(mesh);
    State state{.document = {.mesh = std::move(*mesh)}};
    state.viewport.editor_camera.yaw = state.viewport.editor_camera.pitch = 0;
    state.viewport.editor_camera.distance = 8;
    (*editor_example::instance_transform(state, 1)).position = {};
    (*editor_example::instance_transform(state, 1)).scale = 1;
    (*editor_example::instance_transform(state, 2)).position = {};
    (*editor_example::sun_settings(state, 2)).displacement = 0;
    (*editor_example::sun_settings(state, 2)).visible = false;
    return state;
}
Vec2 projected(const State& state, Vec3 position, Extent2D size = extent) {
    const auto snapshot = camera(state).snapshot(size);
    REQUIRE(snapshot);
    const auto& m = snapshot->view_projection;
    const auto x = m[0][0] * position.x + m[1][0] * position.y + m[2][0] * position.z + m[3][0];
    const auto y = m[0][1] * position.x + m[1][1] * position.y + m[2][1] * position.z + m[3][1];
    const auto w = m[0][3] * position.x + m[1][3] * position.y + m[2][3] * position.z + m[3][3];
    return {(x / w + 1) * .5F, (1 - y / w) * .5F};
}
} // namespace

TEST_CASE("Local selection gizmos do not wait for worker schemas", "[editor][selection][gizmo]") {
    auto state = scene();
    state.viewport.selected_object = 1;
    auto schema = local_position_gizmo(state, 7);
    REQUIRE(editor::validate(schema));
    CHECK(schema.stamp == editor::Stamp{1, 7, state.document.revision});
    REQUIRE(schema.controls.size() == 1);
    CHECK(std::get<Vec3>(schema.controls.front().fields.front().value) == Vec3{});
    REQUIRE(key_property(state, {1, "position"}, 5, Vec3{2, 3, 4}));
    state.viewport.time = 5;
    ++state.document.revision;
    schema = local_position_gizmo(state, 7);
    REQUIRE(schema.controls.size() == 1);
    CHECK(std::get<Vec3>(schema.controls.front().fields.front().value) == Vec3{2, 3, 4});
    mesh_settings(state, 1)->visible = false;
    CHECK(local_position_gizmo(state, 7).controls.empty());
    state.viewport.selected_object = 2;
    sun_settings(state, 2)->visible = true;
    CHECK(local_position_gizmo(state, 7).controls.size() == 1);
    state.viewport.mode = ViewMode::mesh;
    CHECK(local_position_gizmo(state, 7).controls.empty());
    state.viewport.mode = ViewMode::scene;
    state.viewport.selected_object = 0;
    CHECK(local_position_gizmo(state, 7).controls.empty());
    state.viewport.selected_object = 999;
    CHECK(local_position_gizmo(state, 7).controls.empty());
}

TEST_CASE("Gizmo-only selection keeps controls but lets picking pass through the hidden surface",
          "[editor][selection][gizmo-only]") {
    auto state=scene();
    state.viewport.selected_object=1;
    REQUIRE(pick_object(state,{.5F,.5F},extent)==1);
    state.viewport.gizmo_only=true;
    CHECK_FALSE(pick_object(state,{.5F,.5F},extent));
    CHECK(local_position_gizmo(state,1).controls.size()==1);
    state.viewport.gizmo_only=false;
    CHECK(pick_object(state,{.5F,.5F},extent)==1);
    sun_settings(state,2)->visible=true;
    state.viewport.selected_object=2;
    state.viewport.gizmo_only=true;
    CHECK(pick_object(state,{.5F,.5F},extent)==1);
    CHECK(local_position_gizmo(state,1).controls.size()==1);
}

TEST_CASE("Projected instance origins reuse unchanged scene inputs, not selection revisions",
          "[editor][selection][performance]") {
    auto state=scene();
    for(u32 id=3;id<303;++id)
        state.document.instances.push_back({id,BlueprintId::mesh,"Ship",MeshSettings{}, {}});
    InstanceProjection projection;
    const auto view=camera(state);
    const auto expected=instance_points(state,extent,view);
    const auto points=projection.get(state,extent,view);
    CHECK(std::ranges::equal(points,expected));
    const auto* storage=points.data();
    for(u32 i=0;i<100;++i) {
        ++state.viewport.sequence;
        state.viewport.selected_object=3+i;
        CHECK(projection.get(state,extent,view).data()==storage);
    }
    CHECK(projection.rebuilds()==1);
    instance_transform(state,1)->position={1,0,0};
    ++state.document.revision;
    CHECK(std::ranges::equal(projection.get(state,extent,view),instance_points(state,extent,view)));
    CHECK(projection.rebuilds()==2);
    REQUIRE(key_property(state,{1,"position"},1,Vec3{-1,0,0}));
    ++state.document.revision;
    (void)projection.get(state,extent,view);
    state.viewport.time=1;
    CHECK(std::ranges::equal(projection.get(state,extent,view),instance_points(state,extent,view)));
    CHECK(projection.rebuilds()==4);
    state.viewport.editor_camera.yaw=45;
    const auto turned=camera(state);
    CHECK(std::ranges::equal(projection.get(state,extent,turned),instance_points(state,extent,turned)));
    CHECK(projection.rebuilds()==5);
    (void)projection.get(state,{1600,600},turned);
    CHECK(projection.rebuilds()==6);
    auto clipped=turned;
    clipped.set_perspective({.vertical_fov=degrees(45),.near_plane=.1F,.far_plane=1.F});
    CHECK(projection.get(state,extent,clipped).empty());
    CHECK(projection.rebuilds()==7);
    CHECK(projection.get(state,{},view).empty());
    CHECK(std::ranges::equal(projection.get(state,extent,view),instance_points(state,extent,view)));
    CHECK(projection.rebuilds()==8);
}

TEST_CASE("Instance projection invalidates isolated views and animated visibility",
          "[editor][selection][performance]") {
    auto state=scene();
    const auto view=camera(state);
    InstanceProjection projection;
    REQUIRE(key_property(state,{1,"visible"},1,false));
    REQUIRE(projection.get(state,extent,view).size()==1);
    state.viewport.time=1;
    CHECK(projection.get(state,extent,view).empty());
    state.viewport.time=0;
    state.viewport.mode=ViewMode::mesh;
    CHECK(projection.get(state,extent,view).empty());
    sun_settings(state,2)->visible=true;
    auto second=instantiate(state,BlueprintId::sun); REQUIRE(second);
    sun_settings(state,*second)->visible=true;
    ++state.document.revision;
    state.viewport.mode=ViewMode::sun;
    for(const auto id : {2U,*second}) {
        state.viewport.selected_object=id;
        const auto points=projection.get(state,extent,view);
        REQUIRE(points.size()==1);
        CHECK(points.front().object==id);
    }
}

TEST_CASE("Scene picking uses exact two-sided triangles, not vertex or bounding-box proxies",
          "[editor][selection]") {
    auto state = scene();
    CHECK(pick_object(state, {.5F, .5F}, extent) == 1);
    CHECK_FALSE(pick_object(state, projected(state, {.8F, .8F, 0}), extent));
    CHECK_FALSE(pick_object(state, {.01F, .01F}, extent));
    // Same triangle viewed from behind: editor mesh rendering has no culling.
    state.viewport.editor_camera.yaw = 180;
    CHECK(pick_object(state, {.5F, .5F}, extent) == 1);
    // No face across the center: an object's aggregate bounds cannot fill holes.
    auto hole = scene({-2, -1, 0, -1, -1, 0, -1.5F, 1, 0, 1, -1, 0, 2, -1, 0, 1.5F, 1, 0},
                      {{0, 1, 2}, {3, 4, 5}});
    CHECK_FALSE(pick_object(hole, {.5F, .5F}, extent));
    CHECK(pick_object(hole, projected(hole, {-1.5F, 0, 0}), extent) == 1);
    CHECK_FALSE(pick_object(scene({-1, 0, 0, 0, 0, 0, 1, 0, 0}), {.5F, .5F}, extent));
}

TEST_CASE("Local-space picking follows the rendered matrix through combined rotations and scales",
          "[editor][selection][spatial]") {
    auto state=scene();
    auto* transform=instance_transform(state,1);
    for(const float scale:{.01F,.25F,1.F,12.F})for(const Vec3 rotation:{Vec3{21,37,13},Vec3{-71,141,42},Vec3{180,0,0}}) {
        transform->rotation=rotation;transform->scale=scale;transform->position={.2F,-.1F,.5F};
        PickStats stats;
        CHECK(pick_object(state,projected(state,transform->position),extent,nullptr,&stats)==1);
        CHECK(stats.mesh_instances==1);CHECK(stats.geometry.triangle_tests==1);
    }
}

TEST_CASE("Picking and animated gizmos use stable IDs for multiple blueprint instances",
          "[editor][selection][instances]") {
    auto state = scene();
    REQUIRE(erase_instance(state, 1));
    auto first = instantiate(state, BlueprintId::mesh);
    auto second = instantiate(state, BlueprintId::mesh);
    REQUIRE(first);
    REQUIRE(second);
    instance_transform(state, *first)->scale = .5F;
    instance_transform(state, *second)->scale = .5F;
    instance_transform(state, *first)->position = {-1, 0, 0};
    instance_transform(state, *second)->position = {1, 0, 0};
    CHECK(pick_object(state, projected(state, {-1, 0, 0}), extent) == *first);
    CHECK(pick_object(state, projected(state, {1, 0, 0}), extent) == *second);
    CHECK_FALSE(pick_object(state, {.5F, .5F}, extent));
    state.viewport.selected_object = *second;
    REQUIRE(key_property(state, {*second, "position"}, 2, Vec3{1, 1, 0}));
    state.viewport.time = 2;
    editor::Inspector inspector{{*second, 4, state.document.revision}};
    inspector.translation_gizmo("position", instance_transform(state, *second)->position, [](Vec3) {});
    const auto schema = selection_gizmo_schema(state, inspector.schema());
    REQUIRE(schema.controls.size() == 1);
    CHECK(std::get<Vec3>(schema.controls[0].fields[0].value) == Vec3{1, 1, 0});
    CHECK(pick_object(state, projected(state, {1, 1, 0}), extent) == *second);
    CHECK(project_vertex(state, 2, extent));
    REQUIRE(erase_instance(state, *second));
    CHECK(selection_gizmo_schema(state, inspector.schema()).controls.empty());
    CHECK_FALSE(pick_object(state, projected(state, {1, 1, 0}), extent));
    CHECK(pick_object(state, projected(state, {-1, 0, 0}), extent) == *first);
    state.viewport.mode = ViewMode::mesh;
    CHECK_FALSE(pick_object(state, {.5F, .5F}, extent));
    REQUIRE(erase_instance(state, *first));
    CHECK(project_vertex(state, 0, extent)); // retained blueprint has no instance dependency
    CHECK_FALSE(pick_object(state, {.5F, .5F}, extent));
}

TEST_CASE("Imported blueprints pick and project their own geometry in scene and isolated views",
          "[editor][selection][import]") {
    auto state = scene();
    auto different = scene({1.5F, -.5F, 0, 2.5F, -.5F, 0, 2, .5F, 0});
    const auto blueprint = static_cast<BlueprintId>(3);
    state.document.mesh_assets.push_back({blueprint, "Offset triangle", std::move(different.document.mesh),
                                 MeshSettings{}});
    state.document.next_blueprint_id = 4;
    const auto created = instantiate(state, blueprint);
    REQUIRE(created);
    CHECK(pick_object(state, projected(state, {}), extent) == 1);
    CHECK(pick_object(state, projected(state, {2, 0, 0}), extent) == *created);
    const auto vertex = project_vertex(state, 2, extent);
    REQUIRE(vertex);
    const auto expected = projected(state, {2, .5F, 0});
    CHECK(vertex->x == expected.x);
    CHECK(vertex->y == expected.y);
    CHECK(instance_mesh(state, 1) == &state.document.mesh);
    CHECK(editable_mesh(state) == instance_mesh(state, *created));
    state.viewport.mode = ViewMode::mesh;
    state.viewport.inspected_mesh = blueprint;
    CHECK_FALSE(pick_object(state, projected(state, {}), extent));
    CHECK_FALSE(pick_object(state, projected(state, {2, 0, 0}), extent));
    const auto isolated_vertex = project_vertex(state, 2, extent);
    REQUIRE(isolated_vertex);
    const auto isolated_expected = projected(state, {2, .5F, 0});
    CHECK(isolated_vertex->x == isolated_expected.x);
    CHECK(isolated_vertex->y == isolated_expected.y);
    REQUIRE(erase_instance(state, *created));
    CHECK(editable_mesh(state) == &state.document.mesh_assets.front().geometry);
    CHECK(project_vertex(state, 2, extent));
    CHECK_FALSE(pick_object(state, projected(state, {}), extent));
    state.viewport.inspected_mesh = BlueprintId::mesh;
    CHECK(editable_mesh(state) == &state.document.mesh);
    CHECK(project_vertex(state, 2, extent));
}

TEST_CASE("Scene picking respects mesh transforms, camera orbit, zoom, and viewport aspect",
          "[editor][selection]") {
    auto state = scene();
    (*editor_example::instance_transform(state, 1)).position = {1, .25F, -.25F};
    (*editor_example::instance_transform(state, 1)).scale = .4F;
    for (const float yaw : {-40.F, 0.F, 60.F}) {
        state.viewport.editor_camera.yaw = yaw;
        state.viewport.editor_camera.pitch = 19;
        for (const float distance : {5.F, 9.F, 20.F}) {
            state.viewport.editor_camera.distance = distance;
            for (const Extent2D size : {Extent2D{600, 800}, Extent2D{1400, 500}}) {
                CAPTURE(yaw, distance, size.width, size.height);
                CHECK(pick_object(state, projected(state, (*editor_example::instance_transform(state, 1)).position, size), size) == 1);
            }
        }
    }
}

TEST_CASE("Scene picking chooses the closest eligible entity and ignores hidden or excluded views",
          "[editor][selection]") {
    auto state = scene();
    (*editor_example::sun_settings(state, 2)).visible = true;
    CHECK(pick_object(state, {.5F, .5F}, extent) == 2); // front of sphere precedes mesh
    (*editor_example::instance_transform(state, 1)).position.z = 2;
    CHECK(pick_object(state, {.5F, .5F}, extent) == 1);
    (*editor_example::mesh_settings(state, 1)).visible = false;
    CHECK(pick_object(state, {.5F, .5F}, extent) == 2);
    (*editor_example::sun_settings(state, 2)).visible = false;
    CHECK_FALSE(pick_object(state, {.5F, .5F}, extent));
    (*editor_example::mesh_settings(state, 1)).visible = (*editor_example::sun_settings(state, 2)).visible = true;
    state.viewport.mode = ViewMode::mesh;
    CHECK_FALSE(pick_object(state, {.5F, .5F}, extent));
    state.viewport.mode = ViewMode::sun;
    CHECK(pick_object(state, {.5F, .5F}, extent) == 2);
    (*editor_example::sun_settings(state, 2)).visible = false;
    CHECK_FALSE(pick_object(state, {.5F, .5F}, extent));
}

TEST_CASE("Scene picking follows the panned orbit target without moving authored objects",
          "[editor][selection]") {
    auto state = scene();
    (*editor_example::instance_transform(state, 1)).position = {2, 1, 0};
    (*editor_example::instance_transform(state, 1)).scale = .25F;
    (*editor_example::sun_settings(state, 2)).visible = false;
    const auto original_screen = projected(state, (*editor_example::instance_transform(state, 1)).position);
    CHECK(pick_object(state, original_screen, extent) == 1);
    CHECK_FALSE(pick_object(state, {.5F, .5F}, extent));
    const auto authored_mesh = state.document.mesh.document();
    state.viewport.editor_camera.target = (*editor_example::instance_transform(state, 1)).position;
    CHECK(pick_object(state, {.5F, .5F}, extent) == 1);
    CHECK_FALSE(pick_object(state, original_screen, extent));
    CHECK((*editor_example::instance_transform(state, 1)).position == Vec3{2, 1, 0});
    CHECK(state.document.mesh.document() == authored_mesh);
}

TEST_CASE("Scene picking clips near, far, behind-camera, and invalid coordinates",
          "[editor][selection]") {
    auto state = scene();
    for (const float z : {9.F, 7.99F, -193.F}) {
        (*editor_example::instance_transform(state, 1)).position.z = z;
        CHECK_FALSE(pick_object(state, {.5F, .5F}, extent));
    }
    (*editor_example::instance_transform(state, 1)).position.z = 7.9F;
    CHECK(pick_object(state, {.5F, .5F}, extent) == 1);
    (*editor_example::instance_transform(state, 1)).position.z = -191;
    CHECK(pick_object(state, {.5F, .5F}, extent) == 1);
    (*editor_example::instance_transform(state, 1)).position = {};
    for (const auto p :
         {Vec2{-0.1F, .5F}, Vec2{.5F, 1.1F}, Vec2{std::numeric_limits<f32>::quiet_NaN(), .5F},
          Vec2{.5F, std::numeric_limits<f32>::infinity()}})
        CHECK_FALSE(pick_object(state, p, extent));
    CHECK_FALSE(pick_object(state, {.5F, .5F}, {0, 600}));
    state.viewport.editor_camera.yaw = std::numeric_limits<f32>::quiet_NaN();
    CHECK_FALSE(pick_object(state, {.5F, .5F}, extent));
}

TEST_CASE("Sun picking selects only its bounded body, not the bloom or prominence fringe",
          "[editor][selection]") {
    auto state = scene();
    (*editor_example::mesh_settings(state, 1)).visible = false;
    (*editor_example::sun_settings(state, 2)).visible = true;
    (*editor_example::sun_settings(state, 2)).radius = 1;
    (*editor_example::sun_settings(state, 2)).displacement = 1;
    CHECK(pick_object(state, {.5F, .5F}, extent) == 2);
    CHECK(pick_object(state, projected(state, {.95F, 0, 0}), extent) == 2);
    const auto displaced_limb = projected(state, {1.014F, 0, 0});
    CHECK(pick_object(state, displaced_limb, extent) == 2);
    (*editor_example::sun_settings(state, 2)).displacement = 0;
    CHECK_FALSE(pick_object(state, displaced_limb, extent));
    CHECK_FALSE(pick_object(state, projected(state, {1.1F, 0, 0}), extent));
    (*editor_example::sun_settings(state, 2)).bloom = 5;
    CHECK_FALSE(pick_object(state, projected(state, {1.1F, 0, 0}), extent));
    (*editor_example::instance_transform(state, 2)).position.z = 12;
    CHECK_FALSE(pick_object(state, {.5F, .5F}, extent));
    (*editor_example::instance_transform(state, 2)).position.z = 8; // inside the backface-culled sphere
    CHECK_FALSE(pick_object(state, {.5F, .5F}, extent));
    (*editor_example::instance_transform(state, 2)).position.z = -194;
    CHECK_FALSE(pick_object(state, {.5F, .5F}, extent));
}

TEST_CASE("The bundled animated cube remains pickable and has a gizmo at its visible position",
          "[editor][selection][regression]") {
    auto loaded = load_scene(VNG_TIMELINE_SCENE_PATH);
    REQUIRE(loaded);
    auto& state = *loaded;
    // The bundled scene is editable. Picking this whole animation requires a
    // stable editor view, independently of the scene's own camera.
    state.viewport.editor_camera = CameraPose{};
    state.viewport.selected_object = 1;
    REQUIRE(state.document.timeline.find({1, "position"}));
    editor::Inspector inspector{{1, 4, state.document.revision}};
    inspector.translation_gizmo("position", (*editor_example::instance_transform(state, 1)).position, [](Vec3) {});
    const auto base_schema = inspector.schema();
    for (const f32 time : {0.F, 1.5F, 3.F, 5.F, 6.F, 9.F, 10.F}) {
        CAPTURE(time);
        state.viewport.time = time;
        const auto values = evaluate_scene(state, time);
        CHECK(pick_object(state, projected(state, values.model_transform.position), extent) == 1);
        const auto schema = selection_gizmo_schema(state, base_schema);
        REQUIRE(schema.controls.size() == 1);
        CHECK(schema.stamp == base_schema.stamp);
        CHECK(std::get<Vec3>(schema.controls[0].fields[0].value) == values.model_transform.position);
        CHECK(std::get<Vec3>(base_schema.controls[0].fields[0].value) == (*editor_example::instance_transform(state, 1)).position);
    }
    state.viewport.time = 8;
    CHECK(selection_gizmo_schema(state, base_schema).controls.empty());
    state.viewport.time = 3;
    ++state.document.revision;
    CHECK(selection_gizmo_schema(state, base_schema).controls.empty());
    --state.document.revision;
    state.viewport.selected_object = 2;
    CHECK(selection_gizmo_schema(state, base_schema).controls.empty());
}

TEST_CASE("Animated translation changes the current key, not the mesh base or other keys",
          "[editor][selection][regression]") {
    auto state = scene();
    state.viewport.selected_object = 1;
    REQUIRE(key_property(state, {1, "position"}, 0, Vec3{0, 0, 0}));
    REQUIRE(key_property(state, {1, "position"}, 3, Vec3{1, 1, 0},
                         timeline::Interpolation::hold));
    REQUIRE(key_property(state, {1, "position"}, 6, Vec3{2, 0, 0}));
    const auto base = (*editor_example::mesh_settings(state, 1));
    const auto mesh = state.document.mesh.document();
    const auto revision = state.document.revision;
    const editor::Event event{{1, 7, revision}, "position", editor::Phase::apply,
                               {{"position", Vec3{2, 3, 1}}}};
    state.viewport.time = 3;
    auto applied = apply_animated_translation(state, event);
    REQUIRE(applied);
    CHECK(*applied);
    CHECK((*editor_example::mesh_settings(state, 1)) == base);
    CHECK(state.document.mesh.document() == mesh);
    CHECK(state.document.revision == revision);
    const auto* track = state.document.timeline.find({1, "position"});
    REQUIRE(track);
    REQUIRE(track->keys.size() == 3);
    CHECK(track->keys[1].incoming == timeline::Interpolation::hold);
    CHECK(std::get<Vec3>(track->keys[0].value) == Vec3{0, 0, 0});
    CHECK(std::get<Vec3>(track->keys[1].value) == Vec3{2, 3, 1});
    CHECK(std::get<Vec3>(track->keys[2].value) == Vec3{2, 0, 0});
    CHECK(evaluate_scene(state, 3).model_transform.position == Vec3{2, 3, 1});
    state.viewport.time = 4;
    CHECK_FALSE(apply_animated_translation(state, event));
    state.document.keyframe_names[4] = "Editable pose";
    auto inserted = event;
    inserted.values[0].value = Vec3{2, 4, 1};
    applied = apply_animated_translation(state, inserted);
    REQUIRE(applied);
    CHECK(*applied);
    track = state.document.timeline.find({1, "position"});
    REQUIRE(track->keys.size() == 4);
    CHECK(track->keys[2].time == 4.F);
    CHECK(track->keys[2].incoming == timeline::Interpolation::linear);
    CHECK(evaluate_scene(state, 4).model_transform.position == Vec3{2, 4, 1});
}

TEST_CASE("Animated translation rejects stale or malformed events atomically",
          "[editor][selection]") {
    auto state = scene();
    state.viewport.selected_object = 1;
    REQUIRE(key_property(state, {1, "position"}, 3, Vec3{1, 1, 0}));
    const auto original = state.document.timeline;
    const editor::Event valid{{1, 7, state.document.revision}, "position", editor::Phase::apply,
                               {{"position", Vec3{2, 3, 1}}}};
    SECTION("Wrong selected object") {
        auto event = valid;
        event.stamp.object = 2;
        CHECK_FALSE(apply_animated_translation(state, event));
    }
    SECTION("Old revision") {
        auto event = valid;
        ++event.stamp.revision;
        CHECK_FALSE(apply_animated_translation(state, event));
    }
    SECTION("Only committed viewport gestures may edit keys") {
        auto event = valid;
        event.phase = editor::Phase::update;
        CHECK_FALSE(apply_animated_translation(state, event));
    }
    SECTION("Invalid position") {
        auto event = valid;
        event.values[0].value = Vec3{std::numeric_limits<f32>::infinity(), 0, 0};
        CHECK_FALSE(apply_animated_translation(state, event));
    }
    SECTION("Wrong value type") {
        auto event = valid;
        event.values[0].value = 1.F;
        CHECK_FALSE(apply_animated_translation(state, event));
    }
    SECTION("Playing") {
        state.viewport.paused = false;
        CHECK_FALSE(apply_animated_translation(state, valid));
    }
    SECTION("Hidden") {
        (*editor_example::mesh_settings(state, 1)).visible = false;
        CHECK_FALSE(apply_animated_translation(state, valid));
    }
    SECTION("Unrelated controls continue to the worker") {
        auto event = valid;
        event.control = "model";
        auto applied = apply_animated_translation(state, event);
        REQUIRE(applied);
        CHECK_FALSE(*applied);
    }
    CHECK(state.document.timeline == original);
}

TEST_CASE("Clicking an animated gizmo without moving does not create a key",
          "[editor][selection]") {
    auto state = scene();
    state.viewport.selected_object = 1;
    REQUIRE(key_property(state, {1, "position"}, 3, Vec3{1, 1, 0}));
    state.viewport.time = 1.5F;
    const auto original = state.document.timeline;
    const editor::Event event{{1, 7, state.document.revision}, "position", editor::Phase::apply,
                               {{"position", evaluate_scene(state, state.viewport.time).model_transform.position}}};
    auto applied = apply_animated_translation(state, event);
    REQUIRE(applied);
    CHECK(*applied);
    CHECK(state.document.timeline == original);
}

TEST_CASE("Sun position tracks use the same selection gizmo authoring path",
          "[editor][selection]") {
    auto state = scene();
    state.viewport.selected_object = 2;
    (*editor_example::sun_settings(state, 2)).visible = true;
    state.viewport.time = 2;
    REQUIRE(key_property(state, {2, "position"}, 2, Vec3{-1, 1, 0}));
    const auto base = (*editor_example::instance_transform(state, 2)).position;
    const editor::Event event{{2, 7, state.document.revision}, "position", editor::Phase::apply,
                               {{"position", Vec3{-2, 2, 1}}}};
    auto applied = apply_animated_translation(state, event);
    REQUIRE(applied);
    CHECK(*applied);
    CHECK((*editor_example::instance_transform(state, 2)).position == base);
    CHECK(evaluate_scene(state, 2).sun_transform.position == Vec3{-2, 2, 1});
}

TEST_CASE("Unanimated translations retain the worker callback path", "[editor][selection]") {
    auto state = scene();
    state.viewport.selected_object = 1;
    const auto base = (*editor_example::instance_transform(state, 1)).position;
    const editor::Event event{{1, 7, state.document.revision}, "position", editor::Phase::apply,
                               {{"position", Vec3{2, 3, 1}}}};
    auto applied = apply_animated_translation(state, event);
    REQUIRE(applied);
    CHECK_FALSE(*applied);
    CHECK((*editor_example::instance_transform(state, 1)).position == base);
    CHECK(state.document.timeline.tracks().empty());
}
