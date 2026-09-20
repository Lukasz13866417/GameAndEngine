#include "../../examples/editor/navigation.hpp"
#include "../../examples/editor/camera_walk.hpp"
#include "../../examples/editor/ui_scale.hpp"
#include "../../examples/editor/scroll_trace.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <limits>
#include <sstream>

namespace {
using namespace vng;
using namespace editor_example;
using Kind = input::EventKind;
constexpr Vec2 origin{100, 50}, size{800, 600};
input::Event event(Kind kind, Vec2 position = {500, 350}, input::Modifiers mods = {}) {
    return {.kind = kind, .position = position, .button = 2, .modifiers = mods};
}
input::Event wheel(f32 amount, Vec2 position = {500, 350}) {
    return {.kind = Kind::scroll, .position = position, .scroll = {0, amount}};
}
bool update(NavigationTool& tool, State& state, std::initializer_list<input::Event> events) {
    const std::span<const input::Event> input{events.begin(), events.size()};
    return tool.update(state, origin, size, input, input);
}
State scene() {
    content::vmesh::Document document;
    document.vertex_count = 3;
    document.vertex_fields = {{"position",
                               {content::vmesh::ScalarType::Float32, 3},
                               std::vector<f32>{0, 0, 0, 1, 0, 0, 0, 1, 0}}};
    document.faces = {{0, 1, 2}};
    auto mesh = editor::EditableMesh::create(std::move(document));
    REQUIRE(mesh);
    return {.document = {.mesh = std::move(*mesh)}};
}
} // namespace

