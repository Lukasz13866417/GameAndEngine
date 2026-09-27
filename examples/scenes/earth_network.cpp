#include "earth_scene.hpp"
#include "../support/earth_express_route.hpp"

namespace example::earth {
vng::content::Result<vng::content::vmesh::Document> redesign_infrastructure(const vng::content::vmesh::Document& source) {
    using namespace vng;
    using namespace placement;
    constexpr auto marker="earth/infrastructure/composition";
    if(source.metadata.contains(marker)&&source.metadata.at(marker)=="desert-express-2")return source;
    auto catalogue=infrastructure_parts(source);if(!catalogue)return std::unexpected(catalogue.error());
    content::Result<content::vmesh::Document> result{source};
    // Remove routes before their socket owners. Cities keep both their recipe
    // identities and actual baked geometry (including hand-painted edits).
    for(const bool tunnels_first:{true,false})for(const auto& p:*catalogue)
        if(p.kind!=InfrastructureKind::settlement&&(p.kind==InfrastructureKind::skyway)==tunnels_first) {
            result=remove_infrastructure(*result,p.id);if(!result)return result;
        }
    // No routes/hubs remain. Enable the new composition without invoking a
    // whole-infrastructure rebuild that would overwrite hand-edited cities.
    result->metadata["earth/infrastructure/skyways"]="true";
    result->metadata["earth/infrastructure/launch_hubs"]="true";
    const auto append=[&](InfrastructurePart p)->content::Result<void> {
        result=add_infrastructure(*result,p.kind,p.location);if(!result)return std::unexpected(result.error());
        auto parts=infrastructure_parts(*result);if(!parts)return std::unexpected(parts.error());
        p.id=parts->back().id;p.seed=p.id;
        result=edit_infrastructure(*result,p);if(!result)return std::unexpected(result.error());
        return {};
    };
    struct Corridor {const char* name;Vec2 a,b;f32 bend;};
    for(const auto& route:std::array{
        Corridor{"Pacific crescent",{100,22},{137,38},1.7F},
        Corridor{"Arabian coast",{37,25},{72,20},-1.8F},
        Corridor{"Mediterranean gateway",{-5,42},{35,33},1.5F},
        Corridor{"Pacific America",{-129,48},{-103,27},-1.5F}}) {
        for(unsigned lane=0;lane<2;++lane) {
            const auto offset=f32(lane)*.23F;
            InfrastructurePart p;p.kind=InfrastructureKind::skyway;
            p.name=std::string(route.name)+(lane?" / local lane":" / regional lane");
            p.location={route.a.x,route.a.y+offset};p.end={route.b.x,route.b.y+offset};
            p.tunnel_class=lane?TunnelSizeClass::local:TunnelSizeClass::regional;
            p.size=lane?1.F:.75F;p.height=.25F;p.terminal_b=!lane;
            // Low sweeping cubic instead of a high sine arch. Different
            // corridor lengths need different chord clearance from the globe.
            const auto separation=std::acos(std::clamp(dot(direction(p.location),direction(p.end)),-1.F,1.F));
            const auto control_radius=1.037F+.17F*separation*separation;
            p.bezier_controls.emplace();
            for(const auto t:{1.F/3,2.F/3})p.bezier_controls->push_back(mul(direction({
                std::lerp(p.location.x,p.end.x,t),std::lerp(p.location.y,p.end.y,t)+route.bend}),control_radius));
            p.curve_segments=40;
            p.scaffold_positions=std::vector<f32>{0,.38F,.72F,1};
            if(auto added=append(std::move(p));!added)return std::unexpected(added.error());
        }
    }
    // Keep the flight's physical route. Its adjacent lanes are spherical rigid
    // offsets, close and coherent but still independent editable recipes.
    for(unsigned lane=0;lane<3;++lane) {
        auto p=express_route::recipe(1);
        if(lane) {
            p.name=lane==1?"Arabian express / inbound lane":"Arabian express / service lane";
            const auto midpoint=infrastructure_center(p);
            // The express runs roughly east/west. Offset north across its
            // direction, far enough to clear the wider dispersal mouths too.
            const auto delta=between(direction(midpoint),direction({midpoint.x,midpoint.y+.45F*f32(lane)}));
            p.location=location(mesh_frame::vector(delta,direction(p.location)));
            p.end=location(mesh_frame::vector(delta,direction(p.end)));
            for(auto& v:*p.bezier_controls)v=mesh_frame::vector(delta,v);
            p.curve_segments=64;p.size=lane==1?1.F:.8F;
            p.scaffold_positions=std::vector<f32>{.12F,.5F,.9F};
        }
        if(auto added=append(std::move(p));!added)return std::unexpected(added.error());
    }
    struct Gateway {const char* name;Vec2 point;f32 heading;};
    for(const auto& gateway:std::array{
        Gateway{"Arabia / departure arcology",{52.8F,24.2F},-40},
        Gateway{"Arabia / western exchange",{49.8F,22.8F},35},
        Gateway{"Arabia / eastern exchange",{55.2F,23.4F},-25},
        Gateway{"Arabia / southern port",{53.8F,20.5F},20},
        Gateway{"Arabia / northern port",{50.8F,25.5F},-20}}) {
        InfrastructurePart p;p.kind=InfrastructureKind::hub;p.name=gateway.name;
        p.location=gateway.point;p.heading=gateway.heading;p.size=.8F;p.scale=.55F;p.height=.75F;p.scaffold=false;
        if(auto added=append(std::move(p));!added)return std::unexpected(added.error());
    }
    result->metadata[marker]="desert-express-2";
    result->metadata.erase("earth/infrastructure/global-corridors");
    result->metadata["earth/infrastructure/network-revision"]="desert-long-haul-3";
    return result;
}

vng::content::Result<vng::content::vmesh::Document> expand_global_infrastructure(
    const vng::content::vmesh::Document& source) {
    using namespace vng;
    using namespace placement;
    constexpr auto marker="earth/infrastructure/global-corridors";
    // Existing authored additions are never silently replaced. The explicit
    // redesign operation clears this marker when replacing the whole network.
    if(source.metadata.contains(marker))return source;
    content::Result<content::vmesh::Document> result{source};
    // Enable new geometry without rebuilding/repainting existing cities or
    // changing the cinematic express. Every addition remains an editable part.
    result->metadata["earth/infrastructure/skyways"]="true";
    result->metadata["earth/infrastructure/launch_hubs"]="true";
    const auto append=[&](InfrastructurePart p)->content::Result<void> {
        result=add_infrastructure(*result,p.kind,p.location);if(!result)return std::unexpected(result.error());
        auto parts=infrastructure_parts(*result);if(!parts)return std::unexpected(parts.error());
        p.id=parts->back().id;p.seed=p.id;
        result=edit_infrastructure(*result,p);if(!result)return std::unexpected(result.error());
        return {};
    };
    struct Corridor {const char* name;Vec2 a,b;f32 bend;bool gateway;};
    const std::array routes{
        Corridor{"North Pacific / oceanic arc",{-178,30},{-138,40},2.2F,true},
        Corridor{"North Atlantic / oceanic arc",{-72,42},{-22,48},-2.F,true},
        Corridor{"Arctic / aurora link",{-40,65},{15,68},2.F,false},
        Corridor{"Eurasia / northern link",{35,55},{86,63},-1.8F,true},
        Corridor{"Siberia / polar link",{88,66},{144,62},2.F,false},
        Corridor{"West Pacific / island arc",{140,26},{179,36},-2.4F,true},
        Corridor{"South Pacific / oceanic arc",{-173,-43},{-127,-36},-2.1F,true},
        Corridor{"Southeast Pacific / oceanic arc",{-115,-40},{-66,-49},2.3F,false},
        Corridor{"South Atlantic / oceanic arc",{-49,-34},{0,-44},-1.9F,true},
        Corridor{"Southern Africa / cape link",{10,-43},{58,-36},2.1F,true},
        Corridor{"Southern Indian / oceanic arc",{68,-48},{119,-40},-2.2F,false},
        Corridor{"Australasia / southern link",{130,-35},{177,-45},2.F,true},
        Corridor{"Equatorial Africa / inland link",{-14,12},{27,4},1.8F,true},
        Corridor{"Central Pacific / equatorial arc",{-166,5},{-115,-5},-2.2F,true},
        Corridor{"Indian Ocean / equatorial arc",{58,-5},{106,-16},2.F,true},
        Corridor{"Andes / continental spine",{-78,3},{-66,-36},-1.5F,true},
        Corridor{"Australia / inland spine",{116,-24},{153,-29},1.5F,true},
        Corridor{"North America / inland spine",{-112,30},{-72,48},1.8F,false},
        Corridor{"East Pacific / meridian",{-114,25},{-100,-18},-1.2F,false},
        Corridor{"West Pacific / meridian",{161,14},{152,-28},1.8F,false},
        Corridor{"Arabia / red sea trunk",{32,17},{58,25},-2.8F,false},
        Corridor{"Arabia / gulf high line",{39,30},{68,25},1.8F,false},
        Corridor{"Arabia / southern bypass",{41,17},{68,16},-1.2F,false},
        Corridor{"Arabia / northbound spine",{49,8},{58,37},2.F,false},
        Corridor{"Arabia / gulf freight lane",{39,30.55F},{68,25.55F},1.8F,false},
    };
    for(std::size_t i=0;i<routes.size();++i) {
        const auto& route=routes[i];
        InfrastructurePart p;p.kind=InfrastructureKind::skyway;p.name=route.name;
        p.location=route.a;p.end=route.b;p.height=.3F;
        p.tunnel_class=i%3==0?TunnelSizeClass::trunk:TunnelSizeClass::regional;
        p.size=i%3==0?.7F:.95F;
        const auto separation=std::acos(std::clamp(dot(direction(p.location),direction(p.end)),-1.F,1.F));
        const auto control_radius=1.043F+.17F*separation*separation;
        p.bezier_controls.emplace();
        for(const auto t:{1.F/3,2.F/3})p.bezier_controls->push_back(mul(direction({
            std::lerp(p.location.x,p.end.x,t),std::lerp(p.location.y,p.end.y,t)+route.bend}),control_radius));
        // Orbital corridors need fewer samples than the cinematic close-up.
        // Sparse ocean pylons and separate compact gateways keep the vertex
        // budget available for subsequent hand editing.
        p.curve_segments=32;p.scaffold_positions=std::vector<f32>{0,.5F,1};
        if(auto added=append(std::move(p));!added)return std::unexpected(added.error());
        if(route.gateway) {
            InfrastructurePart hub;hub.kind=InfrastructureKind::hub;
            hub.name=std::string(route.name)+" / gateway";
            hub.location={route.b.x+.45F,route.b.y+.35F};
            hub.heading=i%2?35.F:-35.F;hub.size=.8F;hub.scale=.65F;hub.height=1.7F;hub.scaffold=false;
            if(auto added=append(std::move(hub));!added)return std::unexpected(added.error());
        }
    }
    result->metadata[marker]="2";
    return result;
}
}
