#include <vng/editor/mesh.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <algorithm>
#include <set>

namespace {
using namespace vng;
editor::EditableMesh square(bool faces=true) {
    content::vmesh::Document d;
    d.vertex_count=4; d.metadata["name"]="Quad";
    d.vertex_fields={{"position",{content::vmesh::ScalarType::Float32,3},std::vector<f32>{0,0,0,2,0,0,2,2,0,0,2,0}},
        {"color/0",{content::vmesh::ScalarType::Float32,3},std::vector<f32>{1,0,0,0,1,0,0,0,1,1,1,1}},
        {"part",{content::vmesh::ScalarType::UInt32,1},std::vector<u32>{10,20,30,40}}};
    if(faces) d.faces={{0,1,2},{0,2,3}};
    auto mesh=editor::EditableMesh::create(std::move(d)); REQUIRE(mesh); return std::move(*mesh);
}
TEST_CASE("Mesh center cache follows geometry edits and leaves copies unchanged", "[editor][mesh][mesh-origin]") {
    auto mesh=square();CHECK(mesh.center()==Vec3{1,1,0});
    auto copy=mesh;
    REQUIRE(mesh.set_position(0,{4,0,0}));
    CHECK(mesh.center()==Vec3{2,1,0});CHECK(copy.center()==Vec3{1,1,0});
    const std::array edges{gfx::Edge{0,1}};
    REQUIRE(copy.subdivide(edges));
    CHECK(copy.center().x==Catch::Approx(1));CHECK(copy.center().y==Catch::Approx(.8F));
    CHECK(mesh.center()==Vec3{2,1,0});
}
float area(const editor::EditableMesh& m) {
    float total{};
    for(auto f:m.document().faces) {
        auto a=m.position(f[0]),b=m.position(f[1]),c=m.position(f[2]);
        const auto cross=(b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
        CHECK(cross>0.F); total+=cross*.5F;
    }
    return total;
}
}
TEST_CASE("Mesh fill creates loose edges and ordered triangulated polygons transactionally","[editor][topology]") {
    auto mesh=square(false);
    REQUIRE(mesh.fill(std::array<u32,2>{0,1}));
    REQUIRE(mesh.document().edges); CHECK(mesh.edges().size()==1);
    CHECK_FALSE(mesh.fill(std::array<u32,2>{1,0}));
    REQUIRE(mesh.fill(std::array<u32,4>{0,1,2,3}));
    CHECK(mesh.document().faces.size()==2); CHECK(area(mesh)==Catch::Approx(4));
    const auto before=mesh.document();
    CHECK_FALSE(mesh.fill(std::array<u32,3>{0,99,2}));
    CHECK_FALSE(mesh.fill(std::array<u32,4>{0,2,1,3}));
    CHECK_FALSE(mesh.fill(std::array<u32,3>{0,1,0}));
    CHECK(mesh.document()==before);
    auto roundtrip=content::vmesh::write_vmesh(mesh.document()); REQUIRE(roundtrip);
    auto parsed=content::vmesh::parse_vmesh(*roundtrip); REQUIRE(parsed); CHECK(*parsed==before);
}
TEST_CASE("Fill handles concavity and rejects crossing boundaries","[editor][topology]") {
    auto d=square(false).document();
    d.vertex_fields.resize(1); d.vertex_count=5;
    d.vertex_fields[0].values=std::vector<f32>{0,0,0,2,0,0,2,2,0,1,1,0,0,2,0};
    auto mesh=editor::EditableMesh::create(d); REQUIRE(mesh);
    REQUIRE(mesh->fill(std::array<u32,5>{0,1,2,3,4}));
    CHECK(mesh->document().faces.size()==3); CHECK(area(*mesh)==Catch::Approx(3));
}
TEST_CASE("Subdivision preserves winding area shared topology and all vertex fields","[editor][topology]") {
    auto mesh=square();
    const auto metadata=mesh.document().metadata;
    const auto all=mesh.edges(); REQUIRE(all.size()==5);
    auto added=mesh.subdivide(all); REQUIRE(added); CHECK(added->size()==5);
    CHECK(mesh.size()==9); CHECK(mesh.document().faces.size()==8); CHECK(area(mesh)==Catch::Approx(4));
    CHECK(mesh.document().metadata==metadata);
    CHECK(std::get<std::vector<u32>>(mesh.document().vertex_fields[2].values)[added->front()]==10);
    const auto& colors=std::get<std::vector<f32>>(mesh.document().vertex_fields[1].values);
    CHECK(colors[12]==.5F); CHECK(colors[13]==.5F); CHECK(colors[14]==0.F);
    u32 center_count{};
    for(u32 i=4;i<mesh.size();++i) if(mesh.position(i)==Vec3{1,1,0}) ++center_count;
    CHECK(center_count==1);
}
TEST_CASE("Every triangle edge mask subdivides without degenerate faces or cracks","[editor][topology]") {
    for(unsigned mask=1;mask<8;++mask) {
        auto mesh=square();
        const std::array<gfx::Edge,3> edges{{{0,1},{1,2},{2,0}}};
        std::vector<gfx::Edge> selected;
        for(unsigned i=0;i<3;++i) if(mask&(1U<<i)) selected.push_back(edges[i]);
        REQUIRE(mesh.subdivide(selected));
        CHECK(area(mesh)==Catch::Approx(4));
        for(auto e:selected) CHECK(std::ranges::none_of(mesh.edges(),[&](auto other) {
            return (e[0]==other[0]&&e[1]==other[1]) || (e[0]==other[1]&&e[1]==other[0]);
        }));
    }
}
TEST_CASE("Subdivision handles loose edges duplicate requests and invalid selections","[editor][topology]") {
    auto mesh=square(false); REQUIRE(mesh.fill(std::array<u32,2>{0,1}));
    REQUIRE(mesh.subdivide(std::array<gfx::Edge,2>{{{0,1},{1,0}}}));
    CHECK(mesh.size()==5); CHECK(mesh.document().edges->size()==2); CHECK(mesh.document().faces.empty());
    const auto before=mesh.document();
    CHECK_FALSE(mesh.subdivide(std::array<gfx::Edge,2>{{{0,4},{2,90}}}));
    CHECK(mesh.document()==before);
}
TEST_CASE("Align to line honors ordered anchors does not clamp and preserves attributes","[editor][topology]") {
    auto mesh=square();
    const auto before=mesh.document();
    const auto a=mesh.position(2),b=mesh.position(0);
    auto changed=mesh.align_to_line(std::array<u32,4>{2,0,3,1}); REQUIRE(changed);
    CHECK(*changed==std::vector<u32>{3,1});
    CHECK(mesh.position(2)==a); CHECK(mesh.position(0)==b);
    CHECK(mesh.position(1)==Vec3{1,1,0}); CHECK(mesh.position(3)==Vec3{1,1,0});
    CHECK(mesh.document().faces==before.faces); CHECK(mesh.document().vertex_fields[1]==before.vertex_fields[1]);
    const auto stable=mesh.document();
    CHECK_FALSE(mesh.align_to_line(std::array<u32,3>{1,3,0}));
    CHECK(mesh.document()==stable);
    REQUIRE(mesh.set_position(1,{6,0,0}));
    REQUIRE(mesh.align_to_line(std::array<u32,3>{0,2,1}));
    CHECK(mesh.position(1)==Vec3{3,3,0});
}
