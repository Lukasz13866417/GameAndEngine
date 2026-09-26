#include "../../examples/editor/scale_tool.hpp"
#include <catch2/catch_test_macros.hpp>
#include <array>

using namespace vng;
using namespace editor_example;
TEST_CASE("Uniform scale handle captures only available presses and commits raw release", "[editor][ui][scale]") {
    ScaleTool tool;
    gfx::Camera camera; camera.set_position({0,0,8}).look_at({0,0,0});
    const auto snapshot=*camera.snapshot({640,480});
    const auto pump=[&](std::span<const input::Event> events,bool available=true,bool enabled=true) {
        return tool.update({1,1,1},{},1,snapshot,{0,0,640,480},
            available ? events : std::span<const input::Event>{},events,enabled);
    };
    (void)pump({}); REQUIRE(tool.visible());
    const auto handle=tool.handle();
    const std::array down{input::Event{.kind=input::EventKind::pointer_down,.position=handle}};
    CHECK_FALSE(pump(down,false).began); CHECK_FALSE(tool.dragging());
    CHECK(pump(down).began); REQUIRE(tool.dragging());
    const std::array move{input::Event{.kind=input::EventKind::pointer_move,.position={handle.x+40,handle.y-20}}};
    const auto preview=pump(move,false); CHECK(preview.changed); CHECK(preview.value>1.F); CHECK(tool.dragging());
    const std::array release{input::Event{.kind=input::EventKind::pointer_up,.position={handle.x+80,handle.y-20}}};
    const auto committed=pump(release,false); CHECK(committed.finished); CHECK_FALSE(committed.cancelled);
    CHECK(committed.value>preview.value); CHECK_FALSE(tool.dragging()); CHECK(tool.handledPointer());
    (void)pump({});
    const std::array another{input::Event{.kind=input::EventKind::pointer_down,.position=tool.handle()}};
    REQUIRE(pump(another).began);
    const std::array escape{input::Event{.kind=input::EventKind::key_down,.key=input::Key::escape}};
    CHECK(pump(escape).cancelled); CHECK_FALSE(tool.dragging());
    (void)pump({}); REQUIRE(pump(another).began); CHECK(pump({},true,false).cancelled);
}

TEST_CASE("Scale handles stay selected and normalized arrows scale the object", "[editor][ui][scale][gizmo-focus]") {
    ScaleTool tool;
    gfx::Camera camera;camera.set_position({0,0,8}).look_at({});
    const auto snapshot=*camera.snapshot({640,480});
    const auto pump=[&](std::span<const input::Event> events,bool available=true,float step=1.F) {
        return tool.update({1,1,1},{},1,snapshot,{0,0,640,480},
            available?events:std::span<const input::Event>{},events,true,{},step);
    };
    pump({});
    const auto p=tool.handle();
    const std::array click{input::Event{.kind=input::EventKind::pointer_down,.position=p},
        input::Event{.kind=input::EventKind::pointer_up,.position=p}};
    REQUIRE(pump(click).finished);REQUIRE(tool.selected());CHECK_FALSE(tool.dragging());
    const std::array right{input::Event{.kind=input::EventKind::key_down,.position={100,100},.key=input::Key::right}};
    const auto scaled=pump(right,true,2.F);CHECK(scaled.began);CHECK(scaled.changed);CHECK(scaled.value>1.05F);
    pump(click,false);CHECK(tool.selected());CHECK(tool.dragging());
    CHECK_FALSE(pump(std::span{click}.first(1)).finished);CHECK(tool.selected());
    CHECK(pump(std::span{click}.last(1)).finished);CHECK_FALSE(tool.selected());CHECK_FALSE(tool.dragging());
}
