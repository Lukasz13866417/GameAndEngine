#include "earth_assets.hpp"
#include "earth_clouds.hpp"
#include "earth_geography.hpp"
#include "mesh_frame.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numbers>

namespace example::earth {
namespace {
using namespace vng;
constexpr f32 radians=std::numbers::pi_v<f32>/180.F;
Vec3 add(Vec3 a,Vec3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
Vec3 sub(Vec3 a,Vec3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
Vec3 mul(Vec3 a,f32 b){return {a.x*b,a.y*b,a.z*b};}
f32 dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
Vec3 unit(Vec3 p){return mul(p,1.F/std::sqrt(dot(p,p)));}
Vec3 direction(Vec2 p){return {std::sin(p.x*radians)*std::cos(p.y*radians),std::sin(p.y*radians),std::cos(p.x*radians)*std::cos(p.y*radians)};}
Vec2 location(Vec3 p){return {std::atan2(p.x,p.z)/radians,std::asin(std::clamp(p.y,-1.F,1.F))/radians};}
f32 noise(Vec3 p){return std::sin(p.x*6.3F+std::sin(p.z*4.1F))*std::sin(p.y*7.7F+p.z*2.2F);}
f32 smooth(f32 a,f32 b,f32 v){const auto t=std::clamp((v-a)/(b-a),0.F,1.F);return t*t*(3-2*t);}
f32 band(f32 low,f32 high,f32 feather,f32 value) {
    return smooth(low-feather,low+feather,value)*(1-smooth(high-feather,high+feather,value));
}
Vec3 blend(Vec3 a,Vec3 b,f32 weight){return add(mul(a,1-weight),mul(b,weight));}
// Sample on the sphere, not in longitude/latitude, so the painted detail has no
// dateline seam or pole stretch. Frequencies stay below the land grid's limit.
f32 surface_detail(Vec3 n) {
    return .58F*noise(mul(n,5.F))
        +.29F*noise(add(mul(Vec3{n.z,n.x,n.y},9.F),Vec3{2.3F,-1.7F,4.1F}))
        +.13F*noise(mul(Vec3{n.y,n.z,n.x},13.F));
}
f32 aridity(Vec2 location,f32 variation,f32 extent_scale=1.F) {
    struct Region {Vec2 center,extent;f32 strength;};
    // Approximate climate shapes, not country rectangles. Overlapping dry cores
    // merge into one field with broad, irregular scrub/grassland transitions.
    constexpr std::array regions{
        Region{{13,24},{37,13},1},       // Sahara / Sahel
        Region{{48,23},{15,12},1},       // Arabian peninsula
        Region{{59,32},{13,8},.95F},     // Iranian plateau
        Region{{62,42},{13,8},.85F},     // Central Asian deserts / steppe
        Region{{83,40},{10,6},1},        // Taklamakan
        Region{{104,43},{17,8},1},       // Gobi
        Region{{71,27},{6,5},.9F},       // Thar
        Region{{-112,30},{8,7},1},       // Sonoran / Baja / southwest US
        Region{{-104,28},{7,8},1},       // Chihuahuan / northern Mexico
        Region{{-102,23},{5,5},.6F},     // Drier Mexican plateau
        Region{{20,-24},{11,11},.95F},   // Namib / Kalahari
        Region{{134,-25},{18,11},1}};    // Australian interior
    f32 result{};
    for(const auto& region:regions) {
        auto lon=location.x-region.center.x;
        lon-=360.F*std::round(lon/360.F);
        const auto radius=std::hypot(lon/region.extent.x,(location.y-region.center.y)/region.extent.y)/extent_scale;
        result=std::max(result,region.strength*(1-smooth(.5F,1.18F,radius+variation*.12F)));
    }
    return result;
}
f32 land_distance(Vec3 n) {
    const auto uv=location(n);
    // The sea-ice cap and Antarctic coastline are broad graphic shapes.
    f32 result=std::max(uv.y-79.F-2.F*std::sin(uv.x*radians*4),-uv.y-71.F-3.F*std::sin(uv.x*radians*3));
    for(const auto& polygon:geography::continents) {
        auto p=uv;f32 center{};for(auto v:polygon)center+=v.x;center/=static_cast<f32>(polygon.size());
        p.x+=360.F*std::round((center-p.x)/360.F);
        f32 distance=1e6F;bool inside{};
        for(std::size_t i=0,j=polygon.size()-1;i<polygon.size();j=i++) {
            const auto a=polygon[j],b=polygon[i];
            if((a.y>p.y)!=(b.y>p.y) && p.x<(b.x-a.x)*(p.y-a.y)/(b.y-a.y)+a.x)inside=!inside;
            const auto dx=b.x-a.x,dy=b.y-a.y;
            const auto t=std::clamp(((p.x-a.x)*dx+(p.y-a.y)*dy)/(dx*dx+dy*dy),0.F,1.F);
            distance=std::min(distance,std::hypot(p.x-a.x-t*dx,p.y-a.y-t*dy));
        }
        result=std::max(result,inside?distance:-distance);
    }
    return result;
}
struct Sample {Vec3 normal;f32 distance;};
using Polygon=std::vector<Sample>;
Polygon clip(std::span<const Sample> polygon,f32 threshold) {
    Polygon result;
    for(std::size_t i=0;i<polygon.size();++i) {
        const auto a=polygon[i],b=polygon[(i+1)%polygon.size()];
        if(a.distance>=threshold)result.push_back(a);
        if((a.distance>=threshold)!=(b.distance>=threshold)) {
            const auto t=(threshold-a.distance)/(b.distance-a.distance);
            result.push_back({unit(add(mul(a.normal,1-t),mul(b.normal,t))),threshold});
        }
    }
    return result;
}
enum class Layer {ocean,land,cloud};
struct Vertex {Vec3 position,normal,color;f32 emission;};
Vertex surface(Sample sample,Layer layer,const CloudParticles& clouds,u32 formation) {
    const auto n=sample.normal;
    const auto uv=location(n);
    f32 radius=1,emission=.035F;Vec3 color{};
    const auto grain=noise(mul(n,2.1F));
    if(layer==Layer::ocean) {
        // Readable cobalt oceans and a narrow, quiet turquoise shelf.
        color=add(Vec3{.009F,.06F,.23F},mul(Vec3{.005F,.008F,.018F},grain));
        const auto coastal=1-smooth(0.F,4.F,std::abs(sample.distance));
        color=add(color,mul(Vec3{.008F,.055F,.045F},coastal));
    } else if(layer==Layer::land) {
        radius=1.004F+.003F*smooth(0.F,5.F,sample.distance);
        const auto detail=surface_detail(n);
        const auto dry=aridity(uv,grain*.6F+detail*.4F);
        const auto vegetation=add(Vec3{.085F,.235F,.095F},mul(Vec3{.045F,.065F,.028F},detail));
        const auto scrub=add(Vec3{.29F,.285F,.13F},mul(Vec3{.055F,.046F,.023F},detail));
        const auto sand=add(Vec3{.52F,.365F,.17F},mul(Vec3{.085F,.068F,.043F},detail));
        // Vegetation -> dry grass/scrub -> sand. The intermediate palette avoids
        // a hard green/beige contour; fine painted variation crosses that blend.
        color=blend(blend(vegetation,scrub,dry),blend(scrub,sand,dry),dry);
        const auto andes_axis=-72.F+uv.y*.04F+1.1F*std::sin(uv.y*.16F);
        const auto andes=(1-smooth(.25F,2.8F,std::abs(uv.x-andes_axis)))*band(-49,8,1.5F,uv.y);
        const auto himalaya=band(72,101,1.5F,uv.x)*(1-smooth(.3F,2.8F,std::abs(uv.y-(35.F-(uv.x-72)*.24F))));
        const auto mountains=std::max(andes,himalaya);
        radius+=.007F*mountains;
        color=blend(color,add(Vec3{.29F,.31F,.22F},mul(Vec3{.035F,.033F,.027F},detail)),mountains);
        const auto polar=smooth(64.F+grain*2,69.F+grain*2,std::abs(uv.y));
        const auto greenland=band(-65,-15,1,uv.x)*smooth(60,62,uv.y);
        color=blend(color,Vec3{.56F,.7F,.73F},std::max({polar,greenland,himalaya}));
        const auto rainforest=band(-8,9,3,uv.y)*std::max(1-smooth(-52,-46,uv.x),band(8,35,3,uv.x));
        color=blend(color,add(Vec3{.065F,.19F,.095F},mul(Vec3{.024F,.045F,.019F},detail)),rainforest);
    } else {
        const auto puff=clouds.sample(n,formation);
        const auto middle=smooth(CloudParticles::boundary,1.4F,puff.density);
        color=blend(Vec3{.48F,.58F,.68F},Vec3{.84F,.87F,.89F},middle);
        // Dense overlapping cyclone kernels otherwise saturate into a flat
        // white disk. Fine baked cloud-top variation stays below the contour
        // grid's sampling limit and adds no texture/resource at runtime.
        const auto detail=.65F*noise(mul(n,14.F))+.35F*noise(mul(Vec3{n.z,n.x,n.y},23.F));
        color=mul(color,1.F-(.05F+.10F*detail)*smooth(1.4F,3.F,puff.density));
        return {mul(n,puff.radius),puff.normal,color,.015F};
    }
    return {mul(n,radius),n,color,emission};
}
class Builder {
    CloudParticles clouds_;
    content::vmesh::Document result_;
    std::vector<f32> positions_,normals_,colors_,emissions_;
    std::vector<u32> layers_,formations_;
    u32 formation_{};
    std::map<std::array<int,11>,u32> vertices_;
    u32 vertex(Vertex v,Layer layer) {
        std::array<int,11> key{};unsigned at{};
        for(auto x:{v.position.x,v.position.y,v.position.z,v.normal.x,v.normal.y,v.normal.z,v.color.x,v.color.y,v.color.z,v.emission})key[at++]=static_cast<int>(std::round(x*100000));
        key[10]=static_cast<int>(formation_);
        if(auto found=vertices_.find(key);found!=vertices_.end())return found->second;
        const auto id=static_cast<u32>(positions_.size()/3);vertices_.emplace(key,id);
        positions_.insert(positions_.end(),{v.position.x,v.position.y,v.position.z});
        normals_.insert(normals_.end(),{v.normal.x,v.normal.y,v.normal.z});
        colors_.insert(colors_.end(),{v.color.x,v.color.y,v.color.z,1});emissions_.push_back(v.emission);
        layers_.push_back(layer==Layer::cloud?1U:0U);formations_.push_back(formation_);return id;
    }
public:
    explicit Builder(CloudSettings settings):clouds_(settings) {}
    void formation(u32 id){formation_=id;}
    CloudParticles::Bounds cloud_bounds(u32 id)const{return clouds_.bounds(id);}
    f32 density(Vec3 n,Layer layer) const {
        return layer==Layer::cloud?clouds_.sample(n,formation_).density-CloudParticles::boundary:land_distance(n);
    }
    void polygon(std::span<const Sample> samples,Layer layer) {
        for(std::size_t i=1;i+1<samples.size();++i) {
            auto a=surface(samples[0],layer,clouds_,formation_),b=surface(samples[i],layer,clouds_,formation_),c=surface(samples[i+1],layer,clouds_,formation_);
            const auto face=cross(sub(b.position,a.position),sub(c.position,a.position));
            if(dot(face,face)<1e-14F)continue;
            // Winding follows the spherical surface, not the deliberately
            // softened puff lighting normals (which can lean across a face).
            if(dot(face,a.position)<0)std::swap(b,c);
            const auto ia=vertex(a,layer),ib=vertex(b,layer),ic=vertex(c,layer);
            if(ia!=ib&&ib!=ic&&ia!=ic)result_.faces.emplace_back(ia,ib,ic);
        }
    }
    content::vmesh::Document finish() {
        using namespace content::vmesh;
        result_.metadata={{"name","EARTH / stylized homeworld"},{"author","Original Vibe Engine authored geometry"},
            {"source/tool","examples/support/earth_assets.cpp"},{"render/lighting","illustrated"},{"editor/blueprint","earth"},
            {"geography","Hand-drawn simplified continents; not survey data"},
            {"clouds","Seeded puff particles blended into a static cloud surface"},
            {"style","Illustrated sphere, warm desert belts, fine painted variation, blended biomes, billowing cloud clusters"}};
        result_.vertex_count=positions_.size()/3;
        result_.vertex_fields={{"position",{ScalarType::Float32,3},std::move(positions_)},
            {"normal",{ScalarType::Float32,3},std::move(normals_)},{"color/0",{ScalarType::Float32,4},std::move(colors_)},
            {"emission",{ScalarType::Float32,1},std::move(emissions_)},
            {"earth/layer",{ScalarType::UInt32,1},std::move(layers_)},
            {"earth/cloud",{ScalarType::UInt32,1},std::move(formations_)}};
        return std::move(result_);
    }
};
void add_layer(Builder& mesh,Layer layer,unsigned longitude,unsigned latitude,
               CloudParticles::Bounds bounds={{-180,-90},{180,90}}) {
    // Keep the same global lattice, but visit only a formation's compact
    // support. Longitude indices can cross the dateline; sin/cos wrap them.
    const auto x0=static_cast<int>(std::floor((bounds.minimum.x+180)*static_cast<f32>(longitude)/360));
    const auto x1=std::min(x0+static_cast<int>(longitude),static_cast<int>(std::ceil((bounds.maximum.x+180)*static_cast<f32>(longitude)/360)));
    const auto y0=std::clamp(static_cast<int>(std::floor((bounds.minimum.y+90)*static_cast<f32>(latitude)/180)),0,static_cast<int>(latitude));
    const auto y1=std::clamp(static_cast<int>(std::ceil((bounds.maximum.y+90)*static_cast<f32>(latitude)/180)),y0,static_cast<int>(latitude));
    const auto width=static_cast<unsigned>(x1-x0+1),height=static_cast<unsigned>(y1-y0+1);
    std::vector<Sample> samples;
    samples.reserve(width*height);
    for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x) {
        const auto n=direction({-180.F+360.F*static_cast<f32>(x)/static_cast<f32>(longitude),
                               -90.F+180.F*static_cast<f32>(y)/static_cast<f32>(latitude)});
        samples.push_back({n,mesh.density(n,layer)});
    }
    for(unsigned y=0;y+1<height;++y)for(unsigned x=0;x+1<width;++x) {
        const auto a=y*width+x,b=a+1,c=a+width,d=c+1;
        for(const auto ids:std::array{std::array{a,b,d},std::array{a,d,c}}) {
            const std::array triangle{samples[ids[0]],samples[ids[1]],samples[ids[2]]};
            if(layer==Layer::ocean)mesh.polygon(triangle,layer);
            else mesh.polygon(clip(triangle,0),layer);
        }
    }
}
void add_clouds(Builder& mesh, u32 source=0) {
    // Separate closed ownership domains even when two formations overlap. A
    // face can never span clouds, so moving one cannot tear its neighbor.
    for(const auto& cloud:cloud_catalog()) {
        if(source && source!=cloud.id)continue;
        mesh.formation(cloud.id);
        add_layer(mesh,Layer::cloud,448,224,mesh.cloud_bounds(cloud.id));
    }
}
}
content::vmesh::Document make_mesh(CloudSettings settings) {
    Builder mesh(settings);
    // The smooth ocean needs little geometry. Spend the close-up budget on
    // coastline/terrain and cloud outlines instead of another dense full sphere.
    // Resample the fields before clipping: subdividing the old triangles would
    // only repeat their straight edges, without recovering small bays/islands.
    add_layer(mesh,Layer::ocean,128,64);
    add_layer(mesh,Layer::land,256,128);
    if(settings.visible)add_clouds(mesh);
    auto result=mesh.finish();write_cloud_settings(result,settings);return result;
}
content::vmesh::Document make_cloud_mesh(CloudSettings settings,u32 source) {
    Builder mesh(settings);
    if(settings.visible)add_clouds(mesh,source);
    auto result=mesh.finish();write_cloud_settings(result,settings);return result;
}
content::Result<content::vmesh::Document> make_savannah_variant(const content::vmesh::Document& source) {
    namespace vm=content::vmesh;
    const auto invalid=[](std::string message)->content::Result<vm::Document> {
        return std::unexpected(example::mesh_frame::error(std::move(message)));
    };
    if(!is_earth(source))return invalid("Savannah variation needs an Earth blueprint");
    if(auto valid=vm::validate(source);!valid)return std::unexpected(valid.error());
    auto frame=example::mesh_frame::read(source);if(!frame)return std::unexpected(frame.error());
    auto inverse=example::mesh_frame::inverse(*frame);if(!inverse)return std::unexpected(inverse.error());
    auto result=source;
    const auto field=[&](std::string_view name){return std::ranges::find(result.vertex_fields,name,&vm::VertexField::name);};
    auto p=field("position"),c=field("color/0"),layer=field("earth/layer"),emission=field("emission");
    if(p==result.vertex_fields.end()||c==result.vertex_fields.end()||
       p->type!=vm::FieldType{vm::ScalarType::Float32,3}||c->type!=vm::FieldType{vm::ScalarType::Float32,4})
        return invalid("Earth variation needs position and color/0 fields");
    // Earth layer IDs distinguish terrain (0) from clouds (1), not land from
    // ocean. Shipped legacy assets use the generator's emission stamps instead.
    const std::vector<u32>* layers{};const std::vector<f32>* emissions{};
    if(layer!=result.vertex_fields.end()) {
        if(layer->type!=vm::FieldType{vm::ScalarType::UInt32,1})return invalid("Invalid Earth layer field");
        layers=&std::get<std::vector<u32>>(layer->values);
        if(std::ranges::any_of(*layers,[](auto value){return value>1;}))return invalid("Unknown Earth layer");
    } else {
        if(emission==result.vertex_fields.end()||emission->type!=vm::FieldType{vm::ScalarType::Float32,1})return invalid("Legacy Earth needs emission layer stamps");
        emissions=&std::get<std::vector<f32>>(emission->values);
        if(std::ranges::any_of(*emissions,[](auto value){return std::abs(value-.015F)>1e-6F&&std::abs(value-.035F)>1e-6F;}))
            return invalid("Legacy Earth layer ownership is ambiguous");
    }
    const auto& positions=std::get<std::vector<f32>>(p->values);
    auto& colors=std::get<std::vector<f32>>(c->values);
    for(std::size_t i=0;i<source.vertex_count;++i)if(layers?(*layers)[i]==0:(*emissions)[i]>.03F) {
        auto local=example::mesh_frame::point(*inverse,{positions[i*3],positions[i*3+1],positions[i*3+2]});
        if(dot(local,local)<1.002F*1.002F)continue; // Leave the unit ocean shell unchanged.
        const auto n=unit(local);const auto uv=location(n);
        const auto detail=surface_detail(n),grain=noise(mul(n,2.1F));
        const auto variation=grain*.6F+detail*.4F;
        const auto growth=std::max(0.F,aridity(uv,variation,1.12F)-aridity(uv,variation));
        const Vec3 old{colors[i*4],colors[i*4+1],colors[i*4+2]};
        // Keep ice, pale peaks and cool coastal colors; warm most vegetation
        // without turning the whole landmass into sand. Wet tropics stay greener.
        const auto ice=smooth(.3F,.5F,std::min({old.x,old.y,old.z}));
        const auto green=smooth(.01F,.09F,old.y-old.x)*(1-smooth(0.F,.04F,old.z-old.y));
        const auto wet=1-smooth(7.F,18.F,std::abs(uv.y));
        auto color=add(old,mul(Vec3{.053F,-.005F,-.012F},green*(.92F+.08F*detail)*(1-.3F*wet)*(1-ice)));
        const auto dry_scrub=add(Vec3{.40F,.325F,.145F},mul(Vec3{.035F,.03F,.016F},detail));
        color=blend(color,dry_scrub,growth*.48F*(1-ice));
        for(unsigned channel=0;channel<3;++channel)colors[i*4+channel]=std::clamp(color[channel],0.F,1.F);
    }
    result.metadata["name"]="Earth / savannah homeworld";
    result.metadata["editor/earth-variant"]="savannah";
    result.metadata["style"]="Earth copy: slightly wider deserts, warm savannah greens; original geometry and clouds retained";
    return result;
}
}
