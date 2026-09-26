#include "departure_voyage.hpp"
#include "../support/asteroid_assets.hpp"
#include "../support/space_assets.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>

namespace example::tunnel::voyage {
namespace {
using namespace vng;
namespace project = editor_example;
using timeline::Interpolation;
constexpr double pi = std::numbers::pi;
constexpr double degree = pi / 180;

// Double-precision authoring vectors; keys are rounded once, near the origin.
struct V { double x{}, y{}, z{}; };
V operator+(V a, V b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
V operator-(V a, V b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
V operator-(V a) { return {-a.x,-a.y,-a.z}; }
V operator*(V a, double s) { return {a.x*s,a.y*s,a.z*s}; }
double dot(V a, V b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
V cross(V a, V b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
double length(V a) { return std::sqrt(dot(a,a)); }
V unit(V a) { return a*(1/length(a)); }
V lerp(V a, V b, double u) { return a+(b-a)*u; }
V from(Vec3 p) { return {p.x,p.y,p.z}; }
Vec3 to(V p) { return {static_cast<f32>(p.x),static_cast<f32>(p.y),static_cast<f32>(p.z)}; }
double smooth(double x) { x=std::clamp(x,0.,1.); return x*x*(3-2*x); }
// Smoothly 0 before t0, 1 after t1.
double ease(double t, double t0, double t1) { return smooth((t-t0)/(t1-t0)); }
V rotate(V v, V axis, double angle) {
    const auto c=std::cos(angle), s=std::sin(angle);
    return v*c+cross(axis,v)*s+axis*(dot(axis,v)*(1-c));
}
V slerp(V a, V b, double u) {
    const auto angle=std::acos(std::clamp(dot(a,b),-1.,1.));
    if (angle<1e-9) return a;
    return unit(a*(std::sin((1-u)*angle)/std::sin(angle))+b*(std::sin(u*angle)/std::sin(angle)));
}
// Removes the component along `axis` and normalizes.
V flatten(V v, V axis) { return unit(v-axis*dot(v,axis)); }
// An instance's Euler rotation (degrees, T*Rz*Ry*Rx) applied to a direction.
V turned(Vec3 degrees, V v) {
    v=rotate(v,{1,0,0},degrees.x*degree);
    v=rotate(v,{0,1,0},degrees.y*degree);
    return rotate(v,{0,0,1},degrees.z*degree);
}
template<class T> T take(content::Result<T> value) {
    if (!value) throw std::runtime_error(value.error().message);
    return std::move(*value);
}

// "World" is the skyway's frame: the tunnel exit at the origin, Earth below.
// The sun is infinitely far: it lights the skyway along the same direction
// as its previous directional light.
const V sun_direction=unit({-.35,.8,.65});
constexpr double sun_distance=900000;
// Earth to Moon, turned away from the sun so that from the lunar night Earth
// is a crescent with its city lights, and the Moon is gibbous from Earth.
// Distances are compressed for the cinematic.
const V moon_axis=[]{
    const V m=unit({-.30,.42,-1.});
    return rotate(m,unit(cross(sun_direction,m)),20*degree);
}();
constexpr double moon_distance=110000;
constexpr double moon_radius=space::MoonSurface::radius;
// Where the Moon and belt locations are shown: near the origin for precision,
// clear of the skyway's interior haze (positive Z around the tunnel axis).
const V moon_stage_anchor{0,0,-600}, belt_stage_anchor{0,0,-600};

// A location's scene frame: the world point `pivot` is shown at `anchor`,
// rotated so close shots stay near the origin and flights clear of Euler
// gimbal lock. Rotating a location also turns its sun and bodies with it.
struct Stage {
    V pivot{}, anchor{}, right{1,0,0}, up{0,1,0}, back{0,0,1};
    // The world direction `forward` along scene -Z and `up` along scene +Y.
    static Stage facing(V pivot, V anchor, V forward, V up) {
        const auto r=unit(cross(forward,up));
        return {pivot,anchor,r,cross(-forward,r),-forward};
    }
    // The least rotation that shows the sun along `sun_scene`.
    static Stage lit(V pivot, V anchor, V sun_scene) {
        const auto axis=unit(cross(sun_direction,sun_scene));
        const auto angle=std::acos(std::clamp(dot(sun_direction,sun_scene),-1.,1.));
        return {pivot,anchor,rotate({1,0,0},axis,-angle),rotate({0,1,0},axis,-angle),rotate({0,0,1},axis,-angle)};
    }
    [[nodiscard]] V direction(V d) const { return {dot(d,right),dot(d,up),dot(d,back)}; }
    [[nodiscard]] V point(V p) const { return anchor+direction(p-pivot); }
    // Scene rotation of something whose world axes -Z and +Y are given.
    [[nodiscard]] Vec3 rotation(V forward, V upward) const { return orientation(to(direction(forward)),to(direction(upward))); }
};

// A finely integrated flight, so baked keys, bank and scenery placement agree.
struct Pose { V position, forward, up; double speed; };
class Flight {
public:
    // `direction` is the unit heading, `level` the reference up for a pose
    // without turns; sideways acceleration banks the craft into its turns.
    Flight(double begin, double end, V start, std::function<V(double)> direction,
           std::function<double(double)> speed, std::function<V(double,V)> level, double bank)
        : begin_(begin) {
        const auto count=static_cast<std::size_t>(std::ceil((end-begin)/step))+1;
        std::vector<V> positions{start};
        for (std::size_t i=1;i<count;++i) {
            const auto t=begin+(static_cast<double>(i)-.5)*step;
            positions.push_back(positions.back()+direction(t)*(speed(t)*step));
        }
        std::vector<V> forward(count), level_up(count), side(count);
        std::vector<double> lean(count);
        for (std::size_t i=0;i<count;++i) {
            const auto t=begin+static_cast<double>(i)*step;
            forward[i]=direction(t);
            const auto velocity=[&](double at){ return direction(at)*speed(at); };
            const auto accel=(velocity(t+.02)-velocity(t-.02))*(1/.04);
            // Bank into sideways acceleration only, easing toward 45 degrees
            // rather than stopping hard there; climbs and dives pitch the
            // craft without rolling it.
            level_up[i]=flatten(level(t,positions[i]),forward[i]);
            side[i]=cross(forward[i],level_up[i]);
            lean[i]=std::tanh(dot(accel,side[i])*bank);
        }
        // A pilot's bank: smoothed over a third of a second without lag (run
        // forward, then back) and never rolling faster than 40 degrees a second.
        const auto blend=1-std::exp(-step/.3);
        for (std::size_t i=1;i<count;++i) lean[i]=lean[i-1]+blend*(lean[i]-lean[i-1]);
        for (std::size_t i=count-1;i-->0;) lean[i]=lean[i+1]+blend*(lean[i]-lean[i+1]);
        const auto most=40*degree*step;
        for (std::size_t i=1;i<count;++i)
            lean[i]=std::tan(std::clamp(std::atan(lean[i]),std::atan(lean[i-1])-most,std::atan(lean[i-1])+most));
        for (std::size_t i=0;i<count;++i)
            samples_.push_back({positions[i],forward[i],unit(level_up[i]+side[i]*lean[i]),speed(begin+static_cast<double>(i)*step)});
    }
    [[nodiscard]] Pose at(double t) const {
        const auto x=std::clamp((t-begin_)/step,0.,static_cast<double>(samples_.size()-1));
        const auto i=std::min(static_cast<std::size_t>(x),samples_.size()-2);
        const auto u=x-static_cast<double>(i);
        const auto& a=samples_[i]; const auto& b=samples_[i+1];
        return {lerp(a.position,b.position,u),unit(lerp(a.forward,b.forward,u)),unit(lerp(a.up,b.up,u)),
                std::lerp(a.speed,b.speed,u)};
    }
private:
    static constexpr double step=1./480;
    double begin_;
    std::vector<Pose> samples_;
};

// A centripetal Catmull-Rom path through waypoints, sampled by arc length.
class Path {
public:
    explicit Path(std::vector<V> points) {
        points.insert(points.begin(),points[0]*2-points[1]);
        points.push_back(points.back()*2-points[points.size()-2]);
        for (std::size_t i=1;i+2<points.size();++i)
            for (unsigned k=0;k<64;++k) {
                const auto u=k/64.;
                const auto& p0=points[i-1]; const auto& p1=points[i]; const auto& p2=points[i+1]; const auto& p3=points[i+2];
                const auto u2=u*u, u3=u2*u;
                samples_.push_back((p1*2+(p2-p0)*u+(p0*2-p1*5+p2*4-p3)*u2+(p1*3-p0-p2*3+p3)*u3)*.5);
            }
        samples_.push_back(points[points.size()-2]);
        lengths_.push_back(0);
        for (std::size_t i=1;i<samples_.size();++i) lengths_.push_back(lengths_.back()+length(samples_[i]-samples_[i-1]));
    }
    [[nodiscard]] double length_km() const { return lengths_.back(); }
    [[nodiscard]] V at(double s) const {
        s=std::clamp(s,0.,lengths_.back());
        const auto i=std::min<std::size_t>(static_cast<std::size_t>(std::ranges::upper_bound(lengths_,s)-lengths_.begin()),lengths_.size()-1);
        if (i==0) return samples_.front();
        const auto span=lengths_[i]-lengths_[i-1];
        return lerp(samples_[i-1],samples_[i],span>0 ? (s-lengths_[i-1])/span : 0);
    }
private:
    std::vector<V> samples_;
    std::vector<double> lengths_;
};

// A quintic Hermite curve from `from` leaving along `leave` to `to` arriving
// along `arrive` (tangent lengths in km), with no curvature at either end:
// continuous in curvature throughout, so a craft banking along it never
// snaps. Sampled by arc length.
class Curve {
public:
    Curve(V from, V leave, V to, V arrive) {
        for (unsigned k=0;k<=512;++k) {
            const auto u=k/512., u3=u*u*u, u4=u3*u, u5=u4*u;
            samples_.push_back(from*(1-10*u3+15*u4-6*u5)+leave*(u-6*u3+8*u4-3*u5)+
                               arrive*(-4*u3+7*u4-3*u5)+to*(10*u3-15*u4+6*u5));
        }
        lengths_.push_back(0);
        for (std::size_t i=1;i<samples_.size();++i) lengths_.push_back(lengths_.back()+length(samples_[i]-samples_[i-1]));
    }
    [[nodiscard]] double length_km() const { return lengths_.back(); }
    [[nodiscard]] V at(double s) const {
        s=std::clamp(s,0.,lengths_.back());
        const auto i=std::min<std::size_t>(static_cast<std::size_t>(std::ranges::upper_bound(lengths_,s)-lengths_.begin()),lengths_.size()-1);
        if (i==0) return samples_.front();
        const auto span=lengths_[i]-lengths_[i-1];
        return lerp(samples_[i-1],samples_[i],span>0 ? (s-lengths_[i-1])/span : 0);
    }
private:
    std::vector<V> samples_;
    std::vector<double> lengths_;
};

// A frame fixed to a pose: offsets are {right, up, back} in kilometres.
V relative(const Pose& pose, V offset) {
    const auto right=cross(pose.forward,pose.up);
    return pose.position+right*offset.x+pose.up*offset.y-pose.forward*offset.z;
}
struct Shot { V eye, target; double zoom{1}; };
// Orbit around a pivot from one shot to another: never through the subject.
// The look direction turns evenly, however far away each shot's target is.
Shot orbit(const Shot& a, const Shot& b, V pivot, double u) {
    const auto ra=a.eye-pivot, rb=b.eye-pivot;
    const auto radius=std::exp(std::lerp(std::log(length(ra)),std::log(length(rb)),u));
    const auto eye=pivot+slerp(unit(ra),unit(rb),u)*radius;
    const auto look=slerp(unit(a.target-a.eye),unit(b.target-b.eye),u);
    return {eye,eye+look*std::lerp(length(a.target-a.eye),length(b.target-b.eye),u),
            std::exp(std::lerp(std::log(a.zoom),std::log(b.zoom),u))};
}

// One location of the voyage: its frame, the courier and camera in that
// location's authoring coordinates, and its editing.
struct Location {
    Stage stage;
    std::function<Pose(double)> courier;
    std::function<Shot(double)> camera;
    bool scene_axes{}; // authored directly in scene axes; the stage maps only world bodies
    // Composed sky: where this location shows the Moon and Earth, if not
    // where its stage would put them.
    std::optional<V> moon{}, earth{};
    std::vector<double> cuts{};                  // camera cuts within the location
    std::vector<std::array<double,3>> dense{};   // {begin, end, step}: fast moves
    std::vector<std::array<double,2>> glow{};    // spans where the drive reads as a light
    // Sun directions (in scene axes) cheated for a shot: held from a cut, or
    // swung linearly where a camera move hides it.
    std::vector<std::tuple<double,V,Interpolation>> relight{};
    // Scene directions for Earth's Arabian face and the Moon's near side to
    // turn toward.
    std::optional<V> earth_facing{}, moon_facing{};
    // Times where the courier jumps, out of sight, to a cheated path and back.
    std::vector<double> jumps{};
};

// Shared authoring context: the state, the batch and small keying helpers.
struct Voyage {
    project::State& state;
    KeyBatch& keys;
    const Handoff& handoff;
    const Cast& cast;
    V earth_center{}, moon_center{};
    u32 flash{}, spark{}; // glowing spheres of radius 1 km (the far drive glow) and 0.1 km (sparks)
    u32 add_blueprint(std::string name, content::vmesh::Document mesh) {
        const auto id=state.document.next_blueprint_id++;
        state.document.mesh_assets.push_back({static_cast<project::BlueprintId>(id),std::move(name),
            take(editor::EditableMesh::create(std::move(mesh))),{}});
        return id;
    }
    u32 add(u32 blueprint, std::string name, V position, Vec3 rotation = {}, f32 scale = 1, bool visible = true) {
        const auto id=state.document.next_instance_id++;
        state.document.instances.push_back({id,static_cast<project::BlueprintId>(blueprint),std::move(name),
            project::MeshSettings{.visible=visible},{to(position),rotation,scale}});
        return id;
    }
    void show(u32 id, double time, bool visible) { keys.key({id,"visible"},static_cast<f32>(time),visible,Interpolation::hold); }
    void place(u32 id, double time, V position, Interpolation incoming = Interpolation::hold) {
        keys.key({id,"position"},static_cast<f32>(time),to(position),incoming);
    }
    void turn(u32 id, double time, Vec3 rotation, Interpolation incoming = Interpolation::hold) {
        keys.key({id,"rotation"},static_cast<f32>(time),rotation,incoming);
    }
    void stretch(u32 id, double time, Vec3 axes, Interpolation incoming = Interpolation::linear) {
        keys.key({id,"axis_scale"},static_cast<f32>(time),axes,incoming);
    }
    void glow(u32 id, double time, double brightness, Interpolation incoming = Interpolation::linear) {
        keys.key({id,"brightness"},static_cast<f32>(time),static_cast<f32>(brightness),incoming);
    }
    void pose(u32 id, double time, const Pose& pose, Interpolation incoming = Interpolation::linear) {
        keys.key({id,"position"},static_cast<f32>(time),to(pose.position),incoming);
        keys.key({id,"rotation"},static_cast<f32>(time),orientation(to(pose.forward),to(pose.up)),incoming);
    }
};

// Sample times: a base rate plus denser windows for fast moves; each cut gets
// a key a millisecond before it so the outgoing shot runs to the cut.
struct Timing {
    std::vector<double> times;
    void every(double begin, double end, double step) {
        for (double t=begin;t<end-1e-9;t+=step) times.push_back(t);
        times.push_back(end);
    }
    void cut(double t) { times.push_back(t-.001); times.push_back(t); cuts.push_back(t); }
    // Samples that land on a cut up to rounding become exactly the cut, so
    // the cut's key always holds the new shot.
    std::vector<double> sorted() {
        std::ranges::sort(times);
        std::vector<double> result;
        for (auto t : times) {
            for (const auto c : cuts) if (std::abs(t-c)<1e-6) t=c;
            if (result.empty() || t-result.back()>1e-6) result.push_back(t);
        }
        return result;
    }
    std::vector<double> cuts;
};

// ==========================================================================
// Earth orbit: the climb to the orbital gateway and the burn for the Moon.
// Authored in the skyway's own frame.
// ==========================================================================
constexpr double ring_time=38.8, turn_begin=45.4, turn_end=48.6, burn_time=47.55, ignition=47.25;
constexpr double nozzle_plane=.0594; // the Kestrel's exhaust plane behind its origin

Location earth_orbit(Voyage& v) {
    const double t_h=v.handoff.time;
    const auto p_h=from(v.handoff.position), v_h=from(v.handoff.velocity);
    const auto up_h=unit(p_h-v.earth_center), f_h=flatten(v_h,up_h);
    const auto speed_h=length(v_h);
    const auto gamma_h=std::atan2(dot(v_h,up_h),dot(v_h,f_h));
    // Pull up hard out of the terminal, climb clear of Earth's towers and
    // skyways (up to ~400 km), then arc over into the gateway's approach at
    // about 690 km.
    const auto climb=[=](double t) {
        const auto gamma=gamma_h+(58*degree-gamma_h)*ease(t,t_h+.3,t_h+4.3)+(14-58)*degree*ease(t,30.5,36);
        return unit(f_h*std::cos(gamma)+up_h*std::sin(gamma));
    };
    // Race up, slow right down for the gateway, then burn hard for the Moon.
    const auto speed=[=](double t) {
        return speed_h+(70-speed_h)*ease(t,t_h+.2,28)+(3.6-70)*ease(t,31.5,37.9)+
               450*std::pow(ease(t,burn_time,moon_cut),2.5);
    };
    const auto earth=v.earth_center;
    const auto level=[=](double, V p){ return unit(p-earth); };
    // After the dock the courier turns toward where the Moon will be from the
    // point where the burn begins.
    const Flight probe(t_h,moon_cut+.1,p_h,climb,speed,level,.5);
    const auto aim=unit(v.moon_center-probe.at(burn_time).position);
    const auto heading=[=](double t){ return slerp(climb(t),aim,ease(t,turn_begin,turn_end)); };
    const auto flight=std::make_shared<const Flight>(t_h,moon_cut+.1,p_h,heading,speed,level,.5);
    const auto courier=[flight](double t){ return flight->at(t); };

    // The gateway sits on the straight approach: the courier crosses the ring
    // plane at ring_time below the hub, between two spokes, and passes under
    // the construction dock.
    const auto at_ring=flight->at(ring_time);
    const auto sx=at_ring.forward, sy=flatten(level(0,at_ring.position),sx), sz=cross(sx,sy);
    constexpr double lane=4.2;
    const auto origin=at_ring.position+sy*lane;
    const auto local=[=](V p){ return origin+sx*p.x+sy*p.y+sz*p.z; };
    const auto station_rotation=orientation(to(-sz),to(sy));
    const auto gateway=v.add_blueprint("GATEWAY / Arabian orbital yard",space::gateway_station());
    const auto ring=v.add_blueprint("GATEWAY / habitat ring",space::habitat_ring());
    std::vector<u32> parts{v.add(gateway,"GATEWAY / Arabian orbital yard",origin,station_rotation)};
    const auto ring_id=v.add(ring,"GATEWAY / habitat ring",origin,station_rotation);
    parts.push_back(ring_id);
    // One g at the rim: 2.3 degrees a second about its axis (local X); the
    // spokes are 45 degrees clear of the lane at ring_time.
    for (double t=t_h;;t=std::min(t+10.,static_cast<double>(moon_cut))) {
        auto spun=station_rotation;
        spun.x+=static_cast<f32>(std::remainder(45+2.3*(t-ring_time),360.));
        v.turn(ring_id,t,spun,Interpolation::linear);
        if (t>=moon_cut) break;
    }
    // The next flagship-class hull under construction, bow to the dock mouth.
    parts.push_back(v.add(static_cast<u32>(v.cast.transport),"BASTION / hull under construction",
        local({15,-.2,0}),orientation(to(sx),to(sy)),8.5F));
    // Weld sparks on the hull: brief, irregular flickers.
    for (const auto& [spot, phase] : std::array{std::pair{V{14.2,.45,.5},.0},std::pair{V{15.9,-.5,-.4},.37},std::pair{V{15.1,.35,-.6},.71}}) {
        const auto id=v.add(v.spark,"GATEWAY / weld spark",local(spot),{},.15F,false);
        v.glow(id,t_h,.35,Interpolation::hold);
        for (double t=36.+phase;t<46;t+=.53+.41*std::fmod(t*1.7,1.)) {
            v.show(id,t,true);
            v.show(id,t+.08+.05*std::fmod(t*3.1,1.),false);
        }
        parts.push_back(id);
    }
    // Station traffic uses the gateway like the skyway: through the ring,
    // each crossing it midway between two spokes.
    struct Traffic { u32 blueprint; const char* name; V from, to; f32 scale; };
    const std::array traffic{
        Traffic{static_cast<u32>(v.cast.transport),"Freighter / inbound",{-24,4.23,.34},{12,4.23,.34},.9F},
        Traffic{static_cast<u32>(v.cast.shuttle),"Shuttle / outbound",{9,3.66,-2.15},{-40,3.66,-2.15},1.1F},
        Traffic{static_cast<u32>(v.cast.patrol),"Tug / dock",{13,2.6,-1.4},{17,2.4,-.6},.8F},
        Traffic{static_cast<u32>(v.cast.patrol),"Patrol / picket",{-30,-9,8},{25,-7,10},.9F}};
    for (const auto& craft : traffic) {
        const auto a=local(craft.from), b=local(craft.to);
        const auto id=v.add(craft.blueprint,craft.name,a,orientation(to(unit(b-a)),to(sy)),craft.scale);
        v.place(id,t_h,a);
        v.place(id,moon_cut,b,Interpolation::linear);
        parts.push_back(id);
    }
    // A freighter crossing close in front of the wide shot of the gateway,
    // for motion and scale.
    const auto wide_eye=local({-38,34,46}), wide_target=local({-14,-6,0});
    {
        const auto look=unit(wide_target-wide_eye), across=unit(cross(look,sy));
        const auto crossing=wide_eye+look*5-sy*.7;
        const auto a=crossing-across*(2.6*(31.4-t_h)), b=crossing+across*(2.6*(moon_cut-31.4));
        const auto id=v.add(static_cast<u32>(v.cast.transport),"Freighter / crossing",a,orientation(to(across),to(sy)),.9F);
        v.place(id,t_h,a);
        v.place(id,moon_cut,b,Interpolation::linear);
        parts.push_back(id);
    }
    for (const auto id : parts) v.show(id,moon_cut,false);
    for (const auto id : v.cast.traffic) v.show(id,moon_cut,false);

    // Camera: the pull-up, the climb, Earth and the gateway, threading the
    // ring, past the hull under construction, and the burn for the Moon from
    // a camera the courier leaves behind.
    // Eye and target both fixed to the courier's frame.
    const auto framed=[courier](double t, V eye, V target, double zoom = 1) {
        const auto pose=courier(t);
        return Shot{relative(pose,eye),relative(pose,target),zoom};
    };
    const auto chase=[framed](double t, V offset, double lead = 0, double zoom = 1) {
        return framed(t,offset,{0,0,-lead},zoom);
    };
    // The skyway's last shot, eye and target alike, in the courier's frame:
    // the pull-up starts from exactly that framing.
    const auto start=courier(t_h);
    const auto right=cross(start.forward,start.up);
    const auto in_frame=[&](Vec3 point) {
        const auto d=from(point)-start.position;
        return V{dot(d,right),dot(d,start.up),-dot(d,start.forward)};
    };
    const auto handoff_eye=in_frame(v.handoff.camera.eye), handoff_target=in_frame(v.handoff.camera.target);
    const auto handoff_zoom=static_cast<double>(v.handoff.camera.zoom);
    // Framed on Earth's local vertical rather than the pitching craft.
    const auto side_view=[=](double t) {
        const auto pose=courier(t);
        const auto vertical=level(t,pose.position);
        const auto along=flatten(pose.forward,vertical), across=cross(along,vertical);
        return Shot{pose.position+across*.8+vertical*.25-along*.35,pose.position+along*.2,1};
    };
    // Behind the courier and a little to one side on a wide lens, tilted up
    // so the dock's lattice passes overhead.
    const auto dock_shot=[=](double t) {
        const auto pose=courier(t);
        return Shot{pose.position-pose.forward*.45+cross(pose.forward,sy)*.12+sy*.03,
                    pose.position+pose.forward*1.2+sy*.25,.85};
    };
    // The hull in profile through the dock's lattice, level with it, as the
    // courier comes up the lane below.
    const Shot dock_profile{local({9,-1.2,-7.2}),local({15,-.8,0}),1};
    const V burn_offset{.02,.07,.36};
    // After ignition the camera coasts to rest behind the courier while the
    // lens lengthens: the courier burns away up toward the Moon, which ends
    // high on the right third. `coasting` is the chase's own clock, easing to
    // a stop.
    const auto coasting=[](double t) {
        const auto x=t-ignition, u=std::min(x,.6)/.6;
        return t<ignition ? t : ignition+(x<.6 ? x-.6*(u*u*u-.5*u*u*u*u) : .3);
    };
    const auto burn_eye=chase(ignition+.3,burn_offset,.4).eye;
    const auto burn_look=[&]{
        const auto to_moon=unit(v.moon_center-burn_eye);
        const auto side=unit(cross(to_moon,sy)), lift=cross(side,to_moon);
        return unit(to_moon-side*std::tan(4.4*degree)-lift*std::tan(2.5*degree));
    }();
    const auto camera=[=](double t) -> Shot {
        const auto pose=courier(t);
        if (t<30) {
            const auto pull_up=orbit(framed(t,handoff_eye,handoff_target,handoff_zoom),chase(t,{.08,-.07,.31},.05),
                pose.position,ease(t,t_h,t_h+3.4));
            return orbit(pull_up,side_view(t),pose.position,ease(t,24.6,26.8));
        }
        // Earth and the gateway, with a freighter crossing in front.
        if (t<32.8) return {wide_eye+sx*(.15*(t-30)),wide_target,1};
        // Threading the ring from just behind: the gateway grows ahead.
        if (t<39.4) return chase(t,{.03,.10,.42},5,1);
        if (t<41.6) return dock_profile;
        if (t<45.5) return dock_shot(t);
        // Round behind the courier as it clears the dock and turns for the Moon.
        if (t<ignition) return orbit(dock_shot(t),chase(t,burn_offset,.4),pose.position,ease(t,45.5,47.1));
        const auto held=chase(coasting(t),burn_offset,.4);
        const auto look=slerp(unit(held.target-held.eye),burn_look,ease(t,ignition,ignition+1.2));
        return {held.eye,held.eye+look*10,1+2*std::clamp((t-ignition-.3)/(moon_cut-ignition-.3),0.,1.)};
    };
    Location location{Stage{},courier,camera};
    location.cuts={30,32.8,39.4,41.6};
    location.dense={{t_h,27,1./30},{38.3,39.4,1./30},{41.6,45.4,1./15},{45.4,moon_cut,1./30}};
    location.glow={{47.9,moon_cut}};
    // Cheated through the turn: the sun swings a quarter turn to the camera's
    // right, so the Moon ahead shows its terminator rather than a flat full
    // face; the courier's own turn hides the change.
    location.relight={{45.5,sun_direction,Interpolation::linear},
                      {47.1,unit(cross(burn_look,sy)+sy*.25),Interpolation::linear}};
    return location;
}

// ==========================================================================
// The Moon: a slingshot low over its night side and a mining base, into
// sunrise. Authored around the Moon in world axes, shown through its stage.
// ==========================================================================
constexpr double pass_time=57.4, climb_begin=65.5;

Location moon_pass(Voyage& v, u32& moon_blueprint, u32& base_blueprint) {
    const auto earth_direction=-moon_axis;
    // The base: before dawn (the sun about 7 degrees below the horizon) with
    // Earth a crescent about 20 degrees up. Solves c.s = -0.12, c.e = 0.35.
    const auto base_direction=[&]{
        const auto s=sun_direction, e=earth_direction, n=unit(cross(s,e));
        const auto se=dot(s,e);
        const auto planar=s*((-.12-.35*se)/(1-se*se))+e*((.35+.12*se)/(1-se*se));
        return unit(planar+n*std::sqrt(std::max(0.,1-dot(planar,planar))));
    }();
    const auto track=flatten(sun_direction,base_direction); // toward the sunrise
    const auto track_axis=unit(cross(base_direction,track));
    const auto ground=[=](double km){ return rotate(base_direction,track_axis,km/moon_radius); };
    // The base lies beside the track on the side away from Earth, so a camera
    // beyond it looks across the base and the courier's path toward Earth.
    const auto away_from_earth=dot(earth_direction,cross(track,base_direction))>0 ? -1. : 1.;
    const auto base_site=rotate(ground(0),track,away_from_earth*2.6/moon_radius);
    const auto moon=std::make_shared<const space::MoonSurface>(space::MoonCorridor{to(ground(0)),to(track)},
        to(earth_direction),std::vector<space::MoonSite>{{to(base_site),9}});
    moon_blueprint=v.add_blueprint("MOON / Selene",space::moon_mesh(*moon));
    base_blueprint=v.add_blueprint("LUNAR / Serenity mining works",space::lunar_base(*moon,to(base_site),to(track)));
    const auto center=v.moon_center;
    const auto base=center+from(moon->point(to(base_site)));
    const auto pass_point=center+from(moon->point(to(ground(0))));
    const auto stage=Stage::facing(pass_point,moon_stage_anchor,track,base_direction);

    // Arrive fast, slow to about 6 km/s over the base, then race for the dawn.
    constexpr double step=1./240;
    const auto ground_speed=[](double t){
        return 6+34*(1-ease(t,moon_cut,56.8))+10*ease(t,58.5,64)+12*ease(t,66,belt_cut);
    };
    auto km=std::make_shared<std::vector<double>>(std::vector<double>{0});
    for (double t=moon_cut;t<belt_cut+.6;t+=step) km->push_back(km->back()+ground_speed(t)*step);
    const auto pass_km=(*km)[static_cast<std::size_t>((pass_time-moon_cut)/step)];
    const auto distance=[=](double t) {
        const auto x=std::clamp((t-moon_cut)/step,0.,static_cast<double>(km->size()-1));
        const auto i=std::min(static_cast<std::size_t>(x),km->size()-2);
        return std::lerp((*km)[i],(*km)[i+1],x-static_cast<double>(i))-pass_km;
    };
    // A smooth ceiling over the ground near the track: the highest ground
    // per half kilometre, a running maximum with look-ahead, then smoothing
    // over tens of kilometres. The courier rides it without bobbing.
    constexpr double cell=.5, reach=400;
    const auto cells=static_cast<std::size_t>(2*reach/cell)+1;
    std::vector<double> raw(cells);
    for (std::size_t i=0;i<cells;++i) {
        const auto s=-reach+cell*static_cast<double>(i);
        double highest=-1e9;
        for (const auto side : {-2.5,0.,2.5})
            highest=std::max(highest,static_cast<double>(moon->height(to(rotate(ground(s),track,side/moon_radius)))));
        raw[i]=highest;
    }
    std::vector<double> held(cells,-1e9);
    for (std::size_t i=0;i<cells;++i)
        for (auto j=i>=8 ? i-8 : 0;j<std::min(cells,i+20);++j) held[i]=std::max(held[i],raw[j]);
    auto ceiling=std::make_shared<std::vector<double>>(cells);
    for (std::size_t i=0;i<cells;++i) {
        double sum{}, weight{};
        for (int k=-48;k<=48;++k) {
            const auto j=static_cast<long>(i)+k;
            if (j<0 || j>=static_cast<long>(cells)) continue;
            const auto w=std::exp(-.5*(k*cell/8)*(k*cell/8));
            sum+=w*held[static_cast<std::size_t>(j)]; weight+=w;
        }
        (*ceiling)[i]=sum/weight;
    }
    const auto floor_at=[=](double s) {
        const auto x=std::clamp((s+reach)/cell,1.,static_cast<double>(cells-3));
        const auto i=static_cast<std::size_t>(x);
        const auto u=x-static_cast<double>(i);
        const auto& c=*ceiling;
        const auto p0=c[i-1], p1=c[i], p2=c[i+1], p3=c[i+2];
        return p1+.5*u*(p2-p0+u*(2*p0-5*p1+4*p2-p3+u*(3*(p1-p2)+p3-p0)));
    };
    // Earthrise over Serenity is seen from 6 km out beyond the base, looking
    // over it toward the Earth.
    const auto base_eye=[&]{
        const auto d=unit(base+flatten(base-pass_point,unit(base-center))*6-center);
        return center+d*(moon_radius+moon->height(to(d))+.45);
    }();
    const auto base_up=unit(base_eye-center);
    const auto toward_earth=flatten(v.earth_center-base_eye,base_up);
    const auto base_look=unit(toward_earth*std::cos(9.5*degree)+base_up*std::sin(9.5*degree));
    const auto base_right=unit(cross(base_look,base_up)), base_top=cross(base_right,base_look);
    // Cheated for that shot, and hidden by cuts either side: the courier
    // roars in low over the camera's left shoulder, crosses just under the
    // Earth, and races away toward the valley right of frame; after the cut
    // it is back on its track along the mass driver.
    constexpr double flyby_begin=56.6, flyby_end=59;
    const auto shoulder=base_eye-base_right*.12+base_top*.12-base_look*.06;
    const auto under_earth=base_eye+rotate(unit(v.earth_center-base_eye),base_right,-4.5*degree)*.4;
    const auto away=rotate(rotate(base_look,base_top,-9.3*degree),base_right,-5*degree);
    const auto flyby=std::make_shared<const Path>(std::vector<V>{shoulder-base_look*.95-base_right*.1+base_top*.1,
                                                                 shoulder,under_earth,under_earth+away*3.5});
    const auto altitude=[](double t){
        return 3.5+110*std::pow(1-ease(t,moon_cut,56.4),1.4)+45*std::pow(ease(t,climb_begin,belt_cut+.3),1.5);
    };
    // The ceiling smoothed in time as well as distance: at tens of
    // kilometres a second, ground-scale smoothing alone still bobs the
    // courier. Climbing for the dawn it levels off onto the highest ground
    // still ahead, so the climb itself is smooth.
    const auto highest_ahead=[&]{
        double h=-1e9;
        for (double t=climb_begin-1;t<belt_cut+.6;t+=.02) h=std::max(h,floor_at(distance(t)));
        return h;
    }();
    const auto terrain=[=](double t) {
        double sum{}, weight{};
        for (int k=-6;k<=6;++k) {
            const auto w=std::exp(-.5*k*k/9.);
            sum+=w*floor_at(distance(t+k*.06)); weight+=w;
        }
        return std::lerp(sum/weight,highest_ahead,ease(t,climb_begin-1,climb_begin+1.5));
    };
    const auto position=[=](double t) {
        if (t>=flyby_begin && t<flyby_end) return flyby->at(flyby->length_km()*(t-flyby_begin)/(flyby_end-flyby_begin));
        return center+ground(distance(t))*(moon_radius+terrain(t)+altitude(t));
    };
    // Differences stay inside the location, even at its first instant.
    const auto window=[](double t){ return std::clamp(t-.01,static_cast<double>(moon_cut),belt_cut+.08); };
    const auto heading=[=](double t){ return unit(position(window(t)+.02)-position(window(t))); };
    const auto speed=[=](double t){ return length(position(window(t)+.02)-position(window(t)))/.02; };
    const auto flight=std::make_shared<const Flight>(moon_cut,belt_cut+.1,position(moon_cut),heading,speed,
        [=](double, V p){ return unit(p-center); },.35);
    const auto courier=[=](double t){ auto pose=flight->at(t); pose.position=position(t); return pose; };

    // Camera: behind and above the courier as it dives for the night side,
    // the sun setting ahead; Earthrise over Serenity as the courier roars
    // past; a low run along the mass driver's lights; the dawn. Chase frames
    // sit on the local vertical, so the camera never rolls with the courier.
    // The limb stays 7 degrees above center, leaving the setting sun room
    // above it, and the courier 13 below: the camera's height tracks the dip
    // of the horizon as it descends.
    const auto descent=[=](double t) {
        const auto p=position(t), vertical=unit(p-center), ahead=flatten(courier(t).forward,vertical), side=cross(ahead,vertical);
        const auto dip=std::acos(std::min(1.,moon_radius/length(p-center)));
        const auto rise=dip+20*degree, look=dip+7*degree;
        const auto eye=p+(vertical*std::sin(rise)-ahead*std::cos(rise))*.38+side*.04;
        return Shot{eye,eye+(ahead*std::cos(look)-vertical*std::sin(look))*10,1};
    };
    // The base low in frame and the Earth high.
    const Shot base_shot{base_eye,base_eye+base_look*9,1.3};
    const auto low_run=[=](double t) {
        const auto p=position(t), vertical=unit(p-center), ahead=flatten(courier(t).forward,vertical), side=cross(ahead,vertical);
        return Shot{p-ahead*.34+vertical*.10+side*.03,p+ahead*.35-vertical*.02,1};
    };
    // Into the sunrise: the courier low in frame, the horizon near center.
    const auto dawn=[=](double t) {
        const auto p=position(t), vertical=unit(p-center), ahead=flatten(courier(t).forward,vertical), side=cross(ahead,vertical);
        const auto eye=p-ahead*.55+vertical*.12+side*.10;
        return Shot{eye,eye+ahead*25-vertical*(25*std::tan(3*degree)),1};
    };
    const auto camera=[=](double t) -> Shot {
        if (t<55.3) return descent(t);
        if (t<59) return base_shot;
        if (t<64.3) return low_run(t);
        return orbit(low_run(t),dawn(t),position(t),ease(t,64.3,66.2));
    };
    Location location{stage,courier,camera};
    // Cheated: 6 degrees higher as the courier arrives, so the ground reads
    // as craters in the last light, sinking back as it dives (the sun then
    // sets faster); and from the cut to the low run a little to the left, so
    // it rises on the left third and rakes the crater walls.
    const auto lit=stage.direction(sun_direction);
    location.relight={{moon_cut,rotate(lit,{1,0,0},6*degree),Interpolation::hold},
                      {55.2,lit,Interpolation::linear},
                      {59,rotate(lit,{0,1,0},20*degree),Interpolation::hold}};
    location.cuts={55.3,59};
    location.jumps={flyby_begin,flyby_end};
    // The steep climb into the sunrise needs dense keys: linear keys every
    // eighth of a second would kink its screen motion at each key.
    location.dense={{56.5,59,1./30},{64.2,belt_cut,1./30}};
    return location;
}

// ==========================================================================
// The belt: into the rocks, a clearing, the fleet's arrival, the courier
// taking station beside the flagship, and the fleet's jump outward.
// Laid out directly in scene axes: the run goes along -Z with +Y up; the
// fleet sets out 60 degrees to its left, never along the X axis itself,
// where a craft's Euler angles would sit in gimbal lock.
// ==========================================================================
constexpr double stop_time=88.6, clearing_cut=86.8, join_begin=96.3, station_cut=97.6, join_end=100.8,
                 formation_cut=99.9, cruise_begin=101.3, jump_time=105.9;

Location belt(Voyage& v) {
    const V out{0,0,-1}, up{0,1,0}, right{1,0,0};
    // Directions by bearing (degrees right of -Z) and elevation.
    const auto bearing=[](double az, double el) {
        return V{std::sin(az*degree)*std::cos(el*degree),std::sin(el*degree),-std::cos(az*degree)*std::cos(el*degree)};
    };
    const auto fleet_heading=bearing(-60,0), port=cross(up,fleet_heading);
    // Where the Moon and Earth hang: off to the side of the run, outside the
    // chase shots, and behind the fleet as the last shot sees it.
    const auto moon_bearing=bearing(68,12), earth_bearing=bearing(74,17);
    const auto clearing=belt_stage_anchor;
    // Raking side light through the rocks and the arrivals; for the fleet in
    // formation, light from behind the camera's left so hulls and home are
    // lit toward it (a cut hides the change).
    const auto side_light=unit({.81,.58,.05}), formation_light=unit({-.296,.5,.814});
    const auto stage=Stage::lit(v.moon_center+moon_axis*21000,clearing,side_light);
    constexpr double step=1./240;
    const auto run_speed=[](double t){ return 5.2*(1-ease(t,84.6,stop_time)); };
    auto km=std::make_shared<std::vector<double>>(std::vector<double>{0});
    for (double t=belt_cut;t<stop_time+.3;t+=step) km->push_back(km->back()+run_speed(t)*step);
    const auto stop_km=(*km)[static_cast<std::size_t>((stop_time-belt_cut)/step)];
    const auto run=[=](double t) {
        const auto x=std::clamp((t-belt_cut)/step,0.,static_cast<double>(km->size()-1));
        const auto i=std::min(static_cast<std::size_t>(x),km->size()-2);
        return std::lerp((*km)[i],(*km)[i+1],x-static_cast<double>(i))-stop_km;
    };
    // Hero rocks sit on the flight line; each weave is a dodge around one.
    struct Hero { double at, radius; V away; };
    const std::array heroes{Hero{-62,.7,{1,0,0}},Hero{-47,1.2,unit({-.5,.8,0})},Hero{-32,.9,{-1,0,0}},
                            Hero{-19,1.5,unit({.6,-.7,0})}};
    const auto dodge=[=](double s) {
        V offset{};
        for (const auto& hero : heroes) {
            const auto x=(s-hero.at)/4.2;
            offset=offset+hero.away*((hero.radius+.28)*std::exp(-x*x));
        }
        return offset;
    };
    const auto run_point=[=](double t){ const auto s=run(t); return clearing+out*s+dodge(s); };
    const auto stop=run_point(stop_time);

    // The fleet arrives around the stopped courier in waves, far to near:
    // escorts, then frigates and wingmen, then carriers, then the flagship,
    // whose flank is the courier's slot. Stations are given as seen from the
    // clearing camera behind the courier: bearing and elevation in degrees,
    // and distance in kilometres.
    struct Vessel { u32 blueprint; const char* name; f32 scale; V bearing; double arrival, length; };
    const auto transport=static_cast<u32>(v.cast.transport), patrol=static_cast<u32>(v.cast.patrol),
               shuttle=static_cast<u32>(v.cast.shuttle), kestrel=static_cast<u32>(v.cast.courier);
    const std::array fleet{
        Vessel{shuttle,"MANTA / escort high",2.4F,{22,14,10.5},90.,.27},
        Vessel{shuttle,"MANTA / escort low",2.4F,{-30,-5,9.5},90.35,.27},
        Vessel{shuttle,"MANTA / escort near",2.4F,{27,2,7},90.6,.27},
        Vessel{patrol,"LANCER / frigate high",3.2F,{15,14,7},91.5,.62},
        Vessel{patrol,"LANCER / frigate low",3.2F,{-18,-12,7},91.8,.62},
        Vessel{kestrel,"KESTREL / wing one",1,{-33,-7,5.5},92.05,.11},
        Vessel{kestrel,"KESTREL / wing two",1,{-26,4,6},92.2,.11},
        Vessel{patrol,"LANCER / frigate rear",3.2F,{0,17,8.5},92.5,.62},
        Vessel{transport,"BASTION / carrier far",5.2F,{-31,13,5.5},93.4,1.41},
        Vessel{transport,"BASTION / carrier near",5.2F,{19,-2,6},93.9,1.41},
        Vessel{transport,"BASTION / flagship",8.5F,{-6,4,4.2},95.3,2.31}};
    const auto clearing_eye=stop+V{-.1,.1,.7};
    const auto station=[&](const Vessel& ship){ return clearing_eye+bearing(ship.bearing.x,ship.bearing.y)*ship.bearing.z; };
    const auto flagship=station(fleet.back());
    // The jumps run from the back of the formation to the front over two and
    // a half seconds; then the flagship; then, alone, the courier.
    std::vector<double> jumps(fleet.size());
    {
        std::vector<std::size_t> order(fleet.size()-1);
        for (std::size_t i=0;i<order.size();++i) order[i]=i;
        std::ranges::sort(order,{},[&](std::size_t i){ return dot(station(fleet[i]),fleet_heading); });
        for (std::size_t k=0;k<order.size();++k) jumps[order[k]]=jump_time+2.5*static_cast<double>(k)/static_cast<double>(order.size()-1);
        jumps.back()=jump_time+2.8;
    }
    const auto courier_jump=jump_time+4.6;
    // The formation sets off together once the courier has taken station.
    const auto cruise=[](double t) {
        const auto d=t-cruise_begin;
        return d<=0 ? 0. : d<6 ? .5*.02*d*d : .36+.12*(d-6);
    };
    const auto slot=flagship+port*1.6+up*.15;

    // The courier: the run, a stop, then one smooth climbing turn onto the
    // fleet's heading into its slot, on with the formation, and the jump: the
    // hull stretches ahead from its tail before it vanishes.
    const auto reach_km=length(slot-stop);
    const Curve join(stop,out*reach_km,slot,fleet_heading*reach_km);
    const auto position=[=](double t) {
        if (t<=stop_time) return run_point(t);
        if (t<=join_begin) return stop;
        const auto p=join.at(join.length_km()*ease(t,join_begin,join_end))+fleet_heading*cruise(t);
        return t>courier_jump ? p+fleet_heading*(.11*1.5*std::min(1.,(t-courier_jump)/.06)) : p;
    };
    const auto moving=[=](double t) {
        const auto d=position(std::max(t+.02,belt_cut+.04))-position(std::max(t-.02,static_cast<double>(belt_cut)));
        return length(d)>1e-6 ? unit(d) : out;
    };
    // Nose along the motion while it moves, along the run while stopped, and
    // along the fleet's heading once it has turned into its slot. Joining,
    // it climbs with its nose kept within 10 degrees of level: it slides up
    // into place rather than pitching up at the flagship.
    const auto levelled=[=](V m) {
        const auto pitch=std::clamp(std::asin(std::clamp(dot(m,up),-1.,1.)),-10*degree,10*degree);
        return unit(flatten(m,up)*std::cos(pitch)+up*std::sin(pitch));
    };
    const auto heading=[=](double t) {
        if (t<stop_time-1.5) return moving(t);
        if (t<join_begin) return slerp(moving(t),out,ease(t,stop_time-1.5,stop_time));
        if (t<join_end-1) return slerp(out,levelled(moving(t)),ease(t,join_begin,join_begin+.8));
        return slerp(levelled(moving(t)),fleet_heading,ease(t,join_end-1,join_end));
    };
    const auto flight=std::make_shared<const Flight>(belt_cut,duration+.1,position(belt_cut),heading,
        [=](double t){ return length(position(t+.01)-position(std::max(t-.01,static_cast<double>(belt_cut))))/.02; },
        [=](double, V){ return up; },.6);
    const auto courier=[=](double t){ auto pose=flight->at(t); pose.position=position(t); return pose; };

    // Rocks: darker, greyer and fewer, with hero rocks on the line, fragments
    // close to the path for parallax, and the lanes the fleet arrives and
    // leaves by kept clear, as is the view home past it.
    std::array<u32,3> blueprints{};
    std::array<double,3> reach{};
    for (u32 type=0;type<3;++type) {
        auto mesh=asteroids::rock_mesh(type);
        mesh.metadata["render/lighting"]="matte";
        for (auto& field : mesh.vertex_fields) {
            auto& values=std::get<std::vector<f32>>(field.values);
            if (field.name=="color/0")
                for (std::size_t i=0;i+2<values.size();i+=4) {
                    const auto grey=.3F*values[i]+.59F*values[i+1]+.11F*values[i+2];
                    for (std::size_t c=0;c<3;++c) values[i+c]=.44F*(grey+.4F*(values[i+c]-grey));
                }
            if (field.name=="position")
                for (std::size_t i=0;i+2<values.size();i+=3)
                    reach[type]=std::max(reach[type],length({values[i],values[i+1],values[i+2]}));
        }
        // Nine parts smooth to one part facet: the shared rocks mix in more
        // facet, which tiles large, close rocks like a quilt. The mesh gives
        // every face its own corners; welding equal positions recovers the
        // smooth normals.
        {
            const auto field=[&](const char* name) -> std::vector<f32>& {
                return std::get<std::vector<f32>>(std::ranges::find(mesh.vertex_fields,std::string(name),&content::vmesh::VertexField::name)->values);
            };
            const auto& position=field("position");
            auto& normal=field("normal");
            const auto at=[&](u32 i){ return V{position[3*i],position[3*i+1],position[3*i+2]}; };
            std::map<std::array<f32,3>,V> smooth_sum;
            const auto key=[&](u32 i){ return std::array{position[3*i],position[3*i+1],position[3*i+2]}; };
            for (const auto& face : mesh.faces) {
                const auto [a,b,c]=face.vertices;
                const auto n=cross(at(b)-at(a),at(c)-at(a));
                for (const auto i : {a,b,c}) smooth_sum[key(i)]=smooth_sum[key(i)]+n;
            }
            for (const auto& face : mesh.faces) {
                const auto [a,b,c]=face.vertices;
                const auto flat=unit(cross(at(b)-at(a),at(c)-at(a)));
                for (const auto i : {a,b,c}) {
                    const auto n=unit(unit(smooth_sum[key(i)])*.9+flat*.1);
                    normal[3*i]=static_cast<f32>(n.x); normal[3*i+1]=static_cast<f32>(n.y); normal[3*i+2]=static_cast<f32>(n.z);
                }
            }
        }
        blueprints[type]=v.add_blueprint(std::array{"BELT / basalt","BELT / iron","BELT / regolith"}[type],std::move(mesh));
    }
    // Fragments: a small faceted rock at a hundredth of a kilometre.
    auto pebble=space::fragment(5);
    for (auto& field : pebble.vertex_fields)
        if (field.name=="position") for (auto& x : std::get<std::vector<f32>>(field.values)) x*=.01F;
    const auto pebble_blueprint=v.add_blueprint("BELT / fragment",std::move(pebble));
    constexpr double pebble_reach=.012; // its farthest vertex at scale one, in km
    struct Rock { V position; double radius; u32 blueprint; f32 scale; };
    std::vector<Rock> rocks;
    u32 state=90210U;
    const auto random=[&]{ state=state*1664525U+1013904223U; return static_cast<double>(state>>8)/16777216.; };
    std::vector<V> path;
    for (double t=belt_cut;t<=duration;t+=.25) path.push_back(position(t));
    const auto clear=[&](V p, double r, double margin) {
        for (const auto& c : path) if (length(p-c)<r+margin) return false;
        for (const auto& ship : fleet) {
            const auto s=station(ship), d=p-s;
            const auto behind=-dot(d,fleet_heading);
            // Its station, and its lanes: 27 km back the way it arrives, 32
            // km ahead the way it leaves.
            if (length(d)<r+ship.length+2.5) return false;
            if (behind>-32 && behind<27 && length(d+fleet_heading*behind)<r+ship.length*.6+.6) return false;
        }
        for (const auto& other : rocks) if (length(p-other.position)<r+other.radius+.2) return false;
        return true;
    };
    const auto place_rock=[&](V p, double radius_km, double margin = .9) {
        const auto type=static_cast<u32>(random()*3)%3;
        const auto scale=std::max(.05,radius_km/reach[type]);
        const auto r=reach[type]*scale;
        if (!clear(p,r,margin)) return;
        rocks.push_back({p,r,blueprints[type],static_cast<f32>(scale)});
    };
    for (const auto& hero : heroes) {
        const auto type=static_cast<u32>(rocks.size())%3;
        rocks.push_back({clearing+out*hero.at,hero.radius,blueprints[type],static_cast<f32>(hero.radius/reach[type])});
    }
    const auto run_start=run(belt_cut);
    for (unsigned attempt=0;attempt<9000 && rocks.size()<210;++attempt) {
        const auto along=run_start-8+(-run_start+4)*std::pow(random(),.8);
        const auto spread=3+10*random(), angle=2*pi*random();
        place_rock(clearing+out*along+right*(std::cos(angle)*spread)+up*(std::sin(angle)*spread*.7),
                   .25+2.1*std::pow(random(),2.4));
    }
    for (unsigned attempt=0;attempt<5000 && rocks.size()<265;++attempt) {
        const auto d=unit(V{2*random()-1,.6*(2*random()-1),2*random()-1});
        const auto p=flagship+d*(16+26*random());
        if (dot(unit(p-flagship),moon_bearing)>.7) continue; // keep the view home clear
        place_rock(p,.3+2.6*std::pow(random(),2));
    }
    // Fragments within a few hundred metres of the run.
    const auto first_pebble=rocks.size();
    for (unsigned attempt=0;attempt<4000 && rocks.size()<first_pebble+70;++attempt) {
        const auto t=belt_cut+(86-belt_cut)*random();
        const auto around=courier(t);
        const auto sideways=cross(around.forward,up);
        const auto angle=2*pi*random(), spread=.25+.55*random();
        const auto p=around.position+sideways*(std::cos(angle)*spread)+up*(std::sin(angle)*spread);
        const auto r=.012+.06*std::pow(random(),2);
        if (!clear(p,r,.18)) continue;
        rocks.push_back({p,r,pebble_blueprint,static_cast<f32>(std::max(.05,r/pebble_reach))});
    }
    const auto span=duration-belt_cut;
    for (std::size_t i=0;i<rocks.size();++i) {
        const auto& rock=rocks[i];
        // Tumbling: under half a turn between keys, inside the +/-360 range.
        const auto rate=(random()*2-1)*std::min(6.,5/(.4+rock.radius));
        const auto first=[&](double turn){ return turn>0 ? -180+random()*(180-turn) : -turn-180+random()*(360+turn); };
        const Vec3 spin{static_cast<f32>(first(rate*span)),static_cast<f32>(first(.6*rate*span)),static_cast<f32>(random()*360-180)};
        const auto id=v.add(rock.blueprint,(i<first_pebble ? "BELT / rock " : "BELT / fragment ")+std::to_string(i+1),
                            rock.position,spin,rock.scale,false);
        for (const auto u : {0.,.5,1.})
            v.turn(id,belt_cut+span*u,{static_cast<f32>(spin.x+rate*span*u),static_cast<f32>(spin.y+.6*rate*span*u),spin.z},
                   u>0 ? Interpolation::linear : Interpolation::hold);
        v.show(id,belt_cut,true);
    }

    // Arrivals, heading outward: a point of light, then a white needle whose
    // head rides the nose as the hull, stretched out behind it, contracts
    // and coasts into place. Departures reverse it: the drives brighten, the
    // hull stretches ahead from its tail and vanishes, a needle races off
    // and a thin flash streaks along the way it went.
    auto streak=space::engine_plume(std::array{Vec3{0,0,0}},1);
    streak.metadata["name"]="WARP / streak";
    const auto streak_blueprint=v.add_blueprint("WARP / streak",std::move(streak));
    const auto facing=orientation(to(fleet_heading),to(up));
    // A needle is the unit streak cone at a tenth of the scale, so thin ones
    // stay within the editor's minimum axis scale.
    const auto needle_axes=[](double radius, double length_km) {
        const auto thin=static_cast<f32>(std::max(.05,radius*10));
        return Vec3{thin,thin,static_cast<f32>(std::max(.05,10*length_km))};
    };
    // A spark swelling to `radius` kilometres, optionally drawn out eightfold
    // along the fleet's heading.
    const auto flash=[&](std::string name, V at, double begin, double radius, double peak, double end, bool streaked) {
        const auto id=v.add(v.spark,std::move(name),at,streaked ? facing : Vec3{},.05F,false);
        if (streaked) v.stretch(id,begin,{1,1,8},Interpolation::hold);
        v.show(id,begin,true);
        v.keys.key({id,"scale"},static_cast<f32>(begin),.05F,Interpolation::hold);
        v.keys.key({id,"scale"},static_cast<f32>(peak),static_cast<f32>(std::max(.05,radius*10)),Interpolation::linear);
        v.keys.key({id,"scale"},static_cast<f32>(end),.05F,Interpolation::linear);
        v.show(id,end+.01,false);
    };
    const auto depart=[&](u32 id, const std::string& name, V center, double length, double t_j,
                          double spool, double stretch, double brightness, bool keyed_path) {
        v.glow(id,t_j-spool,brightness);
        v.glow(id,t_j,2.4);
        v.stretch(id,t_j,{1,1,1},Interpolation::hold);
        if (stretch>.1) {
            // Drawn out: the hull reaches ahead from its tail, then in the
            // last frames the tail races after the nose.
            v.stretch(id,t_j+stretch*.8,{1,1,4});
            v.stretch(id,t_j+stretch,{1,1,1.2F});
            if (!keyed_path) {
                v.place(id,t_j+stretch*.8,center+fleet_heading*(length*1.5),Interpolation::linear);
                v.place(id,t_j+stretch,center+fleet_heading*(length*2.9),Interpolation::linear);
            }
        } else {
            v.stretch(id,t_j+stretch,{1,1,4});
            if (!keyed_path) v.place(id,t_j+stretch,center+fleet_heading*(length*1.5),Interpolation::linear);
        }
        v.show(id,t_j+stretch+.01,false);
        const auto nose=center+fleet_heading*(length*.5);
        const auto launch=t_j+stretch*.8;
        const auto radius=std::max(.005,length*.015);
        const auto needle=v.add(streak_blueprint,"WARP / departure / "+name,nose,facing,.1F,false);
        v.show(needle,launch,true);
        for (double t=launch;t<=launch+.36+1e-6;t+=1./30) {
            const auto u=t-launch;
            const auto head=30*(1-std::exp(-u/.07));
            const auto incoming=u<1e-6 ? Interpolation::hold : Interpolation::linear;
            v.place(needle,t,nose+fleet_heading*(length*3*ease(t,t_j,t_j+stretch)+head),incoming);
            v.stretch(needle,t,needle_axes(radius*(1-.6*ease(u,.15,.34)),head*(1-ease(u,.1,.34))),incoming);
        }
        v.show(needle,launch+.37,false);
        flash("WARP / departure flash / "+name,nose,launch,std::clamp(length*.025,.004,.05),launch+2./30,launch+5./30,true);
    };
    for (std::size_t n=0;n<fleet.size();++n) {
        const auto& ship=fleet[n];
        const auto final_center=station(ship);
        const auto final_nose=final_center+fleet_heading*(ship.length*.5);
        const auto t0=ship.arrival, t_j=jumps[n];
        const bool flagship_class=n+1==fleet.size();
        // How far the nose still has to go: a fast exponential collapse, then
        // a coast that fades over three seconds. The hull trails the nose,
        // stretched sixfold at first and contracting.
        const auto remaining=[&](double t) {
            const auto u=std::max(0.,t-t0);
            return ship.length*.8*std::exp(-u/.08)+.375*std::pow(std::max(0.,1-u/3),2);
        };
        const auto stretch=[&](double t){ return 1+5*std::exp(-std::max(0.,t-t0)/.06); };
        const auto nose_at=[&](double t){ return final_nose-fleet_heading*remaining(t); };
        const auto center_at=[&](double t){ return nose_at(t)-fleet_heading*(ship.length*.5*stretch(t)); };
        const auto id=v.add(ship.blueprint,ship.name,final_center,facing,ship.scale,false);
        v.show(id,t0+.04,true);
        for (double t=t0+.04;t<=t0+.4+1e-6;t+=1./30) {
            const auto incoming=t<t0+.05 ? Interpolation::hold : Interpolation::linear;
            v.place(id,t,center_at(t),incoming);
            v.stretch(id,t,{1,1,static_cast<f32>(stretch(t))},incoming);
        }
        for (double t=t0+.6;t<=t0+3+1e-6;t+=.4) v.place(id,t,center_at(t),Interpolation::linear);
        v.glow(id,t0+.04,2,Interpolation::hold);
        v.glow(id,t0+1,1);
        // Held a little darker in formation, so the courier leads the eye:
        // the flagship's deck and the wingmen that share the courier's hull.
        const auto dim=flagship_class ? .7 : ship.blueprint==kestrel ? .65 : 1.;
        if (dim<1) { v.glow(id,formation_cut,1,Interpolation::hold); v.glow(id,formation_cut+1.5,dim); }
        // Then the formation eases outward together, until the jump.
        for (const auto t : {cruise_begin,cruise_begin+3,t_j})
            v.place(id,t,final_center+fleet_heading*cruise(t),Interpolation::linear);
        depart(id,ship.name,final_center+fleet_heading*cruise(t_j),ship.length,t_j,.6,flagship_class ? .45 : .06,dim,false);
        // The needle: head at the nose, tail back along the way it came.
        const auto needle=v.add(streak_blueprint,std::string("WARP / streak / ")+ship.name,nose_at(t0),facing,.1F,false);
        const auto radius=std::max(.005,ship.length*.015);
        v.show(needle,t0,true);
        for (double t=t0;t<=t0+.3+1e-6;t+=1./30) {
            const auto u=t-t0;
            const auto incoming=u<.01 ? Interpolation::hold : Interpolation::linear;
            v.place(needle,t,nose_at(t),incoming);
            v.stretch(needle,t,needle_axes(radius*(1-.6*ease(u,.18,.3)),std::min(20*ship.length,25.)*std::exp(-u/.06)),incoming);
        }
        v.show(needle,t0+.32,false);
        // A small point of light where the nose first appears, just before.
        flash(std::string("WARP / flash / ")+ship.name,nose_at(t0),t0-.3,std::clamp(ship.length*.06,.012,.05),t0,t0+.12,false);
    }
    // The courier goes last and alone, its drive swelling; its path already
    // carries the stretch.
    depart(v.cast.hero,"KESTREL / express courier",position(courier_jump),.11,courier_jump,1.3,.06,1,true);

    // Camera: the belt from its edge as the courier threads in past the
    // first rock; chasing it through; from above; leading it head-on until
    // it overtakes us; the clearing, where it brakes into frame and the fleet
    // arrives around it; behind it as it takes station; then beside it in
    // formation, drifting back until home is in view, holding through the
    // jump, and pushing in on home.
    const auto smoothed=[=](double t) {
        V heading{};
        for (int k=-5;k<=5;++k)
            heading=heading+courier(std::clamp(t+k*.06,static_cast<double>(belt_cut),static_cast<double>(duration))).forward*std::exp(-.5*k*k/6.25);
        return unit(heading);
    };
    // Out on the side the courier dodges to, so it threads in front of the
    // first hero rock and passes over a kilometre from the lens.
    const Shot threading{clearing+out*(-60)+right*2.2+up*.5,clearing+out*(-66)+right*.3,1};
    // Half a kilometre behind on a level frame and a smoothed heading: the
    // camera swings round smoothly as the courier weaves between the rocks.
    const auto chase=[=](double t) {
        const auto p=courier(t).position, forward=smoothed(t), side=unit(cross(forward,up));
        return Shot{p-forward*.52+up*.13+side*.05,p+forward*.6,1};
    };
    const auto top_down=[=](double t) {
        const auto pose=courier(t);
        return Shot{pose.position+up*.52-pose.forward*.1,pose.position+pose.forward*.08,1};
    };
    // Ahead of the courier and to its left, looking back at it: in the last
    // moments it overtakes the camera and leaves the frame on the left.
    const auto leading=[=](double t) {
        const auto p=courier(t).position, forward=smoothed(t), left=unit(cross(up,forward));
        const auto eye=p+forward*(.45-.55*ease(t,86.1,86.8))+left*.15+up*.05;
        return Shot{eye,eye+slerp(unit(p-eye),-forward,ease(t,85.9,86.4))*10,1.2};
    };
    const Shot clearing_shot{clearing_eye,clearing_eye+bearing(-6,4)*10,1};
    // Behind the courier and a little to starboard on a level frame and a
    // smoothed heading, looking a little toward the flagship's bridge, so the
    // flagship's stern sweeps past on the right as the courier comes about.
    const auto bridge=flagship+up*.3;
    const auto taking_station=[=](double t) {
        const auto p=courier(t).position;
        V heading{};
        for (int k=-6;k<=6;++k) heading=heading+courier(std::clamp(t+k*.1,join_begin,join_end+.5)).forward*std::exp(-.5*k*k/9.);
        const auto forward=unit(heading), starboard=unit(cross(forward,up));
        return Shot{p-forward*.45+starboard*.12+up*.03,lerp(p+forward*.6,bridge,.12),1};
    };
    // Beside the courier as it eases into its slot; drifting round toward its
    // bow and back to three quarters of a kilometre, until home is on the
    // right third; after the jump, a slow push in on home.
    const auto final_look=unit({.87,.14,-.47}), home_look=bearing(66,12);
    const auto formation=[=](double t) {
        const auto p=courier(std::min(t,courier_jump-.01)).position;
        const auto settle=ease(t,join_end,join_end+3.7);
        const auto a=(90-20*settle)*degree, d=std::lerp(.38,.75,settle), h=std::lerp(-.08,-.05,settle);
        const auto eye=p+fleet_heading*(std::cos(a)*d)+port*(std::sin(a)*d)+up*h;
        const auto early=unit(p+fleet_heading*.06+up*.02-eye);
        const auto look=slerp(slerp(early,final_look,settle),home_look,ease(t,courier_jump+.3,duration-.5));
        return Shot{eye,eye+look*10,std::lerp(1.,2.,ease(t,courier_jump+.3,duration-.3))};
    };
    const auto camera=[=](double t) -> Shot {
        if (t<74.9) return threading;
        if (t<78.6) return chase(t);
        if (t<80.8) return top_down(t);
        if (t<clearing_cut) return leading(t);
        if (t<station_cut) return clearing_shot;
        if (t<formation_cut) return taking_station(t);
        return formation(t);
    };
    Location location{stage,courier,camera,true,clearing+moon_bearing*60000,clearing+earth_bearing*150000};
    location.cuts={74.9,78.6,80.8,clearing_cut,station_cut,formation_cut};
    location.dense={{74.9,88.7,1./30},{85.85,86.45,1./60},{join_begin,formation_cut+1.5,1./30},
                    {courier_jump-.1,courier_jump+.2,1./60},
                    {courier_jump+.3,static_cast<double>(duration),1./15}};
    location.glow={{belt_cut,74.45}};
    location.relight={{formation_cut,formation_light,Interpolation::hold}};
    // Home turns toward the fleet: Earth its Arabian face, where the voyage
    // began, and the Moon its familiar near side.
    location.earth_facing=-earth_bearing;
    location.moon_facing=-moon_bearing;
    return location;
}

} // namespace

content::Result<void> author(project::State& state, KeyBatch& keys, const Handoff& handoff, const Cast& cast) try {
    Voyage v{state,keys,handoff,cast,from(handoff.earth_center),{}};
    v.moon_center=v.earth_center+moon_axis*moon_distance;
    const auto* earth_instance=project::find_instance(state,cast.earth);
    if (!earth_instance) throw std::runtime_error("The voyage needs the skyway's Earth instance");
    const auto earth_rotation=earth_instance->transform.rotation;
    v.flash=v.add_blueprint("KESTREL / drive glow",space::glow_sphere("KESTREL / drive glow",{.55F,.78F,1.F},14));
    auto spark=space::glow_sphere("WARP / spark",{.55F,.78F,1.F},14);
    for (auto& field : spark.vertex_fields)
        if (field.name=="position") for (auto& x : std::get<std::vector<f32>>(field.values)) x*=.1F;
    v.spark=v.add_blueprint("WARP / spark",std::move(spark));
    const std::array nozzles{Vec3{-.0159F,-.0011F,0},Vec3{.0159F,-.0011F,0}};
    const auto plume=v.add_blueprint("KESTREL / burn plume",space::engine_plume(nozzles,.006F,{.15F,.65F,1.F}));
    u32 moon_blueprint{}, base_blueprint{};
    const std::array locations{earth_orbit(v),moon_pass(v,moon_blueprint,base_blueprint),belt(v)};
    const std::array starts{static_cast<double>(handoff.time),static_cast<double>(moon_cut),static_cast<double>(belt_cut)};
    const auto at=[&](double t) -> const Location& { return locations[t<moon_cut ? 0 : t<belt_cut ? 1 : 2]; };
    const auto scene_courier=[&](double t) {
        const auto& location=at(t);
        auto pose=location.courier(t);
        if (!location.scene_axes)
            pose={location.stage.point(pose.position),location.stage.direction(pose.forward),location.stage.direction(pose.up),pose.speed};
        return pose;
    };
    const auto scene_camera=[&](double t) {
        const auto& location=at(t);
        auto shot=location.camera(t);
        if (!location.scene_axes) shot={location.stage.point(shot.eye),location.stage.point(shot.target),shot.zoom};
        return shot;
    };

    // Persistent bodies: Earth, the Moon (with its base) and the sun, placed
    // in each location's frame and jumping only at the cuts between them.
    const auto moon_id=v.add(moon_blueprint,"MOON / Selene",v.moon_center);
    const auto base_id=v.add(base_blueprint,"LUNAR / Serenity mining works",v.moon_center);
    const auto sun_id=state.document.next_instance_id++;
    // 9,600 km: the sun effect allows up to 10,000.
    state.document.instances.push_back({sun_id,project::BlueprintId::sun,"SOL / the sun",
        project::SunSettings{.radius=1.6F,.displacement=1,.bloom=.3F,.white_spots=true},
        {to(sun_direction*sun_distance),{},6000}});
    // Seen from this far the sun effect is a small orange disc; a slightly
    // larger white-hot core over it makes it blaze through bloom.
    const auto core_blueprint=v.add_blueprint("SOL / white-hot core",space::glow_sphere("SOL / white-hot core",{1.F,.94F,.84F},40));
    const auto core_id=v.add(core_blueprint,"SOL / white-hot core",sun_direction*sun_distance,{},9900);
    const auto earth_forward=turned(earth_rotation,{0,0,-1}), earth_up=turned(earth_rotation,{0,1,0});
    for (std::size_t i=0;i<locations.size();++i) {
        const auto& stage=locations[i].stage;
        const auto time=i==0 ? 0. : starts[i];
        // The least turn that brings `from` (a world direction on a body) round
        // to face `facing` (a scene direction), applied to its axes.
        const auto facing_rotation=[&](V from_world, V facing, V forward_world, V up_world) {
            const auto from_scene=stage.direction(from_world);
            const auto axis=unit(cross(from_scene,facing));
            const auto angle=std::acos(std::clamp(dot(from_scene,facing),-1.,1.));
            return orientation(to(rotate(stage.direction(forward_world),axis,angle)),to(rotate(stage.direction(up_world),axis,angle)));
        };
        for (const auto id : {moon_id,base_id}) {
            v.place(id,time,locations[i].moon.value_or(stage.point(v.moon_center)));
            if (const auto& facing=locations[i].moon_facing)
                v.turn(id,time,facing_rotation(unit(v.earth_center-v.moon_center),*facing,{0,0,-1},{0,1,0}));
            else
                v.turn(id,time,stage.rotation({0,0,-1},{0,1,0}));
        }
        v.place(cast.earth,time,locations[i].earth.value_or(stage.point(v.earth_center)));
        if (const auto& facing=locations[i].earth_facing) {
            v.turn(cast.earth,time,facing_rotation(unit(from(handoff.position)-v.earth_center),*facing,earth_forward,earth_up));
        } else {
            v.turn(cast.earth,time,stage.rotation(earth_forward,earth_up));
        }
        for (const auto id : {sun_id,core_id}) {
            v.place(id,time,stage.anchor+stage.direction(sun_direction)*sun_distance);
            for (const auto& [at,direction,incoming] : locations[i].relight)
                v.place(id,at,stage.anchor+direction*sun_distance,incoming);
        }
    }

    // The courier, the camera, the burn plume and the far drive glow, on
    // shared sample times. Every camera cut holds; the courier's path stays
    // continuous except where it moves to the next location.
    const auto plume_id=v.add(plume,"KESTREL / burn plume",from(handoff.position),{},1,false);
    v.show(plume_id,ignition,true);
    v.show(plume_id,moon_cut,false);
    // Far away the drive reads as a steady point of light about three pixels
    // across (a 0.1 km spark scaled to the distance); it fades in and out
    // over 0.4 s wherever a span starts or ends within a shot.
    const auto glow_id=v.add(v.spark,"KESTREL / drive glow",from(handoff.position),{},.05F,false);
    v.glow(glow_id,handoff.time,0,Interpolation::hold);
    for (std::size_t i=0;i<locations.size();++i)
        for (const auto& [begin,end] : locations[i].glow) {
            const auto opens=begin>starts[i]+1e-6, closes=i+1==locations.size() || end<starts[i+1]-1e-6;
            v.glow(glow_id,begin,opens ? 0 : .3,Interpolation::hold);
            v.glow(glow_id,begin+(opens ? .4 : 0),.3);
            if (closes) { v.glow(glow_id,end-.4,.3); v.glow(glow_id,end,0); }
        }
    std::vector<double> cuts{static_cast<double>(moon_cut),static_cast<double>(belt_cut)};
    Timing timing;
    timing.every(handoff.time,duration,.125);
    for (const auto& location : locations) {
        cuts.insert(cuts.end(),location.cuts.begin(),location.cuts.end());
        for (const auto& [begin,end,step] : location.dense) timing.every(begin,end,step);
        for (const auto t : location.jumps) timing.cut(t);
    }
    for (const auto t : cuts) timing.cut(t);
    bool glowing=false;
    for (const auto t : timing.sorted()) {
        const auto relocated=t==moon_cut || t==belt_cut || std::ranges::any_of(locations,[&](const Location& l){
            return std::ranges::find(l.jumps,t)!=l.jumps.end(); });
        const auto cut=std::ranges::find(cuts,t)!=cuts.end();
        const auto pose=scene_courier(t);
        v.pose(cast.hero,t,pose,relocated ? Interpolation::hold : Interpolation::linear);
        const auto shot=scene_camera(t);
        key_look(keys,cast.camera,static_cast<f32>(t),{to(shot.eye),to(shot.target),static_cast<f32>(shot.zoom)},
                 cut ? Interpolation::hold : Interpolation::linear);
        const auto exhaust=relative(pose,{0,0,nozzle_plane});
        if (t>=ignition-.2 && t<moon_cut) {
            v.pose(plume_id,t,{exhaust,pose.forward,pose.up,pose.speed});
            v.stretch(plume_id,t,{1,1,static_cast<f32>(.05+.04*ease(t,ignition,burn_time+1.))});
        }
        // Far away the drive reads as a point of light a few pixels across.
        const auto& location=at(t);
        const auto in_glow=std::ranges::any_of(location.glow,[&](const auto& span){ return t>=span[0] && t<span[1]; });
        const auto field=2*std::atan(std::tan(project::camera_vertical_fov*degree/2)/shot.zoom);
        const auto radius=length(shot.eye-pose.position)*1.5*field/800;
        if (in_glow!=glowing) { v.show(glow_id,t,in_glow); glowing=in_glow; }
        if (in_glow) {
            v.place(glow_id,t,pose.position,cut ? Interpolation::hold : Interpolation::linear);
            keys.key({glow_id,"scale"},static_cast<f32>(t),static_cast<f32>(std::max(.05,radius*10)),
                     cut ? Interpolation::hold : Interpolation::linear);
        }
    }
    if (glowing) v.show(glow_id,duration,false);
    auto& names=state.document.keyframe_names;
    names[handoff.time]="08 / Pull up into the sky";
    names[30]="09 / Earth and the gateway";
    names[32.8F]="10 / Threading the ring";
    names[39.4F]="11 / The hull under construction";
    names[static_cast<f32>(ignition)]="12 / Burn for the Moon";
    names[moon_cut]="13 / Down to the lunar night";
    names[55.3F]="14 / Earthrise over Serenity";
    names[59]="15 / Along the mass driver";
    names[static_cast<f32>(climb_begin)]="16 / Lunar sunrise";
    names[belt_cut]="17 / Into the belt";
    names[static_cast<f32>(clearing_cut)]="18 / The clearing";
    names[90]="19 / The fleet arrives";
    names[95.3F]="20 / The flagship";
    names[static_cast<f32>(station_cut)]="21 / Taking station";
    names[static_cast<f32>(formation_cut)]="22 / In formation";
    names[static_cast<f32>(jump_time)]="23 / The jump";
    names[static_cast<f32>(jump_time+4.6)]="24 / Home";
    state.document.timeline_duration=duration;
    return {};
} catch (const std::exception& error) {
    content::Diagnostic diagnostic;
    diagnostic.message=error.what();
    return std::unexpected(std::move(diagnostic));
}

project::WorldBounds bounds(const project::State& state) {
    project::WorldBounds result{{1e9F,1e9F,1e9F},{-1e9F,-1e9F,-1e9F}};
    for (const auto& instance : state.document.instances) {
        double reach=.1;
        if (const auto* sun=std::get_if<project::SunSettings>(&instance.settings)) reach=3.*sun->radius*instance.transform.scale;
        if (const auto* mesh=project::mesh_geometry(state,instance.blueprint)) {
            double farthest{};
            for (u32 i=0;i<mesh->size();++i) farthest=std::max(farthest,length(from(mesh->position(i))));
            const auto& axes=instance.transform.axis_scale;
            reach=farthest*instance.transform.scale*std::max({axes.x,axes.y,axes.z});
        }
        std::vector<Vec3> positions{instance.transform.position};
        if (const auto* track=state.document.timeline.find({instance.id,"position"}))
            for (const auto& key : track->keys) positions.push_back(std::get<Vec3>(key.value));
        for (const auto p : positions) for (unsigned c=0;c<3;++c) {
            result.minimum[c]=std::min(result.minimum[c],static_cast<f32>(p[c]-reach));
            result.maximum[c]=std::max(result.maximum[c],static_cast<f32>(p[c]+reach));
        }
    }
    for (unsigned c=0;c<3;++c) {
        result.minimum[c]=std::max(result.minimum[c],-project::scene_coordinate_limit);
        result.maximum[c]=std::min(result.maximum[c],project::scene_coordinate_limit);
    }
    return result;
}
} // namespace example::tunnel::voyage
