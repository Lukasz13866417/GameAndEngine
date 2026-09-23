#include "../../examples/editor/viewport_session.hpp"
#include "../../examples/editor/presented_view.hpp"
#include "../../examples/editor/animation.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <limits>

using namespace vng;
namespace project = editor_example;
namespace {
project::State make_state() {
    auto loaded=project::load_scene(VNG_TIMELINE_SCENE_PATH);
    REQUIRE(loaded);
    loaded->document.revision=1;
    loaded->viewport={};
    return std::move(*loaded);
}
}

TEST_CASE("Viewport requests are fixed size, validated, and independent of authored revision", "[editor][viewport]") {
    project::ViewportRequest request{7,{}};
    request.view.sequence=42;
    request.view.editor_camera={17,-10,6,{1,2,3},2.5F};
    request.view.smooth_zoom=true;
    request.view.show_regions=true;request.view.show_world_bounds=true;
    request.view.gizmo_only=true;
    const auto bytes=project::encode_viewport_request(request);
    REQUIRE(bytes); CHECK(bytes->size()==76);
    auto decoded=project::decode_viewport_request(*bytes);
    REQUIRE(decoded); CHECK(decoded->view==request.view);
    CHECK(decoded->required_document_revision==7);
    auto previous=*bytes;previous[7]=2;previous[40]=static_cast<char>(static_cast<unsigned char>(previous[40])&15);
    auto old=project::decode_viewport_request(previous);REQUIRE(old);
    CHECK_FALSE(old->view.show_regions);CHECK_FALSE(old->view.show_world_bounds);
    CHECK_FALSE(old->view.gizmo_only);
    auto version3=*bytes;version3[7]=3;version3[40]=static_cast<char>(static_cast<unsigned char>(version3[40])&63);
    auto old3=project::decode_viewport_request(version3);REQUIRE(old3);CHECK_FALSE(old3->view.gizmo_only);
    for (std::size_t n=0;n<bytes->size();++n) CHECK_FALSE(project::decode_viewport_request(bytes->substr(0,n)));
    auto malformed=*bytes; malformed[40]=char(255);
    CHECK_FALSE(project::decode_viewport_request(malformed));
    request.view.editor_camera.distance=std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(project::encode_viewport_request(request));
}

TEST_CASE("Viewport inbox coalesces targets and waits only for required document data", "[editor][viewport]") {
    auto state=make_state();
    editor::InteractionTrace trace;
    project::ViewportInbox inbox;
    project::ViewportRequest request{3,state.viewport};
    request.view.sequence=2; request.view.editor_camera.distance=5;
    inbox.offer(request);
    CHECK_FALSE(inbox.apply(state,trace));
    request.required_document_revision=1; request.view.sequence=3; request.view.editor_camera.distance=4;
    inbox.offer(request); // Newest replaces even a request waiting for another document.
    REQUIRE(inbox.apply(state,trace));
    CHECK(state.document.revision==1);
    CHECK(state.viewport.sequence==3);
    CHECK(state.viewport.editor_camera.distance==4);
    request.view.sequence=2; request.view.editor_camera.distance=9;
    inbox.offer(request); CHECK_FALSE(inbox.apply(state,trace));
    CHECK(state.viewport.editor_camera.distance==4);
    request.required_document_revision=4; request.view.sequence=4;
    inbox.offer(request); CHECK_FALSE(inbox.apply(state,trace));
    state.document.revision=4;
    REQUIRE(inbox.apply(state,trace));
    CHECK(state.viewport.editor_camera.distance==9);
}