TEST_CASE("Camera drags have independent configurable sensitivities", "[editor][navigation]") {
    for (auto modifiers : {input::Modifiers{}, input::Modifiers{.shift=true}, input::Modifiers{.control=true}}) {
        auto a=scene(), b=a;
        NavigationTool base, faster; faster.speeds({2,2,.6F});
        REQUIRE(update(base,a,{event(Kind::pointer_down,{500,350},modifiers),event(Kind::pointer_up,{520,360})}));
        REQUIRE(update(faster,b,{event(Kind::pointer_down,{500,350},modifiers),event(Kind::pointer_up,{520,360})}));
        const auto before=scene().viewport.editor_camera;
        if (modifiers.control || modifiers.shift) {
            for (unsigned i=0;i<3;++i) CHECK(b.viewport.editor_camera.target[i]-before.target[i]==
                Catch::Approx(2*(a.viewport.editor_camera.target[i]-before.target[i])).margin(.00001));
        } else {
            CHECK(b.viewport.editor_camera.yaw-before.yaw==Catch::Approx(2*(a.viewport.editor_camera.yaw-before.yaw)));
            CHECK(b.viewport.editor_camera.pitch-before.pitch==Catch::Approx(2*(a.viewport.editor_camera.pitch-before.pitch)));
        }
        CHECK(a.viewport.editor_camera.zoom==before.zoom);
        CHECK(b.viewport.editor_camera.zoom==before.zoom);
    }
}
TEST_CASE("Left Alt boosts pan dolly and optical zoom but not orbit or Right Alt", "[editor][navigation][fast-pan]") {
    for(const auto mode:{NavigationTool::ScrollMode::move_forward,NavigationTool::ScrollMode::zoom}) {
        for(const auto mods:{input::Modifiers{.shift=true},input::Modifiers{.control=true},input::Modifiers{}}) {
            auto a=scene(),b=a,c=a;const auto before=a.viewport.editor_camera;
            NavigationTool normal,fast,right;normal.scroll_mode(mode);fast.scroll_mode(mode);right.scroll_mode(mode);
            auto boosted=mods;boosted.alt=boosted.left_alt=true;
            auto altgr=mods;altgr.alt=true;
            REQUIRE(update(normal,a,{event(Kind::pointer_down,{500,350},mods),event(Kind::pointer_up,{510,355},mods)}));
            REQUIRE(update(fast,b,{event(Kind::pointer_down,{500,350},boosted),event(Kind::pointer_up,{510,355},boosted)}));
            REQUIRE(update(right,c,{event(Kind::pointer_down,{500,350},altgr),event(Kind::pointer_up,{510,355},altgr)}));
            CHECK(c.viewport.editor_camera==a.viewport.editor_camera);
            if(mods.control&&mode==NavigationTool::ScrollMode::zoom)
                CHECK(std::log(b.viewport.editor_camera.zoom/before.zoom)==Catch::Approx(4*std::log(a.viewport.editor_camera.zoom/before.zoom)).margin(.00001));
            else if(mods.control||mods.shift)for(unsigned i=0;i<3;++i)
                CHECK(b.viewport.editor_camera.target[i]-before.target[i]==Catch::Approx(4*(a.viewport.editor_camera.target[i]-before.target[i])).margin(.00001));
            else CHECK(b.viewport.editor_camera==a.viewport.editor_camera);
        }
        auto a=scene(),b=a;NavigationTool normal,fast;normal.scroll_mode(mode);fast.scroll_mode(mode);
        auto scroll=wheel(1);auto boosted=scroll;boosted.modifiers.left_alt=true;
        REQUIRE(update(normal,a,{scroll,scroll,scroll,scroll}));REQUIRE(update(fast,b,{boosted}));
        CHECK(b.viewport.editor_camera.zoom==Catch::Approx(a.viewport.editor_camera.zoom));
        for(unsigned c=0;c<3;++c)CHECK(b.viewport.editor_camera.target[c]==Catch::Approx(a.viewport.editor_camera.target[c]).margin(.00001));
    }
}
TEST_CASE("Free-rotate owns bare MMB without disabling modified camera navigation", "[editor][navigation][free-rotate]") {
    auto s=scene();const auto original=s.viewport.editor_camera;
    NavigationTool tool;tool.orbit_enabled(false);
    CHECK_FALSE(update(tool,s,{event(Kind::pointer_down),event(Kind::pointer_up,{540,390})}));
    CHECK_FALSE(tool.dragging());CHECK_FALSE(tool.handledPointer());CHECK(s.viewport.editor_camera==original);
    for(auto mods:{input::Modifiers{.shift=true},input::Modifiers{.control=true},input::Modifiers{.alt=true}}) {
        s.viewport.editor_camera=original;
        CHECK(update(tool,s,{event(Kind::pointer_down,{500,350},mods),event(Kind::pointer_up,{540,390})}));
        CHECK_FALSE(tool.dragging());
    }
    tool.orbit_enabled(true);
    CHECK(update(tool,s,{event(Kind::pointer_down),event(Kind::pointer_up,{540,390})}));
}
TEST_CASE("Mesh-centered drags use the object reference without reframing the camera", "[editor][navigation][mesh-origin]") {
    auto s=scene();s.viewport.mode=ViewMode::mesh;
    auto& pose=s.viewport.editor_camera;pose={0,0,8,{3,2,4},2};
    const auto before=pose;
    const Vec3 center{1,1,0};
    NavigationTool tool;tool.drag_origin(center);
    CHECK_FALSE(update(tool,s,{}));CHECK(pose==before);
    const auto eye=camera(pose,ViewMode::mesh).position();
    REQUIRE(update(tool,s,{event(Kind::pointer_down,{500,350},{.control=true}),event(Kind::pointer_up,{500,330})}));
    const auto moved_eye=camera(pose,ViewMode::mesh).position();
    for(unsigned i=0;i<3;++i)
        CHECK(moved_eye[i]-center[i]==Catch::Approx((eye[i]-center[i])*std::exp(-.2)).margin(.00001));
    CHECK(pose.yaw==before.yaw);CHECK(pose.pitch==before.pitch);
    CHECK(pose.distance==before.distance);CHECK(pose.zoom==before.zoom);
    // Panning uses the mesh plane's depth, not the unrelated orbit distance.
    pose=before;
    REQUIRE(update(tool,s,{event(Kind::pointer_down,{500,350},{.shift=true}),event(Kind::pointer_up,{520,360})}));
    const auto scale=2*(eye.z-center.z)*std::tan(camera_vertical_fov*std::numbers::pi/360)/(before.zoom*size.y);
    CHECK(pose.target.x==Catch::Approx(before.target.x-20*scale));
    CHECK(pose.target.y==Catch::Approx(before.target.y+10*scale));
    CHECK(pose.target.z==before.target.z);
    CHECK(s.document.revision==1);
    // Turning off the reference restores ordinary camera-forward dragging.
    pose=before;tool.drag_origin({});
    REQUIRE(update(tool,s,{event(Kind::pointer_down,{500,350},{.control=true}),event(Kind::pointer_up,{500,330})}));
    CHECK(pose.target.x==before.target.x);CHECK(pose.target.y==before.target.y);
}
TEST_CASE("Navigation freezes the mesh reference per gesture and leaves wheel alone", "[editor][navigation][mesh-origin]") {
    auto s=scene();s.viewport.mode=ViewMode::mesh;s.viewport.editor_camera={0,0,8,{3,2,4},1};
    const auto before=s;
    NavigationTool tool;tool.drag_origin(Vec3{});
    update(tool,s,{event(Kind::pointer_down,{500,350},{.control=true})});
    tool.drag_origin(Vec3{20,10,0});
    update(tool,s,{event(Kind::pointer_move,{500,340}),event(Kind::pointer_up,{500,330})});
    auto once=before;NavigationTool single;single.drag_origin(Vec3{});
    update(single,once,{event(Kind::pointer_down,{500,350},{.control=true}),event(Kind::pointer_up,{500,330})});
    for(unsigned i=0;i<3;++i)CHECK(s.viewport.editor_camera.target[i]==Catch::Approx(once.viewport.editor_camera.target[i]));
    s=before;once=before;NavigationTool regular;
    update(tool,s,{wheel(1)});update(regular,once,{wheel(1)});
    CHECK(s.viewport.editor_camera==once.viewport.editor_camera);
}
TEST_CASE("Mesh-centered orbit preserves the mesh view position and eye radius after panning", "[editor][navigation][mesh-origin]") {
    const Vec3 center{1,-2,0};
    const auto view_position=[&](const CameraPose& pose) {
        const auto snapshot=camera(pose,ViewMode::mesh).snapshot({800,600});REQUIRE(snapshot);
        const auto& m=snapshot->view;
        Vec3 point{};
        for(unsigned c=0;c<3;++c)point[c]=m[0][c]*center.x+m[1][c]*center.y+m[2][c]*center.z+m[3][c];
        return point;
    };
    const auto radius=[&](const CameraPose& pose) {
        const auto eye=camera(pose,ViewMode::mesh).position();
        double sum{};for(unsigned c=0;c<3;++c)sum+=std::pow(double(eye[c])-center[c],2);
        return std::sqrt(sum);
    };
    for(const auto pose:{CameraPose{0,0,8,{3,2,4},2},CameraPose{179,79,8,{3,2,4},1}}) {
        auto s=scene();s.viewport.mode=ViewMode::mesh;s.viewport.editor_camera=pose;
        NavigationTool tool;tool.drag_origin(center);
        CHECK_FALSE(update(tool,s,{event(Kind::pointer_down)}));
        CHECK(s.viewport.editor_camera==pose);
        // A stationary release must not numerically reframe the view either.
        CHECK_FALSE(update(tool,s,{event(Kind::pointer_up)}));
        const auto before=view_position(pose);
        REQUIRE(update(tool,s,{event(Kind::pointer_down),event(Kind::pointer_up,{440,450})}));
        const auto after=view_position(s.viewport.editor_camera);
        for(unsigned c=0;c<3;++c)CHECK(after[c]==Catch::Approx(before[c]).margin(.00002));
        CHECK(radius(s.viewport.editor_camera)==Catch::Approx(radius(pose)));
        CHECK(s.viewport.editor_camera.zoom==pose.zoom);
        CHECK(s.viewport.editor_camera.distance==pose.distance);
        CHECK(s.viewport.editor_camera.target!=pose.target);
        auto split=scene();split.viewport.mode=ViewMode::mesh;split.viewport.editor_camera=pose;
        NavigationTool split_tool;split_tool.drag_origin(center);
        update(split_tool,split,{event(Kind::pointer_down),event(Kind::pointer_move,{470,400}),event(Kind::pointer_up,{440,450})});
        for(unsigned c=0;c<3;++c)CHECK(split.viewport.editor_camera.target[c]==Catch::Approx(s.viewport.editor_camera.target[c]).margin(.00001));
    }
}

