#include "support/sun_assets.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numbers>
#include <stdexcept>

namespace {
using namespace vng;
namespace sun = example::sun;
Vec3 subtract(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x};
}
} // namespace

TEST_CASE("Solar sphere has outward non-degenerate geometry and an exact seam", "[example][sun]")
{
    constexpr u32 longitude = 32, latitude = 16;
    const auto mesh = sun::make_surface(longitude, latitude);
    REQUIRE(mesh.validate());
    CHECK(mesh.face_count() == 2U * longitude * (latitude - 1U));
    for (const auto& vertex : mesh.vertices()) {
        const auto p = vertex.get(gfx::Position{});
        const auto uv = vertex.get(sun::SurfaceUV{});
        CHECK(std::abs(dot(p, p) - 1) < 1.0e-6F);
        CHECK(uv.x >= 0.0F);
        CHECK(uv.x <= 1.0F);
        CHECK(uv.y >= 0.0F);
        CHECK(uv.y <= 1.0F);
    }
    for (const auto& face : mesh.faces()) {
        const auto a = mesh.vertices()[face[0]].get(gfx::Position{});
        const auto b = mesh.vertices()[face[1]].get(gfx::Position{});
        const auto c = mesh.vertices()[face[2]].get(gfx::Position{});
        CHECK(dot(cross(subtract(b, a), subtract(c, a)), a) > 1.0e-6F);
    }
    for (u32 y = 0; y + 1U < latitude; ++y) {
        const auto& first = mesh.vertices()[y * (longitude + 1U)];
        const auto& last = mesh.vertices()[y * (longitude + 1U) + longitude];
        CHECK(first.get(gfx::Position{}) == last.get(gfx::Position{}));
        CHECK(first.get(sun::SurfaceUV{}).x == 0.0F);
        CHECK(last.get(sun::SurfaceUV{}).x == 1.0F);
    }
    const auto second = sun::make_surface(longitude, latitude);
    CHECK(mesh.faces() == second.faces());
    CHECK(std::ranges::equal(mesh.vertices().bytes(), second.vertices().bytes()));
    CHECK_THROWS_AS(sun::make_surface(2, 16), std::invalid_argument);
    CHECK_THROWS_AS(sun::make_surface(16, 1), std::invalid_argument);
}

