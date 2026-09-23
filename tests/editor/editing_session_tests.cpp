#include "../../examples/editor/editing_session.hpp"
#include "../../examples/editor/instance_controls.hpp"
#include "../../examples/editor/animation.hpp"
#include "../../examples/editor/preview_updates.hpp"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <unistd.h>

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
const void* mesh_storage(const State& state) {
    return std::get<std::vector<f32>>(state.document.mesh.document().vertex_fields[0].values).data();
}
struct Temp {
    std::filesystem::path path;
    Temp() {
        char pattern[] = "/tmp/vng-edit-session-XXXXXX";
        auto* created = ::mkdtemp(pattern); REQUIRE(created); path = created;
    }
    ~Temp() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
static_assert(std::same_as<decltype(std::declval<EditingSession&>().state()), const State&>);
static_assert(std::same_as<decltype(std::declval<EditingSession&>().viewport()), ViewportState&>);
}

TEST_CASE("Only paused keyframe poses allow instance and scene camera authoring",
          "[editor][session][keyframe][readonly]") {
    auto initial = scene();
    REQUIRE(key_property(initial, {1, "scale"}, 0, 1.F));
    REQUIRE(key_property(initial, {1, "scale"}, 4, 2.F));
    const auto camera = ensure_camera(initial, {0, 0, 8, {}});
    REQUIRE(camera);
    const CameraPose moved{30, 10, 6, {1, 0, 0}};
    EditingSession session{initial}; session.select_keyframe(session.state().viewport.time);
    for (const auto time : {2.F, 4.F, 6.F}) {
        session.viewport().time = time;
        CHECK_FALSE(session.can_edit_scene_pose());
        CHECK_FALSE(session.begin_move(1));
        CHECK_FALSE(session.begin_rotation(1));
        CHECK_FALSE(session.begin_scale(1));
        CHECK_FALSE(session.set_camera(*camera, moved));
        CHECK_FALSE(session.begin_remote(7));
        CHECK_FALSE(session.set_transform(1, {0, 30, 0}, 2));
        CHECK_FALSE(session.dirty());
        CHECK_FALSE(session.take_changes());
        // Editor navigation and document-wide tools are not animation edits.
        session.viewport().editor_camera.yaw += 10;
        REQUIRE(session.begin_world_bounds()); REQUIRE(session.cancel());
        REQUIRE(session.begin_vertices(BlueprintId::mesh, std::array<u32,1>{0}));
        REQUIRE(session.cancel());
    }
    session.viewport().time = 2;
    const auto pose = evaluate_scene(session.state(), 2).model_transform;
    REQUIRE(session.add_keyframe(2));
    CHECK(evaluate_scene(session.state(), 2).model_transform == pose);
    REQUIRE(session.begin_scale(1)); REQUIRE(session.scale(3)); REQUIRE(session.commit());
    CHECK(evaluate_scene(session.state(), 2).model_transform.scale == 3);
    session.viewport().paused = false;
    CHECK_FALSE(session.begin_move(1)); CHECK_FALSE(session.set_camera(*camera, moved));
    session.viewport().paused = true;
    session.viewport().time = 0;
    session.select_keyframe(0);
    REQUIRE(session.begin_move(1)); REQUIRE(session.cancel());
}

TEST_CASE("Adjust subdivision from the original mesh with one undo and stale-edit protection", "[editor][session][tool-options]") {
    EditingSession session{scene()};
    const auto original=session.state().document.mesh.document();
    const auto edges=session.state().document.mesh.edges();
    const std::array<u32,3> vertices{0,1,2};
    REQUIRE(session.mesh_operation(BlueprintId::mesh,MeshOperation::subdivide,vertices,edges));
    const auto first=session.state().document.revision;
    REQUIRE(session.can_adjust_mesh_operation(first));
    REQUIRE(session.adjust_mesh_operation(first,{.levels=3}));
    REQUIRE(mesh_edit_geometry(session.state(),BlueprintId::mesh)->document().faces.size()==64);
    const auto third=session.state().document.revision;
    CHECK_FALSE(session.adjust_mesh_operation(first,{.levels=1}));
    REQUIRE(session.adjust_mesh_operation(third,{.levels=2}));
    CHECK(mesh_edit_geometry(session.state(),BlueprintId::mesh)->document().faces.size()==16);
    CHECK(session.state().document.mesh.document()==original);
    const auto revision=session.state().document.revision;
    const auto draft=mesh_edit_geometry(session.state(),BlueprintId::mesh)->document();
    CHECK_FALSE(session.adjust_mesh_operation(revision,{.levels=99}));
    CHECK(session.state().document.revision==revision);
    CHECK(mesh_edit_geometry(session.state(),BlueprintId::mesh)->document()==draft);
    REQUIRE(session.undo());
    CHECK_FALSE(session.state().document.mesh_drafts.contains(BlueprintId::mesh));
    CHECK_FALSE(session.can_undo());
    CHECK_FALSE(session.can_adjust_mesh_operation(revision));
    REQUIRE(session.redo());
    CHECK(mesh_edit_geometry(session.state(),BlueprintId::mesh)->document()==draft);
    CHECK_FALSE(session.can_adjust_mesh_operation(session.state().document.revision));
}