TEST_CASE("UI scaling transforms coordinates but preserves native pixels and wheel units", "[editor][ui-scale]") {
    input::Frame frame{.logical_size={1600,1000},.framebuffer={3200,2000},.pointer={500,250}};
    frame.events={{.kind=Kind::scroll,.position={500,250},.scroll={0,.25F}}};
    scale_ui_input(frame,125);
    CHECK(frame.logical_size.x==Catch::Approx(1280/editor_ui_density));
    CHECK(frame.logical_size.y==Catch::Approx(800/editor_ui_density));
    CHECK(frame.pointer.x==Catch::Approx(400/editor_ui_density));
    CHECK(frame.pointer.y==Catch::Approx(200/editor_ui_density));
    CHECK(frame.events[0].position==frame.pointer); CHECK(frame.events[0].scroll.y==.25F);
    CHECK(frame.framebuffer==Extent2D{3200,2000});
}

TEST_CASE("A minimized surface retains layout without retaining focus", "[editor][ui-scale]") {
    UiSurfaceSize size;
    input::Frame frame{.logical_size={960,720},.framebuffer={1200,900}};
    size.retain(frame);
    frame.logical_size={}; frame.framebuffer={};
    size.retain(frame);
    CHECK(frame.logical_size==Vec2{960,720}); CHECK(frame.framebuffer==Extent2D{1200,900});
    CHECK_FALSE(frame.focused);
}

TEST_CASE("Walk uses the camera axes and separate world vertical speed", "[editor][navigation][walk]") {
    CameraWalk walk; walk.active(true);
    CameraPose pose{90,0,8,{},2};
    input::Frame frame;
    frame.events={{.kind=Kind::key_down,.key=input::Key::w}};
    REQUIRE(walk.update(pose,.1,frame,{},true));
    CHECK(pose.target.x==Catch::Approx(-1)); CHECK(pose.target.z==Catch::Approx(0).margin(.00001));
    CHECK(pose.zoom==2); CHECK(pose.distance==8);
    frame.events.clear(); REQUIRE(walk.update(pose,.1,frame,{},true));
    CHECK(pose.target.x==Catch::Approx(-2));
    frame.events={{.kind=Kind::key_up,.key=input::Key::w},{.kind=Kind::key_down,.key=input::Key::d}};
    REQUIRE(walk.update(pose,.1,frame,{},true)); CHECK(pose.target.z==Catch::Approx(-1));
    frame.events={{.kind=Kind::key_up,.key=input::Key::d},{.kind=Kind::key_down,.key=input::Key::e}};
    REQUIRE(walk.update(pose,.1,frame,{10,10,20,4},true)); CHECK(pose.target.y==Catch::Approx(2));
    frame.events={{.kind=Kind::key_up,.key=input::Key::e},{.kind=Kind::key_down,.key=input::Key::q,.modifiers={.shift=true}}};
    REQUIRE(walk.update(pose,.1,frame,{10,10,20,4},true)); CHECK(pose.target.y==Catch::Approx(-6));
}

