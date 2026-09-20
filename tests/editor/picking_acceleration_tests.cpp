#include "../../examples/editor/selection.hpp"
#include <vng/spatial/triangle_bvh.hpp>
#include <vng/editor/selection.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numeric>

using namespace vng;

TEST_CASE("Triangle hierarchy prunes a large grid and preserves distance clipping", "[editor][selection][spatial]") {
    std::vector<Vec3> vertices;
    std::vector<spatial::TriangleBvh::Triangle> faces;
    for(u32 y=0;y<100;++y)for(u32 x=0;x<100;++x) {
        const auto first=static_cast<u32>(vertices.size());
        vertices.insert(vertices.end(),{{float(x),float(y),0},{float(x)+.8F,float(y),0},{float(x),float(y)+.8F,0}});
        faces.push_back({first,first+1,first+2});
    }
    spatial::TriangleBvh tree{vertices,faces};
    for(u32 y=0;y<100;y+=7)for(u32 x=0;x<100;x+=11) {
        spatial::QueryStats stats;
        spatial::Ray3 ray{{x+.2,y+.2,5},{0,0,-2},0,10};
        const auto hit=tree.intersect(ray,&stats);REQUIRE(hit);
        CHECK(hit->triangle==y*100+x);CHECK(hit->distance==2.5);
        CHECK(stats.triangle_tests<40);
        ray.maximum=2;CHECK_FALSE(tree.intersect(ray));
        ray.maximum=10;ray.minimum=3;CHECK_FALSE(tree.intersect(ray));
        ray.minimum=0;ray.origin={x+.7,y+.7,5};CHECK_FALSE(tree.intersect(ray));
    }
    spatial::QueryStats miss;
    CHECK_FALSE(tree.intersect({{-2,-2,5},{0,0,-1}},&miss));
    CHECK(miss.bounds_tests==1);CHECK(miss.triangle_tests==0);
}

TEST_CASE("Mesh picking snapshots share immutable indices and invalidate on geometry edits", "[editor][selection][spatial]") {
    content::vmesh::Document document;
    document.vertex_count=3;
    document.vertex_fields={{"position",{content::vmesh::ScalarType::Float32,3},std::vector<f32>{-1,-1,0,1,-1,0,0,1,0}}};
    document.faces={{0,1,2}};
    auto original=editor::EditableMesh::create(std::move(document));REQUIRE(original);
    auto edited=*original;
    const auto* index=&original->picking_index();
    CHECK(&edited.picking_index()==index);
    REQUIRE(edited.set_position(0,edited.position(0)));
    CHECK(&edited.picking_index()==index); // no-op edit retains index
    for(u32 i=0;i<3;++i) {auto p=edited.position(i);p.x+=10;REQUIRE(edited.set_position(i,p));}
    CHECK(&edited.picking_index()!=index);
    const spatial::Ray3 ray{{0,0,5},{0,0,-1}};
    CHECK(original->picking_index().intersect(ray));
    CHECK_FALSE(edited.picking_index().intersect(ray));
    edited=*original;CHECK(&edited.picking_index()==index); // undo restores owned snapshot
}

TEST_CASE("Selection membership and deltas stay coherent across bulk mutation and copies", "[editor][selection]") {
    editor::Selection<u32> selected;
    std::vector<u32> ids(10000);std::iota(ids.begin(),ids.end(),0U);
    selected.select(ids);const auto all=selected;
    selected.select(std::span<const u32>{ids}.first(5000),editor::SelectionMode::remove);
    auto change=selected.changes_from(all);
    CHECK(change.added.empty());CHECK(change.removed.size()==5000);
    for(u32 i=0;i<10000;++i)CHECK(selected.contains(i)==(i>=5000));
    selected.select(selected.items());CHECK(selected.size()==5000);
    selected.retain([](u32 id){return id%2==0;});CHECK(selected.size()==2500);
    selected.clear();CHECK(selected.size()==0);CHECK_FALSE(selected.contains(9998));
    CHECK(all.size()==10000);CHECK(all.contains(9998));
    selected=all;selected.select(0,editor::SelectionMode::add);
    change=selected.changes_from(all);CHECK(change.added.empty());CHECK(change.removed.empty());
    CHECK(change.previous_active==9999);CHECK(change.active==0);CHECK_FALSE(change.empty());
}
