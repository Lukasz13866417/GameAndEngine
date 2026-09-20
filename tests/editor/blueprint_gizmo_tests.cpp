#include "../../examples/editor/blueprint_gizmos.hpp"
#include "../../examples/editor/selection.hpp"
#include "../../examples/editor/effects.hpp"
#include "../../examples/editor/animation.hpp"
#include "../../examples/editor/translation_tool.hpp"
#include "../../examples/editor/editing_session.hpp"
#include "../../examples/editor/gizmo_selector.hpp"
#include "../../examples/editor/viewport_interaction.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <cmath>

namespace {
using namespace vng;
using namespace editor_example;
constexpr ui::Rect viewport{100, 100, 640, 480};
gfx::CameraSnapshot orthographic() {
    gfx::Camera camera;
    camera.set_position({0,0,10}).look_at({0,0,0}).set_orthographic({.vertical_height=10});
    return *camera.snapshot({640,480});
}
Vec2 project(Vec3 p, const gfx::CameraSnapshot& camera) {
    Vec4 clip{};
    for (std::size_t row=0; row<4; ++row) {
        clip[row]=camera.view_projection[3][row];
        for (std::size_t c=0; c<3; ++c) clip[row]+=camera.view_projection[c][row]*p[c];
    }
    return {viewport.x+(clip.x/clip.w+1)*.5F*viewport.width,
            viewport.y+(1-clip.y/clip.w)*.5F*viewport.height};
}
Vec3 drag(TranslationTool& tool, const editor::Schema& schema, const gfx::CameraSnapshot& camera,
          Vec3 expected, bool reverse=false) {
    (void)tool.update(schema,camera,viewport,{},{},true);
    const auto handle=tool.handle("Forward / back",reverse);
    REQUIRE(handle);
    const auto origin=std::get<Vec3>(schema.controls[0].fields[0].value);
    const auto a=project(origin,camera), b=project(expected,camera);
    const std::array events{
        input::Event{.kind=input::EventKind::pointer_down,.position=*handle},
        input::Event{.kind=input::EventKind::pointer_up,.position={handle->x+b.x-a.x,handle->y+b.y-a.y}}};
    const auto event=tool.update(schema,camera,viewport,events,events,true);
    REQUIRE(event);
    CHECK(event->control=="position");
    const auto result=std::get<Vec3>(event->values[0].value);
    for (std::size_t i=0;i<3;++i) CHECK(std::abs(result[i]-expected[i])<1e-4F);
    return result;
}
State oriented_mesh() {
    content::vmesh::Document mesh;
    mesh.metadata={{"coordinates/forward","-Z"},{"coordinates/up","+Y"}};
    mesh.vertex_count=3;
    mesh.vertex_fields={{"position",{content::vmesh::ScalarType::Float32,3},std::vector<f32>{-1,0,0,1,0,0,0,1,0}}};
    mesh.faces={{0,1,2}};
    State state{.document={.mesh=*editor::EditableMesh::create(std::move(mesh))}};
    state.viewport.selected_object=1;
    instance_transform(state,1)->position={};
    return state;
}

TEST_CASE("Blueprint attitude uses declared forward and up and selection exposes only common tools", "[editor][gizmo][blueprint][attitude]") {
    auto state=oriented_mesh();
    auto axes=blueprint_attitude_axes(state,BlueprintId::mesh); REQUIRE(axes);
    CHECK(*axes==std::array<Vec3,3>{{{0,1,0},{1,0,0},{0,0,-1}}});
    const std::vector generic{GizmoMode::move,GizmoMode::rotate,GizmoMode::scale,GizmoMode::free_rotate};
    auto ship=generic; ship.push_back(GizmoMode::forward); ship.push_back(GizmoMode::attitude);
    const std::array<u32,1> one{1};
    CHECK(selection_gizmos(state,one)==ship);
    const auto second=instantiate(state,BlueprintId::mesh); REQUIRE(second);
    const std::array<u32,2> ships{1,*second}, mixed{1,2};
    CHECK(selection_gizmos(state,ships)==ship);
    CHECK(selection_gizmos(state,mixed)==generic);
    CHECK(selection_gizmos(state,{}).empty());
    auto schema=local_position_gizmo(state,1);
    filter_translation_gizmos(schema,selection_gizmos(state,mixed));
    CHECK(schema.controls.front().translation_axes.empty());
    const auto original=state.document.mesh.document();
    auto document=original;
    document.metadata["coordinates/forward"]="+X"; document.metadata["coordinates/up"]="+Z";
    state.document.mesh=*editor::EditableMesh::create(document);
    REQUIRE(blueprint_attitude_axes(state,BlueprintId::mesh));
    CHECK(*blueprint_attitude_axes(state,BlueprintId::mesh)==std::array<Vec3,3>{{{0,0,1},{0,-1,0},{1,0,0}}});
    for(auto metadata:std::vector<decltype(document.metadata)>{
        {{"coordinates/forward","-Z"}}, {{"coordinates/forward","-Z"},{"coordinates/up","+Z"}},
        {{"coordinates/forward","-Z"},{"coordinates/up","wrong"}},
        {{"coordinates/forward","-Z"},{"coordinates/up","+Y"},{"editor/gizmos/attitude","off"}}}) {
        document.metadata=std::move(metadata); state.document.mesh=*editor::EditableMesh::create(document);
        CHECK_FALSE(blueprint_attitude_axes(state,BlueprintId::mesh));
        const auto common=selection_gizmos(state,one);
        CHECK(std::ranges::find(common,GizmoMode::attitude)==common.end());
    }
}

TEST_CASE("Gizmo selector retains compatible modes and removes incompatible custom choices", "[editor][ui][gizmo][attitude]") {
    auto state=oriented_mesh();
    auto font=text::Font::load(VNG_TEST_FONT_PATH); REQUIRE(font);
    ui::Screen screen{ui::dark_theme(*font)};
    GizmoSelector selector{screen.column().width(300).height(40)};
    const std::array<u32,1> ship{1}; const std::array<u32,2> mixed{1,2};
    selector.show(state,ship); selector.value(GizmoMode::attitude);
    CHECK(selector.value()==GizmoMode::attitude);
    selector.show(state,ship); CHECK(selector.value()==GizmoMode::attitude);
    selector.show(state,mixed); CHECK(selector.value()==GizmoMode::move);
    CHECK(selector.common().size()==4);
    selector.value(GizmoMode::forward); CHECK(selector.value()==GizmoMode::move);
    selector.show(state,ship); selector.value(GizmoMode::forward); CHECK(selector.value()==GizmoMode::forward);
    selector.show(state,{}); CHECK(selector.common().empty());
    selector.show(state,ship); CHECK(selector.value()==GizmoMode::move);
    REQUIRE(selector.cycle(-1));CHECK(selector.value()==GizmoMode::attitude);
    REQUIRE(selector.cycle(1));CHECK(selector.value()==GizmoMode::move);
    for(auto expected:{GizmoMode::rotate,GizmoMode::scale,GizmoMode::free_rotate,GizmoMode::forward,GizmoMode::attitude,GizmoMode::move}) {
        REQUIRE(selector.cycle(1));CHECK(selector.value()==expected);
    }
    selector.show(state,mixed);REQUIRE(selector.cycle(-1));CHECK(selector.value()==GizmoMode::free_rotate);
    selector.show(state,{});CHECK_FALSE(selector.cycle(1));
}

TEST_CASE("Each listed gizmo describes the shortcut used for capability-filtered input", "[editor][ui][gizmo]") {
    for(const auto& description:gizmo_descriptions) {
        const std::array available{description.mode};
        input::Event event{.kind=input::EventKind::key_down,.key=description.key};
        if(description.key==input::Key::unknown)CHECK_FALSE(gizmo_shortcut(event,available));
        else CHECK(gizmo_shortcut(event,available)==description.mode);
        CHECK_FALSE(gizmo_shortcut(event,{}));
        CHECK(gizmo_choice_label(description.mode).ends_with("("+std::string(description.shortcut)+")"));
        event.modifiers.control=true;CHECK_FALSE(gizmo_shortcut(event,available));
        event.modifiers={};event.repeat=true;CHECK_FALSE(gizmo_shortcut(event,available));
        CHECK(std::ranges::count(gizmo_descriptions,description.key,&GizmoDescription::key)==1);
    }
}
TEST_CASE("MMB rotation moves a mixed selection around its shared center with one undo", "[editor][ui][rotation][free-rotate]") {
    auto state=oriented_mesh();
    const auto original=state.document.instances;
    const auto geometry=state.document.mesh.document();
    const std::array<u32,2> selected{1,2};
    const auto center=selection_center(instance_centers(state,1,selected));
    EditingSession editing{state};editing.select_keyframe(0);
    RotationInteraction tool{editing};const auto camera=orthographic();
    const auto update=[&](std::initializer_list<input::Event> events) {
        const std::span<const input::Event> input{events.begin(),events.size()};
        return tool.update(1,camera,viewport,input,input,true,selected,false,{},true);
    };
    auto began=update({{.kind=input::EventKind::pointer_down,.position={350,300},.button=2}});
    REQUIRE(began);REQUIRE(began->began);
    auto moved=update({{.kind=input::EventKind::pointer_move,.position={470,380}}});
    REQUIRE(moved);REQUIRE(moved->changed);
    const auto after=selection_center(instance_centers(editing.state(),1,selected));
    for(unsigned c=0;c<3;++c)CHECK(std::abs(center[c]-after[c])<.00001F);
    CHECK(editing.state().document.instances!=original);
    CHECK(editing.state().document.mesh.document()==geometry);
    REQUIRE(update({{.kind=input::EventKind::pointer_up,.position={470,380},.button=2}}));
    REQUIRE_FALSE(tool.active());REQUIRE(editing.undo());
    CHECK(editing.state().document.instances==original);CHECK_FALSE(editing.can_undo());
}
}

