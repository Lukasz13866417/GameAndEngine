#include "../../examples/editor/blueprint_mesh_panel.hpp"
#include "../../examples/support/earth_assets.hpp"
#include "../../examples/support/earth_infrastructure_placement.hpp"
#include "../../examples/editor/surface_move_tool.hpp"
#include "../../examples/editor/surface_part_tool.hpp"
#include "../../examples/editor/socket_pick_tool.hpp"
#include <catch2/catch_approx.hpp>
#include <vng/ui/inspection.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <algorithm>
#include <thread>

namespace {
using namespace vng;
using namespace editor_example;
MeshPartId cloud_part(u32 id){return {"earth/cloud",id};}
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
    void option(std::string key,editor::Phase phase=editor::Phase::activate,std::vector<editor::NamedValue> values={}) {
        editor::Inspector options{1,1,1};panel.describe_options(options);
        const auto result=options.dispatch({options.schema().stamp,std::move(key),phase,std::move(values)});
        if(!result)FAIL(result.error().message);
        finish();pump();
    }
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
    void choose(std::string_view name) {
        const auto snapshot=screen.inspect();REQUIRE(snapshot);
        for(const auto& item:snapshot->widgets)if(item.visible&&item.role==ui::WidgetRole::option&&item.text==name) {
            const auto r=item.bounds;const Vec2 p{r.x+r.width*.5F,r.y+r.height*.5F};
            pump({{.kind=input::EventKind::pointer_down,.position=p},{.kind=input::EventKind::pointer_up,.position=p}});return;
        }
        FAIL("Missing dropdown option: "<<name);
    }
    void finish() {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(panel.busy() && std::chrono::steady_clock::now()<deadline) {pump();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        REQUIRE_FALSE(panel.busy());
    }
    void fill(std::string_view name,std::string value) {
        const auto tree=screen.inspect();REQUIRE(tree);
        for(const auto& w:tree->widgets)if(w.visible&&w.role==ui::WidgetRole::text_field&&w.label==name) {
            const Vec2 p{w.bounds.x+w.bounds.width*.5F,w.bounds.y+w.bounds.height*.5F};
            pump({{.kind=input::EventKind::pointer_down,.position=p},{.kind=input::EventKind::pointer_up,.position=p},
                {.kind=input::EventKind::key_down,.key=input::Key::a,.modifiers={.control=true}},
                {.kind=input::EventKind::text,.text=std::move(value)},
                {.kind=input::EventKind::key_down,.key=input::Key::enter}});return;
        }
        FAIL("Missing part text field: "<<name);
    }
};
}
TEST_CASE("Earth infrastructure controls stage locally and rebuild only the undoable mesh draft", "[editor][ui][infrastructure]") {
    Fixture f;f.frame.logical_size={800,2400};f.frame.framebuffer={800,2400};f.pump();
    const auto original=f.editing.state().document.mesh.document();
    CHECK(f.widget("Raised skyway tunnels").enabled);
    CHECK(f.widget("Launch hubs").enabled);
    CHECK(f.widget("Structure size").enabled);
    CHECK(f.widget("Building height").enabled);
    f.click("Night city lights");
    CHECK_FALSE(f.editing.dirty());
    f.click("Apply infrastructure");f.finish();f.pump();
    const auto& edited=editable_mesh(f.editing.state())->document();
    auto settings=example::earth::infrastructure_settings(edited);REQUIRE(settings);
    CHECK(settings->night_lights);CHECK_FALSE(settings->skyways);CHECK_FALSE(settings->launch_hubs);
    CHECK(edited.vertex_count>original.vertex_count);
    CHECK(f.editing.state().document.mesh.document()==original);
    REQUIRE(f.editing.undo());f.pump();
    CHECK(editable_mesh(f.editing.state())->document()==original);
    REQUIRE(f.editing.redo());f.pump();
    CHECK(example::earth::infrastructure_settings(editable_mesh(f.editing.state())->document())->night_lights);
}
TEST_CASE("Earth addon scale menu stages values and processors use normal part tools", "[editor][ui][addon-scales]") {
    namespace earth=example::earth;
    Fixture f;f.frame.logical_size={800,4000};f.frame.framebuffer={800,4000};f.pump();
    f.click("Addon baseline scales...");f.pump();
    const auto before=editable_mesh(f.editing.state())->document();
    f.fill("All addons / baseline scale","1.5");f.fill("Tunnels coefficient","0.8");
    f.fill("Launch pads coefficient","1.2");f.fill("Atmospheric processors coefficient","1.4");
    CHECK(editable_mesh(f.editing.state())->document()==before);
    f.click("Apply addon scales");f.finish();f.pump();
    const auto settings=earth::infrastructure_settings(editable_mesh(f.editing.state())->document());REQUIRE(settings);
    CHECK(settings->addon_scale==1.5F);CHECK(settings->tunnel_scale==.8F);
    CHECK(settings->launch_pad_scale==1.2F);CHECK(settings->processor_scale==1.4F);
    CHECK(f.widget("All addons / baseline scale").enabled); // Menu remains open after Apply.
    REQUIRE(f.editing.undo());f.pump();CHECK(editable_mesh(f.editing.state())->document()==before);
    f.click("Addon baseline scales...");
    gfx::Camera camera;camera.set_position({0,0,4}).look_at({});const auto view=camera.snapshot({800,600});REQUIRE(view);
    f.click("Place atmospheric processor");REQUIRE(f.panel.place_part({.5F,.5F},*view));f.finish();f.pump();
    const auto parts=earth::infrastructure_parts(editable_mesh(f.editing.state())->document());REQUIRE(parts);REQUIRE(parts->size()==1);
    CHECK(parts->front().kind==earth::InfrastructureKind::processor);CHECK(f.panel.selected_part().value==parts->front().id);
    REQUIRE(f.panel.gizmo());CHECK(f.panel.gizmo()->can_rotate);CHECK(f.panel.gizmo()->radial_range);
    const auto processor=editable_mesh(f.editing.state())->document();
    REQUIRE(f.panel.cycle_gizmo(1));f.pump();CHECK(f.panel.title()=="Scale part (S)");
    REQUIRE(f.panel.gizmo()->scale);CHECK(f.panel.gizmo()->scale->value==1);
    REQUIRE(f.panel.edit_part({.began=true}));
    REQUIRE(f.panel.edit_part({.changed=true,.finished=true,.scale_value=1.8F}));f.finish();f.pump();
    CHECK(earth::infrastructure_parts(editable_mesh(f.editing.state())->document())->front().scale==1.8F);
    const auto scaled_settings=earth::infrastructure_settings(editable_mesh(f.editing.state())->document());REQUIRE(scaled_settings);
    CHECK(*scaled_settings==*earth::infrastructure_settings(processor));
    REQUIRE(f.editing.undo());f.pump();CHECK(editable_mesh(f.editing.state())->document()==processor);
    REQUIRE(f.panel.edit_part({.began=true}));
    REQUIRE(f.panel.edit_part({.changed=true,.scale_value=2.F}));
    REQUIRE(f.panel.edit_part({.cancelled=true}));f.finish();f.pump();
    CHECK(editable_mesh(f.editing.state())->document()==processor);
    f.option("addon_part_scale",editor::Phase::apply,{{"scale",1.4F}});
    CHECK(earth::infrastructure_parts(editable_mesh(f.editing.state())->document())->front().scale==1.4F);
    REQUIRE(f.editing.undo());f.pump();
    f.click("Addon baseline scales...");f.pump();CHECK(f.widget("Atmospheric processors coefficient").enabled);
    f.click("Addon baseline scales...");f.click("Remove part");f.finish();f.pump();
    CHECK(earth::infrastructure_parts(editable_mesh(f.editing.state())->document())->empty());
}
TEST_CASE("Addon scaling uses the shared scale handle and S without moving the surface anchor", "[editor][ui][addon-scales]") {
    SurfacePartTool tool;
    SurfaceMove target{{},{0,0,1},1,"Part scale (S)",Mat4::identity(),false,{}, {},PartScale{1,20}};
    gfx::Camera camera;camera.set_position({0,0,4}).look_at({});const auto view=*camera.snapshot({800,600});
    const auto pump=[&](std::initializer_list<input::Event> events,bool available=true,float speed=1) {
        const std::span<const input::Event> all{events.begin(),events.size()};
        return tool.update(target,view,{0,0,800,600},available?all:std::span<const input::Event>{},all,true,true,speed);
    };
    (void)pump({});REQUIRE(tool.visible());REQUIRE(tool.handle());CHECK_FALSE(tool.rotation().visible());
    const auto handle=*tool.handle();
    CHECK_FALSE(pump({{.kind=input::EventKind::key_down,.position=handle,.key=input::Key::s}},false).began);
    REQUIRE(pump({{.kind=input::EventKind::key_down,.position=handle,.key=input::Key::s}}).began);
    auto moved=pump({{.kind=input::EventKind::pointer_move,.position={handle.x+30,handle.y}}});
    REQUIRE(moved.changed);REQUIRE(moved.scale_value);CHECK(*moved.scale_value>1);CHECK(tool.position()==target.position);
    const auto arrow=pump({{.kind=input::EventKind::key_down,.key=input::Key::right}},true,2);
    REQUIRE(arrow.scale_value);CHECK(*arrow.scale_value>*moved.scale_value);
    const auto done=pump({{.kind=input::EventKind::key_down,.key=input::Key::enter}});
    CHECK(done.finished);CHECK_FALSE(done.cancelled);CHECK_FALSE(tool.dragging());
    (void)pump({});
    REQUIRE(pump({{.kind=input::EventKind::pointer_down,.position=*tool.handle()}}).began);
    CHECK(pump({{.kind=input::EventKind::key_down,.key=input::Key::escape}}).cancelled);
    CHECK_FALSE(tool.dragging());
}
TEST_CASE("Infrastructure placement and endpoint tools are blueprint-local undoable edits", "[editor][ui][infrastructure][parts]") {
    namespace earth=example::earth;
    Fixture f;f.frame.logical_size={800,2400};f.frame.framebuffer={800,2400};f.pump();
    gfx::Camera camera;camera.set_position({0,0,4}).look_at({});
    const auto snapshot=camera.snapshot({800,600});REQUIRE(snapshot);
    const auto published=f.editing.state().document.mesh.document();
    const auto instance_count=f.editing.state().document.instances.size();
    const auto revision=f.editing.state().document.revision;
    f.click("Place launch hub");CHECK(f.panel.placing());CHECK_FALSE(f.panel.gizmo());
    CHECK_FALSE(f.panel.place_part({0,0},*snapshot));CHECK(f.panel.placing());
    CHECK(f.editing.state().document.revision==revision);
    f.panel.cancel_placement();CHECK_FALSE(f.panel.placing());
    f.click("Place launch hub");REQUIRE(f.panel.place_part({.5F,.5F},*snapshot));
    f.finish();f.pump();
    auto parts=earth::infrastructure_parts(editable_mesh(f.editing.state())->document());REQUIRE(parts);REQUIRE(parts->size()==1);
    const MeshPartId hub{std::string(earth::infrastructure_part_field),parts->front().id};
    CHECK(f.panel.selected_part()==hub);CHECK(f.panel.gizmo());CHECK(f.widget("Part name").enabled);
    CHECK(f.panel.pick_part({.5F,.5F},*snapshot)==hub);
    CHECK(f.editing.state().document.instances.size()==instance_count);
    CHECK(f.editing.state().document.mesh.document()==published);
    const auto before_move=editable_mesh(f.editing.state())->document();
    REQUIRE(f.panel.edit_part({.began=true}));
    const auto radius=f.panel.gizmo()->radius;const auto factor=radius/std::sqrt(1.13F);
    REQUIRE(f.panel.edit_part({.changed=true,.finished=true,.position={.3F*factor,.2F*factor,factor}}));f.finish();f.pump();
    CHECK(editable_mesh(f.editing.state())->document()!=before_move);
    REQUIRE(f.editing.undo());f.pump();CHECK(editable_mesh(f.editing.state())->document()==before_move);
    f.click("Place skyway");REQUIRE(f.panel.place_part({.52F,.45F},*snapshot));f.finish();f.pump();
    parts=earth::infrastructure_parts(editable_mesh(f.editing.state())->document());REQUIRE(parts);REQUIRE(parts->size()==2);
    const auto tunnel=parts->back();CHECK(tunnel.kind==earth::InfrastructureKind::skyway);
    const auto before_endpoint=editable_mesh(f.editing.state())->document();
    CHECK(f.widget("Gizmo").text=="Whole part");CHECK(f.panel.gizmo_handles().size()==1);
    const auto before_cycle=f.editing.state().document.revision;
    CHECK_FALSE(f.panel.cycle_gizmo(0));
    REQUIRE(f.panel.cycle_gizmo(1));f.pump();
    CHECK(f.widget("Gizmo").text=="Tunnel endpoints");CHECK(f.panel.gizmo_handles().size()==2);
    REQUIRE(f.panel.select_handle(1));
    REQUIRE(f.panel.cycle_gizmo(-1));f.pump();
    CHECK(f.widget("Gizmo").text=="Whole part");CHECK(f.panel.gizmo_handles().size()==1);
    REQUIRE(f.panel.cycle_gizmo(-1));f.pump(); // wraps backwards
    CHECK(f.widget("Gizmo").text=="Scale part (S)");CHECK(f.panel.selected_handle()==0);
    REQUIRE(f.panel.cycle_gizmo(1));f.pump(); // wraps forwards
    CHECK(f.widget("Gizmo").text=="Whole part");
    CHECK(f.editing.state().document.revision==before_cycle);
    CHECK(editable_mesh(f.editing.state())->document()==before_endpoint);
    REQUIRE(f.panel.select_gizmo(1));f.pump();
    CHECK(f.widget("Gizmo").text=="Tunnel endpoints");CHECK(f.panel.gizmo_handles().size()==2);
    REQUIRE(f.panel.select_handle(1));REQUIRE(f.panel.gizmo());CHECK(f.panel.gizmo()->label=="Endpoint B");
    CHECK_FALSE(f.panel.gizmo()->can_rotate);
    REQUIRE(f.panel.edit_part({.began=true}));
    CHECK_FALSE(f.panel.cycle_gizmo(1)); // cannot switch the owner of an active gesture
    CHECK(f.panel.gizmo()->label=="Endpoint B");
    REQUIRE(f.panel.edit_part({.changed=true,.finished=true,.position={.35F,.1F,1}}));f.finish();f.pump();
    parts=earth::infrastructure_parts(editable_mesh(f.editing.state())->document());REQUIRE(parts);
    CHECK(parts->back().location==tunnel.location);CHECK(parts->back().end!=tunnel.end);
    REQUIRE(f.editing.undo());f.pump();CHECK(editable_mesh(f.editing.state())->document()==before_endpoint);
    CHECK(f.panel.gizmo()->label=="Endpoint B");
    f.click("Remove part");f.finish();f.pump();CHECK_FALSE(f.panel.selected_part());
    REQUIRE(f.editing.undo());f.pump();CHECK(editable_mesh(f.editing.state())->document()==before_endpoint);
    CHECK(f.editing.state().document.mesh.document()==published);
    REQUIRE(f.panel.select_part(hub));f.pump();
    REQUIRE(f.panel.cycle_gizmo(1));CHECK(f.panel.title()=="Scale part (S)");
    REQUIRE(f.panel.cycle_gizmo(1));CHECK(f.panel.title()=="Whole part");
    REQUIRE(f.panel.select_part({std::string(earth::infrastructure_part_field),tunnel.id}));f.pump();
    CHECK(f.widget("Gizmo").text=="Whole part");CHECK(f.panel.gizmo_handles().size()==1);
    REQUIRE(f.panel.select_part({}));f.pump();CHECK_FALSE(f.panel.cycle_gizmo(-1));
}
TEST_CASE("Scaffold gizmos move delete add and set up uniform supports without sidebar actions", "[editor][ui][manual-scaffolds]") {
    namespace earth=example::earth;
    Fixture f;f.frame.logical_size={800,3400};f.frame.framebuffer={800,3400};
    auto doc=earth::add_infrastructure(earth::make_mesh({.visible=false}),earth::InfrastructureKind::skyway,{0,0});REQUIRE(doc);
    auto mesh=editor::EditableMesh::create(*doc);REQUIRE(mesh);
    REQUIRE(f.editing.replace_mesh_draft(BlueprintId::mesh,f.editing.state().document.revision,std::move(*mesh)));
    f.pump();REQUIRE(f.panel.select_part({std::string(earth::infrastructure_part_field),1}));f.pump();
    REQUIRE(f.panel.select_gizmo(2));f.pump();CHECK(f.widget("Gizmo").text=="Scaffold positions");
    REQUIRE(f.panel.gizmo_handles().size()==2);REQUIRE(f.panel.select_handle(1));REQUIRE(f.panel.gizmo()->path);
    CHECK_FALSE(f.panel.gizmo()->can_rotate);CHECK_FALSE(f.panel.gizmo()->radial_range);
    const auto path=f.panel.gizmo()->path;
    const auto before=editable_mesh(f.editing.state())->document();
    REQUIRE(f.panel.edit_part({.began=true}));
    REQUIRE(f.panel.edit_part({.changed=true,.finished=true,.position=path->sample(.63F)}));f.finish();f.pump();
    const auto manual=editable_mesh(f.editing.state())->document();
    const auto positions=earth::infrastructure_parts(manual)->front().scaffold_positions;REQUIRE(positions);REQUIRE(positions->size()==2);
    CHECK((*positions)[0]==0);CHECK((*positions)[1]==Catch::Approx(.63F));
    CHECK(f.editing.state().document.mesh.document()!=manual);CHECK(f.panel.selected_handle()==1);
    REQUIRE(f.editing.undo());f.pump();CHECK(editable_mesh(f.editing.state())->document()==before);
    REQUIRE(f.editing.redo());f.pump();CHECK(editable_mesh(f.editing.state())->document()==manual);
    REQUIRE(f.panel.edit_part({.began=true}));
    REQUIRE(f.panel.edit_part({.changed=true,.position=f.panel.gizmo()->path->sample(.4F)}));
    REQUIRE(f.panel.edit_part({.cancelled=true}));f.finish();f.pump();CHECK(editable_mesh(f.editing.state())->document()==manual);
    REQUIRE(f.panel.select_gizmo(4));CHECK(f.panel.title()=="Scale part (S)"); // No uniform setup with interiors.
    REQUIRE(f.panel.select_gizmo(2));REQUIRE(f.panel.select_handle(1));
    REQUIRE(f.panel.erase_handle());f.finish();f.pump();CHECK(f.panel.gizmo_handles().size()==1);
    REQUIRE(f.editing.undo());f.pump();CHECK(editable_mesh(f.editing.state())->document()==manual);
    REQUIRE(f.panel.select_handle(1));f.option("erase_gizmo_handle");
    REQUIRE(f.panel.select_gizmo(3));f.pump();CHECK(f.panel.title()=="Add scaffold");
    const auto add_before=editable_mesh(f.editing.state())->document();
    REQUIRE(f.panel.edit_part({.began=true}));CHECK(f.panel.busy());
    REQUIRE(f.panel.edit_part({.changed=true,.finished=true,.position=path->sample(.45F)}));f.pump();
    CHECK(editable_mesh(f.editing.state())->document()==add_before);
    CHECK(path->parameter(f.panel.gizmo()->position)==Catch::Approx(.45F));
    const auto cursor=f.panel.gizmo()->position;
    REQUIRE(f.panel.edit_part({.began=true}));
    REQUIRE(f.panel.edit_part({.changed=true,.position=path->sample(.7F)}));
    REQUIRE(f.panel.edit_part({.cancelled=true}));f.pump();
    CHECK(f.panel.gizmo()->position==cursor);
    f.option("apply_scaffold_setup");
    auto added=earth::infrastructure_parts(editable_mesh(f.editing.state())->document())->front();
    REQUIRE(added.scaffold_positions);REQUIRE(added.scaffold_positions->size()==2);
    CHECK(added.scaffold_positions->back()==Catch::Approx(.45F));
    REQUIRE(f.panel.select_gizmo(2));REQUIRE(f.panel.select_handle(1));f.option("erase_gizmo_handle");
    REQUIRE(f.panel.select_gizmo(4));f.pump();CHECK(f.panel.gizmo_handles().size()==3);
    const auto uniform_before=editable_mesh(f.editing.state())->document();
    for(const auto& [index,t]:std::array{std::pair{0U,.18F},std::pair{1U,.88F},std::pair{2U,.4F}}) {
        if(index!=f.panel.selected_handle())REQUIRE(f.panel.select_handle(index));
        REQUIRE(f.panel.edit_part({.began=true}));
        REQUIRE(f.panel.edit_part({.changed=true,.finished=true,.position=path->sample(t)}));f.pump();
        CHECK(path->parameter(f.panel.gizmo()->position)==Catch::Approx(t));
        CHECK(editable_mesh(f.editing.state())->document()==uniform_before);
    }
    f.option("scaffold_setup",editor::Phase::commit,{{"first",.2F},{"last",.8F},{"spacing",.1F}});
    CHECK(editable_mesh(f.editing.state())->document()==uniform_before);
    f.option("apply_scaffold_setup");
    auto uniform=earth::infrastructure_parts(editable_mesh(f.editing.state())->document())->front();
    REQUIRE(uniform.scaffold_positions);CHECK(uniform.scaffold_positions->size()>3);
    CHECK((*uniform.scaffold_positions)[1]==.2F);CHECK(uniform.scaffold_positions->back()==.8F);
    REQUIRE(f.panel.select_gizmo(4));CHECK(f.panel.title()=="Scale part (S)");
    REQUIRE(f.editing.undo());f.pump();CHECK(editable_mesh(f.editing.state())->document()==uniform_before);
    REQUIRE(f.panel.select_gizmo(2));REQUIRE(f.panel.erase_handle());f.finish();f.pump();
    CHECK(f.panel.gizmo_handles().empty());CHECK(f.panel.has_gizmo());CHECK(f.panel.options_available());
    REQUIRE(f.panel.cycle_gizmo(1));CHECK(f.panel.title()=="Add scaffold");
    f.option("reset_scaffold_endpoints");
    auto reset=earth::infrastructure_parts(editable_mesh(f.editing.state())->document())->front();
    REQUIRE(reset.scaffold_positions);CHECK(*reset.scaffold_positions==std::vector<f32>{0,1});
    f.option("gizmo_scaffolding",editor::Phase::commit,{{"visible",false}});
    reset=earth::infrastructure_parts(editable_mesh(f.editing.state())->document())->front();
    CHECK_FALSE(reset.scaffold);CHECK(*reset.scaffold_positions==std::vector<f32>{0,1});
    const auto widgets=f.screen.inspect();REQUIRE(widgets);
    for(const auto& w:widgets->widgets)if(w.visible) {
        if(w.role==ui::WidgetRole::button)CHECK(w.text!="Add scaffold");
        CHECK(w.text!="Remove scaffold");CHECK(w.text!="Restore automatic scaffold spacing");
        CHECK(w.label!="Scaffold spacing (Earth radii; 0 = auto)");
    }
}
TEST_CASE("A path-constrained move uses pointer projection and sensitivity-scaled arrows", "[editor][ui][surface-gizmo][manual-scaffolds]") {
    auto path=std::make_shared<MovePath>();path->points={{-.6F,0,1},{0,.3F,1.2F},{.6F,0,1}};
    REQUIRE(path->valid());CHECK_FALSE(MovePath{}.valid());CHECK_FALSE((MovePath{{{0,0,1},{0,0,1}}}).valid());
    gfx::Camera camera;camera.set_position({0,0,4}).look_at({});auto view=camera.snapshot({800,600});REQUIRE(view);
    const ui::Rect viewport{0,0,800,600};
    SurfaceMove target{{},path->sample(.2F),1,"Support",Mat4::identity(),false,{},path};
    SurfacePartTool tool;
    const auto pump=[&](std::initializer_list<input::Event> events={},float speed=1) {
        const std::span<const input::Event> raw{events.begin(),events.size()};
        return tool.update(target,*view,viewport,raw,raw,true,!tool.dragging(),speed);
    };
    pump();REQUIRE(tool.handle());const auto start=*tool.handle();
    REQUIRE(pump({{.kind=input::EventKind::pointer_down,.position=start}}).began);
    const auto clicked=pump({{.kind=input::EventKind::pointer_up,.position=start}});
    CHECK(clicked.finished);CHECK_FALSE(clicked.changed);pump();
    REQUIRE(pump({{.kind=input::EventKind::pointer_down,.position=start}}).began);
    const auto dragged=pump({{.kind=input::EventKind::pointer_move,.position={500,240}}});REQUIRE(dragged.changed);
    const auto t=path->parameter(dragged.position);CHECK(t>.2F);
    const auto constrained=path->sample(t);
    CHECK(dragged.position.x==Catch::Approx(constrained.x).margin(.00001F));CHECK(dragged.position.y==Catch::Approx(constrained.y).margin(.00001F));
    CHECK(dragged.position.z==Catch::Approx(constrained.z).margin(.00001F));
    REQUIRE(pump({{.kind=input::EventKind::key_down,.key=input::Key::escape}}).cancelled);pump();
    REQUIRE(pump({{.kind=input::EventKind::key_down,.position=start,.key=input::Key::g}}).began);
    const auto arrow=pump({{.kind=input::EventKind::key_down,.key=input::Key::right}},2);
    REQUIRE(arrow.changed);CHECK(path->parameter(arrow.position)==Catch::Approx(.22F));
    const auto clamped=pump({{.kind=input::EventKind::key_down,.key=input::Key::right}},1000);
    CHECK(clamped.position==path->points.back());
    CHECK_FALSE(pump({{.kind=input::EventKind::key_down,.key=input::Key::right}}).changed);
    REQUIRE(pump({{.kind=input::EventKind::key_down,.key=input::Key::escape}}).cancelled);
    target.frame[0][0]=.8F;target.frame[1][1]=1.2F;target.frame[3]={.1F,0,0,1};pump();REQUIRE(tool.handle());
    const auto framed=*tool.handle();REQUIRE(pump({{.kind=input::EventKind::pointer_down,.position=framed}}).began);
    camera.set_position({.2F,.1F,4}).look_at({});view=camera.snapshot({800,600});REQUIRE(view);pump();
    const auto release=pump({{.kind=input::EventKind::pointer_up,.position=framed}});
    REQUIRE(release.finished);CHECK(path->parameter(release.position)==Catch::Approx(.2F).margin(.0001F));
}
TEST_CASE("A multi-handle surface gizmo selects and drags either endpoint immediately", "[editor][ui][surface-gizmo][endpoints]") {
    gfx::Camera camera;camera.set_position({0,0,4}).look_at({});
    const auto view=camera.snapshot({800,600});REQUIRE(view);
    const ui::Rect viewport{0,0,800,600};
    const std::array handles{SurfaceMove{{},{-.3F,0,std::sqrt(.91F)},1,"Endpoint A"},
        SurfaceMove{{},{.3F,0,std::sqrt(.91F)},1,"Endpoint B"}};
    SurfacePartTool tool;
    tool.update(handles,0,*view,viewport,{},{},true,true);
    auto positions=tool.handle_positions();REQUIRE(positions.size()==2);REQUIRE(positions[0]);REQUIRE(positions[1]);
    CHECK_FALSE(tool.rotation().visible());
    const auto first=*positions[0],second=*positions[1];
    const std::array press{input::Event{.kind=input::EventKind::pointer_down,.position=second}};
    auto action=tool.update(handles,0,*view,viewport,press,press,true,true);
    REQUIRE(action.began);CHECK(action.selected_handle==1);CHECK(tool.dragging());
    const std::array move{input::Event{.kind=input::EventKind::pointer_move,.position={second.x+35,second.y-20}}};
    action=tool.update(handles,1,*view,viewport,move,move,true,false);
    REQUIRE(action.changed);CHECK(action.position.x>handles[1].position.x);
    CHECK(tool.handle_positions()[0]==first);
    const std::array release{input::Event{.kind=input::EventKind::pointer_up,.position=move[0].position}};
    CHECK(tool.update(handles,1,*view,viewport,release,release,true,false).finished);
    const std::array press_first{input::Event{.kind=input::EventKind::pointer_down,.position=first}};
    action=tool.update(handles,1,*view,viewport,press_first,press_first,true,true);
    REQUIRE(action.began);CHECK(action.selected_handle==0);
    const std::array cancel{input::Event{.kind=input::EventKind::key_down,.key=input::Key::escape}};
    CHECK(tool.update(handles,0,*view,viewport,cancel,cancel,true,false).cancelled);
    tool.update(handles,0,*view,viewport,{},{},true,true);
    ui::DrawList list;tool.append(list,font(),false,0);
    CHECK(std::ranges::any_of(list.commands,[](const auto& command){
        const auto* label=std::get_if<ui::TextDraw>(&command);return label&&label->text=="Endpoint B";
    }));
}
TEST_CASE("New Earth structures use blueprint-declared placement and terminal properties", "[editor][ui][infrastructure][structures]") {
    Fixture f;f.frame.logical_size={800,3000};f.frame.framebuffer={800,3000};f.pump();
    gfx::Camera camera;camera.set_position({0,0,4}).look_at({});
    const auto view=camera.snapshot({800,600});REQUIRE(view);
    const auto before=f.editing.state().document.mesh.document();
    for(const auto name:{"Place dispersal terminal","Place orbital elevator","Place tunnel joiner"}) {
        f.click(name);REQUIRE(f.panel.placing());REQUIRE(f.panel.place_part({.5F,.5F},*view));f.finish();f.pump();
        REQUIRE(f.panel.gizmo());CHECK(f.panel.gizmo()->can_rotate);
    }
    CHECK(example::earth::infrastructure_parts(editable_mesh(f.editing.state())->document())->size()==3);
    CHECK(f.editing.state().document.mesh.document()==before);
    f.click("Place skyway");REQUIRE(f.panel.place_part({.5F,.5F},*view));f.finish();f.pump();
    const auto plain=editable_mesh(f.editing.state())->document();
    f.click("Dispersal terminal at B");CHECK(editable_mesh(f.editing.state())->document()==plain);
    f.click("Apply part properties");f.finish();f.pump();
    CHECK(example::earth::infrastructure_parts(editable_mesh(f.editing.state())->document())->back().terminal_b);
    CHECK(editable_mesh(f.editing.state())->size()>plain.vertex_count);
    REQUIRE(f.editing.undo());f.pump();CHECK(editable_mesh(f.editing.state())->document()==plain);
}
TEST_CASE("Structure height and support controls are undoable blueprint-owned edits", "[editor][ui][infrastructure][structure-placement]") {
    namespace earth=example::earth;
    Fixture f;f.frame.logical_size={800,3800};f.frame.framebuffer={800,3800};f.pump();
    gfx::Camera camera;camera.set_position({0,0,4}).look_at({});
    const auto view=camera.snapshot({800,600});REQUIRE(view);
    f.click("Place dispersal terminal");REQUIRE(f.panel.place_part({.5F,.5F},*view));f.finish();f.pump();
    const auto original=editable_mesh(f.editing.state())->document();
    auto gizmo=f.panel.gizmo();REQUIRE(gizmo);REQUIRE(gizmo->radial_range);
    CHECK(f.widget("Width / footprint").enabled);
    REQUIRE(f.panel.edit_part({.began=true}));
    REQUIRE(f.panel.edit_part({.changed=true,.finished=true,.position={0,0,gizmo->radius+.12F}}));f.finish();f.pump();
    CHECK(earth::infrastructure_parts(editable_mesh(f.editing.state())->document())->front().altitude==Catch::Approx(.12F));
    CHECK(f.panel.gizmo()->radius==Catch::Approx(gizmo->radius+.12F));
    REQUIRE(f.editing.undo());f.pump();CHECK(editable_mesh(f.editing.state())->document()==original);
    f.fill("Altitude (Earth radii)","0.1");
    CHECK(editable_mesh(f.editing.state())->document()==original);
    f.click("Apply part properties");f.finish();f.pump();
    auto recipe=earth::infrastructure_parts(editable_mesh(f.editing.state())->document())->front();
    CHECK(recipe.altitude==Catch::Approx(.1F));CHECK(recipe.scale==1.F);CHECK(recipe.scaffold);
    REQUIRE(f.editing.undo());f.pump();CHECK(editable_mesh(f.editing.state())->document()==original);
    f.click("Place skyway");REQUIRE(f.panel.place_part({.55F,.5F},*view));f.finish();f.pump();
    const auto automatic=editable_mesh(f.editing.state())->document();
    REQUIRE(f.panel.select_gizmo(4));f.pump();
    f.option("scaffold_setup",editor::Phase::commit,{{"spacing",.06F}});
    f.option("apply_scaffold_setup");
    const auto spaced=editable_mesh(f.editing.state())->document();CHECK(spaced.vertex_count>automatic.vertex_count);
    REQUIRE(f.panel.select_gizmo(2));f.pump();
    f.option("gizmo_scaffolding",editor::Phase::commit,{{"visible",false}});
    CHECK(editable_mesh(f.editing.state())->document().vertex_count<spaced.vertex_count);
    REQUIRE(f.editing.undo());f.pump();CHECK(editable_mesh(f.editing.state())->document()==spaced);
}
TEST_CASE("Tunnel connection dropdowns attach detach and undo without changing scene instances", "[editor][ui][infrastructure][sockets]") {
    namespace earth=example::earth;
    Fixture f;f.frame.logical_size={800,3400};f.frame.framebuffer={800,3400};f.pump();
    gfx::Camera camera;camera.set_position({0,0,4}).look_at({});
    const auto view=camera.snapshot({800,600});REQUIRE(view);
    const auto scene_count=f.editing.state().document.instances.size();
    for(const auto name:{"Place dispersal terminal","Place tunnel joiner","Place skyway"}) {
        f.click(name);REQUIRE(f.panel.place_part({.5F,.5F},*view));f.finish();f.pump();
    }
    const auto before=editable_mesh(f.editing.state())->document();
    CHECK(f.widget("End B connection").text=="Free endpoint (detached)");
    f.click("End B connection");f.choose("Dispersal terminal 1 / Entrance");f.finish();f.pump();
    auto parts=earth::infrastructure_parts(editable_mesh(f.editing.state())->document());REQUIRE(parts);
    CHECK(parts->back().socket_b==earth::TunnelSocketRef{1,1});
    CHECK(f.panel.gizmo_handles().size()==1);CHECK(f.panel.gizmo()->label=="Endpoint A");
    CHECK(f.widget("End B connection").text=="Dispersal terminal 1 / Entrance");
    const auto connected=editable_mesh(f.editing.state())->document();
    f.click("End B connection");f.choose("Free endpoint (detached)");f.finish();f.pump();
    CHECK_FALSE(earth::infrastructure_parts(editable_mesh(f.editing.state())->document())->back().socket_b);
    REQUIRE(f.editing.undo());f.pump();CHECK(editable_mesh(f.editing.state())->document()==connected);
    REQUIRE(f.editing.undo());f.pump();CHECK(editable_mesh(f.editing.state())->document()==before);
    f.click("End B connection");f.choose("Tunnel joiner 2 / Left outlet");f.finish();f.pump();
    CHECK(earth::infrastructure_parts(editable_mesh(f.editing.state())->document())->back().socket_b==earth::TunnelSocketRef{2,2});
    CHECK(f.editing.state().document.instances.size()==scene_count);
    CHECK(f.editing.state().document.mesh.document()!=editable_mesh(f.editing.state())->document());
}
TEST_CASE("Viewport connection picks the structure then an available socket without selecting the target", "[editor][ui][sockets][viewport-connection]") {
    namespace earth=example::earth;
    Fixture f;f.frame.logical_size={800,3400};f.frame.framebuffer={800,3400};
    auto doc=earth::add_infrastructure(earth::make_mesh({.visible=false}),earth::InfrastructureKind::joiner,{0,0});REQUIRE(doc);
    doc=earth::add_infrastructure(*doc,earth::InfrastructureKind::skyway,{10,12});REQUIRE(doc);
    auto mesh=editor::EditableMesh::create(*doc);REQUIRE(mesh);
    REQUIRE(f.editing.replace_mesh_draft(BlueprintId::mesh,f.editing.state().document.revision,std::move(*mesh)));
    f.pump();const MeshPartId source{std::string(earth::infrastructure_part_field),2};
    REQUIRE(f.panel.select_part(source));f.pump();
    gfx::Camera camera;camera.set_position({0,0,1.4F}).look_at({});
    const auto view=camera.snapshot({800,600});REQUIRE(view);
    const auto revision=f.editing.state().document.revision;
    f.click("Attach endpoint A in viewport");REQUIRE(f.panel.connecting());
    CHECK_FALSE(f.panel.gizmo());CHECK(f.panel.connection_slots().empty());
    CHECK_FALSE(f.panel.pick_connection_target({0,0},*view));
    REQUIRE(f.panel.pick_connection_target({.5F,.5F},*view));
    CHECK(f.panel.selected_part()==source);CHECK(f.editing.state().document.revision==revision);
    auto slots=f.panel.connection_slots();REQUIRE(slots.size()==3);
    SocketPickTool picker;picker.update(slots,*view,{0,0,800,600});
    const auto points=picker.handles();REQUIRE(points.size()==3);
    for(std::size_t i=0;i<points.size();++i){REQUIRE(points[i]);CHECK(picker.hit(*points[i])==i);}
    CHECK_FALSE(picker.hit({-100,-100}));CHECK_FALSE(f.panel.attach_slot(99));
    REQUIRE(f.panel.attach_slot(1));CHECK_FALSE(f.panel.connecting());f.finish();f.pump();
    auto parts=earth::infrastructure_parts(editable_mesh(f.editing.state())->document());REQUIRE(parts);
    CHECK(parts->back().socket_a==earth::TunnelSocketRef{1,2});CHECK(f.panel.selected_part()==source);
    f.click("Attach endpoint B in viewport");REQUIRE(f.panel.pick_connection_target({.5F,.5F},*view));
    CHECK(f.panel.connection_slots().size()==2); // occupied left outlet excluded
    f.panel.cancel_connection();CHECK_FALSE(f.panel.connecting());
    REQUIRE(f.editing.undo());f.pump();CHECK(editable_mesh(f.editing.state())->document()==*doc);
}
TEST_CASE("Free endpoint altitude uses a selectable radial gizmo with drag arrows and cancellation", "[editor][ui][surface-gizmo][altitude]") {
    gfx::Camera camera;camera.set_position({2,1,4}).look_at({0,0,1});
    const auto view=camera.snapshot({800,600});REQUIRE(view);
    SurfacePartTool tool;SurfaceMove target{{},{0,0,1.024F},1.024F,"Endpoint A",Mat4::identity(),false,Vec2{1.024F,2}};
    const auto pump=[&](std::initializer_list<input::Event> events={},float speed=1) {
        const std::span<const input::Event> raw{events.begin(),events.size()};
        return tool.update(target,*view,{0,0,800,600},raw,raw,true,!tool.dragging(),speed);
    };
    pump();const auto handle=tool.altitude().handle("Up / down");REQUIRE(handle);REQUIRE(tool.handle());
    const auto origin=*tool.handle();const Vec2 next{handle->x+(handle->x-origin.x)*.7F,handle->y+(handle->y-origin.y)*.7F};
    REQUIRE(pump({{.kind=input::EventKind::pointer_down,.position=*handle}}).began);
    auto action=pump({{.kind=input::EventKind::pointer_move,.position=next}});
    REQUIRE(action.changed);CHECK(action.position.z>target.position.z);CHECK(action.position.x==0);CHECK(action.position.y==0);
    REQUIRE(pump({{.kind=input::EventKind::pointer_up,.position=next}}).finished);
    CHECK(tool.altitude().selected_axis());pump();
    auto arrow=pump({{.kind=input::EventKind::key_down,.key=input::Key::up}},.5F);
    REQUIRE(arrow.changed);CHECK(arrow.position.z>target.position.z);
    REQUIRE(pump({{.kind=input::EventKind::key_down,.key=input::Key::escape}}).cancelled);
    pump();CHECK_FALSE(tool.dragging());
    // Limits constrain the visible axis too, not only the eventual authoring edit.
    auto tip=tool.altitude().handle("Up / down");REQUIRE(tip);
    REQUIRE(pump({{.kind=input::EventKind::pointer_down,.position=*tip}}).began);
    const auto bottom=pump({{.kind=input::EventKind::key_down,.key=input::Key::down}},1000);
    CHECK(bottom.position.z==Catch::Approx(1.024F));
    REQUIRE(tool.altitude().preview_position());
    CHECK(tool.altitude().preview_position()->z==Catch::Approx(1.024F));
    auto top=pump({{.kind=input::EventKind::key_down,.key=input::Key::up}},1000);
    CHECK(top.position.z==Catch::Approx(2.F));
    CHECK(tool.altitude().preview_position()->z==Catch::Approx(2.F));
    const auto released=pump({{.kind=input::EventKind::pointer_up,.position=*tip}});
    REQUIRE(released.finished);CHECK(released.position.z==Catch::Approx(2.F));
    target.radial_range.reset();pump();CHECK_FALSE(tool.altitude().visible());
}
TEST_CASE("Structure altitude and heading rings have exclusive keyboard focus", "[editor][ui][surface-gizmo][structure-placement]") {
    gfx::Camera camera;camera.set_position({2,1,4}).look_at({0,0,1});
    const auto view=camera.snapshot({800,600});REQUIRE(view);
    SurfacePartTool tool;SurfaceMove target{{},{0,0,1.1F},1.1F,"Terminal",Mat4::identity(),true,Vec2{1.05F,2}};
    const auto pump=[&](std::initializer_list<input::Event> events={}) {
        const std::span<const input::Event> raw{events.begin(),events.size()};
        return tool.update(target,*view,{0,0,800,600},raw,raw,true,!tool.dragging());
    };
    pump();auto tip=tool.altitude().handle("Up / down");REQUIRE(tip);
    pump({{.kind=input::EventKind::pointer_down,.position=*tip},{.kind=input::EventKind::pointer_up,.position=*tip}});
    REQUIRE(tool.altitude().selected_axis());
    const auto ring=tool.rotation().rings()[2].points[20];
    auto turn=pump({{.kind=input::EventKind::pointer_down,.position=ring},{.kind=input::EventKind::pointer_up,.position=ring}});
    REQUIRE(turn.rotation_degrees);REQUIRE(tool.rotation().selected_axis());CHECK_FALSE(tool.altitude().selected_axis());
    turn=pump({{.kind=input::EventKind::key_down,.position=ring,.key=input::Key::up}});
    REQUIRE(turn.rotation_degrees);CHECK(turn.changed);
    pump({{.kind=input::EventKind::key_down,.key=input::Key::enter}});pump();
    tip=tool.altitude().handle("Up / down");REQUIRE(tip);
    pump({{.kind=input::EventKind::pointer_down,.position=*tip},{.kind=input::EventKind::pointer_up,.position=*tip}});
    REQUIRE(tool.altitude().selected_axis());CHECK_FALSE(tool.rotation().selected_axis());
    const auto lift=pump({{.kind=input::EventKind::key_down,.position=*tip,.key=input::Key::up}});
    CHECK(lift.changed);CHECK_FALSE(lift.rotation_degrees);CHECK(lift.position.z>target.position.z);
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
TEST_CASE("A surface heading edit finishes on an outside click after arrow rotation", "[editor][ui][surface-gizmo][gizmo-focus]") {
    gfx::Camera camera;camera.set_position({0,0,4}).look_at({});
    const auto view=camera.snapshot({800,600});REQUIRE(view);
    const ui::Rect viewport{0,0,800,600};
    SurfacePartTool tool;
    const SurfaceMove target{{},{0,0,1},1,"Whole part",Mat4::identity(),true};
    const auto pump=[&](std::initializer_list<input::Event> events={},bool available=true) {
        const std::span<const input::Event> raw{events.begin(),events.size()};
        return tool.update(target,*view,viewport,available?raw:std::span<const input::Event>{},raw,true,!tool.dragging());
    };
    pump();
    const auto ring=tool.rotation().rings()[2].points[8];
    REQUIRE(pump({{.kind=input::EventKind::pointer_down,.position=ring}}).began);
    REQUIRE(pump({{.kind=input::EventKind::pointer_up,.position=ring}}).finished);
    REQUIRE(tool.rotation().selected_axis()==2);
    REQUIRE(pump({{.kind=input::EventKind::key_down,.position=ring,.key=input::Key::left}}).began);
    REQUIRE(tool.dragging());
    // UI clicks must not end a keyboard edit or clear the selected ring.
    const Vec2 outside{50,50};
    pump({{.kind=input::EventKind::pointer_down,.position=outside}},false);
    pump({{.kind=input::EventKind::pointer_up,.position=outside}},false);
    REQUIRE(tool.dragging());CHECK(tool.rotation().selected_axis()==2);
    auto down=pump({{.kind=input::EventKind::pointer_down,.position=outside}});
    CHECK_FALSE(down.finished);CHECK(tool.dragging());
    auto up=pump({{.kind=input::EventKind::pointer_up,.position=outside}});
    REQUIRE(up.finished);CHECK_FALSE(up.cancelled);REQUIRE(up.rotation_degrees);
    CHECK(*up.rotation_degrees==1.F);
    CHECK_FALSE(tool.dragging());CHECK_FALSE(tool.rotation().selected_axis());
    // Once finished, ordinary viewport clicks reach selection again.
    pump({{.kind=input::EventKind::pointer_down,.position=outside}});CHECK_FALSE(tool.handled());
    pump({{.kind=input::EventKind::pointer_up,.position=outside}});CHECK_FALSE(tool.handled());
}
TEST_CASE("Cloud creation removal and rotation are blueprint-owned undoable actions", "[editor][ui][cloud-catalog]") {
    Fixture f;f.pump();f.click("Clouds visible");f.click("Rebuild clouds");f.finish();f.pump();
    f.click("Add spiral cloud");f.finish();f.pump();
    auto formations=example::earth::cloud_formations(editable_mesh(f.editing.state())->document());REQUIRE(formations);
    REQUIRE(formations->size()==18);const auto id=formations->back().id;
    CHECK(f.panel.selected_part()==cloud_part(id));f.pump();CHECK(f.widget("Rotate cloud").enabled);
    f.click("Remove cloud");f.finish();f.pump();
    CHECK_FALSE(f.panel.selected_part());
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
        CHECK(panel.pick_part({.5F,.5F},*snapshot)==cloud_part(15));
        CHECK_FALSE(panel.pick_part({.5F,.8F},*snapshot)); // not the farther cloud
        CHECK_FALSE(panel.pick_part({0,0},*snapshot));
        CHECK_FALSE(panel.pick_part({-1,0},*snapshot));
    }
    REQUIRE(panel.select_part(cloud_part(15)));CHECK(panel.selected_part()==cloud_part(15));REQUIRE(panel.gizmo());
    CHECK(panel.gizmo()->label=="Atlantic spiral");
    CHECK_FALSE(panel.select_part(cloud_part(999)));CHECK(panel.selected_part()==cloud_part(15));
    REQUIRE(panel.select_part({}));CHECK_FALSE(panel.gizmo());
    CHECK(editing.state().document.revision==revision);CHECK_FALSE(editing.dirty());
    CHECK_FALSE(editing.can_undo());
    REQUIRE(panel.select_part(cloud_part(15)));
    auto placement=Mat4::identity();placement[0][0]=2;placement[1][1]=.7F;placement[3][0]=1;
    REQUIRE(editing.begin_mesh_transform(BlueprintId::mesh));REQUIRE(editing.mesh_transform(placement));REQUIRE(editing.commit());
    panel.sync();REQUIRE(panel.gizmo());CHECK(panel.gizmo()->frame==placement);
    const auto snapshot=camera.snapshot({800,600});REQUIRE(snapshot);
    // Orthographic view: translated cloud is now right of the centre; original
    // screen centre hits terrain. Test through the real shared geometry BVH.
    CHECK(panel.pick_part({.65F,.5F},*snapshot)==cloud_part(15));
    CHECK_FALSE(panel.pick_part({.5F,.5F},*snapshot));
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