TEST_CASE("Adjusted operation respects saved identity and cannot overwrite a later edit", "[editor][session][tool-options]") {
    Temp temp;EditingSession session{scene()};
    const auto edges=session.state().document.mesh.edges();
    REQUIRE(session.mesh_operation(BlueprintId::mesh,MeshOperation::subdivide,{},edges));
    auto revision=session.state().document.revision;
    REQUIRE(session.save_as(temp.path/"operation.vscene"));CHECK_FALSE(session.dirty());
    REQUIRE(session.adjust_mesh_operation(revision,{.levels=2}));CHECK(session.dirty());
    revision=session.state().document.revision;
    REQUIRE(session.instantiate(BlueprintId::mesh));
    const auto instances=session.state().document.instances;
    CHECK_FALSE(session.adjust_mesh_operation(revision,{.levels=1}));
    CHECK(session.state().document.instances==instances);
}

TEST_CASE("Alignment strength uses the original positions and keeps both anchors fixed", "[editor][session][tool-options]") {
    EditingSession session{scene()};const std::array<u32,3> vertices{0,1,2};
    REQUIRE(session.mesh_operation(BlueprintId::mesh,MeshOperation::align,vertices));
    REQUIRE(session.adjust_mesh_operation(session.state().document.revision,{.strength=.25F}));
    const auto* mesh=mesh_edit_geometry(session.state(),BlueprintId::mesh);
    CHECK(mesh->position(0)==Vec3{0,0,0});CHECK(mesh->position(1)==Vec3{1,0,0});
    CHECK(mesh->position(2)==Vec3{0,.75F,0});
    REQUIRE(session.adjust_mesh_operation(session.state().document.revision,{.strength=.5F}));
    CHECK(mesh_edit_geometry(session.state(),BlueprintId::mesh)->position(2)==Vec3{0,.5F,0});
    REQUIRE(session.undo());CHECK_FALSE(session.can_undo());
}

TEST_CASE("Opening a scene does not grant pose edit permission at time zero",
          "[editor][session][keyframe][startup]") {
    Temp temp;
    auto opened = scene();
    const auto camera = ensure_camera(opened, {0, 0, 8, {}});
    REQUIRE(camera);
    EditingSession session{opened};
    const auto revision = session.state().document.revision;
    const auto initial = session.state().document.instances;
    const auto locked = [&] {
        CHECK_FALSE(session.can_edit_scene_pose());
        CHECK_FALSE(session.begin_move(1));
        CHECK_FALSE(session.begin_rotation(1));
        CHECK_FALSE(session.begin_scale(1));
        CHECK_FALSE(session.set_camera(*camera, {30, 10, 6, {1, 0, 0}}));
        CHECK_FALSE(session.begin_remote(7));
        CHECK_FALSE(session.set_transform(1, {0, 30, 0}, 2));
    };
    locked();
    // Object selection and viewing-camera navigation are not authoring intent.
    session.viewport().selected_object = 1;
    session.viewport().editor_camera.yaw += 10;
    locked();
    CHECK(session.state().document.revision == revision);
    CHECK(session.state().document.instances == initial);
    CHECK_FALSE(session.dirty());
    CHECK_FALSE(session.take_changes());
    REQUIRE(session.begin_vertices(BlueprintId::mesh, std::array<u32, 1>{0}));
    REQUIRE(session.cancel());
    session.select_keyframe(0);
    CHECK(session.can_edit_scene_pose());
    REQUIRE(session.begin_move(1)); REQUIRE(session.cancel());
    REQUIRE(session.begin_remote(7)); session.abandon_remote();
    session.select_keyframe(std::nullopt);
    locked();
    // Even Add at an already existing key is an explicit selection.
    REQUIRE(session.add_keyframe(0));
    CHECK(session.can_edit_scene_pose());
    REQUIRE(session.save_as(temp.path / "scene.vscene"));
    REQUIRE(session.load(temp.path / "scene.vscene"));
    locked();
    CHECK_FALSE(session.dirty());
}

