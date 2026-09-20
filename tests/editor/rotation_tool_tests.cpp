#include "../../examples/editor/rotation_tool.hpp"
#include "../../examples/editor/rotation_math.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace vng;
using editor_example::RotationGizmo;
using editor_example::RotationTool;
using input::EventKind;
constexpr ui::Rect viewport{100, 100, 640, 480};

struct Fixture {
    RotationTool tool;
    RotationGizmo target{{3, 4, 5}, {}, {}};
    gfx::CameraSnapshot camera;

    Fixture() {
        gfx::Camera value;
        value.set_position({0, 0, 10}).look_at({0, 0, 0}).set_orthographic({.vertical_height = 10});
        auto snapshot = value.snapshot({640, 480});
        REQUIRE(snapshot);
        camera = *snapshot;
        pump();
    }
    std::optional<Vec3> pump(std::span<const input::Event> raw = {}, bool available = true,
                            bool enabled = true) {
        return tool.update(target, camera, viewport, available ? raw : std::span<const input::Event>{},
                           raw, enabled);
    }
    std::size_t point(u32 axis, std::size_t first = 1, std::size_t last = 90) const {
        const auto& ring = tool.rings()[axis];
        for (auto i = first; i < last; ++i)
            if (ring.projected[i] && tool.hit_axis(ring.points[i]) == axis) return i;
        FAIL("No separately selectable point for ring " << axis);
        return 0;
    }
    void start(u32 axis, std::size_t i) {
        const std::array events{input::Event{.kind = EventKind::pointer_down,
                                             .position = tool.rings()[axis].points[i]}};
        REQUIRE_FALSE(pump(events));
        REQUIRE(tool.dragging());
        REQUIRE(tool.preview_rotation());
        REQUIRE(tool.handledPointer());
    }
};
bool close(Vec3 a, Vec3 b, f32 epsilon = .003F) {
    return std::abs(a.x - b.x) < epsilon && std::abs(a.y - b.y) < epsilon &&
           std::abs(a.z - b.z) < epsilon;
}
} // namespace

TEST_CASE("Free rotation uses MMB and the camera frame without Euler-axis constraints", "[editor][ui][rotation][free-rotate]") {
    Fixture f;f.target.free_rotation=true;f.target.rotation_degrees={20,90,-45};f.pump();
    const auto before=f.target.rotation_degrees;
    const Vec2 start{260,260},end{380,340}; // Starts away from the halo.
    const std::array press{input::Event{.kind=EventKind::pointer_down,.position=start,.button=2}};
    CHECK_FALSE(f.pump(press,false));CHECK_FALSE(f.tool.dragging());
    REQUIRE_FALSE(f.pump(press));REQUIRE(f.tool.dragging());
    const std::array move{input::Event{.kind=EventKind::pointer_move,.position=end}};
    f.pump(move,false);
    REQUIRE(f.tool.preview_rotation());
    const auto first=*f.tool.preview_rotation();
    f.pump(move,false);CHECK(close(*f.tool.preview_rotation(),first)); // baseline, not accumulation
    using namespace editor_example::rotation_math;
    const auto expected=multiply(matrix(turn({},f.camera.right,20)),multiply(matrix(turn({},f.camera.up,30)),matrix(before)));
    const auto actual=matrix(first);
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)CHECK(std::abs(expected[r][c]-actual[r][c])<.00001);
    const std::array release{input::Event{.kind=EventKind::pointer_up,.position=end,.button=2}};
    REQUIRE(f.pump(release,false));CHECK_FALSE(f.tool.dragging());
    ui::DrawList list{{840,680},{840,680},{}};f.tool.append(list);
    CHECK_FALSE(list.commands.empty());CHECK(list.commands.size()<500);
    for(auto modifiers:{input::Modifiers{.shift=true},input::Modifiers{.control=true},input::Modifiers{.alt=true}}) {
        auto modified=press;modified[0].modifiers=modifiers;
        f.pump(modified);CHECK_FALSE(f.tool.dragging());
    }
    for(auto kind:{EventKind::focus_lost,EventKind::key_down,EventKind::pointer_down}) {
        f.pump(press);REQUIRE(f.tool.dragging());f.pump(move);
        const std::array cancel{input::Event{.kind=kind,.key=input::Key::escape,.button=1}};
        CHECK_FALSE(f.pump(cancel,false));CHECK_FALSE(f.tool.dragging());CHECK(f.tool.handledPointer());
    }
}

