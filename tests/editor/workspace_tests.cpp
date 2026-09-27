#include "../../examples/editor/workspace_ui.hpp"
#include "../../examples/editor/viewport_tools_ui.hpp"
#include <vng/ui/inspection.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace vng;
using namespace editor_example;
struct Fixture {
    static text::Font font() { auto f=text::Font::load(VNG_TEST_FONT_PATH); REQUIRE(f); return *f; }
    static State state() {
        content::vmesh::Document d; d.vertex_count=4;
        d.vertex_fields={{"position",{content::vmesh::ScalarType::Float32,3},std::vector<f32>{-1,-1,0,1,-1,0,1,1,0,-1,1,0}}};
        d.faces={{0,1,2},{0,2,3}};
        auto mesh=editor::EditableMesh::create(std::move(d)); REQUIRE(mesh);
        State result{.document={.mesh=std::move(*mesh)}}; result.viewport.mode=ViewMode::mesh;
        return result;
    }
    ui::Screen screen{ui::dark_theme(font())};
    EditingWorkspaceUI workspace{state(),screen.column().width(300).height(400),screen.column(),screen.column(),
        {screen.column(),screen.column(),screen.column(),screen.column(),screen.column()},
        {screen.column().visible(false),screen.column().visible(false),screen.column().visible(false),screen.column().visible(false),screen.column().visible(false)},screen.column().visible(false)};
    EditingWorkspaceUI& session{workspace}; // Parent actions, never a mutable domain escape.
    input::Frame frame{.logical_size={1000,800},.framebuffer={1000,800}};
    MeshEditingReply send(MeshInput input={}, bool allowed=true) {
        return dispatch(workspace,workspace_situation(session.state().viewport),WorkspaceContext{input,allowed}).mesh;
    }
    Fixture() { send(); }
    void draw(std::initializer_list<input::Event> events={}) {
        frame.events=events;
        for(const auto& e:events) if(e.kind==input::EventKind::pointer_down||e.kind==input::EventKind::pointer_up) frame.pointer=e.position;
        REQUIRE(screen.update(frame,.016F)); REQUIRE(screen.draw_list());
    }
    ui::WidgetSnapshot widget(std::string_view label) {
        const auto tree=screen.inspect(); REQUIRE(tree);
        for(const auto& node:tree->widgets) if(node.visible&&node.label==label) return node;
        FAIL("Missing mesh menu control: " << label); return {};
    }
};
template<class T> concept PublicWorkspaceHandler = requires(T& workspace, InspectMesh mesh, WorkspaceContext context) {
    workspace.handle(mesh,context);
};
static_assert(!PublicWorkspaceHandler<EditingWorkspaceUI>);
static_assert(!std::is_copy_constructible_v<MeshEditingUI>);
static_assert(!std::is_copy_constructible_v<EditingWorkspaceUI>);
static_assert(!std::is_copy_assignable_v<EditingWorkspaceUI>);
static_assert(!std::is_move_constructible_v<EditingWorkspaceUI>);
static_assert(!std::is_move_assignable_v<EditingWorkspaceUI>);
static_assert(std::same_as<decltype(std::declval<EditingWorkspaceUI&>().session()),const EditingSession&>);
static_assert(std::same_as<decltype(std::declval<EditingWorkspaceUI&>().selected_instances()),const editor::Selection<u32>&>);

// Const construction is the actual mutation boundary: UI/controller children
// must compile without receiving a mutable authoring authority at all.
static_assert(std::is_constructible_v<MeshEditingUI,const EditingSession&,ui::Container,ui::Container>);
static_assert(std::is_constructible_v<EditingViewportUI,const EditingSession&,ui::Container,ui::Container,ui::Container,ui::Container>);
static_assert(std::is_constructible_v<TimelineEditingUI,const EditingSession&,TimelineHosts>);
static_assert(std::is_constructible_v<MeshOperationControls,const EditingSession&>);
static_assert(std::is_constructible_v<BlueprintMeshPanel,ui::Container,const EditingSession&>);
static_assert(std::is_constructible_v<ViewportToolsUI,const EditingSession&,ui::Container,ui::Container,ui::Container,ui::Container>);
static_assert(std::is_constructible_v<InstanceTransformGizmo,const EditingSession&>);
static_assert(std::is_constructible_v<MeshTransformGizmo,const EditingSession&>);
static_assert(std::is_constructible_v<InstanceRotationGizmo,const EditingSession&>);
template<class T> concept MutableSessionEscape = requires(T& owner) { owner.session().undo(); };
static_assert(!MutableSessionEscape<EditingWorkspaceUI>);
template<class T> concept MutableDocumentEscape = requires(T& owner) { owner.state().document.revision=2; };
static_assert(!MutableDocumentEscape<EditingWorkspaceUI>);
template<class T> concept MutableSelectionEscape = requires(T& owner) { owner.selected_instances().clear(); };
static_assert(!MutableSelectionEscape<EditingWorkspaceUI>);
template<class T> concept ExternalOperationAcknowledgement = requires(T& child,MeshEditProposal proposal,content::Result<bool> result) {
    child.accept_operation(proposal,result);
};
static_assert(!ExternalOperationAcknowledgement<MeshEditingUI>);
static_assert(!ExternalOperationAcknowledgement<EditingViewportUI>);