TEST_CASE("Track preferences unblock animated paste beyond 256 without leaking into scenes or history",
          "[editor][session][settings][timeline]") {
    auto initial = scene();
    for (unsigned i = 1; i < 64; ++i) REQUIRE(instantiate(initial, BlueprintId::mesh));
    for (const auto& instance : initial.document.instances) {
        if (!is_mesh_instance(initial, instance.id)) continue;
        REQUIRE(key_property(initial, {instance.id,"position"}, 0, instance.transform.position));
        REQUIRE(key_property(initial, {instance.id,"rotation"}, 0, instance.transform.rotation));
        REQUIRE(key_property(initial, {instance.id,"scale"}, 0, instance.transform.scale));
        REQUIRE(key_property(initial, {instance.id,"brightness"}, 0, 1.F));
    }
    REQUIRE(initial.document.timeline.tracks().size() == 256);
    initial.viewport.time = 0;
    const auto original = encode(initial); REQUIRE(original);
    EditingSession s{std::move(initial)}; s.select_keyframe(s.state().viewport.time);
    const auto unchanged = [&] {
        auto encoded = encode(s.state()); REQUIRE(encoded); CHECK(*encoded == *original);
    };
    const std::array<u32,1> copied{1};
    REQUIRE(s.copy_instances(copied));
    const auto failed = s.paste(); REQUIRE_FALSE(failed);
    CHECK(failed.error().message.find("needs 260 tracks; limit is 256") != std::string::npos);
    CHECK(failed.error().message.find("Settings > Timeline track limit") != std::string::npos);
    unchanged();
    CHECK_FALSE(s.dirty()); CHECK_FALSE(s.can_undo()); CHECK_FALSE(s.take_changes());
    CHECK_FALSE(s.timeline_track_limit(0));
    CHECK_FALSE(s.timeline_track_limit(static_cast<unsigned>(timeline::max_tracks + 1)));
    CHECK(s.timeline_track_limit() == 256);
    REQUIRE(s.timeline_track_limit(1024));
    unchanged(); CHECK_FALSE(s.take_changes());
    auto pasted = s.paste(); REQUIRE(pasted);
    CHECK(s.state().document.timeline.tracks().size() == 260);
    CHECK(s.state().document.instances.size() == 66);
    // Full worker snapshots and compact follow-up edits have no stale 256-track gate.
    auto bytes = encode(s.state()); REQUIRE(bytes);
    auto worker = decode(*bytes); REQUIRE(worker);
    CHECK(worker->document.timeline == s.state().document.timeline);
    (void)s.take_changes();
    REQUIRE(s.timeline_track_limit(1)); // Never destroys existing tracks.
    REQUIRE(s.begin_move(1)); REQUIRE(s.move({3,0,0})); REQUIRE(s.commit());
    auto notice = s.take_changes(); REQUIRE(notice);
    auto patch = capture_patch(worker->document.revision, s.state(), notice->changes); REQUIRE(patch);
    auto wire = encode_patch(*patch); REQUIRE(wire);
    auto decoded = decode_patch(*wire); REQUIRE(decoded);
    REQUIRE(editor_example::apply_patch(*worker, *decoded));
    CHECK(worker->document.timeline == s.state().document.timeline);
    REQUIRE(s.undo()); REQUIRE(s.undo());
    CHECK(s.state().document.timeline.tracks().size() == 256);
    REQUIRE(s.redo());
    CHECK(s.state().document.timeline.tracks().size() == 260);
    CHECK(s.timeline_track_limit() == 1);
    CHECK_FALSE(s.paste()); // Raising it, not undo/redo, restores authoring capacity.
    Temp temp;
    REQUIRE(s.save_as(temp.path / "large.vscene"));
    REQUIRE(s.load(temp.path / "large.vscene"));
    CHECK(s.state().document.timeline.tracks().size() == 260);
    CHECK(s.timeline_track_limit() == 1); CHECK_FALSE(s.dirty());
    REQUIRE(s.timeline_track_limit(1024));
    REQUIRE(s.copy_instances(copied)); REQUIRE(s.paste());
    CHECK(s.state().document.timeline.tracks().size() == 264);
}

TEST_CASE("Instance budgets guard additions atomically without restricting load or history",
          "[editor][session][settings][instances]") {
    auto initial=scene();
    for(unsigned i=0;i<270;++i) REQUIRE(instantiate(initial,BlueprintId::mesh));
    auto bytes=encode_scene(initial); REQUIRE(bytes);
    auto decoded=decode(*bytes); REQUIRE(decoded);
    CHECK(decoded->document.instances.size()==272);
    EditingSession s{*decoded}; s.select_keyframe(s.state().viewport.time);
    REQUIRE(s.instance_limit(272));
    const auto original=encode_scene(s.state()); REQUIRE(original);
    REQUIRE(s.copy_instances(std::array<u32,2>{1,2}));
    auto failed=s.paste(); REQUIRE_FALSE(failed);
    CHECK(failed.error().message.find("Settings > Instance limit")!=std::string::npos);
    CHECK_FALSE(s.instantiate(BlueprintId::mesh));
    Temp temp;
    const auto mesh_path=temp.path/"mesh.vmesh";
    REQUIRE(content::vmesh::write_vmesh(mesh_path, s.state().document.mesh.document()));
    CHECK_FALSE(s.import_mesh(mesh_path));
    CHECK(*encode_scene(s.state())==*original);
    CHECK_FALSE(s.dirty()); CHECK_FALSE(s.can_undo()); CHECK_FALSE(s.take_changes());
    REQUIRE(s.instance_limit(300)); REQUIRE(s.paste());
    CHECK(s.state().document.instances.size()==274);
    REQUIRE(s.instance_limit(1));
    REQUIRE(s.undo()); REQUIRE(s.redo()); CHECK(s.state().document.instances.size()==274);
    REQUIRE(s.save_as(temp.path/"large.vscene")); REQUIRE(s.load(temp.path/"large.vscene"));
    CHECK(s.state().document.instances.size()==274); CHECK(s.instance_limit()==1);
    s.select_keyframe(0);
    REQUIRE(s.begin_move(1)); REQUIRE(s.move({1,2,3})); REQUIRE(s.commit());
    CHECK_FALSE(s.instance_limit(0)); CHECK_FALSE(s.instance_limit(max_scene_instances+1));
}