TEST_CASE("Lens and forward motion smooth independently without changing authored values", "[editor][viewport][zoom]") {
    project::CameraPose start{20,10,8,{1,2,3},1};
    auto goal=start; goal.zoom=4; goal.target={3,4,5};
    project::ZoomMotion motion;
    motion.target(start,false); motion.target(goal,true);
    REQUIRE(motion.advance(.016));
    CHECK(motion.pose().zoom>1); CHECK(motion.pose().zoom<4);
    CHECK(motion.pose().target.x>1); CHECK(motion.pose().target.x<3);
    CHECK(motion.pose().distance==8);
    for(unsigned i=0;i<100 && !motion.settled();++i) motion.advance(.016);
    CHECK(motion.settled()); CHECK(motion.pose()==goal);
    CHECK(goal.zoom==4); CHECK(start.zoom==1);
    motion.target(start,false); CHECK(motion.pose()==start);
    start.target={900000,900000,900000}; goal=start; goal.target.x+=10;
    motion.target(start,false); motion.target(goal,true);
    for(unsigned i=0;i<100 && !motion.settled();++i) motion.advance(.016);
    CHECK(motion.settled()); CHECK(motion.pose()==goal);
}

TEST_CASE("Scene roundtrip keeps independent editor and scene camera lens values", "[editor][viewport][zoom]") {
    auto state=make_state();
    state.viewport.editor_camera.zoom=2;
    const auto camera=project::ensure_camera(state,{0,0,8,{},.5F});
    REQUIRE(camera);
    REQUIRE(project::key_camera(state,*camera,0,{0,0,8,{},1}));
    REQUIRE(project::key_camera(state,*camera,5,{0,0,8,{},4}));
    auto text=project::encode(state); REQUIRE(text);
    auto decoded=project::decode(*text); REQUIRE(decoded);
    CHECK(decoded->viewport.editor_camera==state.viewport.editor_camera);
    CHECK(project::evaluate_camera(*decoded,2.5F)->zoom==Catch::Approx(2.5F));
    CHECK(project::evaluate_camera(*decoded,5)==project::evaluate_camera(state,5));
}
TEST_CASE("Wheel smoothing is time based and does not mutate the target", "[editor][viewport]") {
    project::CameraPose start; start.distance=10;
    auto goal=start; goal.distance=5;
    project::ZoomMotion a,b;
    a.target(start,false); b.target(start,false);
    a.target(goal,true); b.target(goal,true);
    a.advance(.016);
    CHECK(a.pose().distance<10); CHECK(a.pose().distance>5); CHECK_FALSE(a.settled());
    a.advance(.084); b.advance(.1);
    CHECK(a.pose().distance==Catch::Approx(b.pose().distance));
    for(int i=0;i<100;++i) {
        const auto previous=a.pose().distance;
        a.advance(.01);
        CHECK(a.pose().distance<=previous); CHECK(a.pose().distance>=5);
    }
    CHECK(a.settled()); CHECK(goal.distance==5);
    goal.yaw=45; goal.distance=7;
    a.target(goal,true); CHECK(a.settled()); // Orbit/direct view changes have no drag lag.
    CHECK(a.pose()==goal);
}

TEST_CASE("Continuous fractional zoom targets move on each rendered frame", "[editor][viewport]") {
    project::CameraPose goal;
    project::ZoomMotion motion;
    motion.target(goal, false);
    for (int i = 0; i < 120; ++i) {
        goal.distance *= static_cast<float>(std::exp(-.15 / 120));
        const auto previous = motion.pose().distance;
        motion.target(goal, true);
        REQUIRE(motion.advance(1.0 / 180));
        CHECK(motion.pose().distance < previous);
        CHECK(motion.pose().distance >= goal.distance);
    }
    for (int i = 0; i < 120; ++i) motion.advance(1.0 / 180);
    CHECK(motion.settled());
    CHECK(motion.pose() == goal);
}

TEST_CASE("Presented camera metadata is independent of the next viewport target", "[editor][viewport]") {
    auto state=make_state();
    editor::preview::FrameInfo info;
    info.has_view=true; info.view_mode=static_cast<u32>(state.viewport.mode);
    info.view_camera={0,0,5,0,0,0,1};
    state.viewport.editor_camera.distance=2;
    REQUIRE(project::matches_view(info,state));
    auto view=project::presented_camera(info).snapshot({800,600});
    REQUIRE(view); CHECK(view->position.z==Catch::Approx(5));
    state.viewport.mode=project::ViewMode::mesh;
    CHECK_FALSE(project::matches_view(info,state));
}

