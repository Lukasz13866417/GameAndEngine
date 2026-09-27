#include "../../examples/editor/move_gizmo.hpp"
#include "../../examples/editor/surface_altitude_movement.hpp"
#include "../../examples/editor/rotation_origin_movement.hpp"
#include "../../examples/editor/editing_session.hpp"
#include "../../examples/editor/scene_move_gizmo.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <type_traits>

namespace {
using namespace vng;
using namespace editor_example;
struct BaseMovement;
struct TipMovement;
class PrivateTarget {
public:
    explicit PrivateTarget(Vec3 value) : position_(value) {}
private:
    friend struct BaseMovement;
    friend struct TipMovement;
    Vec3 position_;
};
struct BaseMovement {
    using Target=PrivateTarget;
    struct Edit { Vec3 local; };
    static Vec3 world_position(const Target& target) { return target.position_; }
    static Edit move_to(const Target&,Vec3 world) { return {world}; }
};
struct TipMovement {
    using Target=PrivateTarget;
    using Edit=BaseMovement::Edit;
    static Vec3 world_position(const Target& target) {
        auto position=target.position_;position.x+=2;return position;
    }
    static Edit move_to(const Target&,Vec3 world) { world.x-=2;return {world}; }
};
static_assert(std::is_empty_v<BaseMovement> && std::is_empty_v<TipMovement>);

void near(Vec3 a,Vec3 b) {
    for(unsigned i=0;i<3;++i)CHECK(a[i]==Catch::Approx(b[i]).margin(.0001));
}
gfx::CameraSnapshot camera() {
    gfx::Camera value;
    value.set_position({0,0,10}).look_at({}).set_orthographic({.vertical_height=10});
    return *value.snapshot({800,600});
}
template<class Binding> struct Fixture {
    MoveGizmo<Binding> gizmo;
    typename Binding::Target target{{}};
    gfx::CameraSnapshot view=camera();
    editor::Stamp stamp{1,1,1};
    typename MoveGizmo<Binding>::Reply pump(std::initializer_list<input::Event> events={},bool can_begin=true) {
        const std::span<const input::Event> input{events.begin(),events.size()};
        return gizmo.update(target,
            MoveGizmoContext{stamp,view,{0,0,800,600},input,input,can_begin});
    }
};
State scene() {
    content::vmesh::Document document;
    document.vertex_count=3;
    document.vertex_fields={{"position",{content::vmesh::ScalarType::Float32,3},
        std::vector<f32>{-1,-1,0,1,-1,0,0,2,0}}};
    document.faces={{0,1,2}};
    auto mesh=editor::EditableMesh::create(document);REQUIRE(mesh);
    State state{.document={.mesh=std::move(*mesh)}};
    instance_transform(state,1)->position={};
    auto second=instantiate(state,BlueprintId::mesh);REQUIRE(second);REQUIRE(*second==3);
    instance_transform(state,3)->position={5,0,0};
    return state;
}
}

TEST_CASE("A move gizmo binds directly to private data through an explicit friend", "[editor][ui][movement-binding]") {
    Fixture<BaseMovement> f;
    CHECK_FALSE(f.pump().edit);
    const auto start=f.gizmo.handle("X");REQUIRE(start);
    auto began=f.pump({{.kind=input::EventKind::pointer_down,.position=*start,.button=0}});
    CHECK(began.began);CHECK_FALSE(began.edit);
    const Vec2 end{start->x+60,start->y};
    auto moved=f.pump({{.kind=input::EventKind::pointer_move,.position=end}},false);
    REQUIRE(moved.edit);near(moved.edit->local,{1,0,0});
    // A proposal does not mutate the borrowed target or require its lifetime
    // after update. Repeated observations do not repeat an expensive recipe.
    near(BaseMovement::world_position(f.target),{});
    CHECK_FALSE(f.pump().edit);
    CHECK_FALSE(f.pump({{.kind=input::EventKind::pointer_move,.position=end}},false).edit);
    auto done=f.pump({{.kind=input::EventKind::pointer_up,.position=end,.button=0}},false);
    CHECK(done.finished);CHECK_FALSE(done.cancelled);CHECK_FALSE(f.gizmo.dragging());
    CHECK(f.gizmo.selected_axis()==0);
    const auto before=f.gizmo.debug_string();CHECK(before==f.gizmo.debug_string());
    CHECK(before.find("situation: idle")!=std::string::npos);
}

