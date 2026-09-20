#include "support/fleet_scene.hpp"
#include "editor/animation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace {
namespace fleet = example::fleet;
namespace project = editor_example;
using namespace vng;

project::State authored()
{
    auto result=fleet::author_scene(std::filesystem::path(VNG_TEST_ASSET_DIRECTORY));
    INFO((result ? "authored fleet scene" : result.error().message));
    REQUIRE(result);
    return std::move(*result);
}
Vec3 sub(Vec3 a,Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
float dot(Vec3 a,Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
float length(Vec3 v) { return std::sqrt(dot(v,v)); }
Vec3 transform(const Mat4& matrix,Vec3 point)
{
    const Vec4 input{point.x,point.y,point.z,1};
    Vec3 output{};
    for(std::size_t row=0;row<3;++row)
        for(std::size_t column=0;column<4;++column)
            output[row]+=matrix[column][row]*input[column];
    return output;
}
// Segment/sphere intersection, not a projected-center-only heuristic. Testing
// every authored vertex also covers silhouette tips and engine exhaust geometry.
bool occulted(Vec3 eye,Vec3 point,Vec3 sphere,float radius)
{
    const auto direction=sub(point,eye),relative=sub(sphere,eye);
    const auto distance2=dot(direction,direction);
    if(distance2<=0) return false;
    const auto t=dot(relative,direction)/distance2;
    if(t<=0||t>=1) return false;
    const auto closest=Vec3{eye.x+direction.x*t,eye.y+direction.y*t,eye.z+direction.z*t};
    return length(sub(closest,sphere))<radius;
}
}

TEST_CASE("Fleet scene explicitly separates reusable blueprints and independently editable instances",
          "[example][fleet][scene]")
{
    auto state=authored();
    REQUIRE(state.document.mesh_assets.size()==3);
    REQUIRE(state.document.instances.size()>=14);
    REQUIRE(project::find_instance(state,fleet::hero));
    REQUIRE(project::find_instance(state,fleet::sun));
    REQUIRE(project::find_instance(state,fleet::flagship));
    CHECK(state.document.timeline_duration==fleet::duration);
    CHECK(state.document.environment.stars>0);
    CHECK(state.document.environment.bloom_strength>0.F);
    CHECK_FALSE(project::sun_settings(state,fleet::sun)->white_spots);

    std::set<u32> identities;
    std::map<project::BlueprintId,std::size_t> occurrences;
    for(const auto& instance : state.document.instances) {
        REQUIRE(identities.insert(instance.id).second);
        CHECK(!instance.name.empty());
        CHECK(instance.id<state.document.next_instance_id);
        ++occurrences[instance.blueprint];
        if(instance.id!=fleet::sun) {
            REQUIRE(project::instance_mesh(state,instance.id));
            CHECK(project::mesh_settings(state,instance.id)->visible);
        }
    }
    for(const auto& blueprint : state.document.mesh_assets) {
        CHECK(occurrences[blueprint.id]>=2);
        CHECK(static_cast<u32>(blueprint.id)<state.document.next_blueprint_id);
    }
    const auto original=project::instance_mesh(state,fleet::flagship)->document();
    const auto blueprint=project::find_instance(state,fleet::flagship)->blueprint;
    auto sibling=std::ranges::find_if(state.document.instances,[&](const auto& instance) {
        return instance.id!=fleet::flagship&&instance.blueprint==blueprint;
    });
    REQUIRE(sibling!=state.document.instances.end());
    const auto sibling_before=*sibling;
    project::instance_transform(state,fleet::flagship)->scale*=.75F;
    CHECK(project::instance_mesh(state,fleet::flagship)->document()==original);
    CHECK(*sibling==sibling_before);
    REQUIRE(project::erase_instance(state,fleet::flagship));
    REQUIRE(project::mesh_geometry(state,blueprint));
    CHECK(project::mesh_geometry(state,blueprint)->document()==original);
    CHECK(project::find_instance(state,sibling_before.id));
    CHECK(project::validate_animation(state));
}

TEST_CASE("Fleet choreography is ordinary editable timestamp tracks, not hidden callbacks",
          "[example][fleet][scene]")
{
    const auto state=authored();
    REQUIRE(project::validate_animation(state));
    REQUIRE(project::has_camera_animation(state));
    for(const auto property : {"yaw","pitch","distance","target"}) {
        const auto* track=state.document.timeline.find({project::camera_animation_object,property});
        REQUIRE(track);
        REQUIRE(track->keys.size()>=8);
        CHECK(track->keys.front().time==0.F);
        CHECK(track->keys.back().time==fleet::duration);
    }
    for(const auto property : {"position","rotation"}) {
        const auto* track=state.document.timeline.find({fleet::hero,property});
        REQUIRE(track);
        REQUIRE(track->keys.size()>=8);
        CHECK(track->keys.front().time==0.F);
        CHECK(track->keys.back().time==fleet::duration);
        for(std::size_t i=1;i<track->keys.size();++i) {
            CHECK(track->keys[i].time>track->keys[i-1].time);
            CHECK(track->keys[i].incoming==timeline::Interpolation::linear);
        }
    }
    const auto before=project::evaluate_camera(state,0);
    const auto after=project::evaluate_camera(state,fleet::reveal_time);
    CHECK(before!=after);
    CHECK(state.document.keyframe_names.size()>=6);
    CHECK(state.document.timeline.find({fleet::flagship,"position"}));
    // Ships exist throughout the shot. The solar limb and camera reveal them,
    // rather than a visibility key spawning the fleet in front of the viewer.
    for(const auto& instance : state.document.instances)
        CHECK_FALSE(state.document.timeline.find({instance.id,"visible"}));
}

TEST_CASE("Fleet flyby keeps the camera and entire hero ship clear of the solar surface",
          "[example][fleet][scene]")
{
    const auto state=authored();
    const auto* hero=project::find_instance(state,fleet::hero);
    const auto* sun=project::find_instance(state,fleet::sun);
    REQUIRE(hero);
    REQUIRE(sun);
    const auto* mesh=project::instance_mesh(state,fleet::hero);
    REQUIRE(mesh);
    float bound{};
    for(u32 i=0;i<mesh->size();++i) bound=std::max(bound,length(mesh->position(i)));
    float camera_clearance=10000,hero_clearance=10000;
    // Include interpolated states between authored one-second keys.
    for(float time=0;time<=fleet::duration;time+=.125F) {
        const auto star=project::evaluate_instance(state,*sun,time);
        const auto lead=project::evaluate_instance(state,*hero,time);
        const auto center=star.transform.position;
        // Conservative allowance for the solar displacement envelope.
        const auto radius=std::get<project::SunSettings>(star.settings).radius*star.transform.scale*1.05F;
        const auto eye=project::camera(project::evaluate_camera(state,time),project::ViewMode::scene).position();
        camera_clearance=std::min(camera_clearance,length(sub(eye,center))-radius);
        hero_clearance=std::min(hero_clearance,
            length(sub(lead.transform.position,center))-radius-bound*lead.transform.scale);
    }
    CHECK(camera_clearance>.1F);
    CHECK(hero_clearance>.1F);
}

TEST_CASE("The opening hides complete fleet silhouettes behind the sun and later reveals them",
          "[example][fleet][scene]")
{
    const auto state=authored();
    const auto* sun=project::find_instance(state,fleet::sun);
    REQUIRE(sun);
    const auto star=project::evaluate_instance(state,*sun,0);
    // Slightly inside the nominal surface, avoiding a test that relies on a
    // protruding solar strand or bloom to hide an otherwise visible hull tip.
    const auto radius=std::get<project::SunSettings>(star.settings).radius*star.transform.scale*.97F;
    const auto eye=project::camera(project::evaluate_camera(state,0),project::ViewMode::scene).position();
    std::size_t checked_vertices{},exposed_vertices{};
    for(const auto& base : state.document.instances) {
        if(base.id==fleet::hero||base.id==fleet::sun) continue;
        INFO(base.name);
        const auto instance=project::evaluate_instance(state,base,0);
        const auto* mesh=project::instance_mesh(state,base.id);
        REQUIRE(mesh);
        const auto matrix=project::mesh_transform(project::ViewMode::scene,instance.transform);
        for(u32 vertex=0;vertex<mesh->size();++vertex) {
            ++checked_vertices;
            exposed_vertices+=occulted(eye,transform(matrix,mesh->position(vertex)),
                                       star.transform.position,radius) ? 0U : 1U;
        }
    }
    REQUIRE(checked_vertices>10000);
    CHECK(exposed_vertices==0);
    const auto revealed_eye=project::camera(project::evaluate_camera(state,fleet::reveal_time),
                                           project::ViewMode::scene).position();
    std::size_t revealed{};
    for(const auto& base : state.document.instances) {
        if(base.id==fleet::hero||base.id==fleet::sun) continue;
        const auto instance=project::evaluate_instance(state,base,fleet::reveal_time);
        revealed+=!occulted(revealed_eye,instance.transform.position,star.transform.position,radius) ? 1U : 0U;
    }
    CHECK(revealed>=state.document.instances.size()-4);
}

TEST_CASE("Fleet scene preserves all blueprints, instances and tunable animation on save/load",
          "[example][fleet][scene]")
{
    auto state=authored();
    const auto replacement=Vec3{5,2,-42};
    REQUIRE(project::key_property(state,{fleet::hero,"position"},fleet::reveal_time,replacement));
    state.document.keyframe_names[fleet::reveal_time]="My reveal adjustment";
    const auto source=project::encode_scene(state);
    REQUIRE(source);
    const auto restored=project::decode(*source);
    INFO((restored ? "scene decoded" : restored.error().message));
    REQUIRE(restored);
    CHECK(restored->document.instances==state.document.instances);
    CHECK(restored->document.mesh.document()==state.document.mesh.document());
    REQUIRE(restored->document.mesh_assets.size()==state.document.mesh_assets.size());
    for(std::size_t i=0;i<state.document.mesh_assets.size();++i) {
        const auto& a=restored->document.mesh_assets[i];
        const auto& b=state.document.mesh_assets[i];
        CHECK(a.id==b.id);
        CHECK(a.name==b.name);
        CHECK(a.geometry.document()==b.geometry.document());
        CHECK(a.settings==b.settings);
    }
    CHECK(restored->document.environment==state.document.environment);
    CHECK(restored->document.animation_camera==state.document.animation_camera);
    CHECK(restored->document.timeline==state.document.timeline);
    CHECK(restored->document.timeline_duration==fleet::duration);
    CHECK(restored->document.keyframe_names==state.document.keyframe_names);
    const auto* hero=project::find_instance(*restored,fleet::hero);
    REQUIRE(hero);
    CHECK(project::evaluate_instance(*restored,*hero,fleet::reveal_time).transform.position==replacement);
}
