#include "../../examples/editor/editing_session.hpp"
#include "../../examples/editor/preview_updates.hpp"
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <map>
#include <sstream>
#include "../../examples/editor/blueprint_gizmos.hpp"
#include "../../examples/editor/effects.hpp"
#include "../../examples/editor/annotation_geometry.hpp"

namespace {
using namespace vng;
using namespace editor_example;
State scene() {
    content::vmesh::Document d;d.vertex_count=3;
    d.vertex_fields={{"position",{content::vmesh::ScalarType::Float32,3},std::vector<f32>{0,0,0,1,0,0,0,1,0}}};
    d.faces={{0,1,2}};auto mesh=editor::EditableMesh::create(std::move(d));REQUIRE(mesh);
    return {.document={.mesh=std::move(*mesh)}};
}
static_assert(editor::SceneCageProvider<Region>);
}
TEST_CASE("Region blueprint instances own independent local geometry and ordinary transforms", "[editor][region][blueprint]") {
    auto state=scene();auto a=instantiate(state,BlueprintId::region);REQUIRE(a);
    auto b=instantiate(state,BlueprintId::region);REQUIRE(b);
    const auto original=region_settings(state,*b)->boundary;
    region_settings(state,*a)->boundary.points[0].x+=.25F;
    CHECK(region_settings(state,*b)->boundary==original);
    auto* instance=find_instance(state,*a);instance->transform={{8,3,-7},{25,65,-15},1.7F};
    const auto world=region_world_snapshot(state);
    const auto restored=region_to_local(state,*find_region(world,*a));
    for(std::size_t i=0;i<restored.points.size();++i)for(unsigned c=0;c<3;++c)
        CHECK(std::abs(restored.points[i][c]-region_settings(state,*a)->boundary.points[i][c])<1e-5F);
    CHECK(state.document.instances.size()==4); // internal vertices are not instances
    const std::array<u32,2> both{*a,*b},mixed{*a,1};
    auto modes=selection_gizmos(state,both);
    CHECK(std::ranges::find(modes,GizmoMode::region_vertices)!=modes.end());
    CHECK(selection_gizmos(state,mixed)==std::vector{GizmoMode::move,GizmoMode::rotate,GizmoMode::scale,GizmoMode::free_rotate});
    auto encoded=encode(state);REQUIRE(encoded);auto decoded=decode(*encoded);REQUIRE(decoded);
    CHECK(decoded->document.instances==state.document.instances);
}
TEST_CASE("Region copy paste and boundary patches isolate siblings and undo atomically", "[editor][region][session]") {
    EditingSession session{scene()}; session.select_keyframe(session.state().viewport.time);auto a=session.instantiate(BlueprintId::region);REQUIRE(a);
    REQUIRE(session.copy_instances(std::span{&*a,1}));auto pasted=session.paste();REQUIRE(pasted);
    REQUIRE(pasted->object);const auto b=*pasted->object;REQUIRE(b!=*a);
    const auto sibling=*region_settings(session.state(),*a);
    auto worker=session.state();(void)session.take_changes();
    auto boundary=*find_region(region_snapshot(session.state()),b);boundary.points[0].y+=.5F;boundary.show_walls=true;
    REQUIRE(session.begin_region(b));REQUIRE(session.region(boundary));REQUIRE(session.commit());
    auto notice=session.take_changes();REQUIRE(notice);CHECK(notice->changes.regions.contains(b));CHECK_FALSE(notice->changes.full);
    auto patch=capture_patch(worker.document.revision,session.state(),notice->changes);REQUIRE(patch);
    auto bytes=encode_patch(*patch);REQUIRE(bytes);auto decoded=decode_patch(*bytes);REQUIRE(decoded);
    REQUIRE(apply_patch(worker,*decoded));CHECK(worker.document.instances==session.state().document.instances);
    CHECK(region_settings(worker,b)->show_walls);CHECK_FALSE(region_settings(worker,*a)->show_walls);
    auto saved=encode(session.state());REQUIRE(saved);auto loaded=decode(*saved);REQUIRE(loaded);
    CHECK(region_settings(*loaded,b)->show_walls);
    CHECK(*region_settings(session.state(),*a)==sibling);
    REQUIRE(session.undo());CHECK(*region_settings(session.state(),b)==sibling);
    REQUIRE(session.redo());CHECK(region_settings(session.state(),b)->boundary.points==boundary.points);
    const std::array ids{*a,b};REQUIRE(session.erase_instances(ids));CHECK(region_snapshot(session.state()).items.empty());
    REQUIRE(session.undo());CHECK(region_snapshot(session.state()).items.size()==2);
}

