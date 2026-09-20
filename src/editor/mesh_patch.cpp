#include <vng/editor/mesh_patch.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace vng::editor {
namespace {
using namespace content;
namespace vm=content::vmesh;
auto invalid(std::string message){Diagnostic e;e.message=std::move(message);return std::unexpected(std::move(e));}
void check(bool value,std::string_view message){if(!value)throw std::invalid_argument(std::string(message));}
const vm::VertexField* field(const vm::Document& d,std::string_view name) {
    auto it=std::ranges::find(d.vertex_fields,name,&vm::VertexField::name);return it==d.vertex_fields.end()?nullptr:&*it;
}
void shape(const MeshPatch& p) {
    const auto limits=mesh_limits();
    check(p.vertex_count<=limits.max_vertices,"Mesh patch exceeds vertex budget");
    if(p.replacement) {
        check(!p.vertex_count && p.fields.empty() && p.metadata.empty(),"Mixed mesh patch replacement and values");
        auto valid=vm::validate(*p.replacement,limits);check(bool(valid),valid?"":valid.error().message);return;
    }
    check(p.fields.size()<=limits.max_vertex_fields && p.metadata.size()<=limits.max_metadata_entries,"Mesh patch exceeds field budget");
    std::set<std::string> names;std::size_t scalars{},bytes{};
    for(const auto& a:p.fields) {
        check(names.insert(a.field.name).second,"Duplicate mesh patch field");
        vm::Document sample;sample.vertex_count=a.vertices.size();sample.vertex_fields={a.field};
        auto valid=vm::validate(sample,limits);check(bool(valid),valid?"":valid.error().message);
        scalars+=a.vertices.size()*a.field.type.components;
        check(scalars<=limits.max_scalar_values,"Mesh patch exceeds scalar budget");
        for(std::size_t i=0;i<a.vertices.size();++i)
            check(a.vertices[i]<p.vertex_count && (!i || a.vertices[i]>a.vertices[i-1]),"Invalid mesh patch index order");
    }
    for(const auto& [key,value]:p.metadata) {
        check(!key.empty() && key.size()<=limits.max_metadata_key_bytes &&
            (!value || value->size()<=limits.max_metadata_value_bytes),"Invalid mesh patch metadata");
        bytes+=key.size()+(value?value->size():0);
        check(bytes<=limits.max_decoded_bytes,"Mesh patch metadata exceeds budget");
    }
}
struct Writer {
    std::string bytes{"VNGMESH1"};
    void integer(u32 n){for(unsigned i=0;i<4;++i)bytes+=static_cast<char>((n>>(i*8))&255);}
    void string(std::string_view s){integer(static_cast<u32>(s.size()));bytes+=s;}
    void field(const vm::VertexField& f) {
        string(f.name);integer(static_cast<u32>(f.type.scalar));integer(f.type.components);
        std::visit([&](const auto& values){integer(static_cast<u32>(values.size()));for(auto v:values)integer(std::bit_cast<u32>(v));},f.values);
    }
};
struct Reader {
    std::string_view bytes;std::size_t at{8};
    u32 integer(){check(bytes.size()-at>=4,"Truncated mesh patch");u32 v{};for(unsigned i=0;i<4;++i)v|=u32(static_cast<unsigned char>(bytes[at++]))<<(8*i);return v;}
    bool flag(){const auto v=integer();check(v<=1,"Invalid mesh patch flag");return v!=0;}
    u32 count(std::size_t max){auto n=integer();check(n<=max,"Mesh patch count exceeds budget");return n;}
    std::string string(std::size_t max){auto n=count(max);check(n<=bytes.size()-at,"Truncated mesh patch text");auto s=std::string(bytes.substr(at,n));at+=n;return s;}
    vm::VertexField field() {
        auto name=string(mesh_limits().max_metadata_key_bytes);
        auto scalar=integer(),components=integer();check(scalar<=static_cast<u32>(vm::ScalarType::UInt32) && components>=1 && components<=4,"Invalid mesh patch format");
        const auto n=count(mesh_limits().max_scalar_values);check(n<=(bytes.size()-at)/4,"Truncated mesh field");
        vm::VertexField result{std::move(name),{static_cast<vm::ScalarType>(scalar),static_cast<u8>(components)},std::vector<f32>{}};
        const auto read=[&]<class T>() {std::vector<T> values;values.reserve(n);for(u32 i=0;i<n;++i)values.push_back(std::bit_cast<T>(integer()));return values;};
        switch(result.type.scalar) {
        case vm::ScalarType::Float32:result.values=read.template operator()<f32>();break;
        case vm::ScalarType::Int32:result.values=read.template operator()<i32>();break;
        case vm::ScalarType::UInt32:result.values=read.template operator()<u32>();break;
        }
        return result;
    }
};
}
void MeshChanges::merge(const MeshChanges& other) {
    if(whole || other.whole){*this={.whole=true};return;}
    for(const auto& [name,ids]:other.fields)fields[name].insert(ids.begin(),ids.end());
    metadata.insert(other.metadata.begin(),other.metadata.end());
}
MeshChanges mesh_changes(const vm::Document& a,const vm::Document& b) {
    if(a.vertex_count!=b.vertex_count || a.faces!=b.faces || a.edges!=b.edges || a.vertex_fields.size()!=b.vertex_fields.size())return {.whole=true};
    MeshChanges result;
    for(std::size_t f=0;f<a.vertex_fields.size();++f) {
        const auto& x=a.vertex_fields[f];const auto& y=b.vertex_fields[f];
        if(x.name!=y.name || x.type!=y.type)return {.whole=true};
        if(x.values==y.values)continue;
        auto& ids=result.fields[x.name];
        std::visit([&](const auto& old) {
            const auto& next=std::get<std::decay_t<decltype(old)>>(y.values);
            for(u32 v=0;v<a.vertex_count;++v)for(unsigned c=0;c<x.type.components;++c)
                if(old[v*x.type.components+c]!=next[v*x.type.components+c]){ids.insert(v);break;}
        },x.values);
    }
    for(const auto& [name,value]:a.metadata)if(auto it=b.metadata.find(name);it==b.metadata.end() || it->second!=value)result.metadata.insert(name);
    for(const auto& [name,value]:b.metadata)if(!a.metadata.contains(name))result.metadata.insert(name);
    return result;
}
MeshChanges changes_of(const MeshPatch& p) {
    if(p.replacement)return {.whole=true};
    MeshChanges c;for(const auto& f:p.fields)c.fields[f.field.name].insert(f.vertices.begin(),f.vertices.end());
    for(const auto& [key,value]:p.metadata)c.metadata.insert(key);
    return c;
}
Result<MeshPatch> capture_mesh_patch(const vm::Document& d,const MeshChanges& changes) {
    if(changes.whole)return MeshPatch{.replacement=d};
    MeshPatch patch{.vertex_count=d.vertex_count};
    for(const auto& [name,ids]:changes.fields) {
        const auto* f=field(d,name);if(!f)return invalid("Missing mesh attribute in patch capture");
        AttributePatch a{{f->name,f->type,std::visit([](const auto& values)->vm::FieldValues{return std::decay_t<decltype(values)>{};},f->values)},{ids.begin(),ids.end()}};
        if(!ids.empty() && *ids.rbegin()>=d.vertex_count)return invalid("Mesh patch index outside source");
        std::visit([&](auto& values) {
            const auto& source=std::get<std::decay_t<decltype(values)>>(f->values);
            values.clear();values.reserve(ids.size()*f->type.components);
            for(auto id:ids)for(unsigned c=0;c<f->type.components;++c)values.push_back(source[id*f->type.components+c]);
        },a.field.values);
        patch.fields.push_back(std::move(a));
    }
    for(const auto& name:changes.metadata) {
        auto it=d.metadata.find(name);patch.metadata.emplace(name,it==d.metadata.end()?std::nullopt:std::optional{it->second});
    }
    return patch;
}
Result<EditableMesh> apply_mesh_patch(const EditableMesh& mesh,const MeshPatch& patch) {
    try{shape(patch);}catch(const std::invalid_argument& e){return invalid(e.what());}
    if(patch.replacement)return EditableMesh::create(*patch.replacement);
    if(patch.vertex_count!=mesh.size())return invalid("Mesh patch targets different vertex indexing");
    auto candidate=mesh.document();
    for(const auto& a:patch.fields) {
        auto it=std::ranges::find(candidate.vertex_fields,a.field.name,&vm::VertexField::name);
        if(it==candidate.vertex_fields.end() || it->type!=a.field.type)return invalid("Mesh patch attribute format changed");
        std::visit([&](auto& values) {
            const auto& input=std::get<std::decay_t<decltype(values)>>(a.field.values);
            for(std::size_t i=0;i<a.vertices.size();++i)for(unsigned c=0;c<it->type.components;++c)
                values[a.vertices[i]*it->type.components+c]=input[i*it->type.components+c];
        },it->values);
    }
    for(const auto& [key,value]:patch.metadata){if(value)candidate.metadata[key]=*value;else candidate.metadata.erase(key);}
    return EditableMesh::create(std::move(candidate));
}
Result<void> validate_mesh_patch(const MeshPatch& p) {
    try{shape(p);return {};}catch(const std::invalid_argument& e){return invalid(e.what());}
}
Result<std::string> encode_mesh_patch(const MeshPatch& p) {
    try {
        shape(p);Writer w;w.integer(p.replacement.has_value());
        if(p.replacement) {
            const auto& d=*p.replacement;w.integer(static_cast<u32>(d.vertex_count));
            w.integer(static_cast<u32>(d.metadata.size()));for(const auto& [k,v]:d.metadata){w.string(k);w.string(v);}
            w.integer(static_cast<u32>(d.vertex_fields.size()));for(const auto& f:d.vertex_fields)w.field(f);
            w.integer(static_cast<u32>(d.faces.size()));for(auto f:d.faces)for(auto v:f.vertices)w.integer(v);
            w.integer(d.edges.has_value());if(d.edges){w.integer(static_cast<u32>(d.edges->size()));for(auto e:*d.edges)for(auto v:e.vertices)w.integer(v);}
        } else {
            w.integer(static_cast<u32>(p.vertex_count));w.integer(static_cast<u32>(p.fields.size()));
            for(const auto& f:p.fields){w.field(f.field);w.integer(static_cast<u32>(f.vertices.size()));for(auto v:f.vertices)w.integer(v);}
            w.integer(static_cast<u32>(p.metadata.size()));for(const auto& [k,v]:p.metadata){w.string(k);w.integer(v.has_value());if(v)w.string(*v);}
        }
        check(w.bytes.size()<=mesh_limits().max_source_bytes,"Mesh packet exceeds byte budget");return std::move(w.bytes);
    }catch(const std::invalid_argument& e){return invalid(e.what());}
}
Result<MeshPatch> decode_mesh_patch(std::string_view bytes) {
    try {
        const auto limits=mesh_limits();check(bytes.size()>=8 && bytes.size()<=limits.max_source_bytes && bytes.substr(0,8)=="VNGMESH1","Invalid mesh packet");
        Reader r{bytes};MeshPatch p;
        if(r.flag()) {
            auto& d=p.replacement.emplace();d.vertex_count=r.count(limits.max_vertices);
            const auto metadata=r.count(limits.max_metadata_entries);for(u32 i=0;i<metadata;++i){auto k=r.string(limits.max_metadata_key_bytes);auto v=r.string(limits.max_metadata_value_bytes);check(d.metadata.emplace(std::move(k),std::move(v)).second,"Duplicate mesh metadata");}
            const auto fields=r.count(limits.max_vertex_fields);for(u32 i=0;i<fields;++i)d.vertex_fields.push_back(r.field());
            const auto faces=r.count(limits.max_faces);for(u32 i=0;i<faces;++i)d.faces.push_back({r.integer(),r.integer(),r.integer()});
            if(r.flag()){d.edges.emplace();const auto edges=r.count(limits.max_edges);for(u32 i=0;i<edges;++i)d.edges->push_back({r.integer(),r.integer()});}
        } else {
            p.vertex_count=r.count(limits.max_vertices);
            const auto fields=r.count(limits.max_vertex_fields);for(u32 i=0;i<fields;++i){AttributePatch a{r.field(),{}};const auto n=r.count(limits.max_vertices);for(u32 j=0;j<n;++j)a.vertices.push_back(r.integer());p.fields.push_back(std::move(a));}
            const auto metadata=r.count(limits.max_metadata_entries);for(u32 i=0;i<metadata;++i){auto k=r.string(limits.max_metadata_key_bytes);auto v=r.flag()?std::optional{r.string(limits.max_metadata_value_bytes)}:std::nullopt;check(p.metadata.emplace(std::move(k),std::move(v)).second,"Duplicate mesh metadata");}
        }
        check(r.at==bytes.size(),"Trailing mesh patch bytes");shape(p);return p;
    }catch(const std::invalid_argument& e){return invalid(e.what());}
}
}
