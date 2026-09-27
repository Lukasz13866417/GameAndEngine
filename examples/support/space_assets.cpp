#include "space_assets.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>

namespace example::space {
namespace {
using namespace vng;
namespace vm = content::vmesh;
constexpr f32 pi = std::numbers::pi_v<f32>;
constexpr f32 degree = pi / 180;
Vec3 add(Vec3 a, Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec3 mul(Vec3 a, f32 s) { return {a.x*s,a.y*s,a.z*s}; }
f32 dot(Vec3 a, Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
f32 length(Vec3 a) { return std::sqrt(dot(a,a)); }
Vec3 unit(Vec3 a) { return mul(a,1/length(a)); }
Vec3 lerp(Vec3 a, Vec3 b, f32 t) { return add(a,mul(sub(b,a),t)); }
f32 smooth(f32 x) { x=std::clamp(x,0.F,1.F); return x*x*(3-2*x); }
// Any unit vector perpendicular to a unit direction.
Vec3 perpendicular(Vec3 d) { return unit(cross(d, std::abs(d.y)<.9F ? Vec3{0,1,0} : Vec3{1,0,0})); }

// Deterministic value noise in [-1, 1]; no global random state.
f32 lattice(i32 x, i32 y, i32 z, u32 seed) {
    u32 h = seed ^ (static_cast<u32>(x)*0x8da6b343U) ^ (static_cast<u32>(y)*0xd8163841U) ^
            (static_cast<u32>(z)*0xcb1ab31fU);
    h ^= h >> 13; h *= 0x5bd1e995U; h ^= h >> 15;
    return static_cast<f32>(h & 0xffffffU) / 8388607.5F - 1;
}
f32 noise(Vec3 p, u32 seed) {
    const auto fx=std::floor(p.x), fy=std::floor(p.y), fz=std::floor(p.z);
    const auto x=static_cast<i32>(fx), y=static_cast<i32>(fy), z=static_cast<i32>(fz);
    const auto fade=[](f32 t){ return t*t*t*(t*(t*6-15)+10); };
    const auto u=fade(p.x-fx), v=fade(p.y-fy), w=fade(p.z-fz);
    const auto at=[&](i32 i,i32 j,i32 k){ return lattice(x+i,y+j,z+k,seed); };
    const auto along=[&](i32 j,i32 k){ return std::lerp(at(0,j,k),at(1,j,k),u); };
    return std::lerp(std::lerp(along(0,0),along(1,0),v),std::lerp(along(0,1),along(1,1),v),w);
}
f32 fbm(Vec3 p, u32 seed, unsigned octaves) {
    f32 sum{}, amplitude{1}, total{};
    for (unsigned i=0;i<octaves;++i) {
        sum+=amplitude*noise(p,seed+i*101U); total+=amplitude;
        p=mul(p,2.03F); amplitude*=.5F;
    }
    return sum/total;
}
struct Random {
    u32 state;
    f32 next() { state=state*1664525U+1013904223U; return static_cast<f32>(state>>8)/16777216.F; }
    Vec3 direction() {
        const auto z=2*next()-1, a=2*pi*next(), r=std::sqrt(std::max(0.F,1-z*z));
        return {r*std::cos(a),z,r*std::sin(a)};
    }
};

// The structures share Earth's infrastructure palette and illustrated
// lighting: cool greys, near-black collars, and saturated lights at modest
// emission so they read as colour, not white, after tone mapping.
struct Material { Vec3 color; f32 emission{}; };
constexpr Material hull{{.16F,.19F,.21F}};
constexpr Material plating{{.12F,.17F,.20F}};
constexpr Material dark{{.05F,.07F,.09F}};
constexpr Material solar{{.02F,.04F,.09F}};
constexpr Material frame_metal{{.085F,.10F,.12F}};
constexpr Material warm_window{{1.F,.32F,.06F},2.2F};
constexpr Material cyan_light{{.02F,.55F,1.F},2.5F};
constexpr Material white_light{{.85F,.9F,1.F},2.5F};
constexpr Material red_light{{1.F,.03F,.02F},3.F};
constexpr Material green_light{{.05F,1.F,.2F},3.F};
constexpr Material amber_light{{1.F,.4F,.05F},2.4F};
// Small lamps seen from kilometres away: hotter, so bloom makes them points.
constexpr Material beacon_red{{1.F,.03F,.02F},6.F};
constexpr Material lamp_cyan{{.02F,.55F,1.F},4.F};
constexpr Material lamp_white{{.85F,.9F,1.F},4.F};

// Flat-shaded hard-surface geometry: every triangle owns its three vertices,
// with an outward normal chosen against a point known to be inside.
struct Builder {
    std::vector<f32> positions, normals, colors, emissions;
    std::vector<gfx::TriangleFace> faces;
    void vertex(Vec3 p, Vec3 n, Material m) {
        positions.insert(positions.end(),{p.x,p.y,p.z});
        normals.insert(normals.end(),{n.x,n.y,n.z});
        colors.insert(colors.end(),{m.color.x,m.color.y,m.color.z});
        emissions.push_back(m.emission);
    }
    void triangle(Vec3 a, Vec3 b, Vec3 c, Material m, Vec3 inside) {
        auto n=cross(sub(b,a),sub(c,a));
        const auto area=length(n);
        if (!(area>1e-12F)) return;
        n=mul(n,1/area);
        if (dot(n,sub(mul(add(add(a,b),c),1.F/3),inside))<0) { std::swap(b,c); n=mul(n,-1); }
        const auto first=static_cast<u32>(positions.size()/3);
        for (auto p : {a,b,c}) vertex(p,n,m);
        faces.emplace_back(first,first+1,first+2);
    }
    void quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Material m, Vec3 inside) {
        triangle(a,b,c,m,inside); triangle(a,c,d,m,inside);
    }
    // An oriented box: center, three unit axes and half extents.
    void box(Vec3 center, Vec3 x, Vec3 y, Vec3 z, Vec3 half, Material top, Material side) {
        const auto corner=[&](f32 i,f32 j,f32 k) {
            return add(center,add(add(mul(x,i*half.x),mul(y,j*half.y)),mul(z,k*half.z)));
        };
        quad(corner(-1,1,-1),corner(1,1,-1),corner(1,1,1),corner(-1,1,1),top,center);
        quad(corner(-1,-1,-1),corner(1,-1,-1),corner(1,-1,1),corner(-1,-1,1),side,center);
        quad(corner(-1,-1,-1),corner(1,-1,-1),corner(1,1,-1),corner(-1,1,-1),side,center);
        quad(corner(-1,-1,1),corner(1,-1,1),corner(1,1,1),corner(-1,1,1),side,center);
        quad(corner(-1,-1,-1),corner(-1,1,-1),corner(-1,1,1),corner(-1,-1,1),side,center);
        quad(corner(1,-1,-1),corner(1,1,-1),corner(1,1,1),corner(1,-1,1),side,center);
    }
    void box(Vec3 center, Vec3 half, Material top, Material side) {
        box(center,{1,0,0},{0,1,0},{0,0,1},half,top,side);
    }
    // A square beam between two points.
    void beam(Vec3 a, Vec3 b, f32 width, Material m) {
        const auto axis=unit(sub(b,a)), x=perpendicular(axis), y=cross(axis,x);
        box(mul(add(a,b),.5F),x,y,axis,{width*.5F,width*.5F,length(sub(b,a))*.5F},m,m);
    }
    // A capped prism with `sides` faces around the segment a-b.
    void prism(Vec3 a, Vec3 b, f32 radius, unsigned sides, Material side, Material cap, f32 phase = 0) {
        const auto axis=unit(sub(b,a)), x=perpendicular(axis), y=cross(axis,x);
        const auto inside=mul(add(a,b),.5F);
        const auto ring=[&](Vec3 c, unsigned i) {
            const auto t=phase+2*pi*static_cast<f32>(i)/static_cast<f32>(sides);
            return add(c,add(mul(x,radius*std::cos(t)),mul(y,radius*std::sin(t))));
        };
        for (unsigned i=0;i<sides;++i) {
            quad(ring(a,i),ring(a,i+1),ring(b,i+1),ring(b,i),side,inside);
            triangle(a,ring(a,i),ring(a,i+1),cap,inside);
            triangle(b,ring(b,i),ring(b,i+1),cap,inside);
        }
    }
    // A smooth dome over a base circle; normals follow the sphere.
    void dome(Vec3 center, Vec3 up, f32 radius, Material m, unsigned rings = 7, unsigned segments = 20) {
        const auto x=perpendicular(up), y=cross(up,x);
        const auto point=[&](unsigned r, unsigned s) {
            const auto elevation=.5F*pi*static_cast<f32>(r)/static_cast<f32>(rings);
            const auto azimuth=2*pi*static_cast<f32>(s)/static_cast<f32>(segments);
            return add(add(mul(x,std::cos(elevation)*std::cos(azimuth)),mul(y,std::cos(elevation)*std::sin(azimuth))),
                       mul(up,std::sin(elevation)));
        };
        for (unsigned r=0;r<rings;++r) for (unsigned s=0;s<segments;++s) {
            const std::array quad_points{point(r,s),point(r,s+1),point(r+1,s+1),point(r+1,s)};
            const auto first=static_cast<u32>(positions.size()/3);
            for (auto d : quad_points) vertex(add(center,mul(d,radius)),d,m);
            // Counter-clockwise from outside: azimuth increases around +up.
            faces.emplace_back(first,first+1,first+2);
            faces.emplace_back(first,first+2,first+3);
        }
    }
    vm::Document document(std::string name, std::string parts, bool lit = true) && {
        vm::Document result;
        result.metadata={{"name",std::move(name)},{"author","Original Vibe Engine procedural asset"},
            {"source/tool","examples/support/space_assets.cpp"},{"units","kilometres"},
            {"parts",std::move(parts)}};
        result.vertex_count=positions.size()/3;
        if (!lit) std::ranges::fill(normals,0.F);
        result.vertex_fields={{"position",{vm::ScalarType::Float32,3},std::move(positions)},
            {"normal",{vm::ScalarType::Float32,3},std::move(normals)},
            {"color/0",{vm::ScalarType::Float32,3},std::move(colors)},
            {"emission",{vm::ScalarType::Float32,1},std::move(emissions)}};
        result.faces=std::move(faces);
        return result;
    }
};

f32 chord_angle(Vec3 a, Vec3 b) { return length(sub(a,b)); } // ~angle for small separations

// Positions from `begin` to `end`, `step` apart across [core_begin, core_end]
// and growing geometrically beyond it up to `coarsest`. Sorted, with both ends.
std::vector<f32> spaced(f32 begin, f32 end, f32 core_begin, f32 core_end, f32 step, f32 growth, f32 coarsest) {
    const auto grow=[&](f32 from, f32 limit, f32 sign) {
        std::vector<f32> side;
        f32 a=from, s=step;
        while ((limit-a)*sign>1e-4F) {
            s=std::min(s*growth,coarsest);
            const auto remaining=(limit-a)*sign;
            // Spread the final stretch evenly instead of ending with a sliver.
            if (remaining<2*s) { const auto n=std::max(1.F,std::round(remaining/s)); s=remaining/n; }
            a+=sign*s;
            if ((limit-a)*sign<1e-4F) a=limit;
            side.push_back(a);
        }
        return side;
    };
    const auto count=std::max(1,static_cast<int>(std::lround((core_end-core_begin)/step)));
    auto result=grow(core_begin,begin,-1);
    std::ranges::reverse(result);
    for (int i=0;i<=count;++i) result.push_back(core_begin+(core_end-core_begin)*static_cast<f32>(i)/static_cast<f32>(count));
    const auto after=grow(core_end,end,1);
    result.insert(result.end(),after.begin(),after.end());
    return result;
}
constexpr unsigned bucket_columns=180, bucket_rows=90;
f32 longitude(Vec3 d) { return std::atan2(d.x,d.z)/degree; }
f32 latitude(Vec3 d) { return std::asin(std::clamp(d.y,-1.F,1.F))/degree; }
// Mesh spacing, in kilometres, of the grid that shows each crater tier.
constexpr std::array<f32,3> tier_spacing{5,5,.6F};
} // namespace

MoonSurface::MoonSurface(MoonCorridor corridor, Vec3 near_side, std::vector<MoonSite> sites)
    : corridor_(corridor), center_(unit(corridor.origin)), near_side_(unit(near_side)), sites_(std::move(sites)) {
    along_=unit(sub(corridor.along,mul(center_,dot(corridor.along,center_))));
    across_=cross(center_,along_);
    // Maria: the familiar near-side pattern, in latitude and longitude as
    // seen from Earth (north up, east to the right). Overlapping basins make
    // the large irregular seas; their edges are warped in relief().
    const auto north=unit(sub(Vec3{0,1,0},mul(near_side_,near_side_.y))), east=cross(north,near_side_);
    const auto at=[&](f32 lat, f32 lon) {
        const auto a=lat*degree, o=lon*degree;
        return unit(add(mul(add(mul(near_side_,std::cos(o)),mul(east,std::sin(o))),std::cos(a)),mul(north,std::sin(a))));
    };
    struct Sea { f32 lat, lon, radius; };
    for (const auto& sea : std::array{
             Sea{33,-16,17},Sea{28,17,11},Sea{8,31,12},Sea{2,25,8},Sea{17,59,8},Sea{-8,51,10},Sea{-2,47,6},
             Sea{-15,35,5.5F},Sea{-21,-17,10},Sea{18,-57,16},Sea{5,-50,14},Sea{30,-45,12},Sea{-5,-40,10},
             Sea{-24,-39,6.5F},Sea{56,-10,6},Sea{57,10,6},Sea{55,30,5},Sea{13,4,5},Sea{8,-22,7}})
        basins_.push_back({at(sea.lat,sea.lon),1.2F*sea.radius*degree});
    Random random{1969U};
    const auto add_crater=[&](Vec3 center, f32 km, f32 depth, Tier tier, f32 fresh = .15F) {
        craters_.push_back({center,km/radius,depth,std::max(.13F,tier_spacing[static_cast<std::size_t>(tier)]/km),
                            random.next()<fresh,tier});
    };
    // Global craters: angular radius 0.7-7 degrees with a steep size distribution.
    for (unsigned i=0;i<220;++i) {
        const auto r=std::min(7.F,.7F*std::pow(std::max(random.next(),.01F),-.5F))*degree;
        const auto km=r*radius;
        add_crater(random.direction(),km,std::min(.12F*km,1.2F+.035F*km),Tier::global);
    }
    const auto scatter=[&](const MoonCorridor::Span& span) {
        const auto along=span.begin+(span.end-span.begin)*random.next();
        return direction(along,(2*random.next()-1)*span.half_width);
    };
    // The patch: craters 10-45 km in radius, enough to saturate the ground.
    for (unsigned i=0;i<300;++i) {
        const auto center=scatter(corridor_.patch);
        const auto km=std::min(45.F,10.F*std::pow(std::max(random.next(),.002F),-.6F));
        add_crater(center,km,std::min(.16F*km,1.F+.045F*km),Tier::patch);
    }
    // The strip: bowls 1.2-10 km in radius, deep enough to read from a few
    // kilometres up.
    for (unsigned i=0;i<650;++i) {
        const auto center=scatter(corridor_.strip);
        const auto km=std::min(10.F,1.2F*std::pow(std::max(random.next(),.002F),-.55F));
        add_crater(center,km,std::min(.28F*km,.9F+.05F*km),Tier::strip,.08F);
    }
    // Middling hollows, 4-9 km in radius, in a wider band around the strip:
    // the patch shows them as broad dimples, so the ground roughens toward
    // the flight line gradually rather than all at once at the strip.
    const auto& strip=corridor_.strip;
    for (unsigned i=0;i<220;++i) {
        const auto along=strip.begin-40+(strip.end-strip.begin+120)*random.next();
        const auto across=(2*random.next()-1)*(strip.half_width+45);
        const auto km=4+5*random.next();
        add_crater(direction(along,across),km,std::min(.16F*km,1.F+.045F*km),Tier::patch,.1F);
    }
    buckets_.resize(bucket_columns*bucket_rows);
    for (u32 i=0;i<craters_.size();++i) {
        const auto& c=craters_[i];
        const auto reach=3.3F*c.radius/degree+.1F;
        const auto lat=latitude(c.center), lon=longitude(c.center);
        const auto row0=static_cast<int>(std::floor((std::max(-90.F,lat-reach)+90)/2));
        const auto row1=static_cast<int>(std::floor((std::min(89.99F,lat+reach)+90)/2));
        const auto polar=std::abs(lat)+reach>84;
        const auto half=polar ? 180.F : reach/std::cos((std::abs(lat)+reach)*degree);
        for (int row=row0;row<=row1;++row) {
            if (polar || half>=180) {
                for (unsigned col=0;col<bucket_columns;++col) buckets_[static_cast<unsigned>(row)*bucket_columns+col].push_back(i);
                continue;
            }
            for (int col=static_cast<int>(std::floor((lon-half+180)/2));col<=static_cast<int>(std::floor((lon+half+180)/2));++col) {
                const auto wrapped=static_cast<unsigned>((col%static_cast<int>(bucket_columns)+static_cast<int>(bucket_columns))%static_cast<int>(bucket_columns));
                buckets_[static_cast<unsigned>(row)*bucket_columns+wrapped].push_back(i);
            }
        }
    }
    for (auto& site : sites_) site.direction=unit(site.direction);
    for (const auto& site : sites_) site_levels_.push_back(relief(site.direction).height);
}

Vec3 MoonSurface::direction(f32 along, f32 across) const {
    const auto a=along/radius, b=across/radius;
    return unit(add(mul(add(mul(center_,std::cos(a)),mul(along_,std::sin(a))),std::cos(b)),mul(across_,std::sin(b))));
}

Vec2 MoonSurface::coordinates(Vec3 d) const {
    return {std::atan2(dot(d,along_),dot(d,center_))*radius,std::asin(std::clamp(dot(d,across_),-1.F,1.F))*radius};
}

f32 MoonSurface::inside(const MoonCorridor::Span& span, Vec3 d) const {
    const auto c=coordinates(d);
    return std::min({c.x-span.begin,span.end-c.x,span.half_width-std::abs(c.y)});
}

std::size_t MoonSurface::bucket(Vec3 d) const {
    const auto row=std::min(bucket_rows-1,static_cast<unsigned>(std::max(0.F,(latitude(d)+90)/2)));
    const auto col=std::min(bucket_columns-1,static_cast<unsigned>(std::max(0.F,(longitude(d)+180)/2)));
    return static_cast<std::size_t>(row)*bucket_columns+col;
}

MoonSurface::Sample MoonSurface::relief(Vec3 d) const {
    Sample out{};
    // Seas: flat, low lava plains with irregular, noise-warped shores.
    const auto warp=.35F*fbm(mul(d,radius/260),31U,3);
    f32 sea{};
    for (const auto& basin : basins_) {
        const auto a=chord_angle(d,basin.center)/basin.radius+warp;
        if (a>1.5F) continue;
        sea=std::max(sea,1-smooth((a-.65F)/.55F));
    }
    out.height-=1.1F*sea;
    out.mare=sea;
    // Each finer tier fades in well inside the grid that resolves it; the
    // strip's detail fades out gradually away from the flight line, so from
    // high up it reads as rougher ground rather than a band.
    const auto& strip=corridor_.strip;
    const auto c=coordinates(d);
    const auto wander=6*noise(mul(d,radius/25),77U); // no straight edge to the rough ground
    const auto strip_weight=(1-smooth((std::abs(c.y)+wander-5)/(strip.half_width-10)))*
                            smooth((c.x-strip.begin-3)/25)*smooth((strip.end-c.x-3)/25);
    const std::array weights{1.F,smooth((inside(corridor_.patch,d)-15)/60),strip_weight};
    for (const auto i : buckets_[bucket(d)]) {
        const auto& c=craters_[i];
        const auto w=weights[static_cast<std::size_t>(c.tier)];
        if (w<=0) continue;
        const auto separation=chord_angle(d,c.center);
        if (separation>3.2F*c.radius) continue;
        const auto x=separation/c.radius;
        f32 value{};
        if (x<1) value-=c.depth*std::pow(1-x*x,.75F);
        value+=.22F*c.depth*std::exp(-((x-1)/c.soft)*((x-1)/c.soft));
        value+=.12F*c.depth*(x<1 ? smooth((x-.8F)/.2F) : 1/(x*x*x));
        // A low, broad central peak: it may catch first light, but never as a spike.
        if (c.radius*radius>25) value+=.14F*c.depth*std::exp(-(x/.26F)*(x/.26F));
        out.height+=w*value;
        if (x<1) out.floor=std::max(out.floor,w*(1-x*x));
        // Fresh ejecta only where the strip's grid resolves it; on coarser
        // grids it breaks up into sparkle along the limb.
        if (c.fresh && c.tier==Tier::strip && x>.9F) out.bright=std::max(out.bright,w*std::exp(-((x-1.5F)/.6F)*((x-1.5F)/.6F)));
    }
    // Rolling ground everywhere; finer undulation only where the strip's
    // grid resolves it, so coarser grids never alias it into ripples.
    const auto p=mul(d,radius);
    out.height+=.21F*noise(mul(p,1.F/22),11U)+.11F*noise(mul(p,1.F/10.8F),112U);
    if (weights[2]>0)
        out.height+=weights[2]*(.05F*noise(mul(p,1.F/5.3F),213U)+.025F*noise(mul(p,1.F/2.6F),314U));
    return out;
}

f32 MoonSurface::height(Vec3 direction) const {
    const auto d=unit(direction);
    auto h=relief(d).height;
    for (std::size_t i=0;i<sites_.size();++i) {
        const auto distance=chord_angle(d,sites_[i].direction)*radius;
        if (distance>sites_[i].radius_km*2) continue;
        h=std::lerp(site_levels_[i],h,smooth((distance-sites_[i].radius_km)/sites_[i].radius_km));
    }
    return h;
}

Vec3 MoonSurface::point(Vec3 direction, f32 altitude) const {
    const auto d=unit(direction);
    return mul(d,radius+height(d)+altitude);
}

Vec3 MoonSurface::albedo(Vec3 direction) const {
    const auto d=unit(direction);
    const auto sample=relief(d);
    const auto p=mul(d,radius);
    // Warm-grey highlands and darker, cooler seas at about half their
    // brightness, as the near side looks to the eye.
    const auto highland=1+.08F*fbm(mul(p,1.F/70),5U,3)+.04F*fbm(mul(p,1.F/9),7U,2);
    const Vec3 highlands{.50F,.43F,.37F}, seas{.21F,.21F,.22F};
    auto color=lerp(mul(highlands,highland),mul(seas,1+.15F*fbm(mul(p,1.F/40),9U,2)),sample.mare);
    color=mul(color,1-.12F*sample.floor);
    return lerp(color,{.80F,.80F,.80F},.5F*sample.bright);
}

vm::Document moon_mesh(const MoonSurface& surface) {
    const auto& corridor=surface.corridor();
    const auto km=[](f32 degrees){ return degrees*degree*MoonSurface::radius; };
    // A grid over kilometre samples along and across the corridor, rows
    // across and columns along, each cell split from (r, c) to (r+1, c+1).
    struct Piece {
        std::vector<f32> along, across;
        std::vector<Vec3> points, normals;
        [[nodiscard]] u32 index(std::size_t r, std::size_t c) const { return static_cast<u32>(r*along.size()+c); }
        // The piece's own piecewise-linear surface (and normals) at a sample.
        [[nodiscard]] Vec3 at(const std::vector<Vec3>& values, f32 a, f32 b) const {
            const auto cell=[](const std::vector<f32>& s, f32 x) {
                return static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(std::ranges::upper_bound(s,x)-s.begin(),1,
                    static_cast<std::ptrdiff_t>(s.size())-1)-1);
            };
            const auto c=cell(along,a), r=cell(across,b);
            const auto u=(a-along[c])/(along[c+1]-along[c]), v=(b-across[r])/(across[r+1]-across[r]);
            const auto& p00=values[index(r,c)]; const auto& p10=values[index(r,c+1)];
            const auto& p01=values[index(r+1,c)]; const auto& p11=values[index(r+1,c+1)];
            return u>=v ? add(p00,add(mul(sub(p10,p00),u),mul(sub(p11,p10),v)))
                        : add(p00,add(mul(sub(p11,p01),u),mul(sub(p01,p00),v)));
        }
        [[nodiscard]] Vec3 point(f32 a, f32 b) const { return at(points,a,b); }
        [[nodiscard]] Vec3 normal(f32 a, f32 b) const { return unit(at(normals,a,b)); }
    };
    std::vector<Vec3> points, colors, normals;
    std::vector<gfx::TriangleFace> faces;
    // Adds a piece's vertices and faces, leaving out cells inside `hole`
    // (along and across bounds on its own grid lines) for the next piece.
    const auto emit=[&](Piece& piece, std::array<f32,4> hole, bool wraps) {
        const auto first=static_cast<u32>(points.size());
        const auto columns=piece.along.size(), rows=piece.across.size();
        const auto spans=wraps ? columns : columns-1;
        const auto cell=[&](std::size_t r, std::size_t c) {
            return std::array{piece.index(r,c),piece.index(r,(c+1)%columns),piece.index(r+1,(c+1)%columns),piece.index(r+1,c)};
        };
        // Smooth normals from every cell, the hole's included, so the next
        // piece's rim can blend into them.
        piece.normals.assign(piece.points.size(),{});
        for (std::size_t r=0;r+1<rows;++r) for (std::size_t c=0;c<spans;++c) {
            const auto q=cell(r,c);
            for (const auto& t : {std::array{q[0],q[1],q[2]},std::array{q[0],q[2],q[3]}}) {
                const auto& a=piece.points[t[0]]; const auto& b=piece.points[t[1]]; const auto& d=piece.points[t[2]];
                const auto n=cross(sub(b,a),sub(d,a));
                for (const auto i : t) piece.normals[i]=add(piece.normals[i],n);
            }
        }
        for (auto& n : piece.normals) n=unit(n);
        const auto inside=[&](f32 x, f32 low, f32 high){ return x>=low-1e-3F && x<=high+1e-3F; };
        for (std::size_t r=0;r+1<rows;++r) for (std::size_t c=0;c<spans;++c) {
            if (c+1<columns && inside(piece.along[c],hole[0],hole[1]) && inside(piece.along[c+1],hole[0],hole[1]) &&
                inside(piece.across[r],hole[2],hole[3]) && inside(piece.across[r+1],hole[2],hole[3])) continue;
            const auto q=cell(r,c);
            for (const auto& t : {std::array{q[0],q[1],q[2]},std::array{q[0],q[2],q[3]}}) {
                auto [a,b,d]=t;
                const auto n=cross(sub(piece.points[b],piece.points[a]),sub(piece.points[d],piece.points[a]));
                if (dot(n,piece.points[a])<0) std::swap(b,d);
                faces.emplace_back(first+a,first+b,first+d);
            }
        }
        for (std::size_t i=0;i<piece.points.size();++i) {
            const auto a=piece.along[i%columns], b=piece.across[i/columns];
            colors.push_back(surface.albedo(surface.direction(a,b)));
        }
        points.insert(points.end(),piece.points.begin(),piece.points.end());
        normals.insert(normals.end(),piece.normals.begin(),piece.normals.end());
    };
    // A finer piece inside `coarse`: its rim lies exactly on the coarse
    // triangles, and it eases onto the true surface over `blend` kilometres.
    const auto nested=[&](const Piece& coarse, std::vector<f32> along, std::vector<f32> across, f32 blend) {
        Piece piece{std::move(along),std::move(across),{},{}};
        const auto a0=piece.along.front(), a1=piece.along.back(), b0=piece.across.front(), b1=piece.across.back();
        for (const auto b : piece.across) for (const auto a : piece.along) {
            const auto w=smooth(std::min({a-a0,a1-a,b-b0,b1-b})/blend);
            piece.points.push_back(lerp(coarse.point(a,b),surface.point(surface.direction(a,b)),w));
        }
        return piece;
    };
    // Rims blend their normals into the coarse piece's the same way.
    const auto blend_normals=[&](Piece& piece, const Piece& coarse, f32 blend) {
        const auto a0=piece.along.front(), a1=piece.along.back(), b0=piece.across.front(), b1=piece.across.back();
        for (std::size_t i=0;i<piece.normals.size();++i) {
            const auto a=piece.along[i%piece.along.size()], b=piece.across[i/piece.along.size()];
            const auto w=smooth(std::min({a-a0,a1-a,b-b0,b1-b})/blend);
            piece.normals[i]=unit(lerp(coarse.normal(a,b),piece.normals[i],w));
        }
    };
    // The patch and strip, snapped outward onto the grid lines of the piece
    // they sit in: every 2.5 degrees, then every 5 km.
    const auto snap=[](const MoonCorridor::Span& span, f32 step) {
        return MoonCorridor::Span{std::floor(span.begin/step)*step,std::ceil(span.end/step)*step,
                                  std::ceil(span.half_width/step)*step};
    };
    const auto patch=snap(corridor.patch,km(2.5F)), strip=snap(corridor.strip,5);

