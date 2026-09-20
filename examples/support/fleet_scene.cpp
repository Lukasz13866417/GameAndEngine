#include "fleet_scene.hpp"
#include "../editor/animation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <tuple>

namespace example::fleet {
namespace {
using namespace vng;
namespace project = editor_example;
constexpr f32 degrees_per_radian = 180.F / std::numbers::pi_v<f32>;
Vec3 add(Vec3 a, Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec3 mul(Vec3 v, f32 s) { return {v.x*s,v.y*s,v.z*s}; }
f32 length(Vec3 v) { return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z); }
struct Knot { f32 time; Vec3 value; };
// Time-aware cubic Hermite handles keep the tracking shot continuous. Bake
// once to editable linear keys; the engine timeline remains backend neutral.
template<std::size_t N>
Vec3 curve(const std::array<Knot,N>& knots, f32 time) {
    if (time <= knots.front().time) return knots.front().value;
    if (time >= knots.back().time) return knots.back().value;
    std::size_t i = 0;
    while (time > knots[i+1].time) ++i;
    const auto dt = knots[i+1].time-knots[i].time;
    const auto u = (time-knots[i].time)/dt, u2 = u*u, u3 = u2*u;
    const auto tangent = [&](std::size_t j) {
        const auto a = j ? j-1 : j, b = std::min(j+1,N-1);
        return mul(sub(knots[b].value,knots[a].value), 1.F/(knots[b].time-knots[a].time));
    };
    return add(add(mul(knots[i].value,2*u3-3*u2+1),mul(tangent(i),(u3-2*u2+u)*dt)),
               add(mul(knots[i+1].value,-2*u3+3*u2),mul(tangent(i+1),(u3-u2)*dt)));
}
constexpr std::array ship_path{
    Knot{0,{2,-4,24}}, Knot{3,{4,-1,13}}, Knot{7,{9,.7F,1}},
    Knot{11,{12.8F,1.4F,-10}}, Knot{15,{12.5F,1.8F,-24}},
    Knot{20,{8,1.5F,-34}}, Knot{26,{4.5F,.5F,-42}}, Knot{34,{2,.5F,-51}}};
constexpr std::array eye_path{
    Knot{0,{0,3.3F,22}}, Knot{3,{.2F,3.5F,20}}, Knot{7,{4.5F,4.8F,12}},
    Knot{11,{8,5.8F,1}}, Knot{15,{13,6,-9}}, Knot{20,{12,7,-19}},
    Knot{26,{8,7,-26}}, Knot{34,{6,6,-32}}};
constexpr std::array look_path{
    Knot{0,{4,0,-1}}, Knot{3,{4,-.3F,10}}, Knot{7,{10,1.2F,-5}},
    Knot{11,{12.5F,2,-18}}, Knot{15,{9,2,-34}}, Knot{20,{2,2,-59}},
    Knot{26,{-1,1,-70}}, Knot{34,{-2,1,-76}}};

project::CameraPose camera_at(f32 time) {
    const auto target = curve(look_path,time), delta = sub(curve(eye_path,time),target);
    const auto distance = length(delta);
    return {std::atan2(delta.x,delta.z)*degrees_per_radian,
        std::asin(delta.y/distance)*degrees_per_radian,distance,target};
}
Vec3 orientation_at(f32 time) {
    auto forward = sub(curve(ship_path,std::min(time+.05F,duration)),
                       curve(ship_path,std::max(time-.05F,0.F)));
    forward = mul(forward,1.F/length(forward));
    const auto bank = -16.F * std::sin(std::clamp((time-3.F)/17.F,0.F,1.F)*std::numbers::pi_v<f32>);
    // Small bank about screen-Z reads naturally as the Kestrel turns the limb.
    return {std::asin(forward.y)*degrees_per_radian,
        std::atan2(-forward.x,-forward.z)*degrees_per_radian,bank};
}
void checked(content::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}
} // namespace

content::Result<project::State> author_scene(const std::filesystem::path& assets) {
    auto lead = editor::EditableMesh::load(assets/"spaceship.vmesh");
    if (!lead) return std::unexpected(lead.error());
    project::State state{.document = {.mesh = std::move(*lead)}};
    state.document.timeline_duration = duration;
    state.document.environment = {.stars = 6000, .star_seed = 73, .exposure = .9F,
        .bloom_threshold = 2.2F, .bloom_strength = .18F};
    state.document.instances = {
        {hero,project::BlueprintId::mesh,"KESTREL / pathfinder",project::MeshSettings{},
            {ship_path.front().value,orientation_at(0),.20F}},
        {sun,project::BlueprintId::sun,"HELIOS / occulting star",
            project::SunSettings{.radius=3,.displacement=1,.bloom=.20F,.white_spots=false},
            {{0,0,-12},{},3}}};
    state.document.next_instance_id = 3;
    for (const auto& [id,name,file] : std::array{
             std::tuple{3U,"BASTION / capital carrier","fleet_carrier.vmesh"},
             std::tuple{4U,"LANCER / frigate","fleet_frigate.vmesh"},
             std::tuple{5U,"MANTA / escort","fleet_escort.vmesh"}}) {
        auto mesh = editor::EditableMesh::load(assets/file);
        if (!mesh) return std::unexpected(mesh.error());
        state.document.mesh_assets.push_back({static_cast<project::BlueprintId>(id),name,std::move(*mesh),{}});
    }
    state.document.next_blueprint_id = 6;
    struct Vessel { u32 blueprint; const char* name; Vec3 position; f32 scale,yaw,roll; };
    constexpr std::array formation{
        Vessel{3,"BASTION / flagship",{-1,1,-72},1.15F,-38,-2},
        Vessel{3,"BASTION / rear carrier",{-13,4,-86},.80F,-28,4},
        Vessel{4,"LANCER / port screen",{-14,-1,-62},.55F,-18,3},
        Vessel{4,"LANCER / upper wing",{-10,6,-78},.55F,-27,-4},
        Vessel{4,"LANCER / starboard screen",{8,-3,-64},.60F,-8,3},
        Vessel{4,"LANCER / high guard",{11,4,-82},.55F,-22,1},
        Vessel{4,"LANCER / rear guard",{0,-5,-90},.55F,-6,-3},
        Vessel{5,"MANTA / forward port",{-8,3,-54},.34F,-15,5},
        Vessel{5,"MANTA / forward starboard",{6,-4,-58},.36F,-9,-4},
        Vessel{5,"MANTA / outer picket",{17,1,-70},.38F,-14,3},
        Vessel{5,"MANTA / port picket",{-19,2,-85},.35F,-11,-2},
        Vessel{5,"MANTA / upper picket",{4,8,-91},.35F,-5,-6},
        Vessel{1,"KESTREL / rendezvous alpha",{-6,-3,-49},.18F,-14,9},
        Vessel{1,"KESTREL / rendezvous beta",{4,2,-52},.18F,-9,-5},
        Vessel{1,"KESTREL / distant wing",{13,5,-62},.19F,-12,4},
        Vessel{1,"KESTREL / port wing",{-13,-5,-70},.20F,-8,-3}};
    for (const auto& vessel : formation) {
        const auto id = state.document.next_instance_id++;
        state.document.instances.push_back({id,static_cast<project::BlueprintId>(vessel.blueprint),
            vessel.name,project::MeshSettings{},
            {vessel.position,{0,vessel.yaw,vessel.roll},vessel.scale}});
    }
    state.document.animation_camera = camera_at(0);
    state.viewport.editor_camera = state.document.animation_camera;
    state.viewport.time = 0;
    state.viewport.selected_object = hero;
    try {
        // One-second baked samples: curved choreography with regular, editable
        // timestamps. No C++ callbacks survive in the saved scene.
        for (int second = 0; second <= static_cast<int>(duration); ++second) {
            const auto t = static_cast<f32>(second);
            const auto pose = camera_at(t);
            const auto mode = second ? timeline::Interpolation::linear : timeline::Interpolation::hold;
            for (const auto& [property,value] : std::array<std::pair<std::string_view,timeline::Value>,4>{{
                {"yaw",pose.yaw}, {"pitch",pose.pitch},{"distance",pose.distance},{"target",pose.target}}})
                checked(project::key_property(state,{project::camera_animation_object,std::string(property)},t,value,mode));
            checked(project::key_property(state,{hero,"position"},t,curve(ship_path,t),mode));
            checked(project::key_property(state,{hero,"rotation"},t,orientation_at(t),mode));
        }
        // A slow common forward drift preserves formation but prevents the
        // reveal from feeling like static scenery. Every ship stays present.
        for (const auto& instance : state.document.instances) {
            if (instance.id <= sun) continue;
            checked(project::key_property(state,{instance.id,"position"},0,instance.transform.position,
                                           timeline::Interpolation::hold));
            checked(project::key_property(state,{instance.id,"position"},duration,
                add(instance.transform.position,{1.5F,0,-4}),timeline::Interpolation::linear));
        }
        state.document.keyframe_names = {{0,"01 / Helios hides the fleet"},
            {3,"02 / Pathfinder enters"},{7,"03 / Approach the limb"},
            {11,"04 / First fleet silhouettes"},{15,"05 / Open the formation"},
            {20,"06 / The armada"},{26,"07 / Rendezvous"},{34,"08 / Join the formation"}};
        checked(project::validate_animation(state));
    } catch (const std::exception& error) {
        content::Diagnostic diagnostic;
        diagnostic.message = error.what();
        return std::unexpected(std::move(diagnostic));
    }
    return state;
}
} // namespace example::fleet