// Only the actual owning parent may call these child handlers. In particular,
// dispatch must not remain a back door around the narrower ownership boundary.
template<class Component, class Situation, class Context>
constexpr bool private_to_owner =
    !requires(Component& child, const Situation& situation, const Context& context) {
        child.handle(situation, context);
    } && !requires(Component& child, const Situation& situation, const Context& context) {
        dispatch(child, situation, context);
    };
static_assert(private_to_owner<EditingViewportUI, InspectMesh, ViewportEditingContext>);
static_assert(private_to_owner<MeshEditingUI, InspectMesh, MeshEditingContext>);
static_assert(private_to_owner<MeshMenu, MeshMenu::Vertices, MeshMenuContext>);
static_assert(private_to_owner<MeshMenu, MeshMenu::Edges, MeshMenuContext>);
static_assert(private_to_owner<MeshMenu, MeshMenu::Faces, MeshMenuContext>);
static_assert(private_to_owner<MeshMenu, MeshMenu::Inactive, MeshMenuContext>);
static_assert(private_to_owner<MeshNavigationControls, MeshNavigationControls::MeshView, MeshNavigationControls::Context>);
static_assert(private_to_owner<ToolPanel, ToolPanel::Show, ToolPanel::Context>);
static_assert(private_to_owner<SceneLists, SceneLists::Browsing, SceneListsContext>);
static_assert(private_to_owner<TimelineEditingUI, TimelineEditingUI::Available, TimelineContext>);
static_assert(private_to_owner<TimelineEditingUI, TimelineEditingUI::Unavailable, TimelineContext>);
static_assert(private_to_owner<CameraNavigationLogic, CameraNavigationLogic::Orbiting, NavigationFrame>);
static_assert(private_to_owner<CameraNavigationLogic, CameraNavigationLogic::Walking, NavigationFrame>);
static_assert(private_to_owner<CameraNavigationLogic, CameraNavigationLogic::Unavailable, NavigationFrame>);