TEST_CASE("Region wall grids clip concave polygons and use per-instance visibility", "[editor][region][grid]") {
    RegionGeometry boundary;
    boundary.points={{0,0,0},{2,0,0},{2,1,0},{1,1,0},{1,2,0},{0,2,0}};
    boundary.faces={{0,1,2,3,4,5}};
    std::vector<SceneLine> lines;region_wall_grid(lines,boundary,{1,1,1,1});
    REQUIRE_FALSE(lines.empty());
    for(const auto& line:lines) for(const auto t:{0.F,.25F,.5F,.75F,1.F}) {
        const auto x=line.from.x+(line.to.x-line.from.x)*t,y=line.from.y+(line.to.y-line.from.y)*t;
        CHECK(x>=-.001F);CHECK(y>=-.001F);CHECK(x<=2.001F);CHECK(y<=2.001F);
        CHECK((x<=1.001F||y<=1.001F));
    }
    auto state=scene();auto id=instantiate(state,BlueprintId::region);REQUIRE(id);
    state.viewport.show_regions=true;
    CHECK(scene_annotation_lines(state,0).size()==12);
    region_settings(state,*id)->show_walls=true;
    CHECK(scene_annotation_lines(state,0).size()>12);
    region_settings(state,*id)->visible=false;
    CHECK(scene_annotation_lines(state,0).empty());
    state.viewport.show_world_bounds=true;CHECK(scene_annotation_lines(state,0).size()==12);
    state.viewport.mode=ViewMode::mesh;CHECK(scene_annotation_lines(state,0).empty());
}
TEST_CASE("Legacy region IDs migrate into the instance registry without colliding with meshes", "[editor][region][migration]") {
    auto state=scene();auto bytes=encode(state);REQUIRE(bytes);
    const auto version=bytes->find("editor_project = 3;");REQUIRE(version!=std::string::npos);
    bytes->replace(version,std::string("editor_project = 3;").size(),"editor_project = 2;");
    auto r=make_region(RegionShape::box,{7,4,-3},2);r.id=1;r.name="Legacy region";
    std::ostringstream legacy;legacy<<"\nregions = ";write_regions(legacy,{2,{r}});legacy<<";\n";
    auto decoded=decode(*bytes+legacy.str());REQUIRE(decoded);
    const auto regions=region_world_snapshot(*decoded);REQUIRE(regions.items.size()==1);
    CHECK(regions.items.front().id!=1);CHECK(mesh_settings(*decoded,1)!=nullptr);
    CHECK(regions.items.front().points==r.points);CHECK(regions.items.front().name==r.name);
    CHECK(decoded->viewport.selected_object==state.viewport.selected_object);
}
TEST_CASE("Region primitives describe true polyhedral outlines without face diagonals", "[editor][region]") {
    for(const auto& [shape,edges]:{std::pair{RegionShape::box,12U},{RegionShape::tetrahedron,6U},
        {RegionShape::octahedron,12U},{RegionShape::prism,9U}}) {
        auto r=make_region(shape,{3,4,5},2);r.id=1;REQUIRE(validate(r));
        const auto cage=r.scene_cage();CHECK(cage.edges.size()==edges);CHECK(cage.points.size()==r.points.size()+1);
        CHECK(cage.primary_point==region_center);CHECK(cage.object==1);
        r.points.front().x+=.75F;REQUIRE(validate(r));CHECK(r.scene_cage().edges.size()>=6);
    }
}
TEST_CASE("Region validation rejects malformed and oversized cages", "[editor][region]") {
    auto r=make_region(RegionShape::box,{},2);r.id=1;
    SECTION("bad index"){r.faces[0][0]=900;}
    SECTION("repeated corner"){r.faces[0].push_back(r.faces[0][0]);}
    SECTION("bad edge"){r.loose_edges={{0,0}};}
    SECTION("nonfinite"){r.points[0].x=std::numeric_limits<float>::infinity();}
    SECTION("excess points"){r.points.resize(max_region_points+1);}
    SECTION("text controls"){r.note=std::string(1,'\0');}
    SECTION("out of bounds"){r.points[0].x=scene_coordinate_limit*2;}
    CHECK_FALSE(validate(r));
}
TEST_CASE("Regions persist and update without changing mesh geometry or timeline", "[editor][region][session]") {
    auto initial=scene();auto worker=initial;EditingSession session{initial}; session.select_keyframe(session.state().viewport.time);
    const auto id=session.add_region(RegionShape::prism,{0,0,0},2);REQUIRE(id);
    auto notice=session.take_changes();REQUIRE(notice);CHECK(notice->changes.full);
    auto snapshot=encode(session.state());REQUIRE(snapshot);auto copied=decode(*snapshot);REQUIRE(copied);worker=std::move(*copied);
    CHECK(region_snapshot(worker)==region_snapshot(session.state()));
    REQUIRE(session.begin_region(*id));
    const auto original=*find_region(region_snapshot(session.state()),*id);
    auto changed=original;changed.name="Hangar \"A\"";changed.note="TODO: ships here\nThen add lights \\ cables";
    for(unsigned i=1;i<10;++i) {changed.points[0].x=original.points[0].x+static_cast<float>(i)*.1F;REQUIRE(session.region(changed));}
    REQUIRE(session.commit());REQUIRE(session.undo());CHECK(*find_region(region_snapshot(session.state()),*id)==original);
    REQUIRE(session.redo());CHECK(*find_region(region_snapshot(session.state()),*id)==changed);
    REQUIRE(session.begin_region(*id));auto collapsed=changed;for(auto& p:collapsed.points)p.z=0;
    CHECK(session.region(collapsed));REQUIRE(session.cancel());
    CHECK(*find_region(region_snapshot(session.state()),*id)==changed);
    auto encoded=encode_scene(session.state());REQUIRE(encoded);auto restored=decode(*encoded);REQUIRE(restored);
    CHECK(region_snapshot(*restored)==region_snapshot(session.state()));
    CHECK(restored->document.timeline==initial.document.timeline);
    CHECK(restored->document.mesh.document()==initial.document.mesh.document());
    REQUIRE(session.erase_region(*id));CHECK(region_snapshot(session.state()).items.empty());
    REQUIRE(session.undo());CHECK(region_snapshot(session.state()).items.size()==1);
    REQUIRE(session.undo());REQUIRE(session.undo());CHECK(region_snapshot(session.state()).items.empty());
    auto next=session.add_region(RegionShape::box,{},1);REQUIRE(next);CHECK(*next>*id);
}
TEST_CASE("Malformed region patches are atomic and legacy patch versions remain readable", "[editor][region][patch]") {
    auto state=scene();DocumentPatch patch{1,2};
    auto r=make_region(RegionShape::octahedron,{},1);r.id=1;patch.regions={{.id=r.id,.replacement=r}};
    auto bytes=encode_patch(patch);REQUIRE(bytes);
    for(std::size_t n=0;n<bytes->size();++n)REQUIRE_FALSE(decode_patch(std::string_view(*bytes).substr(0,n)));
    std::size_t topology_bytes=8;
    for(const auto& face:r.faces)topology_bytes+=4+face.size()*4;
    // Empty-patch header is 40 bytes: magic/version, revisions, counts,
    // duration flag, world-bounds flag, region-presence flag. V6 removed the
    // registry allocator and added a per-region replacement flag.
    auto legacy=*bytes;
    legacy.resize(legacy.size()-8); // V8/V9 append mesh-patch/placement counts.
    // V7 adds the wall flag after the two length-prefixed strings.
    legacy.erase(49+8+r.name.size()+r.note.size(),1);
    auto v6=legacy;v6[8]=6;REQUIRE(decode_patch(v6));
    legacy.insert(40,std::string{"\x02\0\0\0",4});legacy.erase(52,1);
    for (const unsigned version : {3U,4U,5U}) {
        auto wire=version==3?legacy.substr(0,legacy.size()-topology_bytes):legacy;
        wire[8]=static_cast<char>(version);
        auto migrated=decode_patch(wire);REQUIRE(migrated);CHECK(migrated->regions==patch.regions);
    }
    patch.regions[0].replacement->id=0;CHECK_FALSE(apply_patch(state,patch));CHECK(state.document.revision==1);CHECK(region_snapshot(state).items.empty());
    patch.regions.clear();bytes=encode_patch(patch);REQUIRE(bytes);
    auto v2=*bytes;v2[8]=2;v2.resize(v2.size()-9);REQUIRE(decode_patch(v2));
    auto v1=v2;v1[8]=1;v1.pop_back();REQUIRE(decode_patch(v1));
}
TEST_CASE("Region topology preserves dents and allows open intermediate shapes", "[editor][region]") {
    auto r=make_region(RegionShape::box,{},2);r.id=1;
    const auto faces=r.faces;const auto edges=r.edges();
    r.points[0]={0,0,0};REQUIRE(validate(r));
    CHECK(r.faces==faces);CHECK(r.scene_cage().edges==edges);
    r.faces.clear();REQUIRE(validate(r));
    auto state=scene();auto id=instantiate(state,BlueprintId::region);REQUIRE(id);
    region_settings(state,*id)->boundary=r;
    auto encoded=encode_scene(state);REQUIRE(encoded);auto restored=decode(*encoded);REQUIRE(restored);
    CHECK(region_snapshot(*restored)==region_snapshot(state)); // explicit empty faces must not re-hull
}
TEST_CASE("Region subdivision shares edge vertices and preserves adjacent boundaries", "[editor][region]") {
    using Kind=editor::CageElement;
    auto r=make_region(RegionShape::box,{},2);r.id=1;
    const std::array<u32,1> face{0};
    auto created=edit_region_geometry(r,RegionAction::subdivide,Kind::face,face);REQUIRE(created);
    CHECK(created->size()==5);CHECK(r.points.size()==13);CHECK(r.faces.size()==9);
    std::map<RegionEdge,unsigned> incidence;
    for(const auto& f:r.faces)for(std::size_t i=0;i<f.size();++i) {
        auto a=f[i],b=f[(i+1)%f.size()];if(a>b)std::swap(a,b);++incidence[{a,b}];
    }
    for(const auto& [edge,count]:incidence){(void)edge;CHECK(count==2);}
    const auto topology=r.faces;r.points[created->back()]={0,0,0};REQUIRE(validate(r));CHECK(r.faces==topology);
    DocumentPatch patch{1,2};patch.regions={{.id=r.id,.replacement=r}};
    auto bytes=encode_patch(patch);REQUIRE(bytes);auto decoded=decode_patch(*bytes);REQUIRE(decoded);CHECK(*decoded==patch);
    const auto original=r;
    const std::array<u32,1> bad{99999};CHECK_FALSE(edit_region_geometry(r,RegionAction::erase,Kind::vertex,bad));CHECK(r==original);
}
TEST_CASE("Region geometry controls add fill align and delete without touching scene meshes", "[editor][region]") {
    using Kind=editor::CageElement;
    auto r=make_region(RegionShape::box,{},2);r.id=1;
    auto added=edit_region_geometry(r,RegionAction::add_vertex,Kind::vertex,{});REQUIRE(added);CHECK(r.points.size()==9);
    const std::array<u32,2> pair{0,added->front()};REQUIRE(edit_region_geometry(r,RegionAction::fill,Kind::vertex,pair));
    CHECK(r.loose_edges.size()==1);
    const std::array<u32,3> boundary{0,1,added->front()};
    REQUIRE(edit_region_geometry(r,RegionAction::fill,Kind::vertex,boundary));CHECK(r.faces.size()==7);
    const auto a=r.points[0],b=r.points[1];
    REQUIRE(edit_region_geometry(r,RegionAction::align,Kind::vertex,boundary));
    CHECK(r.points[0]==a);CHECK(r.points[1]==b);CHECK(r.points[8].y==a.y);CHECK(r.points[8].z==a.z);
    REQUIRE(edit_region_geometry(r,RegionAction::erase,Kind::vertex,*added));
    CHECK(r.points.size()==8);CHECK(r.faces.size()==6);CHECK(r.loose_edges.empty());REQUIRE(validate(r));
    const std::array<u32,1> first{0};
    REQUIRE(edit_region_geometry(r,RegionAction::erase,Kind::face,first));CHECK(r.faces.size()==5);
    REQUIRE(edit_region_geometry(r,RegionAction::erase,Kind::edge,first));REQUIRE(validate(r));
}
TEST_CASE("Legacy region documents migrate once and explicit topology remains authoritative", "[editor][region]") {
    auto document=content::parse_document(R"(regions 1.0
        next_id = 2;
        items = [{id=1; name="Legacy"; note="TODO";
            points=[[1,1,1],[-1,-1,1],[-1,1,-1],[1,-1,-1]];}];
    )");
    REQUIRE(document);
    auto regions=document->read([](auto r){return read_regions(r);});REQUIRE(regions);
    REQUIRE(regions->items.size()==1);CHECK(regions->items[0].faces.size()==4);
    auto state=scene();auto id=instantiate(state,BlueprintId::region);REQUIRE(id);
    region_settings(state,*id)->boundary=regions->items[0];
    region_settings(state,*id)->boundary.points[0]={0,0,0};
    auto encoded=encode_scene(state);REQUIRE(encoded);auto restored=decode(*encoded);REQUIRE(restored);
    CHECK(region_snapshot(*restored)==region_snapshot(state));
}

