#include "../../examples/editor/viewport_input.hpp"
#include "../../examples/editor/gizmo_input.hpp"
#include "../../examples/editor/workspace.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

namespace {
using namespace vng;
using namespace editor_example;
struct WorkspaceFixture {
    static text::Font font() {auto value=text::Font::load(VNG_TEST_FONT_PATH);REQUIRE(value);return std::move(*value);}
    static State initial() {
        content::vmesh::Document mesh;mesh.vertex_count=3;
        // Mean vertex position is the origin, so rotation around its own center
        // does not intentionally translate the instance after the move check.
        mesh.vertex_fields={{"position",{content::vmesh::ScalarType::Float32,3},std::vector<f32>{-1,-1,0,1,-1,0,0,2,0}}};
        mesh.faces={{0,1,2}};
        auto editable=editor::EditableMesh::create(mesh);REQUIRE(editable);
        State result{.document={.mesh=std::move(*editable)}};
        instance_transform(result,1)->position={};
        return result;
    }
    text::Font face{font()};
    ui::Screen screen{ui::dark_theme(face)};
    EditingWorkspace workspace{initial()};
    gfx::Camera camera;
    input::EventSequence sequence;
    WorkspaceFixture() {
        workspace.initialize(screen.column(),screen.column(),screen.column(),
            {screen.column(),screen.column(),screen.column(),screen.column(),screen.column()},
            {screen.column(),screen.column(),screen.column(),screen.column(),screen.column()},screen.column());
        workspace.attach_viewport_tools(screen.column(),screen.column(),screen.column(),screen.column(),screen.column());
        workspace.attach_manipulation(screen.column(),screen.column());
        workspace.select_keyframe(0);
        workspace.select_instance(1); // The document's normal initial selection is Sun (#2).
        REQUIRE(workspace.state().viewport.selected_object==1);
        REQUIRE(std::ranges::equal(workspace.selected_instances().items(),std::array<u32,1>{1}));
        workspace.gizmo_selector().show(workspace.state(),workspace.selected_instances().items());
        camera.set_position({0,0,10}).look_at({}).set_orthographic({.vertical_height=10});
    }
    std::vector<ViewportInputReply> pump(std::initializer_list<input::Event> events,double seconds=.02) {
        input::Frame raw{.logical_size={800,600},.framebuffer={800,600},.pointer={460,300}};
        raw.events.assign(events.begin(),events.end());sequence.identify(raw.events);
        if(!raw.events.empty())raw.pointer=raw.events.back().position;
        workspace.begin_viewport_frame();
        ViewportInputSteps steps{raw,raw.events,seconds};
        std::vector<ViewportInputReply> replies;
        while(auto step=steps.next()) {
            ViewportInputContext context{
                .input={step->frame,step->unhandled,step->seconds,seconds,step->tick},
                .presented={camera,{800,600},{0,0,800,600},1,workspace.state().document.revision,0,true,true},
                .tools={.enabled=true,.ready=true,.scene_aids_visible=true}};
            replies.push_back(workspace.interact_viewport(context));
        }
        return replies;
    }
};
}

TEST_CASE("Viewport descends in occurrence order and advances time once", "[editor][input][parent-coordination]") {
    input::Frame raw{.logical_size={800,600},.framebuffer={800,600},.pointer={400,300}};
    raw.events={{.kind=input::EventKind::pointer_down,.position={200,100}},
                {.kind=input::EventKind::pointer_up,.position={200,100}},
                {.kind=input::EventKind::pointer_down,.position={200,100}},
                {.kind=input::EventKind::pointer_move,.position={400,300}}};
    input::EventSequence sequence;sequence.identify(raw.events);
    const std::array available{raw.events[2],raw.events[3]};
    ViewportInputSteps steps{raw,available,.02};
    unsigned occurrences{},ticks{};double elapsed{};
    while(auto step=steps.next()) {
        elapsed+=step->seconds;
        CHECK(step->frame.logical_size==raw.logical_size);
        CHECK(step->frame.framebuffer==raw.framebuffer);
        if(step->tick) {
            ++ticks;CHECK(step->frame.events.empty());CHECK(step->unhandled.empty());
            CHECK(step->frame.pointer==raw.pointer);CHECK(step->seconds==.02);
        } else {
            REQUIRE(step->frame.events.size()==1);
            CHECK(step->frame.events.front().routing_id==raw.events[occurrences].routing_id);
            CHECK(step->unhandled.size()==(occurrences>=2?1:0));
            CHECK(step->frame.pointer==raw.events[occurrences].position);
            CHECK(step->seconds==0);++occurrences;
        }
    }
    CHECK(occurrences==4);CHECK(ticks==1);CHECK(elapsed==.02);
    CHECK_FALSE(steps.next());
}

