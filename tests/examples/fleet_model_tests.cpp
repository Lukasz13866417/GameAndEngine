#include <vng/content/vmesh.hpp>
#include <vng/content/vmesh_schema.hpp>
#include <vng/gfx/geometry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <filesystem>

namespace {
namespace vm = vng::content::vmesh;
struct Emission : vng::gfx::Semantic<vng::f32> {};
using Vertex = vng::gfx::Record<vng::gfx::Position,vng::gfx::Normal,vng::gfx::Color,Emission>;
vng::Vec3 sub(vng::Vec3 a,vng::Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
vng::Vec3 cross(vng::Vec3 a,vng::Vec3 b)
{
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
float dot(vng::Vec3 a,vng::Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
}

TEST_CASE("Fleet blueprints decode through the standard semantic mesh API", "[example][fleet]")
{
    const auto directory=std::filesystem::path(VNG_TEST_SPACESHIP_PATH).parent_path();
    auto schema=vm::schema<Vertex>();
    schema.map("position",vng::gfx::Position{});
    schema.map("normal",vng::gfx::Normal{});
    schema.map("color/0",vng::gfx::Color{});
    schema.map("emission",Emission{});
    std::array<vng::Vec3,3> sizes;
    const std::array filenames{"fleet_carrier.vmesh","fleet_frigate.vmesh","fleet_escort.vmesh"};
    for(std::size_t model=0;model<filenames.size();++model) {
        INFO(filenames[model]);
        const auto document=vm::read_vmesh(directory/filenames[model]);
        REQUIRE(document);
        CHECK(document->metadata.at("coordinates/up")=="+Y");
        CHECK(document->metadata.at("coordinates/forward")=="-Z");
        CHECK(document->metadata.at("source/tool")=="examples/tools/make_fleet.cpp");
        REQUIRE(document->faces.size()>2500);
        REQUIRE(document->faces.size()<6000);
        const auto mesh=vm::decode(*document,schema);
        REQUIRE(mesh);

        vng::Vec3 minimum{100,100,100},maximum{-100,-100,-100};
        std::size_t invalid_vertices{},invalid_triangles{},emissive_vertices{};
        for(const auto& vertex : mesh->vertices()) {
            const auto p=vertex.get(vng::gfx::Position{});
            const auto n=vertex.get(vng::gfx::Normal{});
            const auto c=vertex.get(vng::gfx::Color{});
            const auto e=vertex.get(Emission{});
            bool valid=std::isfinite(e)&&e>=0&&e<=8&&std::abs(dot(n,n)-1)<1.0e-4F;
            for(std::size_t i=0;i<3;++i) {
                valid=valid&&std::isfinite(p[i])&&std::isfinite(n[i]);
                minimum[i]=std::min(minimum[i],p[i]);
                maximum[i]=std::max(maximum[i],p[i]);
            }
            for(std::size_t i=0;i<4;++i) valid=valid&&std::isfinite(c[i])&&c[i]>=0&&c[i]<=1;
            invalid_vertices+=valid&&c.w==1 ? 0U : 1U;
            emissive_vertices+=e>0 ? 1U : 0U;
        }
        for(const auto& face : mesh->faces()) {
            const auto& a=mesh->vertices()[face[0]];
            const auto& b=mesh->vertices()[face[1]];
            const auto& c=mesh->vertices()[face[2]];
            const auto geometric=cross(sub(b.get(vng::gfx::Position{}),a.get(vng::gfx::Position{})),
                                       sub(c.get(vng::gfx::Position{}),a.get(vng::gfx::Position{})));
            const auto area=std::sqrt(dot(geometric,geometric));
            bool valid=area>1.0e-8F;
            for(const auto& v : {a,b,c}) valid=valid&&dot(geometric,v.get(vng::gfx::Normal{}))>area*.9999F;
            invalid_triangles+=valid ? 0U : 1U;
        }
        CHECK(invalid_vertices==0);
        CHECK(invalid_triangles==0);
        CHECK(emissive_vertices>200);
        CHECK(emissive_vertices<mesh->vertices().size()/4);
        sizes[model]=sub(maximum,minimum);
        const auto serialized=vm::write_vmesh(*document);
        REQUIRE(serialized);
        const auto restored=vm::parse_vmesh(*serialized);
        REQUIRE(restored);
        CHECK(*restored==*document);
    }
    // Different silhouettes, not three uniform rescalings of the same ship.
    CHECK(sizes[0].z>16.F);
    CHECK(sizes[0].y>4.F);
    CHECK(sizes[1].z/sizes[1].x>2.F);
    CHECK(sizes[2].x/sizes[2].z>1.3F);
}
