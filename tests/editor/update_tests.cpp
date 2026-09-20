#include "../../examples/editor/preview_updates.hpp"
#include "../../examples/editor/project.hpp"
#include "../../examples/editor/animation_camera_edit.hpp"
#include "../../examples/editor/document_patch.hpp"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <limits>

namespace {
using namespace vng;
using namespace editor_example;
State source() {
    content::vmesh::Document d;
    d.vertex_count = 3;
    d.vertex_fields = {{"position",
                        {content::vmesh::ScalarType::Float32, 3},
                        std::vector<f32>{0, 0, 0, 1, 0, 0, 0, 1, 0}}};
    d.faces = {{0, 1, 2}};
    auto mesh = editor::EditableMesh::create(std::move(d));
    REQUIRE(mesh);
    return {.document = {.mesh = std::move(*mesh)}, .viewport = {.selected_object = 1}};
}
std::string packet(PreviewUpdates& updates, u64 generation, const State& state) {
    auto next = updates.next(generation, state);
    REQUIRE(next);
    REQUIRE(*next);
    return std::move(**next);
}
void no_packet(PreviewUpdates& updates, u64 generation, const State& state) {
    auto next = updates.next(generation, state);
    REQUIRE(next);
    REQUIRE_FALSE(*next);
}
} // namespace
TEST_CASE("Editor backpressure coalesces latest absolute positions without losing the final edit") {
    auto state = source();
    PreviewUpdates updates;
    updates.add(1);
    REQUIRE(packet(updates, 1, state).starts_with("snapshot\n"));
    const std::array<u32, 1> vertex{0};
    for (u32 i = 1; i <= 100; ++i) {
        REQUIRE(state.document.mesh.set_position(0, {static_cast<f32>(i), 0, 0}));
        ++state.document.revision;
        updates.changed(vertex);
        no_packet(updates, 1, state);
    }
    updates.acknowledge(1, 1);
    const auto wire = packet(updates, 1, state);
    REQUIRE(wire.starts_with("vertices\n"));
    REQUIRE(wire.size() == 53);
    auto edit = decode_edit(std::string_view(wire).substr(9));
    REQUIRE(edit);
    REQUIRE(edit->base_revision == 1);
    REQUIRE(edit->revision == 101);
    REQUIRE(edit->vertices == std::vector<VertexPosition>{{0, {100, 0, 0}}});
    // Return to the original coordinate while that packet is still in flight.
    REQUIRE(state.document.mesh.set_position(0, {0, 0, 0}));
    ++state.document.revision;
    updates.changed(vertex);
    updates.acknowledge(1, 100);
    no_packet(updates, 1, state); // stale ack
    updates.acknowledge(1, 101);
    auto final = packet(updates, 1, state);
    auto last = decode_edit(std::string_view(final).substr(9));
    REQUIRE(last);
    REQUIRE(last->base_revision == 101);
    REQUIRE(last->vertices[0].position == Vec3{});
    updates.acknowledge(1, 102);
    REQUIRE(updates.ready(1, 102));
    no_packet(updates, 1, state);
}

TEST_CASE("Transform coalescing keeps property identity across ACKs and mixed pending changes") {
    auto state = source();
    PreviewUpdates updates;
    updates.add(1);
    packet(updates, 1, state);
    updates.acknowledge(1, state.document.revision);
    REQUIRE(apply_rotation_value(state, 1, {10, 20, 30}));
    ++state.document.revision;
    updates.rotation_changed(1);
    const auto first = packet(updates, 1, state);
    REQUIRE(first.starts_with("rotation\n"));
    REQUIRE(decode_rotation_edit(std::string_view(first).substr(9)));
    // Another property may follow an in-flight packet without a full scene:
    // it will be based on that packet's ACK, never its pre-edit revision.
    REQUIRE(apply_position_value(state, 1, {1, 2, 3}));
    ++state.document.revision;
    updates.position_changed(1);
    no_packet(updates, 1, state);
    updates.acknowledge(1, 2);
    const auto next = packet(updates, 1, state);
    REQUIRE(next.starts_with("position\n"));
    auto translated = decode_position_edit(std::string_view(next).substr(9));
    REQUIRE(translated);
    CHECK(translated->base_revision == 2);
    CHECK(translated->revision == 3);
    updates.acknowledge(1, 3);
    ++state.document.revision;
    updates.position_changed(1);
    updates.rotation_changed(1);
    CHECK(packet(updates, 1, state).starts_with("patch\n"));
    updates.acknowledge(1, state.document.revision);
    CHECK(updates.ready(1, state.document.revision));
}