TEST_CASE("Solar map is deterministic linear multi-channel data", "[example][sun]")
{
    const auto image = sun::make_surface_map(256, 128);
    CHECK(image.extent == Extent2D{256, 128});
    REQUIRE(image.pixels.size() == 256U * 128U * 4U);
    CHECK(image.pixels == sun::make_surface_map(256, 128).pixels);
    // Every sector's duplicated polar vertex sees the identical displacement.
    for (const auto row : {0U, image.extent.height - 1U}) {
        const auto first = static_cast<std::size_t>(row) * image.extent.width * 4U;
        for (u32 x = 1; x < image.extent.width; ++x)
            for (std::size_t channel = 0; channel < 4; ++channel)
                CHECK(image.pixels[first + x * 4U + channel]
                    == image.pixels[first + channel]);
    }
    std::array<unsigned, 4> minima{255, 255, 255, 255}, maxima{};
    std::size_t active{}, dark_lanes{}, bright_cells{};
    double global_emission{};
    for (std::size_t i = 0; i < image.pixels.size(); i += 4U) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            const auto value = std::to_integer<unsigned>(image.pixels[i + channel]);
            minima[channel] = std::min(minima[channel], value);
            maxima[channel] = std::max(maxima[channel], value);
        }
        active += std::to_integer<unsigned>(image.pixels[i + 2U]) > 160U ? 1U : 0U;
        dark_lanes += std::to_integer<unsigned>(image.pixels[i]) < 60U ? 1U : 0U;
        bright_cells += std::to_integer<unsigned>(image.pixels[i]) > 170U ? 1U : 0U;
        global_emission += std::to_integer<unsigned>(image.pixels[i + 3U]);
    }
    for (std::size_t channel = 0; channel < 4; ++channel)
        CHECK(maxima[channel] - minima[channel] > 80U);
    CHECK(active > 4U);
    CHECK(active < image.pixels.size() / 400U);
    CHECK(dark_lanes > 500U);
    CHECK(bright_cells > 1000U);
    double minimum_patch = 255, maximum_patch{};
    std::size_t patch_count{}, finely_detailed_patches{};
    for (u32 y = 16; y + 16U < image.extent.height; y += 8U) {
        for (u32 x = 0; x < image.extent.width; x += 8U) {
            double mean{}, squared{};
            for (u32 dy = 0; dy < 8U; ++dy)
                for (u32 dx = 0; dx < 8U; ++dx) {
                    const auto value = static_cast<double>(std::to_integer<unsigned>(image.pixels[
                        (static_cast<std::size_t>(y + dy) * image.extent.width + x + dx) * 4U + 1U]));
                    mean += value;
                    squared += value * value;
                }
            mean /= 64.0;
            minimum_patch = std::min(minimum_patch, mean);
            maximum_patch = std::max(maximum_patch, mean);
            ++patch_count;
            finely_detailed_patches += squared / 64.0 - mean * mean > 250.0 ? 1U : 0U;
        }
    }
    // Fine structure is widespread, rather than detail limited to a handful
    // of active regions or broad smooth continental-looking dark patches.
    CHECK(finely_detailed_patches > patch_count * 4U / 5U);
    CHECK(minimum_patch > 75.0);
    CHECK(maximum_patch - minimum_patch < 100.0);
    // Emission belongs to the generated arcades, not arbitrary fine noise:
    // sample A under every actual closed tube's cap center / footpoint.
    const auto arcades = sun::make_prominences();
    std::size_t footpoints{}, emitting_footpoints{};
    double total_emission{};
    for (const auto& vertex : arcades.vertices()) {
        const auto offset = vertex.get(sun::TubeOffset{});
        if (dot(offset, offset) != 0.0F) continue;
        const auto center = subtract(vertex.get(gfx::Position{}), offset);
        const auto length = std::sqrt(dot(center, center));
        const Vec3 direction{center.x / length, center.y / length, center.z / length};
        auto longitude = std::atan2(direction.z, direction.x)
            / (2.0F * std::numbers::pi_v<float>);
        if (longitude < 0.0F) longitude += 1.0F;
        const auto latitude = std::acos(std::clamp(direction.y, -1.0F, 1.0F))
            / std::numbers::pi_v<float>;
        const auto x = std::min(static_cast<u32>(longitude * static_cast<float>(image.extent.width)),
            image.extent.width - 1U);
        const auto y = std::min(static_cast<u32>(latitude * static_cast<float>(image.extent.height)),
            image.extent.height - 1U);
        const auto alpha = std::to_integer<unsigned>(
            image.pixels[(static_cast<std::size_t>(y) * image.extent.width + x) * 4U + 3U]);
        ++footpoints;
        emitting_footpoints += alpha > 50U ? 1U : 0U;
        total_emission += alpha;
    }
    CHECK(footpoints == 264U);
    CHECK(emitting_footpoints > footpoints * 9U / 10U);
    CHECK(total_emission / static_cast<double>(footpoints) > 110.0);
    CHECK(total_emission / static_cast<double>(footpoints)
        > global_emission / static_cast<double>(image.pixels.size() / 4U) + 80.0);
    CHECK_THROWS_AS(sun::make_surface_map(0, 32), std::invalid_argument);
    CHECK_THROWS_AS(sun::make_surface_map(32, 1), std::invalid_argument);
    CHECK_THROWS_AS(sun::make_surface_map(0xFFFFFFFFU, 0xFFFFFFFFU), std::length_error);
}

TEST_CASE("Solar tufts have a short spatial correlation length, not long swirls", "[example][sun]")
{
    const auto image = sun::make_surface_map(1024, 512);
    const auto pixel = [&](u32 x, u32 y) {
        return static_cast<double>(std::to_integer<unsigned>(image.pixels[
            (static_cast<std::size_t>(y) * image.extent.width + x) * 4U + 1U]));
    };
    const auto first_row = image.extent.height * 3U / 8U;
    const auto last_row = image.extent.height * 5U / 8U;
    const auto count = static_cast<double>(
        image.extent.width * (last_row - first_row));
    double mean{}, second_moment{};
    for (u32 y = first_row; y < last_row; ++y)
        for (u32 x = 0; x < image.extent.width; ++x) {
            const auto value = pixel(x, y);
            mean += value;
            second_moment += value * value;
        }
    mean /= count;
    const auto variance = second_moment / count - mean * mean;
    REQUIRE(variance > 500.0);
    const auto correlation = [&](u32 dx, u32 dy) {
        double covariance{};
        for (u32 y = first_row; y < last_row; ++y)
            for (u32 x = 0; x < image.extent.width; ++x)
                covariance += (pixel(x, y) - mean)
                    * (pixel((x + dx) % image.extent.width, y + dy) - mean);
        return covariance / count / variance;
    };
    // At this resolution one texel subtends ~0.006 radians. Nearby samples
    // remain related across mixed small/medium tufts (not white noise or only
    // the finest grain), but structure is mostly gone by ~0.018 radians.
    // The previous long-curl field had C(3,0) around 0.116 and C(4,0) 0.076.
    CHECK(correlation(1, 0) > .35);
    CHECK(correlation(0, 1) > .35);
    CHECK(correlation(2, 0) > .07);
    CHECK(correlation(2, 0) < .18);
    CHECK(std::abs(correlation(3, 0)) < .05);
    CHECK(std::abs(correlation(0, 3)) < .10);
    CHECK(std::abs(correlation(4, 0)) < .04);
}

