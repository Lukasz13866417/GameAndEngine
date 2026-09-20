// Reproducible, deliberately authored geometry for the Kestrel flyby example.
// This is an offline asset tool, not a dependency of the running demo.
// Usage: vng_make_spaceship path/to/spaceship.vmesh
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
using vng::f32;
using vng::u32;
namespace vmesh = vng::content::vmesh;

Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 mul(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
Vec3 cross(Vec3 a, Vec3 b)
{
    return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
Vec3 lerp(Vec3 a, Vec3 b, float t) { return add(a, mul(sub(b, a), t)); }
Vec3 average(std::span<const Vec3> points)
{
    Vec3 result{};
    for (auto p : points) result = add(result, p);
    return mul(result, 1.0F / static_cast<float>(points.size()));
}

struct Material { Vec4 color; float emission{}; };
constexpr Material graphite{{0.032F, 0.049F, 0.068F, 1}};
constexpr Material navy{{0.047F, 0.086F, 0.126F, 1}};
constexpr Material ivory{{0.66F, 0.70F, 0.68F, 1}};
constexpr Material cool_armor{{0.29F, 0.39F, 0.43F, 1}};
constexpr Material metal{{0.19F, 0.24F, 0.27F, 1}};
constexpr Material black{{0.009F, 0.016F, 0.021F, 1}};
constexpr Material copper{{0.61F, 0.205F, 0.061F, 1}};
constexpr Material teal{{0.036F, 0.34F, 0.39F, 1}};
constexpr Material glass{{0.018F, 0.15F, 0.22F, 1}};
constexpr Material glass_side{{0.012F, 0.062F, 0.095F, 1}};
constexpr Material engine{{0.09F, 0.69F, 0.91F, 1}, 7.0F};
constexpr Material plasma{{0.028F, 0.34F, 0.71F, 1}, 3.8F};
constexpr Material running_light{{0.11F, 0.68F, 0.72F, 1}, 3.0F};
constexpr Material port_light{{0.72F, 0.07F, 0.025F, 1}, 2.5F};

struct Model {
    std::vector<float> positions, normals, colors, emissions;
    std::vector<vng::gfx::TriangleFace> faces;

    void triangle(Vec3 a, Vec3 b, Vec3 c, Material material, Vec3 inside)
    {
        Vec3 n = cross(sub(b, a), sub(c, a));
        const auto center = mul(add(add(a, b), c), 1.0F / 3.0F);
        if (dot(n, sub(center, inside)) < 0) {
            std::swap(b, c);
            n = mul(n, -1);
        }
        const float length = std::sqrt(dot(n, n));
        if (!(length > 1.0e-8F)) throw std::runtime_error("degenerate authored triangle");
        n = mul(n, 1.0F / length);
        const auto first = static_cast<u32>(positions.size() / 3);
        for (auto p : {a, b, c}) {
            positions.insert(positions.end(), {p.x, p.y, p.z});
            normals.insert(normals.end(), {n.x, n.y, n.z});
            colors.insert(colors.end(), {material.color.x, material.color.y,
                                        material.color.z, material.color.w});
            emissions.push_back(material.emission);
        }
        faces.emplace_back(first, first + 1, first + 2);
    }

    // Authored polygons are convex. Each triangle has its own true flat normal.
    void polygon(std::span<const Vec3> p, Material m, Vec3 inside)
    {
        for (std::size_t i = 1; i + 1 < p.size(); ++i)
            triangle(p[0], p[i], p[i + 1], m, inside);
    }
    void quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Material m, Vec3 inside)
    {
        polygon(std::array{a, b, c, d}, m, inside);
    }

    void prism(std::span<const Vec3> base, Vec3 extrusion,
               Material cap, Material side = graphite)
    {
        std::vector<Vec3> top;
        for (auto p : base) top.push_back(add(p, extrusion));
        const auto center = add(average(base), mul(extrusion, .5F));
        polygon(base, side, center);
        polygon(top, cap, center);
        for (std::size_t i = 0; i < base.size(); ++i) {
            const auto j = (i + 1) % base.size();
            quad(base[i], base[j], top[j], top[i], side, center);
        }
    }

    void panel(const std::array<Vec3, 4>& patch, Vec3 inside, Material m,
               float inset = .045F, float thickness = .018F)
    {
        // Leave an actual exposed hull seam around every armor plate.
        const Vec3 center = average(patch);
        Vec3 n = cross(sub(patch[1], patch[0]), sub(patch[3], patch[0]));
        n = mul(n, 1.0F / std::sqrt(dot(n, n)));
        if (dot(n, sub(center, inside)) < 0) n = mul(n, -1);
        std::array<Vec3, 4> small;
        for (std::size_t i = 0; i != 4; ++i)
            small[i] = add(lerp(patch[i], center, inset), mul(n, .006F));
        prism(small, mul(n, thickness), m, metal);
    }

    void plate_xz(std::span<const Vec3> p, float thickness, Material m)
    {
        prism(p, {0, thickness, 0}, m, graphite);
    }

    vmesh::Document document() &&
    {
        vmesh::Document result;
        result.metadata = {
            {"name", "KESTREL / K-07 long-range interceptor"},
            {"author", "Vibe Engine example asset"},
            {"source/tool", "examples/tools/make_spaceship.cpp"},
            {"coordinates/up", "+Y"}, {"coordinates/forward", "-Z"},
            {"coordinates/unit", "metre"},
            {"shading", "flat geometric normals; linear vertex colors; emission is a multiplier"},
            {"parts", "segmented hull, canopy, swept wings, nacelles, engine nozzles, fins, vents, markings"},
        };
        result.vertex_count = positions.size() / 3;
        result.vertex_fields = {
            {"position", {vmesh::ScalarType::Float32, 3}, std::move(positions)},
            {"normal", {vmesh::ScalarType::Float32, 3}, std::move(normals)},
            {"color/0", {vmesh::ScalarType::Float32, 4}, std::move(colors)},
            {"emission", {vmesh::ScalarType::Float32, 1}, std::move(emissions)},
        };
        result.faces = std::move(faces);
        return result;
    }
};

struct HullSection { float z, width, top, bottom; };
std::array<Vec3, 8> hull_ring(HullSection s)
{
    return {{{-.65F*s.width, s.top, s.z}, {.65F*s.width, s.top, s.z},
             {s.width, .45F*s.top, s.z}, {s.width, -.50F*s.bottom, s.z},
             {.65F*s.width, -s.bottom, s.z}, {-.65F*s.width, -s.bottom, s.z},
             {-s.width, -.50F*s.bottom, s.z}, {-s.width, .45F*s.top, s.z}}};
}

void hull(Model& m)
{
    constexpr std::array sections{
        HullSection{-5.05F, .075F, .045F, .040F},
        HullSection{-4.55F, .25F, .14F, .13F},
        HullSection{-3.65F, .45F, .27F, .22F},
        HullSection{-2.65F, .64F, .41F, .33F},
        HullSection{-1.55F, .80F, .50F, .43F},
        HullSection{-.30F, .94F, .57F, .51F},
        HullSection{.80F, 1.0F, .59F, .55F},
        HullSection{1.85F, .87F, .50F, .47F},
        HullSection{2.85F, .57F, .33F, .34F},
        HullSection{3.45F, .32F, .18F, .22F},
    };
    m.polygon(hull_ring(sections.front()), metal, {0, 0, -4.9F});
    m.polygon(hull_ring(sections.back()), metal, {0, 0, 3.0F});
    for (std::size_t s = 0; s + 1 < sections.size(); ++s) {
        const auto a = hull_ring(sections[s]);
        const auto b = hull_ring(sections[s + 1]);
        const Vec3 center{0, 0, .5F*(sections[s].z + sections[s + 1].z)};
        for (std::size_t i = 0; i < a.size(); ++i) {
            const auto j = (i + 1) % a.size();
            std::array patch{a[i], a[j], b[j], b[i]};
            m.polygon(patch, graphite, center);
            // Canopy covers the central front deck; its surrounding plates remain.
            const auto material = (i == 0 || i == 1 || i == 7) ? ivory :
                                  (i == 2 || i == 6) ? navy : cool_armor;
            m.panel(patch, center, material, .055F, .018F);
            if ((i == 2 || i == 6) && s >= 2 && s <= 6) {
                // Small recessed service hatches within the wide side belt.
                std::array inner{
                    lerp(lerp(a[i], a[j], .22F), lerp(b[i], b[j], .22F), .22F),
                    lerp(lerp(a[i], a[j], .77F), lerp(b[i], b[j], .77F), .22F),
                    lerp(lerp(a[i], a[j], .77F), lerp(b[i], b[j], .77F), .72F),
                    lerp(lerp(a[i], a[j], .22F), lerp(b[i], b[j], .22F), .72F)};
                m.panel(inner, center, s % 3 == 0 ? copper : graphite, .04F, .031F);
            }
        }
    }
}

void cockpit(Model& m)
{
    // An angular canopy with individual side panes, a copper gasket, and a
    // narrow central spine; not a scaled box or a translucent billboard.
    const std::array<Vec3, 6> base{{
        {-.32F,.30F,-3.62F}, {.32F,.30F,-3.62F}, {.62F,.52F,-1.57F},
        {.43F,.56F,-.70F}, {-.43F,.56F,-.70F}, {-.62F,.52F,-1.57F}}};
    m.prism(base, {0,.035F,0}, copper, graphite);
    const Vec3 left_front{-.27F,.365F,-3.38F};
    const Vec3 right_front{.27F,.365F,-3.38F};
    const Vec3 left_high{-.36F,1.005F,-1.70F};
    const Vec3 right_high{.36F,1.005F,-1.70F};
    const Vec3 left_back{-.30F,.72F,-.82F};
    const Vec3 right_back{.30F,.72F,-.82F};
    const Vec3 center{0,.60F,-1.8F};
    m.quad(left_front,right_front,right_high,left_high,glass,center);
    m.quad(left_high,right_high,right_back,left_back,glass,center);
    m.quad(base[0],left_front,left_high,base[5],glass_side,center);
    m.quad(base[1],base[2],right_high,right_front,glass_side,center);
    m.quad(base[5],left_high,left_back,base[4],glass_side,center);
    m.quad(base[2],base[3],right_back,right_high,glass_side,center);
    m.quad(base[4],left_back,right_back,base[3],graphite,center);
    m.panel({left_high,right_high,right_back,left_back}, center, navy,.79F,.024F);
    // Central canopy rail follows the glass slope with a real thickness.
    const std::array front_rail{
        Vec3{-.022F,.379F,-3.35F}, Vec3{.022F,.379F,-3.35F},
        Vec3{.027F,1.023F,-1.70F}, Vec3{-.027F,1.023F,-1.70F}};
    m.prism(front_rail,{0,.015F,0},graphite,metal);
}

Vec3 radial(float x, float y, float z, float r, float a)
{
    return {x + r*std::cos(a), y + r*std::sin(a), z};
}

void annulus(Model& m, float x, float y, float z, float outer, float inner,
             float length, Material body, unsigned segments = 16)
{
    const float step = 2*std::numbers::pi_v<float>/static_cast<float>(segments);
    for (unsigned i = 0; i < segments; ++i) {
        const auto a = static_cast<float>(i)*step;
        const auto b = static_cast<float>(i+1)*step;
        const std::array<Vec3,4> section{{
            radial(x,y,z,outer,a), radial(x,y,z,outer,b),
            radial(x,y,z,inner,b), radial(x,y,z,inner,a)}};
        m.prism(section,{0,0,length},body,body);
    }
}

void barrel(Model& m, float x, float y, std::span<const std::array<float,2>> profile,
            Material body, unsigned segments, bool armor = false)
{
    const float step = 2*std::numbers::pi_v<float>/static_cast<float>(segments);
    for (std::size_t s = 0; s + 1 < profile.size(); ++s) {
        const Vec3 center{x,y,.5F*(profile[s][0]+profile[s+1][0])};
        for (unsigned i = 0; i < segments; ++i) {
            const float a = static_cast<float>(i)*step;
            const float b = static_cast<float>(i+1)*step;
            const std::array patch{
                radial(x,y,profile[s][0],profile[s][1],a),
                radial(x,y,profile[s][0],profile[s][1],b),
                radial(x,y,profile[s+1][0],profile[s+1][1],b),
                radial(x,y,profile[s+1][0],profile[s+1][1],a)};
            m.polygon(patch,body,center);
            if (armor && s > 0 && s + 2 < profile.size()) {
                const bool upper = std::sin(.5F*(a+b)) > .15F;
                m.panel(patch,center,upper ? ivory : navy,.13F,.016F);
            }
        }
    }
    for (std::size_t cap : {std::size_t{0}, profile.size()-1}) {
        const float z=profile[cap][0], radius=profile[cap][1];
        const Vec3 center{x,y,z+(cap==0 ? .01F : -.01F)};
        for (unsigned i=0;i<segments;++i)
            m.triangle({x,y,z},radial(x,y,z,radius,static_cast<float>(i)*step),
                       radial(x,y,z,radius,static_cast<float>(i+1)*step),body,center);
    }
}

void engine_nacelle(Model& m, float x)
{
    constexpr float y=-.11F;
    constexpr std::array<std::array<float,2>,7> profile{{
        {{-.75F,.15F}}, {{-.18F,.43F}}, {{.65F,.57F}}, {{1.70F,.60F}},
        {{2.48F,.59F}}, {{3.15F,.52F}}, {{3.48F,.47F}}}};
    barrel(m,x,y,profile,graphite,16,true);
    // A recessed front intake, and the aft ceramic/metal concentric nozzle.
    annulus(m,x,y,-.69F,.22F,.135F,.10F,cool_armor);
    annulus(m,x,y,3.10F,.575F,.465F,.19F,metal);
    annulus(m,x,y,3.36F,.55F,.435F,.20F,copper);
    annulus(m,x,y,3.55F,.54F,.41F,.16F,graphite);
    annulus(m,x,y,3.68F,.475F,.35F,.15F,metal);
    annulus(m,x,y,3.78F,.35F,.29F,.07F,engine);
    constexpr std::array<std::array<float,2>,2> core{{{{3.77F,.292F}},{{3.83F,.292F}}}};
    barrel(m,x,y,core,engine,16);
    constexpr std::array<std::array<float,2>,4> plume{{
        {{3.84F,.24F}},{{4.28F,.29F}},{{4.95F,.17F}},{{5.94F,.012F}}}};
    barrel(m,x,y,plume,plasma,12);
    // Eight radial heat-spreading ribs. Their inner edges intersect the casing
    // intentionally, just as the armor panels overlap their underlying shell.
    for (unsigned i=0;i<8;++i) {
        const float a=static_cast<float>(i)*std::numbers::pi_v<float>/4;
        const float b=a+.055F;
        const std::array<Vec3,4> fin{{
            radial(x,y,2.55F,.58F,a),radial(x,y,3.37F,.54F,a),
            radial(x,y,3.38F,.67F,a),radial(x,y,2.67F,.66F,a)}};
        const Vec3 tangent{-std::sin(b)*.042F,std::cos(b)*.042F,0};
        m.prism(fin,tangent,metal,graphite);
    }
    // Six longitudinal top cooling slots with separated metallic louvres.
    for (unsigned i=0;i<7;++i) {
        const float z=1.11F+static_cast<float>(i)*.17F;
        m.plate_xz(std::array{Vec3{x-.25F,.495F,z},Vec3{x+.25F,.495F,z},
                              Vec3{x+.25F,.495F,z+.09F},Vec3{x-.25F,.495F,z+.09F}},
                   .045F,black);
    }
}

void wing(Model& m, float sign)
{
    auto p=[sign](float x,float y,float z) { return Vec3{sign*x,y,z}; };
    const std::array<Vec3,6> plan{{
        p(.65F,-.16F,-1.80F),p(1.40F,-.16F,-.96F),p(3.73F,-.16F,1.27F),
        p(3.61F,-.16F,2.20F),p(1.25F,-.16F,1.48F),p(.69F,-.16F,.55F)}};
    m.plate_xz(plan,.18F,navy);
    const std::array<Vec3,4> armor{{
        p(1.10F,.032F,-1.02F),p(3.55F,.032F,1.32F),
        p(3.46F,.032F,1.80F),p(1.31F,.032F,.99F)}};
    m.plate_xz(armor,.030F,ivory);
    const std::array<Vec3,4> leading{{
        p(1.40F,.068F,-.72F),p(3.52F,.068F,1.32F),
        p(3.35F,.068F,1.45F),p(1.29F,.068F,-.49F)}};
    m.plate_xz(leading,.012F,copper);
    const std::array<Vec3,4> dark_inset{{
        p(1.78F,.070F,.13F),p(3.04F,.070F,1.23F),
        p(2.73F,.070F,1.35F),p(1.68F,.070F,.56F)}};
    m.plate_xz(dark_inset,.014F,graphite);
    // Three diagonal recognition bars, applied geometry instead of a texture.
    for (unsigned i=0;i<3;++i) {
        const float x=2.18F+static_cast<float>(i)*.21F;
        const float z=.70F+static_cast<float>(i)*.19F;
        m.plate_xz(std::array{p(x,.095F,z),p(x+.095F,.095F,z+.084F),
                    p(x-.025F,.095F,z+.245F),p(x-.12F,.095F,z+.16F)},.008F,teal);
    }
    // A thin layered trailing-edge flap and its copper hinge line.
    m.plate_xz(std::array{p(2.16F,-.055F,1.78F),p(3.57F,-.055F,2.24F),
                           p(3.42F,-.055F,2.41F),p(2.12F,-.055F,1.95F)},.07F,cool_armor);
    m.plate_xz(std::array{p(2.23F,.03F,1.82F),p(3.48F,.03F,2.23F),
                           p(3.43F,.03F,2.28F),p(2.22F,.03F,1.87F)},.014F,copper);
    // Wing-tip blended vertical winglet, swept with a tapered top edge.
    const std::array<Vec3,4> winglet{{
        p(3.54F,.025F,1.11F),p(3.52F,.025F,2.25F),
        p(3.82F,.86F,2.21F),p(3.77F,.83F,1.82F)}};
    m.prism(winglet,{sign*.075F,0,0},navy,ivory);
    m.prism(std::array{p(3.78F,.83F,1.90F),p(3.82F,.86F,2.18F),
                        p(3.82F,.89F,2.18F),p(3.78F,.86F,1.90F)},
            {sign*.08F,0,0},sign<0 ? port_light : running_light,metal);
    // Closed landing-gear access panel, tucked under the wing root.
    m.prism(std::array{p(.85F,-.18F,-.45F),p(1.34F,-.18F,.06F),
                        p(1.35F,-.18F,.82F),p(.90F,-.18F,.53F)},
            {0,-.035F,0},cool_armor,metal);
}

void deck_details(Model& m)
{
    // Dorsal raised spine tapers into a swept tail fin.
    constexpr std::array<HullSection,4> sections{{
        {-.55F,.14F,.68F,-.54F},{.35F,.16F,.74F,-.55F},
        {1.35F,.11F,.65F,-.45F},{2.30F,.06F,.45F,-.36F}}};
    for (std::size_t i=0;i+1<sections.size();++i) {
        const auto a=hull_ring(sections[i]);
        const auto b=hull_ring(sections[i+1]);
        for (std::size_t s=0;s<8;++s)
            m.quad(a[s],a[(s+1)%8],b[(s+1)%8],b[s],navy,
                   {0,.56F,.5F*(sections[i].z+sections[i+1].z)});
    }
    const std::array<Vec3,4> fin{{
        {-.055F,.56F,.83F},{-.055F,.31F,2.77F},
        {-.035F,1.35F,2.40F},{-.035F,1.31F,2.10F}}};
    m.prism(fin,{.11F,0,0},navy,ivory);
    // Colored fin flash and a tiny navigation strip.
    m.prism(std::array{Vec3{.078F,.65F,1.58F},Vec3{.078F,.42F,2.48F},
                       Vec3{.055F,.65F,2.39F},Vec3{.055F,.87F,1.84F}},
            {.008F,0,0},copper,navy);
    m.plate_xz(std::array{Vec3{-.045F,1.34F,2.17F},Vec3{.045F,1.34F,2.17F},
                          Vec3{.045F,1.34F,2.37F},Vec3{-.045F,1.34F,2.37F}},
                .017F,running_light);

    for (float sign : {-1.0F,1.0F}) {
        // Transverse louvres on the aft deck, in two contrasting framed beds.
        const float x=sign*.43F;
        m.plate_xz(std::array{Vec3{x-.16F,.605F,-.06F},Vec3{x+.16F,.605F,-.06F},
                              Vec3{x+.14F,.605F,.95F},Vec3{x-.14F,.605F,.95F}},.025F,black);
        for (unsigned i=0;i<8;++i) {
            const float z=.015F+static_cast<float>(i)*.112F;
            m.plate_xz(std::array{Vec3{x-.14F,.64F,z},Vec3{x+.14F,.64F,z},
                                  Vec3{x+.14F,.64F,z+.035F},Vec3{x-.14F,.64F,z+.035F}},
                        .023F,metal);
        }
        // Small splayed underwing sensor pods, with teal inset apertures.
        constexpr std::array<std::array<float,2>,3> pod{{
            {{-.92F,.055F}},{{-.55F,.13F}},{{.25F,.105F}}}};
        barrel(m,sign*2.05F,-.30F,pod,graphite,8);
        annulus(m,sign*2.05F,-.30F,-.93F,.065F,.035F,.025F,teal,8);
        // Two narrow metal sensor whiskers deliberately follow the silhouette.
        constexpr std::array<std::array<float,2>,2> antenna{{
            {{-2.65F,.018F}},{{-1.10F,.026F}}}};
        barrel(m,sign*.87F,-.11F,antenna,metal,6);
        // Waist accent pods tie the wing, hull, and nacelle together visually.
        const std::array<Vec3,4> brace{{
            {sign*.78F,.04F,-.43F},{sign*1.53F,.04F,.21F},
            {sign*1.76F,.04F,1.19F},{sign*.98F,.04F,.96F}}};
        m.plate_xz(brace,.20F,navy);
        m.plate_xz(std::array{Vec3{sign*1.12F,.251F,.17F},Vec3{sign*1.49F,.251F,.49F},
                              Vec3{sign*1.52F,.251F,.64F},Vec3{sign*1.13F,.251F,.34F}},
                    .016F,copper);
    }
}

vmesh::Document make_spaceship()
{
    Model m;
    hull(m);
    cockpit(m);
    wing(m,-1); wing(m,1);
    engine_nacelle(m,-1.59F); engine_nacelle(m,1.59F);
    deck_details(m);
    return std::move(m).document();
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "Usage: vng_make_spaceship OUTPUT.vmesh\n";
        return 2;
    }
    try {
        const auto model=make_spaceship();
        if (auto result=vmesh::write_vmesh(std::filesystem::path(argv[1]),model); !result) {
            std::cerr << result.error().message << '\n';
            return 1;
        }
        const auto& positions=std::get<std::vector<f32>>(model.vertex_fields[0].values);
        Vec3 minimum{100,100,100}, maximum{-100,-100,-100};
        for (std::size_t i=0;i<positions.size();i+=3)
            for (std::size_t c=0;c<3;++c) {
                minimum[c]=std::min(minimum[c],positions[i+c]);
                maximum[c]=std::max(maximum[c],positions[i+c]);
            }
        std::cout << model.metadata.at("name") << '\n'
                  << model.vertex_count << " flat-shaded vertices, " << model.faces.size() << " triangles\n"
                  << "bounds: [" << minimum.x << ", " << minimum.y << ", " << minimum.z
                  << "] .. [" << maximum.x << ", " << maximum.y << ", " << maximum.z << "]\n"
                  << "wrote " << argv[1] << '\n';
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
