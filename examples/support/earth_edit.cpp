#include "earth_assets.hpp"
#include "mesh_frame.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <numbers>
#include <sstream>

namespace example::earth {
namespace {
using namespace vng;
namespace vm=content::vmesh;
using Document=vm::Document;
content::Diagnostic error(std::string message) { content::Diagnostic e;e.message=std::move(message);return e; }
const vm::VertexField* field(const Document& d,std::string_view name) {
    const auto at=std::ranges::find(d.vertex_fields,name,&vm::VertexField::name);
    return at==d.vertex_fields.end()?nullptr:&*at;
}
struct Parameter {std::string_view name;f32 CloudSettings::*member;};
constexpr Parameter parameters[]={{"coverage",&CloudSettings::coverage},{"puff_size",&CloudSettings::puff_size},
    {"spiral_size",&CloudSettings::spiral_size},{"altitude",&CloudSettings::altitude},{"relief",&CloudSettings::relief},
    {"edge_scatter",&CloudSettings::edge_scatter}};
std::string key(std::string_view name){return "earth/clouds/"+std::string(name);}
content::Result<std::vector<u32>> layers(const Document& d) {
    if(const auto* layer=field(d,"earth/layer")) {
        if(layer->type!=vm::FieldType{vm::ScalarType::UInt32,1})return std::unexpected(error("Invalid Earth layer field"));
        const auto& values=std::get<std::vector<u32>>(layer->values);
        if(std::ranges::any_of(values,[](u32 v){return v>1;}))return std::unexpected(error("Unknown Earth layer"));
        return values;
    }
    // Older shipped assets predate explicit layer IDs. Their generator stamped
    // exactly .015 on clouds and .035 on ocean/land. Refuse ambiguous edits;
    // never infer ownership from color, position, or the blueprint's name.
    const auto* emission=field(d,"emission");
    if(!emission || emission->type!=vm::FieldType{vm::ScalarType::Float32,1})
        return std::unexpected(error("Legacy Earth has no usable cloud layer information"));
    std::vector<u32> result;result.reserve(d.vertex_count);
    for(auto value:std::get<std::vector<f32>>(emission->values)) {
        if(std::abs(value-.015F)<.000001F)result.push_back(1);
        else if(std::abs(value-.035F)<.000001F)result.push_back(0);
        else return std::unexpected(error("Legacy Earth layer ownership is ambiguous after emission edits"));
    }
    return result;
}

// The recipe retains a rigid orientation, not just a center. Otherwise a
// rebuild after several surface moves would lose accumulated cloud heading.
struct Rotation { f32 x{},y{},z{},w{1}; };
Vec3 add(Vec3 a,Vec3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
Vec3 mul(Vec3 a,f32 s){return {a.x*s,a.y*s,a.z*s};}
f32 dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
Vec3 unit(Vec3 a){return mul(a,1/std::sqrt(dot(a,a)));}
Rotation normalized(Rotation q) {
    const auto s=1/std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);
    return {q.x*s,q.y*s,q.z*s,q.w*s};
}
Vec3 rotate(Rotation q,Vec3 v) {
    const Vec3 axis{q.x,q.y,q.z};const auto t=mul(cross(axis,v),2);
    return add(v,add(mul(t,q.w),cross(axis,t)));
}
Rotation compose(Rotation a,Rotation b) {
    const Vec3 av{a.x,a.y,a.z},bv{b.x,b.y,b.z};
    const auto v=add(add(mul(bv,a.w),mul(av,b.w)),cross(av,bv));
    return normalized({v.x,v.y,v.z,a.w*b.w-dot(av,bv)});
}
Rotation between(Vec3 a,Vec3 b) {
    // Near antipodes the cross product subtracts almost equal products. Use
    // double intermediates; float cancellation can pick a non-perpendicular
    // axis and miss the requested point by several degrees.
    const double x=static_cast<double>(a.y)*b.z-static_cast<double>(a.z)*b.y;
    const double y=static_cast<double>(a.z)*b.x-static_cast<double>(a.x)*b.z;
    const double z=static_cast<double>(a.x)*b.y-static_cast<double>(a.y)*b.x;
    const auto cosine=std::clamp(static_cast<double>(a.x)*b.x+static_cast<double>(a.y)*b.y+static_cast<double>(a.z)*b.z,-1.,1.);
    const auto sine=std::sqrt(x*x+y*y+z*z);
    if(sine<1e-6 && cosine<0) {
        const auto axis=unit(cross(a,std::abs(a.x)<.8F?Vec3{1,0,0}:Vec3{0,1,0}));
        return {axis.x,axis.y,axis.z,0};
    }
    if(sine<1e-7)return {};
    const auto half=std::atan2(sine,cosine)*.5,scale=std::sin(half)/sine;
    return normalized({static_cast<f32>(x*scale),static_cast<f32>(y*scale),static_cast<f32>(z*scale),static_cast<f32>(std::cos(half))});
}
constexpr f32 radians=std::numbers::pi_v<f32>/180;
Vec3 direction(Vec2 p) {
    return {std::sin(p.x*radians)*std::cos(p.y*radians),std::sin(p.y*radians),std::cos(p.x*radians)*std::cos(p.y*radians)};
}
std::string pose_key(u32 id){return key("formation/"+std::to_string(id)+"/rotation");}
constexpr u32 max_cloud_id=4096;
using Recipes=std::map<u32,u32>; // instance identity -> immutable generator template
content::Result<Recipes> recipes(const Document& d) {
    Recipes result;
    for(const auto& cloud:cloud_catalog())if(!d.metadata.contains(key("removed/"+std::to_string(cloud.id))))result[cloud.id]=cloud.id;
    const auto prefix=key("template/");
    for(const auto& [name,value]:d.metadata)if(name.starts_with(prefix)) {
        u32 id{},source{};const auto suffix=std::string_view(name).substr(prefix.size());
        const auto a=std::from_chars(suffix.data(),suffix.data()+suffix.size(),id);
        const auto b=std::from_chars(value.data(),value.data()+value.size(),source);
        if(a.ec!=std::errc{}||a.ptr!=suffix.data()+suffix.size()||b.ec!=std::errc{}||b.ptr!=value.data()+value.size()||
           id<=cloud_catalog().size()||id>max_cloud_id||!source||source>cloud_catalog().size())
            return std::unexpected(error("Invalid cloud template identity"));
        result[id]=source;
    }
    if(result.size()>128)return std::unexpected(error("At most 128 cloud formations are supported"));
    return result;
}
content::Result<std::vector<Rotation>> poses(const Document& d) {
    auto catalog=recipes(d);if(!catalog)return std::unexpected(catalog.error());
    std::vector<Rotation> result(catalog->empty()?1:catalog->rbegin()->first+1);
    for(const auto& [id,source]:*catalog)if(const auto at=d.metadata.find(pose_key(id));at!=d.metadata.end()) {
        std::istringstream input(at->second);input.imbue(std::locale::classic());
        Rotation q;std::string extra;
        if(!(input>>q.x>>q.y>>q.z>>q.w) || (input>>extra))return std::unexpected(error("Invalid cloud formation rotation"));
        const auto length=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
        if(!std::isfinite(length) || std::abs(length-1)>.002F)return std::unexpected(error("Cloud rotation must be finite and normalized"));
        result[id]=normalized(q);
    }
    return result;
}
void write_pose(Document& d,u32 id,Rotation q) {
    std::string text;
    for(const auto value:{q.x,q.y,q.z,q.w}) {
        char bytes[64];const auto converted=std::to_chars(bytes,bytes+64,value);
        if(!text.empty())text+=' ';
        text.append(bytes,converted.ptr);
    }
    d.metadata[pose_key(id)]=std::move(text);
}
content::Result<std::span<const u32>> ownership(const Document& d) {
    const auto* f=field(d,"earth/cloud");
    if(!f)return std::unexpected(error("Rebuild clouds once to enable whole-formation editing on this legacy mesh"));
    if(f->type!=vm::FieldType{vm::ScalarType::UInt32,1})return std::unexpected(error("Invalid Earth cloud ownership field"));
    const auto& ids=std::get<std::vector<u32>>(f->values);
    auto catalog=recipes(d);if(!catalog)return std::unexpected(catalog.error());
    auto layer=layers(d);if(!layer)return std::unexpected(layer.error());
    for(std::size_t i=0;i<ids.size();++i)
        if((ids[i] && !catalog->contains(ids[i])) || (ids[i]!=0)!=((*layer)[i]!=0))
            return std::unexpected(error("Cloud ownership disagrees with the Earth layer"));
    return std::span<const u32>{ids};
}
content::Result<void> transform_clouds(Document& d,std::span<const Rotation> rotations) {
    auto ids=ownership(d);if(!ids)return std::unexpected(ids.error());
    for(auto name:{"position","normal"}) {
        const auto* source=field(d,name);
        if(!source || source->type!=vm::FieldType{vm::ScalarType::Float32,3})
            return std::unexpected(error("Cloud movement requires float3 "+std::string(name)));
    }
    for(auto& f:d.vertex_fields)if(f.name=="position" || f.name=="normal") {
        auto& values=std::get<std::vector<f32>>(f.values);
        for(std::size_t i=0;i<d.vertex_count;++i)if(const auto id=(*ids)[i]) {
            const auto q=rotations[id];
            if(q.x==0 && q.y==0 && q.z==0)continue;
            const auto v=rotate(q,{values[i*3],values[i*3+1],values[i*3+2]});
            values[i*3]=v.x;values[i*3+1]=v.y;values[i*3+2]=v.z;
        }
    }
    return {};
}
std::vector<u32> cloud_vertices(std::span<const u32> owners,std::optional<u32> id={}) {
    std::vector<u32> selected;
    for(u32 i=0;i<owners.size();++i)if(id?owners[i]==*id:owners[i]!=0)selected.push_back(i);
    return selected;
}
content::Result<void> isolated(const Document& document,std::span<const u32> owners,u32 id) {
    const auto joined=[&](auto primitive) {
        unsigned count{};for(auto v:primitive.vertices)count+=owners[v]==id;
        return count && count!=primitive.vertices.size();
    };
    for(auto face:document.faces)if(joined(face))return std::unexpected(error("Separate faces connecting this cloud to other formations or terrain before editing it"));
    if(document.edges)for(auto edge:*document.edges)if(joined(edge))return std::unexpected(error("Separate edges connecting this cloud before editing it"));
    return {};
}
// Select/remap whole ownership domains, preserving every field of retained vertices.
content::Result<Document> select_clouds(const Document& source,const Recipes& mapping) {
    auto owners=ownership(source);if(!owners)return std::unexpected(owners.error());
    Document result=source;std::vector<u32> remap(source.vertex_count,UINT32_MAX);result.vertex_count=0;
    for(u32 i=0;i<source.vertex_count;++i)if(mapping.contains((*owners)[i]))remap[i]=u32(result.vertex_count++);
    for(auto& field:result.vertex_fields)std::visit([&](auto& values) {
        auto original=std::move(values);values.clear();values.reserve(result.vertex_count*field.type.components);
        for(u32 i=0;i<source.vertex_count;++i)if(remap[i]!=UINT32_MAX)
            for(unsigned c=0;c<field.type.components;++c)values.push_back(original[i*field.type.components+c]);
    },field.values);
    if(auto it=std::ranges::find(result.vertex_fields,"earth/cloud",&vm::VertexField::name);it!=result.vertex_fields.end())
        for(auto& id:std::get<std::vector<u32>>(it->values))id=mapping.at(id);
    const auto mixed=[&](auto primitive) {
        unsigned retained{};for(auto v:primitive.vertices)retained+=remap[v]!=UINT32_MAX;
        return retained && retained!=primitive.vertices.size();
    };
    result.faces.clear();for(auto face:source.faces) {
        if(mixed(face))return std::unexpected(error("Separate faces bridging cloud formations before editing them"));
        if(remap[face[0]]!=UINT32_MAX)result.faces.push_back({remap[face[0]],remap[face[1]],remap[face[2]]});
    }
    if(source.edges) {
        result.edges.emplace();result.edges->clear();for(auto edge:*source.edges) {
            if(mixed(edge))return std::unexpected(error("Separate edges bridging cloud formations before editing them"));
            if(remap[edge[0]]!=UINT32_MAX)result.edges->push_back({remap[edge[0]],remap[edge[1]]});
        }
    }
    return result;
}
content::Result<void> append_clouds(Document& target,const Document& more) {
    const auto offset=u32(target.vertex_count);
    for(const auto& f:more.vertex_fields)if(!field(target,f.name)) {
        auto added=f;std::visit([&](auto& v){v.assign(target.vertex_count*f.type.components,0);},added.values);
        target.vertex_fields.push_back(std::move(added));
    }
    for(auto& f:target.vertex_fields) {
        const auto* next=field(more,f.name);
        if(next&&next->type!=f.type)return std::unexpected(error("Incompatible cloud field: "+f.name));
        std::visit([&](auto& values) {
            if(next) {const auto& v=std::get<std::decay_t<decltype(values)>>(next->values);values.insert(values.end(),v.begin(),v.end());}
            else values.resize((target.vertex_count+more.vertex_count)*f.type.components,0);
        },f.values);
    }
    for(auto f:more.faces)target.faces.push_back({f[0]+offset,f[1]+offset,f[2]+offset});
    if(more.edges) {if(!target.edges)target.edges.emplace();for(auto e:*more.edges)target.edges->push_back({e[0]+offset,e[1]+offset});}
    target.vertex_count+=more.vertex_count;return {};
}
}
bool is_earth(const Document& d) {
    if(const auto at=d.metadata.find("editor/blueprint");at!=d.metadata.end())return at->second=="earth";
    const auto at=d.metadata.find("source/tool");
    return at!=d.metadata.end() && at->second=="examples/support/earth_assets.cpp";
}
content::Result<CloudSettings> cloud_settings(const Document& d) {
    if(!is_earth(d))return std::unexpected(error("This blueprint does not declare Earth editing behavior"));
    CloudSettings settings;
    // Existing saved meshes predate this control. Their geometry remains the
    // unscattered recipe until the author explicitly changes/rebuilds it.
    settings.edge_scatter=0;
    for(const auto& parameter:parameters)if(const auto at=d.metadata.find(key(parameter.name));at!=d.metadata.end()) {
        const auto& text=at->second;
        const auto [end,ec]=std::from_chars(text.data(),text.data()+text.size(),settings.*parameter.member);
        if(ec!=std::errc{} || end!=text.data()+text.size())return std::unexpected(error("Invalid cloud parameter: "+std::string(parameter.name)));
    }
    if(const auto at=d.metadata.find(key("visible"));at!=d.metadata.end()) {
        if(at->second!="true" && at->second!="false")return std::unexpected(error("Invalid cloud visibility"));
        settings.visible=at->second=="true";
    }
    if(!valid(settings))return std::unexpected(error("Cloud parameters exceed supported authoring ranges"));
    return settings;
}
void write_cloud_settings(Document& d,const CloudSettings& settings) {
    d.metadata["editor/blueprint"]="earth";
    for(const auto& parameter:parameters) {
        char bytes[64];const auto [end,ec]=std::to_chars(bytes,bytes+64,settings.*parameter.member);
        if(ec==std::errc{})d.metadata[key(parameter.name)]=std::string(bytes,end);
    }
    d.metadata[key("visible")]=settings.visible?"true":"false";
}
content::Result<Document> rebuild_clouds(const Document& original,CloudSettings settings) {
    if(!is_earth(original) || !valid(settings))return std::unexpected(error("Invalid Earth cloud rebuild request"));
    if(auto checked=vm::validate(original);!checked)return std::unexpected(checked.error());
    auto frame=mesh_frame::read(original);if(!frame)return std::unexpected(frame.error());
    if(*frame!=Mat4::identity()) {
        auto layer=layers(original);if(!layer)return std::unexpected(layer.error());
        auto vertices=cloud_vertices(*layer);
        auto source=original;source.metadata.erase(std::string(mesh_frame::metadata_key));
        if(auto restored=mesh_frame::bake(source,*mesh_frame::inverse(*frame),std::span<const u32>{vertices});!restored)return std::unexpected(restored.error());
        auto rebuilt=rebuild_clouds(source,settings);if(!rebuilt)return rebuilt;
        layer=layers(*rebuilt);if(!layer)return std::unexpected(layer.error());
        vertices=cloud_vertices(*layer);
        // Terrain stays bit-exact. Only clouds cross the recipe's local frame.
        if(auto baked=mesh_frame::bake(*rebuilt,*frame,std::span<const u32>{vertices});!baked)return std::unexpected(baked.error());
        mesh_frame::write(*rebuilt,*frame);return rebuilt;
    }
    auto layer=layers(original);if(!layer)return std::unexpected(layer.error());
    auto placement=poses(original);if(!placement)return std::unexpected(placement.error());
    auto catalog=recipes(original);if(!catalog)return std::unexpected(catalog.error());
    auto previous=cloud_settings(original);if(!previous)return std::unexpected(previous.error());
    const bool height_only=has_cloud_ownership(original) && previous->visible && settings.visible &&
        previous->coverage==settings.coverage && previous->puff_size==settings.puff_size &&
        previous->spiral_size==settings.spiral_size && previous->edge_scatter==settings.edge_scatter &&
        (previous->altitude!=settings.altitude || previous->relief!=settings.relief);
    if(height_only) {
        // Height controls don't alter footprints or topology. Sample the same
        // authoring kernels at existing vertices; never march/tessellate again.
        auto owners=ownership(original);if(!owners)return std::unexpected(owners.error());
        for(auto face:original.faces) {
            const auto count=(*layer)[face[0]]+(*layer)[face[1]]+(*layer)[face[2]];
            if(count && count!=3)return std::unexpected(error("Clouds are connected to terrain: separate those faces before rebuilding"));
        }
        if(original.edges)for(auto edge:*original.edges)if((*layer)[edge[0]]!=(*layer)[edge[1]])
            return std::unexpected(error("Clouds are connected to terrain by an edge"));
        CloudParticles particles(settings);Document result=original;
        auto position=std::ranges::find(result.vertex_fields,"position",&vm::VertexField::name);
        auto normal=std::ranges::find(result.vertex_fields,"normal",&vm::VertexField::name);
        if(position==result.vertex_fields.end() || normal==result.vertex_fields.end() ||
            position->type!=vm::FieldType{vm::ScalarType::Float32,3} || normal->type!=position->type)
            return std::unexpected(error("Cloud height edit requires float3 position and normal"));
        auto& positions=std::get<std::vector<f32>>(position->values);auto& normals=std::get<std::vector<f32>>(normal->values);
        for(std::size_t i=0;i<original.vertex_count;++i)if(const auto id=(*owners)[i]) {
            const Vec3 point{positions[i*3],positions[i*3+1],positions[i*3+2]};
            if(dot(point,point)<1e-12F)return std::unexpected(error("Cloud point is at Earth's center"));
            const auto q=(*placement)[id];const Rotation inverse{-q.x,-q.y,-q.z,q.w};
            const auto direction=unit(rotate(inverse,point));const auto sample=particles.sample(direction,catalog->at(id));
            const auto p=rotate(q,mul(direction,sample.radius)),n=rotate(q,sample.normal);
            for(unsigned c=0;c<3;++c){positions[i*3+c]=p[c];normals[i*3+c]=n[c];}
        }
        write_cloud_settings(result,settings);return result;
    }
    const auto generated=make_cloud_mesh(settings);
    auto empty=select_clouds(generated,{});if(!empty)return std::unexpected(empty.error());
    auto clouds=std::move(*empty);
    for(const auto& [id,source]:*catalog) {
        auto part=select_clouds(generated,{{source,id}});if(!part)return std::unexpected(part.error());
        if(auto appended=append_clouds(clouds,*part);!appended)return std::unexpected(appended.error());
    }
    clouds.metadata=original.metadata;
    if(auto moved=transform_clouds(clouds,*placement);!moved)return std::unexpected(moved.error());
    Document result;result.metadata=original.metadata;write_cloud_settings(result,settings);
    std::vector<u32> indices(original.vertex_count,std::numeric_limits<u32>::max());
    std::size_t ground_count{};
    for(std::size_t i=0;i<indices.size();++i)if(!(*layer)[i])indices[i]=static_cast<u32>(ground_count++);
    result.vertex_count=ground_count+clouds.vertex_count;
    // Retain every existing field on retained vertices. New cloud vertices use
    // generated Earth attributes; unrelated user fields get explicit zeros.
    for(const auto& source:original.vertex_fields) {
        const auto* generated=field(clouds,source.name);
        if(generated && generated->type!=source.type)return std::unexpected(error("Incompatible Earth field: "+source.name));
        auto target=source;
        std::visit([&](auto& values) {
            using Values=std::decay_t<decltype(values)>;
            const auto& source_values=std::get<Values>(source.values);
            values.clear();values.reserve(result.vertex_count*source.type.components);
            for(std::size_t i=0;i<indices.size();++i)if(!(*layer)[i])
                for(unsigned c=0;c<source.type.components;++c)values.push_back(source_values[i*source.type.components+c]);
            if(generated) {
                const auto& more=std::get<Values>(generated->values);values.insert(values.end(),more.begin(),more.end());
            } else values.resize(result.vertex_count*source.type.components,0);
        },target.values);
        result.vertex_fields.push_back(std::move(target));
    }
    for(const auto& generated:clouds.vertex_fields)if(!field(result,generated.name)) {
        if(generated.name!="earth/layer" && generated.name!="earth/cloud")return std::unexpected(error("Earth rebuild requires the original "+generated.name+" field"));
        std::vector<u32> values(ground_count,0);
        const auto& more=std::get<std::vector<u32>>(generated.values);values.insert(values.end(),more.begin(),more.end());
        result.vertex_fields.push_back({generated.name,generated.type,std::move(values)});
    }
    for(const auto face:original.faces) {
        const auto count=(*layer)[face[0]]+(*layer)[face[1]]+(*layer)[face[2]];
        if(count && count!=3)return std::unexpected(error("Clouds are connected to terrain: separate those faces before rebuilding"));
        if(!count)result.faces.push_back({indices[face[0]],indices[face[1]],indices[face[2]]});
    }
    const auto offset=static_cast<u32>(ground_count);
    for(const auto face:clouds.faces)result.faces.push_back({face[0]+offset,face[1]+offset,face[2]+offset});
    if(original.edges) {
        result.edges.emplace();
        for(const auto edge:*original.edges) {
            if((*layer)[edge[0]]!=(*layer)[edge[1]])return std::unexpected(error("Clouds are connected to terrain by an edge"));
            if(!(*layer)[edge[0]])result.edges->push_back({indices[edge[0]],indices[edge[1]]});
        }
    }
    if(auto checked=vm::validate(result);!checked)return std::unexpected(checked.error());
    return result;
}
bool has_cloud_ownership(const Document& d) {return field(d,"earth/cloud")!=nullptr;}
content::Result<std::vector<CloudFormation>> cloud_formations(const Document& d) {
    if(!is_earth(d))return std::unexpected(error("This is not an Earth blueprint"));
    auto placement=poses(d);if(!placement)return std::unexpected(placement.error());
    auto catalog=recipes(d);if(!catalog)return std::unexpected(catalog.error());
    std::vector<CloudFormation> result;
    for(const auto& [id,source]:*catalog) {
        auto cloud=cloud_catalog()[source-1];cloud.id=id;
        if(id!=source)cloud.name="Cloud "+std::to_string(id)+(source>=15?" / spiral":" / bank");
        result.push_back(std::move(cloud));
    }
    for(auto& cloud:result) {
        const auto n=unit(rotate((*placement)[cloud.id],direction(cloud.location)));
        cloud.location={std::atan2(n.x,n.z)/radians,std::asin(std::clamp(n.y,-1.F,1.F))/radians};
    }
    return result;
}
content::Result<Document> move_cloud(const Document& original,u32 id,Vec2 location) {
    auto catalog=recipes(original);if(!catalog)return std::unexpected(catalog.error());
    if(!is_earth(original) || !catalog->contains(id) ||
       !std::isfinite(location.x) || !std::isfinite(location.y) ||
       std::abs(location.x)>180 || std::abs(location.y)>90)
        return std::unexpected(error("Invalid cloud location: use longitude [-180,180] and latitude [-90,90]"));
    if(auto checked=vm::validate(original);!checked)return std::unexpected(checked.error());
    auto ids=ownership(original);if(!ids)return std::unexpected(ids.error());
    auto frame=mesh_frame::read(original);if(!frame)return std::unexpected(frame.error());
    if(*frame!=Mat4::identity()) {
        const auto vertices=cloud_vertices(*ids,id);
        auto source=original;source.metadata.erase(std::string(mesh_frame::metadata_key));
        if(auto restored=mesh_frame::bake(source,*mesh_frame::inverse(*frame),std::span<const u32>{vertices});!restored)return std::unexpected(restored.error());
        auto moved=move_cloud(source,id,location);if(!moved)return moved;
        if(*moved==source)return original;
        if(auto baked=mesh_frame::bake(*moved,*frame,std::span<const u32>{vertices});!baked)return std::unexpected(baked.error());
        mesh_frame::write(*moved,*frame);return moved;
    }
    // Hand-authored bridges between formations cannot follow a rigid move.
    const auto touches=[&](auto primitive) {
        unsigned selected{};for(auto vertex:primitive.vertices)selected+=(*ids)[vertex]==id;
        return selected && selected!=primitive.vertices.size();
    };
    for(auto face:original.faces)if(touches(face))return std::unexpected(error("Separate faces connecting this cloud to other formations or terrain before moving it"));
    if(original.edges)for(auto edge:*original.edges)if(touches(edge))return std::unexpected(error("Separate edges connecting this cloud before moving it"));
    auto placement=poses(original);if(!placement)return std::unexpected(placement.error());
    const auto from=unit(rotate((*placement)[id],direction(cloud_catalog()[catalog->at(id)-1].location)));
    const auto to=direction(location);
    const Vec3 difference{to.x-from.x,to.y-from.y,to.z-from.z};
    if(dot(difference,difference)<1e-14F)return original;
    const auto delta=between(from,to);
    std::vector<Rotation> changes(placement->size());changes[id]=delta;
    Document result=original;
    if(auto moved=transform_clouds(result,changes);!moved)return std::unexpected(moved.error());
    write_pose(result,id,compose(delta,(*placement)[id]));
    if(auto checked=vm::validate(result);!checked)return std::unexpected(checked.error());
    return result;
}
content::Result<Document> rotate_cloud(const Document& original,u32 id,f32 degrees) {
    auto catalog=recipes(original);if(!catalog)return std::unexpected(catalog.error());
    if(!is_earth(original)||!catalog->contains(id)||!std::isfinite(degrees)||std::abs(degrees)>36000)
        return std::unexpected(error("Invalid cloud rotation"));
    if(auto valid=vm::validate(original);!valid)return std::unexpected(valid.error());
    auto ids=ownership(original);if(!ids)return std::unexpected(ids.error());
    if(degrees==0)return original;
    auto frame=mesh_frame::read(original);if(!frame)return std::unexpected(frame.error());
    if(*frame!=Mat4::identity()) {
        const auto vertices=cloud_vertices(*ids,id);auto local=original;
        local.metadata.erase(std::string(mesh_frame::metadata_key));
        if(auto r=mesh_frame::bake(local,*mesh_frame::inverse(*frame),std::span<const u32>{vertices});!r)return std::unexpected(r.error());
        auto result=rotate_cloud(local,id,degrees);if(!result)return result;
        if(auto r=mesh_frame::bake(*result,*frame,std::span<const u32>{vertices});!r)return std::unexpected(r.error());
        mesh_frame::write(*result,*frame);return result;
    }
    if(auto selected=isolated(original,*ids,id);!selected)return std::unexpected(selected.error());
    auto placement=poses(original);if(!placement)return std::unexpected(placement.error());
    const auto axis=unit(rotate((*placement)[id],direction(cloud_catalog()[catalog->at(id)-1].location)));
    const auto half=degrees*radians*.5F,sine=std::sin(half);
    const Rotation delta{axis.x*sine,axis.y*sine,axis.z*sine,std::cos(half)};
    std::vector<Rotation> changes(placement->size());changes[id]=delta;
    Document result=original;
    if(auto r=transform_clouds(result,changes);!r)return std::unexpected(r.error());
    write_pose(result,id,compose(delta,(*placement)[id]));return result;
}
content::Result<Document> add_cloud(const Document& original,CloudKind kind,Vec2 location) {
    if(auto valid=vm::validate(original);!valid)return std::unexpected(valid.error());
    auto owners=ownership(original);if(!owners)return std::unexpected(owners.error());
    auto catalog=recipes(original);if(!catalog)return std::unexpected(catalog.error());
    if(catalog->size()>=128)return std::unexpected(error("At most 128 cloud formations are supported"));
    auto settings=cloud_settings(original);if(!settings)return std::unexpected(settings.error());
    u32 id=u32(cloud_catalog().size()+1);
    if(auto it=original.metadata.find(key("next-id"));it!=original.metadata.end()) {
        const auto [end,ec]=std::from_chars(it->second.data(),it->second.data()+it->second.size(),id);
        if(ec!=std::errc{}||end!=it->second.data()+it->second.size())return std::unexpected(error("Invalid next cloud identity"));
    }
    if(!catalog->empty())id=std::max(id,catalog->rbegin()->first+1);
    if(id>max_cloud_id)return std::unexpected(error("Cloud identity limit reached"));
    const u32 source=kind==CloudKind::spiral?15:1;
    const auto generated=make_cloud_mesh(*settings,source);
    auto part=select_clouds(generated,{{source,id}});if(!part)return std::unexpected(part.error());
    auto frame=mesh_frame::read(original);if(!frame)return std::unexpected(frame.error());
    if(auto baked=mesh_frame::bake(*part,*frame);!baked)return std::unexpected(baked.error());
    Document result=original;
    result.metadata[key("template/"+std::to_string(id))]=std::to_string(source);
    result.metadata[key("next-id")]=std::to_string(id+1);
    if(auto appended=append_clouds(result,*part);!appended)return std::unexpected(appended.error());
    return move_cloud(result,id,location);
}
content::Result<Document> remove_cloud(const Document& original,u32 id) {
    if(auto valid=vm::validate(original);!valid)return std::unexpected(valid.error());
    auto catalog=recipes(original);if(!catalog)return std::unexpected(catalog.error());
    if(!is_earth(original)||!catalog->contains(id))return std::unexpected(error("Unknown cloud formation"));
    Recipes kept{{0,0}};for(const auto& [other,source]:*catalog)if(other!=id)kept[other]=other;
    auto result=select_clouds(original,kept);if(!result)return result;
    if(id<=cloud_catalog().size())result->metadata[key("removed/"+std::to_string(id))]="true";
    else result->metadata.erase(key("template/"+std::to_string(id)));
    result->metadata.erase(pose_key(id));return result;
}
}