EditingWorkspaceUI timeline_workspace(State state, ui::Screen& screen, TimelineHosts hosts) {
    const auto hidden=[&] { return screen.column().visible(false); };
    return EditingWorkspaceUI{std::move(state),hidden(),hidden(),hidden(),
        {hidden(),hidden(),hidden(),hidden(),hidden()},hosts,hidden()};
}
}
TEST_CASE("Workspace dispatch owns mesh operations and emits precise results", "[editor][workspace]") {
    Fixture f;
    const auto original=f.session.state().document.mesh.document();
    f.send({.mode=MeshSelectMode::edge,.select_all=false});
    auto reply=f.send({.operation=MeshAction::subdivide});
    REQUIRE(reply.authored); REQUIRE(reply.operation_options);
    CHECK(reply.message.find("Subdivided")!=std::string::npos);
    CHECK(f.session.state().document.mesh.document()==original);
    REQUIRE(has_mesh_draft(f.session.state(),BlueprintId::mesh));
    REQUIRE(editable_mesh(f.session.state())->size()>4);
    REQUIRE(f.session.undo());
    CHECK_FALSE(f.session.can_undo());
    CHECK_FALSE(has_mesh_draft(f.session.state(),BlueprintId::mesh));
}
TEST_CASE("Mode and selection shortcuts take effect within the input batch", "[editor][workspace]") {
    Fixture f;
    const std::array keys{
        input::Event{.kind=input::EventKind::key_down,.key=input::Key::two},
        input::Event{.kind=input::EventKind::key_down,.key=input::Key::a},
        input::Event{.kind=input::EventKind::key_down,.key=input::Key::h}};
    auto reply=f.send({.shortcuts=keys});
    CHECK(reply.mode_changed); CHECK(reply.visibility_changed);
    CHECK(f.workspace.mesh_components().mode()==MeshSelectMode::edge);
    CHECK(f.workspace.mesh_components().visibility().hidden_faces.size()==2);
    CHECK(f.session.state().document.revision==1); CHECK_FALSE(f.session.can_undo());
    const std::array reveal{input::Event{.kind=input::EventKind::key_down,.key=input::Key::h,.modifiers={.alt=true}}};
    CHECK(f.send({.shortcuts=reveal}).visibility_changed);
    CHECK(f.workspace.mesh_components().visibility().hidden_faces.empty());
}
TEST_CASE("Workspace owns its model before UI attachment and rejects reattachment", "[editor][workspace]") {
    EditingWorkspaceUI workspace{Fixture::state()};
    CHECK(workspace.state().document.revision==1);
    CHECK_FALSE(workspace.can_undo());
    CHECK(workspace.debug_string().find("authoring authority and child coordination")!=std::string::npos);
    ui::Screen screen{ui::dark_theme(Fixture::font())};
    const auto attach=[&] {
        workspace.initialize(screen.column(),screen.column(),screen.column(),
            {screen.column(),screen.column(),screen.column(),screen.column(),screen.column()},
            {screen.column(),screen.column(),screen.column(),screen.column(),screen.column()},screen.column());
    };
    REQUIRE_NOTHROW(attach());
    CHECK_THROWS_AS(attach(),std::logic_error);
}
TEST_CASE("Workspace selection validates IDs and owns range order without authoring", "[editor][workspace][selection]") {
    auto state=Fixture::state();
    REQUIRE_FALSE(state.document.instances.empty());
    const auto prototype=state.document.instances.front();
    state.document.instances.clear();
    for(u32 id:{3U,7U,11U,15U}) {
        auto instance=prototype; instance.id=id;
        state.document.instances.push_back(std::move(instance));
    }
    state.viewport.selected_object=3;
    state.viewport.selected_vertex=2;
    EditingWorkspaceUI workspace{std::move(state)};
    REQUIRE(workspace.selected_instances().active()==3);
    CHECK(workspace.state().viewport.selected_vertex==2); // Observing a bookmark does not edit it.
    const auto revision=workspace.state().document.revision;
    const auto changed=workspace.select_instance(11,editor::SelectionMode::range);
    CHECK(changed.changed); CHECK(changed.active_changed);
    CHECK(changed.previous_active==3); CHECK(changed.active==11);
    CHECK(workspace.selected_instances().size()==3);
    CHECK(workspace.selected_instances().contains(7));
    CHECK(workspace.state().viewport.selected_object==11);
    CHECK(workspace.state().viewport.selected_vertex==0);
    CHECK_FALSE(workspace.select_instance(999).changed);
    REQUIRE(workspace.selected_instances().active()==11);
    workspace.select_instance(7,editor::SelectionMode::toggle);
    CHECK_FALSE(workspace.selected_instances().contains(7));
    const std::array<u32,4> ids{15,15,999,7};
    workspace.select_instances(ids);
    REQUIRE(workspace.selected_instances().size()==2);
    REQUIRE(workspace.selected_instances().active()==7);
    CHECK_FALSE(workspace.selected_instances().contains(999));
    CHECK(workspace.state().document.revision==revision);
    CHECK_FALSE(workspace.can_undo());
    CHECK_FALSE(workspace.dirty());
    CHECK(workspace.debug_string().find("workspace.selection")!=std::string::npos);
}
TEST_CASE("Workspace selection restores box origins and reconciles domain bookmarks", "[editor][workspace][selection]") {
    auto state=Fixture::state();
    const auto prototype=state.document.instances.front();
    state.document.instances.clear();
    for(u32 id:{3U,7U,11U}) {
        auto instance=prototype;instance.id=id;
        state.document.instances.push_back(std::move(instance));
    }
    state.viewport.selected_object=3;
    EditingWorkspaceUI workspace{std::move(state)};
    const auto origin=workspace.selected_instances();
    const std::array<u32,2> hits{7,11};
    workspace.select_instances(hits,editor::SelectionMode::add);
    REQUIRE(workspace.selected_instances().size()==3);
    REQUIRE(workspace.restore_selection(origin).active_changed);
    CHECK(workspace.selected_instances()==origin);
    CHECK(workspace.state().viewport.selected_object==3);
    workspace.select_instances(hits,editor::SelectionMode::add);
    workspace.viewport().selected_object=7; // A domain history/load bookmark.
    workspace.viewport().selected_vertex=2;
    workspace.reconcile_selection();
    CHECK(workspace.selected_instances().size()==1);
    REQUIRE(workspace.selected_instances().active()==7);
    CHECK(workspace.state().viewport.selected_vertex==2);
    workspace.select_instances(hits,editor::SelectionMode::add);
    REQUIRE(workspace.selected_instances().size()==2);
    workspace.viewport().selected_vertex=1;
    workspace.reset_selection(); // New document with overlapping instance IDs.
    REQUIRE(workspace.selected_instances().size()==1);
    CHECK(workspace.selected_instances().active()==11);
    CHECK(workspace.state().viewport.selected_vertex==1);
    CHECK(workspace.clear_selection().active_changed);
    CHECK(workspace.state().viewport.selected_object==0);
    CHECK(workspace.selected_instances().size()==0);
    CHECK_FALSE(workspace.clear_selection().changed);
}
TEST_CASE("Parent executes topology edits before the next selection shortcut", "[editor][workspace]") {
    Fixture f;
    const std::array keys{
        input::Event{.kind=input::EventKind::key_down,.key=input::Key::a},
        input::Event{.kind=input::EventKind::key_down,.key=input::Key::h}};
    auto reply=f.send({.mode=MeshSelectMode::face,.select_all=false,.operation=MeshAction::subdivide,.shortcuts=keys});
    REQUIRE(reply.authored);
    REQUIRE(reply.visibility_changed);
    REQUIRE(editable_mesh(f.workspace.state())->document().faces.size()==8);
    CHECK(f.workspace.mesh_components().visibility().hidden_faces.size()==8);
    CHECK_FALSE(reply.proposal); // A proposal cannot leak out as an unexecuted edit.
}
TEST_CASE("Operation options record intent and the parent executes it once", "[editor][workspace] [tool-options]") {
    Fixture f;
    auto reply=f.send({.mode=MeshSelectMode::edge,.select_all=false,.operation=MeshAction::subdivide});
    REQUIRE(reply.authored); REQUIRE(reply.operation_options);
    const auto revision=f.workspace.state().document.revision;
    const auto first_size=editable_mesh(f.workspace.state())->size();
    editor::Inspector options{{1,1,1}};
    reply.operation_options->describe_options(options);
    REQUIRE(options.dispatch({{1,1,1},"operation",editor::Phase::apply,{{"levels",u32{2}}}}));
    CHECK(f.workspace.state().document.revision==revision);
    CHECK(editable_mesh(f.workspace.state())->size()==first_size);
    REQUIRE(f.send().authored);
    CHECK(editable_mesh(f.workspace.state())->size()>first_size);
    const auto adjusted=f.workspace.state().document.revision;
    CHECK_FALSE(f.send().authored);
    CHECK(f.workspace.state().document.revision==adjusted);

    editor::Inspector current{{1,2,adjusted}};
    reply.operation_options->describe_options(current);
    REQUIRE(current.dispatch({{1,2,adjusted},"operation",editor::Phase::apply,{{"levels",u32{99}}}}));
    const auto rejected=f.send();
    CHECK_FALSE(rejected.authored); CHECK_FALSE(rejected.message.empty());
    CHECK(f.workspace.state().document.revision==adjusted);
    REQUIRE(current.dispatch({current.schema().stamp,"undo",editor::Phase::activate,{}}));
    CHECK(f.workspace.state().document.revision==adjusted);
    REQUIRE(f.send().authored);
    CHECK_FALSE(f.workspace.can_undo());
    CHECK_FALSE(has_mesh_draft(f.workspace.state(),BlueprintId::mesh));
}
TEST_CASE("Repeated no-op input does not erase an earlier visibility change", "[editor][workspace]") {
    Fixture f;
    f.send({.mode=MeshSelectMode::face,.select_all=false});
    const std::array keys{input::Event{.kind=input::EventKind::key_down,.key=input::Key::h},
        input::Event{.kind=input::EventKind::key_down,.key=input::Key::h}};
    CHECK(f.send({.shortcuts=keys}).visibility_changed);
    CHECK(f.workspace.mesh_components().visibility().hidden_faces.size()==2);
}
TEST_CASE("Input denial and stale situations cannot author a different target", "[editor][workspace]") {
    Fixture f;
    const auto mode=f.workspace.mesh_components().mode();
    f.send({.mode=MeshSelectMode::edge,.select_all=false,.operation=MeshAction::subdivide},false);
    CHECK(f.workspace.mesh_components().mode()==mode);
    CHECK(f.session.state().document.revision==1);
    const auto stale=InspectMesh{BlueprintId::mesh};
    f.session.viewport().mode=ViewMode::scene;
    auto reply=dispatch(f.workspace,stale,WorkspaceContext{MeshInput{.operation=MeshAction::subdivide}}).mesh;
    CHECK_FALSE(reply.authored); CHECK_FALSE(reply.message.empty());
    CHECK(f.session.state().document.revision==1);
    f.send({.mode=MeshSelectMode::edge,.select_all=false,.operation=MeshAction::subdivide});
    CHECK(f.session.state().document.revision==1);
}
TEST_CASE("Mesh menu click executes once and diagnostics do not change state", "[editor][workspace]") {
    Fixture f;
    f.send({.mode=MeshSelectMode::edge,.select_all=false,
        .open_menu=MeshMenuPlacement{{0,0},{1000,800},ui::Rect{0,0,1000,800}}});
    f.draw();
    const auto bounds=f.widget("Subdivide").bounds;
    const Vec2 at{bounds.x+10,bounds.y+10};
    f.draw({{.kind=input::EventKind::pointer_down,.position=at},
            {.kind=input::EventKind::pointer_up,.position=at}});
    const auto reply=f.send({.menu_events=f.frame.events,.poll_menu=true});
    REQUIRE(reply.authored);
    const auto revision=f.session.state().document.revision;
    CHECK_FALSE(f.send({.menu_events=f.frame.events,.poll_menu=true}).authored);
    CHECK(f.session.state().document.revision==revision);
    const auto report=f.workspace.debug_string();
    CHECK(report.find("workspace.viewport.mesh.components.menu")!=std::string::npos);
    CHECK(report.find("last requested action (not an execution result): Subdivide")!=std::string::npos);
    CHECK(report.find("last operation result: Subdivided")!=std::string::npos);
    CHECK(f.workspace.debug_string()==report);
    CHECK(f.session.state().document.revision==revision);
}
TEST_CASE("Mesh selection owner sends concrete menu contexts for every mode", "[editor][workspace]") {
    Fixture f;
    struct Case { MeshSelectMode mode; std::size_t selected; std::string_view situation; bool fill; };
    const std::array cases{
        Case{MeshSelectMode::vertex,0,"Vertices",false},
        Case{MeshSelectMode::vertex,1,"Vertices",false},
        Case{MeshSelectMode::vertex,2,"Vertices",true},
        Case{MeshSelectMode::edge,0,"Edges",false},
        Case{MeshSelectMode::edge,1,"Edges",true},
        Case{MeshSelectMode::face,0,"Faces",false},
        Case{MeshSelectMode::face,1,"Faces",true},
        Case{MeshSelectMode::surface,0,"Inactive",false},
        Case{MeshSelectMode::whole,0,"Inactive",false}};
    const std::array<u32,2> elements{0,1};
    const auto revision=f.session.state().document.revision;
    for(const auto& c:cases) {
        CAPTURE(c.situation,c.selected);
        const std::array selection{MeshSelectionInput{std::span{elements}.first(c.selected)}};
        auto reply=f.send({.mode=c.mode,.selection=selection,
            .open_menu=MeshMenuPlacement{{0,0},{1000,800},ui::Rect{0,0,1000,800}},.poll_menu=true});
        CHECK_FALSE(reply.authored);
        const auto report=f.workspace.mesh_components().debug_report().children.at(0);
        CHECK(report.situation==c.situation);
        CHECK(report.string().find("selected elements: "+std::to_string(c.selected))!=std::string::npos);
        if(c.situation=="Inactive") CHECK_FALSE(f.workspace.mesh_components().menu_open());
        else {
            REQUIRE(f.workspace.mesh_components().menu_open());
            f.draw();
            CHECK(f.widget("Make edge / face (F)").enabled==c.fill);
            CHECK(f.widget("Subdivide").enabled==(c.selected!=0));
        }
    }
    CHECK(f.session.state().document.revision==revision);
    CHECK_FALSE(f.session.can_undo());
}
TEST_CASE("Leaving component inspection closes menus but does not author anything", "[editor][workspace]") {
    Fixture f;
    f.send({.open_menu=MeshMenuPlacement{{0,0},{1000,800},{}}});
    REQUIRE(f.workspace.mesh_components().menu_open());
    f.session.viewport().mode=ViewMode::scene; f.send();
    CHECK_FALSE(f.workspace.mesh_components().menu_open());
    CHECK(f.session.state().document.revision==1);
    CHECK(f.workspace.debug_string().find("InspectScene / suspended")!=std::string::npos);
}

