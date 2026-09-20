#include "../../examples/editor/instance_transform.hpp"
#include "../../examples/editor/rotation_interaction.hpp"
#include "../../examples/editor/rotation_math.hpp"
#include "../../examples/editor/scale_edits.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

namespace {
using namespace vng;
using namespace editor_example;
using input::EventKind;
using input::Key;
State transform_scene(bool region=false, bool ship=false) {
    content::vmesh::Document document;
    if(ship)document.metadata={{"coordinates/forward","+X"},{"coordinates/up","+Z"}};
    document.vertex_count=3;
    document.vertex_fields={{"position",{content::vmesh::ScalarType::Float32,3},
        std::vector<f32>{-1,-1,0,1,-1,0,0,2,0}}};
    document.faces={{0,1,2}};
    auto mesh=editor::EditableMesh::create(document); REQUIRE(mesh);
    State state{.document={.mesh=std::move(*mesh)}};
    auto second=instantiate(state,region ? BlueprintId::region : BlueprintId::mesh); REQUIRE(second); REQUIRE(*second==3);
    instance_transform(state,1)->position={-2,0,0};
    instance_transform(state,3)->position={2,0,0};
    instance_transform(state,3)->scale=2;
    state.viewport.selected_object=region ? 3 : 1;
    return state;
}
struct Fixture {
    EditingSession editing;
    InstanceTransformInteraction tool{editing};
    std::vector<u32> selection{1,3};
    TransformPivot pivot;
    gfx::CameraSnapshot camera;
    u64 generation{1};
    explicit Fixture(State state=transform_scene()):editing(std::move(state)) {
        editing.select_keyframe(editing.state().viewport.time);
        gfx::Camera c;c.set_position({0,0,10}).look_at({}).set_orthographic({.vertical_height=10});
        camera=*c.snapshot({800,600});
    }
    InstanceTransformChange pump(std::initializer_list<input::Event> events={},bool available=true,bool enabled=true) {
        const std::span<const input::Event> raw{events.begin(),events.size()};
        auto result=tool.update(generation,selection,pivot,camera,{0,0,800,600},
            available ? raw : std::span<const input::Event>{},raw,enabled);
        INFO((result ? "updated" : result.error().message)); REQUIRE(result); return *result;
    }
    InstanceTransform value(u32 id) const {
        return evaluate_transform(editing.state(),*find_instance(editing.state(),id),editing.state().viewport.time);
    }
};
void near(Vec3 a,Vec3 b) {for(unsigned c=0;c<3;++c)CHECK(a[c]==Catch::Approx(b[c]).margin(.0001));}
}

TEST_CASE("F moves a ship selection on its primary blueprint forward axis with one undo", "[editor][ui][instance-transform][gizmo]") {
    auto state=transform_scene(false,true);
    instance_transform(state,1)->rotation={0,0,90};
    Fixture f(std::move(state));
    const auto before=f.value(1),second=f.value(3);
    REQUIRE(f.pump({{.kind=EventKind::key_down,.position={400,300},.key=Key::f}}).began);
    CHECK(f.tool.gizmo()==GizmoMode::forward);
    CHECK(f.tool.visible());
    REQUIRE(f.pump({{.kind=EventKind::pointer_move,.position={460,180}}},false).changed);
    near(f.value(1).position,{before.position.x,before.position.y+2,before.position.z});
    near(f.value(3).position,{second.position.x,second.position.y+2,second.position.z});
    CHECK_FALSE(f.pump({{.kind=EventKind::key_down,.key=Key::x}},false).changed); // F never becomes a world-X move.
    ui::DrawList list;f.tool.append(list,{});CHECK_FALSE(list.commands.empty());
    REQUIRE(f.pump({{.kind=EventKind::key_down,.key=Key::enter}},false).committed);
    REQUIRE(f.editing.undo());
    near(f.value(1).position,before.position);near(f.value(3).position,second.position);
    CHECK_FALSE(f.editing.can_undo());
    REQUIRE(f.pump({{.kind=EventKind::key_down,.position={400,300},.key=Key::f}}).began);
    REQUIRE(f.pump({{.kind=EventKind::pointer_move,.position={460,180}}},false).changed);
    REQUIRE(f.pump({{.kind=EventKind::key_down,.key=Key::escape}},false).cancelled);
    near(f.value(1).position,before.position);near(f.value(3).position,second.position);
}

