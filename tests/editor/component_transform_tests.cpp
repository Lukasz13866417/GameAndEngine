#include "../../examples/editor/component_transform.hpp"
#include "../../examples/editor/mesh_transform.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

namespace {
using namespace vng;
using namespace editor_example;
struct Fixture {
    ComponentTransform tool;
    std::array<editor::ScenePoint,2> points{{{4,{-1,0,0},{}},{9,{1,0,0},{}}}};
    gfx::CameraSnapshot camera;
    Fixture() {
        gfx::Camera c;c.set_position({0,0,10}).look_at({}).set_orthographic({.vertical_height=10});
        camera=*c.snapshot({800,600});
    }
    ComponentChange pump(std::initializer_list<input::Event> events,bool unhandled=true,bool enabled=true) {
        std::span<const input::Event> raw{events.begin(),events.size()};
        return tool.update(points,{1,2,3},camera,{0,0,800,600},unhandled?raw:std::span<const input::Event>{},raw,enabled);
    }
};
}
TEST_CASE("Transform arrow nudges compose with mouse movement and respect constraints", "[editor][ui][components][arrows]") {
    using Key=input::Key;using Kind=input::EventKind;
    for(unsigned axis=0;axis<3;++axis) {
        Fixture constrained;
        constrained.points[0].position={-1,-1,-1};
        constrained.points[1].position={1,1,1};
        const auto key=std::array{Key::x,Key::y,Key::z}[axis];
        REQUIRE(constrained.pump({{.kind=Kind::key_down,.position={460,300},.key=Key::r},
            {.kind=Kind::key_down,.key=key}}).began);
        const auto turn=constrained.pump({{.kind=Kind::key_down,.key=Key::up}});
        REQUIRE(turn.changed);
        Vec3 degrees{};degrees[axis]=1;
        const auto expected=rotation_math::direction(degrees,{1,1,1});
        for(unsigned c=0;c<3;++c)
            CHECK(turn.points[1].position[c]==Catch::Approx(expected[c]).margin(1e-5));
    }
    for(auto arrow:{Key::left,Key::up,Key::right,Key::down}) {
        Fixture f;
        REQUIRE(f.pump({{.kind=Kind::key_down,.position={460,300},.key=Key::r},
            {.kind=Kind::key_down,.key=Key::z}}).began);
        auto turn=f.pump({{.kind=Kind::key_down,.key=arrow}});REQUIRE(turn.changed);
        const auto expected=(arrow==Key::left||arrow==Key::up)?1.F:-1.F;
        CHECK(turn.points[1].position.y==Catch::Approx(std::sin(expected*rotation_math::radians)).margin(1e-5));
        auto still=f.pump({{.kind=Kind::pointer_move,.position={460,300}}});
        CHECK(still.points[1].position==turn.points[1].position); // Mouse cannot erase a keyboard nudge.
        auto fine=f.pump({{.kind=Kind::key_down,.key=arrow,.modifiers={.shift=true},.repeat=true}});
        CHECK(fine.points[1].position.y==Catch::Approx(std::sin(expected*1.1F*rotation_math::radians)).margin(1e-5));
        CHECK(f.pump({{.kind=Kind::key_down,.key=Key::escape}}).cancelled);
    }
    Fixture f;REQUIRE(f.pump({{.kind=Kind::key_down,.position={400,300},.key=Key::g}}).began);
    auto x=f.pump({{.kind=Kind::key_down,.key=Key::right}});REQUIRE(x.changed);
    auto xy=f.pump({{.kind=Kind::key_down,.key=Key::up}});REQUIRE(xy.changed);
    CHECK(xy.points[0].position.x==x.points[0].position.x);
    CHECK(xy.points[0].position.y>x.points[0].position.y);
    auto ctrl=f.pump({{.kind=Kind::key_down,.key=Key::left,.modifiers={.control=true}}});CHECK_FALSE(ctrl.changed);
    CHECK(f.pump({{.kind=Kind::key_down,.key=Key::enter}}).finished);
    // Passive gizmos also capture keyboard gestures, without first pressing G/R.
    Fixture passive;auto move=passive.pump({{.kind=Kind::key_down,.position={400,300},.key=Key::right}});
    REQUIRE(move.began);REQUIRE(move.changed);CHECK(move.points[0].position.x> -1);
    CHECK(passive.pump({{.kind=Kind::key_down,.key=Key::escape}}).cancelled);
    passive.points[0].position={-1,-1,-1};
    passive.points[1].position={1,1,1};
    passive.tool.gizmo(GizmoMode::rotate);
    auto rotate=passive.pump({{.kind=Kind::key_down,.position={400,300},.key=Key::up}});
    REQUIRE(rotate.began);REQUIRE(rotate.changed);CHECK(rotate.points[1].position!=passive.points[1].position);
}
TEST_CASE("G moves components without mouse buttons and preserves the captured baseline", "[editor][ui][components]") {
    Fixture f;
    CHECK_FALSE(f.pump({{.kind=input::EventKind::pointer_down,.position={340,300}},
                       {.kind=input::EventKind::pointer_move,.position={320,300}}}).changed);
    auto began=f.pump({{.kind=input::EventKind::key_down,.position={400,300},.key=input::Key::g}});
    REQUIRE(began.began);REQUIRE(f.tool.active());
    auto moved=f.pump({{.kind=input::EventKind::pointer_move,.position={460,240}}},false);
    REQUIRE(moved.changed);REQUIRE(moved.points.size()==2);
    CHECK(moved.points[0].id==4);
    CHECK(moved.points[0].position.x==Catch::Approx(0));
    CHECK(moved.points[0].position.y==Catch::Approx(1));
    f.points[0].position=moved.points[0].position; // Host sends a live preview back.
    moved=f.pump({{.kind=input::EventKind::pointer_move,.position={520,300}}},false);
    CHECK(moved.points[0].position.x==Catch::Approx(1)); // Not incremental.
    auto done=f.pump({{.kind=input::EventKind::key_down,.key=input::Key::enter}},false);
    CHECK(done.finished);CHECK_FALSE(done.cancelled);CHECK(done.changed);CHECK_FALSE(f.tool.active());
    CHECK(done.points[0].position.x==Catch::Approx(1));
}
TEST_CASE("Normalized arrow amounts reach both modal and passive component gizmos", "[editor][ui][components][arrows]") {
    for(bool modal:{false,true})for(bool rotating:{false,true}) {
        Fixture f;f.tool.gizmo(rotating?GizmoMode::rotate:GizmoMode::move);
        f.points[0].position={-1,-1,-1};f.points[1].position={1,1,1};
        if(modal)REQUIRE(f.pump({{.kind=input::EventKind::key_down,.position={460,300},
            .key=rotating?input::Key::r:input::Key::g}}).began);
        const std::array arrow{input::Event{.kind=input::EventKind::key_down,.position={460,300},.key=input::Key::right}};
        const auto slow=f.tool.update(f.points,{1,2,3},f.camera,{0,0,800,600},arrow,arrow,true,{},.5F);
        REQUIRE(slow.changed);
        const auto fast=f.tool.update(f.points,{1,2,3},f.camera,{0,0,800,600},arrow,arrow,true,{},1.5F);
        REQUIRE(fast.changed);
        if(rotating) {
            const auto distance=[](Vec3 p) {
                return std::sqrt((p.x-1)*(p.x-1)+(p.y-1)*(p.y-1)+(p.z-1)*(p.z-1));
            };
            CHECK(distance(fast.points[1].position)>distance(slow.points[1].position)*3.9F);
        } else {
            CHECK(fast.points[1].position.x-1.F==Catch::Approx((slow.points[1].position.x-1.F)*4.F));
        }
    }
}
TEST_CASE("Modal transforms survive camera changes without reapplying motion", "[editor][ui][gizmo-navigation]") {
    for(auto key:{input::Key::g,input::Key::r,input::Key::s}) {
        Fixture f;
        REQUIRE(f.pump({{.kind=input::EventKind::key_down,.position={460,300},.key=key}}).began);
        const auto before=f.pump({{.kind=input::EventKind::pointer_move,.position={490,275}}});
        REQUIRE(before.changed);
        gfx::Camera camera;camera.set_position({4,3,12}).look_at({});
        f.camera=*camera.snapshot({800,600});
        const auto reframed=f.pump({});
        CHECK_FALSE(reframed.cancelled);CHECK_FALSE(reframed.changed);REQUIRE(f.tool.active());
        const auto same=f.pump({{.kind=input::EventKind::pointer_move,.position={490,275}}});
        REQUIRE(same.points.size()==before.points.size());
        for(unsigned i=0;i<2;++i)for(unsigned c=0;c<3;++c)
            CHECK(same.points[i].position[c]==Catch::Approx(before.points[i].position[c]).margin(1e-5));
        const auto moved=f.pump({{.kind=input::EventKind::pointer_move,.position={510,265}}});
        CHECK(moved.points[0].position!=same.points[0].position);
        CHECK(f.pump({{.kind=input::EventKind::key_down,.key=input::Key::escape}}).cancelled);
    }
}
TEST_CASE("Mesh and region point selections share a center-preserving MMB gizmo", "[editor][ui][components][free-rotate]") {
    Fixture f;f.tool.gizmo(GizmoMode::free_rotate);
    REQUIRE(f.pump({{.kind=input::EventKind::pointer_down,.position={400,300},.button=2}}).began);
    auto moved=f.pump({{.kind=input::EventKind::pointer_move,.position={560,380}}},false);
    REQUIRE(moved.changed);REQUIRE(moved.points.size()==2);
    for(unsigned c=0;c<3;++c)CHECK(moved.points[0].position[c]+moved.points[1].position[c]==Catch::Approx(0).margin(.00001));
    CHECK(moved.points[0].position!=f.points[0].position);
    const auto done=f.pump({{.kind=input::EventKind::pointer_up,.position={560,380},.button=2}},false);
    CHECK(done.finished);CHECK_FALSE(done.cancelled);CHECK_FALSE(f.tool.active());
    CHECK(f.tool.gizmo()==GizmoMode::free_rotate);
    // R still switches back to the conventional modal rotation gizmo.
    REQUIRE(f.pump({{.kind=input::EventKind::key_down,.position={400,300},.key=input::Key::r}}).began);
    CHECK(f.tool.gizmo()==GizmoMode::rotate);
}
TEST_CASE("Modal rotate and scale use the selected component center", "[editor][ui][components]") {
    for(auto key:{input::Key::r,input::Key::s}) {
        Fixture f;
        REQUIRE(f.pump({{.kind=input::EventKind::key_down,.position={460,300},.key=key}}).began);
        const Vec2 end=key==input::Key::r?Vec2{400,240}:Vec2{520,300};
        auto done=f.pump({{.kind=input::EventKind::pointer_move,.position=end},
                         {.kind=input::EventKind::pointer_down,.position=end}},false);
        REQUIRE(done.finished);REQUIRE(done.changed);CHECK_FALSE(done.cancelled);
        CHECK(done.points[0].position.x==Catch::Approx(key==input::Key::r?0:-2).margin(.00001));
        CHECK(done.points[0].position.y==Catch::Approx(key==input::Key::r?-1:0).margin(.00001));
        CHECK(done.points[0].position.x+done.points[1].position.x==Catch::Approx(0));
        CHECK(done.points[0].position.y+done.points[1].position.y==Catch::Approx(0));
    }
}
TEST_CASE("Modal transforms constrain axes and cancel on RMB, Escape, focus loss or disable", "[editor][ui][components]") {
    for(auto kind:{input::EventKind::pointer_down,input::EventKind::key_down,input::EventKind::focus_lost}) {
        Fixture f;
        REQUIRE(f.pump({{.kind=input::EventKind::key_down,.position={400,300},.key=input::Key::g}}).began);
        auto move=f.pump({{.kind=input::EventKind::key_down,.key=input::Key::x},
                         {.kind=input::EventKind::pointer_move,.position={460,240}}},false);
        REQUIRE(move.changed);CHECK(move.points[0].position.y==0);
        auto cancel=f.pump({{.kind=kind,.key=input::Key::escape,.button=1}},false);
        CHECK(cancel.cancelled);CHECK(cancel.finished);CHECK(f.tool.handled());CHECK_FALSE(f.tool.active());
    }
    Fixture f;
    REQUIRE_FALSE(f.pump({{.kind=input::EventKind::key_down,.position={400,300},.key=input::Key::g}},false).began);
    REQUIRE(f.pump({{.kind=input::EventKind::key_down,.position={400,300},.key=input::Key::g}}).began);
    CHECK(f.pump({},false,false).cancelled);
}

