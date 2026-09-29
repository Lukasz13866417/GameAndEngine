#include "../../examples/editor/workspace_ui.hpp"
#include "../../examples/editor/animation.hpp"
#include <vng/ui/inspection.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace vng;
using namespace editor_example;
struct WorkspaceFixture {
    static text::Font font() {auto result=text::Font::load(VNG_TEST_FONT_PATH);REQUIRE(result);return *result;}
    static State initial() {
        content::vmesh::Document d;d.vertex_count=3;
        d.vertex_fields={{"position",{content::vmesh::ScalarType::Float32,3},std::vector<f32>{-1,-1,0,1,-1,0,0,1,0}}};
        d.faces={{0,1,2}};
        auto mesh=editor::EditableMesh::create(std::move(d));REQUIRE(mesh);
        return State{.document={.mesh=std::move(*mesh)}};
    }
    ui::Screen screen{ui::dark_theme(font())},popups{ui::dark_theme(font())};
    EditingWorkspaceUI workspace{initial()};
    input::Frame raw{.logical_size={1800,1200},.framebuffer={1800,1200}};
    WorkspaceFixture() {
        workspace.create_panels(screen);
        workspace.initialize_panels(screen,popups,screen.column(),screen.column(),screen.column());
        workspace.initialize_camera(screen.column(),popups.column(),Settings{});
        auto layout=editor_layout(raw.logical_size,false);
        workspace.layout_panels(layout,raw.logical_size);
        workspace.refresh_selection();
        workspace.refresh_vertex();
        workspace.sync_sidebar();
        pump();
    }
    void pump() {REQUIRE(screen.update(raw,.016));REQUIRE(popups.update(raw,.016));}
    void keyframe() {
        const std::array times{0.F};
        (void)dispatch(workspace,InspectScene{},WorkspaceContext{
            .timeline=TimelineContext{.input={.select=std::span<const f32>{times}}}});
        REQUIRE(workspace.can_edit_scene_pose());
    }
    bool text_contains(std::string_view value) {
        pump();auto tree=screen.inspect();REQUIRE(tree);
        return std::ranges::any_of(tree->widgets,[&](const auto& widget){return widget.text.find(value)!=std::string::npos;});
    }
    bool enabled(ui::WidgetRole role,std::string_view label) {
        pump();auto tree=screen.inspect();REQUIRE(tree);
        for(const auto& widget:tree->widgets)if(widget.role==role&&widget.label==label)return widget.enabled;
        FAIL("Missing control: "<<label);return false;
    }
};
// A mutable workspace must still expose only observations of its descendants.
static_assert(std::same_as<decltype(std::declval<EditingWorkspaceUI&>().interaction()),const ViewportToolsUI&>);
static_assert(std::same_as<decltype(std::declval<EditingWorkspaceUI&>().blueprint_panel()),const BlueprintMeshPanel&>);
static_assert(std::same_as<decltype(std::declval<EditingWorkspaceUI&>().gizmo_selector()),const GizmoSelector&>);
static_assert(std::same_as<decltype(std::declval<EditingWorkspaceUI&>().rotation_pivot()),const RotationPivotControls&>);
static_assert(std::same_as<decltype(std::declval<EditingWorkspaceUI&>().camera_ui()),const ViewportCameraUI&>);
}