TEST_CASE("Free rotation arrow nudges retain the mouse contribution", "[editor][ui][rotation][arrows]") {
    Fixture f;f.target.free_rotation=true;f.pump();
    const std::array press{input::Event{.kind=EventKind::pointer_down,.position={300,300},.button=2}};
    f.pump(press);REQUIRE(f.tool.dragging());
    const std::array motion{input::Event{.kind=EventKind::pointer_move,.position={340,320}}};
    f.pump(motion);const auto before=*f.tool.preview_rotation();
    const std::array up{input::Event{.kind=EventKind::key_down,.key=input::Key::up}};
    f.pump(up);const auto after=*f.tool.preview_rotation();
    using namespace editor_example::rotation_math;
    const auto normal=f.camera.forward;
    const auto expected=multiply(matrix(turn({}, {-normal.x,-normal.y,-normal.z},1)),matrix(before));
    const auto actual=matrix(after);
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)
        CHECK(std::abs(actual[r][c]-expected[r][c])<.00001F);
    f.pump(motion);CHECK(close(*f.tool.preview_rotation(),after));
}

TEST_CASE("Rotation rings render at stable pixel size and expose exact Euler axes",
          "[editor][ui][rotation]") {
    Fixture f;
    CHECK(f.tool.visible());
    CHECK_FALSE(f.tool.dragging());
    CHECK_FALSE(f.tool.preview_rotation());
    ui::DrawList list{{840, 680}, {840, 680}, {}};
    f.tool.append(list);
    REQUIRE_FALSE(list.commands.empty());
    CHECK(list.commands.size() < 3000);
    std::array<bool, 3> colors{};
    for (const auto& command : list.commands) {
        const auto& box = std::get<ui::BoxDraw>(command);
        colors[0] |= box.color.x == 1 && box.color.y < .2F;
        colors[1] |= box.color.y == 1 && box.color.x < .2F;
        colors[2] |= box.color.z == 1 && box.color.x < .2F;
        CHECK(viewport.contains({box.rect.x + box.radius, box.rect.y + box.radius}));
    }
    CHECK(colors == std::array{true, true, true});
    const auto front = f.tool.rings()[2].points[0];
    CHECK(std::abs(front.x - (420 + 68)) < .001F);
    CHECK(std::abs(front.y - 340) < .001F);

    f.target.rotation_degrees = {17, 23, 41};
    f.pump();
    const auto y = 23.0 * std::numbers::pi / 180;
    const auto z = 41.0 * std::numbers::pi / 180;
    CHECK(close(f.tool.rings()[0].normal,
                {static_cast<f32>(std::cos(z) * std::cos(y)),
                 static_cast<f32>(std::sin(z) * std::cos(y)), static_cast<f32>(-std::sin(y))}));
    CHECK(close(f.tool.rings()[1].normal,
                {static_cast<f32>(-std::sin(z)), static_cast<f32>(std::cos(z)), 0}));
    CHECK(f.tool.rings()[2].normal == Vec3{0, 0, 1});

    gfx::Camera perspective;
    perspective.set_position({0, 0, 10}).look_at({0, 0, 0});
    f.camera = *perspective.snapshot({640, 480});
    f.target.rotation_degrees = {};
    f.pump();
    CHECK(std::abs(f.tool.rings()[2].points[0].x - front.x) < .001F);
    perspective.set_position({0, 0, 100}).look_at({0, 0, 0});
    f.camera = *perspective.snapshot({640, 480});
    f.pump();
    CHECK(std::abs(f.tool.rings()[2].points[0].x - front.x) < .001F);
}