TEST_CASE("Walk is time-based and releases input on typing pause or focus loss", "[editor][navigation][walk]") {
    CameraWalk a,b; a.active(true); b.active(true);
    CameraPose first{30,20,8},second=first;
    input::Frame frame; frame.events={{.kind=Kind::key_down,.key=input::Key::w}, {.kind=Kind::key_down,.key=input::Key::d}};
    REQUIRE(a.update(first,.01,frame,{},true)); REQUIRE(b.update(second,.1,frame,{},true));
    frame.events.clear(); for(int i=1;i<10;++i) REQUIRE(a.update(first,.01,frame,{},true));
    for(unsigned i=0;i<3;++i) CHECK(first.target[i]==Catch::Approx(second.target[i]).margin(.00001));
    CHECK(std::hypot(first.target.x,first.target.y,first.target.z)==Catch::Approx(1));
    const auto before=first;
    CHECK_FALSE(a.update(first,.1,frame,{},false)); CHECK_FALSE(a.moving());
    CHECK_FALSE(a.update(first,.1,frame,{},true)); CHECK(first==before);
    frame.events={{.kind=Kind::key_down,.key=input::Key::s,.modifiers={.control=true}}};
    CHECK_FALSE(a.update(first,.1,frame,{},true)); // Ctrl+S remains Save.
    frame.events={{.kind=Kind::key_down,.key=input::Key::w}};
    REQUIRE(a.update(first,20,frame,{},true)); // stalls advance by at most 100ms
    for(unsigned i=0;i<3;++i) CHECK(std::abs(first.target[i]-before.target[i])<=1.001F);
    frame.focused=false; CHECK_FALSE(a.update(first,.1,frame,{},true)); CHECK_FALSE(a.active());
    a.active(true); frame.focused=true;
    frame.events={{.kind=Kind::key_down,.key=input::Key::escape}};
    CHECK_FALSE(a.update(first,.1,frame,{},true)); CHECK_FALSE(a.active());
}

TEST_CASE("Explicit viewing distance controls clipping independently of orbit distance", "[editor][navigation][viewing-distance]") {
    CameraPose pose{0,0,8};
    auto close=camera(pose,ViewMode::scene,20).snapshot({800,600});
    auto distant=camera(pose,ViewMode::scene,500).snapshot({800,600});
    REQUIRE(close); REQUIRE(distant);
    const auto depth=[](const auto& view,float z) {
        const auto& m=view.view_projection;
        return (m[2][2]*z+m[3][2])/(m[2][3]*z+m[3][3]);
    };
    CHECK(depth(*close,-100)>1); CHECK(depth(*distant,-100)<1);
    CHECK(close->position==distant->position);
}

TEST_CASE("Walk look rotates the camera without orbiting its position", "[editor][navigation][walk]") {
    auto state=scene(); NavigationTool tool; tool.look_in_place(true);
    const auto eye=camera(state).position();
    const auto before=state.viewport.editor_camera;
    REQUIRE(update(tool,state,{event(Kind::pointer_down),event(Kind::pointer_up,{540,370})}));
    const auto now=camera(state).position();
    for(unsigned i=0;i<3;++i) CHECK(now[i]==Catch::Approx(eye[i]).margin(.00001));
    CHECK(state.viewport.editor_camera.yaw!=before.yaw);
    CHECK(state.viewport.editor_camera.pitch!=before.pitch);
    CHECK(state.viewport.editor_camera.distance==before.distance);
    CHECK(state.viewport.editor_camera.zoom==before.zoom);
}

TEST_CASE("Wheel trace separates UI rejection from changed targets and presented frames", "[editor][navigation][trace]") {
    std::ostringstream output;
    ScrollTrace trace{&output};
    const std::array events{wheel(1)};
    const vng::ui::Rect bounds{origin.x, origin.y, size.x, size.y};
    trace.input(events, {}, bounds, false, {0,0,8}, {0,0,8}, 1);
    CHECK(output.str().find("reason=consumed-by-ui") != std::string::npos);
    trace.input(events, events, bounds, false, {0,0,8}, {0,0,8}, 1, {{"unfocused", true}});
    CHECK(output.str().find("block=unfocused") != std::string::npos);
    trace.input(events, events, bounds, false, {0,0,8}, {0,0,7}, 2);
    trace.submitted(1, 2, true);
    CHECK(output.str().find("reason=target-updated") != std::string::npos);
    CHECK(output.str().find("[scroll/transport] generation=1 view=2 queued") != std::string::npos);

    editor::preview::FrameInfo frame;
    frame.generation = 1; frame.frame_id = 1; frame.view_sequence = 1;
    frame.has_view = true; frame.settled = false; frame.view_camera[2] = 7.8F;
    const auto before = output.str();
    trace.presented(frame);
    CHECK(output.str() == before); // Old pixels must not be reported as a response.
    frame.view_sequence = 2;
    trace.presented(frame);
    CHECK(output.str().find("distance=7.8 settled=0") != std::string::npos);
    const auto first = output.str();
    trace.presented(frame);
    CHECK(output.str() == first); // UI re-presenting the same image is not progress.
    frame.frame_id = 2; frame.settled = true; frame.view_camera[2] = 7;
    trace.presented(frame);
    CHECK(output.str().find("distance=7 settled=1") != std::string::npos);
    const auto settled = output.str();
    frame.frame_id = 3;
    trace.presented(frame);
    CHECK(output.str() == settled); // Stop logging after this input has settled.
    ScrollTrace disabled;
    CHECK_FALSE(disabled.enabled());
    disabled.input(events, events, bounds, false, {0,0,8}, {0,0,7}, 2);
    disabled.presented(frame);
}

