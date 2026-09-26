#include "earth_infrastructure_controls.hpp"
#include "earth_scaffold_gizmos.hpp"
#include "earth_bezier_gizmo.hpp"
#include "../support/earth_assets.hpp"
#include "../support/earth_placement.hpp"
#include "../support/earth_skyway.hpp"
#include "../support/earth_connections.hpp"
#include "../support/earth_infrastructure_placement.hpp"
#include <cctype>

namespace editor_example {
namespace {
namespace earth=example::earth;
using namespace vng;
using Part=earth::InfrastructurePart;
template<class F> MeshDraftEdit edit(std::string label,F apply,bool select=false) {
    return {std::move(label),[apply=std::move(apply)](const editor::EditableMesh& source)->content::Result<editor::EditableMesh> {
        auto result=apply(source.document());if(!result)return std::unexpected(result.error());
        return editor::EditableMesh::create(std::move(*result));
    },select};
}
}
editor::Result<void> append_infrastructure_menus(BlueprintMeshDescription& description,const content::vmesh::Document& d) {
    auto settings=earth::infrastructure_settings(d);
    if(!settings)return std::unexpected(editor::Diagnostic{settings.error().message});
    description.menus.push_back({"Addon baseline scales...",[settings=*settings](editor::Inspector& ui,SubmitMeshDraftEdit submit) {
        using S=earth::InfrastructureSettings;
        auto scale=ui.edit("earth_addon_scales",settings,"Baseline scale x type coefficient x part size");
        scale.slider("addon_scale",&S::addon_scale,.1F,4,"All addons / baseline scale");
        scale.slider("settlement_scale",&S::settlement_scale,.1F,4,"Settlements coefficient");
        scale.slider("tunnel_scale",&S::tunnel_scale,.1F,4,"Tunnels coefficient");
        scale.slider("launch_pad_scale",&S::launch_pad_scale,.1F,4,"Launch pads coefficient");
        scale.slider("terminal_scale",&S::terminal_scale,.1F,4,"Dispersal terminals coefficient");
        scale.slider("elevator_scale",&S::elevator_scale,.1F,4,"Orbital elevators coefficient");
        scale.slider("joiner_scale",&S::joiner_scale,.1F,4,"Tunnel joiners coefficient");
        scale.slider("processor_scale",&S::processor_scale,.1F,4,"Atmospheric processors coefficient");
        scale.apply("Apply addon scales",[submit](const S& next){
            submit(edit("Addon baseline scales",[next](const auto& source){return earth::rebuild_infrastructure(source,next);}));
        });
    }});
    return {};
}
editor::Result<void> append_infrastructure_parts(BlueprintMeshDescription& description,const content::vmesh::Document& d) {
    if(!earth::has_infrastructure_parts(d))return {};
    auto parts=earth::infrastructure_parts(d);if(!parts)return std::unexpected(editor::Diagnostic{parts.error().message});
    for(const auto& p:*parts)description.parts.push_back({{std::string(earth::infrastructure_part_field),p.id},p.name+(p.visible?"":" (hidden)")});
    return {};
}
editor::Result<bool> describe_infrastructure_parts(editor::Inspector& ui,BlueprintMeshDescription& description,
    const content::vmesh::Document& d,SubmitMeshDraftEdit submit,const MeshPartId& selected) {
    auto settings=earth::infrastructure_settings(d);if(!settings)return std::unexpected(editor::Diagnostic{settings.error().message});
    auto frame=example::mesh_frame::read(d);if(!frame)return std::unexpected(editor::Diagnostic{frame.error().message});
    if(!earth::has_infrastructure_parts(d)&&std::ranges::any_of(d.vertex_fields,[](const auto& f){return f.name=="earth/infrastructure";})) {
        ui.action("enable_infrastructure_parts",[submit] {
            submit(edit("Infrastructure part migration",[](const auto& source) {
                auto settings=earth::infrastructure_settings(source);
                if(!settings)return content::Result<content::vmesh::Document>{std::unexpected(settings.error())};
                return earth::rebuild_infrastructure(source,*settings);
            }));
        },"Enable infrastructure editing (rebuild)");
        return false;
    }
    for(auto kind:{earth::InfrastructureKind::settlement,earth::InfrastructureKind::skyway,earth::InfrastructureKind::hub,
            earth::InfrastructureKind::terminal,earth::InfrastructureKind::elevator,earth::InfrastructureKind::joiner,earth::InfrastructureKind::processor}) {
        auto name=std::string(earth::infrastructure_name(kind));name[0]=char(std::tolower(static_cast<unsigned char>(name[0])));
        const auto label="Place "+name;
        description.placements.push_back({label,*frame,[kind](Vec3 point) {
            return edit("New infrastructure",[kind,location=earth::placement::location(point)](const auto& source){return earth::add_infrastructure(source,kind,location);},true);
        }});
    }
    if(selected.field!=earth::infrastructure_part_field)return false;
    auto parts=earth::infrastructure_parts(d);if(!parts)return std::unexpected(editor::Diagnostic{parts.error().message});
    const auto found=std::ranges::find(*parts,selected.value,&Part::id);if(found==parts->end())return false;
    const auto p=*found;
    auto values=ui.edit("infrastructure_part",p,"Infrastructure part / mesh draft");
    values.field("name",&Part::name,"Part name");
    values.toggle("visible",&Part::visible,"Part visible");
    values.slider("size",&Part::size,.25F,4,"Width / footprint");
    if(p.kind!=earth::InfrastructureKind::settlement) {
        if(p.kind!=earth::InfrastructureKind::skyway||!p.bezier_controls)
            values.slider("height",&Part::height,.25F,4,"Height / rise");
        if(p.kind!=earth::InfrastructureKind::skyway)values.field("altitude",&Part::altitude,"Altitude (Earth radii)");
    }
    if(p.kind==earth::InfrastructureKind::skyway) {
        MeshPartChoice size_class{"Tunnel size class (nominal bore)",
            {"Local / 6 km", "Regional / 24 km", "Trunk / 45.4 km"},std::size_t(p.tunnel_class),{}};
        size_class.choose=[p](std::size_t index) {
            auto next=p;next.tunnel_class=earth::TunnelSizeClass(index);
            return edit("Tunnel size class",[next](const auto& source){return earth::edit_infrastructure(source,next);});
        };
        description.choices.push_back(std::move(size_class));
        if(!p.socket_a)values.toggle("terminal_a",&Part::terminal_a,"Dispersal terminal at A");
        if(!p.socket_b)values.toggle("terminal_b",&Part::terminal_b,"Dispersal terminal at B");
        if(p.terminal_a||p.terminal_b)values.slider("terminal_incline",&Part::terminal_incline,0,60,"Mouth incline (degrees)");
        if(!p.socket_a)values.field("altitude_a",&Part::altitude_a,"End A altitude (Earth radii)");
        if(!p.socket_b)values.field("altitude_b",&Part::altitude_b,"End B altitude (Earth radii)");
        const auto sockets=earth::infrastructure_sockets(*parts,*settings);
        for(bool end:{false,true}) {
            const auto current=end?p.socket_b:p.socket_a;
            MeshPartChoice choice{end?"End B connection":"End A connection",{"Free endpoint (detached)"},0,{}};
            MeshPartConnection connection{end?"Attach endpoint B in viewport":"Attach endpoint A in viewport",{}};
            std::vector<earth::TunnelSocketRef> refs{{}};
            for(const auto& socket:sockets) {
                const auto occupied=std::ranges::any_of(*parts,[&](const auto& part){return part.socket_a==socket.id||part.socket_b==socket.id;});
                if(occupied&&socket.id!=current)continue;
                const auto owner=std::ranges::find(*parts,socket.id.part,&Part::id);
                const bool visible=owner!=parts->end()&&owner->visible&&earth::infrastructure_enabled(owner->kind,*settings);
                if(!visible&&socket.id!=current)continue;
                if(socket.id==current)choice.selected=refs.size();
                refs.push_back(socket.id);choice.options.push_back(socket.label);
                if(visible) {
                    const MeshPartId target_id{std::string(earth::infrastructure_part_field),socket.id.part};
                    auto target=std::ranges::find(connection.targets,target_id,&MeshPartSocketTarget::part);
                    if(target==connection.targets.end()) {connection.targets.push_back({target_id,{}});target=std::prev(connection.targets.end());}
                    const auto radius=std::hypot(socket.position.x,socket.position.y,socket.position.z);
                    target->slots.push_back({{{},socket.position,radius,socket.label.substr(socket.label.rfind(" / ")+3),*frame,false},
                        [id=p.id,end,ref=socket.id] {return edit("Tunnel connection",[id,end,ref](const auto& source) {
                            return earth::connect_infrastructure_endpoint(source,id,end,ref);
                        });}});
                }
            }
            choice.choose=[refs=std::move(refs),id=p.id,end](std::size_t index) {
                return edit("Tunnel connection",[id,end,ref=refs.at(index)](const auto& source) {
                    return earth::connect_infrastructure_endpoint(source,id,end,ref);
                });
            };
            description.choices.push_back(std::move(choice));
            description.connections.push_back(std::move(connection));
        }
    }
    values.apply("Apply part properties",[submit](const Part& next){submit(edit("Infrastructure properties",[next](const auto& source){return earth::edit_infrastructure(source,next);}));});
    struct Location {f32 longitude,latitude;};
    const auto center=earth::infrastructure_center(p);
    const bool connected=bool(p.socket_a)||bool(p.socket_b);
    if(!connected) {
        auto location=ui.edit("infrastructure_location",Location{center.x,center.y},"Surface placement");
        location.slider("longitude",&Location::longitude,-180,180,"Longitude (degrees)");
        location.slider("latitude",&Location::latitude,-90,90,"Latitude (degrees)");
        location.apply("Move part",[submit,id=p.id](const Location& next){submit(edit("Infrastructure placement",[id,next](const auto& source){return earth::move_infrastructure(source,id,{next.longitude,next.latitude});}));});
    }
    ui.action("remove_infrastructure_part",[submit,id=p.id]{submit(edit("Remove infrastructure",[id](const auto& source){return earth::remove_infrastructure(source,id);}));},"Remove part");
    const bool shown=p.visible&&earth::infrastructure_enabled(p.kind,*settings);
    if(shown) {
        const auto base=earth::infrastructure_handle_base(p,*settings),radius=base+p.altitude;
        const bool lift=p.kind!=earth::InfrastructureKind::skyway&&p.kind!=earth::InfrastructureKind::settlement;
        if(!connected)description.gizmos.push_back({"Whole part",{{{{},earth::placement::mul(earth::placement::direction(center),radius),radius,"Whole part",*frame,true,
            lift?std::optional{Vec2{base,base+earth::max_infrastructure_altitude}}:std::nullopt},
            [id=p.id,lift](Vec3 point){return edit("Infrastructure placement",[id,lift,point](const auto& source){return lift?
                earth::place_infrastructure(source,id,point):earth::move_infrastructure(source,id,earth::placement::location(point));});},
            [id=p.id](f32 angle){return edit("Infrastructure rotation",[id,angle](const auto& source){return earth::rotate_infrastructure(source,id,angle);});}}}});
        if(!connected&&p.kind!=earth::InfrastructureKind::skyway&&p.kind!=earth::InfrastructureKind::settlement)
            description.gizmos.back().options=[p](editor::Inspector& menu,SubmitMeshDraftEdit submit){describe_scaffold_visibility(menu,std::move(submit),p);};
        if(p.kind==earth::InfrastructureKind::skyway) {
            MeshPartGizmo endpoints{"Tunnel endpoints",{}};
            for(bool end:{false,true}) {
                if(end?bool(p.socket_b):bool(p.socket_a))continue;
                const auto anchor=end?p.end:p.location;
                const auto radius=earth::skyway_endpoint_radius+(end?p.altitude_b:p.altitude_a);
                endpoints.handles.push_back({{{},earth::placement::mul(earth::placement::direction(anchor),radius),radius,end?"Endpoint B":"Endpoint A",*frame,false,
                    Vec2{earth::skyway_endpoint_radius,earth::skyway_endpoint_radius+earth::max_tunnel_endpoint_altitude}},
                    [id=p.id,end](Vec3 point){return edit("Tunnel endpoint",[id,end,point](const auto& source){return earth::place_infrastructure_endpoint(source,id,end,point);});},{}});
            }
            if(!endpoints.handles.empty())description.gizmos.push_back(std::move(endpoints));
            const auto curve=earth::TunnelCurve::create(p,*parts,*settings);
            if(!curve)return std::unexpected(editor::Diagnostic{curve.error().message});
            append_bezier_gizmo(description,p,*curve,*frame);
            if(auto tools=append_scaffold_gizmos(description,p,*curve,*frame);!tools)return std::unexpected(tools.error());
        }
        auto anchor=earth::placement::mul(earth::placement::direction(center),radius);
        if(p.kind==earth::InfrastructureKind::skyway) {
            const auto curve=earth::TunnelCurve::create(p,*parts,*settings);
            if(!curve)return std::unexpected(editor::Diagnostic{curve.error().message});
            anchor=curve->sample(.5F).position;
        }
        MeshPartHandle scale_handle;
        scale_handle.surface={{},anchor,radius,"Part scale (S)",*frame,false,{}, {},PartScale{p.scale,20}};
        scale_handle.scale=[p](f32 value){auto next=p;next.scale=value;
            return edit("Addon scale",[next](const auto& source){return earth::edit_infrastructure(source,next);});};
        description.gizmos.push_back({"Scale part (S)",{std::move(scale_handle)},
            [p](editor::Inspector& menu,SubmitMeshDraftEdit submit) {
                auto size=menu.edit("addon_part_scale",p,"Individual addon scale");
                size.slider("scale",&Part::scale,.05F,20,"Part scale");
                size.apply("Apply part scale",[submit](const Part& next){
                    submit(edit("Addon scale",[next](const auto& source){return earth::edit_infrastructure(source,next);}));
                });
            }});
    }
    description.hint=!shown?"Part hidden. Placement remains editable.":connected?
        "Connected ends follow their structures. Detach to move; free ends have an up/down handle.":
        p.kind==earth::InfrastructureKind::skyway?
        "Scaffold gizmos: move/delete, add, or uniform setup. Scale part (S): width and rise.":
        "G/R: move/turn. Scale part (S): individual size. Up/down lifts structures; scaffold toggle in gizmo options.";
    return true;
}
}
