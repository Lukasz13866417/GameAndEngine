#include "sun_assets.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace example::sun {
namespace {
using namespace vng;
constexpr float pi = std::numbers::pi_v<float>;

Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 subtract(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 scale(Vec3 a, float b) { return {a.x * b, a.y * b, a.z * b}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x};
}
Vec3 unit(Vec3 a) { return scale(a, 1.0F / std::sqrt(dot(a, a))); }
float mix(float a, float b, float t) { return a + (b - a) * t; }
float smooth(float low, float high, float x)
{
    const auto t = std::clamp((x - low) / (high - low), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

// Unsigned arithmetic is deliberate: repeatable modulo-2^32 hashing, including
// for cells with negative coordinates. There is no platform-dependent RNG.
u32 hash(u32 x)
{
    x ^= x >> 16U;
    x *= 0x7FEB352DU;
    x ^= x >> 15U;
    x *= 0x846CA68BU;
    return x ^ (x >> 16U);
}
u32 cell_hash(i32 x, i32 y, i32 z)
{
    return hash(static_cast<u32>(x) * 0x8DA6B343U
        ^ static_cast<u32>(y) * 0xD8163841U
        ^ static_cast<u32>(z) * 0xCB1AB31FU);
}
float fraction(u32 x) { return static_cast<float>(x >> 8U) / 16777216.0F; }

float noise(Vec3 p)
{
    const auto ix = static_cast<i32>(std::floor(p.x));
    const auto iy = static_cast<i32>(std::floor(p.y));
    const auto iz = static_cast<i32>(std::floor(p.z));
    const auto x = smooth(0, 1, p.x - static_cast<float>(ix));
    const auto y = smooth(0, 1, p.y - static_cast<float>(iy));
    const auto z = smooth(0, 1, p.z - static_cast<float>(iz));
    const auto xy = [&](i32 dz) {
        return mix(mix(fraction(cell_hash(ix, iy, iz + dz)),
                       fraction(cell_hash(ix + 1, iy, iz + dz)), x),
            mix(fraction(cell_hash(ix, iy + 1, iz + dz)),
                fraction(cell_hash(ix + 1, iy + 1, iz + dz)), x), y);
    };
    return mix(xy(0), xy(1), z);
}

float granules(Vec3 p)
{
    const auto ix = static_cast<i32>(std::floor(p.x));
    const auto iy = static_cast<i32>(std::floor(p.y));
    const auto iz = static_cast<i32>(std::floor(p.z));
    float first = 100, second = 100, brightness = 1;
    // Two closest jittered feature points define narrow Voronoi boundary
    // lanes; unlike thresholded fractal noise, these really form little cells.
    for (i32 z = -1; z <= 1; ++z) {
        for (i32 y = -1; y <= 1; ++y) {
            for (i32 x = -1; x <= 1; ++x) {
                const auto bits = cell_hash(ix + x, iy + y, iz + z);
                const Vec3 feature{
                    static_cast<float>(ix + x) + .16F
                        + .68F * static_cast<float>(bits & 1023U) / 1023.0F,
                    static_cast<float>(iy + y) + .16F
                        + .68F * static_cast<float>((bits >> 10U) & 1023U) / 1023.0F,
                    static_cast<float>(iz + z) + .16F
                        + .68F * static_cast<float>((bits >> 20U) & 1023U) / 1023.0F};
                const auto delta = subtract(p, feature);
                const auto distance = dot(delta, delta);
                if (distance < first) {
                    second = first;
                    first = distance;
                    brightness = .78F + .22F * fraction(hash(bits));
                } else if (distance < second) {
                    second = distance;
                }
            }
        }
    }
    const auto interior = smooth(.012F, .12F, second - first);
    return .10F + .90F * interior * brightness
        * (.75F + .25F / (1.0F + 3.0F * first));
}

// One authored magnetic-region description drives both the surface maps and
// its 3D arcade. It is deliberately local to this example: these are art
// controls, not a general engine asset or physics API.
struct ActiveRegion {
    Vec3 center;
    Vec3 tangent;
    Vec3 side;
    float height;
    float spread;
    u32 strands;
    float strength;
    float spot;
};

ActiveRegion active_region(Vec3 center, float height, float spread, u32 strands,
    float tilt, float strength, float spot)
{
    center = unit(center);
    const auto across = unit(cross({0, 0, 1}, center));
    const auto along = cross(center, across);
    const auto tangent = add(scale(across, std::cos(tilt)),
        scale(along, std::sin(tilt)));
    return {center, tangent, cross(center, tangent), height, spread,
        strands, strength, spot};
}

const auto& active_regions()
{
    static const std::array regions{
        active_region({.67F, .69F, .28F}, .19F, .15F, 21, .35F, 1.0F, .50F),
        active_region({.75F, .58F, .28F}, .11F, .095F, 10, -.15F, .72F, .30F),
        active_region({-.73F, -.62F, .27F}, .15F, .12F, 19, -.25F, .94F, .62F),
        active_region({-.90F, .32F, .29F}, .075F, .085F, 10, .4F, .88F, .44F),
        active_region({.18F, -.92F, .31F}, .09F, .07F, 8, -.2F, .58F, .35F),
        active_region({.22F, .22F, .95F}, .070F, .12F, 16, .7F, 1.0F, .97F),
        active_region({-.42F, -.23F, .88F}, .060F, .10F, 14, -.45F, .92F, .95F),
        active_region({.41F, -.85F, -.32F}, .12F, .095F, 8, .3F, .85F, .65F),
        // Low, broad surface arcades make magnetic organization visible across
        // the disc as well as in the two largest off-limb prominences.
        active_region({.48F, -.08F, .87F}, .043F, .15F, 10, -.8F, .88F, .60F),
        active_region({-.32F, .48F, .81F}, .056F, .13F, 10, .25F, .78F, .68F),
        active_region({.08F, -.47F, .88F}, .043F, .14F, 6, 1.1F, .58F, .40F)};
    return regions;
}

// X is the softened dark spot/filament mask, Y the broad emitting active-region
// envelope. Both share arcade footpoints; the short surface tufts are separate.
Vec2 magnetic_fields(Vec3 direction, Vec3 warp, float broad)
{
    Vec2 result{};
    for (const auto& region : active_regions()) {
        const auto delta = subtract(direction, region.center);
        if (dot(delta, delta) > .45F * .45F)
            continue;
        const auto x = dot(delta, region.tangent);
        const auto y = dot(delta, region.side);
        const auto pole = region.spread * .86F;
        const auto local_radius = std::sqrt(x * x + y * y);
        const auto envelope = 1.0F - smooth(.10F, .41F, local_radius);

        // Smooth, unequal footpoint lobes surround the same locations used by
        // the 3D strands. A wide low pedestal connects them into one region.
        const auto foot_radius = .065F + region.spread * .28F;
        const auto left_distance = (x + pole) * (x + pole) + y * y;
        const auto right_distance = (x - pole) * (x - pole) + y * y;
        const auto left = std::exp(-left_distance / (foot_radius * foot_radius));
        const auto right = std::exp(-right_distance / (foot_radius * foot_radius * .72F));
        const auto emission = (.17F * envelope + .92F * left + .76F * right) * envelope
            * region.strength * (.62F + .55F * broad);
        result.y = std::max(result.y, emission);

        const auto spot_radius = .017F + region.spread * .065F;
        const auto radius = std::sqrt(std::min(left_distance, right_distance * 1.7F))
            / spot_radius * (.94F + .12F * warp.z);
        const auto umbra = 1.0F - smooth(.28F, 1.05F, radius);
        const auto penumbra = 1.0F - smooth(.65F, 2.3F, radius);
        const auto spot = (.72F * umbra + .28F * penumbra) * region.spot;
        // A soft curved dark filament lies between the footpoints, widening
        // into a dark lane rather than a single sharp painted-on black line.
        const auto filament_path = .045F * std::sin(x / region.spread * pi)
            + .010F * (warp.x - .5F);
        const auto filament = (1.0F - smooth(.004F, .024F, std::abs(y - filament_path)))
            * (1.0F - smooth(region.spread * .6F, region.spread * 1.25F, std::abs(x)));
        result.x = std::max(result.x, std::max(spot,
            filament * region.strength * .35F));
    }
    return result;
}

float compact_emission(Vec3 direction)
{
    // Three deliberately placed compact surface knots, independent of the
    // wider arcade footpoints. Direction-space keeps them attached to the
    // rotating body and preserves UV seam/pole behavior. A is only a mask;
    // the shader supplies their white-hot radiance and fine internal breakup.
    struct Knot { Vec3 center; float major; float minor; float tilt; };
    static const std::array knots{
        // Move the formerly central-left knot below the other regions, so it
        // no longer crowds the two bright footpoints of the lower-left arcade.
        Knot{unit({-.10F, -.72F, .69F}), .055F, .025F, .75F},
        Knot{unit({.12F, .62F, .775F}), .052F, .027F, -.65F},
        Knot{unit({.43F, -.45F, .783F}), .050F, .023F, 1.15F}};
    float result{};
    for (const auto& knot : knots) {
        const auto delta = subtract(direction, knot.center);
        const auto squared_distance = dot(delta, delta);
        const auto support = knot.major * 1.25F;
        if (squared_distance >= support * support) continue;
        const auto tangent = unit(cross({0, 1, 0}, knot.center));
        const auto side = cross(knot.center, tangent);
        const auto along = add(scale(tangent, std::cos(knot.tilt)),
            scale(side, std::sin(knot.tilt)));
        const auto across = cross(knot.center, along);
        const auto x = dot(delta, along) / knot.major;
        const auto raw_y = dot(delta, across) / knot.minor;
        const auto center_detail = noise(add(scale(knot.center, 145.0F), {41, 11, 29}));
        const auto detail = noise(add(scale(direction, 145.0F), {41, 11, 29}));
        const auto y = raw_y - .23F * std::sin(x * 2.8F) - .10F * x * x
            + .22F * (detail - center_detail);
        const auto radius_squared = (x * x + y * y) * (.84F + .32F * detail);
        // A single curved, irregular patch with a smooth strong core: no flat
        // circular plateau and no detached secondary knots. Texture modulates
        // its feathered edges, rather than replacing the authored shape.
        const auto profile = std::exp(-3.0F * radius_squared * radius_squared)
            * (1.0F - smooth(.5F, 1.15F, radius_squared));
        const auto compact = 1.0F - smooth(knot.major, support,
            std::sqrt(squared_distance));
        result = std::max(result, profile * compact);
    }
    return result;
}

std::byte encode(float value)
{
    return static_cast<std::byte>(static_cast<u8>(
        std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F)));
}

} // namespace

SunMesh make_surface(u32 longitude, u32 latitude)
{
    if (longitude < 3 || latitude < 2)
        throw std::invalid_argument("sun sphere requires longitude >= 3 and latitude >= 2");
    const auto count = (static_cast<u64>(longitude) + 1U) * (latitude - 1U)
        + 2U * static_cast<u64>(longitude);
    if (count > std::numeric_limits<u32>::max())
        throw std::length_error("sun sphere exceeds the u32 vertex-index range");
    SunMesh mesh{static_cast<std::size_t>(count)};
    mesh.info().name = "Procedural solar photosphere";
    mesh.faces().reserve(static_cast<std::size_t>(longitude) * (latitude - 1U) * 2U);
    for (u32 y = 1; y < latitude; ++y) {
        const auto v = static_cast<float>(y) / static_cast<float>(latitude);
        const auto radius = std::sin(pi * v);
        const auto height = std::cos(pi * v);
        for (u32 x = 0; x <= longitude; ++x) {
            const auto u = static_cast<float>(x) / static_cast<float>(longitude);
            // Explicitly wrap the duplicate: sin(2*pi) is not exactly zero.
            const auto angle = x == longitude ? 0.0F : 2.0F * pi * u;
            auto& vertex = mesh.vertices()[(y - 1U) * (longitude + 1U) + x];
            vertex.set(gfx::Position{}, {radius * std::cos(angle), height,
                radius * std::sin(angle)});
            vertex.set(SurfaceUV{}, {u, v});
        }
    }
    for (u32 y = 0; y + 2U < latitude; ++y) {
        for (u32 x = 0; x < longitude; ++x) {
            const auto a = y * (longitude + 1U) + x;
            const auto b = a + longitude + 1U;
            mesh.add_face(a, a + 1U, b);
            mesh.add_face(a + 1U, b + 1U, b);
        }
    }
    const auto north = (latitude - 1U) * (longitude + 1U);
    const auto south = north + longitude;
    const auto last_ring = (latitude - 2U) * (longitude + 1U);
    for (u32 x = 0; x < longitude; ++x) {
        const auto u = (static_cast<float>(x) + .5F) / static_cast<float>(longitude);
        mesh.vertices()[north + x].set(gfx::Position{}, {0, 1, 0});
        mesh.vertices()[north + x].set(SurfaceUV{}, {u, 0});
        mesh.vertices()[south + x].set(gfx::Position{}, {0, -1, 0});
        mesh.vertices()[south + x].set(SurfaceUV{}, {u, 1});
        mesh.add_face(north + x, x + 1U, x);
        mesh.add_face(south + x, last_ring + x, last_ring + x + 1U);
    }
    return mesh;
}

gfx::ImageData make_surface_map(u32 width, u32 height)
{
    if (width < 4 || height < 2)
        throw std::invalid_argument("sun surface map requires width >= 4 and height >= 2");
    const auto pixel_count = static_cast<u64>(width) * height;
    if (pixel_count > std::numeric_limits<std::size_t>::max() / 4U
        || pixel_count > std::vector<std::byte>{}.max_size() / 4U)
        throw std::length_error("sun surface map exceeds addressable image storage");
    const auto byte_count = pixel_count * 4U;
    gfx::ImageData result{{width, height}, std::vector<std::byte>(
        static_cast<std::size_t>(byte_count))};
    // Cache trigonometry across rows. Pixels are cell-centered so repeating
    // linear filtering interpolates across the seam instead of duplicating it.
    std::vector<Vec2> longitude(width);
    for (u32 x = 0; x < width; ++x) {
        const auto angle = 2.0F * pi * (static_cast<float>(x) + .5F)
            / static_cast<float>(width);
        longitude[x] = {std::cos(angle), std::sin(angle)};
    }
    for (u32 y = 0; y < height; ++y) {
        const auto angle = pi * (static_cast<float>(y) + .5F)
            / static_cast<float>(height);
        // Clamp filtering at V=0/1 must select the same value for every copy
        // of a polar vertex, including when the map drives true displacement.
        const bool pole = y == 0 || y + 1U == height;
        const auto radius = pole ? 0.0F : std::sin(angle);
        const auto vertical = pole ? (y == 0 ? 1.0F : -1.0F) : std::cos(angle);
        for (u32 x = 0; x < width; ++x) {
            const Vec3 direction{radius * longitude[x].x, vertical,
                radius * longitude[x].y};
            const auto coarse = scale(direction, 5.5F);
            const Vec3 warp{noise(coarse),
                noise(add(coarse, {23.7F, 9.2F, 2.8F})),
                noise(add(coarse, {-8.3F, 17.1F, 21.6F}))};
            const auto broad_p = add(scale(direction, 5.3F), scale(warp, 2.0F));
            const auto broad = smooth(.22F, .76F, .72F * noise(broad_p)
                + .28F * noise(add(scale(broad_p, 2.03F), {11, 7, 19})));
            const auto field = magnetic_fields(direction, warp, broad);
            // A short correlation length is part of the field itself, not
            // fine noise layered over the old long contour/fan patterns. Mild
            // local warping makes irregular tufts without stretching them into
            // the woodgrain-like swirls caused by a strong global warp.
            const auto warp_p = scale(direction, 36.0F);
            const Vec3 tuft_warp{noise(add(warp_p, {17, 9, 41})),
                noise(add(warp_p, {31, 53, 7})), noise(add(warp_p, {61, 23, 19}))};
            const auto tuft_p = add(scale(direction, 126.0F), scale(tuft_warp, 1.35F));
            const auto tufts = noise({tuft_p.x, tuft_p.y * .88F, tuft_p.z});
            const auto medium_p = add(scale(direction, 84.0F), scale(tuft_warp, .95F));
            const auto short_medium = noise({medium_p.x, medium_p.y * .84F, medium_p.z});
            const auto broken = noise(add(scale(tuft_p, 1.71F), {31, 17, 43}));
            const auto threads = noise(add(scale(tuft_p, 2.23F), {61, 13, 37}));
            // Vary the mixture, not the local brightness: some regions have
            // small tufts, others finer strands between them. This avoids one
            // homogeneous grain size without restoring large colored bands.
            const auto medium_weight = .42F + .23F * smooth(.15F, .85F,
                noise(add(scale(direction, 24.0F), {11, 37, 53})));
            const auto turbulence = smooth(.23F, .77F,
                medium_weight * short_medium + (.80F - medium_weight) * tufts
                    + .13F * broken + .07F * threads);
            const auto cells = granules(add(scale(direction, 112.0F),
                scale(warp, 2.1F)));
            const auto index = (static_cast<std::size_t>(y) * width + x) * 4U;
            result.pixels[index] = encode(cells);
            result.pixels[index + 1U] = encode(turbulence);
            result.pixels[index + 2U] = encode(field.x);
            result.pixels[index + 3U] = encode(std::max(field.y, compact_emission(direction)));
        }
    }
    return result;
}

ProminenceMesh make_prominences(u32 seed)
{
    const auto& bundles = active_regions();
    constexpr u32 steps = 48;
    constexpr u32 sides = 6;
    constexpr u32 strand_vertices = (steps + 1U) * sides + 2U;
    u32 strands{};
    for (const auto& bundle : bundles) strands += bundle.strands;
    ProminenceMesh mesh{strands * strand_vertices};
    mesh.info().name = "Procedural magnetic prominence strands";
    mesh.faces().reserve(static_cast<std::size_t>(strands) * (steps + 1U) * sides * 2U);
    const auto random = [&seed] {
        seed = seed * 1664525U + 1013904223U;
        return fraction(seed);
    };
    u32 strand{};
    for (const auto& bundle : bundles) {
        const auto tangent = bundle.tangent;
        const auto side = bundle.side;
        for (u32 member = 0; member < bundle.strands; ++member, ++strand) {
            const auto layer = static_cast<float>(member) / static_cast<float>(bundle.strands - 1U);
            const auto phase = random() * 2 * pi;
            // Flux strands belong to one bundle but are not concentric hoops:
            // independent footpoints, skewed apices and uneven heights break
            // the mechanically nested-wire silhouette. One outer strand keeps
            // the bundle's characteristic scale; others gather below it.
            const auto height = bundle.height * (member == 0 ? 1.10F
                : .44F + .53F * random());
            const auto spread = bundle.spread * (.64F + .44F * random());
            const auto foot_left = spread * (.80F + .28F * random());
            const auto foot_right = spread * (.78F + .32F * random());
            const auto lateral_left = (layer - .5F) * .034F + (random() - .5F) * .012F;
            const auto lateral_right = (layer - .5F) * .025F + (random() - .5F) * .019F;
            const auto skew = (random() - .5F) * .25F;
            const auto profile = .83F + random() * .43F;
            const auto lean = .010F + .023F * random();
            const auto braid = .003F + .005F * random();
            const auto thickness = .0012F + random() * .0016F;
            const auto uv_phase = (static_cast<float>(strand) + .5F) / static_cast<float>(strands);
            const auto centerline = [&](float t) {
                const auto envelope = std::max(0.0F, std::sin(pi * t));
                const auto arch_t = t + skew * envelope;
                const auto arch = std::pow(std::max(0.0F, std::sin(pi * arch_t)), profile);
                const auto twist = braid * envelope * std::sin(t * 3 * pi + phase);
                const auto foot_path = mix(-foot_left, foot_right, t);
                const auto lateral = mix(lateral_left, lateral_right, t)
                    + lean * arch * (.4F + .6F * t) + twist;
                const auto direction = unit(add(bundle.center,
                    add(scale(tangent, foot_path + .014F * arch * std::sin(t * 2 * pi)),
                        scale(side, lateral))));
                return scale(direction, .992F + height * arch
                    + .003F * envelope * std::cos(t * 3 * pi + phase));
            };
            // Evenly spaced rings avoid slivers where a skewed short arch's
            // parameterization slows near its apex. The dense lookup is only
            // used at asset creation; vertex/animation work stays unchanged.
            constexpr u32 samples = steps * 8U;
            std::array<float, samples + 1U> distances{};
            auto previous = centerline(0);
            for (u32 i = 1; i <= samples; ++i) {
                const auto next = centerline(static_cast<float>(i) / static_cast<float>(samples));
                const auto delta = subtract(next, previous);
                distances[i] = distances[i - 1U] + std::sqrt(dot(delta, delta));
                previous = next;
            }
            const auto base = strand * strand_vertices;
            for (u32 step = 0; step <= steps; ++step) {
                const auto distance = distances.back()
                    * (static_cast<float>(step) / static_cast<float>(steps));
                const auto found = std::lower_bound(distances.begin() + 1, distances.end(), distance);
                const auto index = static_cast<std::size_t>(found - distances.begin());
                const auto fraction = (distance - distances[index - 1U])
                    / (distances[index] - distances[index - 1U]);
                const auto t = (static_cast<float>(index - 1U) + fraction) / static_cast<float>(samples);
                const auto center = centerline(t);
                const auto radial = unit(center);
                const auto forward = unit(subtract(
                    centerline(std::min(t + .002F, 1.0F)),
                    centerline(std::max(t - .002F, 0.0F))));
                const auto tube_u = unit(cross(forward, radial));
                const auto tube_v = cross(forward, tube_u);
                const auto width = thickness * (.80F
                    + .18F * std::sin(t * 3 * pi + phase)
                    + .12F * std::sin(t * 7 * pi - phase));
                for (u32 side_index = 0; side_index < sides; ++side_index) {
                    const auto angle = 2 * pi * static_cast<float>(side_index) / static_cast<float>(sides);
                    const auto offset = add(scale(tube_u, width * std::cos(angle)),
                        scale(tube_v, width * std::sin(angle)));
                    const auto position = add(center, offset);
                    auto& vertex = mesh.vertices()[base + step * sides + side_index];
                    vertex.set(gfx::Position{}, position);
                    vertex.set(gfx::Normal{}, unit(position));
                    vertex.set(SurfaceUV{}, {t, uv_phase});
                    vertex.set(TubeOffset{}, offset);
                    if (step < steps) {
                        const auto a = base + step * sides + side_index;
                        const auto b = base + step * sides + (side_index + 1U) % sides;
                        mesh.add_face(a, b, b + sides);
                        mesh.add_face(a, b + sides, a + sides);
                    }
                }
            }
            const auto start = base + (steps + 1U) * sides;
            const auto end = start + 1U;
            for (const auto cap : {start, end}) {
                const auto t = cap == start ? 0.0F : 1.0F;
                auto& vertex = mesh.vertices()[cap];
                vertex.set(gfx::Position{}, centerline(t));
                vertex.set(gfx::Normal{}, unit(centerline(t)));
                vertex.set(SurfaceUV{}, {t, uv_phase});
                vertex.set(TubeOffset{}, {});
            }
            for (u32 side_index = 0; side_index < sides; ++side_index) {
                const auto next = (side_index + 1U) % sides;
                mesh.add_face(start, base + next, base + side_index);
                mesh.add_face(end, base + steps * sides + side_index,
                    base + steps * sides + next);
            }
        }
    }
    return mesh;
}

} // namespace example::sun