    // Everywhere: rows every 2.5 degrees in the corridor's own frame, a
    // regular grid of 144 columns out to 57.5 degrees either side (the patch
    // sits in it). Toward the poles each ring halves its columns and is zipped
    // onto the finer one, so the caps close without a fan of slivers.
    Piece global;
    for (int i=0;i<144;++i) global.along.push_back(km(-180+2.5F*static_cast<f32>(i)));
    for (int i=0;i<47;++i) global.across.push_back(km(-57.5F+2.5F*static_cast<f32>(i)));
    for (const auto b : global.across) for (const auto a : global.along) global.points.push_back(surface.point(surface.direction(a,b)));
    emit(global,{patch.begin,patch.end,-patch.half_width,patch.half_width},true);
    {
        const auto first_cap=static_cast<u32>(points.size());
        const auto first_face=faces.size();
        const auto vertex=[&](Vec3 d) {
            points.push_back(surface.point(d)); colors.push_back(surface.albedo(d)); normals.push_back({});
            return static_cast<u32>(points.size()-1);
        };
        const auto ring=[&](f32 b, u32 columns) {
            std::vector<u32> result;
            for (u32 c=0;c<columns;++c)
                result.push_back(vertex(surface.direction(km(-180+360.F*static_cast<f32>(c)/static_cast<f32>(columns)),km(b))));
            return result;
        };
        const auto zip=[&](const std::vector<u32>& fine, const std::vector<u32>& coarse) {
            const auto n=coarse.size();
            for (std::size_t c=0;c<n;++c) {
                if (fine.size()==n) {
                    faces.emplace_back(fine[c],fine[(c+1)%n],coarse[(c+1)%n]);
                    faces.emplace_back(fine[c],coarse[(c+1)%n],coarse[c]);
                    continue;
                }
                const auto f0=fine[2*c], f1=fine[2*c+1], f2=fine[(2*c+2)%fine.size()];
                faces.emplace_back(f0,f1,coarse[c]);
                faces.emplace_back(f1,f2,coarse[(c+1)%n]);
                faces.emplace_back(f1,coarse[(c+1)%n],coarse[c]);
            }
        };
        for (const auto sign : {-1.F,1.F}) {
            std::vector<u32> previous;
            const auto edge=sign<0 ? 0 : global.across.size()-1;
            for (std::size_t c=0;c<global.along.size();++c) previous.push_back(global.index(edge,c));
            for (int i=0;i<12;++i) {
                const auto b=60+2.5F*static_cast<f32>(i);
                auto next=ring(sign*b,b<75 ? 72U : b<82.5F ? 36U : b<87.5F ? 18U : 9U);
                zip(previous,next);
                previous=std::move(next);
            }
            const auto pole=vertex(surface.direction(0,km(sign*90)));
            for (std::size_t c=0;c<previous.size();++c) faces.emplace_back(previous[c],previous[(c+1)%previous.size()],pole);
        }
        // Outward normals from the cap faces; the regular grid's edge rows
        // already hold their own side's, and take an equal share of the caps'.
        std::vector<Vec3> caps(points.size());
        for (auto f=first_face;f<faces.size();++f) {
            const auto [a,b,c]=faces[f].vertices;
            auto n=cross(sub(points[b],points[a]),sub(points[c],points[a]));
            if (dot(n,points[a])<0) n=mul(n,-1);
            for (const auto i : {a,b,c}) caps[i]=add(caps[i],n);
        }
        for (u32 i=0;i<points.size();++i) {
            if (i>=first_cap) normals[i]=unit(caps[i]);
            else if (length(caps[i])>0) normals[i]=unit(add(normals[i],unit(caps[i])));
        }
    }
    // The patch: 5 km, coarsening toward its rim.
    const auto core=[](f32 low, f32 high, f32 margin){ return std::pair{std::ceil((low+margin)/5)*5,std::floor((high-margin)/5)*5}; };
    const auto [pa0,pa1]=core(patch.begin,patch.end,60);
    const auto [pb0,pb1]=core(-patch.half_width,patch.half_width,60);
    auto patch_piece=nested(global,spaced(patch.begin,patch.end,pa0,pa1,5,1.2F,15),
                            spaced(-patch.half_width,patch.half_width,pb0,pb1,5,1.2F,15),40);
    // The strip: 0.6 km along the flight line, coarsening to 2.5 km at its edges.
    auto strip_piece=nested(patch_piece,spaced(strip.begin,strip.end,strip.begin+15,strip.end-15,.6F,1.15F,2.5F),
                            spaced(-strip.half_width,strip.half_width,-6,6,.6F,1.15F,2.5F),8);
    emit(patch_piece,{strip.begin,strip.end,-strip.half_width,strip.half_width},false);
    // The patch's normals are final once emitted; blend its rim into the
    // global piece's, then the strip's into the patch's.
    {
        auto blended=patch_piece;
        blend_normals(blended,global,40);
        std::copy(blended.normals.begin(),blended.normals.end(),normals.end()-static_cast<std::ptrdiff_t>(blended.normals.size()));
        patch_piece.normals=blended.normals;
    }
    emit(strip_piece,{1,-1,1,-1},false);
    blend_normals(strip_piece,patch_piece,8);
    std::copy(strip_piece.normals.begin(),strip_piece.normals.end(),normals.end()-static_cast<std::ptrdiff_t>(strip_piece.normals.size()));