TEST_CASE("Viewport presentation never synchronizes instance catalogs", "[editor][workspace][performance]") {
    Fixture f;
    editor::Selection<u32> selection;
    const auto blueprints=blueprint_catalog(f.session.state());
    const auto send_lists=[&](SceneListsContext context) {
        return dispatch(f.workspace,workspace_situation(f.session.state().viewport),WorkspaceContext{.lists=context});
    };
    send_lists({.catalog=SceneListCatalog{scene_instances(f.session.state()),blueprints,selection}});
    const auto before=f.workspace.debug_report().children.at(1).string();
    const auto timeline_before=f.workspace.timeline_view().statistics();
    for(int i=0;i<100;++i) {
        (void)dispatch(f.workspace,workspace_situation(f.session.state().viewport),WorkspaceContext{
            .presentation=ViewportPresentation{{0,0,800,600},false,true,false}});
    }
    CHECK(f.workspace.debug_report().children.at(1).string()==before);
    CHECK(f.workspace.timeline_view().statistics()==timeline_before);
    const auto view=f.workspace.debug_report().children.at(0).string();
    send_lists({.enabled=false});
    CHECK(f.workspace.debug_report().children.at(0).string()==view);
    CHECK(f.session.state().document.revision==1);
}
TEST_CASE("Workspace publishes selection to list and timeline children without document refresh", "[editor][workspace][selection][performance]") {
    Fixture f;
    const auto blueprints=blueprint_catalog(f.workspace.state());
    dispatch(f.workspace,workspace_situation(f.workspace.state().viewport),WorkspaceContext{
        .lists=SceneListsContext{.catalog=SceneListCatalog{scene_instances(f.workspace.state()),blueprints,f.workspace.selected_instances()}},
        .timeline=TimelineContext{.input={.synchronize=true}}});
    const auto before=f.workspace.debug_report().children.at(1);
    const auto timeline_before=f.workspace.timeline_view().statistics();
    const auto id=f.workspace.state().document.instances.front().id;
    f.workspace.clear_selection();
    f.workspace.select_instance(id);
    const auto after=f.workspace.debug_report().children.at(1);
    for(std::size_t i=0;i<before.children.size();++i) {
        CHECK(after.children[i].owned[1].value==before.children[i].owned[1].value); // Catalog syncs.
        CHECK(after.children[i].owned[2].value==before.children[i].owned[2].value); // Rows created.
    }
    const auto report=f.workspace.debug_string();
    CHECK(report.find("selected object IDs: 1")!=std::string::npos);
    CHECK(report.find("pending object focus: none")!=std::string::npos); // No keyframe selected.
    CHECK(f.workspace.timeline_view().statistics().document_refreshes==timeline_before.document_refreshes);
    CHECK(f.workspace.timeline_view().statistics().value_refreshes==timeline_before.value_refreshes);
    CHECK(f.workspace.state().document.revision==1);
    CHECK_FALSE(f.workspace.can_undo());
    f.draw();
    const auto widgets=f.screen.inspect(); REQUIRE(widgets);
    const auto expected="> #"+std::to_string(id)+" "+f.workspace.state().document.instances.front().name;
    CHECK(std::ranges::any_of(widgets->widgets,[&](const auto& w){return w.text==expected;}));
}