TEST_CASE("Region patches preserve precise instance and point scopes", "[editor][region][patch]") {
    auto worker=scene();
    const auto a=instantiate(worker,BlueprintId::region),b=instantiate(worker,BlueprintId::region);
    REQUIRE(a);REQUIRE(b);
    auto authored=worker;
    region_settings(authored,*a)->boundary.points[2]={1,2,3};
    region_settings(authored,*a)->boundary.points[5]={4,5,6};
    authored.document.revision=2;
    DocumentChanges changes{.regions={{*a,{.points={2,5}}}}};
    const auto patch=capture_patch(1,authored,changes);REQUIRE(patch);
    REQUIRE(patch->regions.size()==1);
    CHECK(patch->regions[0].id==*a);
    CHECK_FALSE(patch->regions[0].replacement);
    CHECK(patch->regions[0].point_count==8);
    CHECK(patch->regions[0].points==std::vector<RegionPointEdit>{{2,{1,2,3}},{5,{4,5,6}}});
    CHECK(changes_of(*patch).regions==changes.regions);
    const auto bytes=encode_patch(*patch);REQUIRE(bytes);CHECK(bytes->size()<128);
    const auto decoded=decode_patch(*bytes);REQUIRE(decoded);CHECK(*decoded==*patch);
    for(std::size_t n=0;n<bytes->size();++n)
        REQUIRE_FALSE(decode_patch(std::string_view(*bytes).substr(0,n)));
    const auto* point_storage=region_settings(worker,*a)->boundary.points.data();
    const auto* face_storage=region_settings(worker,*a)->boundary.faces.data();
    const auto* sibling_storage=region_settings(worker,*b)->boundary.points.data();
    REQUIRE(apply_patch(worker,*decoded));
    CHECK(worker.document.instances==authored.document.instances);
    CHECK(region_settings(worker,*a)->boundary.points.data()==point_storage);
    CHECK(region_settings(worker,*a)->boundary.faces.data()==face_storage);
    CHECK(region_settings(worker,*b)->boundary.points.data()==sibling_storage);

    changes.regions[*a]={.whole=true};
    authored.document.revision=3;
    find_instance(authored,*a)->name="Hangar";
    region_settings(authored,*a)->note="Only this boundary";
    region_settings(authored,*a)->boundary.points.push_back({7,8,9});
    const auto replacement=capture_patch(2,authored,changes);REQUIRE(replacement);
    REQUIRE(replacement->regions.size()==1);
    REQUIRE(replacement->regions[0].replacement);
    CHECK(replacement->regions[0].replacement->name=="Hangar");
    CHECK(changes_of(*replacement).regions==changes.regions);
    REQUIRE(apply_patch(worker,*replacement));
    CHECK(worker.document.instances==authored.document.instances);
    CHECK(region_settings(worker,*b)->boundary.points.data()==sibling_storage);
}

