#include "earth_assets.hpp"
#include "earth_infrastructure_detail.hpp"
#include "earth_connections.hpp"
#include "earth_placement.hpp"
#include "earth_skyway.hpp"
#include "earth_infrastructure_placement.hpp"
#include <charconv>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>

namespace example::earth {
namespace {
using namespace vng;
namespace vm=content::vmesh;
using Document=vm::Document;
constexpr std::string_view prefix="earth/infrastructure/part/",version="earth/infrastructure/parts-version",next_key="earth/infrastructure/next-id";
constexpr u32 max_id=1000000;
auto error(std::string text){return std::unexpected(mesh_frame::error(std::move(text)));}
std::string key(u32 id){return std::string(prefix)+std::to_string(id);}
content::Result<u32> integer(std::string_view text) {
    u32 value{};const auto [end,ec]=std::from_chars(text.data(),text.data()+text.size(),value);
    if(ec!=std::errc{}||end!=text.data()+text.size()||!value||value>max_id)return error("Invalid infrastructure identity");
    return value;
}
content::Result<std::vector<u32>> selected(const Document& d,u32 id) {
    if(auto checked=vm::validate(d);!checked)return std::unexpected(checked.error());
    const auto f=std::ranges::find(d.vertex_fields,infrastructure_part_field,&vm::VertexField::name);
    if(f==d.vertex_fields.end()||f->type!=vm::FieldType{vm::ScalarType::UInt32,1})return error("Rebuild infrastructure once to enable part editing");
    const auto& owners=std::get<std::vector<u32>>(f->values);
    const auto mixed=[&](auto primitive){unsigned n{};for(auto v:primitive.vertices)n+=owners[v]==id;return n&&n!=primitive.vertices.size();};
    for(auto face:d.faces)if(mixed(face))return error("Separate faces joining this infrastructure part before moving it");
    if(d.edges)for(auto edge:*d.edges)if(mixed(edge))return error("Separate edges joining this infrastructure part before moving it");
    std::vector<u32> result;for(u32 i=0;i<owners.size();++i)if(owners[i]==id)result.push_back(i);return result;
}
bool depends_on(const InfrastructurePart& p,u32 id){return p.socket_a.part==id||p.socket_b.part==id;}
content::Result<Document> refresh(Document result,std::span<const InfrastructurePart> parts,InfrastructureSettings settings,std::span<const u32> ids) {
    if(auto checked=validate_infrastructure_connections(parts,settings);!checked)return std::unexpected(checked.error());
    for(auto id:ids) {
        auto generated=detail::generate_infrastructure(settings,parts,id);if(!generated)return generated;
        auto replaced=detail::replace_infrastructure_geometry(result,std::move(*generated),id);if(!replaced)return replaced;
        result=std::move(*replaced);
    }
    detail::write_infrastructure_parts(result,parts);return result;
}
content::Result<Document> rigid(const Document& source,u32 id,const Mat4& delta,std::vector<InfrastructurePart> parts) {
    auto vertices=selected(source,id);if(!vertices)return std::unexpected(vertices.error());
    auto frame=mesh_frame::read(source);if(!frame)return std::unexpected(frame.error());
    auto result=source;
    const auto transform=mesh_frame::compose(mesh_frame::compose(*frame,delta),*mesh_frame::inverse(*frame));
    if(auto r=mesh_frame::bake(result,transform,std::span<const u32>{*vertices});!r)return std::unexpected(r.error());
    auto settings=infrastructure_settings(source);if(!settings)return std::unexpected(settings.error());
    std::vector<u32> affected;for(const auto& p:parts)if(depends_on(p,id))affected.push_back(p.id);
    return refresh(std::move(result),parts,*settings,affected);
}
void relocated(InfrastructurePart& p,const Mat4& delta) {
    const auto old=placement::frame(p.location,p.heading);
    const auto x=mesh_frame::vector(delta,{old[0][0],old[0][1],old[0][2]});
    p.location=placement::location(mesh_frame::vector(delta,placement::direction(p.location)));
    p.heading=placement::heading(p.location,x);
    if(p.kind==InfrastructureKind::skyway)p.end=placement::location(mesh_frame::vector(delta,placement::direction(p.end)));
    if(p.bezier_controls)for(auto& point:*p.bezier_controls)point=mesh_frame::vector(delta,point);
}
}

bool has_infrastructure_parts(const Document& d){return d.metadata.contains(std::string(version));}
bool valid(const InfrastructurePart& p) {
    if(!std::isfinite(p.terminal_incline)||p.terminal_incline<0||p.terminal_incline>60)return false;
    if(p.curve_segments<8||p.curve_segments>256)return false;
    if(p.bezier_controls&&(p.kind!=InfrastructureKind::skyway||p.bezier_controls->size()>14||
        std::ranges::any_of(*p.bezier_controls,[](Vec3 v){const auto r=std::hypot(v.x,v.y,v.z);return !std::isfinite(r)||r<.01F||r>513;})))return false;
    if(u32(p.tunnel_class)>u32(TunnelSizeClass::trunk)||!p.id||p.id>=max_id||p.name.empty()||p.name.size()>128||!p.seed||!placement::valid(p.location)||!placement::valid(p.end)||
       !std::isfinite(p.heading)||std::abs(p.heading)>36000||!std::isfinite(p.size)||p.size<.25F||p.size>4||
       !std::isfinite(p.height)||p.height<.25F||p.height>4||
       !std::isfinite(p.altitude)||p.altitude<0||p.altitude>max_infrastructure_altitude||!std::isfinite(p.scale)||p.scale<.05F||p.scale>20||
       !std::isfinite(p.scaffold_spacing)||p.scaffold_spacing<0||p.scaffold_spacing>10||
       !std::isfinite(p.altitude_a)||p.altitude_a<0||p.altitude_a>max_tunnel_endpoint_altitude||
       !std::isfinite(p.altitude_b)||p.altitude_b<0||p.altitude_b>max_tunnel_endpoint_altitude)return false;
    if(p.scaffold_positions&&(p.scaffold_positions->size()>64||std::ranges::any_of(*p.scaffold_positions,
        [](f32 t){return !std::isfinite(t)||t<0||t>1;})))return false;
    if(p.kind==InfrastructureKind::skyway) {
        const auto ref_valid=[](TunnelSocketRef ref){return ref.part?ref.part<max_id&&ref.socket>=1&&ref.socket<=3:ref.socket==0;};
        return !p.altitude&&ref_valid(p.socket_a)&&ref_valid(p.socket_b)&&!(p.socket_a&&p.terminal_a)&&!(p.socket_b&&p.terminal_b)&&
            (p.socket_a||p.socket_b||SkywayCurve::create(p.location,p.end).has_value());
    }
    return (p.kind!=InfrastructureKind::settlement||!p.altitude)&&!p.scaffold_spacing&&!p.scaffold_positions&&
        !p.altitude_a&&!p.altitude_b&&p.socket_a==TunnelSocketRef{}&&p.socket_b==TunnelSocketRef{}&&!p.terminal_a&&!p.terminal_b&&
        (p.kind==InfrastructureKind::settlement||p.kind==InfrastructureKind::hub||
        p.kind==InfrastructureKind::terminal||p.kind==InfrastructureKind::elevator||p.kind==InfrastructureKind::joiner||p.kind==InfrastructureKind::processor);
}
Vec2 infrastructure_center(const InfrastructurePart& p) {
    return p.kind==InfrastructureKind::skyway?placement::location(placement::add(placement::direction(p.location),placement::direction(p.end))):p.location;
}
content::Result<std::vector<InfrastructurePart>> infrastructure_parts(const Document& d) {
    if(auto checked=vm::validate(d);!checked)return std::unexpected(checked.error());
    if(!is_earth(d))return error("Expected an Earth blueprint");
    const auto marker=d.metadata.find(std::string(version));
    if(marker==d.metadata.end())return detail::default_infrastructure_parts();
    if(marker->second!="1"&&marker->second!="2"&&marker->second!="3"&&marker->second!="4"&&marker->second!="5"&&marker->second!="6"&&marker->second!="7"&&marker->second!="8"&&marker->second!="9")return error("Unsupported Earth infrastructure parts version");
    std::map<u32,InfrastructurePart> sorted;
    for(const auto& [name,text]:d.metadata)if(name.starts_with(prefix)) {
        auto id=integer(std::string_view(name).substr(prefix.size()));if(!id)return std::unexpected(id.error());
        if(text.size()>4096)return error("Infrastructure recipe too long");
        InfrastructurePart p;p.id=*id;unsigned kind{};
        std::istringstream stream(text);stream.imbue(std::locale::classic());
        if(!(stream>>kind>>p.location.x>>p.location.y>>p.end.x>>p.end.y>>p.heading>>p.size>>p.height>>p.visible>>p.seed>>std::quoted(p.name)))
            return error("Malformed infrastructure recipe");
        if(marker->second!="1"&&!(stream>>p.terminal_a>>p.terminal_b))return error("Malformed infrastructure terminal flags");
        if(marker->second>="3"&&!(stream>>p.socket_a.part>>p.socket_a.socket>>p.socket_b.part>>p.socket_b.socket))return error("Malformed tunnel socket references");
        if(marker->second>="4"&&!(stream>>p.altitude_a>>p.altitude_b))return error("Malformed tunnel endpoint altitudes");
        if(marker->second>="5"&&!(stream>>p.altitude>>p.scale>>p.scaffold>>p.scaffold_spacing))return error("Malformed infrastructure placement/support settings");
        if(marker->second>="6") {
            int count{};if(!(stream>>count)||count< -1||count>64)return error("Invalid manual scaffold count");
            if(count>=0) {
                p.scaffold_positions.emplace(std::size_t(count));
                for(auto& t:*p.scaffold_positions)if(!(stream>>t))return error("Malformed scaffold position");
            }
        }
        if(marker->second>="7") {
            unsigned size{};if(!(stream>>size)||size>u32(TunnelSizeClass::trunk))return error("Invalid tunnel size class");
            p.tunnel_class=TunnelSizeClass(size);
        }
        if(marker->second>="8") {
            int count{};
            if(!(stream>>p.curve_segments>>count)||count< -1||count>14)return error("Invalid Bezier control count");
            if(count>=0) {
                p.bezier_controls.emplace(std::size_t(count));
                for(auto& v:*p.bezier_controls)if(!(stream>>v.x>>v.y>>v.z))return error("Malformed Bezier control point");
            }
        }
        if(marker->second=="9"&&!(stream>>p.terminal_incline))return error("Malformed terminal inclination");
        p.kind=InfrastructureKind(kind);std::string extra;
        if((stream>>extra)||!valid(p)||!sorted.emplace(p.id,p).second)return error("Invalid or duplicate infrastructure part");
    }
    if(sorted.size()>256)return error("At most 256 infrastructure parts are supported");
    if(const auto it=d.metadata.find(std::string(next_key));it!=d.metadata.end()) {
        auto next=integer(it->second);if(!next)return std::unexpected(next.error());
        if(!sorted.empty()&&*next<=sorted.rbegin()->first)return error("Next infrastructure identity overlaps existing parts");
    } else return error("Infrastructure catalogue is missing its next identity");
    const auto ids=std::ranges::find(d.vertex_fields,infrastructure_part_field,&vm::VertexField::name);
    if(ids==d.vertex_fields.end()||ids->type!=vm::FieldType{vm::ScalarType::UInt32,1})return error("Infrastructure parts are missing their ownership field");
    for(auto id:std::get<std::vector<u32>>(ids->values))if(id&&!sorted.contains(id))return error("Unknown infrastructure part in geometry");
    std::vector<InfrastructurePart> result;for(auto& [id,p]:sorted)result.push_back(std::move(p));
    auto settings=infrastructure_settings(d);if(!settings)return std::unexpected(settings.error());
    if(auto checked=validate_infrastructure_connections(result,*settings);!checked)return std::unexpected(checked.error());
    return result;
}
void detail::write_infrastructure_parts(Document& d,std::span<const InfrastructurePart> parts) {
    std::erase_if(d.metadata,[](const auto& item){return item.first.starts_with(prefix);});
    d.metadata[std::string(version)]="9";
    u32 next=1;
    if(const auto it=d.metadata.find(std::string(next_key));it!=d.metadata.end())if(auto parsed=integer(it->second))next=*parsed;
    for(const auto& p:parts) {
        std::ostringstream stream;stream.imbue(std::locale::classic());stream<<std::setprecision(std::numeric_limits<f32>::max_digits10)
            <<u32(p.kind)<<' '<<p.location.x<<' '<<p.location.y<<' '<<p.end.x<<' '<<p.end.y<<' '
            <<p.heading<<' '<<p.size<<' '<<p.height<<' '<<p.visible<<' '<<p.seed<<' '<<std::quoted(p.name)<<' '<<p.terminal_a<<' '<<p.terminal_b
            <<' '<<p.socket_a.part<<' '<<p.socket_a.socket<<' '<<p.socket_b.part<<' '<<p.socket_b.socket
            <<' '<<p.altitude_a<<' '<<p.altitude_b<<' '<<p.altitude<<' '<<p.scale<<' '<<p.scaffold<<' '<<p.scaffold_spacing;
        stream<<' '<<(p.scaffold_positions?int(p.scaffold_positions->size()):-1);
        if(p.scaffold_positions)for(auto t:*p.scaffold_positions)stream<<' '<<t;
        stream<<' '<<u32(p.tunnel_class);
        stream<<' '<<p.curve_segments<<' '<<(p.bezier_controls?int(p.bezier_controls->size()):-1);
        if(p.bezier_controls)for(auto v:*p.bezier_controls)stream<<' '<<v.x<<' '<<v.y<<' '<<v.z;
        stream<<' '<<p.terminal_incline;
        d.metadata[key(p.id)]=stream.str();next=std::max(next,p.id+1);
    }
    d.metadata[std::string(next_key)]=std::to_string(next);
}
content::Result<Document> rebuild_infrastructure_part(const Document& source,u32 id) {
    auto parts=infrastructure_parts(source);if(!parts)return std::unexpected(parts.error());
    if(std::ranges::find(*parts,id,&InfrastructurePart::id)==parts->end())return error("Unknown infrastructure part");
    auto settings=infrastructure_settings(source);if(!settings)return std::unexpected(settings.error());
    std::vector<u32> affected{id};
    for(const auto& p:*parts)if(depends_on(p,id))affected.push_back(p.id);
    return refresh(source,*parts,*settings,affected);
}
content::Result<Document> edit_infrastructure(const Document& source,const InfrastructurePart& replacement) {
    if(!valid(replacement))return error("Invalid infrastructure values (tunnel anchors must be 0.1–150 degrees apart)");
    auto parts=infrastructure_parts(source);if(!parts)return std::unexpected(parts.error());
    auto it=std::ranges::find(*parts,replacement.id,&InfrastructurePart::id);
    if(it==parts->end()||it->kind!=replacement.kind)return error("Unknown infrastructure part or changed recipe kind");
    if(*it==replacement)return source;
    auto renamed=*it;renamed.name=replacement.name;
    if(renamed==replacement) {*it=replacement;auto result=source;detail::write_infrastructure_parts(result,*parts);return result;}
    *it=replacement;
    auto settings=infrastructure_settings(source);if(!settings)return std::unexpected(settings.error());
    std::vector<u32> affected{replacement.id};
    for(const auto& p:*parts)if(depends_on(p,replacement.id))affected.push_back(p.id);
    return refresh(source,*parts,*settings,affected);
}
content::Result<Document> add_infrastructure(const Document& source,InfrastructureKind kind,Vec2 location) {
    if(auto checked=vm::validate(source);!checked)return std::unexpected(checked.error());
    if(!placement::valid(location))return error("Invalid infrastructure location");
    auto settings=infrastructure_settings(source);if(!settings)return std::unexpected(settings.error());
    auto initialized=source;
    if(!has_infrastructure_parts(source)) {
        if(std::ranges::find(source.vertex_fields,infrastructure_part_field,&vm::VertexField::name)!=source.vertex_fields.end())
            return error("Infrastructure ownership is missing its recipe catalogue");
        // A genuinely empty Earth starts with an empty catalogue. Legacy baked
        // infrastructure needs the explicit one-time migration/rebuild first.
        const auto field=std::ranges::find(source.vertex_fields,"earth/infrastructure",&vm::VertexField::name);
        if(field!=source.vertex_fields.end()&&field->type!=vm::FieldType{vm::ScalarType::UInt32,1})return error("Invalid infrastructure ownership field");
        const bool legacy=field!=source.vertex_fields.end()&&std::ranges::any_of(std::get<std::vector<u32>>(field->values),[](u32 id){return id!=0;});
        if(legacy)return error("Enable infrastructure editing first (migrates the baked infrastructure)");
        initialized.vertex_fields.push_back({std::string(infrastructure_part_field),{vm::ScalarType::UInt32,1},std::vector<u32>(source.vertex_count)});
        detail::write_infrastructure_parts(initialized,{});
    }
    auto parts=infrastructure_parts(initialized);if(!parts)return std::unexpected(parts.error());
    if(parts->size()>=256)return error("At most 256 infrastructure parts are supported");
    auto next=integer(initialized.metadata.at(std::string(next_key)));if(!next)return std::unexpected(next.error());
    InfrastructurePart p{*next,kind,"",location,{std::remainder(location.x+9,360.F),location.y}};
    p.seed=*next;p.name=std::string(infrastructure_name(kind))+" "+std::to_string(p.id);
    if(!valid(p))return error("Invalid new infrastructure part");
    parts->push_back(p);detail::write_infrastructure_parts(initialized,*parts);
    bool* enabled=kind==InfrastructureKind::hub?&settings->launch_hubs:kind==InfrastructureKind::skyway?&settings->skyways:
        kind==InfrastructureKind::settlement?&settings->night_lights:kind==InfrastructureKind::processor?&settings->processors:&settings->large_structures;
    if(!*enabled) {
        *enabled=true;return rebuild_infrastructure(initialized,*settings);
    }
    auto generated=detail::generate_infrastructure(*settings,*parts,p.id);if(!generated)return generated;
    auto result=detail::replace_infrastructure_geometry(initialized,std::move(*generated),p.id);
    if(!result)return result;
    detail::write_infrastructure_parts(*result,*parts);return result;
}
content::Result<Document> remove_infrastructure(const Document& source,u32 id) {
    auto parts=infrastructure_parts(source);if(!parts)return std::unexpected(parts.error());
    auto settings=infrastructure_settings(source);if(!settings)return std::unexpected(settings.error());
    const auto sockets=infrastructure_sockets(*parts,*settings);
    std::vector<u32> affected{id};
    for(auto& p:*parts)if(depends_on(p,id)) {
        affected.push_back(p.id);
        for(bool end:{false,true}) {
            auto& ref=end?p.socket_b:p.socket_a;if(ref.part!=id)continue;
            const auto socket=std::ranges::find(sockets,ref,&TunnelSocket::id);
            (end?p.end:p.location)=placement::location(socket->position);
            (end?p.altitude_b:p.altitude_a)=std::max(0.F,std::hypot(socket->position.x,socket->position.y,socket->position.z)-skyway_endpoint_radius);ref={};
        }
    }
    if(!std::erase_if(*parts,[&](const auto& p){return p.id==id;}))return error("Unknown infrastructure part");
    return refresh(source,*parts,*settings,affected);
}
content::Result<Document> move_infrastructure(const Document& source,u32 id,Vec2 target) {
    if(!placement::valid(target))return error("Invalid infrastructure location");
    auto parts=infrastructure_parts(source);if(!parts)return std::unexpected(parts.error());
    auto it=std::ranges::find(*parts,id,&InfrastructurePart::id);if(it==parts->end())return error("Unknown infrastructure part");
    if(it->socket_a||it->socket_b)return error("Move the connected structure or a free tunnel endpoint; detach to move the whole tunnel");
    const auto center=infrastructure_center(*it);if(center==target)return source;
    const auto delta=placement::between(placement::direction(center),placement::direction(target));
    relocated(*it,delta);return rigid(source,id,delta,std::move(*parts));
}
content::Result<Document> place_infrastructure(const Document& source,u32 id,Vec3 target) {
    if(!std::isfinite(target.x)||!std::isfinite(target.y)||!std::isfinite(target.z))return error("Invalid structure position");
    auto parts=infrastructure_parts(source);if(!parts)return std::unexpected(parts.error());
    auto it=std::ranges::find(*parts,id,&InfrastructurePart::id);if(it==parts->end())return error("Unknown infrastructure part");
    if(it->kind==InfrastructureKind::skyway||it->kind==InfrastructureKind::settlement)
        return error("Use the surface/endpoint handles for this part");
    auto settings=infrastructure_settings(source);if(!settings)return std::unexpected(settings.error());
    const auto altitude=std::hypot(target.x,target.y,target.z)-infrastructure_handle_base(*it,*settings);
    if(altitude<-.00001F||altitude>max_infrastructure_altitude+.00001F)return error("Structure altitude must be between 0 and 10 Earth radii");
    const auto height=std::clamp(altitude,0.F,max_infrastructure_altitude);
    const auto location=placement::location(target);
    // Ordinary surface drags retain hand-edited hulls and custom attributes.
    if(std::abs(height-it->altitude)<.000001F)return move_infrastructure(source,id,location);
    relocated(*it,placement::between(placement::direction(it->location),placement::direction(location)));
    it->altitude=height;return edit_infrastructure(source,*it);
}
content::Result<Document> rotate_infrastructure(const Document& source,u32 id,f32 degrees) {
    if(!std::isfinite(degrees)||std::abs(degrees)>36000)return error("Invalid infrastructure rotation");
    auto parts=infrastructure_parts(source);if(!parts)return std::unexpected(parts.error());
    auto it=std::ranges::find(*parts,id,&InfrastructurePart::id);if(it==parts->end())return error("Unknown infrastructure part");
    if(it->socket_a||it->socket_b)return error("Rotate the connected structure; detach to rotate the whole tunnel");
    if(!degrees)return source;
    const auto delta=placement::turn(placement::direction(infrastructure_center(*it)),degrees*placement::radians);
    relocated(*it,delta);return rigid(source,id,delta,std::move(*parts));
}
content::Result<Document> move_infrastructure_endpoint(const Document& source,u32 id,bool end,Vec2 target) {
    auto parts=infrastructure_parts(source);if(!parts)return std::unexpected(parts.error());
    auto it=std::ranges::find(*parts,id,&InfrastructurePart::id);
    if(it==parts->end()||it->kind!=InfrastructureKind::skyway)return error("Expected a skyway part");
    if(end?bool(it->socket_b):bool(it->socket_a))return error("Detach this tunnel endpoint before moving it along the surface");
    (end?it->end:it->location)=target;return edit_infrastructure(source,*it);
}
content::Result<Document> connect_infrastructure_endpoint(const Document& source,u32 id,bool end,TunnelSocketRef target) {
    auto parts=infrastructure_parts(source);if(!parts)return std::unexpected(parts.error());
    auto it=std::ranges::find(*parts,id,&InfrastructurePart::id);
    if(it==parts->end()||it->kind!=InfrastructureKind::skyway)return error("Expected a skyway part");
    auto settings=infrastructure_settings(source);if(!settings)return std::unexpected(settings.error());
    const auto sockets=infrastructure_sockets(*parts,*settings);
    auto& ref=end?it->socket_b:it->socket_a;
    if(target==ref)return source;
    if(!target&&target.socket)return error("Invalid detached socket reference");
    const auto socket=std::ranges::find(sockets,target?target:ref,&TunnelSocket::id);
    if(target&&socket==sockets.end())return error("Unknown terminal/joiner socket");
    if(socket!=sockets.end()) {
        (end?it->end:it->location)=placement::location(socket->position);
        (end?it->altitude_b:it->altitude_a)=std::max(0.F,std::hypot(socket->position.x,socket->position.y,socket->position.z)-skyway_endpoint_radius);
    }
    ref=target;
    if(target)(end?it->terminal_b:it->terminal_a)=false;
    return edit_infrastructure(source,*it);
}
content::Result<Document> place_infrastructure_endpoint(const Document& source,u32 id,bool end,Vec3 target) {
    if(!std::isfinite(target.x)||!std::isfinite(target.y)||!std::isfinite(target.z))return error("Invalid endpoint position");
    const auto radius=std::hypot(target.x,target.y,target.z);
    if(radius<skyway_endpoint_radius-.00001F||radius>skyway_endpoint_radius+max_tunnel_endpoint_altitude)return error("Endpoint height outside its range");
    auto parts=infrastructure_parts(source);if(!parts)return std::unexpected(parts.error());
    auto it=std::ranges::find(*parts,id,&InfrastructurePart::id);
    if(it==parts->end()||it->kind!=InfrastructureKind::skyway)return error("Expected a skyway part");
    if(end?bool(it->socket_b):bool(it->socket_a))return error("Detach this tunnel endpoint before moving it");
    (end?it->end:it->location)=placement::location(target);
    const auto altitude=std::max(0.F,radius-skyway_endpoint_radius);
    (end?it->altitude_b:it->altitude_a)=altitude<.000001F?0.F:altitude;
    return edit_infrastructure(source,*it);
}
}