TEST_CASE("Timeline selection is owned locally and availability survives narrow updates", "[editor][workspace][timeline]") {
    Fixture f;
    f.session.viewport().mode=ViewMode::scene;
    auto send=[&](TimelineContext context) {
        return dispatch(f.workspace,workspace_situation(f.session.state().viewport),WorkspaceContext{.timeline=context}).timeline;
    };
    send({.input={.synchronize=true,.clear_selection=true}});
    CHECK_FALSE(f.workspace.timeline_view().selected_keyframe());
    CHECK_FALSE(f.session.can_edit_scene_pose());
    const std::array time{0.F};
    send({.input={.select=std::span<const f32>{time}}});
    REQUIRE(f.workspace.timeline_view().selected_keyframe()==0.F);
    CHECK(f.session.can_edit_scene_pose());
    send({.enabled=false});
    const auto revision=f.session.state().document.revision;
    // Parent mode changes/presentation must not silently re-enable the leaf.
    dispatch(f.workspace,InspectScene{},WorkspaceContext{.presentation=ViewportPresentation{{0,0,800,600}}});
    send({.input={.objects=std::span<const u64>{}}});
    CHECK(f.workspace.debug_report().children.at(2).situation=="Unavailable");
    send({.input={.clear_selection=true}});
    CHECK_FALSE(f.session.can_edit_scene_pose());
    CHECK(f.session.state().document.revision==revision);
}