TEST_CASE("Vertex update coalescing does not mix independently editable mesh blueprints") {
    auto state = source();
    const auto blueprint = static_cast<BlueprintId>(3);
    state.document.mesh_assets.push_back({blueprint, "Second mesh", state.document.mesh, {}});
    state.document.next_blueprint_id = 4;
    const auto created = instantiate(state, blueprint);
    REQUIRE(created);
    PreviewUpdates updates;
    updates.add(1);
    REQUIRE(packet(updates, 1, state).starts_with("snapshot\n"));
    updates.acknowledge(1, state.document.revision);
    const std::array<u32, 1> indices{0};
    REQUIRE(editable_mesh(state)->set_position(0, {2, 3, 4}));
    ++state.document.revision;
    updates.changed(indices, 3);
    const auto patch = packet(updates, 1, state);
    REQUIRE(patch.starts_with("vertices\n"));
    auto decoded = decode_edit(std::string_view(patch).substr(9));
    REQUIRE(decoded);
    CHECK(decoded->blueprint == 3);
    CHECK(decoded->vertices[0].position == Vec3{2, 3, 4});
    updates.acknowledge(1, state.document.revision);
    REQUIRE(editable_mesh(state)->set_position(0, {3, 4, 5}));
    ++state.document.revision;
    updates.changed(indices, 3);
    REQUIRE(state.document.mesh.set_position(0, {6, 7, 8}));
    ++state.document.revision;
    updates.changed(indices, 1);
    const auto combined = packet(updates, 1, state);
    REQUIRE(combined.starts_with("patch\n"));
    const auto decoded_patch = decode_patch(std::string_view(combined).substr(6));
    REQUIRE(decoded_patch);
    REQUIRE(decoded_patch->vertices.size() == 2);
    CHECK(decoded_patch->vertices[0].blueprint == 1);
    CHECK(decoded_patch->vertices[0].vertices[0].position == Vec3{6, 7, 8});
    CHECK(decoded_patch->vertices[1].blueprint == 3);
    CHECK(decoded_patch->vertices[1].vertices[0].position == Vec3{3, 4, 5});
}
TEST_CASE("Editor update baselines are per generation and full changes supersede queued patches") {
    auto state = source();
    PreviewUpdates updates;
    updates.add(1);
    packet(updates, 1, state);
    updates.acknowledge(1, 1);
    updates.add(2);
    packet(updates, 2, state);
    state.document.revision = 2;
    REQUIRE(state.document.mesh.set_position(1, {2, 0, 0}));
    updates.changed(std::array<u32, 1>{1});
    REQUIRE(packet(updates, 1, state).starts_with("vertices\n"));
    no_packet(updates, 2, state);
    state.document.revision = 3;
    state.document.animation_camera.yaw = 24;
    updates.changed();
    updates.acknowledge(1, 2);
    REQUIRE(packet(updates, 1, state).starts_with("snapshot\n"));
    updates.acknowledge(2, 1);
    REQUIRE(packet(updates, 2, state).starts_with("snapshot\n"));
    updates.reset(1);
    REQUIRE(packet(updates, 1, state).starts_with("snapshot\n"));
    updates.acknowledge(1, 3);
    state.document.revision = 2;
    updates.changed();
    REQUIRE_FALSE(updates.next(1, state));
    updates.remove(1);
    no_packet(updates, 1, state);
    updates.accepted(2, 4);
    REQUIRE(updates.ready(2, 4));
}
TEST_CASE("Blueprint view changes send an explicit target and inactive updates never target the cube") {
    auto state = source();
    const auto imported = static_cast<BlueprintId>(3);
    state.document.mesh_assets.push_back({imported, "Imported", state.document.mesh, {}});
    state.document.next_blueprint_id = 4;
    PreviewUpdates updates;
    updates.add(1);
    packet(updates, 1, state);
    updates.acknowledge(1, state.document.revision);

    REQUIRE(inspect_mesh(state, imported));
    ++state.document.revision;
    updates.changed();
    const auto wire = packet(updates, 1, state);
    REQUIRE(wire.starts_with("snapshot\n"));
    const auto decoded = decode(std::string_view(wire).substr(9));
    REQUIRE(decoded);
    CHECK(decoded->viewport.inspected_mesh == imported);
    CHECK(decoded->viewport.selected_object == 1);
    CHECK(mesh_target(*decoded)->blueprint == imported);
    updates.acknowledge(1, state.document.revision);

    state.viewport.mode = ViewMode::scene;
    state.viewport.selected_object = 2;
    ++state.document.revision;
    updates.changed();
    REQUIRE(packet(updates, 1, state).starts_with("snapshot\n"));
    updates.acknowledge(1, state.document.revision);
    ++state.document.revision; // A revision-only update has no geometry target.
    CHECK_FALSE(editable_mesh(state));
    CHECK(packet(updates, 1, state).starts_with("snapshot\n"));
}
TEST_CASE(
    "Completed preview frames advance during continuous edits without crossing view changes") {
    u64 presented = 1;
    for (u64 authored = 2; authored < 100; ++authored) {
        const u64 completed = authored - 1;
        REQUIRE(accepts_completed_revision(completed, 1, presented, authored));
        presented = completed;
    }
    REQUIRE_FALSE(accepts_completed_revision(97, 1, presented, 100));
    REQUIRE_FALSE(accepts_completed_revision(99, 100, presented, 100)); // changed camera
    REQUIRE(accepts_completed_revision(100, 100, presented, 100));
    REQUIRE_FALSE(accepts_completed_revision(101, 100, presented, 100));
}
TEST_CASE("Rejected full updates unblock on corrected authoring without retry storms") {
    auto state = source();
    PreviewUpdates updates;
    updates.add(1);
    packet(updates, 1, state);
    updates.reject(1);
    for (int i = 0; i < 20; ++i)
        no_packet(updates, 1, state);
    updates.acknowledge(1, 1);
    no_packet(updates, 1, state);
    ++state.document.revision;
    updates.changed(std::array<u32, 1>{0});
    REQUIRE(packet(updates, 1, state).starts_with("snapshot\n"));
    updates.acknowledge(1, state.document.revision);
    REQUIRE(updates.ready(1, state.document.revision));
    updates.reject(1);
    updates.reset(1);
    REQUIRE(packet(updates, 1, state).starts_with("snapshot\n"));
}
TEST_CASE("Batch vertex projection matches scalar projection") {
    auto state = source();
    state.viewport.selected_object = 1;
    state.viewport.mode = ViewMode::mesh;
    const auto points = project_vertices(state, {640, 480});
    REQUIRE(points.size() == state.document.mesh.size());
    for (u32 i = 0; i < points.size(); ++i)
        REQUIRE(points[i] == project_vertex(state, i, {640, 480}));
    (*editor_example::mesh_settings(state, 1)).visible = false;
    // Blueprint inspection is independent of instance visibility.
    CHECK(project_vertices(state, {640, 480}) == points);
    state.viewport.mode = ViewMode::scene;
    for (const auto& point : project_vertices(state, {640, 480}))
        REQUIRE_FALSE(point);
}

