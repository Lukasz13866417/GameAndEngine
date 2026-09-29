#include "earth_structures.hpp"
#include <cmath>
#include <numbers>

namespace example::earth::detail {
namespace {
using namespace vng;
using namespace placement;
constexpr Vec3 hull{.11F,.17F,.20F}, armor{.045F,.065F,.080F}, rib{.022F,.030F,.038F};
constexpr Vec3 trim{.14F,.46F,.52F}, amber{.80F,.43F,.13F};
Vec3 sub(Vec3 a,Vec3 b){return add(a,mul(b,-1));}

class Builder {
public:
    StructureMesh mesh;
    void triangle(Vec3 a,Vec3 b,Vec3 c,Vec3 color) {
        const auto n=unit(cross(sub(b,a),sub(c,a)));const auto i=u32(mesh.vertices.size());
        for(auto p:{a,b,c})mesh.vertices.push_back({p,n,color,0});
        mesh.faces.emplace_back(i,i+1,i+2);
    }
    void quad(Vec3 a,Vec3 b,Vec3 c,Vec3 d,Vec3 color,f32 emission=0) {
        const auto normal=unit(cross(sub(b,a),sub(c,a)));
        const auto i=u32(mesh.vertices.size());
        for(auto p:{a,b,c,d})mesh.vertices.push_back({p,normal,color,emission});
        mesh.faces.emplace_back(i,i+1,i+2);mesh.faces.emplace_back(i,i+2,i+3);
    }
    // Oriented beam / slab. Sizes are full dimensions.
    void box(Vec3 center,Vec3 x,Vec3 y,Vec3 z,Vec3 size,Vec3 color,f32 emission=0) {
        std::array<Vec3,8> p;
        for(unsigned i=0;i<8;++i)p[i]=add(center,add(add(mul(x,(i&1?.5F:-.5F)*size.x),
            mul(y,(i&2?.5F:-.5F)*size.y)),mul(z,(i&4?.5F:-.5F)*size.z)));
        for(auto f:std::array<std::array<unsigned,4>,6>{{{0,4,6,2},{1,3,7,5},{0,1,5,4},
            {2,6,7,3},{0,2,3,1},{4,5,7,6}}})quad(p[f[0]],p[f[1]],p[f[2]],p[f[3]],color,emission);
    }
    void box(Vec3 p,Vec3 size,Vec3 color,f32 emission=0){box(p,{1,0,0},{0,1,0},{0,0,1},size,color,emission);}
    void beam(Vec3 a,Vec3 b,f32 width,Vec3 color,Vec3 reference) {
        const auto axis=unit(sub(b,a));
        const auto x=unit(cross(reference,axis));
        box(mul(add(a,b),.5F),x,cross(axis,x),axis,{width,width,std::hypot(b.x-a.x,b.y-a.y,b.z-a.z)},color);
    }
    void beam(Vec3 a,Vec3 b,f32 width,Vec3 color) {
        const auto axis=unit(sub(b,a));beam(a,b,width,color,std::abs(axis.y)<.9F?Vec3{0,1,0}:Vec3{0,0,1});
    }
};
// A flattened octagonal cross section: broad roof, chamfered corners, hollow
// mouth. Inner and outer skins plus rim thickness are actual opaque geometry.
constexpr std::array<Vec2,8> section{{{1,.55F},{.65F,1},{-.65F,1},{-1,.55F},
    {-1,-.55F},{-.65F,-1},{.65F,-1},{1,-.55F}}};
Vec3 section_point(const SkywaySample& p,unsigned j,f32 width,f32 height) {
    return add(p.position,add(mul(p.side,section[j].x*width),mul(p.up,section[j].y*height)));
}
void rim(Builder& b,const SkywaySample& p,f32 width,f32 height,f32 thickness,f32 depth,Vec3 color) {
    auto front=p,back=p;front.position=add(p.position,mul(p.tangent,depth*.5F));
    back.position=add(p.position,mul(p.tangent,-depth*.5F));
    for(unsigned j=0;j<8;++j) {
        const auto k=(j+1)%8;
        const auto a=section_point(front,j,width,height),c=section_point(back,j,width,height);
        const auto next=section_point(front,k,width,height),far=section_point(back,k,width,height);
        const auto ai=section_point(front,j,width-thickness,height-thickness),ci=section_point(back,j,width-thickness,height-thickness);
        const auto ni=section_point(front,k,width-thickness,height-thickness),fi=section_point(back,k,width-thickness,height-thickness);
        b.quad(a,c,far,next,color);b.quad(ai,ni,fi,ci,color);
        b.quad(a,next,ni,ai,color);b.quad(c,ci,fi,far,color);
    }
}
// Unit apothem: flat roof, floor and side walls, eight equal edges. A radial
// circle sampled eight times and then stretched would not be a regular octagon.
constexpr f32 octagon_corner=.414213562373095F; // tan(pi/8)
constexpr std::array<Vec2,8> bore{{{1,octagon_corner},{octagon_corner,1},
    {-octagon_corner,1},{-1,octagon_corner},{-1,-octagon_corner},
    {-octagon_corner,-1},{octagon_corner,-1},{1,-octagon_corner}}};
Vec3 bore_point(const TunnelSection& p,unsigned corner,f32 apothem) {
    return add(p.frame.position,add(mul(p.frame.side,bore[corner].x*apothem*p.size),
        mul(p.frame.up,bore[corner].y*apothem*p.size)));
}
Vec3 bore_normal(const SkywaySample& p,unsigned face) {
    const auto next=(face+1)%bore.size();
    return unit(add(mul(p.side,bore[face].x+bore[next].x),mul(p.up,bore[face].y+bore[next].y)));
}
}

StructureMesh tunnel_shell(std::span<const TunnelSection> path,f32 light) {
    StructureMesh mesh;
    if(path.size()<2)return mesh;
    constexpr unsigned sides=bore.size();
    constexpr f32 outer=.006F,inner=tunnel_inner_height;
    const auto quad=[&](u32 a,u32 b,u32 c,u32 d) {
        // Match winding to the authored normal regardless of route direction.
        const auto geometric=cross(sub(mesh.vertices[b].position,mesh.vertices[a].position),
            sub(mesh.vertices[c].position,mesh.vertices[a].position));
        if(dot(geometric,mesh.vertices[a].normal)<0)std::swap(b,d);
        mesh.faces.emplace_back(a,b,c);mesh.faces.emplace_back(a,c,d);
    };
    // Finite thickness lets depth testing choose dark exterior or lit lining.
    // Share samples along the route, but NOT normals across interior corners:
    // each of the eight wall planes must read as a plane, not a smooth cylinder.
    constexpr std::array<Vec3,8> lining{{{.048F,.079F,.11F},{.11F,.135F,.15F},
        {.026F,.049F,.073F},{.067F,.10F,.125F},{.044F,.057F,.071F},
        {.025F,.032F,.044F},{.082F,.075F,.068F},{.034F,.057F,.083F}}};
    for(bool inside:{false,true}) {
        const auto first=u32(mesh.vertices.size());
        for(std::size_t i=0;i<path.size();++i)for(unsigned j=0;j<sides;++j) {
            const auto& p=path[i];
            if(inside) {
                // Gentle deterministic bay-to-bay variation; no random flicker
                // when a part is rebuilt or its lighting strength changes.
                const auto color=mul(lining[j],.8F+.1F*f32((i*7+j*3)%5));
                const auto normal=mul(bore_normal(p.frame,j),-1);
                for(auto k:{j,(j+1)%sides})mesh.vertices.push_back({bore_point(p,k,inner),normal,color,0});
            } else {
                const auto normal=unit(add(mul(p.frame.side,bore[j].x),mul(p.frame.up,bore[j].y)));
                mesh.vertices.push_back({bore_point(p,j,outer),normal,mul(hull,.8F+.2F*bore[j].y),0});
            }
        }
        const u32 stride=sides*(inside?2:1);
        for(u32 i=0;i+1<path.size();++i)for(u32 j=0;j<sides;++j) {
            const auto a=first+i*stride+j*(inside?2:1),b=inside?a+1:first+i*stride+(j+1)%sides;
            quad(a,b,b+stride,a+stride);
        }
    }
    // Only seal wall thickness, never the passage. Use distinct rim normals.
    for(const auto index:{std::size_t{0},path.size()-1})for(unsigned j=0;j<sides;++j) {
        const auto& p=path[index];
        const auto first=u32(mesh.vertices.size());
        const auto normal=mul(p.frame.tangent,index==0?-1.F:1.F);
        for(auto v:{bore_point(p,j,outer),bore_point(p,(j+1)%sides,outer),
            bore_point(p,(j+1)%sides,inner),bore_point(p,j,inner)})
            mesh.vertices.push_back({v,normal,rib,0});
        quad(first,first+1,first+2,first+3);
    }
    // Each corner has a narrow recessed-looking guide seam. The nearby change
    // of wall normal, dark lining and luminous trim makes all eight junctions
    // legible. These are strips, not hundreds of separate box meshes.
    for(unsigned j=0;j<sides;++j) {
        const auto first=u32(mesh.vertices.size());
        const auto lamp=j<4?Vec3{.21F,.65F,.80F}:Vec3{.95F,.49F,.17F};
        for(const auto& p:path) {
            const auto x=bore_point(p,j,inner-.000015F),y=bore_point(p,(j+1)%sides,inner-.000015F);
            const auto normal=mul(bore_normal(p.frame,j),-1);
            for(f32 t:{.012F,.030F})mesh.vertices.push_back({add(mul(x,1-t),mul(y,t)),normal,lamp,3*light});
        }
        for(u32 i=0;i+1<path.size();++i) {
            const auto v=first+i*2;quad(v,v+1,v+3,v+2);
        }
    }
    // Staggered dark maintenance plates, alternating between wall planes and
    // between long and short bays. Follow individual route segments rather
    // than spanning multiple bends of a curved / Bezier route.
    for(std::size_t i=0;i+1<path.size();++i) {
        const unsigned j=(i*3+3)%sides;
        const auto first=u32(mesh.vertices.size());
        const auto across=[&](const TunnelSection& p,f32 t) {
            return add(mul(bore_point(p,j,inner-.000025F),1-t),mul(bore_point(p,(j+1)%sides,inner-.000025F),t));
        };
        const f32 left=.24F+.06F*f32(i%3),right=left+(i%2?.28F:.48F);
        for(const auto t:{.10F,.82F})for(const auto u:{left,right}) {
            const auto position=add(mul(across(path[i],u),1-t),mul(across(path[i+1],u),t));
            const auto normal=mul(unit(add(mul(bore_normal(path[i].frame,j),1-t),mul(bore_normal(path[i+1].frame,j),t))),-1);
            mesh.vertices.push_back({position,normal,i%3?armor:Vec3{.14F,.16F,.17F},0});
        }
        quad(first,first+1,first+3,first+2);
    }
    // Shallow transverse reinforcement bands close the eight corner seams into
    // a visible octagon, even far between the large exterior support collars.
    // Two indexed rings per fourth bay keep these much cheaper than eight boxes.
    for(std::size_t i=0;i+1<path.size();i+=4) {
        const auto first=u32(mesh.vertices.size());
        for(f32 t:{.44F,.56F})for(unsigned j=0;j<sides;++j) {
            const auto a=bore_point(path[i],j,inner-.00004F),b=bore_point(path[i+1],j,inner-.00004F);
            const auto center=add(mul(path[i].frame.position,1-t),mul(path[i+1].frame.position,t));
            const auto position=add(mul(a,1-t),mul(b,t));
            mesh.vertices.push_back({position,unit(sub(center,position)),{.009F,.015F,.025F},0});
        }
        for(unsigned j=0;j<sides;++j)quad(first+j,first+(j+1)%sides,first+sides+(j+1)%sides,first+sides+j);
    }
    const auto first=u32(mesh.vertices.size());
    for(const auto& p:path)for(f32 sign:{-1.F,1.F})
        mesh.vertices.push_back({add(p.frame.position,add(mul(p.frame.up,.0062F*p.size),
            mul(p.frame.side,sign*.0011F*p.size))),p.frame.up,trim,1.5F*light});
    for(u32 i=0;i+1<path.size();++i) {
        const auto v=first+i*2;quad(v,v+1,v+3,v+2);
    }
    return mesh;
}

StructureMesh tunnel_collar(const SkywaySample& frame,f32 size) {
    StructureMesh mesh;
    constexpr unsigned sides=bore.size();
    // Shallow inward lip avoids coplanar lining/rib surfaces (z-fighting).
    constexpr f32 lip=tunnel_inner_height*.985F;
    constexpr std::array<Vec2,4> profile{{{lip,-.00065F},{.0084F,-.00065F},
        {.0084F,.00065F},{lip,.00065F}}};
    for(const auto p:profile)for(unsigned j=0;j<sides;++j) {
        const auto radial=add(mul(frame.side,bore[j].x),mul(frame.up,bore[j].y));
        // Radial + shoulder normals distinguish the front and rear shoulders.
        const auto normal=unit(add(mul(unit(radial),p.x==lip?-1.F:1.F),
            mul(frame.tangent,p.y<0?-1.F:1.F)));
        mesh.vertices.push_back({add(frame.position,add(mul(radial,p.x*size),mul(frame.tangent,p.y*size))),
            normal,mul(rib,.85F+.15F*bore[j].y),0});
    }
    for(u32 k=0;k<4;++k)for(u32 j=0;j<sides;++j) {
        const auto a=k*sides+j,b=k*sides+(j+1)%sides,c=((k+1)%4)*sides+(j+1)%sides,d=((k+1)%4)*sides+j;
        mesh.faces.emplace_back(a,b,c);mesh.faces.emplace_back(a,c,d);
    }
    return mesh;
}

std::array<std::size_t,7> collar_sections(std::size_t segments) {
    std::array<std::size_t,7> result{};
    for(std::size_t i=0;i<result.size();++i)result[i]=(i*segments+3)/6; // nearest sample
    return result;
}

TerminalPath freestanding_terminal_path(f32 size,f32 height) {
    TerminalPath path;
    for(std::size_t i=0;i<path.size();++i) {
        const auto t=f32(i)/f32(path.size()-1);
        // Preserve the throat's socket frame; progressively increase incline
        // toward the exit (rise .026 instead of .018 at the default height).
        const Vec3 tangent=unit({0,(.018F+.016F*t)*height,.11F*size});
        path[i]={{0,1.025F+(.018F*t+.008F*t*t)*height,(t-.5F)*.11F*size},tangent,{1,0,0},unit(cross(tangent,{1,0,0}))};
    }
    return path;
}
StructureMesh dispersal_terminal(const TerminalPath& path,f32 size,f32 light) {
    Builder b;
    const auto width=[&](std::size_t i){const auto t=f32(i)/f32(path.size()-1);return size*(.007F+.033F*t*t*(3-2*t));};
    const auto height=[&](std::size_t i){const auto t=f32(i)/f32(path.size()-1);return size*(.006F+.007F*t);};
    const auto wall=.0011F*size;
    for(std::size_t i=0;i+1<path.size();++i)for(unsigned j=0;j<8;++j) {
        const auto k=(j+1)%8;
        const auto a=section_point(path[i],j,width(i),height(i)),d=section_point(path[i],k,width(i),height(i));
        const auto c=section_point(path[i+1],k,width(i+1),height(i+1)),next=section_point(path[i+1],j,width(i+1),height(i+1));
        b.quad(a,d,c,next,j==1?hull:armor);
        b.quad(section_point(path[i],j,width(i)-wall,height(i)-wall),section_point(path[i+1],j,width(i+1)-wall,height(i+1)-wall),
            section_point(path[i+1],k,width(i+1)-wall,height(i+1)-wall),section_point(path[i],k,width(i)-wall,height(i)-wall),hull);
        // Deliberately sparse navigation strips, not an entirely glowing shell.
        if(j==0||j==3)b.quad(a,next,add(next,mul(path[i+1].up,wall*.6F)),add(a,mul(path[i].up,wall*.6F)),trim,light);
    }
    for(std::size_t i:{0U,2U,4U,6U,8U})rim(b,path[i],width(i)+wall,height(i)+wall,wall*1.8F,.003F*size,rib);
    const auto& mouth=path.back();
    // Two short divider fins produce three clear exit lanes. They extend out
    // beyond the flared mouth, so small craft have room to fan away from it.
    for(f32 side:{-.33F,.33F}) {
        const auto center=add(mouth.position,add(mul(mouth.side,side*width(8)),mul(mouth.tangent,.005F*size)));
        b.box(center,mouth.side,mouth.up,mouth.tangent,{.002F*size,.023F*size,.021F*size},rib);
        b.box(add(center,mul(mouth.up,.012F*size)),mouth.side,mouth.up,mouth.tangent,{.0024F*size,.001F*size,.020F*size},amber,light*1.2F);
    }
    // A narrow underside apron and shoulder pylons keep the shape architectural.
    b.box(add(mouth.position,mul(mouth.up,-.015F*size)),mouth.side,mouth.up,mouth.tangent,{.090F*size,.004F*size,.028F*size},armor);
    for(f32 sign:{-1.F,1.F}) {
        const auto p=add(mouth.position,mul(mouth.side,sign*.045F*size));
        b.box(p,mouth.side,mouth.up,mouth.tangent,{.009F*size,.046F*size,.020F*size},hull);
        b.box(add(p,mul(mouth.up,.023F*size)),mouth.side,mouth.up,mouth.tangent,{.010F*size,.002F*size,.021F*size},amber,light*1.4F);
    }
    return std::move(b.mesh);
}
StructureMesh orbital_elevator(f32 size,f32 height,f32 light) {
    Builder b;
    // Twin inclined pylons and cross braces, carrying decks rather than another
    // stack of launch pads. A thin central tether rises above the upper station.
    const auto at=[&](f32 x,f32 y,f32 z){return Vec3{x*size,1.012F+y*height,z*size};};
    b.box(at(0,.005F,0),{.11F*size,.010F*height,.075F*size},armor);
    for(f32 sign:{-1.F,1.F}) {
        b.beam(at(sign*.040F,0,0),at(sign*.014F,.185F,0),.012F*size,hull);
        b.box(at(sign*.044F,.016F,0),{.030F*size,.032F*height,.049F*size},hull);
        for(f32 y:{.065F,.12F})b.beam(at(sign*.03F,y,-.004F),at(-sign*.015F,y+.044F,-.004F),.004F*size,rib);
    }
    for(f32 y:{.080F,.163F}) {
        b.box(at(0,y,0),{.095F*size,.009F*height,.055F*size},armor);
        b.box(at(0,y+.008F,-.010F),{.057F*size,.010F*height,.029F*size},hull);
        b.box(at(0,y+.014F,.006F),{.040F*size,.002F*height,.0012F*size},trim,light*1.4F);
        // Docking fingers have dark channels between them, with warm end lights.
        for(f32 x:{-.034F,0.F,.034F}) {
            b.box(at(x,y,.035F),{.015F*size,.006F*height,.040F*size},hull);
            b.box(at(x,y+.0035F,.053F),{.011F*size,.001F*height,.003F*size},amber,light*1.2F);
        }
    }
    b.box(at(0,.157F,0),{.0025F*size,.312F*height,.0025F*size},rib);
    b.box(at(0,.244F,0),{.026F*size,.011F*height,.019F*size},hull);
    b.box(at(0,.25F,0),{.015F*size,.001F*height,.014F*size},trim,light*1.7F);
    b.box(at(0,.310F,0),{.006F*size,.003F*height,.006F*size},amber,light*2.F);
    return std::move(b.mesh);
}
std::array<SkywaySample,3> joiner_sockets(f32 size,f32 height) {
    const auto y=1.025F+.045F*height;
    const auto sample=[&](Vec3 p,Vec3 tangent){return SkywaySample{p,tangent,cross({0,1,0},tangent),{0,1,0}};};
    return {sample({0,y,-.060F*size},{0,0,-1}),
        sample({-.052F*size,y,.052F*size},{-.6F,0,.8F}),
        sample({.052F*size,y,.052F*size},{.6F,0,.8F})};
}
StructureMesh atmospheric_processor(f32 size,f32 height,f32 light) {
    Builder b;
    const auto at=[&](f32 x,f32 y,f32 z){return Vec3{x*size,1.012F+y*height,z*size};};
    // A stepped industrial island, three hollow intake towers, and offset
    // cooling banks. Open mouths and external ribs give a non-transport silhouette.
    b.box(at(0,.004F,0),{.15F*size,.008F*height,.105F*size},armor);
    b.box(at(0,.010F,0),{.13F*size,.006F*height,.089F*size},hull);
    constexpr std::array<Vec3,3> towers{{{-.039F,1.F,-.012F},{.030F,.78F,-.018F},{0,.60F,.028F}}};
    constexpr std::array<Vec2,5> profile{{{0,.020F},{.022F,.018F},{.065F,.012F},{.098F,.016F},{.12F,.023F}}};
    for(const auto& tower:towers) {
        const auto point=[&](std::size_t level,unsigned j,f32 inset=0.F) {
            const auto angle=2.F*placement::radians*180.F*f32(j)/12;
            const auto r=profile[level].y-inset;
            return at(tower.x+std::cos(angle)*r,.014F+profile[level].x*tower.y,tower.z+std::sin(angle)*r);
        };
        for(unsigned j=0;j<12;++j) {
            const auto k=(j+1)%12;
            for(std::size_t i=0;i+1<profile.size();++i) {
                b.quad(point(i,j),point(i+1,j),point(i+1,k),point(i,k),j%3? hull:armor);
                b.quad(point(i,j,.002F),point(i,k,.002F),point(i+1,k,.002F),point(i+1,j,.002F),rib);
                if(j%2==0)b.beam(point(i,j),point(i+1,j),.0019F*size,rib);
            }
            b.quad(point(4,j),point(4,j,.002F),point(4,k,.002F),point(4,k),armor);
            // Short illuminated vanes around each intake, not a glowing solid cap.
            if(j%2==0)b.box(point(4,j,.0027F),{.002F*size,.001F*height,.002F*size},trim,.7F*light);
        }
        // A recessed grate makes the opening visibly deep from above.
        for(int i=-2;i<=2;++i)
            b.box(at(tower.x+f32(i)*.0035F,.083F*tower.y+.014F,tower.z),{.0012F*size,.001F*height,.020F*size},armor);
    }
    for(f32 sign:{-1.F,1.F}) {
        b.box(at(sign*.063F,.022F,0),{.018F*size,.027F*height,.073F*size},armor);
        for(unsigned i=0;i<9;++i)
            b.box(at(sign*.063F,.037F,-.032F+f32(i)*.008F),{.022F*size,.008F*height,.003F*size},hull);
        b.box(at(sign*.067F,.014F,.040F),{.014F*size,.002F*height,.004F*size},amber,.6F*light);
    }
    b.box(at(.032F,.020F,.040F),{.032F*size,.014F*height,.015F*size},hull);
    b.box(at(.032F,.024F,.048F),{.023F*size,.002F*height,.001F*size},trim,light);
    return std::move(b.mesh);
}
StructureMesh tunnel_joiner(f32 size,f32 height,f32 light) {
    Builder b;const auto sockets=joiner_sockets(size,height);
    const auto y=sockets[0].position.y,h=.006F*size,wall=.0015F*size;
    // Concave Y-shaped chamber, with three genuinely open port edges. Roof
    // and floor share this footprint; no overlapping tubes block the fork.
    const std::array<Vec2,9> outline{{{-.007F,-.060F},{.007F,-.060F},{.014F,-.017F},
        {.0576F,.0478F},{.0464F,.0562F},{0,.020F},{-.0464F,.0562F},{-.0576F,.0478F},{-.014F,-.017F}}};
    const auto at=[&](Vec2 p,f32 dy){return Vec3{p.x*size,y+dy,p.y*size};};
    for(std::size_t i=0;i<outline.size();++i) {
        const auto a=outline[i],c=outline[(i+1)%outline.size()];
        b.triangle({0,y+h+wall,0},at(c,h+wall),at(a,h+wall),hull);
        b.triangle({0,y+h,0},at(a,h),at(c,h),armor);
        b.triangle({0,y-h,0},at(c,-h),at(a,-h),hull);
        b.triangle({0,y-h-wall,0},at(a,-h-wall),at(c,-h-wall),armor);
        if(i==0||i==3||i==6)continue; // open inlet and two outlets
        const auto start=at(a,0),end=at(c,0),edge=sub(end,start),along=unit(edge);
        const auto outward=cross({0,1,0},along);
        b.box(add(mul(add(start,end),.5F),mul(outward,wall*.5F)),outward,{0,1,0},along,
            {wall,2*(h+wall),std::hypot(edge.x,edge.y,edge.z)},armor);
    }
    for(const auto& socket:sockets) {
        rim(b,socket,.0085F*size,.0075F*size,.0016F*size,.004F*size,rib);
        b.box(add(socket.position,{0,.009F*size,0}),socket.side,socket.up,socket.tangent,
            {.009F*size,.001F*size,.004F*size},trim,light);
    }
    b.box({0,y+.012F*size,-.002F*size},{.022F*size,.008F*size,.027F*size},armor);
    b.box({0,y+.0165F*size,-.002F*size},{.014F*size,.001F*size,.021F*size},hull);
    b.box({0,y+.0172F*size,.009F*size},{.012F*size,.001F*size,.002F*size},amber,light);
    return std::move(b.mesh);
}
StructureMesh tunnel_scaffold(const SkywaySample& sample,f32 size) {
    Builder b;
    const auto radial=unit(sample.position),side=sample.side;
    const auto clearance=std::hypot(sample.position.x,sample.position.y,sample.position.z)-1.008F;
    const auto top=add(sample.position,mul(radial,-std::min(.008F*size,clearance*.5F)));
    const auto base=mul(radial,1.008F);
    const auto half=.014F*size;
    const auto top_a=add(top,mul(side,-half)),top_b=add(top,mul(side,half));
    const auto a=add(base,mul(side,-half*1.3F)),c=add(base,mul(side,half*1.3F));
    const auto forward=unit(cross(side,radial));
    // Beam cross-sections follow the scaffold frame, not world Y. A rigid
    // move around the planet then agrees with regenerating the same recipe.
    b.beam(a,top_a,.0038F*size,hull,forward);b.beam(c,top_b,.0038F*size,hull,forward);
    b.beam(top_a,top_b,.004F*size,armor,forward);
    b.beam(a,top_b,.002F*size,rib,forward);b.beam(c,top_a,.002F*size,rib,forward);
    for(auto foot:{a,c})b.box(foot,side,radial,forward,{.011F*size,.004F,.014F*size},armor);
    return std::move(b.mesh);
}
}