TEST_CASE("Navigation parent routes walk orbit and blocked contexts without losing focus state", "[editor][workspace][navigation]") {
    Fixture f;
    ViewportToolsUI interaction{f.session.session(),f.screen.column(),f.screen.column(),f.screen.column(),f.screen.column()};
    const auto& navigation=interaction.camera_navigation();
    input::Frame raw{.logical_size={800,600},.framebuffer={800,600},.focused=true};
    raw.events={{.kind=input::EventKind::key_down,.key=input::Key::w}};
    NavigationFrame frame{.pose={0,0,8,{},1},.mode=ViewMode::scene,
        .viewport={0,0,800,600},.raw=raw,.unhandled=raw.events,.seconds=.016,
        .drag_speeds={},.walk_speeds={}};
    auto reply=dispatch(interaction,ViewportToolsUI::Navigate{},NavigationContext{.frame=frame,.walk_active=true});
    CHECK(reply.changed);
    CHECK(reply.pose.target.z<0.F);
    CHECK(navigation.walking().moving());
    frame.pose=reply.pose;
    raw.events.clear(); frame.unhandled={}; raw.focused=false; frame.controls_have_focus=true;
    reply=dispatch(interaction,ViewportToolsUI::Navigate{},NavigationContext{.frame=frame});
    CHECK_FALSE(reply.changed);
    CHECK(navigation.walking().active());
    CHECK_FALSE(navigation.walking().moving());
    raw.focused=true; frame.controls_have_focus=false;
    raw.events={{.kind=input::EventKind::key_down,.key=input::Key::w}}; frame.unhandled=raw.events;
    reply=dispatch(interaction,ViewportToolsUI::Navigate{},NavigationContext{.frame=frame,.enabled=false});
    CHECK_FALSE(reply.changed); CHECK_FALSE(navigation.walking().moving());
    CHECK(reply.pose==frame.pose);
    const auto report=navigation.debug_string();
    CHECK(report.find("Unavailable")!=std::string::npos);
    CHECK(navigation.debug_string()==report);
    raw.events.clear(); frame.unhandled={};
    reply=dispatch(interaction,ViewportToolsUI::Navigate{},NavigationContext{.frame=frame,.walk_active=false});
    CHECK(navigation.debug_report().situation=="Orbiting");
    CHECK_FALSE(navigation.walking().active());
    CHECK_FALSE(reply.changed);
}