TEST_CASE("Keyboard transforms draw their gizmo and constraints draw only one axis", "[editor][ui][components][gizmo]") {
    gfx::Camera c;c.set_position({6,4,10}).look_at({}).set_orthographic({.vertical_height=10});
    const auto camera=*c.snapshot({800,600});
    for(auto key:{input::Key::g,input::Key::r,input::Key::s}) {
        TransformGesture tool;
        const auto pump=[&](input::Key k) {
            const std::array events{input::Event{.kind=input::EventKind::key_down,.position={450,300},.key=k}};
            return tool.update({1,2,3},{},camera,{0,0,800,600},events,events,true);
        };
        REQUIRE(pump(key).began);
        REQUIRE(tool.visible());
        ui::DrawList all;tool.append(all,{});
        REQUIRE_FALSE(all.commands.empty()); // Geometry does not depend on a font/hint.
        for(unsigned axis=0;axis<3;++axis) {
            const auto constraint=std::array{input::Key::x,input::Key::y,input::Key::z}[axis];
            REQUIRE(pump(constraint).changed);
            CHECK(tool.axis()==static_cast<int>(axis));
            ui::DrawList constrained;tool.append(constrained,{});
            REQUIRE_FALSE(constrained.commands.empty());
            std::array<unsigned,3> colored{};
            for(const auto& command:constrained.commands)if(const auto* box=std::get_if<ui::BoxDraw>(&command)) {
                for(unsigned channel=0;channel<3;++channel)
                    if(box->color[channel]>.6F && box->color[channel]>1.5F*box->color[(channel+1)%3] &&
                       box->color[channel]>1.5F*box->color[(channel+2)%3])++colored[channel];
            }
            CHECK(colored[axis]>0);
            CHECK(colored[(axis+1)%3]==0);
            CHECK(colored[(axis+2)%3]==0);
            REQUIRE(pump(constraint).changed);
            CHECK(tool.axis()==-1);
            ui::DrawList restored;tool.append(restored,{});
            CHECK(restored.commands.size()==all.commands.size());
        }
        REQUIRE(pump(input::Key::escape).cancelled);
        CHECK_FALSE(tool.visible());
        ui::DrawList cancelled;tool.append(cancelled,{});CHECK(cancelled.commands.empty());
    }
}

