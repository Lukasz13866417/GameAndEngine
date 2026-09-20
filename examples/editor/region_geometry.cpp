#include "regions.hpp"
#include <algorithm>
#include <map>
#include <set>

namespace editor_example {
namespace {
using namespace vng;
using Kind = editor::CageElement;
RegionEdge edge(u32 a, u32 b) { return a < b ? RegionEdge{a,b} : RegionEdge{b,a}; }
auto invalid(std::string message) {
    content::Diagnostic diagnostic;
    diagnostic.message = std::move(message);
    return std::unexpected(std::move(diagnostic));
}
Vec3 mean(const Region& region, std::span<const u32> vertices) {
    Vec3 result{};
    for(auto v : vertices) for(unsigned a=0; a<3; ++a)
        result[a] += region.points[v][a] / static_cast<float>(vertices.size());
    return result;
}
}

std::vector<u32> region_vertices(const Region& region, Kind kind, std::span<const u32> elements) {
    std::vector<u32> result;
    const auto add = [&](u32 id) {
        if(id < region.points.size() && std::ranges::find(result,id)==result.end()) result.push_back(id);
    };
    const auto edges = kind == Kind::edge ? region.edges() : std::vector<RegionEdge>{};
    for(auto id : elements) {
        if(kind==Kind::vertex) add(id);
        else if(kind==Kind::edge && id<edges.size()) for(auto v:edges[id]) add(v);
        else if(kind==Kind::face && id<region.faces.size()) for(auto v:region.faces[id]) add(v);
    }
    return result;
}

content::Result<std::vector<u32>> edit_region_geometry(
    Region& region, RegionAction action, Kind kind, std::span<const u32> elements) {
    if(auto valid=validate(region); !valid) return std::unexpected(valid.error());
    const auto all_edges=region.edges();
    const auto count=kind==Kind::vertex ? region.points.size() : kind==Kind::edge ? all_edges.size() : region.faces.size();
    for(auto id:elements) if(id>=count) return invalid("Region selection is stale");
    const auto vertices=region_vertices(region,kind,elements);
    const std::set<u32> selected(elements.begin(),elements.end());
    Region next=region;
    std::vector<u32> result=vertices;
    const auto add_point=[&](Vec3 point) {const auto id=static_cast<u32>(next.points.size());next.points.push_back(point);return id;};

    if(action==RegionAction::add_vertex) {
        result={add_point(vertices.empty() ? region.center() : mean(region,vertices))};
    } else if(action==RegionAction::fill) {
        if(kind!=Kind::vertex || vertices.size()<2) return invalid("Select two vertices for an edge, or an ordered boundary for a face");
        if(vertices.size()==2) {
            const auto e=edge(vertices[0],vertices[1]);
            if(std::ranges::find(all_edges,e)!=all_edges.end()) return invalid("That region edge already exists");
            next.loose_edges.push_back(e);
        } else {
            // An annotation boundary may be concave/nonplanar. It is not a collision solid.
            next.faces.push_back(vertices);
        }
    } else if(action==RegionAction::align) {
        if(vertices.size()<3) return invalid("Select at least three vertices; the first two are fixed anchors");
        const auto a=region.points[vertices[0]], b=region.points[vertices[1]];
        std::array<double,3> axis{};
        double length{};
        for(unsigned c=0;c<3;++c) {axis[c]=double(b[c])-a[c];length+=axis[c]*axis[c];}
        if(length<1e-16) return invalid("Line anchors must have different positions");
        for(std::size_t i=2;i<vertices.size();++i) {
            double t{};
            for(unsigned c=0;c<3;++c) t+=(double(region.points[vertices[i]][c])-a[c])*axis[c];
            for(unsigned c=0;c<3;++c) next.points[vertices[i]][c]=static_cast<float>(a[c]+t/length*axis[c]);
        }
    } else if(action==RegionAction::subdivide) {
        std::set<RegionEdge> split;
        if(kind==Kind::edge) for(auto id:elements) split.insert(all_edges[id]);
        else if(kind==Kind::face) for(auto id:elements) {
            const auto& face=region.faces[id];
            for(std::size_t i=0;i<face.size();++i) split.insert(edge(face[i],face[(i+1)%face.size()]));
        } else for(auto e:all_edges) if(selected.contains(e[0])&&selected.contains(e[1])) split.insert(e);
        if(split.empty()) return invalid("Select edges, faces, or both endpoints of an edge to subdivide");
        if(next.points.size()+split.size()+(kind==Kind::face?selected.size():0)>max_region_points)
            return invalid("Subdivision exceeds region vertex capacity");
        std::map<RegionEdge,u32> midpoints;
        result.clear();
        for(auto e:split) {
            const auto midpoint=add_point(mean(region,e));
            midpoints.emplace(e,midpoint);result.push_back(midpoint);
        }
        next.faces.clear();
        for(u32 index=0;index<region.faces.size();++index) {
            const auto& face=region.faces[index];
            if(kind==Kind::face && selected.contains(index)) {
                const auto center=add_point(mean(region,face));result.push_back(center);
                for(std::size_t i=0;i<face.size();++i)
                    next.faces.push_back({face[i],midpoints.at(edge(face[i],face[(i+1)%face.size()])),center,
                        midpoints.at(edge(face[(i+face.size()-1)%face.size()],face[i]))});
            } else {
                RegionFace boundary;
                for(std::size_t i=0;i<face.size();++i) {
                    boundary.push_back(face[i]);
                    if(auto found=midpoints.find(edge(face[i],face[(i+1)%face.size()]));found!=midpoints.end())
                        boundary.push_back(found->second);
                }
                next.faces.push_back(std::move(boundary));
            }
        }
        next.loose_edges.clear();
        for(auto e:region.loose_edges) {
            if(auto found=midpoints.find(edge(e[0],e[1]));found!=midpoints.end()) {
                next.loose_edges.push_back(edge(e[0],found->second));
                next.loose_edges.push_back(edge(found->second,e[1]));
            } else next.loose_edges.push_back(e);
        }
    } else if(action==RegionAction::erase) {
        if(elements.empty()) return invalid("Select region components to delete");
        if(kind==Kind::face) {
            next.faces.clear();
            for(u32 i=0;i<region.faces.size();++i) if(!selected.contains(i)) next.faces.push_back(region.faces[i]);
        } else if(kind==Kind::edge) {
            std::set<RegionEdge> removed;
            for(auto i:elements) removed.insert(all_edges[i]);
            std::erase_if(next.faces,[&](const auto& f) {
                for(std::size_t i=0;i<f.size();++i) if(removed.contains(edge(f[i],f[(i+1)%f.size()]))) return true;
                return false;
            });
            std::erase_if(next.loose_edges,[&](auto e){return removed.contains(edge(e[0],e[1]));});
        } else {
            if(selected.size()==region.points.size()) return invalid("Use Delete region to remove the entire annotation");
            std::vector<u32> remap(region.points.size());next.points.clear();
            for(u32 i=0;i<region.points.size();++i) if(!selected.contains(i)) {
                remap[i]=static_cast<u32>(next.points.size());next.points.push_back(region.points[i]);
            }
            std::erase_if(next.faces,[&](const auto& f){return std::ranges::any_of(f,[&](auto v){return selected.contains(v);});});
            std::erase_if(next.loose_edges,[&](auto e){return selected.contains(e[0])||selected.contains(e[1]);});
            for(auto& f:next.faces) for(auto& v:f) v=remap[v];
            for(auto& e:next.loose_edges) for(auto& v:e) v=remap[v];
        }
        result.clear();
    }
    if(auto valid=validate(next); !valid) return std::unexpected(valid.error());
    region=std::move(next);
    return result;
}
} // namespace editor_example
