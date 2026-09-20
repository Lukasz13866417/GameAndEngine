#include "../../examples/editor/blueprint_mesh_controls.hpp"
#include "../../examples/editor/editing_session.hpp"
#include "../../examples/editor/mesh_camera_bake.hpp"
#include "../../examples/editor/preview_updates.hpp"
#include "../../examples/support/earth_assets.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <algorithm>

namespace {
using namespace vng;
using namespace editor_example;
namespace earth=example::earth;
namespace vm=content::vmesh;
vm::Document source() {
    return {.metadata={{"editor/blueprint","earth"},{"custom/note","keep this"}},.vertex_count=6,
        .vertex_fields={{"position",{vm::ScalarType::Float32,3},std::vector<f32>{0,0,1,1,0,0,0,1,0, 0,0,1.02F,1.02F,0,0,0,1.02F,0}},
            {"normal",{vm::ScalarType::Float32,3},std::vector<f32>{0,0,1,1,0,0,0,1,0,0,0,1,1,0,0,0,1,0}},
            {"color/0",{vm::ScalarType::Float32,4},std::vector<f32>(24,1)},
            {"emission",{vm::ScalarType::Float32,1},std::vector<f32>{.035F,.035F,.035F,.015F,.015F,.015F}},
            {"earth/layer",{vm::ScalarType::UInt32,1},std::vector<u32>{0,0,0,1,1,1}},
            {"custom/material",{vm::ScalarType::Int32,1},std::vector<i32>{7,8,9,10,11,12}}},
        .faces={{0,1,2},{3,4,5}},.edges=std::vector<gfx::Edge>{{0,1},{3,4}}};
}
editor::EditableMesh mesh(vm::Document d=source()) {auto m=editor::EditableMesh::create(std::move(d));REQUIRE(m);return std::move(*m);}
}
TEST_CASE("Camera baking transfers only selected rotation and optical scale around the mesh center", "[editor][camera-bake]") {
    using namespace example::mesh_frame;
    const Vec3 center{1,2,3};
    auto placement=Mat4::identity();placement[3]={4,1,-2,1};
    const auto world_center=point(placement,center);
    const CameraPose pose{70,-25,12,{3,1,0},2};
    for(const auto options:{CameraBakeOptions{true,false},CameraBakeOptions{false,true},CameraBakeOptions{true,true},CameraBakeOptions{false,false}}) {
        auto baked=mesh_camera_bake(placement,center,pose,options);REQUIRE(baked);
        const auto after_center=point(baked->placement,center);
        for(unsigned c=0;c<3;++c)CHECK(after_center[c]==Catch::Approx(world_center[c]).margin(.00001));
        CHECK(baked->camera.yaw==(options.rotation?CameraPose{}.yaw:pose.yaw));
        CHECK(baked->camera.pitch==(options.rotation?CameraPose{}.pitch:pose.pitch));
        CHECK(baked->camera.zoom==(options.scale?1.F:pose.zoom));
        CHECK(baked->camera.distance==pose.distance);
        const auto edge=vector(baked->placement,{1,0,0});
        CHECK(std::hypot(edge.x,edge.y,edge.z)==Catch::Approx(options.scale?2.F:1.F));
        auto again=mesh_camera_bake(baked->placement,center,baked->camera,options);REQUIRE(again);
        CHECK(again->placement==baked->placement);CHECK(again->camera==baked->camera);
    }
    // Rotation is the inverse viewing change, not the camera's own rotation.
    auto baked=mesh_camera_bake(placement,center,pose,{true,false});REQUIRE(baked);
    const auto old_view=camera(pose,ViewMode::mesh).snapshot({800,600})->view_projection;
    const auto new_view=camera(baked->camera,ViewMode::mesh).snapshot({800,600})->view_projection;
    const auto old_m=compose(old_view,placement),new_m=compose(new_view,baked->placement);
    for(unsigned c=0;c<4;++c)for(unsigned r=0;r<4;++r)CHECK(new_m[c][r]==Catch::Approx(old_m[c][r]).margin(.00002));
    // Panning and dollying alone never become mesh translation or scale.
    CameraPose moved; moved.target={9,8,7};moved.distance=30;
    auto unchanged=mesh_camera_bake(placement,center,moved,{});REQUIRE(unchanged);
    CHECK(unchanged->placement==placement);CHECK(unchanged->camera==moved);
}
TEST_CASE("Camera bake is one undoable draft placement and saves through the mesh baker", "[editor][camera-bake]") {
    State state{.document={.mesh=mesh()}};state.viewport.mode=ViewMode::mesh;
    state.viewport.editor_camera={60,30,8,{2,1,0},1.5F};
    const auto geometry=state.document.mesh.document();
    EditingSession editing(std::move(state));
    auto baked=bake_mesh_camera(editing,{});REQUIRE(baked);REQUIRE(*baked);
    const auto placement=mesh_placement(editing.state(),BlueprintId::mesh,true);
    CHECK(placement!=Mat4::identity());CHECK(editing.state().document.mesh.document()==geometry);
    CHECK(mesh_placement(editing.state(),BlueprintId::mesh,false)==Mat4::identity());
    REQUIRE(editing.undo());CHECK(mesh_placement(editing.state(),BlueprintId::mesh,true)==Mat4::identity());
    REQUIRE(editing.redo());CHECK(mesh_placement(editing.state(),BlueprintId::mesh,true)==placement);
    REQUIRE(editing.apply_mesh(BlueprintId::mesh));
    CHECK(mesh_placement(editing.state(),BlueprintId::mesh,false)==placement);
    auto saved=bake_mesh_placements(editing.state());REQUIRE(saved);
    CHECK(saved->document.mesh.document().vertex_fields!=geometry.vertex_fields);
    CHECK(editing.state().document.mesh.document()==geometry); // Save bakes a copy.
    auto no_change=bake_mesh_camera(editing,{});REQUIRE(no_change);CHECK_FALSE(*no_change);
}
TEST_CASE("Blueprint mesh controls are declared by identity, not display names or instances", "[editor][blueprint-mesh]") {
    editor::Inspector ui{3,1,1};std::optional<MeshDraftEdit> request;
    REQUIRE(describe_blueprint_mesh(ui,mesh(),[&](MeshDraftEdit r){request=std::move(r);}));
    REQUIRE(ui.schema().controls.size()==1);
    CHECK(ui.schema().controls[0].fields.size()==7);
    const editor::Event apply{ui.schema().stamp,"earth_clouds",editor::Phase::apply,{{"visible",false},{"coverage",1.5F},{"edge_scatter",1.25F}}};
    REQUIRE(ui.dispatch(apply));REQUIRE(request);
    auto result=request->apply(mesh());REQUIRE(result);CHECK(result->size()==3);
    CHECK(earth::cloud_settings(result->document())->coverage==1.5F);
    CHECK(earth::cloud_settings(result->document())->edge_scatter==1.25F);
    auto ordinary=source();ordinary.metadata={{"name","Earth"}};
    editor::Inspector none{4,1,1};
    REQUIRE(describe_blueprint_mesh(none,mesh(ordinary),[](MeshDraftEdit){FAIL("Unexpected controls");}));
    CHECK(none.schema().controls.empty());
}
TEST_CASE("Cloud rebuilds preserve terrain and metadata and reject cross-layer connections", "[editor][blueprint-mesh]") {
    auto original=source();
    std::get<std::vector<f32>>(original.vertex_fields[0].values)[0]=.125F; // manual terrain edit
    const auto rebuilt=earth::rebuild_clouds(original,{.visible=false});REQUIRE(rebuilt);
    CHECK(rebuilt->metadata.at("custom/note")=="keep this");CHECK(rebuilt->faces==std::vector<gfx::TriangleFace>{{0,1,2}});
    CHECK(*rebuilt->edges==std::vector<gfx::Edge>{{0,1}});
    CHECK(std::get<std::vector<f32>>(rebuilt->vertex_fields[0].values)[0]==.125F);
    const auto custom=std::ranges::find(rebuilt->vertex_fields,"custom/material",&vm::VertexField::name);
    REQUIRE(custom!=rebuilt->vertex_fields.end());
    CHECK(std::get<std::vector<i32>>(custom->values)==std::vector<i32>{7,8,9});
    REQUIRE(earth::cloud_settings(*rebuilt));CHECK_FALSE(earth::cloud_settings(*rebuilt)->visible);
    original.faces.push_back({0,1,3});CHECK_FALSE(earth::rebuild_clouds(original,{.visible=false}));original.faces.pop_back();
    original.edges->push_back({0,3});CHECK_FALSE(earth::rebuild_clouds(original,{.visible=false}));
    CHECK_FALSE(earth::rebuild_clouds(source(),{.puff_size=0}));
    auto invalid=source();invalid.metadata["earth/clouds/coverage"]="NaN";CHECK_FALSE(earth::cloud_settings(invalid));
    for(auto value:{"-1","2.01","NaN","garbage"}) {
        invalid=source();invalid.metadata["earth/clouds/edge_scatter"]=value;
        CHECK_FALSE(earth::cloud_settings(invalid));
    }
    CHECK_FALSE(earth::rebuild_clouds(source(),{.edge_scatter=-1}));
}
TEST_CASE("Legacy generated Earth assets migrate their layer ownership without a name heuristic", "[editor][blueprint-mesh]") {
    auto original=source();original.metadata.erase("editor/blueprint");
    original.metadata["source/tool"]="examples/support/earth_assets.cpp";
    std::erase_if(original.vertex_fields,[](const auto& f){return f.name=="earth/layer";});
    REQUIRE(earth::is_earth(original));auto result=earth::rebuild_clouds(original,{.visible=false});REQUIRE(result);
    CHECK(earth::cloud_settings(original)->edge_scatter==0);
    CHECK(earth::cloud_settings(*result)->edge_scatter==1);
    CHECK(result->vertex_count==3);CHECK(result->metadata.at("editor/blueprint")=="earth");
    std::get<std::vector<f32>>(original.vertex_fields[3].values)[0]=.08F;
    CHECK_FALSE(earth::rebuild_clouds(original,{.visible=false}));
}
TEST_CASE("Procedural mesh authoring is draft-only, undoable, persistent and revision checked", "[editor][blueprint-mesh]") {
    State state{.document={.mesh=mesh()}};state.viewport.mode=ViewMode::mesh;
    EditingSession editing{state};CHECK_FALSE(editing.can_edit_scene_pose());
    auto rebuilt=earth::rebuild_clouds(source(),{.edge_scatter=1.75F,.visible=false});REQUIRE(rebuilt);
    const auto revision=editing.state().document.revision;
    REQUIRE(editing.replace_mesh_draft(BlueprintId::mesh,revision,mesh(*rebuilt)));
    CHECK(editing.state().document.mesh.document()==source());
    CHECK(editable_mesh(editing.state())->size()==3);
    CHECK_FALSE(editing.replace_mesh_draft(BlueprintId::mesh,revision,mesh()));
    auto bytes=encode_scene(editing.state());REQUIRE(bytes);auto restored=decode(*bytes);REQUIRE(restored);
    CHECK_FALSE(earth::cloud_settings(mesh_edit_geometry(*restored,BlueprintId::mesh)->document())->visible);
    CHECK(earth::cloud_settings(mesh_edit_geometry(*restored,BlueprintId::mesh)->document())->edge_scatter==1.75F);
    REQUIRE(editing.undo());CHECK(editing.state().document.mesh_drafts.empty());
    REQUIRE(editing.redo());CHECK(editable_mesh(editing.state())->size()==3);
    REQUIRE(editing.apply_mesh(BlueprintId::mesh));CHECK(editing.state().document.mesh.size()==3);
    REQUIRE(editing.undo());CHECK(editing.state().document.mesh.document()==source());
    CHECK(editable_mesh(editing.state())->size()==3);
}
TEST_CASE("Earth declares named formation edits as ordinary blueprint controls", "[editor][blueprint-mesh]") {
    auto generated=earth::rebuild_clouds(source(),{.visible=false});REQUIRE(generated);
    editor::Inspector ui{3,1,1};std::optional<MeshDraftEdit> request;
    auto parts=describe_blueprint_mesh(ui,mesh(*generated),[&](MeshDraftEdit r){request=std::move(r);},15);
    REQUIRE(parts);CHECK(parts->parts.size()==17);CHECK(parts->parts.at(14).label=="Atlantic spiral");
    // A selected formation exposes its placement and heading edits plus the
    // catalog actions. The whole-mesh cloud rebuild belongs to the unselected view.
    const auto& controls=ui.schema().controls;
    const auto control=[&](std::string_view key)->const editor::Control* {
        const auto found=std::ranges::find(controls,key,&editor::Control::key);
        return found==controls.end()?nullptr:&*found;
    };
    REQUIRE(controls.size()==5);
    REQUIRE(control("earth_cloud_location"));CHECK(control("earth_cloud_location")->kind==editor::Kind::group);
    CHECK(control("earth_cloud_location")->fields.size()==2);
    REQUIRE(control("earth_cloud_heading"));CHECK(control("earth_cloud_heading")->fields.size()==1);
    for(const auto key:{"add_cloud_bank","add_cloud_spiral","remove_cloud"}) {
        REQUIRE(control(key));CHECK(control(key)->kind==editor::Kind::action);
    }
    CHECK_FALSE(control("earth_clouds"));
    auto before=earth::cloud_formations(*generated);REQUIRE(before);
    REQUIRE(ui.dispatch({ui.schema().stamp,"earth_cloud_location",editor::Phase::apply,{{"longitude",22.F},{"latitude",8.F}}}));
    REQUIRE(request);auto moved=request->apply(mesh(*generated));REQUIRE(moved);
    auto formations=earth::cloud_formations(moved->document());REQUIRE(formations);
    CHECK(std::abs(formations->at(14).location.x-22.F)<1e-4F);
    CHECK(std::abs(formations->at(14).location.y-8.F)<1e-4F);
    CHECK(generated->vertex_fields==moved->document().vertex_fields); // Hidden cloud: metadata only.
    request.reset();
    REQUIRE(ui.dispatch({ui.schema().stamp,"earth_cloud_heading",editor::Phase::apply,{{"degrees",30.F}}}));
    REQUIRE(request);auto turned=request->apply(mesh(*generated));REQUIRE(turned);
    auto headed=earth::cloud_formations(turned->document());REQUIRE(headed);
    REQUIRE(headed->size()==17);
    CHECK(headed->at(14).location==before->at(14).location); // Turning keeps the surface placement.
    request.reset();
    REQUIRE(ui.dispatch({ui.schema().stamp,"remove_cloud",editor::Phase::activate,{}}));
    REQUIRE(request);auto removed=request->apply(mesh(*generated));REQUIRE(removed);
    auto remaining=earth::cloud_formations(removed->document());REQUIRE(remaining);
    CHECK(remaining->size()==16);
    CHECK(std::ranges::none_of(*remaining,[](const earth::CloudFormation& f){return f.id==15;}));
    request.reset();
    REQUIRE(ui.dispatch({ui.schema().stamp,"add_cloud_bank",editor::Phase::activate,{}}));
    REQUIRE(request);auto added=request->apply(mesh(*generated));REQUIRE(added);
    auto grown=earth::cloud_formations(added->document());REQUIRE(grown);
    CHECK(grown->size()==18);
}
TEST_CASE("Blueprint draft patches are narrow atomic binary updates including first edit and undo", "[editor][blueprint-mesh][mesh-patch]") {
    auto original=source();
    auto geometry=mesh(original);
    State initial{.document={.mesh=geometry}};initial.viewport.mode=ViewMode::mesh;
    EditingSession editing{initial};auto worker=initial;
    PreviewUpdates updates;updates.add(1);updates.accepted(1,initial.document.revision);
    const auto sync=[&]() {
        auto notice=editing.take_changes();REQUIRE(notice);REQUIRE_FALSE(notice->changes.full);
        REQUIRE(notice->changes.meshes.size()==1);updates.changed(notice->changes);
        auto packet=updates.next(1,editing.state());REQUIRE(packet);REQUIRE(*packet);
        REQUIRE((**packet).starts_with("patch\n"));
        auto decoded=decode_patch(std::string_view(**packet).substr(6));REQUIRE(decoded);
        auto encoded=encode_patch(*decoded);REQUIRE(encoded);CHECK(*encoded==std::string_view(**packet).substr(6));
        REQUIRE(apply_patch(worker,*decoded));updates.acknowledge(1,worker.document.revision);
        REQUIRE(worker.document.mesh_drafts.size()==editing.state().document.mesh_drafts.size());
        for(const auto& [id,draft]:worker.document.mesh_drafts)CHECK(draft.document()==editing.state().document.mesh_drafts.at(id).document());
        CHECK(worker.document.mesh.document()==original);
    };
    auto next=original;
    std::get<std::vector<f32>>(next.vertex_fields[0].values)[9]=.1F;
    std::get<std::vector<f32>>(next.vertex_fields[1].values)[9]=.2F;
    next.metadata["author/move"]="yes";
    REQUIRE(editing.replace_mesh_draft(BlueprintId::mesh,editing.state().document.revision,mesh(next)));sync();
    REQUIRE(editing.undo());sync();CHECK(worker.document.mesh_drafts.empty());
    REQUIRE(editing.redo());sync();
    auto regenerated=earth::rebuild_clouds(next,{.visible=false});REQUIRE(regenerated);
    REQUIRE(editing.replace_mesh_draft(BlueprintId::mesh,editing.state().document.revision,mesh(*regenerated)));sync();
    REQUIRE(editing.undo());sync();
    REQUIRE(editing.redo());sync();
    const auto changes=editor::mesh_changes(original,next);CHECK_FALSE(changes.whole);CHECK(changes.fields.size()==2);
    auto patch=editor::capture_mesh_patch(original,changes);REQUIRE(patch);auto bytes=editor::encode_mesh_patch(*patch);REQUIRE(bytes);
    for(std::size_t n=0;n<bytes->size();++n)CHECK_FALSE(editor::decode_mesh_patch(std::string_view(*bytes).substr(0,n)));
    CHECK_FALSE(editor::decode_mesh_patch(*bytes+"x"));
    patch->fields[0].vertices[0]=9999;CHECK_FALSE(editor::apply_mesh_patch(geometry,*patch));
    CHECK(geometry.document()==original);
}