TEST_CASE("Mesh and region component adapters preserve modal gizmo geometry", "[editor][ui][components][gizmo]") {
    for(auto key:{input::Key::g,input::Key::r,input::Key::s}) {
        Fixture f;
        REQUIRE(f.pump({{.kind=input::EventKind::key_down,.position={450,300},.key=key}}).began);
        ui::DrawList list;f.tool.append(list,{});
        CHECK_FALSE(list.commands.empty());
        REQUIRE(f.pump({{.kind=input::EventKind::key_down,.key=input::Key::enter}},false).finished);
        ui::DrawList confirmed;f.tool.append(confirmed,{});
        CHECK_FALSE(confirmed.commands.empty()); // No one-frame gap on handoff.
    }
}

TEST_CASE("Whole mesh transforms need no selection and include normals and hidden geometry", "[editor][ui][whole-mesh]") {
    content::vmesh::Document d;d.vertex_count=4;
    d.vertex_fields={{"position",{content::vmesh::ScalarType::Float32,3},std::vector<f32>{-1,-1,0,1,-1,0,1,1,0,-1,1,0}},
        {"normal",{content::vmesh::ScalarType::Float32,3},std::vector<f32>{1,0,0,1,0,0,1,0,0,1,0,0}}};
    d.faces={{0,1,2},{0,2,3}};
    auto m=editor::EditableMesh::create(d);REQUIRE(m);
    State state{.document={.mesh=std::move(*m)}};state.viewport.mode=ViewMode::mesh;
    EditingSession editing{std::move(state)};
    auto font=text::Font::load(VNG_TEST_FONT_PATH);REQUIRE(font);
    ui::Screen screen{ui::dark_theme(*font)};
    MeshTools selection{screen.column(),screen.column()};selection.sync(editing.state());
    selection.mode(MeshSelectMode::face);selection.select(0,false);REQUIRE(selection.hide_selected()==1);
    selection.mode(MeshSelectMode::whole);
    const auto mask=selection.visibility();
    MeshTransform transform{editing};Fixture view;
    const auto pump=[&](std::initializer_list<input::Event> events) {
        std::span<const input::Event> raw{events.begin(),events.size()};
        return transform.update(selection,view.camera,{0,0,800,600},raw,raw,true);
    };
    REQUIRE(pump({{.kind=input::EventKind::key_down,.position={460,300},.key=input::Key::r}}));
    REQUIRE(transform.active());CHECK(selection.selected().empty());
    REQUIRE(pump({{.kind=input::EventKind::pointer_move,.position={400,240}},
        {.kind=input::EventKind::key_down,.position={400,240},.key=input::Key::enter}}));
    CHECK_FALSE(transform.active());
    CHECK(editable_mesh(editing.state())->document()==d);
    CHECK(editing.state().document.mesh_drafts.empty());
    auto baked=bake_mesh_placements(editing.state());REQUIRE(baked);
    const auto& rotated=*editable_mesh(*baked);
    CHECK(rotated.position(0).x==Catch::Approx(1).margin(1e-5));
    CHECK(rotated.position(0).y==Catch::Approx(-1).margin(1e-5));
    const auto& normals=std::get<std::vector<f32>>(rotated.document().vertex_fields[1].values);
    CHECK(normals[0]==Catch::Approx(0).margin(1e-5));CHECK(normals[1]==Catch::Approx(1).margin(1e-5));
    CHECK(selection.visibility()==mask);CHECK(editing.state().document.mesh.document()==d);
    REQUIRE(editing.undo());CHECK(editing.state().document.mesh_drafts.empty());CHECK_FALSE(editing.can_undo());
    REQUIRE(editing.redo());REQUIRE(editing.undo());
    REQUIRE(pump({{.kind=input::EventKind::key_down,.position={460,300},.key=input::Key::s}}));
    REQUIRE(pump({{.kind=input::EventKind::pointer_move,.position={520,300}},
        {.kind=input::EventKind::key_down,.key=input::Key::enter}}));
    CHECK(editable_mesh(editing.state())->document()==d);
    baked=bake_mesh_placements(editing.state());REQUIRE(baked);
    CHECK(editable_mesh(*baked)->position(0)==Vec3{-2,-2,0});
    REQUIRE(editing.undo());
    REQUIRE(pump({{.kind=input::EventKind::key_down,.position={460,300},.key=input::Key::s}}));
    REQUIRE(pump({{.kind=input::EventKind::key_down,.key=input::Key::x},
        {.kind=input::EventKind::pointer_move,.position={520,300}}}));
    baked=bake_mesh_placements(editing.state());REQUIRE(baked);
    CHECK(editable_mesh(*baked)->position(0)==Vec3{-2,-1,0});
    REQUIRE(pump({{.kind=input::EventKind::key_down,.key=input::Key::escape}}));
    CHECK(editing.state().document.mesh_drafts.empty());CHECK_FALSE(editing.can_undo());
    CHECK(editing.state().document.mesh_placements.empty());
    CHECK(selection.transform_mode()==GizmoMode::scale);
    REQUIRE(selection.cycle(1));CHECK(selection.transform_mode()==GizmoMode::free_rotate);
    REQUIRE(selection.cycle(1));CHECK(selection.transform_mode()==GizmoMode::rotate);
    REQUIRE(selection.cycle(-1));CHECK(selection.transform_mode()==GizmoMode::free_rotate);
    REQUIRE(pump({{.kind=input::EventKind::key_down,.position={460,300},.key=input::Key::g}}));
    CHECK_FALSE(transform.active());CHECK_FALSE(editing.busy());
}
