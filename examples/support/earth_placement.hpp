#pragma once
#include "mesh_frame.hpp"
#include <numbers>

namespace example::earth::placement {
using namespace vng;
inline constexpr f32 radians=std::numbers::pi_v<f32>/180;
inline Vec3 add(Vec3 a,Vec3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
inline Vec3 mul(Vec3 a,f32 b){return {a.x*b,a.y*b,a.z*b};}
inline f32 dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline Vec3 cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
inline Vec3 unit(Vec3 p){return mul(p,1/std::hypot(p.x,p.y,p.z));}
inline Vec3 direction(Vec2 p){return {std::sin(p.x*radians)*std::cos(p.y*radians),std::sin(p.y*radians),std::cos(p.x*radians)*std::cos(p.y*radians)};}
inline Vec2 location(Vec3 p){p=unit(p);return {std::atan2(p.x,p.z)/radians,std::asin(std::clamp(p.y,-1.F,1.F))/radians};}
inline bool valid(Vec2 p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::abs(p.x)<=180&&std::abs(p.y)<=90;}
inline Mat4 turn(Vec3 axis,f32 angle) {
    const auto c=std::cos(angle),s=std::sin(angle);auto out=Mat4::identity();
    for(unsigned j=0;j<3;++j){Vec3 e{};e[j]=1;const auto v=add(add(mul(e,c),mul(cross(axis,e),s)),mul(axis,dot(axis,e)*(1-c)));out[j]={v.x,v.y,v.z,0};}
    return out;
}
inline Mat4 between(Vec3 a,Vec3 b) {
    const auto axis=cross(a,b);const auto sine=std::hypot(axis.x,axis.y,axis.z),cosine=std::clamp(dot(a,b),-1.F,1.F);
    if(sine<1e-6F)return cosine>0?Mat4::identity():turn(unit(cross(a,std::abs(a.y)<.8F?Vec3{0,1,0}:Vec3{1,0,0})),std::numbers::pi_v<f32>);
    return turn(mul(axis,1/sine),std::atan2(sine,cosine));
}
inline Mat4 frame(Vec2 p,f32 heading=0) {
    const auto up=direction(p),east=Vec3{std::cos(p.x*radians),0,-std::sin(p.x*radians)},north=cross(up,east);
    auto result=Mat4::identity();result[0]={east.x,east.y,east.z,0};result[1]={up.x,up.y,up.z,0};result[2]={-north.x,-north.y,-north.z,0};
    return mesh_frame::compose(turn(up,heading*radians),result);
}
inline f32 heading(Vec2 p,Vec3 east) {
    const auto f=frame(p);const Vec3 x{f[0][0],f[0][1],f[0][2]};
    return std::atan2(dot(cross(x,east),direction(p)),dot(x,east))/radians;
}
}