TEST_CASE("Blueprint translation axes follow sampled rotation but not translation or scale", "[editor][gizmo][blueprint]") {
    auto state=oriented_mesh();
    const auto original=state.document.mesh.document();
    const auto direction=[&] {
        const auto schema=local_position_gizmo(state,7);
        REQUIRE(schema.controls.size()==1);
        REQUIRE(schema.controls[0].translation_axes.size()==1);
        return schema.controls[0].translation_axes[0].direction;
    };
    CHECK(direction()==Vec3{0,0,-1});
    instance_transform(state,1)->rotation={0,90,0};
    instance_transform(state,1)->position={5,7,4};
    instance_transform(state,1)->scale=2;
    CHECK(std::abs(direction().x+1)<1e-5F);
    CHECK(std::abs(direction().z)<1e-5F);
    REQUIRE(key_property(state,{1,"rotation"},0,Vec3{0,0,0}));
    REQUIRE(key_property(state,{1,"rotation"},4,Vec3{0,90,0}));
    state.viewport.time=2;
    CHECK(std::abs(direction().x+std::sqrt(.5F))<1e-5F);
    CHECK(std::abs(direction().z+std::sqrt(.5F))<1e-5F);
    ProjectControls controls{state};
    editor::Inspector inspector{1,7,state.document.revision};
    controls.describe_editor(inspector);
    const auto found=std::ranges::find(inspector.schema().controls,std::string("position"),&editor::Control::key);
    REQUIRE(found!=inspector.schema().controls.end());
    CHECK(found->translation_axes==local_position_gizmo(state,7).controls[0].translation_axes);
    state.viewport.time=4; // Old worker descriptor must refresh orientation locally.
    const auto refreshed=selection_gizmo_schema(state,inspector.schema());
    const auto updated=std::ranges::find(refreshed.controls,std::string("position"),&editor::Control::key);
    CHECK(updated->translation_axes==local_position_gizmo(state,7).controls[0].translation_axes);
    REQUIRE(encode(state));
    const auto restored=decode(*encode(state));
    REQUIRE(restored);
    CHECK(local_position_gizmo(*restored,7)==local_position_gizmo(state,7));
    CHECK(state.document.mesh.document()==original);
    state.viewport.mode=ViewMode::mesh;
    CHECK(local_position_gizmo(state,7).controls.empty());
    state.viewport.mode=ViewMode::scene;
    state.viewport.selected_object=2;
    CHECK(local_position_gizmo(state,7).controls[0].translation_axes.empty());
}