TEST_CASE("Three authored solar emission knots have compact intentional peaks", "[example][sun]")
{
    const auto image = sun::make_surface_map(512, 256);
    const auto normalize = [](Vec3 p) {
        const auto length = std::sqrt(dot(p, p));
        return Vec3{p.x / length, p.y / length, p.z / length};
    };
    const auto emission = [&](Vec3 direction) {
        direction = normalize(direction);
        auto u = std::atan2(direction.z, direction.x)
            / (2.0F * std::numbers::pi_v<float>);
        if (u < 0.0F) u += 1.0F;
        const auto v = std::acos(std::clamp(direction.y, -1.0F, 1.0F))
            / std::numbers::pi_v<float>;
        const auto x = std::min(static_cast<u32>(u * static_cast<float>(image.extent.width)),
            image.extent.width - 1U);
        const auto y = std::min(static_cast<u32>(v * static_cast<float>(image.extent.height)),
            image.extent.height - 1U);
        return std::to_integer<unsigned>(image.pixels[
            (static_cast<std::size_t>(y) * image.extent.width + x) * 4U + 3U]);
    };
    struct Knot { Vec3 center; float major; float tilt; };
    const std::array knots{
        Knot{normalize({-.10F, -.72F, .69F}), .055F, .75F},
        Knot{normalize({.12F, .62F, .775F}), .052F, -.65F},
        Knot{normalize({.43F, -.45F, .783F}), .050F, 1.15F}};
    for (const auto& knot : knots) {
        INFO("compact knot at " << knot.center.x << ',' << knot.center.y << ',' << knot.center.z);
        CHECK(emission(knot.center) > 245U);
        CHECK(knot.center.z > .65F);
        const auto tangent = normalize(cross({0, 1, 0}, knot.center));
        const auto side = cross(knot.center, tangent);
        const Vec3 along{tangent.x * std::cos(knot.tilt) + side.x * std::sin(knot.tilt),
            tangent.y * std::cos(knot.tilt) + side.y * std::sin(knot.tilt),
            tangent.z * std::cos(knot.tilt) + side.z * std::sin(knot.tilt)};
        const auto across = cross(knot.center, along);
        const auto probe = [&](Vec3 axis, float offset) {
            return emission({knot.center.x + axis.x * offset,
                knot.center.y + axis.y * offset, knot.center.z + axis.z * offset});
        };
        // Strong elongated centers, not equally bright circular polka dots.
        const auto along_axis = probe(along, knot.major * .40F);
        const auto across_axis = probe(across, knot.major * .70F);
        CHECK(along_axis > 160U);
        CHECK(across_axis < 120U);
        CHECK(along_axis > across_axis + 80U);
        for (u32 i = 0; i < 12; ++i) {
            const auto angle = static_cast<float>(i) * std::numbers::pi_v<float> / 6.0F;
            const auto x = 2.5F * knot.major * std::cos(angle);
            const auto y = 2.5F * knot.major * std::sin(angle);
            const Vec3 surrounding{knot.center.x + tangent.x * x + side.x * y,
                knot.center.y + tangent.y * x + side.y * y,
                knot.center.z + tangent.z * x + side.z * y};
            // Deliberate small peaks, not an increase in broad/global emission.
            CHECK(emission(surrounding) < 100U);
        }
    }
    // The moved independent knot must not recreate the three-spot cluster
    // next to the lower-left arcade's two actual footpoint groups.
    const auto arcades = sun::make_prominences();
    std::size_t nearby_arc_roots{};
    for (const auto& vertex : arcades.vertices()) {
        const auto offset = vertex.get(sun::TubeOffset{});
        if (dot(offset, offset) != 0.0F) continue;
        const auto root = normalize(vertex.get(gfx::Position{}));
        if (root.x >= -.2F || root.y >= 0 || root.z <= .6F) continue;
        ++nearby_arc_roots;
        const auto distance = subtract(knots.front().center, root);
        CHECK(dot(distance, distance) > .38F * .38F);
    }
    CHECK(nearby_arc_roots > 10U);
}