TEST_CASE("Interaction envelopes and bounded samples preserve causality", "[editor][timing]") {
    editor::InteractionOrigin origin{1,10,20,2,30,0,false};
    const auto packet=editor::traced_message("view\nhello",origin);
    auto received=editor::receive_interaction(packet);
    REQUIRE(received); CHECK(received->payload=="view\nhello");
    CHECK(received->origin.id==1); CHECK(received->origin.sent_ns>=30);
    CHECK(editor::receive_interaction("ordinary")->origin.id==0);
    CHECK_FALSE(editor::receive_interaction(packet.substr(0,65)));
    auto corrupt=packet; corrupt[9]=0;
    CHECK_FALSE(editor::receive_interaction(corrupt));
    editor::InteractionTrace trace{{1,10,20,2,30,40,false},50,60,70,80,90,100};
    editor::InteractionTimings timings;
    timings.presented(1,7,2,1,trace,false,110,120);
    REQUIRE(timings.samples().size()==1);
    CHECK(timings.samples().front().settled_submit_ns==0);
    timings.presented(1,7,2,1,trace,true,130,140); // UI redrawing the same pixels isn't a new response.
    CHECK(timings.samples().front().settled_submit_ns==0);
    timings.presented(1,7,2,2,trace,true,150,160);
    CHECK(timings.samples().front().settled_submit_ns==150);
    auto invalid=trace; invalid.worker_received_ns=1;
    timings.presented(1,7,3,3,invalid,true,170,180);
    CHECK(timings.samples().size()==1);
    timings.presented(2,7,2,1,trace,true,190,200);
    CHECK(timings.samples().size()==2); // IDs are scoped by worker generation.
    for(u64 n=2;n<300;++n) {
        trace.origin.id=n;
        timings.presented(2,7,n,n,trace,true,200+n,210+n);
    }
    CHECK(timings.samples().size()==editor::InteractionTimings::capacity);
    CHECK(timings.json().find("\"clock\":\"Linux CLOCK_MONOTONIC")!=std::string::npos);
    timings.enabled(false);
    timings.presented(3,7,1,1,trace,true,1000,1010);
    CHECK(timings.samples().back().generation==2);
    timings.clear(); CHECK(timings.samples().empty());
    timings.enabled(true);
    timings.presented(3,7,2,2,trace,true,1100,1110);
    CHECK(timings.samples().empty()); // In-flight old frames cannot repopulate a cleared capture.
}

TEST_CASE("Timing quantiles remain ordered for small sample counts", "[editor][timing]") {
    editor::InteractionTimings timings;
    editor::InteractionTrace trace{{1,10,20,2,30,40,false},50,60,70,80,90,100};
    timings.presented(1,1,1,1,trace,true,1'000'020,1'000'030);
    trace.origin.id=2;
    timings.presented(1,1,2,2,trace,true,5'000'020,5'000'030);
    CHECK(timings.summary().find("Median: 3.00 ms / p95: 5.00 ms")!=std::string::npos);
}

TEST_CASE("Input tracing is lazy and synthetic event times are explicitly marked", "[editor][timing]") {
    editor::InteractionTimings timings;
    timings.begin_tick({}); CHECK(timings.interaction().id==0);
    std::array<input::Event,2> events{};
    timings.begin_tick(events);
    const auto origin=timings.interaction();
    CHECK(origin.id!=0); CHECK(origin.estimated_input); CHECK(origin.input_count==2);
    CHECK(timings.interaction()==origin);
    timings.enabled(false); timings.begin_tick(events);
    CHECK(timings.interaction().id==0);
}

TEST_CASE("Stale viewport targets cannot roll a newer snapshot backward", "[editor][viewport]") {
    auto state=make_state();
    state.viewport.sequence=10;
    project::ViewportInbox inbox;
    project::ViewportRequest stale{state.document.revision,state.viewport};
    stale.view.sequence=5; stale.view.editor_camera.distance=1;
    inbox.offer(stale);
    editor::InteractionTrace trace;
    CHECK_FALSE(inbox.apply(state,trace));
    CHECK(state.viewport.sequence==10);
    CHECK(state.viewport.editor_camera.distance==8);
}

