#include "../../examples/editor/workspace.hpp"
#include "../../examples/editor/camera_navigation.hpp"
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
    EditingSession session{state()};
    ui::Screen screen{ui::dark_theme(font())};
    EditingWorkspace workspace{session,screen.column().width(300).height(400),screen.column(),screen.column(),
        {screen.column(),screen.column(),screen.column(),screen.column(),screen.column()},
        {screen.column().visible(false),screen.column().visible(false),screen.column().visible(false),screen.column().visible(false),screen.column().visible(false)},screen.column().visible(false)};
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
static_assert(!PublicWorkspaceHandler<EditingWorkspace>);
static_assert(!std::is_copy_constructible_v<MeshEditing>);
static_assert(!std::is_copy_constructible_v<EditingWorkspace>);
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

TEST_CASE("Navigation dispatch preserves walk arming across popout focus and releases blocked keys", "[editor][workspace][navigation]") {
    CameraNavigation navigation;
    navigation.walking(true);
    input::Frame raw{.logical_size={800,600},.framebuffer={800,600},.focused=true};
    raw.events={{.kind=input::EventKind::key_down,.key=input::Key::w}};
    NavigationFrame frame{.pose={0,0,8,{},1},.mode=ViewMode::scene,
        .viewport={0,0,800,600},.raw=raw,.unhandled=raw.events,.seconds=.016,
        .drag_speeds={},.walk_speeds={}};
    auto reply=dispatch(navigation,CameraNavigation::Walking{},frame);
    CHECK(reply.changed);
    CHECK(reply.pose.target.z<0.F);
    CHECK(navigation.walking().moving());
    frame.pose=reply.pose;
    raw.events.clear(); frame.unhandled={}; raw.focused=false; frame.controls_have_focus=true;
    reply=dispatch(navigation,CameraNavigation::Walking{},frame);
    CHECK_FALSE(reply.changed);
    CHECK(navigation.walking().active());
    CHECK_FALSE(navigation.walking().moving());
    raw.focused=true; frame.controls_have_focus=false;
    raw.events={{.kind=input::EventKind::key_down,.key=input::Key::w}}; frame.unhandled=raw.events;
    reply=dispatch(navigation,CameraNavigation::Unavailable{},frame);
    CHECK_FALSE(reply.changed); CHECK_FALSE(navigation.walking().moving());
    CHECK(reply.pose==frame.pose);
    const auto report=navigation.debug_string();
    CHECK(report.find("Unavailable")!=std::string::npos);
    CHECK(navigation.debug_string()==report);
}

TEST_CASE("Timeline owner applies a clicked keyframe insertion exactly once", "[editor][workspace][timeline]") {
    EditingSession session{Fixture::state()}; session.viewport().mode=ViewMode::scene; session.viewport().time=3;
    ui::Screen screen{ui::dark_theme(Fixture::font())};
    TimelineEditing timeline{session,{screen.column().position({0,850}).width(1760).height(160),
        screen.column().position({0,0}).width(300).height(800),
        screen.column().position({310,0}).width(760).height(800),
        screen.row().position({1100,0}).width(440).height(36),screen.column()}};
    input::Frame raw{.logical_size={1800,1100},.framebuffer={1800,1100}};
    dispatch(timeline,TimelineEditing::Available{},TimelineContext{.input={.synchronize=true}});
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
    const auto reply=dispatch(timeline,TimelineEditing::Available{},TimelineContext{
        .input={.poll=true,.unhandled=ui->events,.raw=raw.events}});
    CHECK(reply.interacted); CHECK(reply.authored); CHECK(reply.select_inspector);
    REQUIRE(timeline.view().selected_keyframe()==3.F);
    CHECK(session.can_edit_scene_pose()); CHECK(session.state().document.revision==before+1);
    raw.events.clear(); REQUIRE(screen.update(raw,.016F));
    CHECK_FALSE(dispatch(timeline,TimelineEditing::Available{},TimelineContext{.input={.poll=true}}).authored);
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
    EditingSession session{std::move(state)};
    REQUIRE(session.add_keyframe(1.F));
    ui::Screen screen{ui::dark_theme(Fixture::font())};
    auto inspector=screen.column().position({310,0}).width(760).height(600).visible(false);
    TimelineEditing timeline{session,{screen.column().position({0,650}).width(1760).height(160),
        screen.column().position({0,0}).width(300).height(600),inspector,
        screen.row().position({1100,0}).width(440).height(36),screen.column()}};
    input::Frame raw{.logical_size={1800,900},.framebuffer={1800,900}};
    const std::array selected{1.F};
    dispatch(timeline,TimelineEditing::Available{},TimelineContext{.input={
        .synchronize=true,.select=std::span<const f32>{selected},.inspector_visible=false}});
    REQUIRE(screen.update(raw,.016F));
    const auto revision=session.state().document.revision;
    dispatch(timeline,TimelineEditing::Available{},TimelineContext{.input={.focus=3}});
    dispatch(timeline,TimelineEditing::Available{},TimelineContext{.input={.focus=30}});
    REQUIRE(screen.update(raw,.016F));
    CHECK(timeline.debug_string().find("pending object focus: 30")!=std::string::npos);
    inspector.visible(true);
    dispatch(timeline,TimelineEditing::Available{},TimelineContext{.input={.inspector_visible=true}});
    REQUIRE(screen.update(raw,.016F));
    auto widgets=screen.inspect(); REQUIRE(widgets);
    CHECK(std::ranges::any_of(widgets->widgets,[](const auto& w) {
        return w.visible && w.label=="Mesh / Fleet ship 30" && w.clip.width>0 && w.clip.height>0;
    }));
    CHECK(std::ranges::any_of(widgets->widgets,[](const auto& w) {
        return w.visible && w.focused && w.label=="Key";
    }));
    CHECK(timeline.debug_string().find("pending object focus: none")!=std::string::npos);
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
