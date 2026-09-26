#pragma once
#include "earth_connections.hpp"

namespace example::earth::express_route {
// An ordinary authored local route; these helpers only place the example's
// kilometre coordinate system on Earth. No special geometry/rendering recipe.
inline constexpr std::string_view name="Arabian express / cinematic local";
inline constexpr vng::f32 radius_km=6371;
// The mouth is over the Empty Quarter (52 E, 22 N). Rotate the planet beneath
// the kilometre-space flight corridor, keeping its shared interior/haze frame.
inline constexpr vng::Vec3 center{0,-6530,0},rotation{0,38,68};
inline vng::Mat4 earth_to_world() {
    using namespace placement;
    auto m=mesh_frame::compose(turn({0,0,1},rotation.z*radians),turn({0,1,0},rotation.y*radians));
    for(unsigned c=0;c<3;++c)for(unsigned r=0;r<3;++r)m[c][r]*=radius_km;
    m[3]={center.x,center.y,center.z,1};return m;
}
inline InfrastructurePart recipe(vng::u32 id) {
    using namespace placement;
    const auto inverse=*mesh_frame::inverse(earth_to_world());
    const auto position=[](vng::f32 s){const auto a=s/radius_km;return Vec3{0,-2*radius_km*std::pow(std::sin(a*.5F),2.F),radius_km*std::sin(a)};};
    const auto tangent=[](vng::f32 s){const auto a=s/radius_km;return Vec3{0,std::sin(a),-std::cos(a)};};
    const auto a=position(1000),b=position(-12);
    const auto pa=mesh_frame::point(inverse,a),pb=mesh_frame::point(inverse,b);
    InfrastructurePart p;p.id=id;p.kind=InfrastructureKind::skyway;p.name=name;
    p.location=location(pa);p.end=location(pb);p.tunnel_class=TunnelSizeClass::local;
    p.altitude_a=std::hypot(pa.x,pa.y,pa.z)-skyway_endpoint_radius;
    p.altitude_b=std::hypot(pb.x,pb.y,pb.z)-skyway_endpoint_radius;
    const auto handle=4*radius_km/3*std::tan(1012/(4*radius_km));
    p.bezier_controls=std::vector<Vec3>{mesh_frame::point(inverse,add(a,mul(tangent(1000),handle))),
        mesh_frame::point(inverse,add(b,mul(tangent(-12),-handle)))};
    p.curve_segments=192;p.terminal_b=true;p.terminal_incline=8;
    p.scaffold_positions=std::vector<vng::f32>{.05F,.25F,.45F,.65F,.85F,.98F};
    return p;
}
}