TEST_CASE("Viewport MMB orbits about a persistent pivot without editing the scene",
          "[editor][navigation]") {
    auto state = scene();
    state.viewport.editor_camera.target = {2, 3, 4};
    const auto document = state.document.mesh.document();
    const auto revision = state.document.revision;
    NavigationTool tool;
    CHECK_FALSE(update(tool, state, {event(Kind::pointer_down)}));
    CHECK(tool.dragging());
    CHECK(tool.handledPointer());
    REQUIRE(update(tool, state, {event(Kind::pointer_move, {520, 360})}));
    CHECK(state.viewport.editor_camera.yaw == Catch::Approx(9));
    CHECK(state.viewport.editor_camera.pitch == Catch::Approx(15));
    CHECK(state.viewport.editor_camera.target == Vec3{2, 3, 4});
    CHECK(state.viewport.editor_camera.distance == 8.0F);
    CHECK(state.document.revision == revision);
    CHECK(state.document.mesh.document() == document);
    const auto snapshot = camera(state).snapshot({800, 600});
    REQUIRE(snapshot);
    for (std::size_t i = 0; i < 3; ++i)
        CHECK(snapshot->position[i] + snapshot->forward[i] * state.viewport.editor_camera.distance ==
              Catch::Approx(state.viewport.editor_camera.target[i]).margin(.00001));
    REQUIRE(update(tool, state, {event(Kind::pointer_up, {530, 370})}));
    CHECK_FALSE(tool.dragging());
    CHECK(state.viewport.editor_camera.yaw == Catch::Approx(6));
    CHECK(state.viewport.editor_camera.pitch == Catch::Approx(18));
}

TEST_CASE("Viewport Shift MMB pans in the camera plane at pivot depth", "[editor][navigation]") {
    auto state = scene();
    state.viewport.editor_camera.yaw = state.viewport.editor_camera.pitch = 0;
    NavigationTool tool;
    REQUIRE(update(tool, state,
                   {event(Kind::pointer_down, {500, 350}, {.shift = true}),
                    event(Kind::pointer_move, {560, 380})}));
    const auto scale = 2 * 8 * std::tan(camera_vertical_fov * std::numbers::pi / 360) / 600;
    CHECK(state.viewport.editor_camera.target.x == Catch::Approx(-60 * scale));
    CHECK(state.viewport.editor_camera.target.y == Catch::Approx(30 * scale));
    CHECK(state.viewport.editor_camera.target.z == 0.0F);
    CHECK(state.viewport.editor_camera.yaw == 0.0F);
    CHECK(state.viewport.editor_camera.pitch == 0.0F);
    CHECK(state.viewport.editor_camera.distance == 8.0F);
    // Mode is chosen at press; modifier changes during capture do not switch it.
    CHECK(update(tool, state, {event(Kind::pointer_up, {570, 380}, {.control = true})}));
    CHECK(state.viewport.editor_camera.target.x == Catch::Approx(-70 * scale));
    CHECK(state.viewport.editor_camera.distance == 8.0F);

    state = scene();
    state.viewport.mode = ViewMode::mesh;
    state.viewport.editor_camera.yaw = state.viewport.editor_camera.pitch = 0;
    REQUIRE(update(tool, state,
                   {event(Kind::pointer_down, {500, 350}, {.shift = true}),
                    event(Kind::pointer_up, {560, 380})}));
    CHECK(state.viewport.editor_camera.target.x == Catch::Approx(-60 * scale * .52));
    CHECK(state.viewport.editor_camera.target.y == Catch::Approx(30 * scale * .52));
}

TEST_CASE("Pilot mode routes navigation into the animation camera without losing the editor view",
          "[editor][navigation][camera]") {
    auto state = scene();
    state.viewport.editor_camera = {-20, 5, 3, {1, 2, 3}};
    state.document.animation_camera = {70, 20, 12, {-3, 1, 2}};
    const auto original_animation = state.document.animation_camera;
    NavigationTool tool;
    REQUIRE(update(tool, state, {wheel(1)}));
    CHECK(state.document.animation_camera == original_animation);
    const auto editor_pose = state.viewport.editor_camera;
    state.viewport.pilot_camera = true;
    REQUIRE(&view_camera(state) == &state.document.animation_camera);
    REQUIRE(update(tool, state,
        {event(Kind::pointer_down), event(Kind::pointer_up, {530, 360})}));
    CHECK(state.viewport.editor_camera == editor_pose);
    CHECK(state.document.animation_camera != original_animation);
    CHECK(state.document.animation_camera.yaw == Catch::Approx(61));
    const auto authored_snapshot = camera(state).snapshot({800, 600});
    REQUIRE(authored_snapshot);
    state.viewport.pilot_camera = false;
    REQUIRE(&view_camera(state) == &state.viewport.editor_camera);
    const auto editor_snapshot = camera(state).snapshot({800, 600});
    REQUIRE(editor_snapshot);
    CHECK(editor_snapshot->position != authored_snapshot->position);
    CHECK(state.viewport.editor_camera == editor_pose);
}

