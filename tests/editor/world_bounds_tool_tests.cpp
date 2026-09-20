#include "../../examples/editor/world_bounds_tool.hpp"
#include "../../examples/editor/project.hpp"
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace vng;
using namespace editor_example;
struct Fixture {
    WorldBounds bounds{{-2,-2,-2},{2,2,2}};
    gfx::CameraSnapshot camera = *editor_example::camera(CameraPose{30,20,15,{}},ViewMode::scene).snapshot({800,600});
    WorldBoundsTool tool;
    BoundsAction pump(std::initializer_list<input::Event> events = {}, bool available = true, bool editable = true) {
        std::span<const input::Event> raw{events.begin(), events.size()};
        auto action = tool.update(bounds,camera,{0,0,800,600},available ? raw : std::span<const input::Event>{},raw,true,editable);
        if (!action.cancelled) bounds = action.value;
        return action;
    }
};
}
TEST_CASE("World cuboid has six draggable faces and keeps the opposite face fixed", "[editor][ui][bounds]") {
    for (unsigned face=0; face<6; ++face) {
        Fixture f; f.pump();
        REQUIRE(f.tool.handle(face)); const auto p = *f.tool.handle(face);
        const auto original = f.bounds;
        auto action = f.pump({{.kind=input::EventKind::pointer_down,.position=p}});
        REQUIRE(action.began);
        action = f.pump({{.kind=input::EventKind::pointer_move,.position={p.x+25,p.y+35}}},false);
        CHECK(action.changed); CHECK(f.tool.dragging());
        for (unsigned axis=0;axis<3;++axis) {
            if (axis != face/2 || face%2) CHECK(f.bounds.minimum[axis] == original.minimum[axis]);
            if (axis != face/2 || !(face%2)) CHECK(f.bounds.maximum[axis] == original.maximum[axis]);
        }
        action = f.pump({{.kind=input::EventKind::pointer_up,.position={p.x+50,p.y+70}}},false);
        CHECK(action.finished); CHECK_FALSE(action.cancelled); CHECK_FALSE(f.tool.dragging());
        CHECK(valid_world_bounds(f.bounds));
        ui::DrawList list; f.tool.append(list);
        CHECK(list.commands.size() == 30); // twelve 2-triangle lines and six squares
    }
}
TEST_CASE("Bounds capture respects consumed input and cancels on escape focus resize and disabled editing", "[editor][ui][bounds]") {
    for (unsigned cancellation=0;cancellation<4;++cancellation) {
        Fixture f; f.pump(); const auto p = *f.tool.handle(1);
        CHECK_FALSE(f.pump({{.kind=input::EventKind::pointer_down,.position=p}},false).began);
        REQUIRE(f.pump({{.kind=input::EventKind::pointer_down,.position=p}}).began);
        BoundsAction result;
        if (cancellation==0) result=f.pump({{.kind=input::EventKind::key_down,.key=input::Key::escape}});
        if (cancellation==1) result=f.pump({{.kind=input::EventKind::focus_lost}});
        if (cancellation==2) {
            result=f.tool.update(f.bounds,f.camera,{0,0,640,480},{},{},true,true);
        }
        if (cancellation==3) result=f.pump({},false,false);
        CHECK(result.cancelled); CHECK_FALSE(f.tool.dragging()); CHECK(f.tool.handledPointer());
    }
}
TEST_CASE("Cuboid clipping and extreme drags remain finite and ordered", "[editor][ui][bounds]") {
    Fixture f; f.pump(); const auto p=*f.tool.handle(1);
    REQUIRE(f.pump({{.kind=input::EventKind::pointer_down,.position=p}}).began);
    f.pump({{.kind=input::EventKind::pointer_move,.position={-1e9F,-1e9F}}});
    CHECK(valid_world_bounds(f.bounds));
    f.tool.cancel(); f.bounds={{-10,-10,-10},{10,10,10}};
    f.camera=*editor_example::camera(CameraPose{30,20,1,{}},ViewMode::scene).snapshot({800,600});
    f.pump(); ui::DrawList list; f.tool.append(list);
    CHECK(list.commands.size() <= 30);
    for (const auto& command:list.commands) if (const auto* triangle=std::get_if<ui::TriangleDraw>(&command))
        for (const auto point:triangle->points) { CHECK(std::isfinite(point.x)); CHECK(std::isfinite(point.y)); }
}