TEST_CASE("Rotation commits one exact component with perspective and nontrivial orientation",
          "[editor][ui][rotation]") {
    for (u32 axis = 0; axis < 3; ++axis) {
        INFO("axis " << axis);
        Fixture f;
        f.target.position = {.3F, -.2F, .1F};
        f.target.rotation_degrees = {10, 20, 30};
        gfx::Camera value;
        value.set_position({-4, 6, 9}).look_at(f.target.position);
        f.camera = *value.snapshot({640, 480});
        f.pump();
        const auto i = f.point(axis, 4, 65);
        const auto ring = f.tool.rings()[axis];
        const auto end = ring.points[i + 8]; // Exactly thirty degrees along the chosen plane.
        const std::array events{
            input::Event{.kind = EventKind::pointer_down, .position = ring.points[i]},
            input::Event{.kind = EventKind::pointer_move, .position = ring.points[i + 4]},
            input::Event{.kind = EventKind::pointer_up, .position = end}};
        const auto applied = f.pump(events);
        REQUIRE(applied);
        auto expected = f.target.rotation_degrees;
        expected[axis] += 30;
        CHECK(close(*applied, expected));
        CHECK_FALSE(f.tool.preview_rotation());
        CHECK_FALSE(f.tool.dragging());
        CHECK(f.tool.handledPointer());
        CHECK_FALSE(f.pump(std::span{events}.subspan(2), false));
        CHECK_FALSE(f.tool.handledPointer());
    }
}

TEST_CASE("Rotation capture uses unhandled presses and raw releases and preview is inert",
          "[editor][ui][rotation]") {
    Fixture f;
    const auto ring = f.tool.rings()[2];
    const std::array down{input::Event{.kind = EventKind::pointer_down, .position = ring.points[12]}};
    CHECK_FALSE(f.pump(down, false));
    CHECK_FALSE(f.tool.dragging());
    CHECK_FALSE(f.tool.handledPointer());
    f.start(2, 12);
    const std::array move{input::Event{.kind = EventKind::pointer_move, .position = ring.points[20]}};
    CHECK_FALSE(f.pump(move, false));
    REQUIRE(f.tool.preview_rotation());
    CHECK(close(*f.tool.preview_rotation(), {0, 0, 30}));
    const auto preview = f.tool.preview_rotation();
    f.pump();
    CHECK(f.tool.preview_rotation() == preview);
    // Release well outside the viewport still terminates captured input and
    // uses its exact final angle (a radial extension preserves that angle).
    const auto point = ring.points[36];
    const std::array release{input::Event{
        .kind = EventKind::pointer_up,
        .position = {420 + 10 * (point.x - 420), 340 + 10 * (point.y - 340)}}};
    const auto applied = f.pump(release, false);
    REQUIRE(applied);
    CHECK(close(*applied, {0, 0, 90}));
    CHECK_FALSE(f.tool.dragging());
    CHECK_FALSE(f.tool.preview_rotation());
    CHECK(f.tool.handledPointer());
}

TEST_CASE("Rotation unwraps ordered pointer angles across the seam and clamps authored bounds",
          "[editor][ui][rotation]") {
    Fixture f;
    const auto ring = f.tool.rings()[2];
    const std::array events{
        input::Event{.kind = EventKind::pointer_down, .position = ring.points[44]},
        input::Event{.kind = EventKind::pointer_move, .position = ring.points[52]},
        input::Event{.kind = EventKind::pointer_move, .position = ring.points[76]},
        input::Event{.kind = EventKind::pointer_move, .position = ring.points[4]},
        input::Event{.kind = EventKind::pointer_move, .position = ring.points[28]},
        input::Event{.kind = EventKind::pointer_up, .position = ring.points[36]}};
    const auto applied = f.pump(events);
    REQUIRE(applied);
    CHECK(close(*applied, {0, 0, 330}));

    f.target.rotation_degrees = {0, 0, 355};
    f.pump();
    const auto positive_ring = f.tool.rings()[2];
    const std::array positive{
        input::Event{.kind = EventKind::pointer_down, .position = positive_ring.points[12]},
        input::Event{.kind = EventKind::pointer_up, .position = positive_ring.points[20]}};
    REQUIRE(f.pump(positive) == Vec3{0, 0, 360});
    f.target.rotation_degrees = {0, 0, -355};
    f.pump();
    const auto negative_ring = f.tool.rings()[2];
    const std::array negative{
        input::Event{.kind = EventKind::pointer_down, .position = negative_ring.points[20]},
        input::Event{.kind = EventKind::pointer_up, .position = negative_ring.points[12]}};
    REQUIRE(f.pump(negative) == Vec3{0, 0, -360});
}