TEST_CASE("Track budget applies atomically to adding keyframes and saving cameras", "[editor][session][settings][timeline]") {
    EditingSession s{scene()}; s.select_keyframe(s.state().viewport.time);
    const auto properties = animation_properties(s.state()).size();
    REQUIRE(s.timeline_track_limit(static_cast<unsigned>(properties - 1)));
    CHECK_FALSE(s.add_keyframe(1));
    CHECK(s.state().document.timeline.tracks().empty()); CHECK_FALSE(s.can_undo()); CHECK_FALSE(s.take_changes());
    REQUIRE(s.timeline_track_limit(static_cast<unsigned>(properties)));
    REQUIRE(s.add_keyframe(1));
    CHECK(s.state().document.timeline.tracks().size() == properties);
    REQUIRE(s.timeline_track_limit(1));
    REQUIRE(s.add_keyframe(2)); // More keys on existing tracks, not new tracks.
    CHECK(s.state().document.timeline.tracks().size() == properties);

    auto initial = scene();
    const auto camera = ensure_camera(initial, {0, 0, 8, {}});
    REQUIRE(camera);
    initial.viewport.time = 2;
    initial.document.keyframe_names[2] = "Shot";
    EditingSession camera_session{initial}; camera_session.select_keyframe(2);
    REQUIRE(camera_session.timeline_track_limit(1));
    const CameraPose pose{30, 5, 6, {1, 0, 0}, 2};
    CHECK_FALSE(camera_session.set_camera(*camera, pose)); // Saving away from zero keys four camera properties.
    CHECK(camera_session.state().document.timeline == initial.document.timeline);
    CHECK(camera_session.state().document.revision == initial.document.revision);
    CHECK_FALSE(camera_session.dirty()); CHECK_FALSE(camera_session.take_changes());
    REQUIRE(camera_session.timeline_track_limit(4));
    REQUIRE(camera_session.set_camera(*camera, pose));
    CHECK(camera_session.state().document.timeline.tracks().size() == 4);

    // Choosing the active camera keys "active" on both cameras here.
    auto two = scene();
    const auto first = ensure_camera(two, {0, 0, 8, {}});
    REQUIRE(first);
    const auto second = instantiate(two, BlueprintId::camera);
    REQUIRE(second);
    two.viewport.time = 2;
    two.document.keyframe_names[2] = "Cut";
    EditingSession cut_session{two}; cut_session.select_keyframe(2);
    REQUIRE(cut_session.timeline_track_limit(1));
    CHECK_FALSE(cut_session.set_active_camera(*second));
    CHECK(cut_session.state().document.timeline == two.document.timeline);
    CHECK(active_camera(cut_session.state(), 2)->id == *first);
    CHECK_FALSE(cut_session.dirty()); CHECK_FALSE(cut_session.take_changes());
    REQUIRE(cut_session.timeline_track_limit(2));
    REQUIRE(cut_session.set_active_camera(*second));
    CHECK(active_camera(cut_session.state(), 2)->id == *second);

    // Inspector rotation/scale Apply keys rotation, scale and axis scale.
    auto posed = scene();
    posed.viewport.time = 2;
    posed.document.keyframe_names[2] = "Pose";
    EditingSession transform_session{posed}; transform_session.select_keyframe(2);
    REQUIRE(transform_session.timeline_track_limit(1));
    CHECK_FALSE(transform_session.set_transform(1, {0, 30, 0}, 2, Vec3{1, 2, 1}));
    CHECK(transform_session.state().document.timeline == posed.document.timeline);
    CHECK_FALSE(transform_session.dirty()); CHECK_FALSE(transform_session.take_changes());
    REQUIRE(transform_session.timeline_track_limit(3));
    REQUIRE(transform_session.set_transform(1, {0, 30, 0}, 2, Vec3{1, 2, 1}));
    CHECK(transform_session.state().document.timeline.tracks().size() == 3);
}

