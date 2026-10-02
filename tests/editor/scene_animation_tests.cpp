#include "../../examples/editor/editing_session.hpp"
#include "../../examples/editor/animation.hpp"
#include "../../examples/editor/scene_samples.hpp"
#include "../../examples/editor/rotation_math.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <limits>

namespace {
using namespace vng;
using namespace editor_example;
State scene() {
    content::vmesh::Document mesh;mesh.vertex_count=3;
    mesh.vertex_fields={{"position",{content::vmesh::ScalarType::Float32,3},std::vector<f32>{0,0,0,1,0,0,0,1,0}}};mesh.faces={{0,1,2}};
    auto geometry=editor::EditableMesh::create(std::move(mesh));REQUIRE(geometry);
    State state{.document={.mesh=std::move(*geometry)}};
    state.viewport.selected_object=1;
    return state;
}
u32 departure(State& state,AnimationInterval range={0,10}) {
    auto result=create_scene_animation(state,BlueprintId::departure,{1,0},range);
    INFO((result?"created":result.error().message));REQUIRE(result);return *result;
}
Vec3 position(const State& state,float t,u32 id=1) {return evaluate_transform(state,*find_instance(state,id),t).position;}
}
TEST_CASE("Animation factories initialize complete ordinary instances without moving the initial pose","[editor][animation-forest]") {
    auto state=scene();auto initial=position(state,0);
    auto id=departure(state);
    CHECK(is_animation_blueprint(find_instance(state,id)->blueprint));
    CHECK_FALSE(instance_visible(*find_instance(state,id)));
    CHECK(position(state,0)==initial);CHECK(position(state,5)!=initial);
    CHECK_FALSE(erase_instance(state,1));
    auto bytes=encode(state);REQUIRE(bytes);
    auto restored=decode(*bytes);REQUIRE(restored);
    CHECK(*scene_animation(*restored,id)==*scene_animation(state,id));
    CHECK(position(*restored,5)==position(state,5));
    CHECK(animation_debug_string(*scene_animation(state,id)).find("Camera follow")!=std::string::npos);
}
TEST_CASE("Animation parents evaluate typed children and feed motion into camera following","[editor][animation-forest]") {
    auto state=scene();auto camera=instantiate(state,BlueprintId::camera);REQUIRE(camera);
    auto initial=evaluate_transform(state,*find_instance(state,*camera),0);
    auto root=create_scene_animation(state,BlueprintId::departure,{1,*camera},{0,10});REQUIRE(root);
    CHECK(position(state,0,*camera)==initial.position);
    const auto& d=std::get<DepartureSequence>(scene_animation(state,*root)->root);
    const auto result=d.evaluate(5,10);
    CHECK(position(state,5)==result.ship.position);
    CHECK(position(state,5,*camera)==result.camera.position);
    CHECK(result.camera.position.x-result.ship.position.x==Catch::Approx(d.follow.offset.x));
    CHECK(render_camera(state,5).position()==result.camera.position);
    CHECK(has_camera_animation(state));
}
TEST_CASE("Independent roots compose disjoint properties and reject competing writers","[editor][animation-forest]") {
    auto state=scene();auto id=departure(state);
    const auto before=state.document.instances.size();
    auto conflict=create_scene_animation(state,BlueprintId::spin,{1,0},{2,8});CHECK_FALSE(conflict);
    CHECK(state.document.instances.size()==before);
    REQUIRE(conflict.error().message.find("rotation")!=std::string::npos);
    auto& a=std::get<AnimationSettings>(find_instance(state,id)->settings);
    std::get<DepartureSequence>(a.root).motion.orient_to_path=false;
    auto spin=create_scene_animation(state,BlueprintId::spin,{1,0},{2,8});REQUIRE(spin);
    auto sample=evaluate_transform(state,*find_instance(state,1),5);
    CHECK(sample.position!=position(state,0));CHECK(sample.rotation.y==Catch::Approx(30));
    CHECK(validate_scene_animations(state));
    auto bad=*scene_animation(state,id);std::get<DepartureSequence>(bad.root).ship=*spin;
    CHECK_FALSE(validate_scene_animations(state,id,&bad)); // No cycles or root-to-root lookup.
}
TEST_CASE("Creation preview accepts or cancels as one transaction while allowing scrubbing","[editor][animation-forest]") {
    EditingSession session(scene());
    const auto initial=position(session.state(),0);auto id=session.preview_animation(BlueprintId::departure,{1,0},{0,10});REQUIRE(id);
    CHECK(session.creating_animation());CHECK_FALSE(session.can_undo());
    session.viewport().time=4;
    auto draft=*scene_animation(session.state(),*id);std::get<DepartureSequence>(draft.root).motion.speed.final=8;
    REQUIRE(session.animation(draft));REQUIRE(session.cancel());
    CHECK_FALSE(scene_animation(session.state(),*id));CHECK(position(session.state(),0)==initial);
    CHECK_FALSE(session.dirty());CHECK_FALSE(session.can_undo());CHECK(session.state().viewport.selected_object==1);
    id=session.preview_animation(BlueprintId::departure,{1,0},{0,10});REQUIRE(id);REQUIRE(session.commit());
    CHECK(session.can_undo());REQUIRE(session.undo());CHECK_FALSE(scene_animation(session.state(),*id));
    REQUIRE(session.redo());CHECK(scene_animation(session.state(),*id));
}
TEST_CASE("Animation gestures transport only the changed root and undo exactly","[editor][animation-forest]") {
    auto state=scene();auto id=departure(state);EditingSession session(state);
    const auto original=*scene_animation(state,id);
    REQUIRE(session.begin_animation(id));auto value=original;std::get<DepartureSequence>(value.root).motion.speed.final=9;
    REQUIRE(session.animation(value));auto notice=session.take_changes();REQUIRE(notice);
    CHECK_FALSE(notice->changes.full);CHECK(notice->changes.properties.empty());CHECK(notice->changes.meshes.empty());
    CHECK(notice->changes.animations==std::set<u32>{id});
    auto patch=capture_patch(state.document.revision,session.state(),notice->changes);REQUIRE(patch);
    auto bytes=encode_patch(*patch);REQUIRE(bytes);CHECK(bytes->size()<2048);
    auto decoded=decode_patch(*bytes);REQUIRE(decoded);REQUIRE(apply_patch(state,*decoded));
    CHECK(position(state,4)==position(session.state(),4));
    REQUIRE(session.commit());REQUIRE(session.undo());CHECK(*scene_animation(session.state(),id)==original);
    REQUIRE(session.redo());CHECK(*scene_animation(session.state(),id)==value);
}
TEST_CASE("Invalid forest updates fail atomically and preserve unrelated roots","[editor][animation-forest]") {
    auto state=scene();auto id=departure(state);auto spin=create_scene_animation(state,BlueprintId::spin,{2,0},{0,10});REQUIRE(spin);
    DocumentChanges scope;scope.animations.insert(id);auto patch=capture_patch(state.document.revision,state,scope);REQUIRE(patch);
    patch->revision=patch->base_revision+1;patch->animations.at(id).interval.last=12;
    auto before=encode(state);REQUIRE(before);CHECK_FALSE(apply_patch(state,*patch));CHECK(*encode(state)==*before);
    patch->animations.at(id).interval.last=10;
    std::get<DepartureSequence>(patch->animations.at(id).root).motion.speed.initial=std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(apply_patch(state,*patch));CHECK(*encode(state)==*before);
}
TEST_CASE("Animation cache invalidates affected targets not unrelated instances","[editor][animation-forest]") {
    auto state=scene();auto id=departure(state);auto spin=create_scene_animation(state,BlueprintId::spin,{2,0},{0,10});REQUIRE(spin);
    SceneSamples cache;(void)cache.sample(state,3);const auto before=cache.evaluations();
    (void)cache.sample(state,3);CHECK(cache.evaluations()==before);
    auto& a=std::get<AnimationSettings>(find_instance(state,id)->settings);
    std::get<DepartureSequence>(a.root).motion.speed.final=10;
    auto result=cache.sample(state,3);CHECK(cache.evaluations()==before+2); // edited root + its ship
    CHECK(result.front().transform.position==position(state,3));
    REQUIRE(erase_instance(state,id));result=cache.sample(state,3);CHECK(result.front().transform.position==position(state,3));
}
TEST_CASE("Controlled keyframes reject manual writes while other properties remain editable","[editor][animation-forest]") {
    auto state=scene();departure(state,{2,8});
    CHECK_FALSE(key_property(state,{1,"position"},5,Vec3{}));
    CHECK(key_property(state,{1,"scale"},5,2.F));
    CHECK(key_property(state,{1,"position"},1,Vec3{}));
    EditingSession session(state);session.viewport().time=5;session.select_keyframe(5);
    CHECK_FALSE(session.begin_move(1));CHECK_FALSE(session.busy());
}
TEST_CASE("Baking detaches a root and preserves sampled motion and outside interpolation","[editor][animation-forest]") {
    auto state=scene();
    REQUIRE(key_property(state,{1,"position"},0,Vec3{0,0,0}));
    REQUIRE(key_property(state,{1,"position"},10,Vec3{10,0,0}));
    auto before=position(state,1),after=position(state,9);auto id=departure(state,{2,8});
    const auto middle=position(state,5);
    REQUIRE(bake_scene_animation(state,id,121));CHECK_FALSE(scene_animation(state,id));
    CHECK(position(state,5)==middle);
    CHECK(position(state,1).x==Catch::Approx(before.x));CHECK(position(state,9).x==Catch::Approx(after.x));
    CHECK(key_property(state,{1,"position"},5,Vec3{}));
}
TEST_CASE("Speed profile is normalized monotonic and leaves have no mutable scene dependencies","[editor][animation-forest]") {
    SpeedProfile speed{1,4,.4F};CHECK(speed.evaluate(0)==0);CHECK(speed.evaluate(1)==Catch::Approx(1));
    float last{};for(int i=0;i<=100;++i){const auto v=speed.evaluate(static_cast<float>(i)/100);CHECK(v>=last);last=v;}
    RouteCurve route{{Vec3{0,0,0},Vec3{1,0,0},Vec3{2,0,0},Vec3{3,0,0}}};
    CHECK(route.evaluate(.5F)==Vec3{1.5F,0,0});
}
TEST_CASE("Copying an animation remaps jointly copied targets and rejects duplicate writers atomically","[editor][animation-forest]") {
    auto state=scene();auto id=departure(state);EditClipboard clipboard;
    REQUIRE(clipboard.copy_instance(state,id));auto before=encode(state);REQUIRE(before);
    CHECK_FALSE(clipboard.paste(state));CHECK(*encode(state)==*before);
    const std::array ids{id,1U};REQUIRE(clipboard.copy_instances(state,ids));
    auto pasted=clipboard.paste(state);REQUIRE(pasted);REQUIRE(pasted->objects.size()==2);
    const auto copy=pasted->objects[0],ship=pasted->objects[1];
    REQUIRE(scene_animation(state,copy));
    CHECK(std::get<DepartureSequence>(scene_animation(state,copy)->root).ship==ship);
    CHECK(position(state,4,ship)==position(state,4,1));CHECK(validate_scene_animations(state));
    EditingSession session(state);const std::array removed{ship,copy};
    REQUIRE(session.erase_instances(removed));CHECK_FALSE(find_instance(session.state(),ship));
    REQUIRE(session.undo());CHECK(scene_animation(session.state(),copy));CHECK(find_instance(session.state(),ship));
}
TEST_CASE("Retargeting invalidates old and new targets and disabling releases its output scope","[editor][animation-forest]") {
    auto state=scene();auto other=instantiate(state,BlueprintId::mesh);REQUIRE(other);auto id=departure(state);
    SceneSamples cache;(void)cache.sample(state,4);
    auto& a=std::get<AnimationSettings>(find_instance(state,id)->settings);
    std::get<DepartureSequence>(a.root).ship=*other;
    auto instances=cache.sample(state,4);
    CHECK(instances[0].transform.position==find_instance(state,1)->transform.position);
    auto moved=std::ranges::find(instances,*other,&SceneInstance::id);REQUIRE(moved!=instances.end());
    CHECK(moved->transform.position==position(state,4,*other));
    a.enabled=false;
    auto spin=create_scene_animation(state,BlueprintId::spin,{*other,0},{0,10});REQUIRE(spin);
    auto enabled=*scene_animation(state,id);enabled.enabled=true;
    CHECK_FALSE(validate_scene_animations(state,id,&enabled));
    CHECK(validate_scene_animations(state));
}
TEST_CASE("A shared animation frame evaluates each root once and reuses independent results","[editor][animation-forest]") {
    auto state=scene();auto camera=instantiate(state,BlueprintId::camera);REQUIRE(camera);
    auto id=create_scene_animation(state,BlueprintId::departure,{1,*camera},{0,10});REQUIRE(id);
    REQUIRE(create_scene_animation(state,BlueprintId::spin,{2,0},{0,10}));
    AnimationFrame frame{state.document.instances,4};CHECK(frame.evaluated_roots()==2);
    for(const auto& instance:state.document.instances)
        CHECK(evaluate_instance(state,instance,4,&frame)==evaluate_instance(state,instance,4));
    CHECK(frame.evaluated_roots()==2);
    frame.update(state.document.instances,4);CHECK(frame.evaluated_roots()==2);
    auto& a=std::get<AnimationSettings>(find_instance(state,*id)->settings);
    std::get<DepartureSequence>(a.root).motion.speed.final=8;
    frame.update(state.document.instances,4);CHECK(frame.evaluated_roots()==3);
    a.enabled=false;frame.update(state.document.instances,4);CHECK(frame.evaluated_roots()==3);
    CHECK(evaluate_transform(state,*find_instance(state,1),4,&frame)==evaluate_transform(state,*find_instance(state,1),4));
    frame.update(state.document.instances,5);CHECK(frame.evaluated_roots()==4);
}
TEST_CASE("Invalid interval and unsafe bake changes preserve the authored tree","[editor][animation-forest]") {
    auto state=scene();auto id=departure(state);EditingSession session(state);
    CHECK_FALSE(session.duration(5));CHECK(session.state().document.timeline_duration==10);
    auto invalid=*scene_animation(state,id);
    std::get<DepartureSequence>(invalid.root).motion.route.points[0].x=scene_coordinate_limit;
    std::get<DepartureSequence>(invalid.root).motion.turbulence.amplitude=1;
    CHECK_FALSE(validate_scene_animations(state,id,&invalid));
    REQUIRE(erase_instance(state,id));
    auto spin=create_scene_animation(state,BlueprintId::spin,{1,0},{0,10});REQUIRE(spin);
    std::get<SpinAnimation>(std::get<AnimationSettings>(find_instance(state,*spin)->settings).root).degrees_per_second.y=60;
    auto before=encode(state);REQUIRE(before);CHECK_FALSE(bake_scene_animation(state,*spin));CHECK(*encode(state)==*before);
}
TEST_CASE("Departure initialization preserves banked and inverted orientations","[editor][animation-forest]") {
    for(const auto rotation:{Vec3{25,40,35},Vec3{160,-20,45},Vec3{-120,250,15}}) {
        auto state=scene();find_instance(state,1)->transform.rotation=rotation;departure(state);
        const auto pose=evaluate_transform(state,*find_instance(state,1),0);
        const auto expected=rotation_math::matrix(rotation),actual=rotation_math::matrix(pose.rotation);
        for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)
            CHECK(actual[r][c]==Catch::Approx(expected[r][c]).margin(.0001));
    }
}
