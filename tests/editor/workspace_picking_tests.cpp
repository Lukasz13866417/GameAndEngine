#include "../../examples/editor/workspace.hpp"
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace vng;
using namespace editor_example;
struct PickingFixture {
    static text::Font font() { auto f=text::Font::load(VNG_TEST_FONT_PATH);REQUIRE(f);return *f; }
    static State scene() {
        content::vmesh::Document data;data.vertex_count=4;
        data.vertex_fields={{"position",{content::vmesh::ScalarType::Float32,3},std::vector<f32>{-1,-1,0,1,-1,0,1,1,0,-1,1,0}}};
        data.faces={{0,1,2},{0,2,3}};
        auto mesh=editor::EditableMesh::create(std::move(data));REQUIRE(mesh);
        State state{.document={.mesh=std::move(*mesh)}};
        state.document.instances={
            {3,BlueprintId::mesh,"Left",MeshSettings{},InstanceTransform{.position={-2,0,0}}},
            {7,BlueprintId::mesh,"Right",MeshSettings{},InstanceTransform{.position={2,0,0}}}};
        state.viewport.mode=ViewMode::scene;state.viewport.selected_object=0;
        return state;
    }
    ui::Screen screen{ui::dark_theme(font())};
    EditingWorkspace workspace{scene(),screen.column(),screen.column(),screen.column(),
        {screen.column(),screen.column(),screen.column(),screen.column(),screen.column()},
        {screen.column(),screen.column(),screen.column(),screen.column(),screen.column()},screen.column()};
    gfx::Camera camera;
    ui::Rect bounds{0,0,800,600};
    input::Frame raw{.logical_size={800,600},.framebuffer={800,600}};
    input::EventSequence sequence;
    PickingFixture() {
        camera.set_position({0,0,10}).look_at({}).set_orthographic({.vertical_height=8});
        workspace.attach_viewport_tools(screen.column(),screen.column(),screen.column(),screen.column(),screen.column());
    }
    ViewportSelectionReply send(std::initializer_list<input::Event> events,bool available=true,bool components=false,bool defer=false) {
        raw.events=events;
        sequence.identify(raw.events);
        workspace.interaction().begin_step();
        const ViewportInputContext context{
            .input={.raw=raw,.unhandled=available?std::span<const input::Event>{raw.events}:std::span<const input::Event>{}},
            .presented={.camera=camera,.extent={800,600},.bounds=bounds,.generation=1,
                .revision=workspace.state().document.revision,.time_current=true},
            .tools={.enabled=true,.components=components}};
        return workspace.pick_viewport({context,defer});
    }
    void mesh_mode() {
        workspace.viewport().mode=ViewMode::mesh;
        dispatch(workspace,InspectMesh{BlueprintId::mesh},WorkspaceContext{.mesh=MeshInput{.mode=MeshSelectMode::vertex}});
    }
};
}