TEST_CASE("Blueprint manipulation describes common tools and instance-local authoring surfaces", "[editor][gizmo][blueprint]") {
    auto state = oriented_mesh();
    const auto ship = blueprint_manipulation(state, BlueprintId::mesh);
    CHECK(ship.surface == ManipulationSurface::object);
    REQUIRE(ship.forward);
    REQUIRE(ship.attitude);
    const auto boundary = blueprint_manipulation(state, BlueprintId::region);
    CHECK(boundary.surface == ManipulationSurface::boundary);
    CHECK_FALSE(boundary.forward);
    CHECK_FALSE(boundary.attitude);
    REQUIRE(boundary.gizmos.size() == 7);
    auto a = instantiate(state, BlueprintId::region), b = instantiate(state, BlueprintId::region);
    REQUIRE(a); REQUIRE(b);
    CHECK(selection_gizmos(state, std::array{*a, *b}) == boundary.gizmos);
    CHECK(selection_gizmos(state, std::array{1U, *a}) ==
          std::vector{GizmoMode::move, GizmoMode::rotate, GizmoMode::scale, GizmoMode::free_rotate});
}

TEST_CASE("Viewport owns gesture priority completion and rollback", "[editor][viewport][interaction]") {
    auto loaded = text::Font::load(VNG_TEST_FONT_PATH); REQUIRE(loaded);
    ui::Screen screen{ui::dark_theme(*loaded)};
    EditingSession editing{oriented_mesh()}; editing.select_keyframe(editing.state().viewport.time);
    ViewportInteraction tools{editing, screen.column(), screen.column(), screen.column(), screen.column()};
    tools.begin_frame();
    REQUIRE(tools.accepts(ViewportTool::navigation));
    REQUIRE(editing.begin_move(1));
    REQUIRE(editing.move({2, 3, 4}));
    CHECK(tools.active() == ViewportTool::translation);
    tools.update(ViewportTool::navigation, [](bool available) { CHECK(available); });
    tools.update(ViewportTool::translation, [](bool available) { CHECK(available); });
    auto finished = tools.finish(ViewportTool::translation); REQUIRE(finished); CHECK(*finished);
    CHECK_FALSE(tools.busy());
    CHECK_FALSE(tools.accepts(ViewportTool::selection)); // release cannot click through
    tools.begin_frame();
    CHECK(tools.accepts(ViewportTool::selection));
    REQUIRE(editing.begin_move(1));
    REQUIRE(editing.move({8, 9, 10}));
    auto cancelled = tools.cancel(); REQUIRE(cancelled); CHECK(*cancelled);
    CHECK(instance_transform(editing.state(), 1)->position == Vec3{2, 3, 4});
    CHECK_FALSE(tools.busy());
    tools.selection_box.begin({120, 120}, viewport, {});
    CHECK(tools.active() == ViewportTool::selection);
    CHECK_FALSE(tools.accepts(ViewportTool::boundary));
    CHECK_FALSE(tools.accepts(ViewportTool::bounds));
    REQUIRE(tools.cancel());
    CHECK_FALSE(tools.selection_box.active());
}

