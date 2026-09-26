#pragma once

#include "project.hpp"
#include "rotation_math.hpp"
#include <array>
#include <cmath>
#include <numbers>

namespace editor_example {
// Editor-only model. Its normalized geometry is shared by every camera; focus
// determines display size, zoom only changes the frustum. No authored mesh.
struct CameraGlyph {
    vng::Vec3 eye, forward, up, right;
    vng::f32 depth{}, half_height{}, half_width{}, body{};
    [[nodiscard]] vng::Vec3 at(vng::f32 x,vng::f32 y,vng::f32 z) const {
        return {eye.x+forward.x*z+right.x*x+up.x*y,eye.y+forward.y*z+right.y*x+up.y*y,
                eye.z+forward.z*z+right.z*x+up.z*y};
    }
    [[nodiscard]] vng::Vec3 point(vng::Vec3 p) const { return at(p.x*body,p.y*body,p.z*body); }
    [[nodiscard]] vng::Vec3 body_center() const { return point({0,.1F,-.8F}); }
};

[[nodiscard]] inline CameraGlyph camera_glyph(const SceneInstance& evaluated) {
    using namespace vng;
    CameraGlyph g;
    const auto* lens=std::get_if<CameraSettings>(&evaluated.settings);
    const CameraSettings settings=lens ? *lens : CameraSettings{};
    g.eye=evaluated.transform.position;
    g.forward=rotation_math::direction(evaluated.transform.rotation,{0,0,-1});
    g.up=rotation_math::direction(evaluated.transform.rotation,{0,1,0});
    g.right=rotation_math::direction(evaluated.transform.rotation,{1,0,0});
    g.depth=std::clamp(settings.focus*.15F,.35F,12.F);
    g.half_height=g.depth*std::tan(camera_vertical_fov*.5F*std::numbers::pi_v<f32>/180)/
        std::max(settings.zoom,camera_min_zoom);
    g.half_width=g.half_height*16/9;
    g.body=g.depth*.4F;
    return g;
}

namespace camera_glyph_detail {
enum class Finish { housing, metal, rubber, glass, pupil, reflection, screen, accent };
struct Triangle { vng::Vec3 a,b,c; Finish finish; };
struct Line { vng::Vec3 a,b; };
struct Model { std::vector<Triangle> triangles; std::vector<Line> lines; };

// Built once. Only purposeful seams are outlined, not triangle edges. Closed,
// extruded parts remain recognizable from below or behind. Local +Z is forward.
inline const Model& model() {
    using namespace vng;
    static const Model geometry=[] {
        Model m;
        const auto quad=[&](Vec3 a,Vec3 b,Vec3 c,Vec3 d,Finish finish) {
            m.triangles.push_back({a,b,c,finish});m.triangles.push_back({a,c,d,finish});
        };
        const auto box=[&](Vec3 low,Vec3 high,Finish finish) {
            std::array<Vec3,8> p;
            for(unsigned i=0;i<8;++i)p[i]={i&1?high.x:low.x,i&2?high.y:low.y,i&4?high.z:low.z};
            quad(p[0],p[2],p[3],p[1],finish);quad(p[4],p[5],p[7],p[6],finish);
            quad(p[0],p[4],p[6],p[2],finish);quad(p[1],p[3],p[7],p[5],finish);
            quad(p[0],p[1],p[5],p[4],finish);quad(p[2],p[6],p[7],p[3],finish);
        };
        // Beveled housing: eight-sided profile extruded along the lens.
        const std::array<Vec2,8> profile{{{-.35F,-.32F},{.35F,-.32F},{.46F,-.21F},{.46F,.24F},
            {.35F,.35F},{-.35F,.35F},{-.46F,.24F},{-.46F,-.21F}}};
        for(unsigned i=0;i<profile.size();++i) {
            const auto a=profile[i],b=profile[(i+1)%profile.size()];
            const Vec3 back_a{a.x,a.y,-1.25F},back_b{b.x,b.y,-1.25F};
            const Vec3 front_a{a.x,a.y,-.45F},front_b{b.x,b.y,-.45F};
            quad(back_a,back_b,front_b,front_a,Finish::housing);
            m.triangles.push_back({{0,0,-1.25F},back_b,back_a,Finish::housing});
            m.triangles.push_back({{0,0,-.45F},front_a,front_b,Finish::housing});
            m.lines.push_back({back_a,back_b});m.lines.push_back({front_a,front_b});
            m.lines.push_back({back_a,front_a});
        }
        constexpr unsigned segments=16;
        const auto rim=[](f32 radius,f32 z,unsigned i) {
            const auto angle=static_cast<f32>(i%segments)*2*std::numbers::pi_v<f32>/segments;
            return Vec3{radius*std::cos(angle),radius*std::sin(angle),z};
        };
        const auto barrel=[&](f32 back,f32 front,f32 r0,f32 r1,Finish finish,bool outline) {
            for(unsigned i=0;i<segments;++i) {
                const auto a=rim(r0,back,i),b=rim(r0,back,i+1),c=rim(r1,front,i+1),d=rim(r1,front,i);
                quad(a,b,c,d,finish);
                m.triangles.push_back({{0,0,back},b,a,finish});
                m.triangles.push_back({{0,0,front},d,c,finish});
                if(outline)m.lines.push_back({d,c});
            }
        };
        // Closed stepped lens barrel and rubber focusing rings.
        barrel(-.46F,-.36F,.27F,.28F,Finish::metal,true);
        barrel(-.36F,-.12F,.28F,.26F,Finish::rubber,false);
        barrel(-.12F,-.025F,.27F,.28F,Finish::metal,true);
        for(const auto z:{-.32F,-.27F,-.22F,-.17F})
            barrel(z,z+.012F,.282F,.282F,Finish::rubber,false);
        // Keep even the front glass behind the eye plane: this camera cannot
        // obscure its own view when entered.
        barrel(-.024F,-.020F,.235F,.235F,Finish::rubber,false);
        barrel(-.019F,-.015F,.202F,.202F,Finish::glass,false);
        barrel(-.014F,-.010F,.163F,.163F,Finish::pupil,false);
        quad({-.13F,.07F,-.005F},{-.06F,.13F,-.005F},{-.01F,.10F,-.005F},
             {-.08F,.04F,-.005F},Finish::reflection);
        // Side grip, top carry handle with a real opening, and mounting foot.
        box({.44F,-.29F,-1.13F},{.64F,.19F,-.58F},Finish::rubber);
        box({-.09F,.35F,-1.06F},{.09F,.57F,-.95F},Finish::metal);
        box({-.09F,.35F,-.63F},{.09F,.57F,-.52F},Finish::metal);
        box({-.11F,.57F,-1.10F},{.11F,.65F,-.48F},Finish::rubber);
        box({-.25F,-.41F,-1.03F},{.25F,-.32F,-.62F},Finish::metal);
        // Rear monitor and viewfinder eyecup make the reverse view distinct.
        box({-.34F,-.23F,-1.285F},{.34F,.24F,-1.25F},Finish::rubber);
        quad({-.28F,-.17F,-1.29F},{-.28F,.18F,-1.29F},{.28F,.18F,-1.29F},{.28F,-.17F,-1.29F},Finish::screen);
        box({-.34F,.36F,-1.36F},{-.14F,.52F,-1.03F},Finish::rubber);
        // Tally light and side badge carry selection/active-camera accents.
        box({.29F,.27F,-.448F},{.37F,.31F,-.425F},Finish::accent);
        box({-.467F,-.02F,-.96F},{-.46F,.09F,-.73F},Finish::accent);
        for(const auto y:{-.12F,-.17F,-.22F})
            m.lines.push_back({{-.468F,y,-1.07F},{-.468F,y,-.67F}});
        return m;
    }();
    return geometry;
}

inline vng::Vec4 color(const Triangle& t,vng::Vec4 accent) {
    using namespace vng;
    Vec4 base;
    switch(t.finish) {
    case Finish::housing:base={.38F,.46F,.55F,1};break;
    case Finish::metal:base={.56F,.64F,.71F,1};break;
    case Finish::rubber:base={.105F,.13F,.17F,1};break;
    case Finish::glass:return {.045F,.28F,.39F,1};
    case Finish::pupil:return {.02F,.085F,.135F,1};
    case Finish::reflection:return {.36F,.72F,.8F,1};
    case Finish::screen:return {.07F,.26F,.34F,1};
    case Finish::accent:return accent;
    }
    // Stable studio shading, independent of scene lights/exposure. No new
    // material or lighting requirements on the shared annotation renderer.
    const Vec3 u{t.b.x-t.a.x,t.b.y-t.a.y,t.b.z-t.a.z},v{t.c.x-t.a.x,t.c.y-t.a.y,t.c.z-t.a.z};
    const Vec3 n{u.y*v.z-u.z*v.y,u.z*v.x-u.x*v.z,u.x*v.y-u.y*v.x};
    const auto length=std::sqrt(n.x*n.x+n.y*n.y+n.z*n.z);
    const auto shade=.62F+.38F*std::max(0.F,(n.x*-.35F+n.y*.8F+n.z*.48F)/std::max(length,1e-8F));
    return {base.x*shade,base.y*shade,base.z*shade,1};
}
} // namespace camera_glyph_detail

inline const std::size_t camera_glyph_line_count=camera_glyph_detail::model().lines.size()+11;
inline const std::size_t camera_glyph_triangle_count=camera_glyph_detail::model().triangles.size();

template<class Emit>
void camera_glyph_lines(const CameraGlyph& g,Emit emit) {
    using namespace vng;
    for(const auto& line:camera_glyph_detail::model().lines)emit(g.point(line.a),g.point(line.b));
    const std::array corners{g.at(-g.half_width,-g.half_height,g.depth),g.at(g.half_width,-g.half_height,g.depth),
        g.at(g.half_width,g.half_height,g.depth),g.at(-g.half_width,g.half_height,g.depth)};
    for(const auto& corner:corners)emit(g.eye,corner);
    for(unsigned i=0;i<4;++i)emit(corners[i],corners[(i+1)%4]);
    const auto peak=g.at(0,g.half_height*1.35F,g.depth);
    emit(corners[2],peak);emit(peak,corners[3]);
    emit(g.at(0,0,g.depth),g.at(0,0,g.depth*2));
}

template<class Emit>
void camera_glyph_triangles(const CameraGlyph& g,vng::Vec4 accent,Emit emit) {
    for(const auto& t:camera_glyph_detail::model().triangles)
        emit(g.point(t.a),g.point(t.b),g.point(t.c),camera_glyph_detail::color(t,accent));
}
} // namespace editor_example
