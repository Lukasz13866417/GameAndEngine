#include "asteroid_scene.hpp"
#include "asteroid_assets.hpp"
#include "fleet_scene.hpp"
#include "../editor/animation.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace example::asteroids {
namespace {
using namespace vng;
namespace project=editor_example;
Vec3 add(Vec3 a,Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 sub(Vec3 a,Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec3 mul(Vec3 p,f32 s) { return {p.x*s,p.y*s,p.z*s}; }
f32 length(Vec3 p) { return std::sqrt(p.x*p.x+p.y*p.y+p.z*p.z); }
constexpr f32 degrees=180.F/std::numbers::pi_v<f32>;
// Keep the existing entrance and instance identities stable for editor tools.
constexpr Vec3 shot_origin{0,0,30};
struct Knot { f32 time; Vec3 value; };
template<std::size_t N> Vec3 curve(const std::array<Knot,N>& knots,f32 time) {
    if(time<=knots.front().time) return knots.front().value;
    if(time>=knots.back().time) return knots.back().value;
    std::size_t i{};
    while(time>knots[i+1].time) ++i;
    const auto dt=knots[i+1].time-knots[i].time,u=(time-knots[i].time)/dt,u2=u*u,u3=u2*u;
    const auto tangent=[&](std::size_t j) {
        if(j==N-1) return Vec3{}; // Ease into the final held pose, without overshoot.
        const auto a=j?j-1:j,b=std::min(j+1,N-1);
        return mul(sub(knots[b].value,knots[a].value),1.F/(knots[b].time-knots[a].time));
    };
    return add(add(mul(knots[i].value,2*u3-3*u2+1),mul(tangent(i),(u3-2*u2+u)*dt)),
               add(mul(knots[i+1].value,-2*u3+3*u2),mul(tangent(i+1),(u3-u2)*dt)));
}
constexpr std::array flight{
    // Enter already aligned with the gap, then take one broad turn toward the
    // distant fleet. Avoid the old short, sharp sideways entry and exit hooks.
    Knot{0,{3,-3,68}},Knot{3,{5,-1,54}},Knot{10,{10,1,18}},Knot{18,{16,2,-25}},
    Knot{26,{18,2,-68}},Knot{34,{15,1,-104}},Knot{stop_time,{12,1,-121}}};
constexpr std::array follow_offsets{
    Knot{0,{-.5F,2,8.5F}},Knot{10,{1.5F,2,9}},
    Knot{26,{2.5F,2.2F,9}},Knot{stop_time,{3.5F,2.5F,10}}};
constexpr std::array look_offsets{
    Knot{0,{0,.6F,-2}},Knot{26,{-1,.6F,-3}},Knot{stop_time,{-1,.7F,-4}}};
Vec3 eye_at(f32 time) { return add(curve(flight,time),curve(follow_offsets,time)); }
project::CameraPose camera_at(f32 time) {
    const auto eye=eye_at(time),target=add(curve(flight,time),curve(look_offsets,time)),delta=sub(eye,target);
    const auto distance=length(delta);
    // Keep the same eye/direction while focusing beyond the belt. The camera's
    // distance-derived far plane includes the entire fleet from the first frame:
    // the reveal must come from occlusion/framing, not clipping distant ships.
    const auto focus_distance=std::max(150.F,distance);
    return {std::atan2(delta.x,delta.z)*degrees,std::asin(delta.y/distance)*degrees,
        focus_distance,add(sub(eye,mul(delta,focus_distance/distance)),shot_origin)};
}
Vec3 facing(f32 time) {
    time=std::min(time,stop_time-.05F); // Retain heading once velocity reaches zero.
    auto direction=sub(curve(flight,std::min(time+.05F,duration)),curve(flight,std::max(time-.05F,0.F)));
    direction=mul(direction,1.F/length(direction));
    return {std::asin(direction.y)*degrees,std::atan2(-direction.x,-direction.z)*degrees,
        -9.F*std::sin(std::clamp(time/30.F,0.F,1.F)*std::numbers::pi_v<f32>)};
}
void checked(content::Result<void> result) {
    if(!result) throw std::runtime_error(result.error().message);
}
}

content::Result<project::State> author_scene(const std::filesystem::path& assets) {
    // Reuse the original fleet identities/blueprints, not its solar animation.
    auto loaded=fleet::author_scene(assets);
    if(!loaded) return std::unexpected(loaded.error());
    auto state=std::move(*loaded);
    checked(project::erase_instance(state,fleet::sun));
    // The fleet's tracking camera is the last fleet identity; drop it and
    // give back its ID so the belt keeps its stable rock identities.
    checked(project::erase_instance(state,fleet::camera));
    state.document.next_instance_id=fleet::camera;
    state.document.timeline={};
    state.document.keyframe_names.clear();
    state.document.timeline_duration=duration;
    state.document.world_bounds={{-180,-120,-420},{180,120,180}};
    state.document.environment={.stars=8000,.star_seed=119,.exposure=1.0F,
        .bloom_threshold=2.2F,.bloom_strength=.18F};
    for(auto& instance:state.document.instances) {
        if(instance.id==hero) instance.transform={add(flight.front().value,shot_origin),facing(0),.20F};
        else {
            instance.transform.position.x=instance.transform.position.x*1.6F-8;
            instance.transform.position.y*=2.2F;
            instance.transform.position.z+=shot_origin.z-180;
            // Preserve named vessels, but make the capital/escort hierarchy clearer.
            instance.transform.scale*=instance.blueprint==static_cast<project::BlueprintId>(3) ? 1.6F :
                instance.blueprint==static_cast<project::BlueprintId>(4) ? 1.15F : .85F;
        }
    }
    std::array<f32,3> bounds{};
    for(u32 type=0;type<3;++type) {
        auto mesh=editor::EditableMesh::create(rock_mesh(type));
        if(!mesh) return std::unexpected(mesh.error());
        for(u32 i=0;i<mesh->size();++i) bounds[type]=std::max(bounds[type],length(mesh->position(i)));
        state.document.mesh_assets.push_back({static_cast<project::BlueprintId>(6+type),
            std::array{"BASALT / cratered gate","IRON / fractured slab","REGOLITH / rubble"}[type],std::move(*mesh),{}});
    }
    state.document.next_blueprint_id=9;
    struct Rock { Vec3 position; f32 scale; u32 variant; Vec3 rotation; };
    std::vector<Rock> rocks{
        Rock{{-11,8,42},.65F,0,{0,0,0}},Rock{{27,-9,-78},.8F,0,{0,20,13}}};
    const auto clear_corridor=[&](const Rock& rock) {
        const auto radius=bounds[rock.variant]*rock.scale;
        for(f32 time=0;time<=duration;time+=.25F)
            if(length(sub(rock.position,eye_at(time)))<radius+2.F ||
               length(sub(rock.position,curve(flight,time)))<radius+3.2F) return false;
        return true;
    };
    const auto add_rock=[&](const Rock& rock) {
        const auto id=state.document.next_instance_id++;
        state.document.instances.push_back({id,static_cast<project::BlueprintId>(6+rock.variant),
            (id==first_rock?"BELT / entry marker":id==first_rock+1?"BELT / outer fragment":
             "BELT / fragment "+std::to_string(id-first_rock)),project::MeshSettings{},
            {add(rock.position,shot_origin),rock.rotation,rock.scale}});
    };
    u32 random_state=2194;
    const auto random=[&]() {
        random_state=random_state*1664525U+1013904223U;
        return static_cast<f32>(random_state>>8)/16777216.F;
    };
    // A volume, not a screen: independent coordinates throughout 154 units
    // of depth, a denser core and a wide sparse outskirts. Separation remains
    // valid while rocks tumble, so no overlapping slab can form.
    for(u32 attempt=0;attempt<20000 && rocks.size()<rock_count;++attempt) {
        const bool outer=rocks.size()%4==0;
        Rock rock{{outer ? -90.F+180.F*random() : -38.F+76.F*random(),
                   outer ? -45.F+90.F*random() : -26.F+52.F*random(),60.F-154.F*random()},
            .12F+.80F*std::pow(random(),1.7F),1+static_cast<u32>(random()*2),
            {random()*100.F,random()*100.F,random()*100.F}};
        const auto radius=bounds[rock.variant]*rock.scale;
        bool safe=clear_corridor(rock);
        for(const auto& other:rocks)
            if(length(sub(rock.position,other.position))<radius+bounds[other.variant]*other.scale+.6F) { safe=false; break; }
        if(safe) rocks.push_back(rock);
    }
    if(rocks.size()!=rock_count) {
        content::Diagnostic error; error.message="Could not scatter the asteroid belt with safe separation";
        return std::unexpected(std::move(error));
    }
    for(const auto& rock:rocks) add_rock(rock);
    // Six additional mixed wings reuse the same four ship blueprints. Append
    // them after the rocks so existing named ships and diagnostic rock IDs stay stable.
    constexpr std::array centers{Vec3{-46,12,-164},Vec3{-16,-9,-175},Vec3{-26,18,-190},
        Vec3{-2,-17,-205},Vec3{-46,3,-210},Vec3{-14,-2,-220}};
    constexpr std::array<u32,4> blueprints{3,4,5,1};
    constexpr std::array names{"BASTION", "LANCER", "MANTA", "KESTREL"};
    for(u32 wing=0;wing<centers.size();++wing) {
        const std::array scales{1.4F+.22F*wing,.48F+.14F*wing,.20F+.10F*wing,.13F+.035F*wing};
        for(u32 slot=0;slot<blueprints.size();++slot) {
            const auto id=state.document.next_instance_id++;
            const auto position=add(centers[wing],{slot%2 ? 9.F : -9.F,slot<2 ? 3.F : -3.F,-7.F*slot});
            state.document.instances.push_back({id,static_cast<project::BlueprintId>(blueprints[slot]),
                std::string(names[slot])+" / distant wing "+std::to_string(wing+1),project::MeshSettings{},
                {add(position,add(shot_origin,{0,0,-85})),{0,-24.F+3.F*wing,slot%2 ? 3.F : -3.F},scales[slot]}});
        }
    }
    auto tracking=project::ensure_camera(state,camera_at(0),"Tracking camera");
    if(!tracking) return std::unexpected(tracking.error());
    try {
        for(int second=0;second<=static_cast<int>(duration);++second) {
            const auto time=static_cast<f32>(second);
            const auto pose=camera_at(time);
            checked(project::key_camera(state,*tracking,time,pose));
            checked(project::key_property(state,{hero,"position"},time,add(curve(flight,time),shot_origin)));
            checked(project::key_property(state,{hero,"rotation"},time,facing(time)));
        }
        for(const auto& instance:state.document.instances) {
            if(instance.id==hero || instance.id==*tracking) continue;
            if(!is_rock(instance)) {
                checked(project::key_property(state,{instance.id,"position"},0,instance.transform.position));
                checked(project::key_property(state,{instance.id,"position"},duration,
                    add(instance.transform.position,{1.5F,0,-4})));
            } else {
                const auto slow=instance.id<first_rock+2 ? .06F : .5F;
                const auto rotation=instance.transform.rotation;
                checked(project::key_property(state,{instance.id,"rotation"},0,rotation));
                checked(project::key_property(state,{instance.id,"rotation"},duration,
                    add(rotation,{duration*slow,duration*slow*.7F,-duration*slow*.4F})));
            }
        }
        state.document.keyframe_names={{0,"01 / Into the reach"},{3,"02 / Close pursuit"},
            {10,"03 / Drifting fragments"},{18,"04 / Deep in the belt"},{26,"05 / Through the far side"},
            {34,"06 / Open space"},{reveal_time,"07 / Distant armada"},
            {stop_time,"08 / All stop - observe the fleet"},{duration,"09 / Hold position"}};
        state.viewport.editor_camera=camera_at(0);
        state.viewport.time=0;
        checked(project::validate_animation(state));
    } catch(const std::exception& error) {
        content::Diagnostic diagnostic;
        diagnostic.message=error.what();
        return std::unexpected(std::move(diagnostic));
    }
    return state;
}
}