TEST_CASE("Viewport arbitration retains same-frame tool consumption and boundary selection routing", "[editor][viewport][interaction]") {
    auto loaded = text::Font::load(VNG_TEST_FONT_PATH); REQUIRE(loaded);
    ui::Screen screen{ui::dark_theme(*loaded)};
    auto state = oriented_mesh();
    auto region = instantiate(state, BlueprintId::region); REQUIRE(region);
    EditingSession editing{std::move(state)}; editing.select_keyframe(editing.state().viewport.time);
    ViewportInteraction tools{editing, screen.column(), screen.column(), screen.column(), screen.column()};
    tools.selected(*region, GizmoMode::region_vertices);
    CHECK(tools.regions.tool().selected() == *region);
    tools.selected(1, GizmoMode::move);
    CHECK(tools.regions.tool().selected() == 0);
    editing.viewport().selected_object = 1;
    const auto schema = local_position_gizmo(editing.state(), 1);
    const auto camera = orthographic();
    tools.begin_frame();
    (void)tools.update(ViewportTool::translation, [&](bool available) {
        return tools.translation.update(schema, camera, viewport, {}, {}, available);
    });
    auto handle = tools.translation.handle("X"); REQUIRE(handle);
    const std::array events{
        input::Event{.kind=input::EventKind::pointer_down, .position=*handle},
        input::Event{.kind=input::EventKind::pointer_up, .position={handle->x + 20, handle->y}}};
    auto moved = tools.update(ViewportTool::translation, [&](bool available) {
        return tools.translation.update(schema, camera, viewport, events, events, available);
    });
    REQUIRE(moved);
    CHECK_FALSE(tools.translation.dragging());
    CHECK_FALSE(tools.accepts(ViewportTool::selection));
    CHECK_FALSE(tools.accepts(ViewportTool::rotation));
    tools.begin_frame();
    CHECK(tools.accepts(ViewportTool::navigation));
}