TEST_CASE("Edge-on rings and coincident Euler planes remain selectable and finite",
          "[editor][ui][rotation]") {
    Fixture f;
    for (u32 axis = 0; axis < 2; ++axis) {
        INFO("edge-on axis " << axis);
        const auto i = f.point(axis, 4, 20);
        const auto start = f.tool.rings()[axis].points[i];
        f.start(axis, i);
        const Vec2 end{start.x + (axis == 1 ? 30.F : 0), start.y + (axis == 0 ? 30.F : 0)};
        const std::array events{input::Event{.kind = EventKind::pointer_move, .position = end},
                                input::Event{.kind = EventKind::pointer_up, .position = end}};
        const auto applied = f.pump(events, false);
        REQUIRE(applied);
        CHECK(std::isfinite((*applied)[axis]));
        CHECK(std::abs((*applied)[axis]) > 10);
        CHECK(std::abs((*applied)[axis]) < 40);
        CHECK((*applied)[2] == 0);
        f.pump();
    }
    f.target.rotation_degrees = {5, 90, 37};
    f.pump();
    // X and Z planes coincide at y=90, but distinct radii separate the controls.
    CHECK(close(f.tool.rings()[0].normal, {0, 0, -1}));
    CHECK(f.tool.hit_axis(f.tool.rings()[0].points[f.point(0)]) == 0);
    CHECK(f.tool.hit_axis(f.tool.rings()[2].points[f.point(2)]) == 2);
}

TEST_CASE("Rotation lifecycle cancellation never commits stale or invalid values",
          "[editor][ui][rotation]") {
    Fixture f;
    f.start(2, 12);
    SECTION("Escape then release in the same frame cancels") {
        const std::array events{
            input::Event{.kind = EventKind::pointer_move, .position = f.tool.rings()[2].points[20]},
            input::Event{.kind = EventKind::key_down, .key = input::Key::escape},
            input::Event{.kind = EventKind::pointer_up, .position = f.tool.rings()[2].points[28]}};
        CHECK_FALSE(f.pump(events, false));
    }
    SECTION("focus loss cancels") {
        const std::array events{input::Event{.kind = EventKind::focus_lost}};
        CHECK_FALSE(f.pump(events, false));
    }
    SECTION("disabling the tool cancels") { CHECK_FALSE(f.pump({}, true, false)); }
    SECTION("changing revision cancels") { ++f.target.stamp.revision; CHECK_FALSE(f.pump()); }
    SECTION("changing object identity cancels") { ++f.target.stamp.object; CHECK_FALSE(f.pump()); }
    SECTION("changing generation cancels") { ++f.target.stamp.generation; CHECK_FALSE(f.pump()); }
    SECTION("changing position cancels") { f.target.position.x = 1; CHECK_FALSE(f.pump()); }
    SECTION("changing rotation cancels") { f.target.rotation_degrees.z = 1; CHECK_FALSE(f.pump()); }
    SECTION("resizing cancels") {
        CHECK_FALSE(f.tool.update(f.target, f.camera, {100, 100, 700, 480}, {}, {}, true));
    }
    SECTION("nonfinite target cancels") {
        f.target.rotation_degrees.z = std::numeric_limits<f32>::infinity();
        CHECK_FALSE(f.pump());
        CHECK_FALSE(f.tool.visible());
    }
    CHECK_FALSE(f.tool.dragging());
    CHECK_FALSE(f.tool.preview_rotation());
    CHECK(f.tool.handledPointer());
    f.tool.cancel();
    CHECK_FALSE(f.tool.visible());
    ui::DrawList list;
    f.tool.append(list);
    CHECK(list.commands.empty());
}
TEST_CASE("Ring and MMB rotation retain the current angle when the camera moves", "[editor][ui][gizmo-navigation]") {
    for(bool free:{false,true}) {
        Fixture f;f.target.free_rotation=free;f.pump();
        const auto start=free?Vec2{400,300}:f.tool.rings()[2].points[f.point(2)];
        const std::array press{input::Event{.kind=EventKind::pointer_down,.position=start,.button=free?2U:0U}};
        f.pump(press);REQUIRE(f.tool.dragging());
        const std::array move{input::Event{.kind=EventKind::pointer_move,.position={start.x+25,start.y-10}}};
        f.pump(move);const auto before=*f.tool.preview_rotation();
        gfx::Camera camera;camera.set_position({3,2,10}).look_at({}).set_orthographic({.vertical_height=10});
        f.camera=*camera.snapshot({640,480});f.pump();
        REQUIRE(f.tool.dragging());CHECK(close(*f.tool.preview_rotation(),before));
        f.pump(move);CHECK(close(*f.tool.preview_rotation(),before));
    }
}