TEST_CASE("Camera navigation coalesces complete latest poses behind one in-flight update") {
    auto state = source();
    PreviewUpdates updates;
    updates.add(1);
    REQUIRE(packet(updates, 1, state).starts_with("snapshot\n"));
    for (u32 i = 1; i <= 100; ++i) {
        ++state.document.revision;
        state.document.animation_camera.yaw = static_cast<f32>(i) * 0.4F;
        state.document.animation_camera.pitch = static_cast<f32>(i) * 0.1F;
        state.document.animation_camera.target = {static_cast<f32>(i) * 0.02F, -2, 3};
        updates.camera_changed();
        no_packet(updates, 1, state);
    }
    updates.acknowledge(1, 1);
    const auto wire = packet(updates, 1, state);
    REQUIRE(wire.starts_with("animation_camera\n"));
    REQUIRE(wire.size() == 69);
    auto edit = decode_animation_camera_edit(std::string_view(wire).substr(17));
    REQUIRE(edit);
    CHECK(edit->base_revision == 1);
    CHECK(edit->revision == 101);
    CHECK(edit->pose.yaw == state.document.animation_camera.yaw);
    CHECK(edit->pose.pitch == state.document.animation_camera.pitch);
    CHECK(edit->pose.target == state.document.animation_camera.target);
    CHECK_FALSE(updates.ready(1, state.document.revision));
    ++state.document.revision;
    state.document.animation_camera.target = {};
    updates.camera_changed();
    updates.acknowledge(1, 100);
    no_packet(updates, 1, state);
    updates.acknowledge(1, 101);
    const auto final = packet(updates, 1, state);
    auto last = decode_animation_camera_edit(std::string_view(final).substr(17));
    REQUIRE(last);
    CHECK(last->base_revision == 101);
    CHECK(last->revision == 102);
    CHECK(last->pose.target == Vec3{});
    updates.acknowledge(1, 102);
    CHECK(updates.ready(1, 102));
    updates.acknowledge(1, 0); // Zero means no packet in flight, never a valid acknowledgement.
    CHECK(updates.ready(1, 102));
    no_packet(updates, 1, state);
}