TEST_CASE("Region change coalescing unions points and whole boundaries dominate", "[editor][region][patch]") {
    DocumentChanges changes{.regions={{3,{.points={1,4}}}}};
    changes.merge(DocumentChanges{.regions={{3,{.points={2,4}}},{4,{.points={0}}}}});
    CHECK(changes.regions.at(3).points==std::set<u32>{1,2,4});
    CHECK(changes.regions.at(4).points==std::set<u32>{0});
    changes.merge(DocumentChanges{.regions={{3,{.whole=true}}}});
    CHECK(changes.regions.at(3).whole);CHECK(changes.regions.at(3).points.empty());
    changes.merge(DocumentChanges{.regions={{3,{.points={7}}}}});
    CHECK(changes.regions.at(3).whole);CHECK(changes.regions.at(3).points.empty());
    CHECK_FALSE(changes.regions.at(4).whole);
    changes.merge(DocumentChanges{.full=true});
    CHECK(changes.full);CHECK(changes.regions.empty());
}

TEST_CASE("Malformed sparse region patches reject every category atomically", "[editor][region][patch]") {
    auto state=scene();
    const auto id=instantiate(state,BlueprintId::region);REQUIRE(id);
    DocumentPatch patch{1,2};
    patch.vertices={{1,2,{{0,{4,5,6}}}}};
    patch.world_bounds=WorldBounds{{-200,-200,-200},{200,200,200}};
    patch.regions={{.id=*id,.point_count=8,.points={{0,{1,2,3}}}}};
    SECTION("unknown instance"){patch.regions.push_back({.id=999,.point_count=8,.points={{0,{1,2,3}}}});}
    SECTION("duplicate instance"){patch.regions.push_back(patch.regions.front());}
    SECTION("duplicate point"){patch.regions.front().points.push_back(patch.regions.front().points.front());}
    SECTION("missing point"){patch.regions.front().points.front().index=8;}
    SECTION("changed point count"){patch.regions.front().point_count=7;}
    SECTION("nonfinite"){patch.regions.front().points.front().position.x=std::numeric_limits<f32>::quiet_NaN();}
    SECTION("out of bounds"){patch.regions.front().points.front().position.y=scene_coordinate_limit*2;}
    SECTION("empty sparse edit"){patch.regions.front().points.clear();}
    SECTION("mixed replacement and points"){
        auto value=make_region(RegionShape::box,{},1);value.id=*id;patch.regions.front().replacement=value;
    }
    const auto before=encode(state);REQUIRE(before);
    CHECK_FALSE(apply_patch(state,patch));
    const auto after=encode(state);REQUIRE(after);CHECK(*after==*before);
}