TEST_CASE("Invalid pointer samples preserve the last valid rotation preview",
          "[editor][ui][rotation]") {
    Fixture f;
    const auto ring = f.tool.rings()[2];
    const std::array events{
        input::Event{.kind = EventKind::pointer_down, .position = ring.points[12]},
        input::Event{.kind = EventKind::pointer_move, .position = ring.points[20]},
        input::Event{.kind = EventKind::pointer_move,
                     .position = {std::numeric_limits<f32>::quiet_NaN(), 340}}};
    CHECK_FALSE(f.pump(events));
    REQUIRE(f.tool.preview_rotation());
    CHECK(close(*f.tool.preview_rotation(), {0, 0, 30}));
    const std::array release{input::Event{
        .kind = EventKind::pointer_up,
        .position = {std::numeric_limits<f32>::quiet_NaN(), 340}}};
    const auto applied = f.pump(release, false);
    REQUIRE(applied);
    CHECK(close(*applied, {0, 0, 30}));
    CHECK_FALSE(f.tool.dragging());
}

TEST_CASE("Attitude rings follow the body frame and drag a true local axis", "[editor][ui][rotation][attitude]") {
    using namespace editor_example;
    for(u32 axis=0;axis<3;++axis) {
        Fixture f;
        f.target.rotation_degrees={32,41,23};
        f.target.local_axes=std::array<Vec3,3>{{{0,1,0},{1,0,0},{0,0,-1}}};
        const auto normal=rotation_math::direction(f.target.rotation_degrees,(*f.target.local_axes)[axis]);
        gfx::Camera camera;
        camera.set_position({normal.x*10,normal.y*10,normal.z*10}).look_at({},std::abs(normal.y)>.9F?Vec3{0,0,1}:Vec3{0,1,0});
        f.camera=*camera.snapshot({640,480}); f.pump();
        CHECK(close(f.tool.rings()[axis].normal,normal));
        CHECK(f.tool.rings()[axis].label==std::array<std::string_view,3>{"Yaw","Pitch","Roll"}[axis]);
        const auto index=f.point(axis,4,60);
        const auto ring=f.tool.rings()[axis];
        const std::array events{
            input::Event{.kind=EventKind::pointer_down,.position=ring.points[index]},
            input::Event{.kind=EventKind::pointer_move,.position=ring.points[index+4]},
            input::Event{.kind=EventKind::pointer_up,.position=ring.points[index+8]}};
        const auto result=f.pump(events); REQUIRE(result);
        CHECK(close(*result,rotation_math::turn(f.target.rotation_degrees,(*f.target.local_axes)[axis],30)));
        CHECK(f.tool.turn().axis==axis); CHECK(std::abs(f.tool.turn().degrees-30)<.001);
        f.pump(); f.start(axis,f.point(axis));
        f.target.local_axes.reset(); f.pump(); CHECK_FALSE(f.tool.dragging()); CHECK(f.tool.handledPointer());
    }
}
