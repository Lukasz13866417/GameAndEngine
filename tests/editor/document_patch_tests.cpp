#include "../../examples/editor/document_patch.hpp"
#include "../../examples/editor/editing_session.hpp"
#include "../../examples/editor/preview_updates.hpp"
#include "../../examples/editor/keyframes.hpp"
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
using namespace vng;
using namespace editor_example;
State scene() {
    content::vmesh::Document d;
    d.vertex_count = 3;
    d.vertex_fields = {{"position", {content::vmesh::ScalarType::Float32, 3},
                        std::vector<f32>{0, 0, 0, 1, 0, 0, 0, 1, 0}}};
    d.faces = {{0, 1, 2}};
    auto mesh = editor::EditableMesh::create(std::move(d)); REQUIRE(mesh);
    State state{.document = {.mesh = std::move(*mesh)}};
    state.document.mesh_assets.push_back({static_cast<BlueprintId>(3), "Second", state.document.mesh, {}});
    state.document.next_blueprint_id = 4;
    return state;
}
DocumentChanges touched() {
    DocumentChanges changes{.duration = true};
    changes.vertices[1] = {0, 2}; changes.vertices[3] = {1};
    changes.properties = {{1, "scale"}, {2, "bloom"}, {2, "white_spots"}};
    changes.markers = {2, 4};
    return changes;
}
DocumentPatch patch_for(State& after) {
    REQUIRE(after.document.mesh.set_position(0, {.5F, 0, 0}));
    REQUIRE(after.document.mesh.set_position(2, {0, 1.5F, 0}));
    REQUIRE(after.document.mesh_assets[0].geometry.set_position(1, {2, 0, 0}));
    instance_transform(after, 1)->scale = 1.5F;
    sun_settings(after, 2)->bloom = .7F;
    sun_settings(after, 2)->white_spots = true;
    REQUIRE(key_property(after, {1, "scale"}, 4, 2.F));
    after.document.keyframe_names[4] = "Arrival";
    after.document.timeline_duration = 12;
    after.document.revision = 2;
    auto patch = capture_patch(1, after, touched()); REQUIRE(patch); return *patch;
}
}
TEST_CASE("Whole mesh placement is a tiny reversible patch and bakes only at the file boundary", "[editor][patch][whole-mesh]") {
    auto worker=scene();worker.viewport.mode=ViewMode::mesh;
    EditingSession editing{worker};
    const auto original=worker.document.mesh.document();
    const auto* storage=std::get<std::vector<f32>>(editable_mesh(editing.state())->document().vertex_fields[0].values).data();
    REQUIRE(editing.begin_mesh_transform(BlueprintId::mesh));
    auto matrix=Mat4::identity();matrix[0][0]=2;matrix[1][1]=3;
    for(unsigned sample=1;sample<=20;++sample) {
        matrix[2][2]=1+float(sample)*.1F;
        auto changed=editing.mesh_transform(matrix);REQUIRE(changed);REQUIRE(*changed);
        auto notice=editing.take_changes();REQUIRE(notice);
        CHECK_FALSE(notice->changes.full);CHECK(notice->changes.meshes.empty());CHECK(notice->changes.vertices.empty());
        auto patch=capture_patch(worker.document.revision,editing.state(),notice->changes);REQUIRE(patch);
        auto bytes=encode_patch(*patch);REQUIRE(bytes);CHECK(bytes->size()<256);
        auto decoded=decode_patch(*bytes);REQUIRE(decoded);CHECK(*decoded==*patch);
        REQUIRE(apply_patch(worker,*decoded));
        CHECK(mesh_placement(worker,BlueprintId::mesh,true)==matrix);
        CHECK(std::get<std::vector<f32>>(editable_mesh(editing.state())->document().vertex_fields[0].values).data()==storage);
    }
    REQUIRE(editing.commit());
    CHECK(editing.state().document.mesh_drafts.empty());CHECK(worker.document.mesh.document()==original);
    CHECK(mesh_placement(worker,BlueprintId::mesh,false)==Mat4::identity());
    auto transport=encode(worker);REQUIRE(transport);
    auto roundtrip=decode(*transport);REQUIRE(roundtrip);
    CHECK(roundtrip->document.mesh_placements==worker.document.mesh_placements);
    auto baked=bake_mesh_placements(worker);REQUIRE(baked);
    CHECK(baked->document.mesh_placements.empty());CHECK(baked->document.mesh.document()==original);
    CHECK(mesh_edit_geometry(*baked,BlueprintId::mesh)->position(1)==Vec3{2,0,0});
    CHECK(mesh_edit_geometry(*baked,BlueprintId::mesh)->position(2)==Vec3{0,3,0});
    CHECK(worker.document.mesh_placements.size()==1); // Saving never mutates live state.
    REQUIRE(editing.undo());CHECK(editing.state().document.mesh_placements.empty());
    REQUIRE(editing.redo());CHECK(mesh_placement(editing.state(),BlueprintId::mesh,true)==matrix);
    // Undoing a later sparse geometry edit must retain the placement draft.
    const std::array<u32,1> selected{0};REQUIRE(editing.begin_vertices(BlueprintId::mesh,selected));
    const std::array<VertexPosition,1> positions{{{0,{.2F,0,0}}}};
    REQUIRE(editing.vertices(positions));REQUIRE(editing.commit());REQUIRE(editing.undo());
    CHECK(mesh_placement(editing.state(),BlueprintId::mesh,true)==matrix);
    REQUIRE(editing.apply_mesh(BlueprintId::mesh));
    CHECK_FALSE(has_mesh_draft(editing.state(),BlueprintId::mesh));
    CHECK(mesh_placement(editing.state(),BlueprintId::mesh,false)==matrix);
    baked=bake_mesh_placements(editing.state());REQUIRE(baked);
    CHECK(baked->document.mesh.position(1)==Vec3{2,0,0});
    REQUIRE(editing.begin_mesh_transform(BlueprintId::mesh));
    auto changed=matrix;changed[0][0]=4;REQUIRE(editing.mesh_transform(changed));
    REQUIRE(editing.mesh_transform(matrix));
    auto committed=editing.commit();REQUIRE(committed);CHECK_FALSE(*committed);
    CHECK_FALSE(has_mesh_draft(editing.state(),BlueprintId::mesh));
}
TEST_CASE("Document patches preserve multiple blueprints properties sparse tracks and metadata", "[editor][patch]") {
    auto worker = scene(), authored = worker;
    worker.document.keyframe_names[2] = "Remove this";
    const auto patch = patch_for(authored);
    auto bytes = encode_patch(patch); REQUIRE(bytes); CHECK(bytes->size() < 1024);
    auto decoded = decode_patch(*bytes); REQUIRE(decoded); CHECK(*decoded == patch);
    const auto* positions = std::get<std::vector<f32>>(worker.document.mesh.document().vertex_fields[0].values).data();
    const auto* topology = worker.document.mesh.document().faces.data();
    const auto view = worker.viewport;
    REQUIRE(apply_patch(worker, *decoded));
    CHECK(worker.document.mesh.document() == authored.document.mesh.document());
    CHECK(worker.document.mesh_assets[0].geometry.document() == authored.document.mesh_assets[0].geometry.document());
    CHECK(worker.document.instances == authored.document.instances);
    CHECK(worker.document.timeline == authored.document.timeline);
    CHECK(worker.document.keyframe_names == authored.document.keyframe_names);
    CHECK(worker.document.timeline_duration == 12.F);
    CHECK(worker.viewport == view);
    CHECK(std::get<std::vector<f32>>(worker.document.mesh.document().vertex_fields[0].values).data() == positions);
    CHECK(worker.document.mesh.document().faces.data() == topology);
}
TEST_CASE("A rejected mixed patch writes nothing", "[editor][patch]") {
    auto worker = scene(), authored = worker;
    auto patch = patch_for(authored);
    SECTION("stale base") { patch.base_revision = 2; }
    SECTION("late invalid vertex") { patch.vertices.back().vertices[0].index = 50; }
    SECTION("missing blueprint") { patch.vertices.back().blueprint = 99; }
    SECTION("duplicate blueprint") { patch.vertices.push_back(patch.vertices.front()); }
    SECTION("wrong property type") { patch.properties[0].base = true; }
    SECTION("out of range property") { patch.properties[0].base = max_instance_scale+1.F; }
    SECTION("missing object") { patch.properties.back().target.object = 99; }
    SECTION("duplicate property") { patch.properties.push_back(patch.properties.front()); }
    SECTION("invalid metadata") { patch.markers[4] = "bad\nname"; }
    SECTION("key beyond duration") { patch.duration = 1.F; }
    SECTION("nonfinite vertex") { patch.vertices[0].vertices[0].position.x = std::numeric_limits<f32>::quiet_NaN(); }
    SECTION("nonfinite property") { patch.properties[0].base = std::numeric_limits<f32>::quiet_NaN(); }
    const auto before = encode(worker); REQUIRE(before);
    CHECK_FALSE(apply_patch(worker, patch));
    const auto after = encode(worker); REQUIRE(after);
    CHECK(*after == *before);
}
TEST_CASE("Full keyframe patches scale beyond the old 256-instance transport budget", "[editor][patch][instances]") {
    auto worker=scene();
    for(unsigned i=0;i<360;++i) REQUIRE(instantiate(worker,BlueprintId::mesh));
    EditingSession editing{worker}; editing.select_keyframe(editing.state().viewport.time);
    REQUIRE(editing.timeline_track_limit(4096));
    REQUIRE(editing.add_keyframe(1));
    const auto notice=editing.take_changes(); REQUIRE(notice);
    auto patch=capture_patch(worker.document.revision,editing.state(),notice->changes); REQUIRE(patch);
    CHECK(patch->properties.size()>256*8+4);
    auto wire=encode_patch(*patch); REQUIRE(wire);
    auto decoded=decode_patch(*wire); REQUIRE(decoded);
    REQUIRE(apply_patch(worker,*decoded));
    CHECK(worker.document.timeline==editing.state().document.timeline);
    CHECK(worker.document.revision==editing.state().document.revision);
}
TEST_CASE("Patch wire rejects truncation versions and trailing bytes", "[editor][patch]") {
    auto state = scene();
    const auto wire = encode_patch(patch_for(state)); REQUIRE(wire);
    for (std::size_t length = 0; length < wire->size(); ++length)
        REQUIRE_FALSE(decode_patch(std::string_view(*wire).substr(0, length)));
    auto corrupt = *wire; corrupt[8] = 10;
    CHECK_FALSE(decode_patch(corrupt));
    CHECK_FALSE(decode_patch(*wire + "x"));
    // V1 predates bounds/regions flags, mesh-patch and placement counts.
    auto legacy = *wire; legacy[8] = 1; legacy.resize(legacy.size()-10);
    const auto decoded = decode_patch(legacy); REQUIRE(decoded);
    CHECK(*decoded == patch_for(state));
}
TEST_CASE("A property patch can remove its track without replacing unrelated tracks or viewport state", "[editor][patch]") {
    auto worker = scene();
    REQUIRE(key_property(worker, {1, "scale"}, 4, 2.F));
    REQUIRE(key_property(worker, {2, "bloom"}, 4, .7F));
    auto authored = worker;
    REQUIRE(authored.document.timeline.erase({1, "scale"}));
    const auto changes = animation_changes(worker.document, authored.document);
    REQUIRE(changes.properties == std::set<timeline::Target, TargetLess>{{1, "scale"}});
    authored.document.revision = 2;
    const auto patch = capture_patch(1, authored, changes); REQUIRE(patch);
    REQUIRE(patch->properties.size() == 1);
    CHECK_FALSE(patch->properties[0].track);
    const auto view = worker.viewport;
    REQUIRE(apply_patch(worker, *patch));
    CHECK(worker.document.timeline == authored.document.timeline);
    CHECK(worker.viewport == view);
}
TEST_CASE("Coalescing survives view changes ACKs and independent target edits", "[editor][patch]") {
    auto state = scene();
    PreviewUpdates updates; updates.add(1); updates.add(2);
    updates.accepted(1, 1); updates.accepted(2, 1);
    state.document.revision = 2; updates.position_changed(1);
    const auto first = updates.next(1, state); REQUIRE(first); REQUIRE(*first);
    REQUIRE((**first).starts_with("position\n"));
    auto worker = state;
    instance_transform(state, 1)->scale = 1.3F;
    state.document.revision = 3; updates.scale_changed(1);
    REQUIRE(state.document.mesh_assets[0].geometry.set_position(0, {4, 0, 0}));
    updates.changed(std::array<u32, 1>{0}, 3);
    REQUIRE(state.document.mesh.set_position(2, {0, 4, 0}));
    updates.changed(std::array<u32, 1>{2}, 1);
    state.viewport.selected_object = 2; state.viewport.mode = ViewMode::sun;
    REQUIRE_FALSE(*updates.next(1, state));
    updates.acknowledge(1, 2);
    const auto final = updates.next(1, state); REQUIRE(final); REQUIRE(*final);
    REQUIRE((**final).starts_with("patch\n"));
    const auto decoded = decode_patch(std::string_view(**final).substr(6)); REQUIRE(decoded);
    CHECK(decoded->base_revision == 2); CHECK(decoded->revision == 3);
    REQUIRE(apply_patch(worker, *decoded));
    CHECK(instance_transform(worker, 1)->scale == 1.3F);
    CHECK(worker.document.mesh_assets[0].geometry.position(0) == Vec3{4, 0, 0});
    CHECK(worker.document.mesh.position(2) == Vec3{0, 4, 0});
    const auto other = updates.next(2, state); REQUIRE(other); REQUIRE(*other);
    const auto combined = decode_patch(std::string_view(**other).substr(6)); REQUIRE(combined);
    CHECK(combined->base_revision == 1);
    CHECK(combined->properties.size() == 2);
}
TEST_CASE("Placement updates cannot be lost to compact property or vertex packets", "[editor][patch][whole-mesh]") {
    for(bool vertex:{false,true}) {
        auto state=scene();auto worker=state;
        PreviewUpdates updates;updates.add(1);updates.accepted(1,1);
        auto placement=Mat4::identity();placement[0][0]=2;
        state.document.mesh_placements[BlueprintId::mesh].draft=placement;
        DocumentChanges changes;changes.mesh_placements.insert(1);
        if(vertex) {
            REQUIRE(state.document.mesh.set_position(0,{.1F,0,0}));changes.vertices[1].insert(0);
        } else {
            instance_transform(state,1)->scale=2;changes.properties.insert({1,"scale"});
        }
        state.document.revision=2;updates.changed(changes);
        auto packet=updates.next(1,state);REQUIRE(packet);REQUIRE(*packet);
        REQUIRE((**packet).starts_with("patch\n"));
        auto patch=decode_patch(std::string_view(**packet).substr(6));REQUIRE(patch);
        REQUIRE(apply_patch(worker,*patch));
        CHECK(mesh_placement(worker,BlueprintId::mesh,true)==placement);
        if(vertex)CHECK(worker.document.mesh.position(0)==Vec3{.1F,0,0});
        else CHECK(instance_transform(worker,1)->scale==2);
    }
}
TEST_CASE("Undo redo retain exact patch identities and structural edits remain explicit", "[editor][patch]") {
    auto initial = scene();
    REQUIRE(begin_mesh_draft(initial, BlueprintId::mesh));
    EditingSession editing{std::move(initial)}; editing.select_keyframe(editing.state().viewport.time);
    REQUIRE(editing.translate_vertices(BlueprintId::mesh, std::array<u32,2>{0,2}, {.5F,0,0}));
    auto notice = editing.take_changes(); REQUIRE(notice);
    CHECK_FALSE(notice->changes.full);
    CHECK(notice->changes.vertices.at(1) == std::set<u32>{0,2});
    const auto changed = notice->changes;
    REQUIRE(editing.undo());
    notice = editing.take_changes(); REQUIRE(notice);
    CHECK(notice->changes.vertices == changed.vertices);
    REQUIRE(editing.redo());
    CHECK(mesh_edit_geometry(editing.state(), BlueprintId::mesh)->position(0) == Vec3{.5F,0,0});
    REQUIRE(editing.instantiate(BlueprintId::mesh));
    notice = editing.take_changes(); REQUIRE(notice);
    CHECK(notice->changes.full);
}