TEST_CASE("Mixed authored camera and vertex changes stay one atomic patch") {
    for (const bool camera_first : {false, true}) {
        auto state = source();
        PreviewUpdates updates;
        updates.add(1);
        packet(updates, 1, state);
        updates.acknowledge(1, 1);
        auto move_camera = [&] {
            state.document.animation_camera.yaw = 60;
            state.document.animation_camera.target = {2, 3, 4};
            ++state.document.revision;
            updates.camera_changed();
        };
        auto move_vertex = [&] {
            REQUIRE(state.document.mesh.set_position(0, {1, 2, 3}));
            ++state.document.revision;
            updates.changed(std::array<u32, 1>{0});
        };
        if (camera_first) { move_camera(); move_vertex(); }
        else { move_vertex(); move_camera(); }
        const auto wire = packet(updates, 1, state);
        REQUIRE(wire.starts_with("patch\n"));
        auto decoded = decode_patch(std::string_view(wire).substr(6));
        REQUIRE(decoded);
        auto worker = source();
        REQUIRE(apply_patch(worker, *decoded));
        CHECK(worker.document.revision == 3);
        CHECK(worker.document.animation_camera == state.document.animation_camera);
        CHECK(worker.document.mesh.position(0) == state.document.mesh.position(0));
        no_packet(updates, 1, state);
        updates.acknowledge(1, 3);
        CHECK(updates.ready(1, 3));
    }
}