TEST_CASE("Pan follows rotated camera axes and logical viewport size", "[editor][navigation]") {
    auto state = scene();
    state.viewport.editor_camera.yaw = 90;
    state.viewport.editor_camera.pitch = 30;
    auto snapshot = camera(state).snapshot({800, 600});
    REQUIRE(snapshot);
    const auto scale =
        2 * state.viewport.editor_camera.distance * std::tan(camera_vertical_fov * std::numbers::pi / 360) / 600;
    NavigationTool tool;
    const std::array events{event(Kind::pointer_down, {500, 350}, {.shift = true}),
                            event(Kind::pointer_up, {520, 360})};
    REQUIRE(tool.update(state, origin, size, events, events));
    for (std::size_t i = 0; i < 3; ++i)
        CHECK(state.viewport.editor_camera.target[i] ==
              Catch::Approx((-20 * snapshot->right[i] + 10 * snapshot->up[i]) * scale)
                  .margin(.000001));

    auto double_size = scene();
    double_size.viewport.editor_camera.yaw = 90;
    double_size.viewport.editor_camera.pitch = 30;
    REQUIRE(tool.update(double_size, origin, {1600, 1200}, events, events));
    for (std::size_t i = 0; i < 3; ++i)
        CHECK(double_size.viewport.editor_camera.target[i] == Catch::Approx(state.viewport.editor_camera.target[i] * .5F));
}

TEST_CASE("Wheel and Ctrl MMB zoom the lens without moving the camera", "[editor][navigation]") {
    auto state = scene();
    NavigationTool tool;
    tool.scroll_mode(NavigationTool::ScrollMode::zoom);
    REQUIRE(update(tool, state, {wheel(1)}));
    CHECK(state.viewport.editor_camera.zoom == Catch::Approx(std::exp(.15)));
    REQUIRE(update(tool, state, {wheel(-1)}));
    CHECK(state.viewport.editor_camera.zoom == Catch::Approx(1));
    REQUIRE(update(tool, state,
                   {event(Kind::pointer_down, {500, 350}, {.control = true}),
                    event(Kind::pointer_up, {500, 300})}));
    CHECK(state.viewport.editor_camera.zoom == Catch::Approx(std::exp(.5)));
    CHECK(state.viewport.editor_camera.distance == 8);
    CHECK(state.viewport.editor_camera.target == Vec3{});
    CHECK_FALSE(tool.dragging());
    REQUIRE(update(tool, state, {wheel(100000)}));
    CHECK(state.viewport.editor_camera.zoom == camera_max_zoom);
    REQUIRE(update(tool, state, {wheel(-100000)}));
    CHECK(state.viewport.editor_camera.zoom == camera_min_zoom);
    REQUIRE(camera(state).snapshot({800,600}));
}

TEST_CASE("Fractional wheel input updates immediately and sums across UI ticks", "[editor][navigation]") {
    auto state = scene();
    const auto revision = state.document.revision;
    NavigationTool tool;
    tool.scroll_mode(NavigationTool::ScrollMode::zoom);
    for (int i = 1; i <= 120; ++i) {
        const auto before = state.viewport.editor_camera.zoom;
        REQUIRE(update(tool, state, {wheel(1.0F / 120)}));
        CHECK(state.viewport.editor_camera.zoom > before);
        CHECK(state.viewport.editor_camera.zoom == Catch::Approx(std::exp(.15 * i / 120)).margin(.00001));
    }
    CHECK(state.document.revision == revision);
    REQUIRE(update(tool, state, {wheel(-.0001F)})); // No application-level wheel dead zone.
    CHECK(state.viewport.smooth_zoom);
}

TEST_CASE("Forward scroll translates eye and pivot along view direction without changing zoom", "[editor][navigation][zoom]") {
    auto state = scene(); NavigationTool tool;
    state.viewport.editor_camera.zoom=2;
    const auto before=state.viewport.editor_camera;
    const auto first=camera(state).snapshot({800,600}); REQUIRE(first);
    tool.scroll_mode(NavigationTool::ScrollMode::move_forward);
    REQUIRE(update(tool,state,{wheel(1)}));
    const auto after=camera(state).snapshot({800,600}); REQUIRE(after);
    for(unsigned i=0;i<3;++i) {
        CHECK(after->position[i]-first->position[i] == Catch::Approx(first->forward[i]*8*.15).margin(.00001));
        CHECK(state.viewport.editor_camera.target[i] == Catch::Approx(first->forward[i]*8*.15).margin(.00001));
    }
    CHECK(state.viewport.editor_camera.zoom==before.zoom);
    CHECK(state.viewport.editor_camera.distance==before.distance);
    REQUIRE(update(tool,state,{wheel(-1)}));
    for(unsigned i=0;i<3;++i) CHECK(state.viewport.editor_camera.target[i]==Catch::Approx(0).margin(.00001));
    REQUIRE(update(tool,state,{event(Kind::pointer_down,{500,350},{.control=true}),event(Kind::pointer_up,{500,300})}));
    for(unsigned i=0;i<3;++i) CHECK(state.viewport.editor_camera.target[i]==Catch::Approx(first->forward[i]*4).margin(.00001));
    const auto translated=state.viewport.editor_camera;
    tool.scroll_mode(NavigationTool::ScrollMode::zoom);
    REQUIRE(update(tool,state,{wheel(1)}));
    CHECK(state.viewport.editor_camera.target==translated.target);
    CHECK(state.viewport.editor_camera.zoom>translated.zoom);
}