TEST_CASE("Viewport exposes lifecycle releases even when UI consumed the occurrence", "[editor][input][parent-coordination]") {
    input::Frame raw{.focused=false};
    raw.events={{.kind=input::EventKind::key_down,.key=input::Key::w},
                {.kind=input::EventKind::key_up,.key=input::Key::w},
                {.kind=input::EventKind::focus_lost}};
    input::EventSequence sequence;sequence.identify(raw.events);
    ViewportInputSteps steps{raw,{},.016};
    auto step=steps.next();REQUIRE(step);
    CHECK(step->unhandled.empty());CHECK(step->frame.keyDown(input::Key::w));CHECK(step->frame.focused);
    const auto release=steps.next();REQUIRE(release);
    CHECK(release->unhandled.empty());CHECK_FALSE(release->frame.keyDown(input::Key::w));CHECK(release->frame.focused);
    const auto lost=steps.next();REQUIRE(lost);
    CHECK(lost->frame.events.front().kind==input::EventKind::focus_lost);CHECK_FALSE(lost->frame.focused);
    const auto tick=steps.next();REQUIRE(tick);CHECK(tick->tick);CHECK_FALSE(tick->frame.focused);
}

TEST_CASE("An idle viewport still receives exactly one held-control tick", "[editor][input][parent-coordination]") {
    input::Frame raw;
    ViewportInputSteps steps{raw,{},.025};
    auto step=steps.next();REQUIRE(step);CHECK(step->tick);CHECK(step->seconds==.025);
    CHECK(step->frame.events.empty());CHECK_FALSE(steps.next());
}

TEST_CASE("Segmented gizmo arrows respond immediately but never multiply elapsed time", "[editor][input][gizmo][parent-coordination]") {
    GizmoInput input;
    const Vec2 pointer{400,300};
    const std::array press{vng::input::Event{.kind=vng::input::EventKind::key_down,.position=pointer,.key=vng::input::Key::right}};
    const std::array motion{vng::input::Event{.kind=vng::input::EventKind::pointer_move,.position={405,300}}};
    const auto arrows=[&] {
        return std::ranges::count_if(input.events(),[](const auto& event) {
            return event.kind==vng::input::EventKind::key_down&&event.key==vng::input::Key::right;
        });
    };
    input.begin_frame();
    input.route(true,false,false,pointer,press,press,.02F,true,GizmoInput::Phase::event);
    CHECK(arrows()==1);CHECK(input.arrow_step()==Catch::Approx(1.2F));
    for(unsigned i=0;i<8;++i) {
        input.route(true,false,false,pointer,motion,motion,.02F,true,GizmoInput::Phase::event);
        CHECK(arrows()==0);
    }
    input.route(true,false,false,pointer,{},{},.02F,true,GizmoInput::Phase::tick);CHECK(arrows()==0);
    input.begin_frame();
    input.route(true,false,false,pointer,motion,motion,.02F,true,GizmoInput::Phase::event);CHECK(arrows()==0);
    input.route(true,false,false,pointer,{},{},.02F,true,GizmoInput::Phase::tick);CHECK(arrows()==1);
    input.route(true,false,false,pointer,{},{},.02F,true,GizmoInput::Phase::tick);CHECK(arrows()==0);
    // A complete tap in one frame still gets its initial movement.
    input.begin_frame();
    const std::array release{vng::input::Event{.kind=vng::input::EventKind::key_up,.position=pointer,.key=vng::input::Key::right}};
    input.route(true,false,false,pointer,release,release,.02F,true,GizmoInput::Phase::event);
    input.route(true,false,false,pointer,press,press,.02F,true,GizmoInput::Phase::event);CHECK(arrows()==1);
    input.route(true,false,false,pointer,release,release,.02F,true,GizmoInput::Phase::event);
    input.route(true,false,false,pointer,{},{},.02F,true,GizmoInput::Phase::tick);CHECK(arrows()==0);
}