TEST_CASE("Native callbacks cannot bypass the session track preference", "[editor][session][settings][timeline][remote]") {
    auto initial = scene();
    REQUIRE(key_property(initial, {1,"position"}, 0, Vec3{}));
    EditingSession s{initial}; s.select_keyframe(s.state().viewport.time); REQUIRE(s.timeline_track_limit(1));
    auto worker = initial;
    REQUIRE(key_property(worker, {1,"rotation"}, 0, Vec3{0,30,0}));
    ++worker.document.revision;
    DocumentChanges changes; changes.properties.insert({1,"rotation"});
    auto patch = capture_patch(initial.document.revision, worker, changes); REQUIRE(patch);
    REQUIRE(s.begin_remote(7));
    CHECK_FALSE(s.accept_remote(7,*patch));
    CHECK(s.state().document.timeline == initial.document.timeline);
    CHECK(s.state().document.instances == initial.document.instances);
    CHECK_FALSE(s.dirty()); CHECK_FALSE(s.can_undo());
    CHECK(s.state().document.revision > worker.document.revision);
    auto restored = s.take_changes(); REQUIRE(restored);
    CHECK(restored->changes.properties == changes.properties);
    s.abandon_remote();
    CHECK_FALSE(s.can_undo());
}

TEST_CASE("World bounds edits are compact undoable saved scene values", "[editor][session][bounds]") {
    Temp temp; EditingSession s{scene()}; s.select_keyframe(s.state().viewport.time);
    auto worker = s.state();
    const auto original = s.state().document.world_bounds;
    const auto* storage = mesh_storage(s.state());
    const auto sequence = s.viewport().sequence;
    auto bounds = original;
    REQUIRE(s.begin_world_bounds());
    for (unsigned i=0;i<100;++i) { bounds.maximum.x += 1; REQUIRE(s.world_bounds(bounds)); }
    CHECK(mesh_storage(s.state()) == storage);
    CHECK(s.viewport().sequence == sequence);
    auto notice = s.take_changes(); REQUIRE(notice);
    CHECK(notice->changes.world_bounds); CHECK_FALSE(notice->changes.full);
    CHECK(notice->changes.properties.empty()); CHECK(notice->changes.vertices.empty());
    const auto patch = capture_patch(worker.document.revision,s.state(),notice->changes); REQUIRE(patch);
    auto wire = encode_patch(*patch); REQUIRE(wire); CHECK(wire->size() < 100);
    auto decoded = decode_patch(*wire); REQUIRE(decoded); CHECK(*decoded == *patch);
    REQUIRE(editor_example::apply_patch(worker,*decoded)); CHECK(worker.document.world_bounds == bounds);
    REQUIRE(s.commit()); REQUIRE(s.undo()); CHECK(s.state().document.world_bounds == original);
    CHECK_FALSE(s.can_undo()); CHECK_FALSE(s.dirty());
    REQUIRE(s.redo()); CHECK(s.state().document.world_bounds == bounds);
    REQUIRE(s.save_as(temp.path / "bounds.vscene"));
    auto loaded = load_scene(temp.path / "bounds.vscene"); REQUIRE(loaded);
    CHECK(loaded->document.world_bounds == bounds);
    REQUIRE(s.begin_world_bounds());
    auto invalid = bounds; invalid.minimum.x = invalid.maximum.x;
    CHECK_FALSE(s.world_bounds(invalid)); CHECK(s.state().document.world_bounds == bounds);
    REQUIRE(s.world_bounds(original)); REQUIRE(s.cancel());
    CHECK(s.state().document.world_bounds == bounds); CHECK_FALSE(s.dirty());
    auto bytes = encode(s.state()); REQUIRE(bytes);
    const auto start = bytes->find("world_bounds ="); const auto end = bytes->find('\n',start);
    REQUIRE(start != std::string::npos); bytes->erase(start,end-start+1);
    auto old = decode(*bytes); REQUIRE(old); CHECK(old->document.world_bounds == WorldBounds{});
}