TEST_CASE("Workspace viewport picking owns scene clicks and does not readmit UI presses", "[editor][workspace-picking]") {
    PickingFixture f;
    CHECK_FALSE(f.send({{.kind=input::EventKind::pointer_down,.position={250,300}}},false).selection);
    CHECK_FALSE(f.workspace.interaction().selection_box.active());
    const auto picked=f.send({{.kind=input::EventKind::pointer_down,.position={250,300}}});
    REQUIRE(picked.selection);CHECK(picked.selection->active==3);
    REQUIRE(f.workspace.selected_instances().active()==3);
    CHECK(f.workspace.interaction().selection_box.active());
    CHECK_FALSE(f.send({{.kind=input::EventKind::pointer_up,.position={250,300}}},false).selection);
    CHECK_FALSE(f.workspace.interaction().selection_box.active());
    CHECK(f.workspace.state().document.revision==1);CHECK_FALSE(f.workspace.can_undo());
}
TEST_CASE("Workspace selection owns gizmo capabilities without resetting unchanged handles", "[editor][workspace-picking][selection]") {
    PickingFixture f;
    f.workspace.attach_manipulation(f.screen.column(),f.screen.column());
    CHECK(f.workspace.gizmo_selector().common().empty());
    f.workspace.select_instance(3);
    CHECK_FALSE(f.workspace.gizmo_selector().common().empty());
    const auto snapshot=f.camera.snapshot({800,600});REQUIRE(snapshot);
    auto& movement=f.workspace.interaction().translation;
    (void)dispatch(movement,SceneMovement::Instance{{3,{-2,0,0}}},MoveGizmoContext{
        {3,1,f.workspace.state().document.revision},*snapshot,f.bounds});
    REQUIRE(movement.visible());
    f.workspace.reconcile_selection();
    CHECK(movement.visible()); // Presentation refresh is not deselection.
    f.workspace.select_instance(7);
    CHECK_FALSE(movement.visible()); // Retire only the previous target's handle.
    f.workspace.viewport().mode=ViewMode::mesh;
    f.workspace.reconcile_selection();
    CHECK(f.workspace.gizmo_selector().common().empty());
    f.workspace.viewport().mode=ViewMode::scene;
    f.workspace.reconcile_selection();
    CHECK_FALSE(f.workspace.gizmo_selector().common().empty());
    f.workspace.clear_selection();
    CHECK(f.workspace.gizmo_selector().common().empty());
    CHECK_FALSE(f.workspace.can_undo());
}
TEST_CASE("Captured scene rectangle uses its frozen projection and accepts a release over UI", "[editor][workspace-picking]") {
    PickingFixture f;
    f.send({{.kind=input::EventKind::pointer_down,.position={100,100}}});
    REQUIRE(f.workspace.interaction().selection_box.active());
    f.send({{.kind=input::EventKind::pointer_move,.position={700,500}}},false);
    CHECK(f.workspace.interaction().selection_box.dragging());
    f.camera.move_by({100,0,0}); // Completion still uses the captured image.
    const auto released=f.send({{.kind=input::EventKind::pointer_up,.position={700,500}}},false);
    REQUIRE(released.selection);
    CHECK(f.workspace.selected_instances().size()==2);
    CHECK(f.workspace.selected_instances().contains(3));CHECK(f.workspace.selected_instances().contains(7));
    CHECK_FALSE(f.workspace.interaction().selection_box.active());
    CHECK(f.workspace.state().document.revision==1);CHECK_FALSE(f.workspace.dirty());
}
TEST_CASE("Viewport rectangle extends its initial selection rather than its press result", "[editor][workspace-picking]") {
    PickingFixture f;f.workspace.select_instance(3);
    f.send({{.kind=input::EventKind::pointer_down,.position={480,180},.modifiers={.shift=true}}});
    const auto reply=f.send({{.kind=input::EventKind::pointer_up,.position={690,420},.modifiers={.shift=true}}});
    REQUIRE(reply.selection);CHECK(f.workspace.selected_instances().size()==2);
    CHECK(f.workspace.selected_instances().contains(3));CHECK(f.workspace.selected_instances().contains(7));
}
TEST_CASE("Viewport capture is cancelled after resize focus loss or document replacement", "[editor][workspace-picking]") {
    for(int cause=0;cause<3;++cause) {
        PickingFixture f;
        f.send({{.kind=input::EventKind::pointer_down,.position={100,100}}});
        if(cause==0)f.bounds.width=640;
        if(cause==1)f.raw.focused=false;
        if(cause==2)REQUIRE(f.workspace.add_keyframe(2));
        const auto reply=f.send({{.kind=input::EventKind::pointer_up,.position={700,500}}});
        CHECK_FALSE(reply.selection);CHECK_FALSE(f.workspace.interaction().selection_box.active());
    }
}
TEST_CASE("Mesh component picking and its RMB menu remain owned by the viewport", "[editor][workspace-picking]") {
    PickingFixture f;f.mesh_mode();
    auto picked=f.send({{.kind=input::EventKind::pointer_down,.position={325,375}}},true,true);
    CHECK(picked.vertex_changed);CHECK(picked.viewport_changed);
    REQUIRE(f.workspace.mesh_components().selected().size()==1);
    CHECK(f.workspace.mesh_components().selected().front()==0);
    f.send({{.kind=input::EventKind::pointer_up,.position={325,375}}},true,true);
    f.send({{.kind=input::EventKind::pointer_down,.position={80,80}}},true,true);
    auto rectangle=f.send({{.kind=input::EventKind::pointer_up,.position={720,520}}},false,true);
    CHECK(rectangle.vertex_changed);CHECK(rectangle.viewport_changed);
    CHECK(f.workspace.mesh_components().selected().size()==4);
    f.send({{.kind=input::EventKind::pointer_down,.position={500,400},.button=1}},false,true);
    CHECK_FALSE(f.workspace.mesh_components().menu_open());
    const auto selection=f.workspace.mesh_components().selection_revision();
    f.send({{.kind=input::EventKind::pointer_down,.position={500,400},.button=1},
        {.kind=input::EventKind::pointer_down,.position={325,375}}},true,true);
    CHECK(f.workspace.mesh_components().menu_open());
    CHECK(f.workspace.mesh_components().selection_revision()==selection);
    f.send({{.kind=input::EventKind::pointer_down,.position={325,375}}},true,true);
    CHECK(f.workspace.mesh_components().selection_revision()==selection);
    CHECK(f.workspace.state().document.revision==1);CHECK_FALSE(f.workspace.can_undo());
}