TEST_CASE("A captured viewport tool allows camera navigation but excludes other editors", "[editor][viewport][interaction]") {
    auto loaded = text::Font::load(VNG_TEST_FONT_PATH); REQUIRE(loaded);
    ui::Screen screen{ui::dark_theme(*loaded)};
    EditingSession editing{oriented_mesh()}; editing.select_keyframe(editing.state().viewport.time);
    ViewportInteraction tools{editing, screen.column(), screen.column(), screen.column(), screen.column()};
    const auto schema = local_position_gizmo(editing.state(), 1);
    const auto camera = orthographic();
    tools.begin_frame();
    (void)tools.update(ViewportTool::translation, [&](bool available) {
        return tools.translation.update(schema, camera, viewport, {}, {}, available);
    });
    const auto handle = tools.translation.handle("X"); REQUIRE(handle);
    const std::array press{input::Event{.kind=input::EventKind::pointer_down, .position=*handle}};
    (void)tools.update(ViewportTool::translation, [&](bool available) {
        return tools.translation.update(schema, camera, viewport, press, press, available);
    });
    REQUIRE(tools.translation.dragging());
    tools.begin_frame();
    tools.update(ViewportTool::navigation, [](bool available) { CHECK(available); });
    tools.update(ViewportTool::boundary, [](bool available) { CHECK_FALSE(available); });
    CHECK_FALSE(tools.accepts(ViewportTool::selection));
    // The release is UI-consumed/outside the viewport, but raw events belong
    // to the existing capture and must still complete the gesture.
    const std::array release{input::Event{.kind=input::EventKind::pointer_up, .position={900, handle->y}}};
    auto moved = tools.update(ViewportTool::translation, [&](bool available) {
        CHECK(available);
        return tools.translation.update(schema, camera, viewport, {}, release, available);
    });
    REQUIRE(moved);
    CHECK_FALSE(tools.translation.dragging());
    CHECK_FALSE(tools.accepts(ViewportTool::selection));
    tools.begin_frame();
    CHECK(tools.accepts(ViewportTool::navigation));
}

TEST_CASE("Blueprint orientation is declarative and optional, not inferred from names", "[editor][gizmo][blueprint]") {
    auto state=oriented_mesh();
    auto mesh=state.document.mesh.document();
    for (const auto& axis : {"+X","-X","+Y","-Y","+Z","-Z"}) {
        mesh.metadata["coordinates/forward"]=axis;
        state.document.mesh=*editor::EditableMesh::create(mesh);
        REQUIRE(local_position_gizmo(state,1).controls[0].translation_axes.size()==1);
    }
    for (const auto& metadata : std::vector<decltype(mesh.metadata)>{
        {}, {{"name","Spaceship"}}, {{"coordinates/forward","invalid"}},
        {{"coordinates/forward","-Z"},{"editor/gizmos/forward","off"}}}) {
        mesh.metadata=metadata;
        state.document.mesh=*editor::EditableMesh::create(mesh);
        CHECK(local_position_gizmo(state,1).controls[0].translation_axes.empty());
    }
}

TEST_CASE("Additional translation handles drag arbitrary axes in either direction", "[editor][ui][gizmo]") {
    editor::Inspector inspector{1,2,3};
    inspector.translation_gizmo("position",Vec3{},[](Vec3){}).axis("Forward / back",{2,2,0});
    TranslationTool tool;
    (void)drag(tool,inspector.schema(),orthographic(),{1,1,0});
    (void)drag(tool,inspector.schema(),orthographic(),{-1,-1,0},true);
    gfx::Camera perspective;
    perspective.set_position({4,3,8}).look_at({0,0,0});
    auto schema=inspector.schema();
    schema.controls[0].translation_axes[0].direction={1,.5F,-1};
    (void)drag(tool,schema,*perspective.snapshot({640,480}),{1,.5F,-1});
    (void)drag(tool,schema,*perspective.snapshot({640,480}),{-1,-.5F,1},true);
    schema.controls[0].translation_axes[0].direction={1,0,0};
    (void)drag(tool,schema,orthographic(),{1,0,0}); // coincident with world X: outer handle still picks
    schema.controls[0].translation_axes[0].direction={0,0,-1};
    (void)tool.update(schema,orthographic(),viewport,{},{},true);
    CHECK_FALSE(tool.handle("Forward / back")); // camera-parallel: no unstable handle
}