TEST_CASE("A formation crosses the old position wall without losing tracks spacing or persistence",
          "[editor][session][position][regression]") {
    auto initial = scene();
    instance_transform(initial,1)->position = {5,2,-80};
    instance_transform(initial,2)->position = {-7,1,-98};
    const auto third = instantiate(initial,BlueprintId::mesh); REQUIRE(third);
    instance_transform(initial,*third)->position = {12,3,-90};
    REQUIRE(key_property(initial,{1,"position"},0,Vec3{5,2,-80},timeline::Interpolation::hold));
    REQUIRE(key_property(initial,{1,"position"},4,Vec3{5,2,-84}));
    initial.viewport.time=0;
    const auto original_tracks = initial.document.timeline;
    auto worker = initial;
    EditingSession session{std::move(initial)}; session.select_keyframe(session.state().viewport.time);
    const std::array<u32,3> selection{1,2,*third};
    const auto* geometry = mesh_storage(session.state());
    REQUIRE(session.begin_move(1,selection));
    REQUIRE(session.move({5,2,-120})); // The rear member crosses -100 before the primary.
    auto changes=session.take_changes(); REQUIRE(changes);
    CHECK_FALSE(changes->changes.full); CHECK(changes->changes.properties.size()==3);
    auto patch=capture_patch(worker.document.revision,session.state(),changes->changes); REQUIRE(patch);
    auto bytes=encode_patch(*patch); REQUIRE(bytes); CHECK(bytes->size()<1024);
    auto decoded=decode_patch(*bytes); REQUIRE(decoded);
    REQUIRE(editor_example::apply_patch(worker,*decoded));
    for (auto id:selection) {
        CHECK(evaluate_instance(worker,*find_instance(worker,id),0)==
              evaluate_instance(session.state(),*find_instance(session.state(),id),0));
    }
    CHECK(evaluate_instance(session.state(),*find_instance(session.state(),1),0).transform.position == Vec3{5,2,-120});
    CHECK(instance_transform(session.state(),2)->position == Vec3{-7,1,-138});
    CHECK(instance_transform(session.state(),*third)->position == Vec3{12,3,-130});
    CHECK(instance_transform(session.state(),1)->position == Vec3{5,2,-80}); // keyed edit, base unchanged
    CHECK(session.state().document.timeline.find({1,"position"})->keys[0].incoming==timeline::Interpolation::hold);
    CHECK(session.state().document.timeline.find({1,"position"})->keys[1]==original_tracks.find({1,"position"})->keys[1]);
    CHECK(mesh_storage(session.state())==geometry);
    REQUIRE(session.commit());
    Temp temp; REQUIRE(session.save_as(temp.path/"formation.vscene"));
    auto loaded=load_scene(temp.path/"formation.vscene"); REQUIRE(loaded);
    CHECK(loaded->document.instances==session.state().document.instances);
    CHECK(loaded->document.timeline==session.state().document.timeline);
    REQUIRE(session.undo());
    CHECK(instance_transform(session.state(),2)->position.z == -98.F);
    CHECK(session.state().document.timeline==original_tracks);
    REQUIRE(session.redo()); CHECK_FALSE(session.dirty());
    REQUIRE(session.begin_move(1,selection)); REQUIRE(session.move({5,2,-200})); REQUIRE(session.cancel());
    CHECK_FALSE(session.dirty()); CHECK(instance_transform(session.state(),2)->position.z==-138.F);
}

TEST_CASE("Session document edits do not manufacture viewport changes", "[editor][session][timeline]") {
    EditingSession s{scene()}; s.select_keyframe(s.state().viewport.time);
    s.viewport().paused = true;
    const auto time = s.viewport().time;
    const auto sequence = s.viewport().sequence;
    REQUIRE(s.add_keyframe(time));
    CHECK(s.viewport().sequence == sequence);
    const auto values = keyframe_values(s.state(), time);
    REQUIRE(s.update_keyframe(time, time, "Arrival", values));
    CHECK(s.viewport().sequence == sequence);
    REQUIRE(s.add_keyframe(time + 1));
    CHECK(s.viewport().sequence == sequence + 1);
    CHECK(s.viewport().time == time + 1);
}

TEST_CASE("Session separates content identity from transport revisions and navigation", "[editor][session]") {
    Temp temp;
    EditingSession s{scene()}; s.select_keyframe(s.state().viewport.time);
    const auto file = temp.path / "scene.vscene";
    REQUIRE(s.save_as(file));
    const auto initial_revision = s.state().document.revision;
    s.viewport().editor_camera.yaw = 62; ++s.viewport().sequence;
    CHECK(s.state().document.revision == initial_revision);
    CHECK_FALSE(s.dirty()); CHECK_FALSE(s.take_changes());
    REQUIRE(s.begin_scale(1)); REQUIRE(s.scale(1.5F));
    CHECK(s.dirty()); CHECK_FALSE(s.can_undo()); CHECK_FALSE(s.save());
    REQUIRE(s.commit()); REQUIRE(s.save()); CHECK_FALSE(s.dirty());
    REQUIRE(s.begin_rotation(1)); REQUIRE(s.rotate({1,2,3})); REQUIRE(s.commit());
    CHECK(s.dirty());
    const auto revision = s.state().document.revision;
    REQUIRE(s.undo()); CHECK_FALSE(s.dirty());
    CHECK(s.state().document.revision > revision);
    CHECK(s.viewport().editor_camera.yaw == 62.F);
    REQUIRE(s.redo()); CHECK(s.dirty());
    REQUIRE(s.undo()); CHECK_FALSE(s.dirty());
    REQUIRE(s.begin_scale(1)); REQUIRE(s.scale(2)); REQUIRE(s.scale(1.5F));
    CHECK_FALSE(s.dirty()); REQUIRE_FALSE(*s.commit());
    CHECK(s.can_redo());
    REQUIRE(s.begin_scale(1)); REQUIRE(s.scale(2)); REQUIRE(s.cancel());
    CHECK_FALSE(s.dirty()); CHECK(s.can_redo());
    CHECK_FALSE(s.save_as(temp.path / "missing" / "scene.vscene"));
    CHECK(s.path() == file); CHECK_FALSE(s.dirty());
    REQUIRE(s.load(file)); CHECK_FALSE(s.dirty()); CHECK_FALSE(s.can_undo()); CHECK_FALSE(s.can_redo());
}

