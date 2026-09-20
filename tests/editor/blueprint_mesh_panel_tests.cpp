#include "../../examples/editor/blueprint_mesh_panel.hpp"
#include "../../examples/support/earth_assets.hpp"
#include "../../examples/editor/surface_move_tool.hpp"
#include "../../examples/editor/surface_part_tool.hpp"
#include <vng/ui/inspection.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <algorithm>
#include <thread>

namespace {
using namespace vng;
using namespace editor_example;
text::Font font(){auto f=text::Font::load(VNG_TEST_FONT_PATH);REQUIRE(f);return *f;}
State initial() {
    auto mesh=editor::EditableMesh::load(std::filesystem::path(VNG_TEST_FONT_PATH).parent_path().parent_path()/"earth.vmesh");
    REQUIRE(mesh);State state{.document={.mesh=std::move(*mesh)}};state.viewport.mode=ViewMode::mesh;return state;
}
struct Fixture {
    EditingSession editing{initial()};
    ui::Screen screen{ui::dark_theme(font())};
    BlueprintMeshPanel panel{screen.column().width(600),editing};
    input::Frame frame{.logical_size={800,1000},.framebuffer={800,1000}};
    void pump(std::initializer_list<input::Event> events={}) {
        frame.events=events;for(const auto& e:events)frame.pointer=e.position;
        panel.sync();panel.enabled(true);REQUIRE(screen.update(frame,.016F));
        (void)panel.poll(true);REQUIRE(screen.draw_list());
    }
    ui::WidgetSnapshot widget(std::string_view name) {
        const auto snapshot=screen.inspect();REQUIRE(snapshot);
        for(const auto& item:snapshot->widgets)if(item.visible&&(item.text==name||item.label==name))return item;
        FAIL("Missing blueprint control: "<<name);return {};
    }
    void click(std::string_view name) {
        auto r=widget(name).bounds;Vec2 p{r.x+r.width*.5F,r.y+r.height*.5F};
        pump({{.kind=input::EventKind::pointer_down,.position=p},{.kind=input::EventKind::pointer_up,.position=p}});
    }
    void finish() {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(panel.busy() && std::chrono::steady_clock::now()<deadline) {pump();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        REQUIRE_FALSE(panel.busy());
    }
};
}
TEST_CASE("Cloud surface tools expose a heading ring and keyboard gestures", "[editor][ui][surface-gizmo][arrows]") {
    gfx::Camera camera;camera.set_position({0,0,4}).look_at({});
    const auto view=camera.snapshot({800,600});REQUIRE(view);
    SurfacePartTool tool;const SurfaceMove surface{{},{0,0,1},1,"Cloud",Mat4::identity(),true};
    const auto pump=[&](std::initializer_list<input::Event> events={}) {
        const std::span<const input::Event> raw{events.begin(),events.size()};
        return tool.update(surface,*view,{0,0,800,600},raw,raw,true,!tool.dragging());
    };
    pump();REQUIRE(tool.handle());CHECK(tool.rotation().visible());
    CHECK_FALSE(tool.rotation().rings()[0].projected[0]);CHECK(tool.rotation().rings()[2].projected[0]);
    REQUIRE(pump({{.kind=input::EventKind::key_down,.position={450,300},.key=input::Key::r}}).began);
    auto turn=pump({{.kind=input::EventKind::key_down,.key=input::Key::up}});
    REQUIRE(turn.changed);REQUIRE(turn.rotation_degrees);CHECK(*turn.rotation_degrees==1.F);
    turn=pump({{.kind=input::EventKind::key_down,.key=input::Key::left,.repeat=true}});
    CHECK(*turn.rotation_degrees==2.F);
    CHECK(pump({{.kind=input::EventKind::key_down,.key=input::Key::escape}}).cancelled);
    REQUIRE(pump({{.kind=input::EventKind::key_down,.position={400,300},.key=input::Key::g}}).began);
    auto move=pump({{.kind=input::EventKind::key_down,.position={400,300},.key=input::Key::right}});
    REQUIRE(move.changed);CHECK_FALSE(move.rotation_degrees);CHECK(move.position.x>0);
    CHECK(pump({{.kind=input::EventKind::key_down,.key=input::Key::enter}}).finished);
    const auto start=tool.rotation().rings()[2].points[8];
    const auto end=tool.rotation().rings()[2].points[20];
    REQUIRE(pump({{.kind=input::EventKind::pointer_down,.position=start}}).began);
    turn=pump({{.kind=input::EventKind::pointer_move,.position=end}});
    REQUIRE(turn.changed);REQUIRE(turn.rotation_degrees);
    CHECK(std::abs(*turn.rotation_degrees-45.F)<.01F);
    turn=pump({{.kind=input::EventKind::key_down,.key=input::Key::down}});
    CHECK(std::abs(*turn.rotation_degrees-44.F)<.01F);
    turn=pump({{.kind=input::EventKind::pointer_up,.position=end}});
    CHECK(turn.finished);CHECK(std::abs(*turn.rotation_degrees-44.F)<.01F);
}
TEST_CASE("Cloud creation removal and rotation are blueprint-owned undoable actions", "[editor][ui][cloud-catalog]") {
    Fixture f;f.pump();f.click("Clouds visible");f.click("Rebuild clouds");f.finish();f.pump();
    f.click("Add spiral cloud");f.finish();f.pump();
    auto formations=example::earth::cloud_formations(editable_mesh(f.editing.state())->document());REQUIRE(formations);
    REQUIRE(formations->size()==18);const auto id=formations->back().id;
    CHECK(f.panel.selected_part()==id);f.pump();CHECK(f.widget("Rotate cloud").enabled);
    f.click("Remove cloud");f.finish();f.pump();
    CHECK(f.panel.selected_part()==0);
    CHECK(example::earth::cloud_formations(editable_mesh(f.editing.state())->document())->size()==17);
    REQUIRE(f.editing.undo());f.pump();CHECK(example::earth::cloud_formations(editable_mesh(f.editing.state())->document())->size()==18);
    REQUIRE(f.editing.undo());f.pump();CHECK(example::earth::cloud_formations(editable_mesh(f.editing.state())->document())->size()==17);
}
TEST_CASE("Blueprint sliders stage locally and rebuild asynchronously into an undoable draft", "[editor][ui][blueprint-mesh]") {
    Fixture f;f.pump();CHECK_FALSE(f.editing.can_edit_scene_pose());
    CHECK(f.widget("Rebuild clouds").enabled);
    for(auto name:{"Coverage","Puff size","Spiral size","Edge scatter","Altitude","Height variation"})CHECK(f.widget(name).visible);
    const auto count=f.editing.state().document.mesh.size();
    f.click("Clouds visible");CHECK_FALSE(f.editing.dirty());CHECK_FALSE(f.panel.busy());
    f.click("Rebuild clouds");REQUIRE(f.panel.busy());
    f.finish();CHECK(f.editing.dirty());CHECK(f.editing.state().document.mesh.size()==count);
    CHECK(editable_mesh(f.editing.state())->size()<count);
    REQUIRE(f.editing.undo());f.pump();CHECK(f.editing.state().document.mesh_drafts.empty());
    CHECK_FALSE(f.editing.dirty());
}
TEST_CASE("A background blueprint rebuild cannot modify a different inspection target", "[editor][ui][blueprint-mesh]") {
    Fixture f;f.pump();f.click("Clouds visible");f.click("Rebuild clouds");REQUIRE(f.panel.busy());
    f.editing.viewport().mode=ViewMode::scene;f.finish();
    CHECK_FALSE(f.editing.dirty());CHECK(f.editing.state().document.mesh_drafts.empty());
    CHECK(f.panel.status().find("discarded")!=std::string_view::npos);
}
TEST_CASE("Blueprint part selection declares surface controls without scene instances", "[editor][ui][blueprint-mesh]") {
    Fixture f;f.pump();
    // Fast migration with clouds hidden still retains their recipe/placements.
    f.click("Clouds visible");f.click("Rebuild clouds");f.finish();f.pump();
    const auto revision=f.editing.state().document.revision;
    const auto instances=f.editing.state().document.instances.size();
    f.click("Edit part");
    for(int i=0;i<15;++i)f.pump({{.kind=input::EventKind::key_down,.key=input::Key::down},
        {.kind=input::EventKind::key_up,.key=input::Key::down}});
    f.pump({{.kind=input::EventKind::key_down,.key=input::Key::enter},{.kind=input::EventKind::key_up,.key=input::Key::enter}});
    f.pump();
    CHECK(f.widget("Move cloud").enabled);
    CHECK(f.widget("Longitude (degrees)").visible);CHECK(f.widget("Latitude (degrees)").visible);
    CHECK(f.editing.state().document.revision==revision);
    auto snapshot=f.screen.inspect();REQUIRE(snapshot);
    const auto slider=std::ranges::find_if(snapshot->widgets,[](const auto& w) {
        return w.role==ui::WidgetRole::slider && w.label=="Longitude (degrees)";
    });
    REQUIRE(slider!=snapshot->widgets.end());
    const Vec2 point{slider->bounds.x+slider->bounds.width*.6F,slider->bounds.y+slider->bounds.height*.5F};
    f.pump({{.kind=input::EventKind::pointer_down,.position=point},{.kind=input::EventKind::pointer_up,.position=point}});
    CHECK(f.editing.state().document.revision==revision);
    f.click("Move cloud");f.finish();f.pump();
    const auto moved=example::earth::cloud_formations(editable_mesh(f.editing.state())->document());REQUIRE(moved);
    CHECK(moved->at(14).location.x>0.F);
    CHECK(f.editing.state().document.instances.size()==instances);
    CHECK(f.widget("Edit part").text=="Atlantic spiral");
    REQUIRE(f.editing.undo());f.pump();
    CHECK(example::earth::cloud_formations(editable_mesh(f.editing.state())->document())->at(14).location.x<0.F);
    // Removing the ownership via Undo must not leave a stale part selected.
    REQUIRE(f.editing.undo());f.pump();CHECK(f.widget("Rebuild clouds").enabled);
}

TEST_CASE("Surface handle captures immediately, constrains movement and preserves its pending ghost",
          "[editor][ui][surface-gizmo]") {
    gfx::Camera camera;camera.set_position({0,0,4}).look_at({0,0,0});
    auto snapshot=camera.snapshot({800,600});REQUIRE(snapshot);
    const ui::Rect viewport{0,0,800,600};
    const std::optional<SurfaceMove> target{SurfaceMove{{},{0,0,1},1,"Formation"}};
    SurfaceMoveTool tool;
    (void)tool.update(target,*snapshot,viewport,{},{},true,true);
    REQUIRE(tool.handle());
    const auto start=*tool.handle();
    const std::array press{input::Event{.kind=input::EventKind::pointer_down,.position=start}};
    CHECK(tool.update(target,*snapshot,viewport,press,press,true,true).began);
    CHECK(tool.dragging());CHECK(tool.handled());
    const std::array move{input::Event{.kind=input::EventKind::pointer_move,.position={start.x+50,start.y-20}}};
    auto changed=tool.update(target,*snapshot,viewport,{},move,true,false);
    CHECK(changed.changed);CHECK(changed.position.x>0);CHECK(changed.position.y>0);
    CHECK(std::abs(std::hypot(changed.position.x,changed.position.y,changed.position.z)-1)<1e-5F);
    const std::array release{input::Event{.kind=input::EventKind::pointer_up,.position=move[0].position}};
    CHECK(tool.update(target,*snapshot,viewport,{},release,true,false).finished);
    const auto ghost=tool.handle();
    (void)tool.update(target,*snapshot,viewport,{},{},true,false);
    CHECK(tool.handle()==ghost); // CPU job hasn't caught up yet.
    ui::DrawList first,second;
    tool.append(first,font(),true,0);tool.append(second,font(),true,.1);
    REQUIRE_FALSE(first.commands.empty());
    CHECK(std::ranges::any_of(first.commands,[](const auto& draw) {
        const auto* text=std::get_if<ui::TextDraw>(&draw);
        return text&&text->text=="Updating blueprint...";
    }));
    // Far-side selection is not a handle through the planet.
    auto back=target;back->position={0,0,-1};
    (void)tool.update(back,*snapshot,viewport,{},{},true,true);
    CHECK_FALSE(tool.visible());
    (void)tool.update(target,*snapshot,viewport,{},{},true,true);
    CHECK(tool.update(target,*snapshot,viewport,press,press,true,true).began);
    const std::array cancel{input::Event{.kind=input::EventKind::pointer_down,.position=start,.button=1}};
    CHECK(tool.update(target,*snapshot,viewport,{},cancel,true,false).cancelled);
    CHECK_FALSE(tool.dragging());CHECK(tool.handled());
}

TEST_CASE("Cloud handle jobs coalesce and cancel without stale completion or extra undo entries",
          "[editor][ui][surface-gizmo]") {
    Fixture f;f.pump();f.click("Rebuild clouds");f.finish();f.pump();
    f.click("Edit part");
    for(int i=0;i<15;++i)f.pump({{.kind=input::EventKind::key_down,.key=input::Key::down},
        {.kind=input::EventKind::key_up,.key=input::Key::down}});
    f.pump({{.kind=input::EventKind::key_down,.key=input::Key::enter}});f.pump();
    REQUIRE(f.panel.gizmo());
    const auto initial=editable_mesh(f.editing.state())->document();
    const auto revision=f.editing.state().document.revision;
    const auto radius=f.panel.gizmo()->radius;
    REQUIRE(f.panel.edit_part({.began=true}));
    REQUIRE(f.panel.edit_part({.changed=true,.position={radius,0,0}}));
    CHECK(f.panel.pending(revision));
    REQUIRE(f.panel.edit_part({.changed=true,.position={0,radius,0}}));
    REQUIRE(f.panel.edit_part({.changed=true,.finished=true,.position={0,0,radius}}));
    f.finish();
    auto clouds=example::earth::cloud_formations(editable_mesh(f.editing.state())->document());REQUIRE(clouds);
    CHECK(std::abs(clouds->at(14).location.x)<1e-4F);
    CHECK(std::abs(clouds->at(14).location.y)<1e-4F);
    CHECK(f.panel.pending(revision)); // CPU completion is not presentation.
    CHECK_FALSE(f.panel.pending(f.editing.state().document.revision));
    REQUIRE(f.editing.undo());f.pump();
    CHECK(editable_mesh(f.editing.state())->document()==initial);
    REQUIRE(f.editing.redo());f.pump();
    const auto before_cancel=editable_mesh(f.editing.state())->document();
    REQUIRE(f.panel.edit_part({.began=true,.changed=true,.position={radius,0,0}}));
    REQUIRE(f.panel.edit_part({.cancelled=true}));
    f.finish();
    CHECK(editable_mesh(f.editing.state())->document()==before_cancel);
    CHECK_FALSE(f.editing.busy());
}

TEST_CASE("Blueprint part picking follows visible triangles and synchronizes the list without edits", "[editor][ui][part-picking]") {
    // One front formation, terrain in the middle, another formation behind it.
    content::vmesh::Document d;d.metadata["editor/blueprint"]="earth";d.vertex_count=9;
    d.vertex_fields={{"position",{content::vmesh::ScalarType::Float32,3},
        std::vector<f32>{-.4F,-.4F,1,.4F,-.4F,1,0,.4F,1, -2,-2,0,2,-2,0,0,2,0, -1,-1,-1,1,-1,-1,0,1,-1}},
        {"earth/cloud",{content::vmesh::ScalarType::UInt32,1},std::vector<u32>{15,15,15,0,0,0,16,16,16}}};
    d.faces={{0,1,2},{3,4,5},{6,7,8}};
    auto mesh=editor::EditableMesh::create(d);REQUIRE(mesh);
    State state{.document={.mesh=std::move(*mesh)}};state.viewport.mode=ViewMode::mesh;
    EditingSession editing{std::move(state)};ui::Screen screen{ui::dark_theme(font())};
    BlueprintMeshPanel panel{screen.column(),editing};panel.sync();
    gfx::Camera camera;camera.set_position({0,0,4}).look_at({});
    const auto revision=editing.state().document.revision;
    for(bool ortho:{false,true}) {
        if(ortho)camera.set_orthographic({.vertical_height=5});
        auto snapshot=camera.snapshot({800,600});REQUIRE(snapshot);
        CHECK(panel.pick_part({.5F,.5F},*snapshot)==15);
        CHECK(panel.pick_part({.5F,.8F},*snapshot)==0); // not the farther cloud
        CHECK(panel.pick_part({0,0},*snapshot)==0);
        CHECK(panel.pick_part({-1,0},*snapshot)==0);
    }
    REQUIRE(panel.select_part(15));CHECK(panel.selected_part()==15);REQUIRE(panel.gizmo());
    CHECK(panel.gizmo()->label=="Atlantic spiral");
    CHECK_FALSE(panel.select_part(999));CHECK(panel.selected_part()==15);
    REQUIRE(panel.select_part(0));CHECK_FALSE(panel.gizmo());
    CHECK(editing.state().document.revision==revision);CHECK_FALSE(editing.dirty());
    CHECK_FALSE(editing.can_undo());
    REQUIRE(panel.select_part(15));
    auto placement=Mat4::identity();placement[0][0]=2;placement[1][1]=.7F;placement[3][0]=1;
    REQUIRE(editing.begin_mesh_transform(BlueprintId::mesh));REQUIRE(editing.mesh_transform(placement));REQUIRE(editing.commit());
    panel.sync();REQUIRE(panel.gizmo());CHECK(panel.gizmo()->frame==placement);
    const auto snapshot=camera.snapshot({800,600});REQUIRE(snapshot);
    // Orthographic view: translated cloud is now right of the centre; original
    // screen centre hits terrain. Test through the real shared geometry BVH.
    CHECK(panel.pick_part({.65F,.5F},*snapshot)==15);
    CHECK(panel.pick_part({.5F,.5F},*snapshot)==0);
    CHECK(editable_mesh(editing.state())->document()==d);
}

TEST_CASE("Surface handles follow an affine blueprint frame", "[editor][ui][surface-gizmo][whole-mesh]") {
    gfx::Camera camera;camera.set_position({0,0,8}).look_at({});
    auto snapshot=camera.snapshot({800,600});REQUIRE(snapshot);
    SurfaceMove target{{},{0,0,1},1,"Cloud"};
    target.frame[0][0]=2;target.frame[1][1]=.8F;target.frame[2][2]=1.5F;target.frame[3][0]=1;
    SurfaceMoveTool tool;(void)tool.update(target,*snapshot,{0,0,800,600},{},{},true,true);
    REQUIRE(tool.handle());CHECK(tool.handle()->x>400);
    const auto start=*tool.handle();
    const std::array press{input::Event{.kind=input::EventKind::pointer_down,.position=start}};
    REQUIRE(tool.update(target,*snapshot,{0,0,800,600},press,press,true,true).began);
    const std::array move{input::Event{.kind=input::EventKind::pointer_move,.position={start.x+30,start.y-15}}};
    const auto action=tool.update(target,*snapshot,{0,0,800,600},{},move,true,false);
    CHECK(action.changed);CHECK(action.position.x>0);CHECK(action.position.y>0);
    CHECK(std::abs(std::hypot(action.position.x,action.position.y,action.position.z)-1)<1e-5F);
    REQUIRE(tool.handle());CHECK(std::abs(tool.handle()->x-start.x-30)<.01F);
}