TEST_CASE("Queued vertex updates preserve regions and world bounds across acknowledgements") {
    // Exercise each formerly dropped category independently, and both together.
    for (const unsigned categories : {1U, 2U, 3U}) {
        for (const bool vertices_first : {false, true}) {
            CAPTURE(categories, vertices_first);
            const bool with_regions = categories & 1U;
            const bool with_bounds = categories & 2U;
            auto state = source();
            const auto region = instantiate(state, BlueprintId::region);
            REQUIRE(region);
            PreviewUpdates updates;
            updates.add(1);
            const auto snapshot = packet(updates, 1, state);
            REQUIRE(snapshot.starts_with("snapshot\n"));
            auto worker = decode(std::string_view(snapshot).substr(9));
            REQUIRE(worker);
            const auto initial_revision = state.document.revision;

            const auto edit_vertices = [&](f32 coordinate) {
                REQUIRE(state.document.mesh.set_position(0, {coordinate, 2, 3}));
                ++state.document.revision;
                updates.changed(std::array<u32, 1>{0});
            };
            const auto edit_other_categories = [&](f32 coordinate) {
                if (with_regions) {
                    auto* settings = region_settings(state, *region);
                    REQUIRE(settings);
                    settings->boundary.points[0].x = coordinate;
                    settings->note = "Edited boundary " + std::to_string(coordinate);
                    ++state.document.revision;
                    updates.changed(DocumentChanges{.regions = {{*region, {.whole = true}}}});
                }
                if (with_bounds) {
                    state.document.world_bounds.maximum = {100 + coordinate, 120, 130};
                    ++state.document.revision;
                    updates.changed(DocumentChanges{.world_bounds = true});
                }
            };
            const auto edit_both = [&](f32 coordinate) {
                if (vertices_first) {
                    edit_vertices(coordinate);
                    edit_other_categories(coordinate);
                } else {
                    edit_other_categories(coordinate);
                    edit_vertices(coordinate);
                }
            };
            const auto apply_mixed = [&](const std::string& wire, u64 base, const State& expected) {
                REQUIRE(wire.starts_with("patch\n"));
                const auto patch = decode_patch(std::string_view(wire).substr(6));
                REQUIRE(patch);
                CHECK(patch->base_revision == base);
                CHECK(patch->revision == expected.document.revision);
                REQUIRE(patch->vertices.size() == 1);
                CHECK(!patch->regions.empty() == with_regions);
                CHECK(patch->world_bounds.has_value() == with_bounds);
                REQUIRE(apply_patch(*worker, *patch));
                CHECK(worker->document.revision == expected.document.revision);
                CHECK(worker->document.mesh.position(0) == expected.document.mesh.position(0));
                CHECK(region_snapshot(*worker) == region_snapshot(expected));
                CHECK(worker->document.world_bounds == expected.document.world_bounds);
            };

            // The initial full snapshot is still in flight: coalesce complete
            // absolute values without sending or losing any of the categories.
            edit_both(4);
            edit_both(5);
            no_packet(updates, 1, state);
            updates.acknowledge(1, initial_revision);
            const auto first = packet(updates, 1, state);
            const auto first_state = state;

            // Keep editing while the mixed packet awaits its ACK. The already
            // encoded packet must still apply its original values atomically.
            edit_both(6);
            edit_both(7);
            no_packet(updates, 1, state);
            apply_mixed(first, initial_revision, first_state);
            updates.acknowledge(1, initial_revision); // stale ACK cannot release the queue
            no_packet(updates, 1, state);
            updates.acknowledge(1, first_state.document.revision);
            apply_mixed(packet(updates, 1, state), first_state.document.revision, state);
            const auto mixed_revision = state.document.revision;
            updates.acknowledge(1, mixed_revision);
            CHECK(updates.ready(1, mixed_revision));
            no_packet(updates, 1, state);

            // Region/bounds dirtiness must not leak into subsequent vertex-only
            // work: preserve the small packet optimization after convergence.
            edit_vertices(8);
            const auto compact = packet(updates, 1, state);
            REQUIRE(compact.starts_with("vertices\n"));
            const auto edit = decode_edit(std::string_view(compact).substr(9));
            REQUIRE(edit);
            CHECK(edit->base_revision == mixed_revision);
            REQUIRE(apply_edit(*worker, *edit));
            CHECK(worker->document.mesh.position(0) == state.document.mesh.position(0));
            CHECK(region_snapshot(*worker) == region_snapshot(state));
            CHECK(worker->document.world_bounds == state.document.world_bounds);
            CHECK(worker->document.revision == state.document.revision);
            updates.acknowledge(1, state.document.revision);
            CHECK(updates.ready(1, state.document.revision));
            no_packet(updates, 1, state);
        }
    }
}