TEST_CASE("Session vertex gestures own draft publication and exact cancellation", "[editor][session][draft]") {
    EditingSession s{scene()}; s.select_keyframe(s.state().viewport.time);
    const auto* storage = mesh_storage(s.state());
    const std::array<u32,2> vertices{0,1};
    REQUIRE(s.begin_vertices(BlueprintId::mesh, vertices));
    REQUIRE_FALSE(*s.move_vertices({})); REQUIRE_FALSE(*s.commit());
    CHECK_FALSE(s.state().document.mesh_drafts.contains(BlueprintId::mesh));
    REQUIRE(s.begin_vertices(BlueprintId::mesh, vertices));
    REQUIRE(s.move_vertices({1,2,3}));
    CHECK(s.take_changes()->changes.full); // only first movement creates the draft
    REQUIRE(s.move_vertices({2,2,3}));
    auto motion = s.take_changes(); REQUIRE(motion); CHECK_FALSE(motion->changes.full);
    CHECK(motion->changes.vertices.at(1).size() == 2);
    CHECK(mesh_storage(s.state()) == storage);
    CHECK(mesh_geometry(s.state(),BlueprintId::mesh)->position(0) == Vec3{});
    REQUIRE(s.cancel()); CHECK_FALSE(s.dirty()); CHECK_FALSE(s.can_undo());
    CHECK_FALSE(s.state().document.mesh_drafts.contains(BlueprintId::mesh));
    CHECK(s.take_changes()->changes.full);
    REQUIRE(s.translate_vertices(BlueprintId::mesh,vertices,{1,0,0}));
    REQUIRE(s.undo()); CHECK_FALSE(s.state().document.mesh_drafts.contains(BlueprintId::mesh));
    REQUIRE(s.redo()); CHECK(mesh_edit_geometry(s.state(),BlueprintId::mesh)->position(0) == Vec3{1,0,0});
    REQUIRE(s.begin_vertices(BlueprintId::mesh,vertices));
    CHECK_FALSE(s.move_vertices({std::numeric_limits<f32>::quiet_NaN(),0,0}));
    REQUIRE(s.move_vertices({3,0,0})); REQUIRE(s.cancel());
    CHECK(mesh_edit_geometry(s.state(),BlueprintId::mesh)->position(0) == Vec3{1,0,0});
    REQUIRE(s.apply_mesh(BlueprintId::mesh));
    CHECK(mesh_geometry(s.state(),BlueprintId::mesh)->position(0) == Vec3{1,0,0});
    REQUIRE(s.undo());
    CHECK(mesh_geometry(s.state(),BlueprintId::mesh)->position(0) == Vec3{});
    CHECK(mesh_edit_geometry(s.state(),BlueprintId::mesh)->position(0) == Vec3{1,0,0});
}

TEST_CASE("Returning animated gestures to their sampled starting value restores exact tracks", "[editor][session][animation]") {
    auto initial = scene();
    REQUIRE(key_property(initial,{1,"scale"},0,.5F));
    REQUIRE(key_property(initial,{1,"scale"},4,1.5F));
    REQUIRE(key_property(initial,{1,"position"},0,Vec3{}));
    REQUIRE(key_property(initial,{1,"position"},4,Vec3{4,0,0}));
    REQUIRE(key_property(initial,{1,"rotation"},0,Vec3{}));
    REQUIRE(key_property(initial,{1,"rotation"},4,Vec3{4,0,0}));
    initial.viewport.time=2;
    initial.document.keyframe_names[2] = "Editable pose";
    EditingSession s{std::move(initial)}; s.select_keyframe(s.state().viewport.time);
    const auto tracks = s.state().document.timeline;
    REQUIRE(s.begin_scale(1)); REQUIRE(s.scale(2)); REQUIRE(s.scale(1)); REQUIRE_FALSE(*s.commit());
    REQUIRE(s.begin_rotation(1)); REQUIRE(s.rotate({4,0,0})); REQUIRE(s.rotate({2,0,0})); REQUIRE_FALSE(*s.commit());
    REQUIRE(s.begin_move(1,{})); REQUIRE(s.move({4,0,0})); REQUIRE(s.move({2,0,0})); REQUIRE_FALSE(*s.commit());
    CHECK(s.state().document.timeline == tracks);
    CHECK_FALSE(s.can_undo()); CHECK_FALSE(s.dirty());
    REQUIRE(s.begin_scale(1)); s.viewport().time=3;
    CHECK_FALSE(s.scale(2)); REQUIRE(s.cancel());
    CHECK(s.state().document.timeline == tracks);
}