TEST_CASE("The same private target supports different explicit meanings of position", "[editor][ui][movement-binding]") {
    Fixture<BaseMovement> base;Fixture<TipMovement> tip;
    base.pump();tip.pump();
    REQUIRE(base.gizmo.handle("X"));REQUIRE(tip.gizmo.handle("X"));
    CHECK(tip.gizmo.handle("X")->x-base.gizmo.handle("X")->x==Catch::Approx(120));
    const auto start=*tip.gizmo.handle("X");const Vec2 end{start.x+60,start.y};
    const auto result=tip.pump({{.kind=input::EventKind::pointer_down,.position=start,.button=0},
        {.kind=input::EventKind::pointer_move,.position=end},
        {.kind=input::EventKind::pointer_up,.position=end,.button=0}});
    CHECK(result.began);CHECK(result.finished);REQUIRE(result.edit);
    near(result.edit->local,{1,0,0});
    near(TipMovement::world_position(tip.target),{2,0,0});
}

TEST_CASE("Typed movement keeps availability visibility and capture distinct", "[editor][ui][movement-binding]") {
    Fixture<BaseMovement> f;
    f.pump({},false);CHECK(f.gizmo.visible());
    auto start=*f.gizmo.handle("X");
    CHECK_FALSE(f.pump({{.kind=input::EventKind::pointer_down,.position=start,.button=0}},false).began);
    REQUIRE(f.pump({{.kind=input::EventKind::pointer_down,.position=start,.button=0}}).began);
    // Navigation may reframe an active gizmo without authoring another edit.
    gfx::Camera view;view.set_position({0,0,12}).look_at({}).set_orthographic({.vertical_height=12});
    f.view=*view.snapshot({800,600});
    CHECK_FALSE(f.pump({},false).edit);CHECK(f.gizmo.dragging());
    auto cancelled=f.pump({{.kind=input::EventKind::key_down,.key=input::Key::escape}},false);
    CHECK(cancelled.cancelled);CHECK_FALSE(cancelled.edit);CHECK_FALSE(f.gizmo.dragging());
    f.pump();start=*f.gizmo.handle("X");
    REQUIRE(f.pump({{.kind=input::EventKind::pointer_down,.position=start,.button=0}}).began);
    const auto cancelled_by_parent=f.gizmo.cancel();
    CHECK(cancelled_by_parent.cancelled);CHECK_FALSE(f.gizmo.visible());CHECK_FALSE(f.gizmo.selected_axis());
}

TEST_CASE("Gizmo cleanup needs no target or frame context and is safe to repeat", "[editor][ui][movement-binding]") {
    Fixture<BaseMovement> f;f.pump();
    const auto start=f.gizmo.handle("X");REQUIRE(start);
    REQUIRE(f.pump({{.kind=input::EventKind::pointer_down,.position=*start,.button=0}}).began);
    REQUIRE(f.pump({{.kind=input::EventKind::pointer_move,.position={start->x+60,start->y}}}).edit);
    const auto cancelled=f.gizmo.cancel();
    CHECK(cancelled.finished);CHECK(cancelled.cancelled);CHECK_FALSE(cancelled.edit);
    CHECK(f.gizmo.handledPointer());CHECK_FALSE(f.gizmo.dragging());CHECK_FALSE(f.gizmo.visible());
    CHECK_FALSE(f.gizmo.selected_axis());
    ui::DrawList list;f.gizmo.append(list);CHECK(list.commands.empty());
    const auto again=f.gizmo.cancel();
    CHECK_FALSE(again.finished);CHECK_FALSE(again.cancelled);CHECK_FALSE(again.edit);
    CHECK_FALSE(f.gizmo.handledPointer());
    // Selection/reactivation is the caller's decision, not a situation sent
    // to the gizmo. A new update binds a fresh baseline and no old capture.
    f.target=PrivateTarget{{3,0,0}};
    const auto presented=f.pump();
    CHECK(f.gizmo.visible());CHECK_FALSE(presented.began);CHECK_FALSE(presented.edit);
    const auto tip=f.gizmo.handle("X");REQUIRE(tip);
    CHECK(tip->x==Catch::Approx(start->x+180));
}

