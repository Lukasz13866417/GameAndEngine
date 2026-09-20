#include "asteroid_assets.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numbers>
#include <vector>

namespace example::asteroids {
namespace {
using namespace vng;
Vec3 add(Vec3 a, Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec3 mul(Vec3 a, f32 s) { return {a.x*s,a.y*s,a.z*s}; }
f32 dot(Vec3 a, Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
Vec3 unit(Vec3 p) { return mul(p,1.F/std::sqrt(dot(p,p))); }
f32 noise(Vec3 p) {
    return std::sin(p.x*1.73F+std::cos(p.z*2.13F)) *
           std::sin(p.y*2.31F+std::cos(p.x*1.61F));
}
struct Crater { Vec3 direction; f32 width, depth; };
}

content::vmesh::Document rock_mesh(u32 variant) {
    namespace vm = content::vmesh;
    variant %= 3;
    const auto seed = static_cast<f32>(variant)*5.17F;
    const f32 golden = (1.F+std::sqrt(5.F))*.5F;
    std::vector<Vec3> points{{-1,golden,0},{1,golden,0},{-1,-golden,0},{1,-golden,0},
        {0,-1,golden},{0,1,golden},{0,-1,-golden},{0,1,-golden},
        {golden,0,-1},{golden,0,1},{-golden,0,-1},{-golden,0,1}};
    for (auto& p : points) p=unit(p);
    std::vector<std::array<u32,3>> triangles{{0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
        {1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
        {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
        {4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1}};
    // Shared edge midpoints keep topology watertight before splitting shading
    // vertices. The large gate rock receives one extra subdivision.
    for (unsigned level=0;level<(variant==0 ? 4U : 3U);++level) {
        std::map<std::pair<u32,u32>,u32> edges;
        const auto midpoint = [&](u32 a,u32 b) {
            const auto edge=std::minmax(a,b);
            if (const auto it=edges.find(edge);it!=edges.end()) return it->second;
            const auto id=static_cast<u32>(points.size());
            points.push_back(unit(add(points[a],points[b])));
            edges.emplace(edge,id);
            return id;
        };
        std::vector<std::array<u32,3>> next;
        next.reserve(triangles.size()*4);
        for (const auto [a,b,c] : triangles) {
            const auto ab=midpoint(a,b),bc=midpoint(b,c),ca=midpoint(c,a);
            next.insert(next.end(),{{a,ab,ca},{b,bc,ab},{c,ca,bc},{ab,bc,ca}});
        }
        triangles=std::move(next);
    }
    std::array<Crater,9> craters{};
    for (std::size_t i=0;i<craters.size();++i) {
        const auto phase=static_cast<f32>(i)*2.39996F+seed;
        const auto y=-.8F+static_cast<f32>(i)*.2F;
        craters[i]={unit({std::cos(phase)*std::sqrt(1.F-y*y),y,
                         std::sin(phase)*std::sqrt(1.F-y*y)}),
            .19F+.08F*std::sin(phase*1.7F),.09F+.045F*std::cos(phase)};
    }
    // A clearly readable bowl on the camera-facing side, not just noise bumps.
    craters[0]={unit({-.3F,.2F,1}),.42F,.19F};
    const std::array aspects{Vec3{1.08F,.88F,.95F},Vec3{1.22F,.68F,.86F},Vec3{.80F,1.05F,.92F}};
    const auto aspect=aspects[variant];
    std::vector<Vec4> colors;
    colors.reserve(points.size());
    for (auto& p : points) {
        const auto q=add(mul(p,2.2F),{seed,-seed*.7F,seed*.3F});
        f32 radial=1.F+.11F*noise(q)+.055F*noise(mul(q,2.8F))+
                   .025F*noise(mul(q,7.1F))+.009F*noise(mul(q,17.F));
        f32 crater_depth{};
        for (const auto& crater : craters) {
            const auto x=std::sqrt(std::max(0.F,2.F*(1.F-dot(p,crater.direction))))/crater.width;
            const auto bowl=crater.depth*std::exp(-3.5F*x*x);
            radial-=bowl;
            radial+=.027F*std::exp(-55.F*(x-1.F)*(x-1.F));
            crater_depth+=bowl;
        }
        const auto grain=noise(mul(q,13.F));
        const auto strata=std::abs(std::sin(p.y*24.F+noise(mul(q,2.F))*4.F));
        const auto vein=std::pow(std::max(0.F,1.F-strata*5.F),3.F);
        const auto albedo=std::clamp(.19F+.045F*noise(mul(q,4.F))+.025F*grain-
                                   crater_depth*.25F,.075F,.30F);
        const std::array tint{Vec3{1.F,.83F,.65F},Vec3{.78F,.84F,.90F},Vec3{.93F,.73F,.58F}};
        const auto c=add(mul(tint[variant],albedo),mul(Vec3{.11F,.07F,.027F},vein));
        colors.push_back({c.x,c.y,c.z,1});
        p={p.x*radial*4.2F*aspect.x,p.y*radial*4.2F*aspect.y,p.z*radial*4.2F*aspect.z};
    }
    std::vector<Vec3> normals(points.size());
    for (const auto [a,b,c] : triangles) {
        const auto n=cross(sub(points[b],points[a]),sub(points[c],points[a]));
        for (const auto i : {a,b,c}) normals[i]=add(normals[i],n);
    }
    for (auto& n : normals) n=unit(n);
    std::vector<f32> positions, shading, rgba;
    vm::Document result;
    result.metadata={{"name",std::array{"BASALT / cratered gate","IRON / fractured slab","REGOLITH / rubble"}[variant]},
        {"author","Original Vibe Engine procedural asset"},{"source/tool","examples/support/asteroid_assets.cpp"},
        {"shading","Geometric craters, layered rock color, mixed smooth/faceted normals"}};
    for (const auto [a,b,c] : triangles) {
        const auto face=unit(cross(sub(points[b],points[a]),sub(points[c],points[a])));
        const auto first=static_cast<u32>(positions.size()/3);
        for (const auto i : {a,b,c}) {
            const auto p=points[i],n=unit(add(mul(normals[i],.72F),mul(face,.28F)));
            positions.insert(positions.end(),{p.x,p.y,p.z});
            shading.insert(shading.end(),{n.x,n.y,n.z});
            const auto color=colors[i];
            rgba.insert(rgba.end(),{color.x,color.y,color.z,color.w});
        }
        result.faces.emplace_back(first,first+1,first+2);
    }
    result.vertex_count=positions.size()/3;
    result.vertex_fields={{"position",{vm::ScalarType::Float32,3},std::move(positions)},
        {"normal",{vm::ScalarType::Float32,3},std::move(shading)},
        {"color/0",{vm::ScalarType::Float32,4},std::move(rgba)}};
    return result;
}
}
