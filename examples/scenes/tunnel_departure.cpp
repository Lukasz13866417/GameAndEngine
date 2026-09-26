#include "tunnel_departure.hpp"
#include "../editor/animation.hpp"
#include "../support/earth_infrastructure.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace example::tunnel::departure {
namespace {
using namespace vng;
namespace project=editor_example;
namespace vm=content::vmesh;
constexpr f32 radians=std::numbers::pi_v<f32>/180;
Vec3 add(Vec3 a,Vec3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
Vec3 sub(Vec3 a,Vec3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
Vec3 mul(Vec3 a,f32 s){return {a.x*s,a.y*s,a.z*s};}
f32 length(Vec3 a){return std::hypot(a.x,a.y,a.z);}
Vec3 unit(Vec3 a){return mul(a,1/length(a));}
Vec3 cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
Vec3 mix(Vec3 a,Vec3 b,f32 u){return add(mul(a,1-u),mul(b,u));}
void checked(content::Result<void> value){if(!value)throw std::runtime_error(value.error().message);}
template<class T>T take(content::Result<T> value){if(!value)throw std::runtime_error(value.error().message);return std::move(*value);}
Vec3 heading(Vec3 direction) {
    const auto d=unit(direction);
    return {std::asin(d.y)/radians,std::atan2(-d.x,-d.z)/radians,0};
}
editor::EditableMesh material(vm::Document mesh) {
    mesh.metadata["render/lighting"]="tunnel_departure";
    return take(editor::EditableMesh::create(std::move(mesh)));
}
editor::EditableMesh vessel(const std::filesystem::path& path,f32 size) {
    auto mesh=take(editor::EditableMesh::load(path)).document();
    for(auto& field:mesh.vertex_fields)
        if(field.name=="position")for(auto& x:std::get<std::vector<f32>>(field.values))x*=size;
    mesh.metadata["units"]="kilometres";
    return material(std::move(mesh));
}
// Shot offsets and look targets are relative to the courier.
struct Shot {Vec3 offset,target;f32 zoom;};
// Flight samples the same authored recipe as the Earth mesh, including its
// terminal. It is not a second tunnel hidden behind a matching diameter.
struct Route {
    earth::InfrastructurePart part;
    earth::TunnelCurve curve;
    earth::TunnelArc arc;
    Mat4 frame;
    f32 throat;
    std::vector<std::array<double,3>> controls;
    Route(earth::InfrastructurePart p,std::span<const earth::InfrastructurePart> parts,earth::InfrastructureSettings settings,Mat4 authoring=Mat4::identity())
        :part(std::move(p)),curve(take(earth::TunnelCurve::create(part,parts,settings))),arc(curve),
        frame(mesh_frame::compose(earth::express_route::earth_to_world(),authoring)),throat(arc.distance(earth::tunnel_body_range(part,curve).y)) {
        if(part.bezier_controls) {
            auto points=*part.bezier_controls;points.insert(points.begin(),curve.sample(0).position);points.push_back(curve.sample(1).position);
            for(auto p:points) {
                std::array<double,3> q{};
                for(unsigned r=0;r<3;++r){q[r]=frame[3][r];for(unsigned c=0;c<3;++c)q[r]+=double(frame[c][r])*p[c];}
                controls.push_back(q);
            }
        }
    }
    Vec3 sample_world(f32 t) const {
        if(controls.empty())return mesh_frame::point(frame,curve.sample(t).position);
        std::array<std::array<double,3>,16> points{};std::copy(controls.begin(),controls.end(),points.begin());
        for(auto n=controls.size();n>1;--n)for(std::size_t i=0;i+1<n;++i)for(unsigned c=0;c<3;++c)
            points[i][c]=std::lerp(points[i][c],points[i+1][c],double(t));
        return {f32(points[0][0]),f32(points[0][1]),f32(points[0][2])};
    }
    Vec3 terminal_at(f32 t) const {
        const auto begin=arc.parameter(throat);
        auto p=sample_world(begin+(1-begin)*t);
        return add(p,mul(unit(sub(p,earth_center)),(arc.length()-throat)*earth_radius*earth::tunnel_terminal_rise(part)*t*t));
    }
    // Continue tangentially around Earth. A modest initial climb eases into a
    // nearly constant orbital altitude, instead of an unrelated upward burn.
    std::pair<Vec3,Vec3> departure(f32 distance) const {
        const auto mouth=terminal_at(1),radial=sub(mouth,earth_center);
        const auto radius=length(radial);
        const auto up=unit(radial);
        const auto tangent=unit(sub(mouth,terminal_at(.999F)));
        const auto rise=earth::placement::dot(tangent,up);
        const auto forward=unit(sub(tangent,mul(up,rise)));
        const auto slope=rise/std::sqrt(1-rise*rise);
        const auto decay=std::exp(-distance/300.F),angle=distance/radius;
        const auto outward=add(mul(up,std::cos(angle)),mul(forward,std::sin(angle)));
        const auto along=add(mul(up,-std::sin(angle)),mul(forward,std::cos(angle)));
        const auto height=radius+300.F*slope*(1-decay);
        return {add(earth_center,mul(outward,height)),add(mul(along,height/radius),mul(outward,slope*decay))};
    }
    Vec3 at(f32 time) const {
        const auto t=std::clamp(time,0.F,duration),remaining=start_distance-travel(t);
        if(remaining>=0)return sample_world(arc.parameter(throat-remaining/earth_radius));
        if(remaining>=-12)return terminal_at(-remaining/12);
        return departure(-remaining-12).first;
    }
    Vec3 velocity_at(f32 t) const {
        const auto remaining=start_distance-travel(t),v=speed(t);
        if(t>exit_time)return mul(departure(-remaining-12).second,v);
        if(remaining>=0)return mul(unit(sub(at(std::min(throat_time,t+.02F)),at(std::max(0.F,t-.02F)))),v);
        const auto u=std::clamp(-remaining/12,0.F,1.F);
        const auto tangent=unit(sub(terminal_at(std::min(1.F,u+.001F)),terminal_at(std::max(0.F,u-.001F))));
        auto result=mul(tangent,v/std::max(.1F,-tangent.z));
        return result;
    }
};
const Route& default_route() {
    static const auto p=earth::express_route::recipe(1);
    static const Route route(p,std::span{&p,1},earth::InfrastructureSettings{});
    return route;
}
// The opening camera's frame at the courier's pass station. Offsets are
// kilometres right of, above and ahead of the courier's line there.
struct Station {
    Vec3 origin,right,up,forward;
    Vec3 along(Vec3 o) const {return add(add(mul(right,o.x),mul(up,o.y)),mul(forward,o.z));}
    Vec3 at(Vec3 o) const {return add(origin,along(o));}
};
Station station(const Route& route) {
    const auto forward=unit(route.velocity_at(courier_arrival));
    const auto right=unit(cross(forward,{0,1,0}));
    return {route.at(courier_arrival),right,cross(right,forward),forward};
}
// The opening: a camera waits in the empty bore just above the courier's
// line, looking down it. It drifts a little, rising as it turns, and
// settles; traffic comes in from the edges; then the courier, far faster,
// dives in under the camera and away down the middle, past them all.
// Offsets are right of, above and ahead of the courier's line.
constexpr Vec3 opening_eye{.12F,-.01F,0},settled_eye{.12F,.14F,0};
constexpr Vec3 opening_look{-.078F,-.04F,1},settled_look{0,-.02F,1};
// Craft that come in during the opening pass the camera's station at `passes`.
struct Arrival {unsigned type;f32 passes,speed,right,up,scale;};
constexpr std::array<Arrival,3> arrivals{{
    {0,-1.6F,.20F,-.01F,.44F,1.2F}, // a heavy transport, high over the lanes
    {1,1.3F,.30F,.45F,-.02F,1},     // a patrol, to the right
    {2,1.47F,.30F,-.35F,-.12F,.9F}}}; // a shuttle, low to the left
Look opening_at(f32 t,const Route& route) {
    const auto frame=station(route);
    // The little move belongs to the empty beat: already drifting at the
    // first frame, it eases out and settles before the courier arrives.
    const auto u=1-std::pow(1-std::clamp(t/5.F,0.F,1.F),3.F);
    const auto eye=frame.at(mix(opening_eye,settled_eye,u));
    const auto look=unit(frame.along(mix(opening_look,settled_look,u)));
    // As the courier goes by, the operator's aim eases toward where it is
    // heading: one move, at rest as the catch-up begins, never a nod back.
    const auto follow=.35F*smooth(std::clamp((t-courier_arrival-.08F)/(catch_up_begin-courier_arrival-.08F),0.F,1.F));
    return {eye,add(eye,mul(unit(mix(look,unit(sub(route.at(catch_up_begin),eye)),follow)),2.F)),1};
}
Look chase_at(f32 t,const Route& route) {
    // The chase, then one eased move closer and to the side for the exit:
    // round the ship, not through it.
    const Shot chase{{.10F,.14F,.48F},{0,.02F,-.05F},1},exit{{.22F,.12F,.28F},{0,.015F,-.025F},1};
    const auto blend=smooth(std::clamp((t-exit_approach)/exit_approach_duration,0.F,1.F));
    const auto from=chase.offset,to=exit.offset;
    const auto yaw=std::lerp(std::atan2(from.x,from.z),std::atan2(to.x,to.z),blend);
    const auto pitch=std::lerp(std::asin(from.y/length(from)),std::asin(to.y/length(to)),blend);
    const auto radius=std::lerp(length(from),length(to),blend);
    const Vec3 offset{radius*std::sin(yaw)*std::cos(pitch),radius*std::sin(pitch),radius*std::cos(yaw)*std::cos(pitch)};
    const auto p=route.at(t);
    return {add(p,mul(offset,camera_distance_scale)),add(p,mul(mix(chase.target,exit.target,blend),camera_distance_scale)),
        std::lerp(chase.zoom,exit.zoom,blend)};
}
Look camera_at(f32 t,const Route& route) {
    if(t<catch_up_begin)return opening_at(t,route);
    const auto chase=chase_at(t,route);
    if(t>=catch_up_end)return chase;
    // A moment after the courier goes by, the camera races after it: from
    // rest it closes kilometres in a second, past the traffic, and eases into
    // the chase at the courier's own speed.
    const auto u=(t-catch_up_begin)/(catch_up_end-catch_up_begin),e=u*u*u*(u*(u*6-15)+10);
    const auto from=opening_at(catch_up_begin,route);
    const auto eye=mix(from.eye,chase.eye,e);
    const auto look=unit(mix(unit(sub(from.target,from.eye)),unit(sub(chase.target,eye)),e));
    return {eye,add(eye,mul(look,std::lerp(length(sub(from.target,from.eye)),length(sub(chase.target,chase.eye)),e))),1};
}
}

vng::Vec3 flight(vng::f32 time) {
    return default_route().at(time);
}
vng::Vec3 velocity(vng::f32 time) {
    return default_route().velocity_at(time);
}

vng::content::Result<editor_example::State> author_scene(const std::filesystem::path& assets) {
    try {
        auto home=take(editor::EditableMesh::load(assets/"earth_future.vmesh")).document();
        const auto parts=take(earth::infrastructure_parts(home));
        const auto found=std::ranges::find(parts,earth::express_route::name,&earth::InfrastructurePart::name);
        if(found==parts.end())throw std::runtime_error("Earth is missing the Arabian express route; run vng_make_earth --redesign-infrastructure first");
        const Route route(*found,parts,take(earth::infrastructure_settings(home)),take(mesh_frame::read(home)));
        home.metadata["render/lighting"]="tunnel_departure_night";
        project::State state{.document={.mesh=take(editor::EditableMesh::load(assets/"colored_cube.vmesh"))}};
        state.document.instances.clear();
        state.document.timeline_duration=duration;
        const auto blueprint=[&](u32 id,std::string name,editor::EditableMesh mesh) {
            state.document.mesh_assets.push_back({static_cast<project::BlueprintId>(id),std::move(name),std::move(mesh),{}});
        };
        blueprint(3,"KESTREL / courier",vessel(assets/"spaceship.vmesh",.010F));
        // The authored Earth asset is the source of truth, including manual
        // geometry edits and tunnel classes. Do not reconstruct it implicitly.
        blueprint(5,"EARTH / connected homeworld",take(editor::EditableMesh::create(std::move(home))));
        blueprint(7,"BASTION / heavy transport",vessel(assets/"fleet_carrier.vmesh",.016F));
        blueprint(8,"LANCER / patrol",vessel(assets/"fleet_frigate.vmesh",.014F));
        blueprint(9,"MANTA / shuttle",vessel(assets/"fleet_escort.vmesh",.014F));
        state.document.next_blueprint_id=10;
        const auto instance=[&](u32 id,u32 mesh,std::string name,project::InstanceTransform transform={}) {
            state.document.instances.push_back({id,static_cast<project::BlueprintId>(mesh),std::move(name),project::MeshSettings{},transform});
        };
        instance(hero,3,"KESTREL / express courier",{route.at(0),{},1});
        // The actual editable express emerges over the Arabian desert hub.
        instance(earth,5,"Earth / connected departure",{earth_center,earth::express_route::rotation,earth_radius});
        // Thousands of baked samples: one validated commit, not one per key.
        KeyBatch keys;
        u32 next=5;
        std::vector<u32> traffic_ids;
        // A craft `ahead` km along the route from the courier's start at
        // zero, holding a lane beside the courier's line.
        const auto craft=[&](unsigned type,f32 ahead,f32 speed,f32 right,f32 up,f32 scale) {
            const auto id=next++;
            const auto start=start_distance-ahead;
            const auto at=[&](f32 t) {
                const auto remaining=start-speed*t;
                auto p=remaining>=0?route.sample_world(route.arc.parameter(route.throat-remaining/earth_radius)):
                    route.terminal_at(std::min(-remaining/12.F,1.F));
                p.x+=right;p.y+=up;return p;
            };
            instance(id,7+type,std::array{"Heavy transport / ","Patrol / ","Shuttle / "}[type]+std::to_string(traffic_ids.size()+1),
                {at(0),{},scale});
            traffic_ids.push_back(id);
            keys.key({id,"visible"},0,true,timeline::Interpolation::hold);
            for(f32 t=0;t<=voyage::moon_cut;t+=.5F) {
                keys.key({id,"position"},t,at(t));
                // The hero has long since left; traffic disappears beyond the
                // exit rather than piling up at the terminal's last sample.
                if(start-speed*t < -12) {
                    keys.key({id,"visible"},t,false,timeline::Interpolation::hold);break;
                }
            }
            keys.simplify({id,"position"},.0001F);
        };
        // The opening's traffic comes in past its camera.
        const auto station_distance=travel(courier_arrival);
        for(const auto& a:arrivals)
            craft(a.type,station_distance-a.speed*a.passes,a.speed,a.right,a.up,a.scale);
        // The chase's traffic, overtaken one after another. It starts beyond
        // the haze ahead of the opening camera, so the opening's bore is empty.
        for(unsigned i=0;i<12;++i) {
            const auto type=i%3;
            const auto speed=.28F+.08F*static_cast<f32>(type);
            const auto pass=10.6F+.5F*static_cast<f32>(i);
            craft(type,travel(pass)-speed*pass,speed,i%2 ? .85F : -.85F,.28F*(static_cast<f32>(i%3)-1),
                .75F+.15F*static_cast<f32>(i%4));
        }
        // Shared samples keep the camera and courier locked together; every
        // frame while the courier goes by the opening camera, through the
        // catch-up and through the eased move for the exit.
        std::vector<f32> sample_times;
        for(f32 t=0;t<handoff_time;t+=.125F)sample_times.push_back(t);
        for(f32 t=courier_arrival-.3F;t<catch_up_end;t+=1.F/60)sample_times.push_back(t);
        sample_times.push_back(catch_up_begin);sample_times.push_back(catch_up_end);
        for(unsigned frame=0;frame<=36;++frame)
            sample_times.push_back(exit_approach+exit_approach_duration*static_cast<f32>(frame)/36);
        std::ranges::sort(sample_times);
        sample_times.erase(std::unique(sample_times.begin(),sample_times.end()),sample_times.end());
        // The nose follows the average heading over a short window: the path
        // keeps the terminal's quadratic ramp, but the pitch-up eases in and
        // out instead of snapping on at the throat and off at the mouth.
        const auto smoothed_heading=[&](f32 t) {
            Vec3 sum{};
            for(int k=-6;k<=6;++k) {
                const auto v=unit(route.velocity_at(std::clamp(t+static_cast<f32>(k)*.06F,0.F,handoff_time)));
                const auto w=std::exp(-.5F*static_cast<f32>(k*k)/9.F);
                sum=add(sum,mul(v,w));
            }
            return heading(sum);
        };
        for(auto t:sample_times) {
            keys.key({hero,"position"},t,route.at(t));
            keys.key({hero,"rotation"},t,smoothed_heading(t));
        }
        project::SceneInstance camera{camera_id,project::BlueprintId::camera,
            "Cinematic / continuous tracking rig",project::CameraSettings{.active=true},{}};
        project::place_camera(camera,orbit_pose(camera_at(0,route)));state.document.instances.push_back(std::move(camera));
        // Denser samples only around camera moves; ordinary editable tracks,
        // no runtime scene-specific camera controller or hidden cut behavior.
        // The eye itself is keyed, never rebuilt from a distant pivot.
        for(auto t:sample_times)key_look(keys,camera_id,t,camera_at(t,route));
        state.document.next_instance_id=camera_id+1;
        state.document.keyframe_names={{0,"01 / The empty bore"},{2.F,"02 / Traffic comes in"},
            {courier_arrival,"03 / The courier, far faster"},{catch_up_begin,"04 / The camera races after it"},
            {exit_approach,"05 / Close side-offset exit approach"},{throat_time,"06 / Through the dispersal throat"},
            {exit_time,"07 / Clear of the terminal"}};
        const auto look=camera_at(handoff_time,route);
        const voyage::Handoff handoff{handoff_time,route.at(handoff_time),route.velocity_at(handoff_time),earth_center,look};
        const voyage::Cast cast{hero,camera_id,earth,traffic_ids,static_cast<project::BlueprintId>(3),
            static_cast<project::BlueprintId>(7),static_cast<project::BlueprintId>(8),static_cast<project::BlueprintId>(9)};
        checked(voyage::author(state,keys,handoff,cast));
        // The lens rarely changes; the focus only places the editor's orbit pivot.
        keys.simplify({camera_id,"zoom"},.0005F);
        keys.simplify({camera_id,"focus"},.05F);
        // Collinear keys (static shots, holds, straight runs) add nothing:
        // drop them within 10 cm and 0.004 degrees, which pays for the dense
        // keys fast moves need.
        for(const auto object:{camera_id,hero}) {
            keys.simplify({object,"position"},.0001F);
            keys.simplify({object,"rotation"},.004F);
        }
        checked(keys.commit(state));
        // Authored presentation: the demo camera sees the sun and distant
        // bodies without a distant focus (see render_camera).
        state.document.environment={.stars=6500,.star_seed=132,.exposure=.95F,.bloom_threshold=1.1F,.bloom_strength=.30F,
            .view_distance=1'200'000};
        state.document.world_bounds=voyage::bounds(state);
        state.viewport.mode=project::ViewMode::scene;state.viewport.selected_object=hero;
        state.viewport.editor_camera=orbit_pose(camera_at(0,route));
        checked(project::validate_animation(state));checked(project::validate_active_cameras(state));
        return state;
    } catch(const std::exception& e) {
        content::Diagnostic diagnostic;diagnostic.message=e.what();return std::unexpected(std::move(diagnostic));
    }
}
}