TEST_CASE("Forward shortcut is unavailable for generic or mixed blueprint selections", "[editor][ui][instance-transform][gizmo]") {
    for(auto state:{transform_scene(),transform_scene(true,true)}) {
        Fixture f(std::move(state));
        CHECK_FALSE(f.pump({{.kind=EventKind::key_down,.position={400,300},.key=Key::f}}).began);
        CHECK_FALSE(f.tool.active());CHECK_FALSE(f.editing.busy());
    }
}

TEST_CASE("Instance G R S previews from one baseline and commits one narrow undo entry", "[editor][ui][instance-transform]") {
    for(auto key:{Key::g,Key::r,Key::s}) {
        Fixture f;
        const auto before=f.editing.state().document;
        const auto first=f.value(1),second=f.value(3);
        const auto* vertices=std::get<std::vector<f32>>(f.editing.state().document.mesh.document().vertex_fields[0].values).data();
        REQUIRE(f.pump({{.kind=EventKind::key_down,.position={460,300},.key=key}}).began);
        REQUIRE(f.tool.active()); CHECK(f.editing.busy()); CHECK_FALSE(f.editing.can_undo());
        auto action=f.pump({{.kind=EventKind::pointer_move,.position={520,240}}},false);
        REQUIRE(action.changed); CHECK(f.tool.handled());
        if(key==Key::g) {
            near(f.value(1).position,{-1,1,0});near(f.value(3).position,{3,1,0});
        } else if(key==Key::r) {
            CHECK(f.value(1).rotation!=first.rotation);CHECK(f.value(3).rotation!=second.rotation);
            near(selection_center(instance_centers(f.editing.state(),1,f.selection)),{});
        } else {
            CHECK(f.value(1).scale>first.scale);
            CHECK(f.value(3).scale==Catch::Approx(f.value(1).scale*second.scale/first.scale));
            near(f.value(1).position,first.position);near(f.value(3).position,second.position);
        }
        const auto preview=f.value(1);
        CHECK_FALSE(f.pump({{.kind=EventKind::pointer_move,.position={520,240}}},false).changed);
        CHECK(f.value(1)==preview);
        CHECK(f.editing.state().document.mesh.document()==before.mesh.document());
        CHECK(std::get<std::vector<f32>>(f.editing.state().document.mesh.document().vertex_fields[0].values).data()==vertices);
        auto notice=f.editing.take_changes(); REQUIRE(notice);
        CHECK_FALSE(notice->changes.full);CHECK(notice->changes.vertices.empty());CHECK(notice->changes.regions.empty());
        CHECK(notice->changes.properties.size()==(key==Key::g ? 2 : 4));
        action=f.pump({{.kind=EventKind::key_down,.key=Key::enter}},false);
        CHECK(action.finished);CHECK(action.committed);CHECK_FALSE(f.tool.active());CHECK_FALSE(f.editing.busy());
        REQUIRE(f.editing.undo());CHECK_FALSE(f.editing.can_undo());
        CHECK(f.editing.state().document.instances==before.instances);CHECK(f.editing.state().document.timeline==before.timeline);
        REQUIRE(f.editing.redo());CHECK(f.value(1)==preview);
    }
}