TEST_CASE("Animated camera navigation coalesces four tracks instead of whole scene snapshots") {
    auto state=source();
    REQUIRE(key_camera(state,0,{0,0,8,{}}));
    REQUIRE(key_camera(state,5,{10,10,6,{1,2,3}}));
    REQUIRE(key_property(state,{camera_animation_object,"yaw"},5,10.F,timeline::Interpolation::hold));
    auto worker=state;
    PreviewUpdates updates;
    updates.add(1); updates.add(2);
    REQUIRE(packet(updates,1,state).starts_with("snapshot\n"));
    REQUIRE(packet(updates,2,state).starts_with("snapshot\n"));
    updates.acknowledge(1,1); updates.acknowledge(2,1);
    for(int i=1;i<=100;++i) {
        REQUIRE(key_camera(state,5,{static_cast<f32>(i),10,6,{1,2,3}}));
        ++state.document.revision;
        updates.camera_tracks_changed();
    }
    const auto first=packet(updates,1,state);
    REQUIRE(first.starts_with("animation_camera\n"));
    CHECK(first.size()<512);
    auto edit=decode_animation_camera_edit(std::string_view(first).substr(17));
    REQUIRE(edit); REQUIRE(edit->tracks);
    REQUIRE(apply_animation_camera_edit(worker.document,*edit));
    CHECK(evaluate_camera(worker,5).yaw==100.F);
    CHECK(evaluate_camera(worker,4.99F).yaw==0.F);
    no_packet(updates,1,state);

    // Unsent keys at two different timestamps must both survive; a later base
    // pose edit cannot downgrade the pending packet into a pose-only packet.
    REQUIRE(key_camera(state,6,{70,12,7,{2,3,4}}));
    ++state.document.revision; updates.camera_tracks_changed();
    REQUIRE(key_camera(state,7,{60,15,8,{3,4,5}}));
    ++state.document.revision; updates.camera_tracks_changed();
    state.document.animation_camera.yaw=45;
    ++state.document.revision; updates.camera_changed();
    no_packet(updates,1,state);
    updates.acknowledge(1,100); no_packet(updates,1,state);
    updates.acknowledge(1,101);
    const auto final=packet(updates,1,state);
    edit=decode_animation_camera_edit(std::string_view(final).substr(17));
    REQUIRE(edit); REQUIRE(edit->tracks);
    CHECK(edit->base_revision==101);
    CHECK(edit->revision==104);
    REQUIRE(apply_animation_camera_edit(worker.document,*edit));
    CHECK(worker.document.animation_camera.yaw==45.F);
    CHECK(evaluate_camera(worker,6).yaw==70.F);
    CHECK(evaluate_camera(worker,7).yaw==60.F);
    CHECK(worker.document.timeline==state.document.timeline);
    const auto peer2=packet(updates,2,state);
    edit=decode_animation_camera_edit(std::string_view(peer2).substr(17));
    REQUIRE(edit); CHECK(edit->base_revision==1); CHECK(edit->revision==104);
    updates.acknowledge(1,104);
    CHECK(updates.ready(1,104));

    // A transport rejection resynchronizes through the existing full snapshot
    // path, rather than applying this delta onto an unknown document baseline.
    updates.reject(1); no_packet(updates,1,state);
    updates.reset(1);
    CHECK(packet(updates,1,state).starts_with("snapshot\n"));
}

TEST_CASE("Unsent camera keys mixed with geometry retain their explicit targets") {
    auto state=source();
    PreviewUpdates updates;
    updates.add(1); packet(updates,1,state); updates.acknowledge(1,1);
    REQUIRE(key_camera(state,3,{30,0,6,{}}));
    ++state.document.revision; updates.camera_tracks_changed();
    REQUIRE(state.document.mesh.set_position(0,{1,2,3}));
    ++state.document.revision; updates.changed(std::array<u32,1>{0});
    const auto full=packet(updates,1,state);
    REQUIRE(full.starts_with("patch\n"));
    const auto patch=decode_patch(std::string_view(full).substr(6));
    REQUIRE(patch);
    auto restored=source();
    REQUIRE(apply_patch(restored,*patch));
    CHECK(restored.document.mesh.position(0)==Vec3{1,2,3});
    CHECK(evaluate_camera(restored,3).yaw==30.F);
}