TEST_CASE("Workspace owns consecutive gestures in one ordered viewport batch", "[editor][input][parent-coordination][viewport]") {
    WorkspaceFixture f;
    const auto before=f.workspace.state().document.instances;
    const auto replies=f.pump({
        {.kind=input::EventKind::key_down,.position={460,300},.key=input::Key::g},
        {.kind=input::EventKind::pointer_move,.position={520,300}},
        {.kind=input::EventKind::key_down,.position={520,300},.key=input::Key::enter},
        {.kind=input::EventKind::key_down,.position={520,300},.key=input::Key::r},
        {.kind=input::EventKind::pointer_move,.position={460,240}},
        {.kind=input::EventKind::key_down,.position={460,240},.key=input::Key::enter}});
    REQUIRE(replies.size()==7);
    CHECK(replies[2].pose_finished);CHECK(replies[5].pose_finished);
    CHECK_FALSE(f.workspace.busy());
    const auto transformed=evaluate_transform(f.workspace.state(),*find_instance(f.workspace.state(),1),0);
    CHECK(transformed.position.x==Catch::Approx(1));CHECK(transformed.rotation!=Vec3{});
    REQUIRE(f.workspace.undo());
    CHECK(evaluate_transform(f.workspace.state(),*find_instance(f.workspace.state(),1),0).rotation==Vec3{});
    REQUIRE(f.workspace.undo());CHECK_FALSE(f.workspace.can_undo());
    CHECK(f.workspace.state().document.instances==before);
    const auto report=f.workspace.debug_string();
    CHECK(report.find("blueprint_recipe")!=std::string::npos);
    CHECK(report.find("interaction")!=std::string::npos);
    CHECK(report.find("viewport.gizmo_selector")!=std::string::npos);
    CHECK(report.find("viewport.rotation_origin")!=std::string::npos);
}

TEST_CASE("Workspace advances held transform arrows once despite many pointer occurrences", "[editor][input][parent-coordination][viewport]") {
    WorkspaceFixture f;
    f.pump({{.kind=input::EventKind::key_down,.position={460,300},.key=input::Key::g}});
    f.pump({{.kind=input::EventKind::key_down,.position={460,300},.key=input::Key::right},
            {.kind=input::EventKind::pointer_move,.position={460,300}},
            {.kind=input::EventKind::pointer_move,.position={460,300}},
            {.kind=input::EventKind::pointer_move,.position={460,300}}});
    const auto moved=evaluate_transform(f.workspace.state(),*find_instance(f.workspace.state(),1),0).position.x;
    CHECK(moved==Catch::Approx(.1F).margin(.0001F));
    f.pump({});
    const auto held=evaluate_transform(f.workspace.state(),*find_instance(f.workspace.state(),1),0).position.x;
    CHECK(held==Catch::Approx(.2F).margin(.0001F));
    f.pump({{.kind=input::EventKind::key_down,.position={460,300},.key=input::Key::escape}});
    CHECK_FALSE(f.workspace.busy());CHECK_FALSE(f.workspace.can_undo());
    CHECK(evaluate_transform(f.workspace.state(),*find_instance(f.workspace.state(),1),0).position==Vec3{});
}

TEST_CASE("Refreshing viewport presentation preserves captured free rotation", "[editor][input][parent-coordination][viewport]") {
    WorkspaceFixture f;
    f.workspace.gizmo_selector().value(GizmoMode::free_rotate);
    f.pump({{.kind=input::EventKind::pointer_down,.position={460,300},.button=2},
            {.kind=input::EventKind::pointer_move,.position={480,320}}});
    REQUIRE(f.workspace.interaction().rotation.active());
    const auto token=f.workspace.session().active_transaction();
    input::Frame raw{.logical_size={800,600},.framebuffer={800,600},.pointer={480,320}};
    f.workspace.refresh_viewport_gizmos({
        .input={raw,{}},
        .presented={f.camera,{800,600},{0,0,800,600},1,f.workspace.state().document.revision,0,true,true},
        .tools={.enabled=true,.ready=true}});
    CHECK(f.workspace.interaction().rotation.active());
    CHECK(f.workspace.session().active_transaction()==token);
    f.pump({{.kind=input::EventKind::key_down,.position={480,320},.key=input::Key::escape}});
    CHECK_FALSE(f.workspace.busy());CHECK_FALSE(f.workspace.can_undo());
}

TEST_CASE("Region pointer-down observations preserve multi-selection modifiers", "[editor][input][parent-coordination][viewport][region]") {
    WorkspaceFixture f;
    const auto added=f.workspace.instantiate(BlueprintId::region);REQUIRE(added);
    f.workspace.select_instance(1);
    f.pump({});
    const auto point=f.workspace.interaction().regions.tool().point_handle(*added,0);REQUIRE(point);
    f.pump({{.kind=input::EventKind::pointer_down,.position=*point,.modifiers={.shift=true}}});
    CHECK(f.workspace.selected_instances().contains(1));
    CHECK(f.workspace.selected_instances().contains(*added));
    const auto edge=f.workspace.interaction().regions.tool().element_handle(editor::CageElement::edge,0);REQUIRE(edge);
    f.pump({{.kind=input::EventKind::pointer_up,.position=*point},
            {.kind=input::EventKind::pointer_down,.position=*edge,.modifiers={.control=true}}});
    CHECK(f.workspace.selected_instances().contains(1));
    CHECK_FALSE(f.workspace.selected_instances().contains(*added));
}