    vm::Document result;
    std::vector<f32> position, normal, color, emission;
    for (std::size_t i=0;i<points.size();++i) {
        const auto& n=normals[i];
        position.insert(position.end(),{points[i].x,points[i].y,points[i].z});
        normal.insert(normal.end(),{n.x,n.y,n.z});
        color.insert(color.end(),{colors[i].x,colors[i].y,colors[i].z});
        // Earthshine: the near side glows faintly at night; the night-emission
        // convention hides it wherever the sun is up.
        emission.push_back(.10F*std::max(0.F,dot(n,surface.near_side())));
    }
    result.metadata={{"name","MOON / Selene"},{"author","Original Vibe Engine procedural asset"},
        {"source/tool","examples/support/space_assets.cpp"},{"units","kilometres"},
        {"shading","Nested grids stitched without seams: 2.5 degrees globally, 5 km in the corridor patch, "
                   "0.6 km in the low-level strip; near-side maria, craters in three sizes, fresh ejecta, earthshine"},
        {"render/lighting","regolith"}};
    result.vertex_count=points.size();
    result.vertex_fields={{"position",{vm::ScalarType::Float32,3},std::move(position)},
        {"normal",{vm::ScalarType::Float32,3},std::move(normal)},
        {"color/0",{vm::ScalarType::Float32,3},std::move(color)},
        {"emission",{vm::ScalarType::Float32,1},std::move(emission)}};
    result.faces=std::move(faces);
    return result;
}

vm::Document lunar_base(const MoonSurface& surface, Vec3 site, Vec3 rail_heading) {
    Builder b;
    const auto up=unit(site);
    const auto east=unit(sub(rail_heading,mul(up,dot(rail_heading,up)))), north=cross(up,east);
    const auto ground=[&](f32 e, f32 n) {
        const auto d=unit(add(up,add(mul(east,e/MoonSurface::radius),mul(north,n/MoonSurface::radius))));
        return surface.point(d);
    };
    const auto local_up=[&](Vec3 p){ return unit(p); };
    // Habitat domes linked by pressurized tubes, with glowing window bands.
    struct Dome { f32 e, n, radius; };
    constexpr std::array domes{Dome{0,0,.85F},Dome{1.9F,.6F,.55F},Dome{-1.7F,.9F,.6F},
        Dome{.4F,-1.9F,.5F},Dome{-1.2F,-1.5F,.42F},Dome{2.4F,-1.3F,.38F}};
    for (const auto& d : domes) {
        const auto center=ground(d.e,d.n);
        const auto normal=local_up(center);
        b.dome(add(center,mul(normal,-.05F)),normal,d.radius,{{.30F,.34F,.37F}});
        // A thin band of lit windows just outside the dome shell.
        const auto x=perpendicular(normal), y=cross(normal,x);
        constexpr unsigned windows=24;
        for (unsigned i=0;i<windows;++i) {
            if (i%4==3) continue;
            const auto t0=2*pi*static_cast<f32>(i)/windows, t1=t0+2*pi/windows*.7F;
            const auto at=[&](f32 t, f32 h) {
                const auto r=d.radius*1.012F*std::cos(h);
                return add(center,add(add(mul(x,r*std::cos(t)),mul(y,r*std::sin(t))),mul(normal,d.radius*1.012F*std::sin(h)-.05F)));
            };
            b.quad(at(t0,.30F),at(t1,.30F),at(t1,.40F),at(t0,.40F),warm_window,center);
        }
    }
    for (const auto& [i,j] : std::array{std::pair{0,1},std::pair{0,2},std::pair{0,3},std::pair{3,4},std::pair{1,5}}) {
        const auto a=ground(domes[i].e,domes[i].n), c=ground(domes[j].e,domes[j].n);
        const auto lift=[&](Vec3 p){ return add(p,mul(local_up(p),.12F)); };
        b.prism(lift(a),lift(c),.09F,6,plating,plating);
    }
    // Landing pads with cyan edge lights.
    for (const auto& [e,n] : std::array{std::pair{5.F,1.5F},std::pair{5.5F,-2.2F},std::pair{-4.8F,-.4F}}) {
        const auto center=ground(e,n), normal=local_up(center);
        const auto x=perpendicular(normal), y=cross(normal,x);
        std::array<Vec3,6> rim{};
        for (unsigned i=0;i<6;++i) {
            const auto t=pi/3*static_cast<f32>(i);
            rim[i]=add(center,add(mul(x,1.05F*std::cos(t)),mul(y,1.05F*std::sin(t))));
        }
        const auto top=mul(normal,.06F), inside=add(center,mul(normal,.03F));
        for (unsigned i=0;i<6;++i) {
            const auto j=(i+1)%6;
            b.triangle(add(center,top),add(rim[i],top),add(rim[j],top),{{.11F,.12F,.13F}},inside);
            b.quad(rim[i],rim[j],add(rim[j],top),add(rim[i],top),plating,inside);
            for (const auto f : {0.F,.5F}) {
                const auto p=add(lerp(rim[i],rim[j],f),mul(normal,.07F));
                b.box(p,x,normal,y,{.02F,.015F,.02F},lamp_cyan,lamp_cyan);
            }
        }
    }
    // Comms towers with red beacons.
    for (const auto& [e,n,h] : std::array{std::tuple{-3.F,2.6F,1.4F},std::tuple{3.2F,2.8F,1.1F},std::tuple{-.8F,-3.6F,.9F}}) {
        const auto foot=ground(e,n), normal=local_up(foot);
        const auto head=add(foot,mul(normal,h));
        b.prism(foot,head,.05F,4,frame_metal,frame_metal);
        b.box(head,perpendicular(normal),normal,cross(normal,perpendicular(normal)),{.025F,.025F,.025F},beacon_red,beacon_red);
    }
    // Mass driver: a launch ramp rising from the site along a great circle,
    // with gantries every 2.5 km. It keeps its own grade over the terrain;
    // pylons reach down to the ground wherever that lies below it.
    constexpr f32 rail_length=56;
    const auto level=MoonSurface::radius+surface.height(up);
    Vec3 previous_left{}, previous_right{};
    for (f32 s=7;s<=rail_length+1e-3F;s+=1) {
        const auto normal=unit(add(up,mul(east,s/MoonSurface::radius)));
        const auto base=surface.point(normal);
        const auto center=mul(normal,level+.22F+.05F*(s-7));
        const auto left=add(center,mul(north,.12F)), right=add(center,mul(north,-.12F));
        if (s>7) {
            b.beam(previous_left,left,.05F,frame_metal);
            b.beam(previous_right,right,.05F,frame_metal);
        }
        previous_left=left; previous_right=right;
        b.beam(base,center,.05F,plating);
        if (static_cast<int>(std::lround(s*2))%5==0) {
            const auto top=add(center,mul(normal,.34F));
            const auto l=add(top,mul(north,.26F)), r=add(top,mul(north,-.26F));
            const auto bl=add(center,mul(north,.26F)), br=add(center,mul(north,-.26F));
            b.beam(bl,l,.045F,frame_metal); b.beam(br,r,.045F,frame_metal); b.beam(l,r,.045F,frame_metal);
            b.box(add(top,mul(normal,.03F)),east,normal,north,{.025F,.015F,.025F},lamp_white,lamp_white);
            b.box(bl,east,normal,north,{.02F,.02F,.02F},lamp_cyan,lamp_cyan);
            b.box(br,east,normal,north,{.02F,.02F,.02F},lamp_cyan,lamp_cyan);
        }
    }
    // Low processing sheds between the domes and the rail.
    for (const auto& [e,n,w] : std::array{std::tuple{4.F,-.2F,.5F},std::tuple{3.3F,.9F,.35F},std::tuple{-3.2F,-2.3F,.45F}}) {
        const auto p=ground(e,n), normal=local_up(p);
        b.box(add(p,mul(normal,.12F)),east,normal,north,{w,.14F,w*.6F},hull,plating);
        b.box(add(p,add(mul(normal,.2F),mul(north,w*.61F))),east,normal,north,{w*.8F,.03F,.01F},warm_window,warm_window);
    }
    auto result=std::move(b).document("LUNAR / Serenity mining works","domes, tubes, landing pads, towers, mass driver");
    result.metadata["render/lighting"]="illustrated";
    return result;
}

vm::Document gateway_station() {
    Builder b;
    // Hub spindle along X.
    b.prism({-9,0,0},{9,0,0},.75F,6,hull,plating,pi/6);
    for (const auto x : {-7.5F,-4.F,4.F,7.5F}) b.prism({x-.25F,0,0},{x+.25F,0,0},.9F,6,dark,dark,pi/6);
    // Window strips along three faces.
    for (unsigned face=0;face<6;face+=2) {
        const auto t=pi/3*static_cast<f32>(face)+pi/6+pi/6;
        const Vec3 n{0,std::cos(t),std::sin(t)}, side{0,-std::sin(t),std::cos(t)};
        for (f32 x=-6.8F;x<6.9F;x+=1.1F) {
            if (std::abs(x)<1.2F) continue;
            const auto c=add(Vec3{x,0,0},mul(n,.67F));
            b.box(c,{1,0,0},n,side,{.38F,.02F,.07F},warm_window,warm_window);
        }
    }
    // Docking arms near the +X end with berth lights.
    for (unsigned i=0;i<4;++i) {
        const auto t=pi/2*static_cast<f32>(i)+pi/4;
        const Vec3 out{0,std::cos(t),std::sin(t)};
        const auto root=add(Vec3{5.8F,0,0},mul(out,.7F)), tip=add(Vec3{5.8F,0,0},mul(out,2.6F));
        b.prism(root,tip,.16F,6,plating,plating);
        b.prism(tip,add(tip,mul(out,.25F)),.3F,8,plating,dark);
        b.prism(add(tip,mul(out,.25F)),add(tip,mul(out,.27F)),.31F,8,cyan_light,dark);
        b.box(add(tip,mul(out,.3F)),{1,0,0},out,cross({1,0,0},out),{.05F,.05F,.05F},white_light,white_light);
    }
    // Solar wings on a truss beyond the -X end.
    for (const auto side : {-1.F,1.F}) {
        b.beam({-8.2F,0,side*.8F},{-8.2F,0,side*9.F},.18F,frame_metal);
        for (unsigned panel=0;panel<3;++panel) {
            const auto z0=side*(1.6F+2.5F*static_cast<f32>(panel)), z1=side*(3.8F+2.5F*static_cast<f32>(panel));
            b.box({-8.2F,0,(z0+z1)*.5F},{1.2F,.02F,std::abs(z1-z0)*.5F},solar,frame_metal);
        }
        b.box({-8.2F,0,side*9.1F},{.08F,.08F,.08F},side<0 ? red_light : green_light,side<0 ? red_light : green_light);
    }
    // Mass-driver catcher beyond the +X end. Payloads flung from the lunar
    // mass driver fall into a wide funnel that faces the Moon (+X), guided in
    // by lights round its mouth, and are slowed in the receiving bay at its
    // throat; unloaded pods wait in racks beside the boom.
    constexpr f32 boom_end=12.4F, throat=15.8F, mouth=20.2F, throat_radius=1.4F, mouth_radius=3.4F;
    // A point at radius r and angle t about X: t=0 is +Y, t=pi/2 is +Z.
    const auto around=[](f32 x, f32 r, f32 t){ return Vec3{x,r*std::cos(t),r*std::sin(t)}; };
    // A band of facets about X from (x0,r0) to (x1,r1), `depth` thick outward.
    const auto band=[&](f32 x0, f32 r0, f32 x1, f32 r1, f32 depth, unsigned segments, Material outer, Material inner,
                        bool ends, f32 phase = 0) {
        for (unsigned i=0;i<segments;++i) {
            const auto t0=phase+2*pi*static_cast<f32>(i)/static_cast<f32>(segments);
            const auto t1=phase+2*pi*static_cast<f32>(i+1)/static_cast<f32>(segments);
            const auto inside=around((x0+x1)*.5F,(r0+r1+depth)*.5F,(t0+t1)*.5F);
            b.quad(around(x0,r0+depth,t0),around(x0,r0+depth,t1),around(x1,r1+depth,t1),around(x1,r1+depth,t0),outer,inside);
            b.quad(around(x0,r0,t0),around(x0,r0,t1),around(x1,r1,t1),around(x1,r1,t0),inner,inside);
            if (!ends) continue;
            b.quad(around(x0,r0,t0),around(x0,r0,t1),around(x0,r0+depth,t1),around(x0,r0+depth,t0),outer,inside);
            b.quad(around(x1,r1,t0),around(x1,r1,t1),around(x1,r1+depth,t1),around(x1,r1+depth,t0),outer,inside);
        }
    };
    // A strip of rectangular section from a to c, standing `height` off the
    // surface toward `out`.
    const auto rib=[&](Vec3 a, Vec3 c, Vec3 out, f32 width, f32 height, Material top, Material side) {
        const auto along=unit(sub(c,a)), up=unit(sub(out,mul(along,dot(out,along)))), across=cross(along,up);
        b.box(add(mul(add(a,c),.5F),mul(up,height*.5F)),across,up,along,{width*.5F,height*.5F,length(sub(c,a))*.5F},top,side);
    };
    // The funnel: nearly a straight cone, flaring slightly toward the mouth.
    const auto station=[&](f32 s){ return std::lerp(throat,mouth,s); };
    const auto flare=[&](f32 s){ return throat_radius+(mouth_radius-throat_radius)*(.75F*s+.25F*s*s); };
    constexpr unsigned facets=24, stations=6;
    constexpr f32 shell=.06F;
    // Its shell: outside, panels between the ribs in two tones; inside, dark
    // bands narrowing toward the throat.
    for (unsigned k=0;k<stations;++k) {
        const auto s0=static_cast<f32>(k)/stations, s1=static_cast<f32>(k+1)/stations;
        const auto x0=station(s0), x1=station(s1), r0=flare(s0), r1=flare(s1);
        for (unsigned i=0;i<facets;++i) {
            const auto t0=2*pi*static_cast<f32>(i)/facets, t1=2*pi*static_cast<f32>(i+1)/facets;
            const auto inside=around((x0+x1)*.5F,(r0+r1+shell)*.5F,(t0+t1)*.5F);
            b.quad(around(x0,r0+shell,t0),around(x0,r0+shell,t1),around(x1,r1+shell,t1),around(x1,r1+shell,t0),
                   (i/2+k)%2 ? hull : plating,inside);
            b.quad(around(x0,r0,t0),around(x0,r0,t1),around(x1,r1,t1),around(x1,r1,t0),k%2 ? frame_metal : dark,inside);
        }
    }
    // Ribs down every other seam outside, and two hoops with work lights.
    for (unsigned j=0;j<facets;j+=2) {
        const auto t=2*pi*static_cast<f32>(j)/facets;
        const Vec3 out{0,std::cos(t),std::sin(t)};
        for (unsigned k=0;k<stations;++k) {
            const auto s0=static_cast<f32>(k)/stations, s1=static_cast<f32>(k+1)/stations;
            rib(around(station(s0),flare(s0)+shell,t),around(station(s1),flare(s1)+shell,t),out,.12F,.1F,plating,frame_metal);
        }
    }
    for (const auto s : {.35F,.72F}) {
        band(station(s)-.1F,flare(s)+shell,station(s)+.1F,flare(s+.05F)+shell,.08F,facets,dark,dark,true);
        for (unsigned j=2;j<facets;j+=4)
            b.box(around(station(s),flare(s+.025F)+shell+.1F,2*pi*static_cast<f32>(j)/facets),{.035F,.035F,.035F},amber_light,amber_light);
    }
    // The mouth: a heavy rim with guide lights round its outer edge, hot
    // enough to ring the mouth from tens of kilometres.
    band(mouth,mouth_radius-.1F,mouth+.35F,mouth_radius-.1F,.32F,facets*2,hull,plating,true);
    constexpr Material guide{{.85F,.9F,1.F},6.F};
    for (unsigned i=0;i<facets;++i) {
        const auto t=2*pi*(static_cast<f32>(i)+.5F)/facets;
        b.box(around(mouth+.35F,mouth_radius+.22F,t),{1,0,0},{0,std::cos(t),std::sin(t)},{0,-std::sin(t),std::cos(t)},
              {.025F,.022F,.045F},guide,guide);
    }
    // Lead-in lights down the inside toward the throat.
    for (unsigned line=0;line<4;++line) {
        const auto t=pi/4+pi/2*static_cast<f32>(line);
        for (unsigned k=1;k<7;++k) {
            const auto s=static_cast<f32>(k)/7;
            b.box(around(station(s),flare(s)-.03F,t),{.05F,.05F,.05F},lamp_cyan,lamp_cyan);
        }
    }
    // The receiving bay behind the throat: a lit door at the throat, a collar
    // over the joint, and doors either side where pods go out to the racks.
    b.prism({boom_end,0,0},{throat,0,0},1.35F,6,hull,dark,pi/6);
    b.prism({boom_end,0,0},{boom_end+.35F,0,0},1.5F,6,dark,dark,pi/6);
    band(throat-.25F,1.3F,throat+.2F,1.3F,.25F,facets,dark,dark,true);
    b.prism({throat,0,0},{throat+.02F,0,0},.62F,12,warm_window,warm_window);
    band(throat,.62F,throat+.5F,.62F,.18F,12,dark,dark,true);
    for (unsigned i=0;i<12;++i) b.box(around(throat+.51F,.71F,2*pi*(static_cast<f32>(i)+.5F)/12),{.02F,.035F,.035F},lamp_white,lamp_white);
    for (const auto side : {-1.F,1.F}) {
        const auto face=1.35F*std::cos(pi/6), x=boom_end+1.7F;
        b.box({x,0,side*(face+.005F)},{.62F,.34F,.01F},warm_window,warm_window);
        for (const auto dx : {-.4F,0.F,.4F}) b.box({x+dx,0,side*(face+.02F)},{.03F,.34F,.02F},dark,dark);
        for (const auto y : {-.4F,.4F}) b.box({x,y,side*(face+.02F)},{.72F,.06F,.03F},dark,dark);
    }
    // The boom from the hub, with pod racks either side.
    b.prism({9,0,0},{boom_end,0,0},.5F,6,hull,plating,pi/6);
    for (const auto x : {9.2F,10.9F}) b.prism({x-.17F,0,0},{x+.17F,0,0},.62F,6,dark,dark,pi/6);
    const Material cargo{{.34F,.33F,.30F}}, stripe{{.42F,.24F,.06F}};
    for (const auto side : {-1.F,1.F}) {
        const auto z=side*1.55F, x0=10.15F, x1=11.65F;
        for (const auto y : {-.8F,.8F}) b.beam({x0,y,z},{x1,y,z},.08F,frame_metal);
        for (const auto x : {x0,x1}) {
            b.beam({x,-.8F,z},{x,.8F,z},.08F,frame_metal);
            b.beam({x,0,side*.43F},{x,0,z},.1F,frame_metal);
        }
        for (unsigned slot=0;slot<3;++slot) {
            if (side>0 && slot==1) continue;
            const auto x=x0+.25F+.5F*static_cast<f32>(slot);
            b.prism({x,-.42F,z},{x,.42F,z},.2F,10,cargo,dark);
            b.dome({x,.42F,z},{0,1,0},.2F,cargo,3,10);
            b.dome({x,-.42F,z},{0,-1,0},.2F,cargo,3,10);
            b.prism({x,.12F,z},{x,.24F,z},.21F,10,stripe,stripe);
        }
    }
    // Navigation beacons at the extremities.
    b.box({-9.3F,0,0},{.1F,.1F,.1F},white_light,white_light);
    b.box(around(mouth+.17F,mouth_radius+.27F,pi/2),{.08F,.08F,.08F},green_light,green_light);
    b.box(around(mouth+.17F,mouth_radius+.27F,-pi/2),{.08F,.08F,.08F},red_light,red_light);
    auto result=std::move(b).document("GATEWAY / Arabian orbital yard","hub spindle, docking arms, solar wings, mass-driver catcher");
    result.metadata["render/lighting"]="illustrated";
    return result;
}

vm::Document habitat_ring() {
    Builder b;
    constexpr f32 major=habitat_ring_radius(), width=.55F, depth=.42F;
    constexpr unsigned segments=72;
    const auto around=[&](f32 t, f32 r, f32 x) { return Vec3{x,r*std::cos(t),r*std::sin(t)}; };
    for (unsigned i=0;i<segments;++i) {
        const auto t0=2*pi*static_cast<f32>(i)/segments, t1=2*pi*static_cast<f32>(i+1)/segments;
        const auto inner=major-depth, outer=major+depth;
        const auto inside=around((t0+t1)*.5F,major,0);
        const auto lit=i%3!=1;
        // Outer rim, inner rim (windows facing the hub), and both side walls.
        b.quad(around(t0,outer,-width),around(t1,outer,-width),around(t1,outer,width),around(t0,outer,width),hull,inside);
        b.quad(around(t0,inner,-width),around(t1,inner,-width),around(t1,inner,width),around(t0,inner,width),plating,inside);
        b.quad(around(t0,inner,-width),around(t1,inner,-width),around(t1,outer,-width),around(t0,outer,-width),plating,inside);
        b.quad(around(t0,inner,width),around(t1,inner,width),around(t1,outer,width),around(t0,outer,width),plating,inside);
        if (lit) {
            const auto a=around(t0+.01F,inner-.012F,-.16F), c=around(t1-.01F,inner-.012F,.16F);
            b.quad(a,around(t1-.01F,inner-.012F,-.16F),c,around(t0+.01F,inner-.012F,.16F),warm_window,inside);
            const auto e=around(t0+.01F,outer+.012F,-.08F), f=around(t1-.01F,outer+.012F,.08F);
            b.quad(e,around(t1-.01F,outer+.012F,-.08F),f,around(t0+.01F,outer+.012F,.08F),cyan_light,inside);
        }
    }
    // Four spokes from the hub to the ring, with a light at each junction.
    for (unsigned i=0;i<4;++i) {
        const auto t=pi/2*static_cast<f32>(i);
        b.prism(around(t,.85F,0),around(t,major-depth,0),.2F,6,plating,plating);
        b.box(around(t,major-depth-.25F,0),{.12F,.12F,.12F},white_light,white_light);
    }
    b.prism({-.6F,0,0},{.6F,0,0},1.05F,12,dark,dark);
    auto result=std::move(b).document("GATEWAY / spinning habitat ring","ring, windows, spokes, bearing");
    result.metadata["render/lighting"]="illustrated";
    return result;
}

vm::Document glow_sphere(std::string name, Vec3 color, f32 emission) {
    Builder b;
    // Icosahedron subdivided twice: round enough once bloom spreads it.
    const f32 g=(1+std::sqrt(5.F))*.5F;
    std::vector<Vec3> p{{-1,g,0},{1,g,0},{-1,-g,0},{1,-g,0},{0,-1,g},{0,1,g},{0,-1,-g},{0,1,-g},
        {g,0,-1},{g,0,1},{-g,0,-1},{-g,0,1}};
    for (auto& v : p) v=unit(v);
    std::vector<std::array<Vec3,3>> triangles;
    for (const auto& [a,c,d] : std::array<std::array<unsigned,3>,20>{{{0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
             {1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},{3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
             {4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1}}})
        triangles.push_back({p[a],p[c],p[d]});
    for (unsigned level=0;level<2;++level) {
        std::vector<std::array<Vec3,3>> next;
        for (const auto& [a,c,d] : triangles) {
            const auto ac=unit(add(a,c)), cd=unit(add(c,d)), da=unit(add(d,a));
            next.insert(next.end(),{{a,ac,da},{c,cd,ac},{d,da,cd},{ac,cd,da}});
        }
        triangles=std::move(next);
    }
    for (const auto& [a,c,d] : triangles) b.triangle(a,c,d,{color,emission},{0,0,0});
    return std::move(b).document(std::move(name),"unlit emissive sphere",false);
}

vm::Document fragment(u32 seed) {
    Builder b;
    const f32 g=(1+std::sqrt(5.F))*.5F;
    std::vector<Vec3> p{{-1,g,0},{1,g,0},{-1,-g,0},{1,-g,0},{0,-1,g},{0,1,g},{0,-1,-g},{0,1,-g},
        {g,0,-1},{g,0,1},{-g,0,-1},{-g,0,1}};
    for (auto& v : p) v=unit(v);
    std::vector<std::array<Vec3,3>> triangles;
    for (const auto& [a,c,d] : std::array<std::array<unsigned,3>,20>{{{0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
             {1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},{3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
             {4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1}}})
        triangles.push_back({p[a],p[c],p[d]});
    std::vector<std::array<Vec3,3>> next;
    for (const auto& [a,c,d] : triangles) {
        const auto ac=unit(add(a,c)), cd=unit(add(c,d)), da=unit(add(d,a));
        next.insert(next.end(),{{a,ac,da},{c,cd,ac},{d,da,cd},{ac,cd,da}});
    }
    // Lumpy and flattened, with a couple of flat fracture faces.
    const auto shape=[&](Vec3 v) {
        auto r=1+.22F*noise(mul(v,2.1F),seed)+.08F*noise(mul(v,5.3F),seed+7);
        if (v.x>.55F) r=std::min(r,.62F/std::max(v.x,.01F));
        if (v.y<-.6F) r=std::min(r,.7F/std::max(-v.y,.01F));
        return Vec3{v.x*r*1.15F,v.y*r*.8F,v.z*r};
    };
    const Material rock{{.07F,.065F,.06F}};
    for (const auto& [a,c,d] : next) b.triangle(shape(a),shape(c),shape(d),rock,{0,0,0});
    auto result=std::move(b).document("BELT / fragment","small faceted rock");
    result.metadata["render/lighting"]="matte";
    return result;
}

vm::Document engine_plume(std::span<const Vec3> nozzles, f32 radius, Vec3 hot) {
    Builder b;
    constexpr unsigned sides=12, bands=6;
    for (const auto nozzle : nozzles) {
        const auto ring=[&](unsigned band, unsigned i) {
            const auto s=static_cast<f32>(band)/bands;
            // A faint swell behind the nozzle, then a long taper.
            const auto r=radius*(1+.1F*std::sin(s*pi))*std::pow(1-s,.8F);
            const auto t=2*pi*static_cast<f32>(i)/sides;
            return add(nozzle,{r*std::cos(t),r*std::sin(t),s});
        };
        for (unsigned band=0;band<bands;++band) {
            const auto s0=static_cast<f32>(band)/bands, s1=static_cast<f32>(band+1)/bands;
            // Hot at the nozzle, fading to black, which is nothing against
            // space, well before the tip: the cone is opaque.
            const auto color=[&](f32 s){
                const auto fade=std::pow(1-s,2.2F);
                return Material{mul(hot,fade),6.F*fade};
            };
            for (unsigned i=0;i<sides;++i) {
                const auto first=static_cast<u32>(b.positions.size()/3);
                for (const auto& [p,m] : std::array{std::pair{ring(band,i),color(s0)},std::pair{ring(band,i+1),color(s0)},
                         std::pair{ring(band+1,i+1),color(s1)},std::pair{ring(band+1,i),color(s1)}})
                    b.vertex(p,{0,0,0},m);
                b.faces.emplace_back(first,first+1,first+2);
                b.faces.emplace_back(first,first+2,first+3);
            }
        }
    }
    return std::move(b).document("KESTREL / burn plume","unlit tapered exhaust cones",false);
}
}