TEST_CASE("A stale target cancels a typed movement without proposing an edit", "[editor][ui][movement-binding]") {
    Fixture<BaseMovement> f;f.pump();auto start=*f.gizmo.handle("X");
    REQUIRE(f.pump({{.kind=input::EventKind::pointer_down,.position=start,.button=0}}).began);
    ++f.stamp.object;
    const auto result=f.pump();CHECK(result.cancelled);CHECK_FALSE(result.edit);
}

TEST_CASE("Earth altitude movement converts world anchors to constrained local radius", "[editor][ui][movement-binding]") {
    SurfaceMove surface;
    surface.center={1,2,0};surface.position={1,2,2};surface.radial_range=Vec2{1,3};
    // Rotated, non-uniformly scaled and translated blueprint.
    surface.frame={};surface.frame[0][1]=2;surface.frame[1][2]=3;surface.frame[2][0]=4;
    surface.frame[3]={10,-4,1,1};
    const auto inverse=example::mesh_frame::inverse(surface.frame);REQUIRE(inverse);
    const SurfaceAltitudeMovement::Target target{surface,*inverse,{0,0,1}};
    near(SurfaceAltitudeMovement::world_position(target),{18,-2,7});
    near(SurfaceAltitudeMovement::move_to(target,{100,50,50}).local_position,{1,2,3});
    near(SurfaceAltitudeMovement::move_to(target,{-100,50,50}).local_position,{1,2,1});
    near(surface.position,{1,2,2});
}

TEST_CASE("Instance movement proposals preserve selection offsets transaction and draft boundaries", "[editor][ui][movement-binding]") {
    EditingSession editing{scene()};
    editing.select_keyframe(0);
    const auto original=editing.state().document.mesh.document();
    const InstanceMovement::Target target{1,{}};
    const auto proposal=InstanceMovement::move_to(target,{2,3,4});
    CHECK_FALSE(editing.apply(proposal)); // Not an open movement transaction.
    const std::array<u32,2> selection{1,3};REQUIRE(editing.begin_move(1,selection));
    CHECK_FALSE(editing.apply(InstanceMovement::Edit{3,{8,0,0}}));
    REQUIRE(editing.apply(proposal));
    near(instance_transform(editing.state(),1)->position,{2,3,4});
    near(instance_transform(editing.state(),3)->position,{7,3,4});
    CHECK(editing.state().document.mesh.document()==original);
    auto notice=editing.take_changes();REQUIRE(notice);CHECK_FALSE(notice->changes.full);
    CHECK(notice->changes.properties.size()==2);CHECK(notice->changes.vertices.empty());
    REQUIRE(editing.commit());REQUIRE(editing.undo());CHECK_FALSE(editing.can_undo());
    near(instance_transform(editing.state(),1)->position,{});
    near(instance_transform(editing.state(),3)->position,{5,0,0});
    REQUIRE(editing.begin_move(1,selection));REQUIRE(editing.apply(proposal));REQUIRE(editing.cancel());
    near(instance_transform(editing.state(),1)->position,{});CHECK_FALSE(editing.can_undo());
}