TEST_CASE("Timeline owner applies a clicked keyframe insertion exactly once", "[editor][workspace][timeline]") {
    auto state=Fixture::state(); state.viewport.mode=ViewMode::scene; state.viewport.time=3;
    ui::Screen screen{ui::dark_theme(Fixture::font())};
    auto workspace=timeline_workspace(std::move(state),screen,{screen.column().position({0,850}).width(1760).height(160),
        screen.column().position({0,0}).width(300).height(800),
        screen.column().position({310,0}).width(760).height(800),
        screen.row().position({1100,0}).width(440).height(36),screen.column()});
    auto& session=workspace;
    const auto send=[&](TimelineContext context) {
        return dispatch(workspace,workspace_situation(session.state().viewport),WorkspaceContext{.timeline=context}).timeline;
    };
    input::Frame raw{.logical_size={1800,1100},.framebuffer={1800,1100}};
    send({.input={.synchronize=true}});
    REQUIRE(screen.update(raw,.016F));
    REQUIRE(screen.draw_list());
    auto widgets=screen.inspect(); REQUIRE(widgets);
    const auto button=std::ranges::find_if(widgets->widgets,[](const auto& w){return w.visible&&w.label=="Add keyframe";});
    REQUIRE(button!=widgets->widgets.end());
    const Vec2 at{button->bounds.x+10,button->bounds.y+10};
    raw.pointer=at;
    raw.events={{.kind=input::EventKind::pointer_down,.position=at},{.kind=input::EventKind::pointer_up,.position=at}};
    auto ui=screen.update(raw,.016F); REQUIRE(ui);
    const auto before=session.state().document.revision;
    const auto reply=send({
        .input={.poll=true,.unhandled=ui->events,.raw=raw.events}});
    CHECK(reply.interacted); CHECK(reply.authored); CHECK(reply.select_inspector);
    REQUIRE(workspace.timeline_view().selected_keyframe()==3.F);
    CHECK(session.can_edit_scene_pose()); CHECK(session.state().document.revision==before+1);
    raw.events.clear(); REQUIRE(screen.update(raw,.016F));
    CHECK_FALSE(send({.input={.poll=true}}).authored);
    CHECK(session.state().document.revision==before+1);
    REQUIRE(session.undo());
    CHECK_FALSE(session.can_undo());
}

