#include "../../examples/editor/cage_tool.hpp"
#include "../../examples/editor/regions.hpp"
#include "../../examples/editor/project.hpp"
#include "../../examples/editor/region_editor.hpp"
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace vng;
using namespace editor_example;
struct Fixture {
    Region region=make_region(RegionShape::box,{},2);
    gfx::CameraSnapshot camera=*editor_example::camera(CameraPose{30,20,15,{}},ViewMode::scene).snapshot({800,600});
    CageTool tool;
    Fixture(){region.id=1;tool.select(1,0);}
    CageAction pump(std::initializer_list<input::Event> events={},bool unhandled=true,bool enabled=true) {
        const std::array cages{region.scene_cage()};
        tool.refresh(cages);
        const std::span<const input::Event> raw{events.begin(),events.size()};
        auto action=tool.update(camera,{0,0,800,600},unhandled?raw:std::span<const input::Event>{},raw,enabled);
        if(action.changed&&!action.cancelled)region.points.at(action.point)=action.position;
        return action;
    }
};
}
TEST_CASE("Scene cages reuse XYZ handles with immediate feedback and captured release", "[editor][ui][region]") {
    Fixture f;f.pump();REQUIRE(f.tool.handle("X"));const auto handle=*f.tool.handle("X");
    CHECK_FALSE(f.pump({{.kind=input::EventKind::pointer_down,.position=handle}},false).began);
    const auto original=f.region.points;
    REQUIRE(f.pump({{.kind=input::EventKind::pointer_down,.position=handle}}).began);
    CHECK(f.tool.dragging());
    const Vec2 moved{handle.x+35,handle.y};
    auto action=f.pump({{.kind=input::EventKind::pointer_move,.position=moved}},false);
    CHECK(action.changed);CHECK(action.object==1);CHECK(action.point==0);
    CHECK(f.region.points[0].x!=original[0].x);
    CHECK(f.region.points[0].y==original[0].y);CHECK(f.region.points[0].z==original[0].z);
    for(unsigned i=1;i<original.size();++i)CHECK(f.region.points[i]==original[i]);
    action=f.pump({{.kind=input::EventKind::pointer_up,.position=moved}},false);
    CHECK(action.finished);CHECK_FALSE(action.cancelled);CHECK_FALSE(f.tool.dragging());
    REQUIRE(validate(f.region));
    ui::DrawList list;f.tool.append(list,{});CHECK_FALSE(list.commands.empty());
}
TEST_CASE("Scene cage capture cancels rather than committing on context changes", "[editor][ui][region]") {
    for(unsigned mode=0;mode<3;++mode) {
        Fixture f;f.pump();const auto p=*f.tool.handle("X");
        REQUIRE(f.pump({{.kind=input::EventKind::pointer_down,.position=p}}).began);
        CageAction action;
        if(mode==0)action=f.pump({{.kind=input::EventKind::key_down,.key=input::Key::escape}},false);
        if(mode==1)action=f.pump({{.kind=input::EventKind::focus_lost}},false);
        if(mode==2)action=f.pump({},false,false);
        CHECK(action.cancelled);CHECK(action.finished);CHECK_FALSE(f.tool.dragging());
    }
}
TEST_CASE("Region component transforms stay captured across camera navigation", "[editor][ui][gizmo-navigation]") {
    Fixture f;f.pump();const auto p=*f.tool.handle("X");
    REQUIRE(f.pump({{.kind=input::EventKind::pointer_down,.position=p}}).began);
    const auto before=f.pump({{.kind=input::EventKind::pointer_move,.position={p.x+20,p.y}}});
    REQUIRE(before.changed);
    f.camera.view_projection[0][0]+=.01F;
    auto action=f.pump();CHECK_FALSE(action.cancelled);CHECK(f.tool.dragging());
    action=f.pump({{.kind=input::EventKind::pointer_move,.position={p.x+20,p.y}}});
    CHECK_FALSE(action.changed);
}
TEST_CASE("Convex cages clip their edges when the camera is inside the region", "[editor][ui][region]") {
    Fixture f;f.region=make_region(RegionShape::box,{},20);f.region.id=1;f.pump();
    ui::DrawList list;f.tool.append(list,{});
    for(const auto& command:list.commands)if(const auto* triangle=std::get_if<ui::TriangleDraw>(&command))
        for(auto p:triangle->points){CHECK(std::isfinite(p.x));CHECK(std::isfinite(p.y));}
}
TEST_CASE("Scene cage component selection exposes edge and face pivots", "[editor][ui][region]") {
    using Kind=editor::CageElement;
    Fixture f;f.pump();
    const std::array<u32,1> first{0};
    f.tool.select_elements(Kind::edge,first);
    CHECK(f.tool.vertices().size()==2);REQUIRE(f.tool.pivot());
    const auto edges=f.region.edges();
    for(unsigned c=0;c<3;++c)CHECK((*f.tool.pivot())[c]==(f.region.points[edges[0][0]][c]+f.region.points[edges[0][1]][c])*.5F);
    f.tool.select_elements(Kind::face,first);CHECK(f.tool.vertices().size()==4);
    f.tool.select_all();CHECK(f.tool.elements().size()==6);CHECK(f.tool.vertices().size()==8);
    f.tool.mode(Kind::vertex);f.tool.select_all();CHECK(f.tool.elements().size()==8);
}
TEST_CASE("Scene cage shift click keeps ordered vertex selection", "[editor][ui][region]") {
    Fixture f;f.pump();
    const auto p=f.tool.point_handle(1,1);REQUIRE(p);
    auto click=input::Event{.kind=input::EventKind::pointer_down,.position=*p};click.modifiers.shift=true;
    f.pump({click});REQUIRE(f.tool.elements().size()==2);
    CHECK(f.tool.elements()[0]==0);CHECK(f.tool.elements()[1]==1);
    f.pump({click});REQUIRE(f.tool.elements().size()==1);CHECK(f.tool.elements()[0]==0);
}
TEST_CASE("Whole-instance cage selection does not install a competing component gizmo", "[editor][ui][region]") {
    Fixture f;f.tool.components(false);f.pump();
    CHECK_FALSE(f.tool.handle("X"));
    auto point=f.tool.element_handle(editor::CageElement::edge,0);REQUIRE(point);
    f.pump({{.kind=input::EventKind::pointer_down,.position=*point}});
    REQUIRE(f.tool.picked_object());CHECK(*f.tool.picked_object()==1);
    CHECK_FALSE(f.tool.dragging());CHECK(f.tool.vertices().empty());
    f.tool.components(true);f.tool.select(1,2);f.pump();
    REQUIRE(f.tool.handle("X"));CHECK(f.tool.vertices()==std::vector<u32>{2});
}
TEST_CASE("Scene cage edge and face modes support viewport picking", "[editor][ui][region]") {
    using Kind=editor::CageElement;
    for(auto kind:{Kind::edge,Kind::face}) {
        Fixture f;f.pump();f.tool.mode(kind);f.pump();
        const auto point=f.tool.element_handle(kind,0);REQUIRE(point);
        auto click=input::Event{.kind=input::EventKind::pointer_down,.position=*point};
        click.modifiers.shift=true; // selection should win over overlapping gizmos
        f.pump({click});CHECK(f.tool.mode()==kind);REQUIRE(f.tool.elements().size()==1);
        CHECK(f.tool.vertices().size()==(kind==Kind::edge?2:4));
        REQUIRE(f.tool.handle("X"));
    }
}
TEST_CASE("Cage snapshots update sparsely and survive input-only frames without resupply", "[editor][ui][region][precision]") {
    Fixture f;
    auto sibling = make_region(RegionShape::tetrahedron, {4,0,0}, 1); sibling.id = 2;
    f.tool.refresh(std::array{f.region.scene_cage(), sibling.scene_cage()});
    const auto tick = [&] { (void)f.tool.update(f.camera, {0,0,800,600}, {}, {}, true); };
    tick();
    const auto sibling_handle = f.tool.point_handle(2,0); REQUIRE(sibling_handle);
    const auto original = f.tool.point_handle(1,0); REQUIRE(original);
    const auto before_revision = f.tool.selection_revision();
    auto position = f.region.points[0]; position.x += .7F;
    f.tool.refresh_points(1, std::array{editor::ScenePoint{0,position,{}}});
    tick();
    CHECK(f.tool.pivot() == position);
    CHECK(f.tool.point_handle(1,0) != original);
    CHECK(f.tool.point_handle(2,0) == sibling_handle);
    CHECK(f.tool.selection_revision() == before_revision);
    for (unsigned i = 0; i < 20; ++i) tick();
    CHECK(f.tool.pivot() == position);
    CHECK(f.tool.point_handle(2,0) == sibling_handle);
    f.tool.hide(); ui::DrawList hidden; f.tool.append(hidden, {}); CHECK(hidden.commands.empty());
    tick(); CHECK(f.tool.pivot() == position);
    f.tool.erase(2); tick(); CHECK_FALSE(f.tool.point_handle(2,0));
    CHECK(f.tool.pivot() == position);
    f.tool.erase(1); tick(); CHECK(f.tool.selected() == 0); CHECK_FALSE(f.tool.pivot());
}
TEST_CASE("Region component mode survives switching instances and topology undo", "[editor][ui][region][precision]") {
    content::vmesh::Document mesh;
    mesh.vertex_count = 3;
    mesh.vertex_fields = {{"position", {content::vmesh::ScalarType::Float32,3},
        std::vector<f32>{0,0,0, 1,0,0, 0,1,0}}};
    mesh.faces = {{0,1,2}};
    auto editable = editor::EditableMesh::create(std::move(mesh)); REQUIRE(editable);
    State state{.document = {.mesh = std::move(*editable)}};
    const auto a = instantiate(state, BlueprintId::region); REQUIRE(a);
    const auto b = instantiate(state, BlueprintId::region); REQUIRE(b);
    EditingSession editing{std::move(state)}; editing.select_keyframe(editing.state().viewport.time);
    const auto font = text::Font::load(VNG_TEST_FONT_PATH); REQUIRE(font);
    ui::Screen screen{ui::dark_theme(*font)};
    RegionEditor regions{screen.column(), screen.column(), screen.column(), screen.column()};
    const auto camera = *editor_example::camera(CameraPose{30,20,15,{}}, ViewMode::scene).snapshot({800,600});
    const auto tick = [&] {
        if (auto changes = editing.take_changes()) regions.changed(changes->changes);
        regions.update(editing, camera, {0,0,800,600}, {}, {}, true, true);
    };
    for (const auto mode : {GizmoMode::region_edges, GizmoMode::region_faces}) {
        const auto kind = mode == GizmoMode::region_edges ? editor::CageElement::edge : editor::CageElement::face;
        regions.selection(*a, mode); tick();
        regions.selection(*b, mode); tick(); CHECK(regions.tool().mode() == kind);
        auto boundary = region_world_snapshot(editing.state(), *b); REQUIRE(boundary);
        REQUIRE(edit_region_geometry(*boundary, RegionAction::subdivide, editor::CageElement::face, std::array<u32,1>{0}));
        REQUIRE(editing.begin_region(*b)); REQUIRE(editing.region(*boundary)); REQUIRE(editing.commit());
        tick(); CHECK(regions.tool().mode() == kind);
        REQUIRE(editing.undo()); tick(); CHECK(regions.tool().mode() == kind);
        regions.cancel(); tick();
        CHECK(regions.tool().mode() == kind); CHECK(regions.tool().point_handle(*b,0).has_value());
    }
}