TEST_CASE("Solar prominences are deterministic closed finite 3D strands", "[example][sun]")
{
    const auto mesh = sun::make_prominences();
    REQUIRE(mesh.validate());
    CHECK(mesh.face_count() > 10000U);
    CHECK(mesh.face_count() < 100000U);
    float greatest_radius{}, smallest_radius = 10;
    std::map<std::pair<float, float>, Vec3> centers;
    std::map<float, std::pair<float, float>> arcade_heights;
    for (const auto& vertex : mesh.vertices()) {
        const auto p = vertex.get(gfx::Position{});
        const auto n = vertex.get(gfx::Normal{});
        const auto uv = vertex.get(sun::SurfaceUV{});
        const auto offset = vertex.get(sun::TubeOffset{});
        const auto radius = std::sqrt(dot(p, p));
        REQUIRE(std::isfinite(radius));
        CHECK(std::abs(dot(n, n) - 1) < 1.0e-5F);
        CHECK(dot(p, n) > 0.0F);
        CHECK(uv.x >= 0.0F);
        CHECK(uv.x <= 1.0F);
        CHECK(uv.y > 0.0F);
        CHECK(uv.y < 1.0F);
        CHECK(std::isfinite(dot(offset, offset)));
        CHECK(dot(offset, offset) < .004F * .004F);
        const auto center = subtract(p, offset);
        const auto [found, inserted] = centers.try_emplace(
            std::pair{uv.y, uv.x}, center);
        if (!inserted) {
            const auto error = subtract(center, found->second);
            CHECK(dot(error, error) < 1.0e-12F);
        }
        greatest_radius = std::max(greatest_radius, radius);
        smallest_radius = std::min(smallest_radius, radius);
        auto& arcade = arcade_heights[uv.y];
        arcade.first = std::max(arcade.first, radius);
        arcade.second = n.z;
    }
    CHECK(greatest_radius > 1.18F);
    CHECK(greatest_radius < 1.25F);
    CHECK(smallest_radius < 1.0F);
    CHECK(smallest_radius > .98F);
    const auto low_front_arcades = std::ranges::count_if(arcade_heights,
        [](const auto& entry) { return entry.second.first < 1.075F && entry.second.second > .6F; });
    CHECK(low_front_arcades >= 40);
    // Each edge occurs twice, with opposite orientation: closed tubes rather
    // than camera-facing ribbon tricks, and consistent triangle winding.
    std::map<std::pair<u32, u32>, std::pair<unsigned, int>> edges;
    double signed_volume_times_six{};
    for (const auto& face : mesh.faces()) {
        const auto a = mesh.vertices()[face[0]].get(gfx::Position{});
        const auto b = mesh.vertices()[face[1]].get(gfx::Position{});
        const auto c = mesh.vertices()[face[2]].get(gfx::Position{});
        const auto geometric = cross(subtract(b, a), subtract(c, a));
        INFO("face " << face[0] << ',' << face[1] << ',' << face[2]
            << "; squared double area = " << std::scientific << dot(geometric, geometric));
        CHECK(dot(geometric, geometric) > 1.0e-14F);
        signed_volume_times_six += static_cast<double>(dot(a, cross(b, c)));
        for (std::size_t i = 0; i < 3; ++i) {
            const auto from = face[i];
            const auto to = face[(i + 1U) % 3U];
            auto& edge = edges[std::minmax(from, to)];
            ++edge.first;
            edge.second += from < to ? 1 : -1;
        }
    }
    for (const auto& [key, count] : edges) {
        static_cast<void>(key);
        CHECK(count.first == 2U);
        CHECK(count.second == 0);
    }
    CHECK(signed_volume_times_six > 1.0e-5);
    const auto second = sun::make_prominences();
    CHECK(mesh.faces() == second.faces());
    CHECK(std::ranges::equal(mesh.vertices().bytes(), second.vertices().bytes()));
    const auto different = sun::make_prominences(17);
    CHECK_FALSE(std::ranges::equal(mesh.vertices().bytes(), different.vertices().bytes()));
}
