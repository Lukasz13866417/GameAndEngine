#pragma once
#include "animation.hpp"
#include <algorithm>
#include <cmath>

namespace editor_example {
struct SceneLine {
    vng::Vec3 from, to;
    vng::Vec4 color;
    friend bool operator==(const SceneLine&, const SceneLine&) = default;
};
// Clip two orthogonal families of grid lines against each polygon. Pairing
// sorted edge crossings handles concave outlines without fan triangulation.
inline void region_wall_grid(std::vector<SceneLine>& lines, const RegionGeometry& shape, vng::Vec4 color) {
    using namespace vng;
    const auto subtract=[](Vec3 a,Vec3 b){return Vec3{a.x-b.x,a.y-b.y,a.z-b.z};};
    const auto dot=[](Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
    const auto cross=[](Vec3 a,Vec3 b){return Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
    const auto normalize=[&](Vec3 a){const auto n=std::sqrt(dot(a,a));return n>1e-7F?Vec3{a.x/n,a.y/n,a.z/n}:Vec3{};};
    for(const auto& face:shape.faces) {
        if(face.size()<3)continue;
        const auto origin=shape.points.at(face[0]);
        Vec3 normal{},longest{}; f32 length{};
        for(std::size_t i=0;i<face.size();++i) {
            const auto a=subtract(shape.points.at(face[i]),origin),b=subtract(shape.points.at(face[(i+1)%face.size()]),origin);
            const auto n=cross(a,b),edge=subtract(b,a);
            for(unsigned c=0;c<3;++c)normal[c]+=n[c];
            if(dot(edge,edge)>length){longest=edge;length=dot(edge,edge);}
        }
        const auto u=normalize(longest),v=normalize(cross(normal,u));
        if(dot(v,v)<.5F)continue; // Collapsed/degenerate face remains editable via edges.
        for(auto axis:std::array{u,v}) {
            const auto across=cross(normalize(normal),axis);
            f32 low=0,high=0;
            for(auto index:face){const auto x=dot(subtract(shape.points.at(index),origin),axis);low=std::min(low,x);high=std::max(high,x);}
            if(high-low<1e-6F)continue;
            for(unsigned step=1;step<10;++step) {
                const auto coordinate=low+(high-low)*static_cast<f32>(step)/10;
                std::vector<std::pair<f32,Vec3>> crossings;
                for(std::size_t i=0;i<face.size();++i) {
                    const auto a=shape.points.at(face[i]),b=shape.points.at(face[(i+1)%face.size()]);
                    const auto x=dot(subtract(a,origin),axis),y=dot(subtract(b,origin),axis);
                    if((x<=coordinate&&y>coordinate)||(y<=coordinate&&x>coordinate)) {
                        const auto t=(coordinate-x)/(y-x); Vec3 p{};
                        for(unsigned c=0;c<3;++c)p[c]=a[c]+(b[c]-a[c])*t;
                        crossings.emplace_back(dot(subtract(p,origin),across),p);
                    }
                }
                std::ranges::sort(crossings,{},&std::pair<f32,Vec3>::first);
                for(std::size_t i=1;i<crossings.size();i+=2)
                    lines.push_back({crossings[i-1].second,crossings[i].second,color});
            }
        }
    }
}
inline std::vector<SceneLine> scene_annotation_lines(const State& state,vng::f32 time,vng::u32 hidden_object=0) {
    using namespace vng;
    std::vector<SceneLine> lines;
    if(state.viewport.mode!=ViewMode::scene)return lines;
    if(state.viewport.show_world_bounds) {
        std::array<Vec3,8> corners;
        for(unsigned i=0;i<8;++i)for(unsigned c=0;c<3;++c)
            corners[i][c]=i&(1U<<c)?state.document.world_bounds.maximum[c]:state.document.world_bounds.minimum[c];
        for(unsigned i=0;i<8;++i)for(unsigned c=0;c<3;++c)if(!(i&(1U<<c)))
            lines.push_back({corners[i],corners[i|(1U<<c)],{.28F,.65F,.85F,1}});
    }
    if(state.viewport.show_regions) for(const auto& instance:state.document.instances) {
        if(instance.id==hidden_object)continue;
        const auto* settings=std::get_if<RegionSettings>(&instance.settings);
        if(!settings || !evaluate_visibility(state,instance,time))continue;
        const auto transform=mesh_transform(ViewMode::scene,evaluate_transform(state,instance,time));
        auto boundary=settings->boundary;
        for(auto& p:boundary.points) {
            const auto original=p;
            for(unsigned c=0;c<3;++c)p[c]=transform[3][c]+transform[0][c]*original.x+transform[1][c]*original.y+transform[2][c]*original.z;
        }
        const auto color=instance.id==state.viewport.selected_object?Vec4{1,.52F,.08F,1}:Vec4{.1F,.7F,.6F,1};
        for(const auto edge:boundary.edges())lines.push_back({boundary.points.at(edge[0]),boundary.points.at(edge[1]),color});
        if(settings->show_walls)region_wall_grid(lines,boundary,{color.x*.55F,color.y*.55F,color.z*.55F,1});
    }
    return lines;
}
}