TEST_CASE("Worker results enter the same history but cannot overwrite authoring or mesh drafts", "[editor][session][remote]") {
    EditingSession s{scene()}; s.select_keyframe(s.state().viewport.time);
    const auto* storage = mesh_storage(s.state());
    DocumentPatch result{1,2,{},{{{2,"bloom"},.6F,{}}}};
    CHECK_FALSE(s.accept_remote(7,result));
    REQUIRE(s.begin_remote(7));
    CHECK_FALSE(s.begin_scale(1)); CHECK_FALSE(s.undo()); CHECK_FALSE(s.save());
    CHECK_FALSE(s.accept_remote(8,result));
    auto malformed=result; malformed.properties[0].base=4.F;
    CHECK_FALSE(s.accept_remote(7,malformed)); CHECK(s.state().document.revision==1);
    auto geometry=result; geometry.vertices.push_back({1,2,{{0,{9,0,0}}},1});
    CHECK_FALSE(s.accept_remote(7,geometry));
    CHECK(s.state().document.mesh.position(0)==Vec3{});
    s.viewport().editor_camera.yaw=70; ++s.viewport().sequence;
    REQUIRE(s.accept_remote(7,result)); CHECK_FALSE(s.awaiting_remote());
    CHECK(s.dirty()); CHECK(s.can_undo()); CHECK(mesh_storage(s.state())==storage);
    CHECK(s.viewport().editor_camera.yaw==70.F);
    auto notice=s.take_changes(); REQUIRE(notice); CHECK_FALSE(notice->changes.full);
    CHECK(notice->changes.properties==std::set<timeline::Target,TargetLess>{{2,"bloom"}});
    REQUIRE(s.undo()); CHECK(sun_settings(s.state(),2)->bloom==.24F); CHECK_FALSE(s.dirty());
    REQUIRE(s.redo()); CHECK(sun_settings(s.state(),2)->bloom==.6F);
    REQUIRE(s.begin_remote(9));
    result.base_revision=s.state().document.revision; result.revision=result.base_revision+1;
    REQUIRE_FALSE(*s.accept_remote(9,result)); // ACK advances revision, content is unchanged
    REQUIRE(s.undo()); CHECK(sun_settings(s.state(),2)->bloom==.24F);
}

TEST_CASE("Session publishes one compound edit and rejects invalid transforms atomically", "[editor][session]") {
    EditingSession s{scene()}; s.select_keyframe(s.state().viewport.time);
    auto before=*instance_transform(s.state(),1);
    CHECK_FALSE(s.set_transform(1,{30,0,0},max_instance_scale+1.F));
    CHECK(*instance_transform(s.state(),1)==before); CHECK_FALSE(s.take_changes()); CHECK_FALSE(s.dirty());
    REQUIRE(s.set_transform(1,{30,0,0},1.5F));
    CHECK(s.take_changes()->changes.properties.size()==2);
    REQUIRE(s.undo()); CHECK(*instance_transform(s.state(),1)==before);
    REQUIRE(s.redo()); CHECK(instance_transform(s.state(),1)->scale==1.5F);
    editor::Event event;
    event.stamp={1,7,s.state().document.revision,1}; event.control="transform";
    event.values={{"scale",2.F},{"rotation",Vec3{60,0,0}}};
    auto stale=event; --stale.stamp.revision;
    CHECK_FALSE(apply_instance_control(s,stale,7,1));
    CHECK_FALSE(apply_instance_control(s,event,8,1));
    CHECK_FALSE(apply_instance_control(s,event,7,2));
    REQUIRE(apply_instance_control(s,event,7,1));
    CHECK(instance_transform(s.state(),1)->scale==2.F);
}

TEST_CASE("Session IDs remain monotonic and sparse history stays bounded", "[editor][session]") {
    EditingSession s{scene()}; s.select_keyframe(s.state().viewport.time);
    auto first=s.instantiate(BlueprintId::mesh); REQUIRE(first);
    REQUIRE(s.undo()); auto second=s.instantiate(BlueprintId::mesh); REQUIRE(second); CHECK(*second>*first);
    for(unsigned i=0;i<70;++i) {
        REQUIRE(s.begin_scale(1)); REQUIRE(s.scale(.1F+static_cast<f32>(i)*.02F)); REQUIRE(s.commit());
    }
    unsigned undos{};
    while(s.can_undo()) {REQUIRE(s.undo());++undos;}
    CHECK(undos==64);
    CHECK_FALSE(*s.undo());
}
