#include "scenes/solar_system_scene.hpp"
#include "editor/animation.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>

namespace {
using namespace vng;
namespace project=editor_example;
namespace shot=example::solar_system;
constexpr f32 to_degrees=180.F/std::numbers::pi_v<f32>;
Vec3 add(Vec3 a,Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 sub(Vec3 a,Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec3 mul(Vec3 p,f32 s) { return {p.x*s,p.y*s,p.z*s}; }
f32 dot(Vec3 a,Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
f32 length(Vec3 p) { return std::sqrt(dot(p,p)); }
Vec3 normalize(Vec3 p) { return mul(p,1.F/length(p)); }
f32 angle_between(Vec3 a,Vec3 b) { return std::acos(std::clamp(dot(normalize(a),normalize(b)),-1.F,1.F))*to_degrees; }
auto scene() {
    static const auto authored=[] {
        auto result=shot::author_scene(VNG_SOLAR_SYSTEM_ASSETS);
        INFO((result?"authored solar system scene":result.error().message));
        REQUIRE(result);
        return std::move(*result);
    }();
    return authored;
}
// The editor's own eye placement for a pose (project.cpp camera()).
Vec3 eye_of(const project::CameraPose& pose) {
    const auto yaw=pose.yaw/to_degrees,pitch=pose.pitch/to_degrees;
    return add(pose.target,{std::sin(yaw)*std::cos(pitch)*pose.distance,std::sin(pitch)*pose.distance,
        std::cos(yaw)*std::cos(pitch)*pose.distance});
}
Vec3 centroid(const project::State& state,auto&& predicate) {
    Vec3 sum{}; u32 count{};
    for(const auto& instance:state.document.instances) if(predicate(instance)) { sum=add(sum,instance.transform.position); ++count; }
    REQUIRE(count>0);
    return mul(sum,1.F/static_cast<f32>(count));
}
bool is_ship(const project::SceneInstance& instance) {
    return instance.id>=shot::first_ship && instance.id<shot::first_rock && instance.id!=shot::earth;
}
}

TEST_CASE("Solar system scene holds every actor and only the starting keyframe","[solar_system][scene]") {
    const auto state=scene();
    CHECK(state.document.instances.size()==4+shot::ship_count+shot::rock_count); // hero, sun, Earth, camera, ships, rocks
    REQUIRE(project::active_camera(state,0));
    CHECK(project::active_camera(state,0)->id==shot::camera);
    CHECK(state.document.mesh_assets.size()==7); // three capital ships, Earth, three rock variants
    std::set<u32> identities;
    u32 ships{},rocks{};
    for(const auto& instance:state.document.instances) {
        CHECK(identities.insert(instance.id).second);
        ships+=is_ship(instance); rocks+=shot::is_rock(instance);
    }
    CHECK(ships==shot::ship_count);
    CHECK(rocks==shot::rock_count);
    const auto* sun=project::find_instance(state,shot::sun);
    REQUIRE(sun); CHECK(std::holds_alternative<project::SunSettings>(sun->settings));
    const auto* earth=project::find_instance(state,shot::earth);
    REQUIRE(earth); CHECK(earth->blueprint==shot::earth_blueprint); CHECK(earth->transform.scale==shot::earth_radius);
    REQUIRE(project::validate_animation(state));
    CHECK(state.document.timeline.find({shot::camera,"position"}));
    REQUIRE(state.document.keyframe_names.size()==1);
    CHECK(state.document.keyframe_names.begin()->first==0);
    for(const auto& track:state.document.timeline.tracks()) {
        REQUIRE(track.keys.size()==1);
        CHECK(track.keys.front().time==0);
    }
    CHECK(state.document.timeline_duration==shot::duration);
    CHECK(state.viewport.editor_camera==*project::evaluate_camera(state,0));
}

TEST_CASE("Camera starts beside Earth, looking away from the sun with Earth at the side","[solar_system][scene]") {
    const auto state=scene();
    const auto pose=*project::evaluate_camera(state,0);
    const auto eye=eye_of(pose);
    const auto forward=normalize(sub(pose.target,eye));
    const auto from_earth=length(sub(eye,shot::earth_center));
    CHECK(from_earth>1.4F*shot::earth_radius); // Above the surface, not inside it.
    CHECK(from_earth<2.5F*shot::earth_radius); // Still close: Earth dominates its side of the frame.
    const auto* sun=project::find_instance(state,shot::sun); REQUIRE(sun);
    CHECK(dot(forward,normalize(sub(sun->transform.position,eye)))<-.5F); // Outer space, not the sun.
    // 43 degree vertical FOV: about 35 to_degrees half width at 16:9. Earth's
    // centre sits past the frame edge while its near limb is well inside it.
    const auto to_earth=angle_between(forward,sub(shot::earth_center,eye));
    const auto angular_radius=std::asin(shot::earth_radius/from_earth)*to_degrees;
    CHECK(to_earth>30.F); CHECK(to_earth<50.F);
    CHECK(to_earth-angular_radius<15.F);
    CHECK(to_earth+angular_radius>45.F);
    const auto* kestrel=project::find_instance(state,shot::hero); REQUIRE(kestrel);
    const auto behind=sub(kestrel->transform.position,eye);
    CHECK(dot(behind,forward)<0);
    CHECK(length(behind)<10.F);
    // Heading matches the view direction, so it flies out of frame toward the belt.
    const auto expected_yaw=std::atan2(-forward.x,-forward.z)*to_degrees;
    CHECK(std::abs(kestrel->transform.rotation.y-expected_yaw)<1.F);
}

TEST_CASE("Belt is far but in view and the fleet waits thirty percent of the way back","[solar_system][scene]") {
    const auto state=scene();
    const auto pose=*project::evaluate_camera(state,0);
    const auto eye=eye_of(pose);
    const auto forward=normalize(sub(pose.target,eye));
    const auto belt=centroid(state,shot::is_rock),fleet=centroid(state,is_ship);
    const auto belt_distance=length(sub(belt,eye));
    CHECK(belt_distance>250.F);
    CHECK(belt_distance<400.F);
    CHECK(angle_between(forward,sub(belt,eye))<5.F);
    CHECK(angle_between(forward,sub(fleet,eye))<5.F);
    const auto ratio=length(sub(fleet,belt))/belt_distance;
    CHECK(ratio>.25F); CHECK(ratio<.35F);
    // The scene camera's far plane is four times the orbit distance; the whole belt fits.
    f32 farthest_rock{},nearest_rock=1e9F,farthest_ship{};
    for(const auto& instance:state.document.instances) {
        const auto depth=dot(sub(instance.transform.position,eye),forward);
        if(shot::is_rock(instance)) { farthest_rock=std::max(farthest_rock,depth); nearest_rock=std::min(nearest_rock,depth); }
        if(is_ship(instance)) farthest_ship=std::max(farthest_ship,depth);
    }
    CHECK(farthest_rock+20.F<pose.distance*4);
    CHECK(nearest_rock>farthest_ship); // Every rock lies beyond every ship.
    f32 closest=1e9F;
    for(const auto& rock:state.document.instances) if(shot::is_rock(rock))
        for(const auto& ship:state.document.instances) if(is_ship(ship))
            closest=std::min(closest,length(sub(rock.transform.position,ship.transform.position)));
    CHECK(closest>12.F);
}

TEST_CASE("Solar system scene survives a save and load","[solar_system][scene]") {
    const auto state=scene();
    const auto encoded=project::encode_scene(state);
    REQUIRE(encoded);
    const auto decoded=project::decode(*encoded);
    REQUIRE(decoded);
    CHECK(decoded->document.instances==state.document.instances);
    CHECK(decoded->document.timeline==state.document.timeline);
    CHECK(decoded->document.world_bounds==state.document.world_bounds);
    CHECK(*project::evaluate_camera(*decoded,0)==*project::evaluate_camera(state,0));
}