TEST_CASE("Hidden timeline inspector reveals only the latest requested object when shown", "[editor][workspace][timeline]") {
    auto state=Fixture::state(); state.viewport.mode=ViewMode::scene;
    REQUIRE_FALSE(state.document.instances.empty());
    const auto prototype=state.document.instances.front();
    for(u32 id=3;id<=30;++id) {
        auto instance=prototype; instance.id=id; instance.name="Fleet ship "+std::to_string(id);
        state.document.instances.push_back(std::move(instance));
    }
    ui::Screen screen{ui::dark_theme(Fixture::font())};
    auto inspector=screen.column().position({310,0}).width(760).height(600).visible(false);
    auto workspace=timeline_workspace(std::move(state),screen,{screen.column().position({0,650}).width(1760).height(160),
        screen.column().position({0,0}).width(300).height(600),inspector,
        screen.row().position({1100,0}).width(440).height(36),screen.column()});
    auto& session=workspace;
    REQUIRE(session.add_keyframe(1.F));
    const auto send=[&](TimelineContext context) {
        return dispatch(workspace,workspace_situation(session.state().viewport),WorkspaceContext{.timeline=context}).timeline;
    };
    input::Frame raw{.logical_size={1800,900},.framebuffer={1800,900}};
    const std::array selected{1.F};
    send({.input={
        .synchronize=true,.select=std::span<const f32>{selected},.inspector_visible=false}});
    REQUIRE(screen.update(raw,.016F));
    const auto revision=session.state().document.revision;
    const auto timeline_before=workspace.timeline_view().statistics();
    workspace.select_instance(3);
    workspace.select_instance(30);
    CHECK(workspace.timeline_view().statistics().document_refreshes==timeline_before.document_refreshes);
    CHECK(workspace.timeline_view().statistics().value_refreshes==timeline_before.value_refreshes);
    REQUIRE(screen.update(raw,.016F));
    CHECK(workspace.debug_string().find("pending object focus: 30")!=std::string::npos);
    inspector.visible(true);
    send({.input={.inspector_visible=true}});
    REQUIRE(screen.update(raw,.016F));
    auto widgets=screen.inspect(); REQUIRE(widgets);
    CHECK(std::ranges::any_of(widgets->widgets,[](const auto& w) {
        return w.visible && w.label=="Mesh / Fleet ship 30" && w.clip.width>0 && w.clip.height>0;
    }));
    CHECK(std::ranges::any_of(widgets->widgets,[](const auto& w) {
        return w.visible && w.focused && w.label=="Key";
    }));
    CHECK(workspace.debug_string().find("pending object focus: none")!=std::string::npos);
    CHECK(session.state().document.revision==revision);
}

TEST_CASE("List flyouts share catalog selection and close without resetting mesh state", "[editor][workspace]") {
    Fixture f;
    editor::Selection<u32> selection; selection.select(1);
    const auto blueprints=blueprint_catalog(f.session.state());
    const auto catalog=SceneListCatalog{scene_instances(f.session.state()),blueprints,selection};
    const auto send=[&](SceneListsContext context) {
        return dispatch(f.workspace,workspace_situation(f.session.state().viewport),WorkspaceContext{.lists=context});
    };
    send({.catalog=catalog,.presentation=SceneListPresentation{{1000,800},{10,10,100,30},{150,10,100,30}},
        .input={.toggle=SceneListInput::Toggle::instances}});
    CHECK(f.workspace.list_flyout_open());
    const auto mesh_before=f.workspace.mesh_components().selection_revision();
    send({.input={.close=true}});
    CHECK_FALSE(f.workspace.list_flyout_open());
    send({.catalog=catalog,.input={.toggle=SceneListInput::Toggle::blueprints}});
    CHECK(f.workspace.list_flyout_open());
    CHECK(f.workspace.mesh_components().selection_revision()==mesh_before);
    const std::array escape{input::Event{.kind=input::EventKind::key_down,.key=input::Key::escape}};
    send({.input={.events=escape,.poll_flyouts=true}});
    CHECK_FALSE(f.workspace.list_flyout_open());
    CHECK(f.session.state().document.revision==1);
}
