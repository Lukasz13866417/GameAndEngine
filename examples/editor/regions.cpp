#include "regions.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <iomanip>

namespace editor_example {
namespace {
using namespace vng;
using D = std::array<double,3>;
D sub(Vec3 a,Vec3 b) { return {double(a.x)-b.x,double(a.y)-b.y,double(a.z)-b.z}; }
D cross(D a,D b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
double dot(D a,D b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
auto invalid(std::string message) {
    content::Diagnostic error;
    error.message = std::move(message);
    return std::unexpected(std::move(error));
}
// Small, bounded authoring cages: enumerate supporting planes, then take a 2D
// hull on each plane. Coplanar diagonals/interior points are not outline edges.
std::vector<RegionFace> hull_faces(const std::vector<Vec3>& p) {
    if(p.size()<4 || p.size()>32) return {}; // old format's bounded hull algorithm
    double extent{};
    for(auto a:p) for(auto b:p) extent=std::max(extent,std::sqrt(dot(sub(a,b),sub(a,b))));
    const auto epsilon=std::max(1e-7,extent*1e-6);
    bool volume=false;
    std::set<std::vector<u32>> planes;
    std::vector<RegionFace> faces;
    for(u32 i=0;i<p.size();++i) for(u32 j=i+1;j<p.size();++j) for(u32 k=j+1;k<p.size();++k) {
        auto n=cross(sub(p[j],p[i]),sub(p[k],p[i]));
        const auto length=std::sqrt(dot(n,n)); if(length<=epsilon*epsilon) continue;
        for(auto& v:n) v/=length;
        bool positive=false,negative=false; std::vector<u32> face;
        for(u32 v=0;v<p.size();++v) {
            const auto d=dot(n,sub(p[v],p[i]));
            positive|=d>epsilon; negative|=d<-epsilon;
            if(std::abs(d)<=epsilon) face.push_back(v);
        }
        volume|=positive||negative;
        if((positive&&negative)||(!positive&&!negative)||!planes.insert(face).second) continue;
        unsigned drop=0; for(unsigned a=1;a<3;++a) if(std::abs(n[a])>std::abs(n[drop]))drop=a;
        const auto x=(drop+1)%3,y=(drop+2)%3;
        std::ranges::sort(face,[&](u32 a,u32 b) { return p[a][x]!=p[b][x] ? p[a][x]<p[b][x] : p[a][y]<p[b][y]; });
        face.erase(std::unique(face.begin(),face.end(),[&](u32 a,u32 b){return p[a][x]==p[b][x]&&p[a][y]==p[b][y];}),face.end());
        const auto turn=[&](u32 a,u32 b,u32 c) {return (double(p[b][x])-p[a][x])*(double(p[c][y])-p[a][y])-
            (double(p[b][y])-p[a][y])*(double(p[c][x])-p[a][x]);};
        std::vector<u32> hull;
        for(auto v:face) { while(hull.size()>1&&turn(hull[hull.size()-2],hull.back(),v)<=epsilon*epsilon)hull.pop_back(); hull.push_back(v); }
        const auto lower=hull.size();
        for(auto it=face.rbegin()+1;it!=face.rend();++it) {
            while(hull.size()>lower&&turn(hull[hull.size()-2],hull.back(),*it)<=epsilon*epsilon)hull.pop_back();
            hull.push_back(*it);
        }
        if(hull.size()>1)hull.pop_back();
        if(hull.size()>2) faces.push_back(std::move(hull));
    }
    if(!volume)return {};
    return faces;
}
void quote(std::ostream& out,std::string_view text) {
    out<<'"'; for(char c:text) switch(c) {
    case '\\':out<<"\\\\";break; case '"':out<<"\\\"";break;
    case '\n':out<<"\\n";break; case '\r':out<<"\\r";break; case '\t':out<<"\\t";break;
    default:out<<c;
    } out<<'"';
}
}
vng::Vec3 RegionGeometry::center() const {
    vng::Vec3 c{}; if(points.empty())return c;
    for(auto p:points) for(unsigned a=0;a<3;++a)c[a]+=p[a]/static_cast<float>(points.size());
    return c;
}
vng::editor::SceneCage Region::scene_cage() const {
    vng::editor::SceneCage cage;
    cage.object=id; cage.label=name; cage.primary_point=region_center;
    for(vng::u32 i=0;i<points.size();++i)cage.points.push_back({i,points[i],"Vertex "+std::to_string(i+1)});
    cage.edges=edges();
    cage.faces=faces;
    cage.points.push_back({region_center,center(),"Move region"});
    return cage;
}
std::vector<RegionEdge> RegionGeometry::edges() const {
    std::set<RegionEdge> result;
    const auto add=[&](u32 a,u32 b) { if(a>b)std::swap(a,b);result.insert({a,b}); };
    for(auto e:loose_edges)add(e[0],e[1]);
    for(const auto& face:faces)for(std::size_t i=0;i<face.size();++i)add(face[i],face[(i+1)%face.size()]);
    return {result.begin(),result.end()};
}
vng::content::Result<void> migrate_region_hull(Region& r) {
    r.faces=hull_faces(r.points);
    if(r.faces.empty())return invalid("Legacy region does not enclose a convex volume");
    return {};
}
Region make_region(RegionShape shape,vng::Vec3 center,vng::f32 radius) {
    Region r;
    switch(shape) {
    case RegionShape::box: for(unsigned i=0;i<8;++i)r.points.push_back({i&1?1.F:-1.F,i&2?1.F:-1.F,i&4?1.F:-1.F});break;
    case RegionShape::tetrahedron:r.points={{1,1,1},{-1,-1,1},{-1,1,-1},{1,-1,-1}};break;
    case RegionShape::octahedron:r.points={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};break;
    case RegionShape::prism:r.points={{-1,-1,-1},{1,-1,-1},{0,1,-1},{-1,-1,1},{1,-1,1},{0,1,1}};break;
    }
    r.faces=hull_faces(r.points);
    for(auto& p:r.points)for(unsigned a=0;a<3;++a)p[a]=center[a]+p[a]*radius;
    return r;
}
vng::content::Result<void> validate(const Region& r) {
    if(!r.id||r.name.empty()||r.name.size()>256||r.note.size()>4096||r.points.empty()||r.points.size()>max_region_points)
        return invalid("Region needs an ID, name (1..256 bytes), note (up to 4096 bytes) and 1..1024 points");
    for(const auto& text:{r.name,r.note})for(unsigned char c:text)
        if(c<32&&c!='\n'&&c!='\t'&&c!='\r')return invalid("Region text contains invalid control characters");
    for(auto p:r.points)for(unsigned a=0;a<3;++a)
        if(!std::isfinite(p[a])||std::abs(p[a])>scene_coordinate_limit)return invalid("Region coordinates must be finite and within scene limits");
    if(r.faces.size()>max_region_faces||r.loose_edges.size()>max_region_corners)
        return invalid("Region topology limit exceeded");
    std::size_t corners{};
    for(const auto& face:r.faces) {
        corners+=face.size();
        if(face.size()<3||corners>max_region_corners)return invalid("Invalid region face size");
        std::set<u32> seen;
        for(auto v:face)if(v>=r.points.size()||!seen.insert(v).second)return invalid("Invalid or repeated region face vertex");
    }
    for(auto e:r.loose_edges)if(e[0]>=r.points.size()||e[1]>=r.points.size()||e[0]==e[1])
        return invalid("Invalid region edge");
    return {};
}
vng::content::Result<void> validate(const Regions& regions) {
    if(!regions.next_id||regions.items.size()>max_regions)return invalid("Region count/identity limit exceeded");
    std::set<vng::u32> ids;
    for(const auto& r:regions.items) {
        if(r.id>=regions.next_id||!ids.insert(r.id).second)return invalid("Duplicate or invalid region identity");
        if(auto valid=validate(r);!valid)return valid;
    } return {};
}
const Region* find_region(const Regions& r,vng::u32 id) {
    const auto found=std::ranges::find(r.items,id,&Region::id);return found==r.items.end()?nullptr:&*found;
}
void write_regions(std::ostream& out,const Regions& r) {
    out<<"{ next_id = "<<r.next_id<<"; items = [\n";
    for(const auto& item:r.items) {
        out<<"{ id = "<<item.id<<"; name = ";quote(out,item.name);out<<"; note = ";quote(out,item.note);out<<"; show_walls = "<<(item.show_walls ? "true" : "false")<<"; points = [";
        for(auto p:item.points)out<<"["<<p.x<<", "<<p.y<<", "<<p.z<<"],";
        out<<"]; faces = [";
        for(const auto& face:item.faces) {out<<"[";for(auto v:face)out<<v<<",";out<<"],";}
        out<<"]; edges = [";
        for(auto e:item.loose_edges)out<<"["<<e[0]<<","<<e[1]<<"],";
        out<<"]; },\n";
    }out<<"]; }";
}
Regions read_regions(vng::content::Reader reader) {
    Regions result;result.next_id=reader.get<vng::u32>("next_id");
    for(auto item:reader.child("items").elements()) {
        if(result.items.size()>=max_regions)item.fail("Too many regions");
        Region r;
        r.id=item.get<vng::u32>("id");r.name=item.get<std::string>("name");r.note=item.get<std::string>("note");
        r.show_walls=item.get_or<bool>("show_walls",false);
        static_cast<RegionGeometry&>(r)=read_region_geometry(item);
        result.items.push_back(std::move(r));
    }
    if(auto valid=validate(result);!valid)reader.fail(valid.error().message);
    return result;
}

void write_region_geometry(std::ostream& out,const RegionGeometry& item) {
    out<<"{ points = [";
    for(auto p:item.points)out<<"["<<p.x<<", "<<p.y<<", "<<p.z<<"],";
    out<<"]; faces = [";
    for(const auto& face:item.faces) {out<<"[";for(auto v:face)out<<v<<",";out<<"],";}
    out<<"]; edges = [";
    for(auto e:item.loose_edges)out<<"["<<e[0]<<","<<e[1]<<"],";
    out<<"]; }";
}
RegionGeometry read_region_geometry(vng::content::Reader reader) {
    Region r;r.id=1;
        for(auto point:reader.child("points").elements()) {
            if(r.points.size()>=max_region_points)point.fail("Too many region points");
            r.points.push_back(point.as<vng::Vec3>());
        }
        const auto has=[&](std::string_view name) {return std::ranges::any_of(reader.members(),[&](const auto& m){return m.name==name;});};
        if(has("faces")) {
            std::size_t corners{};
            for(auto face:reader.child("faces").elements()) {
                if(r.faces.size()>=max_region_faces)face.fail("Too many region faces");
                RegionFace ids;
                for(auto vertex:face.elements()) {
                    if(++corners>max_region_corners)vertex.fail("Too many region face corners");
                    ids.push_back(vertex.as<u32>());
                }
                r.faces.push_back(std::move(ids));
            }
            if(has("edges"))for(auto edge:reader.child("edges").elements()) {
                if(r.loose_edges.size()>=max_region_corners)edge.fail("Too many region edges");
                auto values=edge.elements();
                if(values.size()!=2)edge.fail("Region edge needs two vertices");
                RegionEdge ids{};std::size_t index{};
                for(auto value:values)ids[index++]=value.as<u32>();
                r.loose_edges.push_back(ids);
            }
        } else if(auto migrated=migrate_region_hull(r);!migrated)reader.fail(migrated.error().message);

    if(auto valid=validate(r);!valid)reader.fail(valid.error().message);
    return static_cast<RegionGeometry&&>(r);
}

} // namespace editor_example