TEST_CASE("Custom axis constraints remain straight at bounds and cancel when orientation changes", "[editor][ui][gizmo]") {
    editor::Inspector inspector{1,2,3};
    inspector.translation_gizmo("position",Vec3{},[](Vec3){}).axis("Forward / back",{1,2,0});
    TranslationTool tool;
    const auto camera=orthographic();
    (void)tool.update(inspector.schema(),camera,viewport,{},{},true);
    const auto handle=tool.handle("Forward / back");
    REQUIRE(handle);
    const std::array down{input::Event{.kind=input::EventKind::pointer_down,.position=*handle}};
    (void)tool.update(inspector.schema(),camera,viewport,down,down,true);
    REQUIRE(tool.dragging());
    const std::array up{input::Event{.kind=input::EventKind::pointer_up,.position={handle->x+480000000,handle->y-960000000}}};
    const auto event=tool.update(inspector.schema(),camera,viewport,{},up,true);
    REQUIRE(event);
    const auto result=std::get<Vec3>(event->values[0].value);
    CHECK(std::abs(result.x-scene_coordinate_limit*.5F)<.1F);
    CHECK(std::abs(result.y-scene_coordinate_limit)<.1F);
    (void)tool.update(inspector.schema(),camera,viewport,down,down,true);
    REQUIRE(tool.dragging());
    auto schema=inspector.schema();
    schema.controls[0].translation_axes[0].direction={0,1,0};
    (void)tool.update(schema,camera,viewport,{},{},true);
    CHECK_FALSE(tool.dragging());
    CHECK(tool.handledPointer());
}

TEST_CASE("Forward motion edits only the selected instance position key and supports undo", "[editor][gizmo][blueprint]") {
    auto state=oriented_mesh();
    instance_transform(state,1)->rotation={0,90,45};
    const auto sibling=instantiate(state,BlueprintId::mesh);
    REQUIRE(sibling);
    state.viewport.selected_object=1;
    REQUIRE(key_property(state,{1,"position"},0,Vec3{}));
    REQUIRE(key_property(state,{1,"position"},4,Vec3{}));
    state.viewport.time=2;
    state.document.keyframe_names[2] = "Editable pose";
    const auto original=state.document.mesh.document();
    const auto other=*instance_transform(state,*sibling);
    const auto base=*instance_transform(state,1);
    EditingSession session{std::move(state)}; session.select_keyframe(session.state().viewport.time);
    REQUIRE(session.begin_move(1,{}));
    const auto schema=local_position_gizmo(session.state(),7);
    const auto axis=schema.controls[0].translation_axes[0].direction;
    TranslationTool tool;
    const auto moved=drag(tool,schema,orthographic(),axis);
    REQUIRE(session.move(moved));
    REQUIRE(session.commit());
    const auto changes=session.take_changes(); REQUIRE(changes);
    CHECK_FALSE(changes->changes.full);
    CHECK(changes->changes.properties.size()==1);
    CHECK(changes->changes.properties.contains({1,"position"}));
    const auto& current=session.state();
    CHECK(*instance_transform(current,1)==base);
    CHECK(evaluate_instance(current,*find_instance(current,1),2).transform.position==moved);
    CHECK(*instance_transform(current,*sibling)==other);
    CHECK(current.document.mesh.document()==original);
    REQUIRE(session.undo());
    CHECK(evaluate_instance(current,*find_instance(current,1),2).transform.position==Vec3{});
    REQUIRE(session.redo());
    CHECK(evaluate_instance(current,*find_instance(current,1),2).transform.position==moved);
    CHECK(current.document.mesh.document()==original);
}

TEST_CASE("Ship forward handles cross the old minus-100 wall", "[editor][ui][gizmo][regression]") {
    auto state=oriented_mesh();
    instance_transform(state,1)->position={0,0,-98};
    gfx::Camera camera;
    camera.set_position({180,120,200}).look_at({0,0,-98});
    TranslationTool tool;
    const auto value=drag(tool,local_position_gizmo(state,7),*camera.snapshot({640,480}),{0,0,-150});
    EditingSession session{std::move(state)}; session.select_keyframe(session.state().viewport.time);
    REQUIRE(session.begin_move(1)); REQUIRE(session.move(value)); REQUIRE(session.commit());
    CHECK(instance_transform(session.state(),1)->position.z < -149.F);
    REQUIRE(encode(session.state()));
}