TEST_CASE("Scene movement uses a fixed baseline across local document revisions without worker schemas", "[editor][ui][movement-binding]") {
    EditingSession editing{scene()};editing.select_keyframe(0);
    SceneMoveGizmo gizmo;const auto view=camera();
    const auto pump=[&](std::initializer_list<input::Event> events) {
        const std::span<const input::Event> input{events.begin(),events.size()};
        const auto position=instance_transform(editing.state(),1)->position;
        return dispatch(gizmo,SceneMoveGizmo::Instance{{1,position}},
            MoveGizmoContext{{1,1,editing.state().document.revision},view,{0,0,800,600},input,input,!editing.busy()});
    };
    pump({});const auto start=gizmo.handle("X");REQUIRE(start);
    REQUIRE(pump({{.kind=input::EventKind::pointer_down,.position=*start,.button=0}}).began);
    REQUIRE(editing.begin_move(1));
    const auto revision=editing.state().document.revision;
    auto moved=pump({{.kind=input::EventKind::pointer_move,.position={start->x+60,start->y}}});
    REQUIRE(moved.edit);REQUIRE(editing.apply(*moved.edit));
    CHECK(editing.state().document.revision>revision);
    moved=pump({{.kind=input::EventKind::pointer_move,.position={start->x+120,start->y}}});
    REQUIRE(moved.edit);near(moved.edit->position,{2,0,0});REQUIRE(editing.apply(*moved.edit));
    CHECK_FALSE(pump({}).edit);CHECK(gizmo.dragging());
    auto done=pump({{.kind=input::EventKind::pointer_up,.position={start->x+120,start->y},.button=0}});
    CHECK(done.finished);CHECK_FALSE(done.cancelled);REQUIRE(editing.commit());
    REQUIRE(editing.undo());near(instance_transform(editing.state(),1)->position,{});
    pump({});REQUIRE(pump({{.kind=input::EventKind::pointer_down,.position=*start,.button=0}}).began);
    auto replaced=dispatch(gizmo,SceneMoveGizmo::Instance{{3,{5,0,0}}},
        MoveGizmoContext{{3,1,editing.state().document.revision},view,{0,0,800,600}});
    CHECK(replaced.cancelled);CHECK_FALSE(replaced.edit);CHECK_FALSE(gizmo.dragging());
}

TEST_CASE("Native custom gizmos retain their own edit protocol beside typed gizmos", "[editor][ui][movement-binding]") {
    SceneMoveGizmo gizmo;const auto view=camera();
    editor::Inspector inspector{{7,2,4}};
    inspector.translation_gizmo("custom_endpoint",Vec3{},[](Vec3){});
    const auto schema=inspector.schema();
    const auto pump=[&](std::initializer_list<input::Event> events) {
        const std::span<const input::Event> input{events.begin(),events.size()};
        return dispatch(gizmo,SceneMoveGizmo::Native{schema},MoveGizmoContext{schema.stamp,view,{0,0,800,600},input,input});
    };
    pump({});const auto start=gizmo.handle("X");REQUIRE(start);
    auto result=pump({{.kind=input::EventKind::pointer_down,.position=*start,.button=0},
        {.kind=input::EventKind::pointer_up,.position={start->x+60,start->y},.button=0}});
    REQUIRE(result.native);CHECK_FALSE(result.edit);CHECK(result.native->control=="custom_endpoint");
    CHECK(result.native->stamp==schema.stamp);near(std::get<Vec3>(result.native->values[0].value),{1,0,0});
}

TEST_CASE("The scene host owns deactivation and exposes cancellation once for rollback", "[editor][ui][movement-binding]") {
    EditingSession editing{scene()};editing.select_keyframe(0);
    SceneMoveGizmo host;const auto view=camera();
    const SceneMoveGizmo::Instance target{{1,{}}};
    const auto pump=[&](std::initializer_list<input::Event> events) {
        const std::span<const input::Event> input{events.begin(),events.size()};
        return dispatch(host,target,MoveGizmoContext{{1,1,editing.state().document.revision},
            view,{0,0,800,600},input,input,!editing.busy()});
    };
    pump({});const auto start=host.handle("X");REQUIRE(start);
    REQUIRE(pump({{.kind=input::EventKind::pointer_down,.position=*start,.button=0}}).began);
    REQUIRE(editing.begin_move(1));
    const auto moved=pump({{.kind=input::EventKind::pointer_move,.position={start->x+60,start->y}}});
    REQUIRE(moved.edit);REQUIRE(editing.apply(*moved.edit));
    const MoveGizmoContext context{{1,1,editing.state().document.revision},view,{0,0,800,600}};
    const auto cancelled=dispatch(host,SceneMoveGizmo::Inactive{},context);
    REQUIRE(cancelled.cancelled);CHECK(cancelled.finished);CHECK_FALSE(cancelled.edit);
    REQUIRE(editing.cancel());near(instance_transform(editing.state(),1)->position,{});
    CHECK_FALSE(editing.can_undo());CHECK_FALSE(host.visible());CHECK_FALSE(host.dragging());
    const auto idle=dispatch(host,SceneMoveGizmo::Inactive{},context);
    CHECK_FALSE(idle.cancelled);CHECK_FALSE(idle.finished);CHECK_FALSE(host.handledPointer());
    CHECK_FALSE(pump({}).began);CHECK(host.visible());CHECK_FALSE(host.dragging());
}
