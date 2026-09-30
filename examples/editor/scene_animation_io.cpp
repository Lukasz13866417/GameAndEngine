#include "scene_animation.hpp"
#include <iomanip>

namespace editor_example {
using namespace vng;
void write_scene_animation(std::ostream& out,const AnimationSettings& a) {
    const auto vec=[&](Vec3 p){out<<'['<<p.x<<','<<p.y<<','<<p.z<<']';};
    out<<"{ version = 1; first = "<<a.interval.first<<"; last = "<<a.interval.last<<"; enabled = "<<a.enabled<<"; ";
    if(const auto* d=std::get_if<DepartureSequence>(&a.root)) {
        out<<"kind = \"departure\"; ship = "<<d->ship<<"; camera = "<<d->camera<<"; route = [";
        for(auto p:d->motion.route.points){vec(p);out<<',';}out<<"]; initial_rotation = ";vec(d->motion.initial_rotation);
        out<<"; initial_speed = "<<d->motion.speed.initial<<"; final_speed = "<<d->motion.speed.final<<"; ramp = "<<d->motion.speed.ramp
           <<"; amplitude = "<<d->motion.turbulence.amplitude<<"; frequency = "<<d->motion.turbulence.frequency
           <<"; orient = "<<d->motion.orient_to_path<<"; camera_offset = ";vec(d->follow.offset);
        out<<"; camera_rotation = ";vec(d->follow.rotation);out<<"; look_at_ship = "<<d->follow.look_at_ship<<"; }";
    } else {
        const auto& spin=std::get<SpinAnimation>(a.root);
        out<<"kind = \"spin\"; target = "<<spin.target<<"; initial_rotation = ";vec(spin.initial_rotation);
        out<<"; angular_speed = ";vec(spin.degrees_per_second);out<<"; }";
    }
}
AnimationSettings read_scene_animation(content::Reader r) {
    if(r.get<u32>("version")!=1)r.fail("Unknown animation-tree version");
    AnimationSettings result{{r.get<f32>("first"),r.get<f32>("last")},r.get<bool>("enabled"),{}};
    const auto kind=r.get<std::string>("kind");
    if(kind=="departure") {
        DepartureSequence d;d.ship=r.get<u32>("ship");d.camera=r.get<u32>("camera");
        auto points=r.get<std::vector<Vec3>>("route");if(points.size()!=4)r.fail("A cubic route needs four control points");
        std::copy(points.begin(),points.end(),d.motion.route.points.begin());
        d.motion.initial_rotation=r.get<Vec3>("initial_rotation");
        d.motion.speed={r.get<f32>("initial_speed"),r.get<f32>("final_speed"),r.get<f32>("ramp")};
        d.motion.turbulence={r.get<f32>("amplitude"),r.get<f32>("frequency")};
        d.motion.orient_to_path=r.get<bool>("orient");d.follow.offset=r.get<Vec3>("camera_offset");
        d.follow.rotation=r.get<Vec3>("camera_rotation");d.follow.look_at_ship=r.get<bool>("look_at_ship");result.root=d;
    } else if(kind=="spin")result.root=SpinAnimation{r.get<u32>("target"),r.get<Vec3>("initial_rotation"),r.get<Vec3>("angular_speed")};
    else r.fail("Unknown animation-tree kind");
    return result;
}
}