TEST_CASE("Instance shortcuts cannot edit an unselected initial keyframe", "[editor][ui][instance-transform][startup]") {
    Fixture f;
    f.editing.select_keyframe(std::nullopt);
    for (auto key : {Key::g, Key::r, Key::s}) {
        CHECK_FALSE(f.pump({{.kind=EventKind::key_down,.position={460,300},.key=key}}).began);
        CHECK_FALSE(f.tool.active());
        CHECK_FALSE(f.editing.dirty());
    }
}
TEST_CASE("Instance rotation uses a shared world-space turn with all pivot modes", "[editor][ui][instance-transform]") {
    for(auto mode:{PivotMode::selection,PivotMode::individual,PivotMode::custom}) {
        auto initial=transform_scene();
        instance_transform(initial,1)->rotation={13,27,11};instance_transform(initial,3)->rotation={25,80,17};
        Fixture f{initial};f.pivot={mode,{0,3,0}};
        const auto centers=instance_centers(f.editing.state(),1,f.selection);
        const auto pivot=mode==PivotMode::custom ? Vec3{0,3,0} : mode==PivotMode::individual ? centers.front().world : Vec3{};
        const Vec2 pixel{400+pivot.x*60,300-pivot.y*60};
        REQUIRE(f.pump({{.kind=EventKind::key_down,.position={pixel.x+60,pixel.y},.key=Key::r}}).began);
        const Vec2 end{pixel.x,pixel.y-60};
        auto action=f.pump({{.kind=EventKind::pointer_move,.position=end},{.kind=EventKind::pointer_down,.position=end}},false);
        REQUIRE(action.committed);CHECK_FALSE(f.tool.active());
        const auto after=instance_centers(f.editing.state(),1,f.selection);
        const auto turn=rotation_math::matrix({0,0,90});
        for(std::size_t i=0;i<centers.size();++i) {
            const auto expected=rotation_math::multiply(turn,rotation_math::matrix(centers[i].transform.rotation));
            const auto actual=rotation_math::matrix(after[i].transform.rotation);
            for(unsigned a=0;a<3;++a)for(unsigned b=0;b<3;++b)CHECK(actual[a][b]==Catch::Approx(expected[a][b]).margin(.00001));
            const auto p=centers[i].world;
            near(after[i].world,mode==PivotMode::individual ? p : Vec3{pivot.x-(p.y-pivot.y),pivot.y+(p.x-pivot.x),p.z});
        }
    }
}
TEST_CASE("Whole regions share instance G R S without rewriting their editable boundary", "[editor][ui][instance-transform]") {
    for(auto key:{Key::g,Key::r,Key::s}) {
        Fixture f{transform_scene(true)};
        const auto before=f.editing.state().document;
        const auto original=f.value(3);
        REQUIRE(f.pump({{.kind=EventKind::key_down,.position={460,300},.key=key}}).began);
        REQUIRE(f.pump({{.kind=EventKind::pointer_move,.position={510,240}}},false).changed);
        CHECK(f.value(3)!=original);
        CHECK(find_instance(f.editing.state(),3)->settings==std::ranges::find(before.instances,3U,&SceneInstance::id)->settings);
        REQUIRE(f.pump({{.kind=EventKind::key_down,.key=Key::enter}},false).committed);
        REQUIRE(f.editing.undo());CHECK(f.editing.state().document.instances==before.instances);
    }
}
TEST_CASE("Modal instance transforms cancel on lost eligibility and never steal typing or shortcuts", "[editor][ui][instance-transform]") {
    for(unsigned reason=0;reason<6;++reason) {
        Fixture f;const auto before=f.editing.state().document;
        REQUIRE(f.pump({{.kind=EventKind::key_down,.position={460,300},.key=Key::g}}).began);
        REQUIRE(f.pump({{.kind=EventKind::pointer_move,.position={520,240}}},false).changed);
        InstanceTransformChange action;
        if(reason==0)action=f.pump({{.kind=EventKind::key_down,.key=Key::escape}},false);
        if(reason==1)action=f.pump({{.kind=EventKind::pointer_down,.position={500,250},.button=1}},false);
        if(reason==2)action=f.pump({{.kind=EventKind::focus_lost}},false);
        if(reason==3)action=f.pump({},false,false);
        if(reason==4){++f.generation;action=f.pump();}
        if(reason==5){f.selection={1};action=f.pump();}
        CHECK(action.cancelled);CHECK(action.finished);CHECK_FALSE(f.tool.active());CHECK_FALSE(f.editing.busy());
        CHECK_FALSE(f.editing.can_undo());CHECK(f.editing.state().document.instances==before.instances);
        CHECK(f.editing.state().document.timeline==before.timeline);
    }
    Fixture f;
    input::Event key{.kind=EventKind::key_down,.position={400,300},.key=Key::s};
    key.modifiers.control=true;CHECK_FALSE(f.pump({key}).began);
    key.modifiers.control=false;CHECK_FALSE(f.pump({key},false).began);
    key.repeat=true;CHECK_FALSE(f.pump({key}).began);
    key.repeat=false;key.position={850,650};CHECK_FALSE(f.pump({key}).began);
    key.position={400,300};f.editing.viewport().time=1;CHECK_FALSE(f.pump({key}).began);
    CHECK_FALSE(f.editing.busy());CHECK_FALSE(f.editing.can_undo());
}
TEST_CASE("Ring adapter does not own a keyboard rotation's transaction", "[editor][ui][instance-transform]") {
    Fixture f;RotationInteraction rings{f.editing};
    REQUIRE(f.pump({{.kind=EventKind::key_down,.position={460,300},.key=Key::r}}).began);
    CHECK_FALSE(rings.active());REQUIRE(rings.cancel());CHECK(f.editing.busy());
    REQUIRE(f.pump({{.kind=EventKind::key_down,.key=Key::escape}}).cancelled);
}
TEST_CASE("Instance G R S axis constraints and same-frame confirmation consume input", "[editor][ui][instance-transform]") {
    Fixture f;
    REQUIRE(f.pump({{.kind=EventKind::key_down,.position={460,300},.key=Key::g}}).began);
    REQUIRE(f.pump({{.kind=EventKind::key_down,.key=Key::x},
                   {.kind=EventKind::pointer_move,.position={520,240}}},false).changed);
    near(f.value(1).position,{-1,0,0});near(f.value(3).position,{3,0,0});
    REQUIRE(f.pump({{.kind=EventKind::key_down,.key=Key::x}},false).changed);
    near(f.value(1).position,{-1,1,0});
    REQUIRE(f.pump({{.kind=EventKind::key_down,.key=Key::escape}},false).cancelled);
    const auto action=f.pump({{.kind=EventKind::key_down,.position={460,300},.key=Key::r},
                             {.kind=EventKind::key_down,.key=Key::z},
                             {.kind=EventKind::pointer_down,.position={400,240}}});
    CHECK(action.began);CHECK(action.finished);CHECK(action.committed);CHECK(f.tool.handled());
    near(f.value(1).rotation,{0,0,90});near(f.value(3).rotation,{0,0,90});
}
TEST_CASE("Instance keyboard scale saturates at shared limits without losing the gesture", "[editor][ui][instance-transform]") {
    for(float first:{.05F,.27F,1.3F,2.3F,3.F})for(float second:{.05F,.051F,1.7F,2.99F,3.F}) {
        INFO("Primary scale " << first << ", secondary scale " << second);
        auto state=transform_scene();instance_transform(state,1)->scale=first;instance_transform(state,3)->scale=second;
        Fixture f{std::move(state)};
        REQUIRE(f.pump({{.kind=EventKind::key_down,.position={460,300},.key=Key::s}}).began);
        for(auto end:{Vec2{400,300},Vec2{4000,300}}) {
            f.pump({{.kind=EventKind::pointer_move,.position=end}},false);
            CHECK(f.tool.active());
            CHECK(f.value(1).scale>=min_instance_scale);CHECK(f.value(1).scale<=max_instance_scale);
            CHECK(f.value(3).scale>=min_instance_scale);CHECK(f.value(3).scale<=max_instance_scale);
            CHECK(f.value(1).scale/first==Catch::Approx(f.value(3).scale/second));
        }
        CHECK(f.pump({{.kind=EventKind::key_down,.key=Key::escape}},false).cancelled);
    }
}
TEST_CASE("S X Y Z changes only the chosen local dimension and switches from the baseline", "[editor][ui][instance-transform]") {
    for(bool region:{false,true})for(unsigned axis=0;axis<3;++axis) {
        auto state=transform_scene(region);
        instance_transform(state,1)->axis_scale={1,2,3};
        instance_transform(state,3)->axis_scale={2,1,4};
        instance_transform(state,1)->rotation={20,40,70};
        Fixture f{state};
        const auto original=f.editing.state().document;
        const auto first=f.value(1),second=f.value(3);
        const auto center=selection_center(instance_centers(state,state.viewport.selected_object,f.selection));
        const Vec2 p{400+center.x*60+100,300-center.y*60};
        REQUIRE(f.pump({{.kind=EventKind::key_down,.position=p,.key=Key::s}}).began);
        const std::array keys{Key::x,Key::y,Key::z};
        REQUIRE(f.pump({{.kind=EventKind::key_down,.key=keys[axis]},
            {.kind=EventKind::pointer_move,.position={p.x+100,p.y}}},false).changed);
        for(unsigned c=0;c<3;++c) {
            CHECK(f.value(1).axis_scale[c]==Catch::Approx(first.axis_scale[c]*(c==axis?2:1)));
            CHECK(f.value(3).axis_scale[c]==Catch::Approx(second.axis_scale[c]*(c==axis?2:1)));
        }
        CHECK(f.value(1).scale==first.scale);CHECK(f.value(3).scale==second.scale);
        CHECK(f.value(1).rotation==first.rotation);CHECK(f.value(3).position==second.position);
        if(region)CHECK(find_instance(f.editing.state(),3)->settings==find_instance(state,3)->settings);
        // Another axis replaces the constraint; it must not accumulate deformation.
        REQUIRE(f.pump({{.kind=EventKind::key_down,.key=keys[(axis+1)%3]}},false).changed);
        CHECK(f.value(1).axis_scale[axis]==first.axis_scale[axis]);
        REQUIRE(f.pump({{.kind=EventKind::key_down,.key=Key::enter}},false).committed);
        auto saved=encode(f.editing.state());REQUIRE(saved);
        auto loaded=decode(*saved);REQUIRE(loaded);
        CHECK(instance_transform(*loaded,1)->axis_scale==f.value(1).axis_scale);
        REQUIRE(f.editing.undo());CHECK(f.editing.state().document.instances==original.instances);
    }
}
TEST_CASE("Axis scale keys and narrow patches preserve instance-local proportions", "[editor][ui][instance-transform]") {
    auto state=transform_scene();
    REQUIRE(key_property(state,{1,"axis_scale"},0,Vec3{1,1,1}));
    REQUIRE(key_property(state,{1,"axis_scale"},2,Vec3{3,2,1}));
    state.viewport.time=2;
    Fixture f{state};
    REQUIRE(f.pump({{.kind=EventKind::key_down,.position={500,300},.key=Key::s},
        {.kind=EventKind::key_down,.key=Key::z}}).began);
    REQUIRE(f.pump({{.kind=EventKind::pointer_move,.position={600,300}}},false).changed);
    REQUIRE(f.pump({{.kind=EventKind::key_down,.key=Key::enter}},false).committed);
    near(f.value(1).axis_scale,{3,2,2});
    const auto sampled=evaluate_transform(f.editing.state(),*find_instance(f.editing.state(),1),1);
    near(sampled.axis_scale,{2,1.5F,1.5F});
    const auto changes=f.editing.take_changes();REQUIRE(changes);CHECK_FALSE(changes->changes.full);
    auto patch=capture_patch(state.document.revision,f.editing.state(),changes->changes);REQUIRE(patch);
    auto bytes=encode_patch(*patch);REQUIRE(bytes);
    auto parsed=decode_patch(*bytes);REQUIRE(parsed);REQUIRE(apply_patch(state,*parsed));
    CHECK(state.document.instances==f.editing.state().document.instances);
    CHECK(state.document.timeline==f.editing.state().document.timeline);
}
TEST_CASE("Stretched regions round trip between local geometry and world editing points", "[editor][ui][instance-transform]") {
    auto state=transform_scene(true);
    auto& transform=*instance_transform(state,3);
    transform.axis_scale={.4F,2.7F,1.3F};transform.rotation={20,55,-15};transform.scale=.8F;
    Region local;local.id=3;
    static_cast<RegionGeometry&>(local)=region_settings(state,3)->boundary;
    const auto world=region_to_world(state,local);
    const auto roundtrip=region_to_local(state,world);
    for(std::size_t i=0;i<local.points.size();++i)near(roundtrip.points[i],local.points[i]);
    CHECK(region_settings(state,3)->boundary.points==local.points);
}
