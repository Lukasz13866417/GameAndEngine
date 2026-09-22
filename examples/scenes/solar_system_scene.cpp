#include "solar_system_scene.hpp"
#include "../support/asteroid_assets.hpp"
#include "fleet_scene.hpp"
#include "../editor/animation.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace example::solar_system {
namespace {
using namespace vng;
namespace project=editor_example;
constexpr f32 degrees=180.F/std::numbers::pi_v<f32>;
Vec3 add(Vec3 a,Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 sub(Vec3 a,Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec3 mul(Vec3 p,f32 s) { return {p.x*s,p.y*s,p.z*s}; }
f32 dot(Vec3 a,Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
f32 length(Vec3 p) { return std::sqrt(dot(p,p)); }
Vec3 normalize(Vec3 p) { return mul(p,1.F/length(p)); }
Vec3 cross(Vec3 a,Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
// Instance yaw turns the mesh's -Z heading about +Y; this rotates offsets the same way.
Vec3 rotate_y(Vec3 p,f32 yaw_degrees) {
    const auto r=yaw_degrees/degrees;
    return {p.x*std::cos(r)+p.z*std::sin(r),p.y,-p.x*std::sin(r)+p.z*std::cos(r)};
}
Vec3 facing(Vec3 direction) {
    return {std::asin(direction.y)*degrees,std::atan2(-direction.x,-direction.z)*degrees,0};
}
void checked(content::Result<void> result) {
    if(!result) throw std::runtime_error(result.error().message);
}
// The camera pose that puts the eye at `eye` and its orbit pivot at `target`.
project::CameraPose pose_from(Vec3 target) {
    const auto delta=sub(eye,target);
    const auto distance=length(delta);
    return {std::atan2(delta.x,delta.z)*degrees,std::asin(delta.y/distance)*degrees,distance,target,1};
}
}

Vec3 view_direction() {
    // Mostly away from the sun, turned so Earth's near limb stays inside the frame.
    return normalize({-.55F,.03F,-1});
}
Vec3 sun_position() {
    const auto forward=view_direction(),right=normalize(cross(forward,{0,1,0})),up=cross(right,forward);
    // Behind and above the camera's right shoulder: lights Earth's visible
    // side and the fleet from the front while staying out of the frame.
    return add(add(eye,mul(forward,-380)),add(mul(right,140),mul(up,90)));
}

content::Result<project::State> author_scene(const std::filesystem::path& assets) {
    // The fleet scene supplies the Kestrel, the three capital blueprints and
    // the named formation; its solar choreography is discarded.
    auto loaded=fleet::author_scene(assets);
    if(!loaded) return std::unexpected(loaded.error());
    auto state=std::move(*loaded);
    // The fleet's tracking camera is its last identity; this shot keys its own.
    checked(project::erase_instance(state,fleet::camera));
    state.document.next_instance_id=fleet::camera;
    if(state.document.next_instance_id!=earth || state.document.next_blueprint_id!=static_cast<u32>(earth_blueprint)) {
        content::Diagnostic error; error.message="Fleet scene identities changed; update the solar system layout";
        return std::unexpected(std::move(error));
    }
    state.document.timeline={};
    state.document.keyframe_names.clear();
    state.document.timeline_duration=duration;
    state.document.environment={.stars=7000,.star_seed=311,.exposure=1.F,
        .bloom_threshold=2.2F,.bloom_strength=.16F};
    const auto forward=view_direction(),right=normalize(cross(forward,{0,1,0})),up=cross(right,forward);
    const auto along=[&](f32 distance) { return add(eye,mul(forward,distance)); };
    const auto heading=facing(forward);
    const auto fleet_anchor=along(fleet_distance),belt_center=along(belt_distance);

    auto earth_mesh=editor::EditableMesh::load(assets/"earth.vmesh");
    if(!earth_mesh) return std::unexpected(earth_mesh.error());
    state.document.mesh_assets.push_back({earth_blueprint,"EARTH / stylized homeworld",std::move(*earth_mesh),{}});
    state.document.instances.push_back({earth,earth_blueprint,"Earth",project::MeshSettings{},
        {earth_center,{0,75,-18},earth_radius}});
    state.document.next_instance_id=earth+1;

    constexpr Vec3 formation_anchor{0,0,-70}; // The fleet scene's formation centre.
    for(auto& instance:state.document.instances) {
        if(instance.id==hero) {
            // A little behind the camera, low on its right, already heading out.
            instance.transform={add(add(eye,mul(forward,-5)),add(mul(right,1.6F),mul(up,-1.2F))),heading,.20F};
        } else if(instance.id==sun) {
            instance.name="HELIOS / home star";
            instance.settings=project::SunSettings{.radius=3,.displacement=1,.bloom=.20F,.white_spots=false};
            instance.transform={sun_position(),{},14};
        } else if(instance.id>=first_ship && instance.id<first_ship+formation_count) {
            const auto offset=mul(sub(instance.transform.position,formation_anchor),1.5F);
            instance.transform.position=add(fleet_anchor,rotate_y(offset,heading.y));
            instance.transform.rotation.y+=heading.y;
            instance.transform.scale*=1.5F;
        }
    }
    // Six mixed wings around the formation reuse the same blueprints. They
    // straddle the anchor so the fleet's centre stays at fleet_distance.
    constexpr std::array centers{Vec3{-38,12,12},Vec3{-8,-9,7},Vec3{-18,18,0},
        Vec3{6,-17,-8},Vec3{-38,3,-10},Vec3{-6,-2,-16}};
    constexpr std::array<u32,4> blueprints{3,4,5,1};
    constexpr std::array names{"BASTION","LANCER","MANTA","KESTREL"};
    for(u32 wing=0;wing<centers.size();++wing) {
        const auto w=static_cast<f32>(wing);
        const std::array scales{1.4F+.22F*w,.48F+.14F*w,.20F+.10F*w,.13F+.035F*w};
        for(u32 slot=0;slot<blueprints.size();++slot) {
            const auto id=state.document.next_instance_id++;
            const auto offset=add(centers[wing],{slot%2 ? 9.F : -9.F,slot<2 ? 3.F : -3.F,-7.F*static_cast<f32>(slot)});
            state.document.instances.push_back({id,static_cast<project::BlueprintId>(blueprints[slot]),
                std::string(names[slot])+" / distant wing "+std::to_string(wing+1),project::MeshSettings{},
                {add(fleet_anchor,rotate_y(offset,heading.y)),{0,heading.y-24.F+3.F*w,slot%2 ? 3.F : -3.F},scales[slot]}});
        }
    }
    if(state.document.next_instance_id!=first_rock) {
        content::Diagnostic error; error.message="Ship identities do not end where the belt begins";
        return std::unexpected(std::move(error));
    }

    std::array<f32,3> bounds{};
    for(u32 type=0;type<3;++type) {
        auto mesh=editor::EditableMesh::create(asteroids::rock_mesh(type));
        if(!mesh) return std::unexpected(mesh.error());
        for(u32 i=0;i<mesh->size();++i) bounds[type]=std::max(bounds[type],length(mesh->position(i)));
        state.document.mesh_assets.push_back({static_cast<project::BlueprintId>(static_cast<u32>(first_rock_blueprint)+type),
            std::array{"BASALT / cratered gate","IRON / fractured slab","REGOLITH / rubble"}[type],std::move(*mesh),{}});
    }
    state.document.next_blueprint_id=static_cast<u32>(first_rock_blueprint)+3;
    // Far enough to read as a belt, large enough to stay visible: a dense core
    // and sparse outskirts spread across the view, all behind the fleet.
    struct Rock { Vec3 position; f32 scale; u32 variant; Vec3 rotation; };
    std::vector<Rock> rocks;
    u32 random_state=4871;
    const auto random=[&]() {
        random_state=random_state*1664525U+1013904223U;
        return static_cast<f32>(random_state>>8)/16777216.F;
    };
    for(u32 attempt=0;attempt<40000 && rocks.size()<rock_count;++attempt) {
        const bool outer=rocks.size()%4==0;
        const auto lateral=outer ? -150.F+300.F*random() : -70.F+140.F*random();
        const auto vertical=outer ? -75.F+150.F*random() : -35.F+70.F*random();
        const auto depth=-40.F+80.F*random();
        Rock rock{add(belt_center,add(add(mul(right,lateral),mul(up,vertical)),mul(forward,depth))),
            (.12F+.80F*std::pow(random(),1.7F))*2.2F,1+static_cast<u32>(random()*2),
            {random()*100.F,random()*100.F,random()*100.F}};
        const auto radius=bounds[rock.variant]*rock.scale;
        bool safe=true;
        for(const auto& other:rocks)
            if(length(sub(rock.position,other.position))<radius+bounds[other.variant]*other.scale+1.F) { safe=false; break; }
        for(const auto& instance:state.document.instances)
            if(safe && instance.id>=first_ship && length(sub(rock.position,instance.transform.position))<radius+12.F) safe=false;
        if(safe) rocks.push_back(rock);
    }
    if(rocks.size()!=rock_count) {
        content::Diagnostic error; error.message="Could not scatter the asteroid belt with safe separation";
        return std::unexpected(std::move(error));
    }
    for(const auto& rock:rocks) {
        const auto id=state.document.next_instance_id++;
        state.document.instances.push_back({id,static_cast<project::BlueprintId>(static_cast<u32>(first_rock_blueprint)+rock.variant),
            "BELT / fragment "+std::to_string(id-first_rock+1),project::MeshSettings{},{rock.position,rock.rotation,rock.scale}});
    }

    project::WorldBounds world{{1e9F,1e9F,1e9F},{-1e9F,-1e9F,-1e9F}};
    for(const auto& instance:state.document.instances) {
        const auto margin=instance.id==sun ? 3.F*instance.transform.scale : instance.id==earth ? earth_radius : 12.F;
        for(unsigned i=0;i<3;++i) {
            world.minimum[i]=std::min(world.minimum[i],instance.transform.position[i]-margin);
            world.maximum[i]=std::max(world.maximum[i],instance.transform.position[i]+margin);
        }
    }
    state.document.world_bounds=world;

    try {
        // Only the starting keyframe: the orbit pivot is the fleet so the
        // distance-derived far plane keeps the belt behind it in view.
        const auto pose=pose_from(fleet_anchor);
        auto shot_camera=project::ensure_camera(state,pose,"Departure camera");
        if(!shot_camera) throw std::runtime_error(shot_camera.error().message);
        if(*shot_camera!=camera) throw std::runtime_error("Solar system camera identity changed");
        checked(project::key_camera(state,*shot_camera,0,pose));
        const auto* kestrel=project::find_instance(state,hero);
        checked(project::key_property(state,{hero,"position"},0,kestrel->transform.position));
        checked(project::key_property(state,{hero,"rotation"},0,kestrel->transform.rotation));
        state.document.keyframe_names={{0,"01 / Leaving home"}};
        state.viewport.mode=project::ViewMode::scene;
        state.viewport.editor_camera=pose;
        state.viewport.time=0;
        state.viewport.selected_object=hero;
        state.viewport.inspected_mesh=project::BlueprintId::mesh;
        checked(project::validate_animation(state));
    } catch(const std::exception& error) {
        content::Diagnostic diagnostic;
        diagnostic.message=error.what();
        return std::unexpected(std::move(diagnostic));
    }
    return state;
}
}