TEST_CASE("Workspace selection owns sidebar focus and inspector invalidation without app glue","[editor][workspace-ownership]") {
    WorkspaceFixture f;
    const auto revision=f.workspace.state().document.revision;
    f.workspace.keyboard_target(SelectionTarget::keyframes);
    f.workspace.show_tab(SidebarTab::keyframe);
    f.workspace.select_instance(1);
    CHECK(f.workspace.keyboard_target()==SelectionTarget::instances);
    CHECK(f.workspace.sidebar_tab()==SidebarTab::keyframe); // Presentation is not keyboard selection.
    CHECK(f.workspace.minimum_inspector_sequence()==f.workspace.viewport().sequence);
    CHECK(f.workspace.gizmos_dirty());
    CHECK(f.workspace.selection_message().find("Selected 1 instance")!=std::string::npos);
    CHECK(f.text_contains("Instance: "+object_name(f.workspace.state(),1)));
    f.workspace.clear_selection();
    CHECK(f.text_contains("No instance selected"));
    CHECK(f.workspace.state().document.revision==revision);
    CHECK_FALSE(f.workspace.dirty());
}
TEST_CASE("Workspace clipboard and deletion update selection tools and panels together","[editor][workspace-ownership]") {
    WorkspaceFixture f;f.keyframe();f.workspace.select_instance(1);
    f.workspace.show_tab(SidebarTab::keyframe);
    auto copied=f.workspace.shortcut(EditShortcut::copy);
    CHECK(copied.message.starts_with("Copied 1 instance"));
    const auto before=f.workspace.state().document.instances.size();
    auto pasted=f.workspace.shortcut(EditShortcut::paste);
    REQUIRE(pasted.changed);CHECK(pasted.message.starts_with("Pasted 1 instance"));
    CHECK(f.workspace.state().document.instances.size()==before+1);
    CHECK(f.workspace.selected_instances().active()!=1);
    CHECK(f.workspace.sidebar_tab()==SidebarTab::properties);
    CHECK(f.workspace.gizmo_selector().value()==GizmoMode::move);
    auto removed=f.workspace.delete_selection();REQUIRE(removed.changed);
    CHECK(f.workspace.state().document.instances.size()==before);
    CHECK(f.workspace.shortcut(EditShortcut::undo).discontinuous);
    CHECK(f.workspace.state().document.instances.size()==before+1);
}
TEST_CASE("Workspace cancellation restores the transaction and releases the owned tool","[editor][workspace-ownership]") {
    WorkspaceFixture f;f.keyframe();f.workspace.select_instance(1);
    const auto original=evaluate_transform(f.workspace.state(),*find_instance(f.workspace.state(),1),0);
    REQUIRE(f.workspace.begin_scale(1));REQUIRE(f.workspace.scale(2));
    REQUIRE(f.workspace.interaction().busy());
    const auto cancelled=f.workspace.cancel_interaction();CHECK(cancelled.message.empty());
    CHECK_FALSE(f.workspace.interaction().busy());CHECK_FALSE(f.workspace.busy());
    CHECK(evaluate_transform(f.workspace.state(),*find_instance(f.workspace.state(),1),0).scale==original.scale);
    CHECK_FALSE(f.workspace.can_undo());
}
TEST_CASE("Programmatic keyframe selection survives workspace presentation refresh","[editor][workspace-ownership]") {
    WorkspaceFixture f;
    f.workspace.select_keyframe(0);
    CHECK(f.workspace.timeline_view().selected_keyframe()==0);
    f.workspace.select_instance(1);
    REQUIRE(f.workspace.begin_move(1));REQUIRE(f.workspace.move({1,2,3}));
    (void)f.workspace.finish_interaction(ViewportTool::translation,false);
    CHECK(f.workspace.can_edit_scene_pose());
    CHECK(f.workspace.timeline_view().selected_keyframe()==0);
    f.workspace.select_keyframe({});
    CHECK_FALSE(f.workspace.timeline_view().selected_keyframe());
    CHECK_FALSE(f.workspace.can_edit_scene_pose());
}
TEST_CASE("Viewport owns camera visits restoration and deleted-target recovery","[editor][workspace-ownership]") {
    WorkspaceFixture f;f.keyframe();
    auto created=f.workspace.instantiate(BlueprintId::camera);REQUIRE(created);
    const auto before=f.workspace.viewport().editor_camera;
    CHECK_FALSE(f.workspace.visit_camera(*created,true).empty());
    CHECK(f.workspace.camera_ui().visiting());CHECK_FALSE(f.workspace.camera_ui().inspecting());
    f.workspace.viewport().editor_camera.distance+=13;
    CHECK(f.workspace.end_camera_visit(true));
    CHECK(f.workspace.viewport().editor_camera==before);
    CHECK_FALSE(f.workspace.visit_camera(*created,false).empty());
    CHECK(f.workspace.camera_ui().inspecting());
    f.workspace.viewport().editor_camera.distance+=5;
    CHECK(f.workspace.reconcile_camera_visit());
    const std::array ids{*created};REQUIRE(f.workspace.erase_instances(ids));
    CHECK(f.workspace.reconcile_camera_visit());
    CHECK_FALSE(f.workspace.camera_ui().visiting());
    CHECK(f.workspace.viewport().editor_camera==before);
}
TEST_CASE("Workspace mesh-mode transition owns context and preserves scene data","[editor][workspace-ownership]") {
    WorkspaceFixture f;
    const auto revision=f.workspace.state().document.revision;
    f.workspace.select_instance(1);
    CHECK_FALSE(f.workspace.change_interaction_mode(InteractionMode::vertices).empty());
    CHECK(f.workspace.state().viewport.mode==ViewMode::mesh);
    CHECK(f.workspace.sidebar_tab()==SidebarTab::properties);
    CHECK_FALSE(f.workspace.change_interaction_mode(InteractionMode::objects).empty());
    CHECK(f.workspace.state().viewport.mode==ViewMode::scene);
    CHECK(f.workspace.state().document.revision==revision);
    CHECK_FALSE(f.workspace.dirty());
}
TEST_CASE("Workspace diagnostics include panel and camera state only under their owners","[editor][workspace-ownership]") {
    WorkspaceFixture f;f.workspace.toggle_camera_menu(Settings{});
    const auto report=f.workspace.debug_string();
    CHECK(report.find("camera controls and private view visit")!=std::string::npos);
    CHECK(report.find("sidebar widgets")!=std::string::npos);
    CHECK(report.find("settings open")!=std::string::npos);
    f.workspace.close_camera_menu();CHECK_FALSE(f.workspace.camera_ui().opened());
}
TEST_CASE("Panel availability follows the host's frame-start gesture snapshot","[editor][workspace-ownership]") {
    WorkspaceFixture f;f.keyframe();f.workspace.select_instance(1);
    f.workspace.present_panels({.inspector_ready=true});
    REQUIRE(f.enabled(ui::WidgetRole::dropdown,"Gizmo"));
    // A keyboard transform begins during this input frame. Refreshing the pose
    // controls later in the same frame keeps that frame's availability...
    REQUIRE(f.workspace.begin_scale(1));REQUIRE(f.workspace.scale(2));
    REQUIRE(f.workspace.interaction().busy());
    f.workspace.present_pose_controls({.inspector_ready=true});
    CHECK(f.enabled(ui::WidgetRole::dropdown,"Gizmo"));
    // ...and the next frame's snapshot disables it with the rest of the sidebar.
    f.workspace.present_panels({.inspector_ready=true,.gesture=true});
    CHECK_FALSE(f.enabled(ui::WidgetRole::dropdown,"Gizmo"));
    CHECK_FALSE(f.enabled(ui::WidgetRole::dropdown,"Mode"));
    (void)f.workspace.cancel_interaction();
    f.workspace.present_panels({.inspector_ready=true});
    CHECK(f.enabled(ui::WidgetRole::dropdown,"Gizmo"));
}
TEST_CASE("Delete and copy target scene instances only in Objects mode","[editor][workspace-ownership]") {
    WorkspaceFixture f;f.keyframe();
    const auto count=f.workspace.state().document.instances.size();
    f.workspace.clear_selection();
    const auto nothing=f.workspace.delete_selection();
    CHECK_FALSE(nothing.changed);CHECK(nothing.message.empty()); // Nothing selected, nothing to explain.
    f.workspace.select_instance(1);
    f.workspace.interaction_mode(InteractionMode::vertices);
    const auto refused=f.workspace.delete_selection();
    CHECK_FALSE(refused.changed);CHECK(refused.message.starts_with("Switch to Objects mode"));
    CHECK(f.workspace.shortcut(EditShortcut::copy).message.starts_with("Copy failed"));
    CHECK(f.workspace.state().document.instances.size()==count);
    f.workspace.interaction_mode(InteractionMode::objects);
    CHECK(f.workspace.shortcut(EditShortcut::copy).message.starts_with("Copied 1 instance"));
    CHECK(f.workspace.delete_selection().changed);
    CHECK(f.workspace.state().document.instances.size()==count-1);
}
TEST_CASE("Hiding the camera menu hands LMB back to the object tools","[editor][workspace-ownership]") {
    WorkspaceFixture f;
    f.workspace.choose_camera_gizmo(CameraGizmoMode::walk);
    f.workspace.present_camera_gizmo({.viewport={0,0,800,600}},Settings{});
    REQUIRE(f.workspace.interaction().object_tools_suspended());
    // Docked Camera settings, Logs, a dialog or Play hide the corner menu. No
    // explicit mode survives without the menu that names it and leaves it.
    f.workspace.present_camera_gizmo({.viewport={0,0,800,600},.visible=false},Settings{});
    CHECK_FALSE(f.workspace.interaction().object_tools_suspended());
    CHECK_FALSE(f.workspace.interaction().camera_gizmo().walking().active());
}
