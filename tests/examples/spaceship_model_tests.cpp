#include <vng/content/vmesh.hpp>
#include <vng/content/vmesh_schema.hpp>
#include <vng/gfx/geometry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <vector>

namespace {
namespace vm = vng::content::vmesh;
struct Emission : vng::gfx::Semantic<vng::f32> {};
using Vertex = vng::gfx::Record<vng::gfx::Position, vng::gfx::Normal,
                                vng::gfx::Color, Emission>;

vng::Vec3 subtract(vng::Vec3 a, vng::Vec3 b)
{
    return {a.x-b.x,a.y-b.y,a.z-b.z};
}
vng::Vec3 cross(vng::Vec3 a, vng::Vec3 b)
{
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
float dot(vng::Vec3 a, vng::Vec3 b)
{
    return a.x*b.x+a.y*b.y+a.z*b.z;
}
} // namespace

TEST_CASE("Spaceship asset has finite CCW geometry and real emission", "[example][spaceship]")
{
    const auto document = vm::read_vmesh(std::filesystem::path(VNG_TEST_SPACESHIP_PATH));
    REQUIRE(document);
    CHECK(document->metadata.at("coordinates/forward") == "-Z");
    CHECK(document->metadata.at("coordinates/up") == "+Y");
    REQUIRE(document->faces.size() >= 3000);
    REQUIRE(document->faces.size() <= 12000);

    auto schema = vm::schema<Vertex>();
    schema.map("position", vng::gfx::Position{});
    schema.map("normal", vng::gfx::Normal{});
    schema.map("color/0", vng::gfx::Color{});
    schema.map("emission", Emission{});
    const auto mesh = vm::decode(*document, schema);
    REQUIRE(mesh);

    vng::Vec3 minimum{100,100,100}, maximum{-100,-100,-100};
    std::size_t invalid_vertices{}, invalid_triangles{}, emissive_vertices{};
    for (const auto& vertex : mesh->vertices()) {
        const auto p = vertex.get(vng::gfx::Position{});
        const auto n = vertex.get(vng::gfx::Normal{});
        const auto c = vertex.get(vng::gfx::Color{});
        const auto e = vertex.get(Emission{});
        bool valid = std::isfinite(e) && e >= 0 && e <= 8;
        valid = valid && std::abs(dot(n,n)-1) < 1.0e-4F;
        for (std::size_t i=0;i<3;++i) {
            valid = valid && std::isfinite(p[i]) && std::isfinite(n[i]);
            minimum[i] = std::min(minimum[i],p[i]);
            maximum[i] = std::max(maximum[i],p[i]);
        }
        for (std::size_t i=0;i<4;++i)
            valid = valid && std::isfinite(c[i]) && c[i] >= 0 && c[i] <= 1;
        valid = valid && c.w == 1;
        invalid_vertices += valid ? 0U : 1U;
        emissive_vertices += e > 0 ? 1U : 0U;
    }
    for (const auto& face : mesh->faces()) {
        const auto& a = mesh->vertices()[face[0]];
        const auto& b = mesh->vertices()[face[1]];
        const auto& c = mesh->vertices()[face[2]];
        const auto geometric = cross(
            subtract(b.get(vng::gfx::Position{}),a.get(vng::gfx::Position{})),
            subtract(c.get(vng::gfx::Position{}),a.get(vng::gfx::Position{})));
        const float area2 = std::sqrt(dot(geometric,geometric));
        bool valid = area2 > 1.0e-8F;
        for (const auto& vertex : {a,b,c})
            valid = valid && dot(geometric,vertex.get(vng::gfx::Normal{})) > area2*.9999F;
        invalid_triangles += valid ? 0U : 1U;
    }
    CHECK(invalid_vertices == 0);
    CHECK(invalid_triangles == 0);
    CHECK(emissive_vertices > 300);
    CHECK(emissive_vertices < mesh->vertices().size()/4);
    CHECK(minimum.x < -3.5F);
    CHECK(maximum.x > 3.5F);
    CHECK(minimum.y > -1.0F);
    CHECK(maximum.y < 1.5F);
    CHECK(minimum.z < -5.0F);
    CHECK(maximum.z > 5.5F);
    CHECK(maximum.z < 7.0F);
}

TEST_CASE("Spaceship file round-trips all authored attributes", "[example][spaceship]")
{
    const auto document = vm::read_vmesh(std::filesystem::path(VNG_TEST_SPACESHIP_PATH));
    REQUIRE(document);
    const auto source = vm::write_vmesh(*document);
    REQUIRE(source);
    const auto restored = vm::parse_vmesh(*source);
    REQUIRE(restored);
    CHECK(*restored == *document);
}