TEST_CASE("Region vertices box-select without becoming scene instances and transform as a group", "[editor][ui][region][components]") {
    Fixture f;f.pump();
    f.pump({{.kind=input::EventKind::pointer_down,.position={10,10}},
            {.kind=input::EventKind::pointer_move,.position={790,590}},
            {.kind=input::EventKind::pointer_up,.position={790,590}}});
    CHECK_FALSE(f.tool.selecting());CHECK(f.tool.selected()==1);
    REQUIRE(f.tool.vertices().size()==8);
    const std::array input{input::Event{.kind=input::EventKind::key_down,.position={500,300},.key=input::Key::g}};
    auto action=f.tool.update(f.camera,{0,0,800,600},input,input,true);
    REQUIRE(action.began);CHECK(f.tool.dragging());
    const std::array motion{vng::input::Event{.kind=input::EventKind::pointer_move,.position={540,300}}};
    action=f.tool.update(f.camera,{0,0,800,600},{},motion,true);
    REQUIRE(action.changed);REQUIRE(action.points.size()==8);
    for(unsigned i=0;i<8;++i)CHECK(action.points[i].id==i);
    const std::array cancel{vng::input::Event{.kind=input::EventKind::pointer_down,.button=1}};
    action=f.tool.update(f.camera,{0,0,800,600},{},cancel,true);
    REQUIRE(action.cancelled);CHECK_FALSE(f.tool.dragging());
    f.pump({{.kind=input::EventKind::pointer_down,.position={10,10}},
            {.kind=input::EventKind::pointer_up,.position={10,10}}});
    CHECK(f.tool.selected()==1);CHECK(f.tool.vertices().empty());
    CHECK_FALSE(f.pump({{.kind=input::EventKind::key_down,.position={500,300},.key=input::Key::g}}).began);
}
