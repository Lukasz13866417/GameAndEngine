#include <vng/editor/mesh.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>

namespace vng::editor {
namespace {
auto invalid(std::string message) {
    content::Diagnostic error;
    error.code = content::ErrorCode::invalid_document;
    error.message = std::move(message);
    return std::unexpected(std::move(error));
}
using Pair = std::pair<u32, u32>;
Pair canonical(u32 a, u32 b) { return {std::min(a,b), std::max(a,b)}; }
struct Point { double x, y; };
double cross(Point a, Point b, Point c) { return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x); }
}
std::vector<gfx::Edge> EditableMesh::edges() const {
    std::set<Pair> unique;
    for (const auto& face : document_.faces)
        for (unsigned i=0; i<3; ++i) unique.insert(canonical(face[i], face[(i+1)%3]));
    if (document_.edges) for (const auto& edge : *document_.edges)
        unique.insert(canonical(edge[0],edge[1]));
    std::vector<gfx::Edge> result;
    for (auto [a,b] : unique) result.emplace_back(a,b);
    return result;
}
content::Result<void> EditableMesh::fill(std::span<const u32> ids) {
    if (ids.size()<2) return invalid("Select at least two vertices");
    if (ids.size()>4096) return invalid("A face boundary is limited to 4096 vertices");
    std::set<u32> unique;
    for (auto id:ids) if (id>=size() || !unique.insert(id).second)
        return invalid("Face/edge selection contains a missing or repeated vertex");
    if (ids.size()==2) {
        const auto all=edges();
        const auto key=canonical(ids[0],ids[1]);
        if (std::ranges::any_of(all,[&](auto e){return canonical(e[0],e[1])==key;}))
            return invalid("That edge already exists");
        if (position(ids[0])==position(ids[1])) return invalid("Cannot create a zero-length edge");
        auto candidate=document_;
        if (!candidate.edges) candidate.edges.emplace();
        candidate.edges->emplace_back(ids[0],ids[1]);
        auto checked=create(std::move(candidate));
        if (!checked) return std::unexpected(checked.error());
        *this=std::move(*checked); return {};
    }
    // Newell normal chooses a stable projection for any planar orientation.
    std::array<double,3> normal{};
    for (std::size_t i=0; i<ids.size(); ++i) {
        const auto a=position(ids[i]), b=position(ids[(i+1)%ids.size()]);
        normal[0]+=(double(a.y)-b.y)*(double(a.z)+b.z);
        normal[1]+=(double(a.z)-b.z)*(double(a.x)+b.x);
        normal[2]+=(double(a.x)-b.x)*(double(a.y)+b.y);
    }
    const auto axis=static_cast<unsigned>(std::max_element(normal.begin(),normal.end(),
        [](double a,double b){return std::abs(a)<std::abs(b);})-normal.begin());
    if (std::abs(normal[axis])<1e-14) return invalid("Face boundary has no area; check vertex order");
    std::vector<Point> points;
    for (auto id:ids) {
        auto p=position(id);
        points.push_back(axis==0?Point{p.y,p.z}:axis==1?Point{p.z,p.x}:Point{p.x,p.y});
    }
    double scale{};
    for(auto p:points) scale=std::max(scale,std::max(std::abs(p.x-points[0].x),std::abs(p.y-points[0].y)));
    const double eps=std::max(1e-20,scale*scale*1e-10), sign=normal[axis]>0?1:-1;
    const auto on=[&](Point a,Point b,Point p){return std::abs(cross(a,b,p))<=eps &&
        p.x>=std::min(a.x,b.x) && p.x<=std::max(a.x,b.x) && p.y>=std::min(a.y,b.y) && p.y<=std::max(a.y,b.y);};
    for(std::size_t i=0;i<ids.size();++i) for(std::size_t j=i+1;j<ids.size();++j) {
        const auto ni=(i+1)%ids.size(), nj=(j+1)%ids.size();
        if(ni==j || nj==i) continue;
        const auto a=points[i],b=points[ni],c=points[j],d=points[nj];
        if ((cross(a,b,c)*cross(a,b,d)<0 && cross(c,d,a)*cross(c,d,b)<0) ||
            on(a,b,c)||on(a,b,d)||on(c,d,a)||on(c,d,b))
            return invalid("Face boundary intersects itself; select vertices around its perimeter");
    }
    std::vector<std::size_t> remaining;
    for(std::size_t i=0;i<ids.size();++i) remaining.push_back(i);
    auto candidate=document_;
    while(remaining.size()>3) {
        bool clipped{};
        for(std::size_t i=0;i<remaining.size();++i) {
            const auto a=remaining[(i+remaining.size()-1)%remaining.size()], b=remaining[i],c=remaining[(i+1)%remaining.size()];
            if(sign*cross(points[a],points[b],points[c])<=eps) continue;
            const bool occupied=std::ranges::any_of(remaining,[&](auto p){return p!=a && p!=b && p!=c &&
                sign*cross(points[a],points[b],points[p])>=-eps && sign*cross(points[b],points[c],points[p])>=-eps &&
                sign*cross(points[c],points[a],points[p])>=-eps;});
            if(occupied) continue;
            candidate.faces.emplace_back(ids[a],ids[b],ids[c]);
            remaining.erase(remaining.begin()+static_cast<std::ptrdiff_t>(i)); clipped=true; break;
        }
        if(!clipped) return invalid("Cannot triangulate degenerate face boundary");
    }
    if(sign*cross(points[remaining[0]],points[remaining[1]],points[remaining[2]])<=eps)
        return invalid("Face boundary contains a degenerate triangle");
    candidate.faces.emplace_back(ids[remaining[0]],ids[remaining[1]],ids[remaining[2]]);
    std::set<std::array<u32,3>> faces;
    for(const auto& face:candidate.faces) {
        std::array<u32,3> key{face[0],face[1],face[2]}; std::ranges::sort(key);
        if(!faces.insert(key).second) return invalid("That face already exists");
    }
    auto checked=create(std::move(candidate)); if(!checked) return std::unexpected(checked.error());
    *this=std::move(*checked); return {};
}
content::Result<std::vector<u32>> EditableMesh::subdivide(std::span<const gfx::Edge> selected) {
    if(selected.empty()) return invalid("Select edges, faces, or both endpoints of an edge to subdivide");
    std::set<Pair> available;
    for(auto edge:edges()) available.insert(canonical(edge[0],edge[1]));
    std::map<Pair,u32> middle;
    for(auto edge:selected) {
        auto key=canonical(edge[0],edge[1]);
        if(!available.contains(key)) return invalid("Subdivide selection refers to a missing edge");
        middle.try_emplace(key,0);
    }
    if(size()+middle.size()>65536) return invalid("Subdivision exceeds the editor vertex limit");
    auto candidate=document_;
    std::vector<u32> added;
    for(auto& [edge,id]:middle) {
        id=static_cast<u32>(candidate.vertex_count++); added.push_back(id);
        for(auto& field:candidate.vertex_fields) std::visit([&](auto& values) {
            using T=typename std::decay_t<decltype(values)>::value_type;
            for(unsigned c=0;c<field.type.components;++c) {
                const auto a=values[std::size_t(edge.first)*field.type.components+c];
                const auto b=values[std::size_t(edge.second)*field.type.components+c];
                if constexpr(std::same_as<T,f32>) values.push_back(static_cast<f32>((double(a)+b)*.5));
                else values.push_back(a);
            }
        },field.values);
    }
    candidate.faces.clear();
    const auto midpoint=[&](u32 a,u32 b){auto it=middle.find(canonical(a,b));return it==middle.end()?std::optional<u32>{}:it->second;};
    for(const auto& face:document_.faces) {
        const auto a=face[0],b=face[1],c=face[2];
        const auto ab=midpoint(a,b),bc=midpoint(b,c),ca=midpoint(c,a);
        const auto count=unsigned(bool(ab))+unsigned(bool(bc))+unsigned(bool(ca));
        if(count==0) candidate.faces.push_back(face);
        else if(count==3) {
            candidate.faces.emplace_back(a,*ab,*ca); candidate.faces.emplace_back(*ab,b,*bc);
            candidate.faces.emplace_back(*ca,*bc,c); candidate.faces.emplace_back(*ab,*bc,*ca);
        } else {
            // Rotate the triangle until split edges are AB (one) or AB+BC (two).
            for(unsigned i=0;i<3;++i) {
                const auto x=face[i],y=face[(i+1)%3],z=face[(i+2)%3];
                const auto xy=midpoint(x,y),yz=midpoint(y,z),zx=midpoint(z,x);
                if(!xy || (count==2 && !yz) || zx) continue;
                if(count==1) { candidate.faces.emplace_back(x,*xy,z); candidate.faces.emplace_back(*xy,y,z); }
                else { candidate.faces.emplace_back(y,*yz,*xy); candidate.faces.emplace_back(x,*xy,z); candidate.faces.emplace_back(*xy,*yz,z); }
                break;
            }
        }
    }
    if(candidate.edges) {
        candidate.edges->clear();
        for(auto edge:*document_.edges) {
            if(auto mid=midpoint(edge[0],edge[1])) {
                candidate.edges->emplace_back(edge[0],*mid); candidate.edges->emplace_back(*mid,edge[1]);
            } else candidate.edges->push_back(edge);
        }
    }
    auto checked=create(std::move(candidate)); if(!checked) return std::unexpected(checked.error());
    *this=std::move(*checked); return added;
}
content::Result<std::vector<u32>> EditableMesh::align_to_line(std::span<const u32> ids) {
    if(ids.size()<3) return invalid("Select at least three vertices; the first two anchor the line");
    std::set<u32> unique;
    for(auto id:ids) if(id>=size() || !unique.insert(id).second) return invalid("Invalid or repeated vertex in line selection");
    const auto a=position(ids[0]), b=position(ids[1]);
    const double dx=double(b.x)-a.x,dy=double(b.y)-a.y,dz=double(b.z)-a.z;
    const auto length=dx*dx+dy*dy+dz*dz;
    if(length<1e-20) return invalid("The first two vertices must be at different positions");
    std::vector<std::pair<u32,Vec3>> values;
    for(auto id:ids.subspan(2)) {
        const auto p=position(id);
        const double t=((double(p.x)-a.x)*dx+(double(p.y)-a.y)*dy+(double(p.z)-a.z)*dz)/length;
        Vec3 q{static_cast<f32>(a.x+t*dx),static_cast<f32>(a.y+t*dy),static_cast<f32>(a.z+t*dz)};
        for(auto v:{q.x,q.y,q.z}) if(!std::isfinite(v)||std::abs(v)>1000000) return invalid("Line projection exceeds editing range");
        if(q!=p) values.emplace_back(id,q);
    }
    std::vector<u32> changed;
    for(auto [id,p]:values) { (void)set_position(id,p); changed.push_back(id); }
    return changed;
}
} // namespace vng::editor