TEST_CASE("Sparse region packets coalesce behind ACKs without growing to whole boundaries", "[editor][region][patch]") {
    auto authored=scene();const auto id=instantiate(authored,BlueprintId::region);REQUIRE(id);
    auto worker=authored;
    PreviewUpdates updates;updates.add(1);updates.accepted(1,1);
    auto move=[&](u32 index,Vec3 position) {
        region_settings(authored,*id)->boundary.points[index]=position;
        ++authored.document.revision;
        updates.changed(DocumentChanges{.regions={{*id,{.points={index}}}}});
    };
    auto receive=[&] {
        auto next=updates.next(1,authored);REQUIRE(next);REQUIRE(*next);
        REQUIRE((**next).starts_with("patch\n"));
        auto patch=decode_patch(std::string_view(**next).substr(6));REQUIRE(patch);
        REQUIRE(patch->regions.size()==1);CHECK_FALSE(patch->regions[0].replacement);
        REQUIRE(apply_patch(worker,*patch));return *patch;
    };
    move(0,{1,2,3});const auto first=receive();
    CHECK(first.regions[0].points.size()==1);
    move(2,{4,5,6});move(0,{7,8,9});move(2,{10,11,12});
    auto blocked=updates.next(1,authored);REQUIRE(blocked);CHECK_FALSE(*blocked);
    updates.acknowledge(1,first.revision);
    const auto second=receive();CHECK(second.base_revision==first.revision);
    CHECK(second.regions[0].points==std::vector<RegionPointEdit>{{0,{7,8,9}},{2,{10,11,12}}});
    CHECK(worker.document.instances==authored.document.instances);
    updates.acknowledge(1,second.revision);CHECK(updates.ready(1,authored.document.revision));
}