TEST_CASE("Pending blueprint edits coalesce with ordinary vertex movement without a snapshot",
          "[editor][blueprint-mesh][mesh-patch]") {
    State initial{.document = {.mesh = mesh()}};
    initial.viewport.mode = ViewMode::mesh;
    EditingSession editing{initial};
    auto worker = initial;
    PreviewUpdates updates;
    updates.add(1);
    updates.accepted(1, initial.document.revision);
    const auto changed = [&] {
        auto notice = editing.take_changes();
        REQUIRE(notice);
        REQUIRE_FALSE(notice->changes.full);
        updates.changed(notice->changes);
    };
    const auto apply = [&](const std::string& packet) {
        REQUIRE(packet.starts_with("patch\n"));
        auto patch = decode_patch(std::string_view(packet).substr(6));
        REQUIRE(patch);
        REQUIRE(patch->meshes.size() == 1);
        CHECK(patch->vertices.empty()); // No duplicate position and mesh lanes.
        REQUIRE(apply_patch(worker, *patch));
        updates.acknowledge(1, worker.document.revision);
    };

    auto next = source();
    std::get<std::vector<f32>>(next.vertex_fields[1].values)[9] = .2F;
    REQUIRE(editing.replace_mesh_draft(BlueprintId::mesh, editing.state().document.revision, mesh(next)));
    changed();
    auto first = updates.next(1, editing.state());
    REQUIRE(first);
    REQUIRE(*first);

    // Further authoring while that first packet is in flight must retain both
    // normal edits and regular mesh-tool position changes, using latest values.
    std::get<std::vector<f32>>(next.vertex_fields[1].values)[12] = .3F;
    next.metadata["author/pending"] = "keep";
    REQUIRE(editing.replace_mesh_draft(BlueprintId::mesh, editing.state().document.revision, mesh(next)));
    changed();
    const std::array<u32, 1> vertices{2};
    REQUIRE(editing.translate_vertices(BlueprintId::mesh, vertices, {.1F, 0, 0}));
    changed();
    auto blocked = updates.next(1, editing.state());
    REQUIRE(blocked);
    CHECK_FALSE(*blocked);

    apply(**first);
    auto second = updates.next(1, editing.state());
    REQUIRE(second);
    REQUIRE(*second);
    apply(**second);
    CHECK(mesh_edit_geometry(worker, BlueprintId::mesh)->document() ==
          mesh_edit_geometry(editing.state(), BlueprintId::mesh)->document());
    CHECK(updates.ready(1, editing.state().document.revision));
    CHECK(worker.document.mesh.document() == source());
}

