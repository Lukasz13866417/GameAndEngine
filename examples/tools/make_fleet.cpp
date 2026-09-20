// Original, reproducible fleet assets. No downloaded models or textures.
// Offline tool: vng_make_fleet examples/assets
// The editor and demos consume the generated .vmesh files, not this generator.
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <vng/content/vmesh.hpp>

namespace {
using vng::Vec3;
using vng::Vec4;
namespace vmesh = vng::content::vmesh;

Vec3 add(Vec3 a, Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec3 mul(Vec3 a, float s) { return {a.x*s,a.y*s,a.z*s}; }
float dot(Vec3 a, Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec3 cross(Vec3 a, Vec3 b)
{
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
Vec3 center(std::span<const Vec3> points)
{
    Vec3 result{};
    for (auto p : points) result=add(result,p);
    return mul(result,1.0F/static_cast<float>(points.size()));
}

struct Material { Vec4 color; float emission{}; };
constexpr Material hull{{.10F,.145F,.18F,1}};
constexpr Material dark{{.018F,.028F,.037F,1}};
constexpr Material steel{{.22F,.29F,.32F,1}};
constexpr Material light{{.56F,.60F,.57F,1}};
constexpr Material copper{{.48F,.18F,.067F,1}};
constexpr Material blue{{.10F,.25F,.34F,1}};
constexpr Material teal{{.10F,.42F,.42F,1}};
constexpr Material red{{.37F,.073F,.052F,1}};
constexpr Material glass{{.04F,.26F,.33F,1},.65F};
constexpr Material cyan{{.075F,.53F,.81F,1},4.0F};
constexpr Material amber{{.8F,.23F,.025F,1},2.2F};

struct Model {
    std::vector<float> positions,normals,colors,emissions;
    std::vector<vng::gfx::TriangleFace> faces;

    void triangle(Vec3 a,Vec3 b,Vec3 c,Material material,Vec3 inside)
    {
        Vec3 n=cross(sub(b,a),sub(c,a));
        if(dot(n,sub(mul(add(add(a,b),c),1.0F/3.0F),inside))<0) {
            std::swap(b,c); n=mul(n,-1);
        }
        const float length=std::sqrt(dot(n,n));
        if(!(length>1.0e-8F)) throw std::runtime_error("degenerate authored triangle");
        n=mul(n,1/length);
        const auto first=static_cast<vng::u32>(positions.size()/3);
        for(auto p : {a,b,c}) {
            positions.insert(positions.end(),{p.x,p.y,p.z});
            normals.insert(normals.end(),{n.x,n.y,n.z});
            colors.insert(colors.end(),{material.color.x,material.color.y,
                                        material.color.z,material.color.w});
            emissions.push_back(material.emission);
        }
        faces.emplace_back(first,first+1,first+2);
    }
    void polygon(std::span<const Vec3> points,Material material,Vec3 inside)
    {
        for(std::size_t i=1;i+1<points.size();++i)
            triangle(points[0],points[i],points[i+1],material,inside);
    }
    void quad(Vec3 a,Vec3 b,Vec3 c,Vec3 d,Material material,Vec3 inside)
    {
        polygon(std::array{a,b,c,d},material,inside);
    }
    // All input polygons here are convex and planar, with explicit thickness.
    void prism(std::span<const Vec3> base,Vec3 extrusion,Material top,Material side=hull)
    {
        std::vector<Vec3> end;
        for(auto p : base) end.push_back(add(p,extrusion));
        const auto inside=add(center(base),mul(extrusion,.5F));
        polygon(base,side,inside); polygon(end,top,inside);
        for(std::size_t i=0;i<base.size();++i) {
            const auto j=(i+1)%base.size();
            quad(base[i],base[j],end[j],end[i],side,inside);
        }
    }
    void box(Vec3 low,Vec3 high,Material top,Material side=hull)
    {
        prism(std::array{low,Vec3{high.x,low.y,low.z},
                         Vec3{high.x,low.y,high.z},Vec3{low.x,low.y,high.z}},
              {0,high.y-low.y,0},top,side);
    }
    void panel(std::array<Vec3,4> patch,Vec3 inside,Material material)
    {
        const auto middle=center(patch);
        auto normal=cross(sub(patch[1],patch[0]),sub(patch[3],patch[0]));
        normal=mul(normal,1/std::sqrt(dot(normal,normal)));
        if(dot(normal,sub(middle,inside))<0) normal=mul(normal,-1);
        for(auto& p : patch) p=add(add(p,mul(sub(middle,p),.095F)),mul(normal,.012F));
        prism(patch,mul(normal,.025F),material,steel);
    }
    vmesh::Document document(std::string name,std::string parts) &&
    {
        vmesh::Document result;
        result.metadata={
            {"name",std::move(name)},{"author","Vibe Engine example asset"},
            {"source/tool","examples/tools/make_fleet.cpp"},
            {"coordinates/up","+Y"},{"coordinates/forward","-Z"},
            {"coordinates/unit","metre"},
            {"shading","flat geometric normals; linear vertex colors; emission is a multiplier"},
            {"parts",std::move(parts)}};
        result.vertex_count=positions.size()/3;
        result.vertex_fields={
            {"position",{vmesh::ScalarType::Float32,3},std::move(positions)},
            {"normal",{vmesh::ScalarType::Float32,3},std::move(normals)},
            {"color/0",{vmesh::ScalarType::Float32,4},std::move(colors)},
            {"emission",{vmesh::ScalarType::Float32,1},std::move(emissions)}};
        result.faces=std::move(faces);
        return result;
    }
};

struct Section { float z,width,height; };
std::array<Vec3,8> ring(Section s,Vec3 offset)
{
    std::array<Vec3,8> points{{
        {-.65F*s.width,s.height,s.z},{.65F*s.width,s.height,s.z},
        {s.width,.45F*s.height,s.z},{s.width,-.45F*s.height,s.z},
        {.65F*s.width,-s.height,s.z},{-.65F*s.width,-s.height,s.z},
        {-s.width,-.45F*s.height,s.z},{-s.width,.45F*s.height,s.z}}};
    for(auto& p : points) p=add(p,offset);
    return points;
}
void armored_hull(Model& m,std::span<const Section> sections,Vec3 offset,
                  Material armor,Material belt)
{
    m.polygon(ring(sections.front(),offset),belt,add(offset,{0,0,sections.front().z+.1F}));
    m.polygon(ring(sections.back(),offset),belt,add(offset,{0,0,sections.back().z-.1F}));
    for(std::size_t s=0;s+1<sections.size();++s) {
        const auto a=ring(sections[s],offset),b=ring(sections[s+1],offset);
        const auto inside=add(offset,{0,0,.5F*(sections[s].z+sections[s+1].z)});
        for(std::size_t i=0;i<a.size();++i) {
            const auto j=(i+1)%a.size();
            const std::array patch{a[i],a[j],b[j],b[i]};
            m.polygon(patch,dark,inside);
            m.panel(patch,inside,(i==2||i==6) ? belt : armor);
        }
    }
}
Vec3 radial(Vec3 center,float radius,float angle)
{
    return {center.x+radius*std::cos(angle),center.y+radius*std::sin(angle),center.z};
}
void nozzle(Model& m,Vec3 origin,float radius,float length)
{
    // Hollow stepped ceramic nozzle, inset luminous core, short tapered exhaust.
    constexpr unsigned segments=12;
    for(unsigned i=0;i<segments;++i) {
        const float a=static_cast<float>(i)*2*std::numbers::pi_v<float>/segments;
        const float b=static_cast<float>(i+1)*2*std::numbers::pi_v<float>/segments;
        for(unsigned band=0;band<3;++band) {
            auto c=add(origin,{0,0,static_cast<float>(band)*radius*.18F});
            const float outer=radius*(1-static_cast<float>(band)*.08F),inner=outer*.78F;
            m.prism(std::array{radial(c,outer,a),radial(c,outer,b),
                              radial(c,inner,b),radial(c,inner,a)},
                    {0,0,radius*.2F},band==1 ? copper : steel,dark);
        }
        auto c=add(origin,{0,0,radius*.38F});
        m.triangle(c,radial(c,radius*.63F,a),radial(c,radius*.63F,b),cyan,
                   add(c,{0,0,-.05F}));
        const auto end=add(origin,{0,0,length});
        m.quad(radial(c,radius*.48F,a),radial(c,radius*.48F,b),
               radial(end,radius*.025F,b),radial(end,radius*.025F,a),
               Material{{.025F,.24F,.44F,1},1.8F},add(c,{0,0,length*.5F}));
    }
}
void top_vents(Model& m,float x,float y,float z,float width,unsigned count)
{
    m.box({x-width*.5F,y,z},{x+width*.5F,y+.06F,z+static_cast<float>(count)*.24F},dark);
    for(unsigned i=0;i<count;++i) {
        const auto slot=z+.035F+static_cast<float>(i)*.24F;
        m.box({x-width*.46F,y+.065F,slot},{x+width*.46F,y+.10F,slot+.075F},steel);
    }
}
void bridge(Model& m,Vec3 center,float width,float height,float length,Material top)
{
    const float x=center.x,y=center.y,z=center.z;
    m.box({x-width*.36F,y,z-length*.30F},{x+width*.36F,y+height*.7F,z+length*.3F},top);
    m.box({x-width*.5F,y+height*.64F,z-length*.5F},
          {x+width*.5F,y+height,z+length*.5F},top,steel);
    m.box({x-width*.44F,y+height*.71F,z-length*.505F},
          {x+width*.44F,y+height*.87F,z-length*.50F},glass,glass);
    for(float sign : {-1.0F,1.0F}) {
        float sx=x+sign*width*.501F;
        m.box({sx-.008F,y+height*.71F,z-length*.40F},
              {sx+.008F,y+height*.87F,z+length*.23F},glass,glass);
    }
}

vmesh::Document carrier()
{
    Model m;
    constexpr std::array sections{
        Section{-8.4F,.42F,.38F},Section{-7.4F,1.10F,.74F},
        Section{-5.5F,1.40F,.88F},Section{-3.5F,1.48F,.95F},
        Section{-1.5F,1.50F,1.03F},Section{.5F,1.50F,1.03F},
        Section{2.5F,1.48F,1.02F},Section{4.5F,1.35F,.98F},
        Section{6.6F,1.04F,.88F}};
    // Split bow / two independent armored flight-deck rails, not a solid wedge.
    for(float sign : {-1.0F,1.0F}) {
        armored_hull(m,sections,{sign*3.30F,0,0},light,blue);
        nozzle(m,{sign*3.30F,0,6.62F},.85F,2.0F);
        top_vents(m,sign*3.30F,1.07F,-1.2F,1.35F,7);
        for(unsigned i=0;i<6;++i) {
            const float z=-5.2F+static_cast<float>(i)*1.6F;
            m.box({sign*3.30F-.52F,1.04F,z},{sign*3.30F+.52F,1.13F,z+.22F},copper);
            m.box({sign*4.74F-.035F,-.13F,z},{sign*4.74F+.035F,.05F,z+.38F},amber,amber);
        }
    }
    m.box({-2.14F,-.81F,-5.6F},{2.14F,-.59F,4.8F},dark,steel);
    for(float x : {-1.55F,1.55F}) {
        m.box({x-.055F,-.58F,-5.5F},{x+.055F,-.55F,2.7F},cyan,cyan);
        for(unsigned i=0;i<5;++i) {
            float z=-4.8F+static_cast<float>(i)*1.3F;
            m.box({x-.20F,-.57F,z},{x+.20F,-.53F,z+.085F},light);
        }
    }
    // Stern superstructure closes the hangar; its dark forward opening remains legible.
    m.box({-2.15F,-.56F,2.8F},{2.15F,1.24F,5.8F},hull,steel);
    m.box({-1.65F,-.53F,2.76F},{1.65F,.71F,2.80F},dark,dark);
    m.box({-1.73F,.72F,2.68F},{1.73F,.87F,2.83F},copper,copper);
    m.box({-2.25F,1.13F,.83F},{2.25F,1.48F,5.3F},light);
    bridge(m,{.55F,1.46F,3.1F},2.6F,1.34F,1.9F,light);
    m.box({.44F,2.75F,3.25F},{.62F,3.72F,3.43F},steel);
    m.box({-.26F,3.42F,3.26F},{1.30F,3.54F,3.38F},light);
    m.box({.45F,3.72F,3.27F},{.61F,3.80F,3.40F},amber,amber);
    nozzle(m,{0,-.12F,5.81F},.70F,2.2F);
    // Outboard defense blisters are chunky enough to survive distant views.
    for(float sign : {-1.0F,1.0F}) for(float z : {-3.8F,3.3F}) {
        m.box({sign*4.35F-.40F,.13F,z-.58F},{sign*4.35F+.40F,.76F,z+.58F},hull);
        m.box({sign*4.35F-.18F,.46F,z-1.32F},{sign*4.35F+.18F,.67F,z-.40F},steel);
    }
    return std::move(m).document("BASTION / CV-18 expedition carrier",
        "split armored bow, recessed hangar deck, approach lights, command island, sensor mast, triple drives, defense blisters");
}

vmesh::Document frigate()
{
    Model m;
    constexpr std::array sections{
        Section{-7.0F,.07F,.11F},Section{-5.5F,.30F,.29F},
        Section{-3.9F,.49F,.47F},Section{-2.3F,.64F,.62F},
        Section{-.7F,.79F,.73F},Section{.9F,.89F,.76F},
        Section{2.4F,.91F,.71F},Section{3.9F,.70F,.60F},Section{5.0F,.52F,.48F}};
    armored_hull(m,sections,{},blue,light);
    bridge(m,{0,.70F,-.5F},1.15F,.70F,1.5F,light);
    // Tall swept dorsal sensor sail: this silhouette differs from both flat ships.
    m.prism(std::array{Vec3{-.105F,.65F,.6F},Vec3{-.105F,.40F,4.25F},
                       Vec3{-.105F,2.70F,3.45F},Vec3{-.105F,2.35F,2.65F}},
            {.21F,0,0},blue,steel);
    m.prism(std::array{Vec3{.112F,1.30F,2.02F},Vec3{.112F,.86F,3.85F},
                       Vec3{.112F,1.30F,3.70F},Vec3{.112F,1.70F,2.57F}},
            {.015F,0,0},teal,teal);
    m.box({-.13F,2.48F,3.17F},{.13F,2.70F,3.40F},amber,amber);
    for(float sign : {-1.0F,1.0F}) {
        // Strong negative space between narrow spine and outrigger drive pods.
        const auto p=[sign](float x,float y,float z){return Vec3{sign*x,y,z};};
        m.prism(std::array{p(.60F,-.35F,-.55F),p(2.55F,-.35F,1.50F),
                           p(2.52F,-.35F,2.62F),p(.65F,-.35F,1.10F)},
                {0,.33F,0},light,hull);
        m.prism(std::array{p(.74F,-.005F,-.35F),p(2.42F,-.005F,1.45F),
                           p(2.34F,-.005F,1.66F),p(.72F,-.005F,.02F)},
                {0,.018F,0},teal,teal);
        constexpr std::array pod{Section{.1F,.12F,.12F},Section{1.0F,.43F,.42F},
                                Section{2.4F,.46F,.43F},Section{3.8F,.36F,.34F}};
        armored_hull(m,pod,{sign*2.60F,-.08F,0},light,blue);
        nozzle(m,{sign*2.60F,-.08F,3.82F},.35F,1.75F);
        m.box({sign*2.6F-.045F,.38F,1.1F},{sign*2.6F+.045F,.41F,2.7F},cyan,cyan);
        // Paired long ventral instrument lances follow the narrow nose.
        m.box({sign*.37F-.045F,-.45F,-4.9F},{sign*.37F+.045F,-.34F,-1.7F},steel);
    }
    nozzle(m,{0,0,5.02F},.51F,1.90F);
    top_vents(m,0,.80F,1.10F,.73F,7);
    return std::move(m).document("LANCET / FF-09 reconnaissance frigate",
        "needle bow, separated outrigger drives, swept sensor sail, forward bridge, longitudinal lances, segmented blue armor");
}

vmesh::Document escort()
{
    Model m;
    constexpr std::array sections{
        Section{-3.8F,.38F,.16F},Section{-2.65F,.87F,.39F},
        Section{-1.40F,1.14F,.58F},Section{.0F,1.26F,.68F},
        Section{1.4F,1.09F,.62F},Section{2.6F,.90F,.44F}};
    armored_hull(m,sections,{},light,red);
    bridge(m,{0,.54F,-.8F},1.12F,.56F,1.45F,red);
    for(float sign : {-1.0F,1.0F}) {
        const auto p=[sign](float x,float y,float z){return Vec3{sign*x,y,z};};
        // Wide forward-swept armored crescent with stepped trailing engine shelf.
        m.prism(std::array{p(.85F,-.22F,-1.7F),p(4.7F,-.22F,-2.5F),
                           p(5.25F,-.22F,-1.35F),p(4.15F,-.22F,2.10F),
                           p(1.0F,-.22F,1.65F)},
                {0,.37F,0},hull,steel);
        m.prism(std::array{p(1.25F,.16F,-1.38F),p(4.57F,.16F,-2.26F),
                           p(4.92F,.16F,-1.31F),p(3.0F,.16F,-.13F),
                           p(1.26F,.16F,-.07F)},
                {0,.11F,0},red,hull);
        m.prism(std::array{p(1.32F,.16F,.11F),p(3.04F,.16F,.10F),
                           p(4.72F,.16F,-.99F),p(4.01F,.16F,1.68F),
                           p(1.33F,.16F,1.43F)},
                {0,.095F,0},light,hull);
        // Three contrasting root stripes, not fine noise that vanishes at distance.
        for(unsigned i=0;i<3;++i) {
            const float z=-1.15F+static_cast<float>(i)*.28F;
            m.prism(std::array{p(1.35F,.28F,z),p(2.14F,.28F,z-.20F),
                               p(2.14F,.28F,z-.07F),p(1.35F,.28F,z+.13F)},
                    {0,.012F,0},light,light);
        }
        // Blunt tips with short vertically tilted shield fins.
        m.prism(std::array{p(4.72F,.0F,-2.42F),p(5.22F,.0F,-1.31F),
                           p(5.38F,.73F,-1.37F),p(4.98F,.73F,-2.14F)},
                {sign*.07F,0,0},red,light);
        m.box({sign*4.95F-.07F,.63F,-1.83F},{sign*4.95F+.07F,.77F,-1.61F},amber,amber);
        for(float x : {1.72F,3.20F}) {
            constexpr std::array pod{Section{.3F,.30F,.24F},Section{1.25F,.45F,.42F},
                                    Section{2.70F,.39F,.34F}};
            armored_hull(m,pod,{sign*x,-.18F,0},steel,red);
            nozzle(m,{sign*x,-.18F,2.72F},.34F,1.4F);
        }
        top_vents(m,sign*2.15F,.30F,.39F,.63F,4);
        // Forward aperture makes the mandibles read as equipment, not paper wings.
        m.box({sign*4.0F-.23F,-.17F,-2.44F},{sign*4.0F+.23F,.14F,-2.20F},dark);
        m.box({sign*4.0F-.17F,-.11F,-2.451F},{sign*4.0F+.17F,.055F,-2.44F},glass,glass);
    }
    m.box({-.34F,.71F,.26F},{.34F,.82F,1.18F},red);
    return std::move(m).document("WARDEN / EC-04 heavy escort",
        "broad forward-swept crescent, layered red armor, quadruple drives, raised command pod, shield fins, recessed forward apertures");
}

void write(const std::filesystem::path& path,const vmesh::Document& model)
{
    if(auto result=vmesh::write_vmesh(path,model);!result)
        throw std::runtime_error(result.error().message);
    const auto& points=std::get<std::vector<float>>(model.vertex_fields[0].values);
    Vec3 minimum{100,100,100},maximum{-100,-100,-100};
    for(std::size_t i=0;i<points.size();i+=3) for(std::size_t c=0;c<3;++c) {
        minimum[c]=std::min(minimum[c],points[i+c]);
        maximum[c]=std::max(maximum[c],points[i+c]);
    }
    std::cout<<path.filename()<<": "<<model.vertex_count<<" vertices, "
             <<model.faces.size()<<" triangles; bounds ["<<minimum.x<<", "
             <<minimum.y<<", "<<minimum.z<<"] .. ["<<maximum.x<<", "
             <<maximum.y<<", "<<maximum.z<<"]\n";
}
} // namespace

int main(int argc,char** argv)
{
    if(argc!=2) { std::cerr<<"Usage: vng_make_fleet OUTPUT_DIRECTORY\n"; return 2; }
    try {
        const std::filesystem::path directory(argv[1]);
        if(!std::filesystem::is_directory(directory))
            throw std::runtime_error("output directory must already exist");
        write(directory/"fleet_carrier.vmesh",carrier());
        write(directory/"fleet_frigate.vmesh",frigate());
        write(directory/"fleet_escort.vmesh",escort());
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n'; return 1;
    }
}
