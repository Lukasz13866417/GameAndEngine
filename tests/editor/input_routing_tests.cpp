#include "../../examples/editor/gizmo_handle_selection.hpp"
#include "../../examples/editor/navigation.hpp"
#include <vng/input/routing.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace vng;
using namespace editor_example;
}

TEST_CASE("Input availability identifies occurrences rather than matching event contents",
          "[editor][input-routing]") {
    std::array events{
        input::Event{.kind=input::EventKind::pointer_down,.position={40,60}},
        input::Event{.kind=input::EventKind::pointer_down,.position={40,60}}};
    input::EventSequence sequence;sequence.identify(events);
    REQUIRE(events[0].routing_id!=0);REQUIRE(events[1].routing_id!=events[0].routing_id);
    input::AvailableEvents available{std::span<const input::Event>{events}.subspan(1)};
    CHECK_FALSE(available.contains(events[0]));CHECK(available.contains(events[1]));
    // A presentation surface can convert coordinates without inventing a new
    // occurrence or resurrecting an otherwise consumed lookalike.
    auto local=events[1];local.position={4,6};
    CHECK(available.contains(local));
    available.reset(std::span<const input::Event>{&local,1});
    CHECK(available.contains(events[1]));CHECK_FALSE(available.contains(events[0]));
    available.include(events[0]);CHECK(available.contains(events[0]));
    const auto previous=events[1].routing_id;sequence.identify(events);
    CHECK(events[0].routing_id>previous);CHECK_FALSE(available.contains(events[0]));
}

TEST_CASE("Standalone zero-ID input falls back to complete event equality",
          "[editor][input-routing]") {
    const input::Event source{.kind=input::EventKind::key_down,.position={30,50},.key=input::Key::left};
    input::AvailableEvents available{std::span<const input::Event>{&source,1}};
    CHECK(available.contains(source));
    auto other=source;other.modifiers.control=true;CHECK_FALSE(available.contains(other));
    other=source;other.repeat=true;CHECK_FALSE(available.contains(other));
    other=source;other.position.x+=1;CHECK_FALSE(available.contains(other));
    other=source;other.routing_id=12;CHECK_FALSE(available.contains(other));
}

TEST_CASE("Consumed duplicate clicks do not clear a selected gizmo handle",
          "[editor][input-routing][gizmo]") {
    std::array events{
        input::Event{.kind=input::EventKind::pointer_down,.position={30,30}},
        input::Event{.kind=input::EventKind::pointer_up,.position={30,30}},
        input::Event{.kind=input::EventKind::pointer_down,.position={30,30}},
        input::Event{.kind=input::EventKind::pointer_up,.position={30,30}}};
    input::EventSequence sequence;sequence.identify(events);
    input::AvailableEvents available{std::span<const input::Event>{events}.subspan(2)};
    GizmoHandleSelection selected;selected.select(2);
    const ui::Rect viewport{0,0,100,100};
    CHECK_FALSE(selected.update(events[0],available,viewport));
    CHECK_FALSE(selected.update(events[1],available,viewport));CHECK(selected.axis()==2);
    CHECK_FALSE(selected.update(events[2],available,viewport));
    CHECK(selected.update(events[3],available,viewport));CHECK_FALSE(selected.axis());
}

TEST_CASE("Camera navigation routes only the unconsumed scroll occurrence",
          "[editor][input-routing][navigation]") {
    std::array events{
        input::Event{.kind=input::EventKind::scroll,.position={100,100},.scroll={0,1}},
        input::Event{.kind=input::EventKind::scroll,.position={100,100},.scroll={0,1}}};
    input::EventSequence sequence;sequence.identify(events);
    auto pose=CameraPose{30,20,15,{}};auto expected=pose;
    bool smooth{},expected_smooth{};
    NavigationTool tool,reference;
    const auto remaining=std::span<const input::Event>{events}.subspan(1);
    (void)tool.update(pose,ViewMode::scene,smooth,{0,0},{800,600},remaining,events,true);
    (void)reference.update(expected,ViewMode::scene,expected_smooth,{0,0},{800,600},remaining,remaining,true);
    CHECK(pose==expected);CHECK(smooth==expected_smooth);CHECK(tool.handledPointer());
}