TEST_CASE("Navigation captures raw movement and final release across UI panels",
          "[editor][navigation]") {
    auto state = scene();
    NavigationTool tool;
    const std::array begin{event(Kind::pointer_down)};
    CHECK_FALSE(tool.update(state, origin, size, {}, begin)); // UI consumed the press.
    CHECK_FALSE(tool.dragging());
    CHECK_FALSE(tool.handledPointer());
    CHECK_FALSE(tool.update(state, origin, size, begin, begin));
    REQUIRE(tool.dragging());
    const std::array outside{event(Kind::pointer_move, {1200, 700}),
                             event(Kind::pointer_up, {1210, 710})};
    REQUIRE(tool.update(state, origin, size, {}, outside));
    CHECK(state.viewport.editor_camera.yaw == Catch::Approx(162));
    CHECK(state.viewport.editor_camera.pitch == camera_max_pitch);
    CHECK_FALSE(tool.dragging());
    CHECK(tool.handledPointer());
    const auto old_yaw = state.viewport.editor_camera.yaw;
    CHECK_FALSE(update(tool, state, {event(Kind::pointer_move, {500, 350})}));
    CHECK(state.viewport.editor_camera.yaw == old_yaw);
}

TEST_CASE("Optical zoom changes projection only and pan speed follows the lens", "[editor][navigation][zoom]") {
    auto a=scene(), b=scene();
    b.viewport.editor_camera.zoom=4;
    const auto wide=camera(a).snapshot({800,600}), tele=camera(b).snapshot({800,600});
    REQUIRE(wide); REQUIRE(tele);
    CHECK(wide->position==tele->position);
    CHECK(wide->forward==tele->forward);
    CHECK(tele->projection[0][0]==Catch::Approx(wide->projection[0][0]*4));
    NavigationTool first,second;
    REQUIRE(update(first,a,{event(Kind::pointer_down,{500,350},{.shift=true}),event(Kind::pointer_up,{520,360})}));
    REQUIRE(update(second,b,{event(Kind::pointer_down,{500,350},{.shift=true}),event(Kind::pointer_up,{520,360})}));
    for (unsigned i=0;i<3;++i) CHECK(b.viewport.editor_camera.target[i]==Catch::Approx(a.viewport.editor_camera.target[i]/4));
}

TEST_CASE("Forward motion retains fractional steps and stays finite at navigation limits", "[editor][navigation][zoom]") {
    auto state=scene(); NavigationTool tool;
    state.viewport.editor_camera={0,0,8,{},3};
    tool.scroll_mode(NavigationTool::ScrollMode::move_forward);
    for(int i=0;i<120;++i) REQUIRE(update(tool,state,{wheel(1.F/120)}));
    CHECK(state.viewport.editor_camera.target.z==Catch::Approx(-1.2F));
    CHECK(state.viewport.editor_camera.zoom==3);
    state.viewport.editor_camera.target.z=-camera_target_limit;
    CHECK_FALSE(update(tool,state,{wheel(100000)}));
    CHECK(valid_camera_pose(state.viewport.editor_camera));
    REQUIRE(update(tool,state,{wheel(-1)}));
}

TEST_CASE("Navigation only begins on unhandled viewport middle button or wheel",
          "[editor][navigation]") {
    auto state = scene();
    NavigationTool tool;
    CHECK_FALSE(update(tool, state, {event(Kind::pointer_down, {99, 50}), wheel(1, {900, 650})}));
    CHECK_FALSE(tool.dragging());
    CHECK_FALSE(tool.handledPointer());
    auto left = event(Kind::pointer_down);
    left.button = 0;
    CHECK_FALSE(update(tool, state, {left, event(Kind::pointer_move, {530, 350})}));
    CHECK_FALSE(tool.dragging());
    const std::array scroll{wheel(1)};
    CHECK_FALSE(tool.update(state, origin, size, {}, scroll));
    CHECK(state.viewport.editor_camera.distance == 8.0F);
    // Support tools whose unhandled input is already the original stream.
    CHECK(tool.update(state, origin, size, scroll, {}));
}

