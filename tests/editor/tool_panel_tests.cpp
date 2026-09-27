#include "../../examples/editor/tool_panel.hpp"
#include "../../examples/editor/transform_gesture.hpp"
#include "../../examples/editor/mesh_operation_controls.hpp"
#include "../../examples/editor/gizmo_controls.hpp"
#include "../../examples/editor/mesh_navigation.hpp"
#include "../../examples/editor/camera_panel.hpp"
#include "../../examples/editor/selection_input_logic.hpp"
#include <vng/ui/inspection.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

namespace {
using namespace vng;
using namespace editor_example;
text::Font font(){auto f=text::Font::load(VNG_TEST_FONT_PATH);REQUIRE(f);return *f;}
struct Fixture {
    ui::Screen screen{ui::dark_theme(font())};
    ToolPanel panel{screen.column()};
    input::Frame frame{.logical_size={1000,800},.framebuffer={1000,800}};
    Fixture(){panel.layout({100,100,800,600});}
    void pump(std::initializer_list<input::Event> events={}) {
        frame.events=events;
        for(const auto& e:events)if(e.kind==input::EventKind::pointer_down||e.kind==input::EventKind::pointer_up)frame.pointer=e.position;
        panel.layout({100,100,800,600});
        REQUIRE(screen.update(frame,.016F));panel.poll();REQUIRE(screen.draw_list());
    }
    ui::WidgetSnapshot widget(std::string_view name) {
        const auto snapshot=screen.inspect();REQUIRE(snapshot);
        for(const auto& item:snapshot->widgets)if(item.visible&&(item.label==name||item.text==name))return item;
        FAIL("Missing visible tool control: "<<name);return {};
    }
    void click(std::string_view name) {
        const auto r=widget(name).bounds;const Vec2 p{r.x+r.width*.5F,r.y+r.height*.5F};
        pump({{.kind=input::EventKind::pointer_down,.position=p},{.kind=input::EventKind::pointer_up,.position=p}});
    }
};
struct CustomTool final : ToolOptions {
    bool available{true};int calls{};u64 revision{},executed_revision{};
    u64 options_revision()const override{return revision;}
    std::string_view title() const override{return "Blueprint custom tool";}
    bool options_available() const override{return available;}
    void describe_options(editor::Inspector& ui) override {
        ui.action("custom",[this,described_revision=revision]{++calls;executed_revision=described_revision;},"Custom action");
    }
};
}

TEST_CASE("Gizmo option revisions refresh same-owner menus and reopen a changed gizmo", "[editor][ui][tool-options]") {
    CustomTool source;GizmoControls input;Fixture f;
    REQUIRE(input.show(true,&source));f.panel.show(input);f.pump();
    f.click("Custom action");CHECK(source.calls==1);
    f.click("Close tool options");CHECK_FALSE(f.panel.opened());
    CHECK_FALSE(input.show(true,&source));
    ++source.revision;
    REQUIRE(input.show(true,&source));f.panel.show(input,true);f.pump();
    CHECK(f.panel.opened());f.click("Custom action");CHECK(source.calls==2);CHECK(source.executed_revision==1);
    ++source.revision;f.panel.validate();f.pump();
    f.click("Custom action");CHECK(source.calls==3);CHECK(source.executed_revision==2);
}

