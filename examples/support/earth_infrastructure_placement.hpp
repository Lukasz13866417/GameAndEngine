#pragma once
#include "earth_infrastructure.hpp"
#include "earth_placement.hpp"
#include "earth_skyway.hpp"

namespace example::earth {
// One placement contract for hulls, sockets and editor handles. Scaffolds are
// generated afterwards in Earth space, so scaling/lifting a hull never lifts
// their ground contact points with it.
inline constexpr vng::f32 infrastructure_base_radius=1.012F;
inline vng::Mat4 infrastructure_frame(const InfrastructurePart& part,InfrastructureSettings settings={}) {
    const auto scale=part.scale*addon_scale(settings,part.kind);
    auto local=vng::Mat4::identity();
    for(unsigned c=0;c<3;++c)local[c][c]=scale;
    local[3][1]=infrastructure_base_radius*(1-scale)+part.altitude;
    return mesh_frame::compose(placement::frame(part.location,part.heading),local);
}
inline vng::f32 infrastructure_handle_base(const InfrastructurePart& p,InfrastructureSettings settings) {
    const auto scale=p.scale*addon_scale(settings,p.kind);
    if(p.kind==InfrastructureKind::skyway)return skyway_endpoint_radius+skyway_default_rise*settings.skyway_height*p.height*scale+
        (p.altitude_a+p.altitude_b)*.5F;
    const auto radius=p.kind==InfrastructureKind::elevator?1.012F+.25F*settings.hub_height*p.height:
        p.kind==InfrastructureKind::terminal?1.025F+.026F*settings.hub_height*p.height:
        p.kind==InfrastructureKind::joiner?1.025F+.045F*settings.hub_height*p.height:
        p.kind==InfrastructureKind::processor?1.012F+.12F*settings.hub_height*p.height:
        p.kind==InfrastructureKind::hub?1.012F+.106F*settings.hub_height*p.height:1.015F;
    return p.kind==InfrastructureKind::settlement?radius:infrastructure_base_radius+(radius-infrastructure_base_radius)*scale;
}
inline SkywaySample infrastructure_sample(const vng::Mat4& frame,SkywaySample p) {
    return {mesh_frame::point(frame,p.position),placement::unit(mesh_frame::vector(frame,p.tangent)),
        placement::unit(mesh_frame::vector(frame,p.side)),placement::unit(mesh_frame::vector(frame,p.up))};
}
}