TEST_CASE("Acknowledged camera and vertex updates can remain separate compact packets") {
    auto state = source();
    PreviewUpdates updates;
    updates.add(1);
    packet(updates, 1, state);
    updates.acknowledge(1, 1);
    state.document.revision = 2;
    state.document.animation_camera.yaw = 30;
    updates.camera_changed();
    REQUIRE(packet(updates, 1, state).starts_with("animation_camera\n"));
    REQUIRE(state.document.mesh.set_position(1, {4, 5, 6}));
    state.document.revision = 3;
    updates.changed(std::array<u32, 1>{1});
    no_packet(updates, 1, state);
    updates.acknowledge(1, 2);
    const auto vertices = packet(updates, 1, state);
    REQUIRE(vertices.starts_with("vertices\n"));
    auto edit = decode_edit(std::string_view(vertices).substr(9));
    REQUIRE(edit);
    CHECK(edit->base_revision == 2);
    CHECK(edit->revision == 3);
    state.document.revision = 4;
    state.document.animation_camera.target = {-1, 0, 2};
    updates.camera_changed();
    no_packet(updates, 1, state);
    updates.acknowledge(1, 3);
    const auto camera = packet(updates, 1, state);
    REQUIRE(camera.starts_with("animation_camera\n"));
    auto pose = decode_animation_camera_edit(std::string_view(camera).substr(17));
    REQUIRE(pose);
    CHECK(pose->base_revision == 3);
    CHECK(pose->revision == 4);
    updates.acknowledge(1, 4);
    CHECK(updates.ready(1, 4));
}

TEST_CASE("Camera update queues preserve per-generation baselines and recover after rejection") {
    auto state = source();
    PreviewUpdates updates;
    updates.add(1); updates.add(2);
    packet(updates, 1, state); packet(updates, 2, state);
    updates.acknowledge(1, 1);
    state.document.revision = 2; state.document.animation_camera.pitch = 20; updates.camera_changed();
    REQUIRE(packet(updates, 1, state).starts_with("animation_camera\n"));
    no_packet(updates, 2, state);
    state.document.revision = 3; state.document.animation_camera.distance = 4; updates.camera_changed();
    updates.acknowledge(2, 1);
    const auto second = packet(updates, 2, state);
    auto second_pose = decode_animation_camera_edit(std::string_view(second).substr(17));
    REQUIRE(second_pose);
    CHECK(second_pose->base_revision == 1);
    CHECK(second_pose->revision == 3);
    updates.acknowledge(1, 2);
    const auto first = packet(updates, 1, state);
    auto first_pose = decode_animation_camera_edit(std::string_view(first).substr(17));
    REQUIRE(first_pose);
    CHECK(first_pose->base_revision == 2);
    CHECK(first_pose->revision == 3);
    updates.reject(1);
    no_packet(updates, 1, state);
    updates.acknowledge(1, 3); // Rejected packet cannot silently establish a baseline.
    no_packet(updates, 1, state);
    state.document.revision = 4; state.document.animation_camera.yaw = 0; updates.camera_changed();
    REQUIRE(packet(updates, 1, state).starts_with("snapshot\n"));
    updates.reset(2);
    REQUIRE(packet(updates, 2, state).starts_with("snapshot\n"));
    updates.accepted(2, 4);
    CHECK(updates.ready(2, 4));
    updates.remove(1);
    no_packet(updates, 1, state);
}

TEST_CASE("Invalid camera encoding does not reserve or discard the pending latest pose") {
    auto state = source();
    PreviewUpdates updates;
    updates.add(1);
    packet(updates, 1, state);
    updates.acknowledge(1, 1);
    state.document.revision = 2;
    state.document.animation_camera.pitch = std::numeric_limits<f32>::quiet_NaN();
    updates.camera_changed();
    REQUIRE_FALSE(updates.next(1, state));
    CHECK_FALSE(updates.ready(1, 2));
    state.document.revision = 3;
    state.document.animation_camera.pitch = 45;
    updates.camera_changed();
    const auto wire = packet(updates, 1, state);
    REQUIRE(wire.starts_with("animation_camera\n"));
    auto edit = decode_animation_camera_edit(std::string_view(wire).substr(17));
    REQUIRE(edit);
    CHECK(edit->base_revision == 1);
    CHECK(edit->revision == 3);
    updates.acknowledge(1, 3);
    CHECK(updates.ready(1, 3));
    updates.changed();
    updates.camera_changed();
    state.document.revision = 4;
    REQUIRE(packet(updates, 1, state).starts_with("snapshot\n"));
}