TEST_CASE("Focused handles defer outside selection until a click or box drag completes", "[editor][ui][gizmo-focus]") {
    SelectionInputLogic input;
    const ui::Rect viewport{0,0,640,480};
    const std::array down{input::Event{.kind=input::EventKind::pointer_down,.position={100,100}}};
    const std::array up{input::Event{.kind=input::EventKind::pointer_up,.position={100,100}}};
    REQUIRE(input.route(down,down,viewport,true,true).empty());
    REQUIRE(input.route({}, {},viewport,true,true).empty());
    const auto clicked=input.route(up,up,viewport,false,true); // Tool focus has cleared on release.
    REQUIRE(clicked.size()==2);CHECK(clicked[0].kind==input::EventKind::pointer_down);
    CHECK(clicked[1].kind==input::EventKind::pointer_up);CHECK(input.available(clicked[0]));
    REQUIRE(input.route(down,down,viewport,true,true).empty());
    const auto ui_release=input.route(up,{},viewport,true,true);
    REQUIRE(ui_release.size()==1);CHECK(ui_release[0].kind==input::EventKind::pointer_up);
    REQUIRE(input.route(down,down,viewport,true,true).empty());
    const std::array move{input::Event{.kind=input::EventKind::pointer_move,.position={150,130}}};
    const auto dragging=input.route(move,move,viewport,true,true);
    REQUIRE(dragging.size()==2);CHECK(dragging[0].position==down[0].position);
    CHECK(dragging[1].kind==input::EventKind::pointer_move);
    // Without a focused handle, selection retains its immediate response.
    REQUIRE(input.route(down,down,viewport,false,true).size()==1);
    REQUIRE(input.route(down,down,viewport,true,true).empty());
    input.cancel();CHECK(input.route(up,up,viewport,true,true).size()==1);
}
TEST_CASE("Tool panel hosts custom blueprint actions at the viewport bottom left", "[editor][ui][tool-options]") {
    CustomTool tool;Fixture f;f.panel.show(tool);f.pump();
    const auto close=f.widget("Close tool options");
    CHECK(close.bounds.x>=100.F);CHECK(close.bounds.x<150.F);CHECK(close.bounds.y>300.F);
    f.click("Custom action");CHECK(tool.calls==1);
    f.click("Close tool options");CHECK_FALSE(f.panel.opened());
    f.panel.show(tool);CHECK_FALSE(f.panel.opened()); // Dismissal sticks for this operation.
    f.panel.show(tool,true);CHECK(f.panel.opened());
    tool.available=false;f.panel.validate();CHECK_FALSE(f.panel.opened());
}
TEST_CASE("Camera action panel follows projected anchors and stays within the viewport", "[editor][ui][camera-panel]") {
    Fixture f;
    auto host=f.screen.column().padding(8).gap(6);
    CameraPanel panel{host};
    SceneInstance instance{7,BlueprintId::camera,"Shot",CameraSettings{}, {}};
    panel.sync(&instance,{},true,false);
    auto view=gfx::Camera{};
    view.set_position({0,0,10}).look_at({});
    const ui::Rect viewport{100,100,800,600};
    const auto place=[&](const gfx::Camera& camera,ui::Rect bounds,std::optional<Vec3> anchor) {
        panel.layout(bounds,camera,{800,600},anchor);f.pump();return host.bounds();
    };
    const auto before=place(view,viewport,Vec3{});
    auto pan=view;pan.move_by({2,0,0});
    const auto after=place(pan,viewport,Vec3{});
    CHECK(after.x<before.x-30);CHECK(after.y==before.y);
    CHECK(panel.contains({after.x+10,after.y+10}));
    CHECK(f.widget("Enter").bounds.x>=after.x);
    f.click("Enter");CHECK(panel.enter_clicked()); // Hit testing follows the moved controls too.
    // Resizing/detachment changes only the viewport coordinate space.
    const auto detached=place(pan,{0,0,800,600},Vec3{});
    CHECK(detached.x==Catch::Approx(after.x-100));CHECK(detached.y==Catch::Approx(after.y-100));
    const auto fallback=place(view,viewport,std::nullopt);
    CHECK(fallback.x==viewport.x+viewport.width-fallback.width-12);
    CHECK(fallback.y==viewport.y+12);
    for(const auto point:{Vec3{0,0,11},Vec3{100,0,0}}) {
        const auto offscreen=place(view,viewport,point); // Behind camera or off-screen.
        CHECK(offscreen.x==fallback.x);CHECK(offscreen.y==fallback.y);
    }
    const auto tiny=place(view,{0,0,200,150},Vec3{});
    CHECK(tiny.x>=0);CHECK(tiny.y>=0);CHECK(tiny.x+tiny.width<=200);CHECK(tiny.y+tiny.height<=150);
}
TEST_CASE("Whole mesh camera control uses the displayed draft center and stays viewport-local", "[editor][ui][mesh-origin]") {
    content::vmesh::Document d;d.vertex_count=3;
    d.vertex_fields={{"position",{content::vmesh::ScalarType::Float32,3},std::vector<f32>{0,0,0,3,0,0,0,3,0}}};
    d.faces={{0,1,2}};
    auto mesh=editor::EditableMesh::create(d);REQUIRE(mesh);
    State state{.document={.mesh=std::move(*mesh)}};state.viewport.mode=ViewMode::mesh;
    Fixture f;MeshNavigationControls controls{f.screen.root()};
    controls.layout({100,100,800,600},true,true);f.pump();
    const auto w=f.widget("Mesh-centered camera");
    CHECK(w.bounds.x<130);CHECK(w.bounds.y<130);CHECK_FALSE(controls.origin(state));
    const auto camera_before=state.viewport.editor_camera;
    f.click("Mesh-centered camera");
    REQUIRE(controls.origin(state));CHECK(*controls.origin(state)==Vec3{1,1,0});
    auto placement=Mat4::identity();placement[3]={4,2,3,1};
    state.document.mesh_placements[BlueprintId::mesh].draft=placement;
    CHECK(*controls.origin(state)==Vec3{5,3,3});
    CHECK(state.viewport.editor_camera==camera_before);CHECK(state.document.revision==1);
    // Camera settings covers the embedded viewport controls, not their state.
    controls.layout({100,100,800,600},true,true,false,true,true);f.pump();
    REQUIRE(controls.origin(state));CHECK(*controls.origin(state)==Vec3{5,3,3});
    CHECK_FALSE(controls.contains({w.bounds.x+5,w.bounds.y+5}));
    const auto covered=f.screen.inspect();REQUIRE(covered);
    CHECK_FALSE(std::ranges::any_of(covered->widgets,[](const auto& item){
        return item.visible&&item.label=="Mesh-centered camera";
    }));
    // Closing settings (or showing a detached viewport) restores the controls.
    controls.layout({100,100,800,600},true,true);f.pump();
    CHECK(f.widget("Mesh-centered camera").visible);
    REQUIRE(controls.origin(state));CHECK(*controls.origin(state)==Vec3{5,3,3});
    controls.layout({100,100,800,600},true,true,true);f.pump();
    CHECK(f.widget("Mesh-centered camera").bounds.y>152); // Below FPS overlay.
    controls.layout({100,100,800,600},false,false);f.pump();
    CHECK_FALSE(controls.origin(state));CHECK_FALSE(controls.contains({110,110}));
    controls.layout({100,100,800,600},true,false);f.pump();
    CHECK(controls.origin(state)); // Preference survives leaving mode 5.
    const auto tree=f.screen.inspect();REQUIRE(tree);
    CHECK_FALSE(std::ranges::any_of(tree->widgets,[](const auto& w){return w.visible&&w.label=="Bake camera transforms...";}));
    controls.layout({100,100,800,600},true,true);f.pump();
    f.click("Bake camera transforms...");CHECK_FALSE(controls.poll());
    controls.layout({100,100,800,600},true,true);f.pump();
    f.click("Scale");CHECK_FALSE(controls.poll());
    f.click("Bake to mesh draft");const auto request=controls.poll();REQUIRE(request);
    CHECK(request->rotation);CHECK_FALSE(request->scale);
}
TEST_CASE("Gizmo input pauses for navigation and options without changing its accumulated pointer", "[editor][ui][gizmo-navigation]") {
    GizmoControls input;
    input.route(false,false,false,{100,100},{},{});
    const auto move=[&](Vec2 p,bool camera=false,bool menu=false) {
        const std::array events{vng::input::Event{.kind=input::EventKind::pointer_move,.position=p}};
        input.route(true,camera,menu,p,events,events);
    };
    move({120,100});REQUIRE(input.events().size()==1);
    CHECK(input.events()[0].position==Vec2{120,100});
    input.sensitivity(.5F);
    move({140,100});CHECK(input.events()[0].position==Vec2{130,100});
    move({500,300},true);CHECK(input.events().empty());
    move({520,300});CHECK(input.events()[0].position==Vec2{140,100});
    move({700,600},false,true);CHECK(input.events().empty());
    input.sensitivity(2);
    move({710,600});CHECK(input.events()[0].position==Vec2{140,100});
    move({720,600});CHECK(input.events()[0].position==Vec2{160,100});
    CHECK(input.unhandled()[0].position==input.events()[0].position);
    const std::array end{vng::input::Event{.kind=input::EventKind::key_down,.key=input::Key::escape}};
    input.route(true,true,true,{710,600},end,end);
    REQUIRE(input.events().size()==1);CHECK(input.events()[0].key==input::Key::escape);
    input.route(true,false,true,{710,600},end,{});
    CHECK(input.events().empty()); // Esc belongs to the focused number field.
    const std::array release{vng::input::Event{.kind=input::EventKind::pointer_up,.position={710,600}}};
    input.route(true,true,true,{710,600},release,{});
    REQUIRE(input.events().size()==1);CHECK(input.events()[0].kind==input::EventKind::pointer_up);
}
TEST_CASE("Scale option drafts require Apply and reject invalid limits atomically", "[editor][ui][scale-limits]") {
    GizmoControls input;input.show(true,nullptr,false,true);
    editor::Inspector inspector{{1,1,1}};input.describe_options(inspector);
    CHECK(input.scale_limits().instance==3.F);
    const auto apply=[&](float maximum) {
        return inspector.dispatch({{1,1,1},"scale_limits",editor::Phase::apply,
            {{"instance",maximum},{"axis",40.F},{"factor",2000.F}}});
    };
    CHECK_FALSE(apply(0.F));CHECK(input.scale_limits().instance==3.F);
    REQUIRE(apply(100.F));CHECK(input.scale_limits().instance==100.F);
    CHECK(input.scale_limits().axis==40.F);CHECK(input.scale_limits().factor==2000.F);
    CHECK_FALSE(apply(std::numeric_limits<float>::infinity()));
    CHECK(input.scale_limits().instance==100.F);
}
TEST_CASE("Scale limit text stays a draft until the options Apply button is clicked", "[editor][ui][scale-limits]") {
    GizmoControls input;Fixture f;input.show(true,nullptr,false,true);f.panel.show(input);f.pump();f.pump();
    const auto snapshot=f.screen.inspect();REQUIRE(snapshot);
    const auto field=std::ranges::find_if(snapshot->widgets,[](const auto& item) {
        return item.role==ui::WidgetRole::text_field&&item.label=="Maximum instance scale";
    });
    REQUIRE(field!=snapshot->widgets.end());
    const Vec2 p{field->bounds.x+10,field->bounds.y+10};
    f.pump({{.kind=input::EventKind::pointer_down,.position=p},{.kind=input::EventKind::pointer_up,.position=p}});
    f.pump({{.kind=input::EventKind::key_down,.key=input::Key::a,.modifiers={.control=true}},
            {.kind=input::EventKind::text,.text="100"}});
    CHECK(input.scale_limits().instance==3.F);
    f.click("Apply scale limits");
    CHECK(input.scale_limits().instance==100.F);
}
TEST_CASE("The active gizmo panel exposes sensitivity alongside custom options", "[editor][ui][gizmo-navigation]") {
    CustomTool tool;GizmoControls input;Fixture f;input.show(true,&tool);f.panel.show(input);f.pump();f.pump();
    const auto snapshot=f.screen.inspect();REQUIRE(snapshot);
    const auto found=std::ranges::find_if(snapshot->widgets,[](const auto& w){return w.visible&&w.role==ui::WidgetRole::slider;});
    REQUIRE(found!=snapshot->widgets.end());const auto r=found->bounds;
    f.pump({{.kind=input::EventKind::pointer_down,.position={r.x+r.width*.6F,r.y+r.height*.5F}}});
    f.pump({{.kind=input::EventKind::pointer_move,.position={r.x+r.width*.8F,r.y+r.height*.5F}}});
    f.pump({{.kind=input::EventKind::pointer_up,.position={r.x+r.width*.8F,r.y+r.height*.5F}}});
    CHECK(input.sensitivity()>1.F);
    f.click("Custom action");CHECK(tool.calls==1);
    input.show(false,nullptr);f.panel.validate();CHECK_FALSE(f.panel.opened());
}
TEST_CASE("Held gizmo arrows move every frame at a sensitivity scaled rate, ignoring OS repeat", "[editor][ui][arrows]") {
    for(const float sensitivity:{.5F,1.F,2.F})for(const int fps:{30,60,120}) {
        GizmoControls router;router.sensitivity(sensitivity);
        float total{};
        const auto tick=[&](std::span<const input::Event> events) {
            router.route(true,false,false,{400,300},events,events,1.F/fps);
            unsigned count{};
            for(const auto& event:router.events())if(auto arrow=transform_arrow(event,router.arrow_step())) {
                total+=rotation_arrow(*arrow);++count;
            }
            CHECK(count==1); // Includes the frames before any repeat event.
        };
        const std::array down{input::Event{.kind=input::EventKind::key_down,.key=input::Key::up}};
        tick(down);
        const std::array repeats{input::Event{.kind=input::EventKind::key_down,.key=input::Key::up,.repeat=true},
            input::Event{.kind=input::EventKind::key_down,.key=input::Key::up,.repeat=true}};
        for(int frame=1;frame<fps;++frame)tick(frame%3==0?std::span<const input::Event>{repeats}:std::span<const input::Event>{});
        CHECK(total==Catch::Approx(60.F*sensitivity));
        const std::array up{input::Event{.kind=input::EventKind::key_up,.key=input::Key::up}};
        router.route(true,false,false,{400,300},up,{},1.F/fps); // Release may be consumed by UI.
        router.route(true,false,false,{400,300},{},{},1.F/fps);
        CHECK(router.events().empty());
    }
}
TEST_CASE("Held gizmo arrows stop at input ownership and transaction boundaries", "[editor][ui][arrows]") {
    const std::array down{input::Event{.kind=input::EventKind::key_down,.key=input::Key::right}};
    for(int boundary=0;boundary<8;++boundary) {
        GizmoControls router;
        router.route(true,false,false,{400,300},down,down);
        std::vector<input::Event> stop;
        if(boundary==0)stop.push_back({.kind=input::EventKind::focus_lost});
        if(boundary==1)stop.push_back({.kind=input::EventKind::key_down,.key=input::Key::enter});
        if(boundary==2)stop.push_back({.kind=input::EventKind::key_down,.key=input::Key::escape});
        if(boundary==3)stop.push_back({.kind=input::EventKind::key_down,.key=input::Key::left,.modifiers={.control=true}});
        router.route(boundary!=4,boundary==5,boundary==6,{400,300},stop,stop,1.F/60.F,boundary!=7);
        router.route(true,false,false,{400,300},{},{});
        CHECK(router.events().empty());
        auto repeat=down;repeat[0].repeat=true;
        router.route(true,false,false,{400,300},repeat,repeat);
        CHECK(router.events().empty()); // Never restart from a stale OS repeat.
    }
    GizmoControls router;
    router.route(false,false,false,{400,300},down,{});
    CHECK(router.events().empty()); // A focused text field owns this press.
    router.route(false,false,false,{400,300},{},{});
    CHECK(router.events().empty());
    auto fine=down;fine[0].modifiers.shift=true;
    router.sensitivity(2.F);router.route(true,false,false,{400,300},fine,fine);
    REQUIRE(router.events().size()==1);
    CHECK(transform_arrow(router.events()[0],router.arrow_step())->x==Catch::Approx(.2F));
    router.route(true,false,false,{400,300},{},{},10.F);
    CHECK(router.arrow_step()==Catch::Approx(6.F)); // No leap after a stall.
}
TEST_CASE("A gizmo menu constrains and confirms through the owning gesture", "[editor][ui][tool-options]") {
    TransformGesture gesture;Fixture f;gfx::Camera camera;
    camera.set_position({0,0,10}).look_at({}).set_orthographic({.vertical_height=10});
    const auto snapshot=*camera.snapshot({800,600});
    const std::array start{input::Event{.kind=input::EventKind::key_down,.position={600,300},.key=input::Key::g}};
    REQUIRE(gesture.update({1,1,1},{},snapshot,{100,100,800,600},start,start,true).began);
    f.panel.show(gesture);f.pump();f.click("X axis");
    CHECK(gesture.active()); // UI did not end or independently mutate the gesture.
    REQUIRE(gesture.update({1,1,1},{},snapshot,{100,100,800,600},{},{},true).changed);
    CHECK(gesture.axis()==0);
    f.click("Confirm transform");
    const auto done=gesture.update({1,1,1},{},snapshot,{100,100,800,600},{},{},true);
    CHECK(done.finished);CHECK_FALSE(done.cancelled);CHECK_FALSE(gesture.active());
    f.panel.validate();CHECK_FALSE(f.panel.opened());
}