TEST_CASE("Invalid mixed document patches do not partially install mesh drafts",
          "[editor][blueprint-mesh][mesh-patch]") {
    State original{.document = {.mesh = mesh()}};
    original.viewport.mode = ViewMode::mesh;
    EditingSession editing{original};
    auto next = source();
    std::get<std::vector<f32>>(next.vertex_fields[1].values)[12] = .3F;
    REQUIRE(editing.replace_mesh_draft(BlueprintId::mesh, original.document.revision, mesh(next)));
    auto notice = editing.take_changes();
    REQUIRE(notice);
    auto patch = capture_patch(original.document.revision, editing.state(), notice->changes);
    REQUIRE(patch);
    patch->properties.push_back({.target = {999999, "missing"}, .base = 1.F});
    CHECK_FALSE(apply_patch(original, *patch));
    CHECK(original.document.revision == patch->base_revision);
    CHECK(original.document.mesh_drafts.empty());
    CHECK(original.document.mesh.document() == source());
}

TEST_CASE("Blueprint part gestures publish narrow previews and keep exactly one sparse undo step",
          "[editor][blueprint-mesh][mesh-gesture]") {
    State initial{.document = {.mesh = mesh()}};
    initial.viewport.mode = ViewMode::mesh;
    EditingSession editing{initial};
    REQUIRE(editing.begin_mesh_draft_edit(BlueprintId::mesh));
    auto next = source();
    for(float value : {.1F,.2F,.3F}) {
        std::get<std::vector<f32>>(next.vertex_fields[0].values)[9] = value;
        std::get<std::vector<f32>>(next.vertex_fields[1].values)[9] = value;
        next.metadata["part/pose"] = std::to_string(value);
        REQUIRE(editing.preview_mesh_draft(editing.state().document.revision, mesh(next)));
        auto notice = editing.take_changes();
        REQUIRE(notice);
        CHECK_FALSE(notice->changes.full);
        CHECK_FALSE(notice->changes.meshes.at(1).whole);
        CHECK(notice->changes.meshes.at(1).fields.size() == 2);
        CHECK_FALSE(editing.can_undo());
    }
    REQUIRE(editing.commit());
    REQUIRE(editing.undo());
    CHECK(editing.state().document.mesh_drafts.empty());
    CHECK_FALSE(editing.can_undo());
    auto notice = editing.take_changes();
    REQUIRE(notice);
    CHECK_FALSE(notice->changes.meshes.at(1).whole);
    REQUIRE(editing.redo());
    CHECK(editable_mesh(editing.state())->document() == next);
    (void)editing.take_changes();

    REQUIRE(editing.begin_mesh_draft_edit(BlueprintId::mesh));
    REQUIRE(editing.preview_mesh_draft(editing.state().document.revision, mesh()));
    REQUIRE(editing.cancel());
    CHECK(editable_mesh(editing.state())->document() == next);
    CHECK_FALSE(editing.take_changes()->changes.full);
    CHECK_FALSE(editing.preview_mesh_draft(editing.state().document.revision, mesh())); // late job
}

TEST_CASE("Returning a blueprint part to its original pose leaves no draft or history",
          "[editor][blueprint-mesh][mesh-gesture]") {
    State initial{.document = {.mesh = mesh()}};
    EditingSession editing{initial};
    REQUIRE(editing.begin_mesh_draft_edit(BlueprintId::mesh));
    auto next = source();
    std::get<std::vector<f32>>(next.vertex_fields[0].values)[9] = .1F;
    REQUIRE(editing.preview_mesh_draft(editing.state().document.revision, mesh(next)));
    REQUIRE(editing.preview_mesh_draft(editing.state().document.revision, mesh()));
    REQUIRE(editing.commit()); // successful expected, containing false
    CHECK_FALSE(editing.can_undo());
    CHECK_FALSE(editing.dirty());
    CHECK(editing.state().document.mesh_drafts.empty());
}