TEST_CASE("Navigation releases capture on focus loss escape disabling or resized viewport",
          "[editor][navigation]") {
    auto state = scene();
    NavigationTool tool;
    for (const auto kind : {Kind::focus_lost, Kind::key_down}) {
        update(tool, state, {event(Kind::pointer_down)});
        REQUIRE(tool.dragging());
        auto cancel = event(kind);
        cancel.key = input::Key::escape;
        const std::array raw{cancel, event(Kind::pointer_move, {600, 450})};
        CHECK_FALSE(tool.update(state, origin, size, {}, raw));
        CHECK_FALSE(tool.dragging());
        CHECK(tool.handledPointer());
        CHECK(tool.cancelled());
    }
    update(tool, state, {event(Kind::pointer_down)});
    CHECK_FALSE(tool.update(state, origin, size, {}, {}, false));
    CHECK_FALSE(tool.dragging());
    CHECK(tool.cancelled());
    update(tool, state, {event(Kind::pointer_down)});
    CHECK_FALSE(tool.update(state, origin, {900, 600}, {}, {}));
    CHECK_FALSE(tool.dragging());
    CHECK(tool.cancelled());
    update(tool, state, {event(Kind::pointer_down)});
    tool.cancel();
    CHECK_FALSE(tool.dragging());
    CHECK(tool.cancelled());
    update(tool, state, {event(Kind::pointer_down), event(Kind::pointer_up)});
    CHECK_FALSE(tool.dragging());
    CHECK_FALSE(tool.cancelled());
}

TEST_CASE("Navigation ignores invalid inputs and keeps extreme gestures finite",
          "[editor][navigation]") {
    auto state = scene();
    NavigationTool tool;
    const auto nan = std::numeric_limits<f32>::quiet_NaN();
    const auto huge = std::numeric_limits<f32>::max();
    CHECK_FALSE(update(tool, state, {wheel(nan), event(Kind::pointer_down, {nan, 350})}));
    const std::array press{event(Kind::pointer_down)};
    CHECK_FALSE(tool.update(state, origin, {0, 600}, press, press));
    CHECK_FALSE(tool.dragging());
    CHECK_FALSE(tool.update(state, origin, {800, nan}, press, press));
    state.viewport.editor_camera.distance = nan;
    CHECK_FALSE(update(tool, state, {wheel(1)}));
    CHECK_FALSE(tool.handledPointer());
    state.viewport.editor_camera.distance = 8;
    REQUIRE(
        update(tool, state, {event(Kind::pointer_down), event(Kind::pointer_move, {huge, huge})}));
    CHECK(std::isfinite(state.viewport.editor_camera.yaw));
    CHECK(std::abs(state.viewport.editor_camera.yaw) <= 180.0F);
    CHECK(state.viewport.editor_camera.pitch == camera_max_pitch);
    CHECK_FALSE(update(tool, state, {event(Kind::pointer_up, {nan, nan})}));
    CHECK_FALSE(tool.dragging()); // malformed release still ends capture.
    REQUIRE(camera(state).snapshot({800, 600}));
    state.viewport.editor_camera.yaw = state.viewport.editor_camera.pitch = 0;
    REQUIRE(update(tool, state,
                   {event(Kind::pointer_down, {500, 350}, {.shift = true}),
                    event(Kind::pointer_up, {huge, huge})}));
    CHECK(state.viewport.editor_camera.target.x == -camera_target_limit);
    CHECK(state.viewport.editor_camera.target.y == camera_target_limit);
    CHECK(state.viewport.editor_camera.target.z == 0.0F);
    REQUIRE(camera(state).snapshot({800, 600}));
    REQUIRE(encode(state));
}

TEST_CASE("Camera pivot and empty selection round trip while old scenes keep origin pivot",
          "[editor][navigation]") {
    auto state = scene();
    state.viewport.editor_camera.target = {2.5F, -3.25F, 7.125F};
    state.viewport.selected_object = 0;
    state.viewport.editor_camera.pitch = camera_max_pitch;
    state.viewport.editor_camera.distance = camera_min_distance;
    const auto text = encode(state);
    REQUIRE(text);
    const auto restored = decode(*text);
    REQUIRE(restored);
    CHECK(restored->viewport.editor_camera.target == state.viewport.editor_camera.target);
    CHECK(restored->viewport.selected_object == 0);
    CHECK(restored->viewport.editor_camera.pitch == state.viewport.editor_camera.pitch);
    CHECK(restored->viewport.editor_camera.distance == state.viewport.editor_camera.distance);
    auto legacy = *text;
    const auto start = legacy.find("camera_target = ", legacy.find("view = "));
    REQUIRE(start != std::string::npos);
    const auto end = legacy.find(';', start);
    REQUIRE(end != std::string::npos);
    legacy.erase(start, end - start + 1);
    const auto old_scene = decode(legacy);
    REQUIRE(old_scene);
    CHECK(old_scene->viewport.editor_camera.target == Vec3{});
    CHECK(old_scene->viewport.selected_object == 0);
    state.viewport.editor_camera.target.x = camera_target_limit + 1;
    CHECK_FALSE(encode(state));
    state.viewport.editor_camera.target.x = std::numeric_limits<f32>::quiet_NaN();
    CHECK_FALSE(encode(state));
    auto wrong_type = *text;
    wrong_type.replace(start, end - start, "camera_target = true");
    CHECK_FALSE(decode(wrong_type));
}
