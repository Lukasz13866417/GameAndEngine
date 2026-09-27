#include "earth_assets.hpp"
#include "earth_geography.hpp"
#include "earth_infrastructure_detail.hpp"
#include "earth_connections.hpp"
#include "earth_placement.hpp"
#include "earth_skyway.hpp"
#include "earth_structures.hpp"
#include "earth_infrastructure_placement.hpp"
#include "mesh_frame.hpp"
#include <array>
#include <charconv>
#include <numbers>

namespace example::earth {
namespace {
using namespace vng;
namespace vm=content::vmesh;
using Document=vm::Document;
constexpr f32 pi=std::numbers::pi_v<f32>, radians=pi/180;
constexpr std::string_view owner_name="earth/infrastructure";
content::Diagnostic error(std::string message){content::Diagnostic d;d.message=std::move(message);return d;}
const vm::VertexField* field(const Document& d,std::string_view name) {
    const auto it=std::ranges::find(d.vertex_fields,name,&vm::VertexField::name);
    return it==d.vertex_fields.end()?nullptr:&*it;
}
Vec3 add(Vec3 a,Vec3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
Vec3 mul(Vec3 a,f32 b){return {a.x*b,a.y*b,a.z*b};}
Vec3 cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
Vec3 unit(Vec3 a){return mul(a,1/std::hypot(a.x,a.y,a.z));}
Vec3 direction(Vec2 p){return {std::sin(p.x*radians)*std::cos(p.y*radians),std::sin(p.y*radians),std::cos(p.x*radians)*std::cos(p.y*radians)};}
bool land(Vec2 p) {
    // Use the same authored coastline as the terrain, not country bounding boxes.
    for(const auto& polygon:geography::continents) {
        bool inside{};
        for(std::size_t i=0,j=polygon.size()-1;i<polygon.size();j=i++) {
            const auto a=polygon[j],b=polygon[i];
            if((a.y>p.y)!=(b.y>p.y)&&p.x<(b.x-a.x)*(p.y-a.y)/(b.y-a.y)+a.x)inside=!inside;
        }
        if(inside)return true;
    }
    return false;
}
struct Flag {std::string_view name;bool InfrastructureSettings::*member;};
constexpr Flag flags[]={{"night_lights",&InfrastructureSettings::night_lights},
    {"skyways",&InfrastructureSettings::skyways},{"launch_hubs",&InfrastructureSettings::launch_hubs},
    {"large_structures",&InfrastructureSettings::large_structures},{"processors",&InfrastructureSettings::processors}};
struct Number {std::string_view name;f32 InfrastructureSettings::*member;};
constexpr Number numbers[]={{"light_strength",&InfrastructureSettings::light_strength},
    {"city_density",&InfrastructureSettings::city_density},{"skyway_height",&InfrastructureSettings::skyway_height},
    {"structure_size",&InfrastructureSettings::structure_size},{"hub_height",&InfrastructureSettings::hub_height},
    {"addon_scale",&InfrastructureSettings::addon_scale},{"settlement_scale",&InfrastructureSettings::settlement_scale},
    {"tunnel_scale",&InfrastructureSettings::tunnel_scale},{"launch_pad_scale",&InfrastructureSettings::launch_pad_scale},
    {"terminal_scale",&InfrastructureSettings::terminal_scale},{"elevator_scale",&InfrastructureSettings::elevator_scale},
    {"joiner_scale",&InfrastructureSettings::joiner_scale},{"processor_scale",&InfrastructureSettings::processor_scale}};
std::string key(std::string_view name){return "earth/infrastructure/"+std::string(name);}
void write_settings(Document& d,InfrastructureSettings s) {
    for(const auto& p:flags)d.metadata[key(p.name)]=s.*p.member?"true":"false";
    for(const auto& p:numbers) {
        char bytes[64];const auto converted=std::to_chars(bytes,bytes+64,s.*p.member);
        d.metadata[key(p.name)]=std::string(bytes,converted.ptr);
    }
    d.metadata["render/emission"]="night";
}

// Approximate settlements, not a satellite light-map. Sparse outliers and
// broken street clusters retain the existing illustrated, low-poly vocabulary.
constexpr Vec2 cities[]={{-121,38},{-118,34},{-112,34},{-105,39},{-99,32},{-95,30},
    {-87,41},{-80,35},{-76,40},{-72,44},{-99,20},{-103,23},{-90,15},{-74,5},
    {-77,-11},{-70,-32},{-59,-33},{-48,-23},{-44,-20},{-39,-7},
    {-5,40},{2,47},{-2,52},{8,50},{12,46},{19,50},{28,48},{30,56},{37,55},
    {-7,32},{3,35},{30,29},{8,9},{1,7},{37,8},{29,-25},{19,-32},
    {46,25},{51,33},{67,25},{73,30},{76,19},{79,13},{87,24},{91,25},
    {103,31},{110,33},{116,38},{120,31},{112,24},{105,20},{101,16},{106,-6},
    {126,37},{136,35},{141,40},{147,-34},{139,-33},{116,-30}};
// Regional corridors, mostly 700–1500 km. Radius 1 corresponds to 6371 km.
// Their thickness and height are deliberately exaggerated for orbital readability.
struct Route {Vec2 a,b;};
constexpr Route routes[]={{{-121,38},{-112,34}},{{-87,41},{-76,40}},{{-99,20},{-90,15}},
    {{-59,-33},{-48,-23}},{{-5,40},{2,47}},{{2,47},{12,46}},{{12,46},{19,50}},
    {{19,50},{28,48}},{{30,29},{37,24}},{{73,30},{79,23}},{{110,33},{120,31}},
    {{112,24},{120,31}},{{126,37},{136,35}},{{139,-33},{147,-34}},{{19,-32},{29,-25}}};
class Builder {
    Document d_;
    std::vector<f32> positions_,normals_,colors_,emissions_;
    std::vector<u32> owners_;
    std::vector<u32> parts_;
    u32 part_{};
    Mat4 placement_{Mat4::identity()};
    u32 random_{13866417};
    f32 random(){random_^=random_<<13;random_^=random_>>17;random_^=random_<<5;return f32(random_>>8)/16777216.F;}
    u32 vertex(Vec3 p,Vec3 color,f32 emission,u32 owner) {
        const auto id=u32(owners_.size());auto n=unit(p);
        p=mesh_frame::point(placement_,p);n=unit(mesh_frame::vector(placement_,n));
        positions_.insert(positions_.end(),{p.x,p.y,p.z});normals_.insert(normals_.end(),{n.x,n.y,n.z});
        colors_.insert(colors_.end(),{color.x,color.y,color.z,1});emissions_.push_back(emission);owners_.push_back(owner);parts_.push_back(part_);return id;
    }
    void append(const detail::StructureMesh& mesh,u32 owner) {
        const auto first=u32(owners_.size());
        for(const auto& v:mesh.vertices) {
            vertex(v.position,v.color,v.emission,owner);
            const auto n=unit(mesh_frame::vector(placement_,v.normal));
            const auto i=normals_.size()-3;normals_[i]=n.x;normals_[i+1]=n.y;normals_[i+2]=n.z;
        }
        for(auto face:mesh.faces){for(auto& i:face.vertices)i+=first;d_.faces.push_back(face);}
    }
    void quad(Vec3 a,Vec3 b,Vec3 c,Vec3 d,Vec3 color,f32 emission,u32 owner) {
        const auto i=vertex(a,color,emission,owner);vertex(b,color,emission,owner);vertex(c,color,emission,owner);vertex(d,color,emission,owner);
        d_.faces.emplace_back(i,i+1,i+2);d_.faces.emplace_back(i,i+2,i+3);
    }
    void scaffold(SkywaySample sample,f32 size,u32 owner) {
        sample=infrastructure_sample(placement_,sample);
        const auto saved=placement_;placement_=Mat4::identity();
        append(detail::tunnel_scaffold(sample,size),owner);placement_=saved;
    }
    void patch(Vec2 location,f32 size,f32 stretch,Vec3 color,f32 emission) {
        if(!land(location))return;
        const auto n=direction(location),east=unit(cross({0,1,0},n)),north=cross(n,east),p=mul(n,1.015F);
        const auto a=mul(east,size*stretch),b=mul(north,size);
        quad(add(add(p,a),b),add(add(p,mul(a,-1)),b),add(add(p,mul(a,-1)),mul(b,-1)),add(add(p,a),mul(b,-1)),color,emission,1);
    }
    void join(u32 a,u32 b,unsigned sides) {
        for(unsigned j=0;j<sides;++j) {
            const auto next=(j+1)%sides;
            d_.faces.emplace_back(a+j,a+next,b+next);d_.faces.emplace_back(a+j,b+next,b+j);
        }
    }
    // Indexed rectangular-section collars: both shoulders, inner wall, outer
    // wall. The dark rib is real geometry, not a painted stripe on the shell.
    void collar(Vec3 p,Vec3 axis,Vec3 x,Vec3 y,f32 inner,f32 outer,f32 depth,
                Vec3 color,f32 emission,u32 owner,unsigned sides=8) {
        const auto first=u32(owners_.size());
        for(const auto profile:std::array<Vec2,4>{{{inner,-depth},{outer,-depth},{outer,depth},{inner,depth}}})
            for(unsigned j=0;j<sides;++j) {
                const auto angle=(f32(j)+.5F)*2*pi/f32(sides);
                const auto offset=add(mul(x,std::cos(angle)*profile.x),mul(y,std::sin(angle)*profile.x));
                vertex(add(add(p,offset),mul(axis,profile.y)),mul(color,.76F+.24F*std::sin(angle)),emission,owner);
            }
        for(unsigned i=0;i<4;++i)join(first+i*sides,first+((i+1)%4)*sides,sides);
    }
    void tower(Vec3 base,Vec3 up,Vec3 east,Vec3 north,f32 bottom,f32 top,f32 height,
               Vec3 color,u32 owner,f32 emission=0) {
        constexpr unsigned sides=6;const auto first=u32(owners_.size());
        for(unsigned level=0;level<2;++level)for(unsigned j=0;j<sides;++j) {
            const auto angle=f32(j)*2*pi/sides,radius=level?top:bottom;
            const auto p=add(add(base,mul(up,f32(level)*height)),add(mul(east,std::cos(angle)*radius),mul(north,std::sin(angle)*radius)));
            vertex(p,mul(color,.62F+.38F*(.5F+.5F*std::cos(angle))),emission,owner);
        }
        join(first,first+sides,sides);
        for(unsigned j=1;j+1<sides;++j)d_.faces.emplace_back(first+sides,first+sides+j,first+sides+j+1);
    }
    content::Result<void> skyway(const InfrastructurePart& part,InfrastructureSettings settings,std::span<const InfrastructurePart> parts) {
        const Route route{part.location,part.end};
        auto curve=TunnelCurve::create(part,parts,settings);
        if(!curve)return std::unexpected(curve.error());
        const auto scale=part.scale*addon_scale(settings,part.kind);
        const auto size=settings.structure_size*part.size*scale*tunnel_width_coefficient(part.tunnel_class);
        const auto range=tunnel_body_range(part,*curve);
        const auto begin=range.x,end=range.y;
        const auto sample_at=[&](f32 t){return curve->sample(begin+t*(end-begin));};
        // Orbital sampling of the same shell; close-up routes supply denser
        // samples without changing its profile, materials or construction.
        const unsigned segments=part.bezier_controls?part.curve_segments:24;
        std::vector<detail::TunnelSection> path(segments+1);
        for(unsigned i=0;i<=segments;++i) {
            const auto t=begin+f32(i)/segments*(end-begin);
            path[i]={curve->sample(t),curve->size(t)};
        }
        append(detail::tunnel_shell(path,settings.light_strength),2);
        for(unsigned i=0;i<=6;++i) {
            const auto t=begin+f32(i)/6*(end-begin);
            append(detail::tunnel_collar(sample_at(f32(i)/6),curve->size(t)),2);
        }
        if(part.scaffold) {
            auto parameters=tunnel_support_positions(part,*curve);
            if(!parameters)return std::unexpected(parameters.error());
            for(auto t:*parameters) {
                // Preserve the original short ground footings until they are
                // actually moved away from an unraised endpoint.
                const bool footing=part.scaffold_spacing==0&&
                    ((t==0&&!part.socket_a&&part.altitude_a==0)||(t==1&&!part.socket_b&&part.altitude_b==0));
                if(footing) {
                    const auto n=direction(t==1?route.b:route.a),east=curve->sample(t).side,north=cross(n,east);
                    tower(mul(n,1.010F),n,east,north,.011F*size,.008F*size,.014F,{.12F,.16F,.19F},2);
                } else {
                    append(detail::tunnel_scaffold(curve->sample(t),curve->size(t)),2);
                }
            }
        }
        for(bool at_end:{false,true})if(at_end?part.terminal_b:part.terminal_a) {
            detail::TerminalPath path;
            for(std::size_t i=0;i<path.size();++i) {
                const auto t=f32(i)/f32(path.size()-1);
                path[i]=tunnel_terminal_sample(part,*curve,at_end,t);
            }
            append(detail::dispersal_terminal(path,size,settings.light_strength),2);
        }
        return {};
    }
    void hub(InfrastructureSettings settings) {
        const Vec3 up{0,1,0},east{1,0,0},north{0,0,-1};
        const auto size=settings.structure_size,h=settings.hub_height;
        const auto base=mul(up,1.012F);
        // Stepped hexagonal arcology with a broad elevated docking deck and a
        // slender crown. Lit windows are separate from the opaque steel shell.
        tower(base,up,east,north,.023F*size,.016F*size,.038F*h,{.16F,.19F,.21F},3);
        tower(add(base,mul(up,.038F*h)),up,east,north,.015F*size,.010F*size,.030F*h,{.12F,.17F,.20F},3);
        tower(add(base,mul(up,.068F*h)),up,east,north,.009F*size,.003F*size,.038F*h,{.20F,.24F,.26F},3);
        collar(add(base,mul(up,.038F*h)),up,east,north,.013F*size,.034F*size,.003F*h,{.085F,.12F,.15F},0,3);
        collar(add(base,mul(up,.042F*h)),up,east,north,.029F*size,.033F*size,.0006F*h,{.18F,.47F,.50F},1.1F*settings.light_strength,3);
        collar(add(base,mul(up,.068F*h)),up,east,north,.009F*size,.016F*size,.001F*h,{.72F,.42F,.16F},1.1F*settings.light_strength,3,6);
        for(unsigned j=0;j<3;++j) {
            const auto angle=f32(j)*2*pi/3;
            const auto p=add(base,add(mul(east,std::cos(angle)*.040F*size),mul(north,std::sin(angle)*.040F*size)));
            tower(p,up,east,north,.009F*size,.006F*size,(.024F+.009F*f32(j))*h,{.12F,.17F,.20F},3);
            const auto top=add(p,mul(up,(.024F+.009F*f32(j))*h));
            tower(top,up,east,north,.0062F*size,.0055F*size,.0015F*h,{.72F,.42F,.16F},3,1.2F*settings.light_strength);
        }
    }
public:
    content::Result<Document> build(InfrastructureSettings settings,std::span<const InfrastructurePart> parts,std::optional<u32> only) {
        if(auto checked=validate_infrastructure_connections(parts,settings);!checked)return std::unexpected(checked.error());
        for(const auto& part:parts) {
            if(only&&part.id!=*only)continue;
            if(!part.visible)continue;
            part_=part.id;placement_=Mat4::identity();
            auto local=settings;local.structure_size*=part.size;local.hub_height*=part.height;local.skyway_height*=part.height;
            const auto scale=part.scale*addon_scale(settings,part.kind);
            if(part.kind==InfrastructureKind::settlement && settings.night_lights) {
            const auto footprint=part.size*scale;
            const auto city=cities[(part.seed-1)%std::size(cities)];
            random_=13866417U^(part.seed*2654435761U);if(!random_)random_=1;
            placement_=mesh_frame::compose(placement::frame(part.location,part.heading),*mesh_frame::inverse(placement::frame(city)));
            const auto count=unsigned(42*settings.city_density);
            for(unsigned i=0;i<count;++i) {
                const auto angle=random()*2*pi,radius=std::pow(random(),1.4F)*2.8F*footprint;
                const Vec2 p{city.x+std::cos(angle)*radius/std::cos(city.y*radians),city.y+std::sin(angle)*radius*.6F};
                const auto strength=(1-radius/(4*footprint))*settings.light_strength;
                patch(p,(.00065F+random()*.0015F)*footprint,.7F+random()*1.9F,
                    {1,.38F+random()*.22F,.10F+random()*.10F},strength*(2.8F+random()*2));
            }
            // Short, broken avenues make these read as settlements rather
            // than evenly distributed stars pasted onto the planet.
            for(unsigned i=0;i<5;++i)patch({city.x+(f32(i)-2)*.24F*footprint,city.y+f32(i%2)*.08F*footprint},
                .00065F*footprint,3.F,{1,.57F,.22F},3.F*settings.light_strength);
            }
            if(part.kind==InfrastructureKind::skyway&&settings.skyways)
                if(auto built=skyway(part,settings,parts);!built)return std::unexpected(built.error());
            if(part.kind==InfrastructureKind::hub&&settings.launch_hubs) {
                placement_=infrastructure_frame(part,settings);
                hub(local);
            }
            if(settings.large_structures&&(part.kind==InfrastructureKind::terminal||part.kind==InfrastructureKind::elevator||part.kind==InfrastructureKind::joiner)) {
                placement_=infrastructure_frame(part,settings);
                if(part.kind==InfrastructureKind::terminal) {
                    const auto path=detail::freestanding_terminal_path(local.structure_size,local.hub_height);
                    append(detail::dispersal_terminal(path,local.structure_size,local.light_strength),u32(part.kind));
                    if(part.scaffold)for(auto i:{0U,6U})scaffold(path[i],local.structure_size*scale,u32(part.kind));
                } else if(part.kind==InfrastructureKind::joiner) {
                    append(detail::tunnel_joiner(local.structure_size,local.hub_height,local.light_strength),u32(part.kind));
                    if(part.scaffold)for(const auto& socket:detail::joiner_sockets(local.structure_size,local.hub_height))
                        scaffold(socket,local.structure_size*scale,u32(part.kind));
                } else append(detail::orbital_elevator(local.structure_size,local.hub_height,local.light_strength),u32(part.kind));
            }
            if(part.kind==InfrastructureKind::processor&&settings.processors) {
                placement_=infrastructure_frame(part,settings);
                append(detail::atmospheric_processor(local.structure_size,local.hub_height,local.light_strength),u32(part.kind));
            }
            if(part.scaffold&&part.altitude>0&&infrastructure_enabled(part.kind,settings)&&
               (part.kind==InfrastructureKind::hub||part.kind==InfrastructureKind::elevator||part.kind==InfrastructureKind::processor)) {
                // Raised buildings keep their intrinsic architecture; these
                // optional ground legs are separate from the elevator pylons.
                for(f32 z:{-.02F,.02F})scaffold({{0,1.012F+.008F*local.structure_size,z*local.structure_size},
                    {0,0,1},{1,0,0},{0,1,0}},local.structure_size*scale,u32(part.kind));
            }
            if(owners_.size()>max_earth_vertices)return std::unexpected(error("Infrastructure vertex budget exceeded; increase scaffold spacing or remove parts"));
        }
        d_.vertex_count=owners_.size();
        d_.vertex_fields={{"position",{vm::ScalarType::Float32,3},std::move(positions_)},
            {"normal",{vm::ScalarType::Float32,3},std::move(normals_)},
            {"color/0",{vm::ScalarType::Float32,4},std::move(colors_)},
            {"emission",{vm::ScalarType::Float32,1},std::move(emissions_)},
            {std::string(owner_name),{vm::ScalarType::UInt32,1},std::move(owners_)},
            {std::string(infrastructure_part_field),{vm::ScalarType::UInt32,1},std::move(parts_)}};
        return std::move(d_);
    }
};
}

std::vector<InfrastructurePart> detail::default_infrastructure_parts() {
    std::vector<InfrastructurePart> parts;u32 id=1,number=1;
    for(auto city:cities) {InfrastructurePart p{id++,InfrastructureKind::settlement,"Settlement "+std::to_string(number),city};p.seed=number++;parts.push_back(p);}
    number=1;for(auto route:routes)parts.push_back({id++,InfrastructureKind::skyway,"Skyway "+std::to_string(number++),route.a,route.b});
    number=1;for(auto city:std::array{cities[0],cities[10],cities[21],cities[31],cities[41],cities[48],cities[56]})
        parts.push_back({id++,InfrastructureKind::hub,"Launch hub "+std::to_string(number++),city});
    parts.push_back({id++,InfrastructureKind::terminal,"Pacific dispersal terminal",{122,25},{},-45});
    parts.push_back({id++,InfrastructureKind::elevator,"Equatorial orbital elevator",{103,1},{},30});
    return parts;
}
content::Result<Document> detail::generate_infrastructure(InfrastructureSettings settings,std::span<const InfrastructurePart> parts,std::optional<u32> only){return Builder{}.build(settings,parts,only);}

std::string_view infrastructure_name(InfrastructureKind kind) {
    switch(kind) {
    case InfrastructureKind::settlement:return "Settlement";
    case InfrastructureKind::skyway:return "Skyway";
    case InfrastructureKind::hub:return "Launch hub";
    case InfrastructureKind::terminal:return "Dispersal terminal";
    case InfrastructureKind::elevator:return "Orbital elevator";
    case InfrastructureKind::joiner:return "Tunnel joiner";
    case InfrastructureKind::processor:return "Atmospheric processor";
    }
    return "Unknown infrastructure";
}
bool infrastructure_enabled(InfrastructureKind kind,const InfrastructureSettings& s) {
    switch(kind) {
    case InfrastructureKind::settlement:return s.night_lights;
    case InfrastructureKind::skyway:return s.skyways;
    case InfrastructureKind::hub:return s.launch_hubs;
    case InfrastructureKind::terminal:case InfrastructureKind::elevator:case InfrastructureKind::joiner:return s.large_structures;
    case InfrastructureKind::processor:return s.processors;
    }
    return false;
}

vng::f32 addon_scale(InfrastructureSettings s,InfrastructureKind kind) {
    vng::f32 coefficient=1;
    switch(kind) {
    case InfrastructureKind::settlement:coefficient=s.settlement_scale;break;
    case InfrastructureKind::skyway:coefficient=s.tunnel_scale;break;
    case InfrastructureKind::hub:coefficient=s.launch_pad_scale;break;
    case InfrastructureKind::terminal:coefficient=s.terminal_scale;break;
    case InfrastructureKind::elevator:coefficient=s.elevator_scale;break;
    case InfrastructureKind::joiner:coefficient=s.joiner_scale;break;
    case InfrastructureKind::processor:coefficient=s.processor_scale;break;
    }
    return s.addon_scale*coefficient;
}
bool valid(InfrastructureSettings s) {
    for(auto value:{s.addon_scale,s.settlement_scale,s.tunnel_scale,s.launch_pad_scale,s.terminal_scale,s.elevator_scale,s.joiner_scale,s.processor_scale})
        if(!std::isfinite(value)||value<.1F||value>4)return false;
    return std::isfinite(s.light_strength)&&s.light_strength>=.1F&&s.light_strength<=4 &&
        std::isfinite(s.city_density)&&s.city_density>=.25F&&s.city_density<=2 &&
        std::isfinite(s.skyway_height)&&s.skyway_height>=.25F&&s.skyway_height<=3 &&
        std::isfinite(s.structure_size)&&s.structure_size>=.5F&&s.structure_size<=2 &&
        std::isfinite(s.hub_height)&&s.hub_height>=.5F&&s.hub_height<=3;
}
content::Result<InfrastructureSettings> infrastructure_settings(const Document& d) {
    InfrastructureSettings s;
    for(const auto& p:flags)if(const auto it=d.metadata.find(key(p.name));it!=d.metadata.end()) {
        if(it->second!="true"&&it->second!="false")return std::unexpected(error("Invalid Earth infrastructure toggle: "+it->first));
        s.*p.member=it->second=="true";
    }
    for(const auto& p:numbers)if(const auto it=d.metadata.find(key(p.name));it!=d.metadata.end()) {
        const auto& text=it->second;const auto parsed=std::from_chars(text.data(),text.data()+text.size(),s.*p.member);
        if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size())return std::unexpected(error("Invalid Earth infrastructure number: "+it->first));
    }
    if(!valid(s))return std::unexpected(error("Earth infrastructure settings outside their range"));
    return s;
}
content::Result<Document> detail::replace_infrastructure_geometry(const Document& source,Document more,std::optional<u32> part) {
    if(auto checked=vm::validate(source);!checked)return std::unexpected(checked.error());
    if(!is_earth(source))return std::unexpected(error("Expected an Earth blueprint"));
    auto frame=mesh_frame::read(source);if(!frame)return std::unexpected(frame.error());
    for(const auto& f:more.vertex_fields)if(f.name!=owner_name&&f.name!=infrastructure_part_field) {
        const auto* existing=field(source,f.name);
        if(!existing||existing->type!=f.type)return std::unexpected(error("Infrastructure requires field: "+f.name));
    }
    if(auto baked=mesh_frame::bake(more,*frame);!baked)return std::unexpected(baked.error());
    Document result=source;
    // Stamp legacy ownership before adding emissive geometry. Cloud editing
    // must never have to guess whether a new building is a cloud.
    if(!field(result,"earth/layer")) {
        const auto* e=field(result,"emission");
        if(!e||e->type!=vm::FieldType{vm::ScalarType::Float32,1})return std::unexpected(error("Legacy Earth lacks layer information"));
        std::vector<u32> layers;layers.reserve(source.vertex_count);
        for(auto value:std::get<std::vector<f32>>(e->values)) {
            if(std::abs(value-.015F)<.000001F)layers.push_back(1);
            else if(std::abs(value-.035F)<.000001F)layers.push_back(0);
            else return std::unexpected(error("Legacy Earth layer ownership is ambiguous"));
        }
        result.vertex_fields.push_back({"earth/layer",{vm::ScalarType::UInt32,1},std::move(layers)});
    }
    if(!field(result,owner_name))result.vertex_fields.push_back({std::string(owner_name),{vm::ScalarType::UInt32,1},std::vector<u32>(source.vertex_count)});
    const auto* ownership=field(result,owner_name);
    if(ownership->type!=vm::FieldType{vm::ScalarType::UInt32,1})return std::unexpected(error("Invalid infrastructure ownership field"));
    const auto owners=std::get<std::vector<u32>>(ownership->values);
    if(std::ranges::any_of(owners,[](u32 id){return id>u32(InfrastructureKind::processor);}))
        return std::unexpected(error("Unknown infrastructure ownership"));
    const auto* layer=field(result,"earth/layer");
    if(layer->type!=vm::FieldType{vm::ScalarType::UInt32,1})return std::unexpected(error("Invalid Earth layer field"));
    const auto& layers=std::get<std::vector<u32>>(layer->values);
    for(std::size_t i=0;i<owners.size();++i)
        if(layers[i]>1||(layers[i]&&owners[i]))return std::unexpected(error("Infrastructure ownership overlaps an invalid or cloud layer"));
    std::vector<bool> remove(source.vertex_count);
    for(std::size_t i=0;i<owners.size();++i)remove[i]=owners[i]!=0;
    if(part) {
        const auto* ids=field(source,infrastructure_part_field);
        if(!ids||ids->type!=vm::FieldType{vm::ScalarType::UInt32,1})return std::unexpected(error("Rebuild infrastructure once to edit its parts"));
        const auto& values=std::get<std::vector<u32>>(ids->values);
        for(std::size_t i=0;i<values.size();++i)remove[i]=values[i]==*part;
    }
    // Same tessellation: patch only the selected vertices, retaining indices,
    // untouched fields and custom attributes. This is the ordinary drag path.
    std::vector<u32> selected,local(source.vertex_count,UINT32_MAX);
    for(u32 i=0;i<source.vertex_count;++i)if(remove[i]){local[i]=u32(selected.size());selected.push_back(i);}
    std::vector<gfx::TriangleFace> old_faces;
    for(auto face:source.faces) {
        unsigned count{};for(auto v:face.vertices)count+=remove[v];
        if(count&&count!=3)return std::unexpected(error("Separate faces joining infrastructure parts before editing"));
        if(count)old_faces.emplace_back(local[face[0]],local[face[1]],local[face[2]]);
    }
    if(source.edges)for(auto edge:*source.edges)if(remove[edge[0]]!=remove[edge[1]])
        return std::unexpected(error("Separate edges joining infrastructure parts before editing"));
    if(part&&selected.size()==more.vertex_count&&old_faces==more.faces) {
        for(const auto& added:more.vertex_fields) {
            auto it=std::ranges::find(result.vertex_fields,added.name,&vm::VertexField::name);
            if(it==result.vertex_fields.end()||it->type!=added.type)return std::unexpected(error("Incompatible infrastructure field: "+added.name));
            std::visit([&](auto& values){const auto& from=std::get<std::decay_t<decltype(values)>>(added.values);
                for(std::size_t i=0;i<selected.size();++i)for(unsigned c=0;c<added.type.components;++c)values[selected[i]*added.type.components+c]=from[i*added.type.components+c];},it->values);
        }
        return result;
    }
    std::vector<u32> remap(source.vertex_count,UINT32_MAX);result.vertex_count=0;
    for(u32 i=0;i<source.vertex_count;++i)if(!remove[i])remap[i]=u32(result.vertex_count++);
    if(result.vertex_count+more.vertex_count>max_earth_vertices)return std::unexpected(error("Earth mesh vertex budget exceeded ("+
        std::to_string(result.vertex_count+more.vertex_count)+" / "+std::to_string(max_earth_vertices)+"); reduce city density or remove unused parts"));
    for(auto& f:result.vertex_fields)std::visit([&](auto& values) {
        auto original=std::move(values);values.clear();values.reserve((result.vertex_count+more.vertex_count)*f.type.components);
        for(u32 i=0;i<source.vertex_count;++i)if(!remove[i])for(unsigned c=0;c<f.type.components;++c)values.push_back(original[i*f.type.components+c]);
    },f.values);
    const auto mixed=[&](auto primitive){unsigned retained{};for(auto v:primitive.vertices)retained+=!remove[v];return retained&&retained!=primitive.vertices.size();};
    result.faces.clear();for(auto face:source.faces) {
        if(mixed(face))return std::unexpected(error("Separate faces joining infrastructure to other geometry before rebuilding"));
        if(!remove[face[0]])result.faces.emplace_back(remap[face[0]],remap[face[1]],remap[face[2]]);
    }
    if(result.edges) {result.edges->clear();for(auto edge:*source.edges) {
        if(mixed(edge))return std::unexpected(error("Separate edges joining infrastructure to other geometry before rebuilding"));
        if(!remove[edge[0]])result.edges->emplace_back(remap[edge[0]],remap[edge[1]]);
    }}
    if(!field(result,infrastructure_part_field))result.vertex_fields.push_back({std::string(infrastructure_part_field),{vm::ScalarType::UInt32,1},std::vector<u32>(result.vertex_count)});
    for(auto& f:result.vertex_fields) {
        const auto* next=field(more,f.name);
        if(next&&next->type!=f.type)return std::unexpected(error("Incompatible infrastructure field: "+f.name));
        std::visit([&](auto& values) {
            if(next){const auto& added=std::get<std::decay_t<decltype(values)>>(next->values);values.insert(values.end(),added.begin(),added.end());}
            else values.resize(values.size()+more.vertex_count*f.type.components,0);
        },f.values);
    }
    for(auto face:more.faces){for(auto& v:face.vertices)v+=u32(result.vertex_count);result.faces.push_back(face);}
    result.vertex_count+=more.vertex_count;
    if(auto checked=vm::validate(result);!checked)return std::unexpected(checked.error());
    return result;
}
content::Result<Document> rebuild_infrastructure(const Document& source,InfrastructureSettings settings) {
    if(!valid(settings))return std::unexpected(error("Invalid infrastructure settings"));
    auto parts=infrastructure_parts(source);if(!parts)return std::unexpected(parts.error());
    auto generated=detail::generate_infrastructure(settings,*parts);if(!generated)return generated;
    auto result=detail::replace_infrastructure_geometry(source,std::move(*generated));
    if(!result)return result;
    write_settings(*result,settings);detail::write_infrastructure_parts(*result,*parts);return result;
}
}
