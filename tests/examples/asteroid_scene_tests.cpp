#include "support/asteroid_scene.hpp"
#include "support/asteroid_assets.hpp"
#include "editor/animation.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <limits>
#include <set>

namespace {
using namespace vng;
namespace project=editor_example;
namespace belt=example::asteroids;
Vec3 sub(Vec3 a,Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
float dot(Vec3 a,Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
float length(Vec3 p) { return std::sqrt(dot(p,p)); }
auto scene() {
    static const auto authored=[] {
        auto result=belt::author_scene(VNG_ASTEROID_ASSETS);
        INFO((result?"authored asteroid scene":result.error().message));
        REQUIRE(result);
        return std::move(*result);
    }();
    return authored; // Each case edits its own state; generate the large shot once.
}
}
TEST_CASE("Original asteroid blueprints contain finite crater geometry and compatible shading", "[asteroid][assets]") {
    std::array<Vec3,3> extents{};
    for(u32 type=0;type<3;++type) {
        const auto document=belt::rock_mesh(type);
        REQUIRE(document.faces.size()>=1280);
        REQUIRE(document.vertex_count==document.faces.size()*3);
        const auto& positions=std::get<std::vector<f32>>(document.vertex_fields[0].values);
        const auto& normals=std::get<std::vector<f32>>(document.vertex_fields[1].values);
        std::size_t invalid{};
        for(std::size_t i=0;i<positions.size();i+=3) {
            const Vec3 p{positions[i],positions[i+1],positions[i+2]};
            const Vec3 n{normals[i],normals[i+1],normals[i+2]};
            invalid+=!(std::isfinite(length(p)) && std::abs(length(n)-1.F)<.001F && dot(n,p)>0);
            for(std::size_t axis=0;axis<3;++axis) extents[type][axis]=std::max(extents[type][axis],std::abs(p[axis]));
        }
        CHECK(invalid==0);
        const auto text=content::vmesh::write_vmesh(document);
        REQUIRE(text);
        const auto restored=content::vmesh::parse_vmesh(*text);
        REQUIRE(restored);
        CHECK(*restored==document);
    }
    CHECK(extents[1].x>extents[1].y*1.4F);
    CHECK(extents[2].y>extents[2].x);
}
TEST_CASE("Asteroid shot stays an ordinary editable scene with shared rock blueprints", "[asteroid][scene]") {
    auto state=scene();
    CHECK(state.document.mesh_assets.size()==6);
    CHECK(state.document.instances.size()==belt::rock_count+belt::fleet_count+2); // pathfinder, fleet, rocks, camera
    CHECK(project::active_camera(state,0)->id==belt::camera);
    CHECK_FALSE(project::find_instance(state,2));
    CHECK(state.document.timeline_duration==belt::duration);
    REQUIRE(project::validate_animation(state));
    CHECK(project::has_camera_animation(state));
    std::set<u32> identities;
    std::map<project::BlueprintId,unsigned> uses;
    for(const auto& instance:state.document.instances) {
        CHECK(identities.insert(instance.id).second);
        ++uses[instance.blueprint];
        if(instance.id==belt::camera) { CHECK(project::is_camera_instance(state,instance.id)); continue; }
        REQUIRE(project::mesh_settings(state,instance.id));
        CHECK(project::mesh_settings(state,instance.id)->visible);
        CHECK_FALSE(state.document.timeline.find({instance.id,"visible"}));
        if(belt::is_rock(instance)) CHECK(state.document.timeline.find({instance.id,"rotation"}));
    }
    for(u32 id=6;id<9;++id) CHECK(uses[static_cast<project::BlueprintId>(id)]>=2);
    const auto geometry=project::instance_mesh(state,belt::first_rock)->document();
    project::instance_transform(state,belt::first_rock)->scale=2.F;
    CHECK(project::instance_mesh(state,belt::first_rock)->document()==geometry);
    REQUIRE(project::key_camera(state,25,{15,8,30,{1,2,-70}}));
    const auto encoded=project::encode_scene(state);
    REQUIRE(encoded);
    CHECK(encoded->size()<16*1024*1024);
    const auto decoded=project::decode(*encoded);
    REQUIRE(decoded);
    CHECK(decoded->document.instances==state.document.instances);
    CHECK(decoded->document.timeline==state.document.timeline);
    CHECK(project::evaluate_camera(*decoded,25)==project::evaluate_camera(state,25));
    CHECK(project::instance_mesh(*decoded,belt::first_rock)->document()==geometry);
}
TEST_CASE("The entire lead ship and camera clear tumbling rocks between authored keys", "[asteroid][scene]") {
    const auto state=scene();
    std::map<project::BlueprintId,float> bounds;
    for(const auto& instance:state.document.instances) {
        if(bounds.contains(instance.blueprint) || instance.id==belt::camera) continue;
        const auto* geometry=project::instance_mesh(state,instance.id);
        for(u32 vertex=0;vertex<geometry->size();++vertex)
            bounds[instance.blueprint]=std::max(bounds[instance.blueprint],length(geometry->position(vertex)));
    }
    const auto& hero=*project::find_instance(state,belt::hero);
    float camera_clearance=1000,ship_clearance=1000;
    u32 nearest_camera{},nearest_ship{};
    for(float time=0;time<=belt::duration;time+=.125F) {
        const auto eye=project::camera(project::evaluate_camera(state,time),project::ViewMode::scene).position();
        const auto ship=project::evaluate_instance(state,hero,time);
        for(const auto& instance:state.document.instances) {
            if(!belt::is_rock(instance)) continue;
            const auto radius=bounds.at(instance.blueprint)*instance.transform.scale;
            const auto a=length(sub(eye,instance.transform.position))-radius;
            const auto b=length(sub(ship.transform.position,instance.transform.position))-radius-
                         bounds.at(hero.blueprint)*ship.transform.scale;
            if(a<camera_clearance) { camera_clearance=a;nearest_camera=instance.id; }
            if(b<ship_clearance) { ship_clearance=b;nearest_ship=instance.id; }
        }
    }
    INFO("camera clearance "<<camera_clearance<<" / rock "<<nearest_camera);
    INFO("ship clearance "<<ship_clearance<<" / rock "<<nearest_ship);
    CHECK(camera_clearance>.4F);
    CHECK(ship_clearance>.15F);
}

TEST_CASE("Lead ship turns gently and the fleet waits well beyond the belt", "[asteroid][scene]") {
    const auto state=scene();
    const auto& hero=*project::find_instance(state,belt::hero);
    auto previous=project::evaluate_instance(state,hero,0).transform.rotation;
    float fastest_turn{},largest_bank{};
    constexpr float step=.125F;
    for(float time=step;time<=belt::duration;time+=step) {
        const auto rotation=project::evaluate_instance(state,hero,time).transform.rotation;
        fastest_turn=std::max(fastest_turn,std::abs(rotation.y-previous.y)/step);
        largest_bank=std::max(largest_bank,std::abs(rotation.z));
        previous=rotation;
    }
    INFO("maximum yaw rate "<<fastest_turn<<" degrees/s; bank "<<largest_bank);
    CHECK(fastest_turn<6.F);
    CHECK(largest_bank<=9.01F);

    // Bounding spheres are conservative even while the rocks rotate. Check
    // actual hull extents, not just the distance between instance origins.
    std::map<project::BlueprintId,float> bounds;
    for(const auto& instance:state.document.instances) {
        if(bounds.contains(instance.blueprint) || instance.id==belt::camera) continue;
        const auto* mesh=project::instance_mesh(state,instance.id);
        for(u32 vertex=0;vertex<mesh->size();++vertex)
            bounds[instance.blueprint]=std::max(bounds[instance.blueprint],length(mesh->position(vertex)));
    }
    float belt_exit=std::numeric_limits<float>::max();
    float fleet_front=std::numeric_limits<float>::lowest();
    for(const auto& instance:state.document.instances) {
        if(instance.id==belt::camera) continue;
        const auto radius=bounds.at(instance.blueprint)*instance.transform.scale;
        if(belt::is_rock(instance))
            belt_exit=std::min(belt_exit,instance.transform.position.z-radius);
        else if(instance.id!=belt::hero)
            fleet_front=std::max(fleet_front,instance.transform.position.z+radius);
    }
    INFO("open-space gap between belt and fleet "<<belt_exit-fleet_front);
    CHECK(belt_exit-fleet_front>60.F);
}

TEST_CASE("Expanded belt and armada fit their bounds and the pathfinder stops far short",
          "[asteroid][scene]") {
    const auto state=scene();
    const auto& hero=*project::find_instance(state,belt::hero);
    std::size_t rocks{},ships{};
    float largest_rock{},smallest_ship=100,largest_ship{},nearest_fleet=1000;
    Vec3 minimum{1000,1000,1000},maximum{-1000,-1000,-1000};
    for(const auto& instance:state.document.instances) {
        if(instance.id==belt::camera) continue;
        const auto& transform=instance.transform;
        if(belt::is_rock(instance)) {
            ++rocks;
            largest_rock=std::max(largest_rock,transform.scale);
            for(unsigned axis=0;axis<3;++axis) {
                minimum[axis]=std::min(minimum[axis],transform.position[axis]);
                maximum[axis]=std::max(maximum[axis],transform.position[axis]);
            }
        } else if(instance.id!=belt::hero) {
            ++ships;
            smallest_ship=std::min(smallest_ship,transform.scale);
            largest_ship=std::max(largest_ship,transform.scale);
        }
        const auto* mesh=project::instance_mesh(state,instance.id);
        float radius{};
        for(u32 vertex=0;vertex<mesh->size();++vertex)
            radius=std::max(radius,length(mesh->position(vertex))*transform.scale);
        for(float time:{0.F,belt::stop_time,belt::duration}) {
            const auto value=project::evaluate_instance(state,instance,time);
            const auto pose=project::evaluate_camera(state,time);
            const auto eye=project::camera(pose,project::ViewMode::scene).position();
            // In particular, distant ships must already be inside the far
            // plane at the start; clipping must not manufacture the reveal.
            CHECK(length(sub(value.transform.position,eye))+radius<std::max(200.F,pose.distance*4));
            for(unsigned axis=0;axis<3;++axis) {
                CHECK(value.transform.position[axis]-radius>state.document.world_bounds.minimum[axis]);
                CHECK(value.transform.position[axis]+radius<state.document.world_bounds.maximum[axis]);
            }
        }
        if(instance.id!=belt::hero && !belt::is_rock(instance))
            for(float time=0;time<=belt::duration;time+=.25F)
                nearest_fleet=std::min(nearest_fleet,
                    length(sub(project::evaluate_instance(state,hero,time).transform.position,
                               project::evaluate_instance(state,instance,time).transform.position))-radius);
    }
    CHECK(rocks==belt::rock_count); CHECK(ships==belt::fleet_count);
    CHECK(largest_rock<=.92F);
    CHECK(maximum.x-minimum.x>165); CHECK(maximum.y-minimum.y>80); CHECK(maximum.z-minimum.z>145);
    CHECK(largest_ship/smallest_ship>12);
    INFO("Nearest fleet hull remains "<<nearest_fleet<<" units from the pathfinder");
    CHECK(nearest_fleet>65);
    const auto position=[&](float time) { return project::evaluate_instance(state,hero,time).transform.position; };
    for(float time=belt::stop_time;time<=belt::duration;time+=.125F)
        CHECK(position(time)==position(belt::stop_time));
    float speed=length(sub(position(35),position(34)));
    for(float time=36;time<=belt::stop_time;++time) {
        const auto next=length(sub(position(time),position(time-1)));
        CHECK(next<speed); speed=next;
    }
    CHECK(speed<.4F);
    CHECK(project::evaluate_camera(state,belt::stop_time)==project::evaluate_camera(state,belt::duration));
}

TEST_CASE("The tracking camera keeps the gentler flight fully framed after entry", "[asteroid][scene]") {
    auto state=scene();
    state.viewport.mode=project::ViewMode::scene;
    state.viewport.selected_object=belt::hero;
    state.viewport.pilot_camera=true;
    for(float time=0;time<=belt::duration;time+=.5F) {
        state.viewport.time=time;
        const auto vertices=project::project_vertices(state,{1280,800});
        INFO("framing at "<<time<<" seconds");
        REQUIRE_FALSE(vertices.empty());
        CHECK(std::ranges::all_of(vertices,[](const auto& p) {
            return p && p->x>.02F && p->x<.98F && p->y>.02F && p->y<.98F;
        }));
        float left=1, right=0;
        for(const auto& p:vertices) if(p) { left=std::min(left,p->x); right=std::max(right,p->x); }
        CHECK(right-left>.1F);
        const auto eye=project::camera(project::evaluate_camera(state,time),project::ViewMode::scene).position();
        const auto ship=project::evaluate_instance(state,*project::find_instance(state,belt::hero),time);
        CHECK(length(sub(eye,ship.transform.position))<12.F);
    }
}

TEST_CASE("Belt rocks occupy a deep volume without overlapping into a wall", "[asteroid][scene]") {
    const auto state=scene();
    std::map<project::BlueprintId,float> bounds;
    std::vector<std::pair<Vec3,float>> rocks;
    std::array<unsigned,8> depth_bins{};
    for(const auto& instance:state.document.instances) {
        if(!belt::is_rock(instance)) continue;
        if(!bounds.contains(instance.blueprint)) {
            const auto* mesh=project::instance_mesh(state,instance.id);
            for(u32 vertex=0;vertex<mesh->size();++vertex)
                bounds[instance.blueprint]=std::max(bounds[instance.blueprint],length(mesh->position(vertex)));
        }
        rocks.emplace_back(instance.transform.position,bounds.at(instance.blueprint)*instance.transform.scale);
        const auto bin=std::clamp(static_cast<int>((instance.transform.position.z+64.F)/154.F*8),0,7);
        ++depth_bins[bin];
    }
    float clearance=1000;
    for(std::size_t i=0;i<rocks.size();++i)
        for(std::size_t j=i+1;j<rocks.size();++j)
            clearance=std::min(clearance,length(sub(rocks[i].first,rocks[j].first))-rocks[i].second-rocks[j].second);
    CHECK(clearance>.59F);
    for(auto count:depth_bins) CHECK(count>35);
}
