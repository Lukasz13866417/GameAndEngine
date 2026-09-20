#include "../../examples/editor/mesh_tools.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <iostream>
namespace {
using namespace vng;
using namespace editor_example;
struct Fixture {
    static text::Font font() {auto f=text::Font::load(VNG_TEST_FONT_PATH);REQUIRE(f);return *f;}
    ui::Screen screen{ui::dark_theme(font())};
    MeshTools tools{screen.column().width(300).height(300),screen.column()};
    State state=make();
    static State make() {
        content::vmesh::Document d;d.vertex_count=4;
        d.vertex_fields={{"position",{content::vmesh::ScalarType::Float32,3},std::vector<f32>{-1,-1,0,1,-1,0,1,1,0,-1,1,0}}};
        d.faces={{0,1,2},{0,2,3}};
        auto m=editor::EditableMesh::create(std::move(d));REQUIRE(m);
        State s{.document={.mesh=std::move(*m)}};s.viewport.mode=ViewMode::mesh;s.viewport.selected_object=1;return s;
    }
    Fixture(){tools.sync(state);}
};
}
TEST_CASE("Mesh component selection is ordered local and resets on topology/blueprint changes","[editor][ui][mesh]") {
    Fixture f;const auto revision=f.state.document.revision;
    const auto topology=f.tools.topology_revision(),selection=f.tools.selection_revision();
    f.tools.sync(f.state);f.tools.select(0,false);
    CHECK(f.tools.topology_revision()==topology);CHECK(f.tools.selection_revision()==selection);
    f.tools.select(2,true);f.tools.select(3,true);
    CHECK(f.tools.vertices(f.state.document.mesh)==std::vector<u32>{0,2,3});
    f.tools.select(2,true);f.tools.select(2,true);
    CHECK(f.tools.vertices(f.state.document.mesh)==std::vector<u32>{0,3,2});
    ++f.state.viewport.sequence;f.state.viewport.editor_camera.yaw+=10;f.tools.sync(f.state);
    CHECK(f.tools.vertices(f.state.document.mesh)==std::vector<u32>{0,3,2});
    CHECK(f.state.document.revision==revision);
    f.tools.mode(MeshSelectMode::face);f.tools.select(1,false);
    CHECK(f.tools.vertices(f.state.document.mesh)==std::vector<u32>{0,2,3});
    CHECK(f.tools.edges(f.state.document.mesh).size()==3);
    REQUIRE(f.state.document.mesh.subdivide(f.tools.edges(f.state.document.mesh)));
    ++f.state.document.revision;f.tools.sync(f.state);CHECK(f.tools.selected().empty());
}
TEST_CASE("Mesh edge and face picking share the displayed camera and component selection","[editor][ui][mesh]") {
    Fixture f;constexpr Extent2D extent{600,600}; const ui::Rect bounds{0,0,600,600};
    gfx::Camera camera; camera.set_position({0,0,5}).look_at({0,0,0});
    const auto points=project_vertices(f.state,extent,&camera);
    REQUIRE(points[0]);REQUIRE(points[1]);
    const auto a=*points[0],b=*points[1],c=*points[2];
    f.tools.mode(MeshSelectMode::edge);
    const auto edge=f.tools.pick(f.state,{(a.x+b.x)/2,(a.y+b.y)/2},extent,camera,bounds); REQUIRE(edge);
    f.tools.select(*edge,false);CHECK(f.tools.vertices(f.state.document.mesh)==std::vector<u32>{0,1});
    f.tools.mode(MeshSelectMode::face);
    const auto face=f.tools.pick(f.state,{(a.x+b.x+c.x)/3,(a.y+b.y+c.y)/3},extent,camera,bounds);REQUIRE(face);CHECK(*face==0);
    f.tools.select(*face,false);
}
TEST_CASE("Surface mode is local and does not select invisible components", "[editor][ui][mesh]") {
    Fixture f;
    const auto document=f.state.document.mesh.document();
    const auto revision=f.state.document.revision;
    f.tools.mode(MeshSelectMode::face);f.tools.select(0,false);
    REQUIRE(f.tools.hide_selected()==1);
    f.tools.select(1,false);
    const auto visibility=f.tools.visibility();
    const auto topology=f.tools.topology_revision(), mask_revision=f.tools.visibility_revision();
    f.tools.mode(MeshSelectMode::surface);f.tools.sync(f.state);
    CHECK(f.tools.selected().empty());
    CHECK(f.tools.vertices(f.state.document.mesh).empty());
    CHECK(f.tools.edges(f.state.document.mesh).empty());
    gfx::Camera camera;camera.set_position({0,0,5}).look_at({0,0,0});
    for(bool xray:{false,true}) {
        f.tools.xray(xray);
        CHECK_FALSE(f.tools.pick(f.state,{.5F,.5F},{600,600},camera,{0,0,600,600}));
        CHECK(f.tools.box(f.state,{0,0,1,1},{600,600},camera).empty());
    }
    f.tools.select(1,false);CHECK(f.tools.selected().empty());
    const std::array<u32,2> ids{0,1};
    f.tools.select(ids,editor::SelectionMode::add);CHECK(f.tools.selected().empty());
    f.tools.select_all(f.state.document.mesh);CHECK(f.tools.selected().empty());
    CHECK(f.tools.hide_selected()==0);
    CHECK(f.tools.visibility()==visibility);
    CHECK(f.tools.topology_revision()==topology);
    CHECK(f.tools.visibility_revision()==mask_revision);
    CHECK(f.state.document.mesh.document()==document);
    CHECK(f.state.document.revision==revision);
    CHECK(f.state.document.mesh_drafts.empty());
    for(auto mode:{MeshSelectMode::vertex,MeshSelectMode::edge,MeshSelectMode::face}) {
        f.tools.mode(mode);f.tools.select_all(f.state.document.mesh);
        CHECK_FALSE(f.tools.selected().empty());
        CHECK(f.tools.visibility()==visibility);
        f.tools.mode(MeshSelectMode::surface);
    }
}
TEST_CASE("Mesh context menu cancels on Escape or outside click","[editor][ui][mesh]") {
    Fixture f;f.tools.open({790,590},{800,600});CHECK(f.tools.menu_open());
    const std::array events{input::Event{.kind=input::EventKind::key_down,.key=input::Key::escape}};
    CHECK_FALSE(f.tools.poll(events));CHECK_FALSE(f.tools.menu_open());
    f.tools.open({400,400},{800,600});
    const std::array click{input::Event{.kind=input::EventKind::pointer_down,.position={20,20}}};
    CHECK_FALSE(f.tools.poll(click));CHECK_FALSE(f.tools.menu_open());
}
TEST_CASE("Whole mesh mode has no implicit component selection or visibility edits", "[editor][ui][whole-mesh]") {
    Fixture f;
    f.tools.mode(MeshSelectMode::face);f.tools.select(0,false);REQUIRE(f.tools.hide_selected()==1);
    const auto visibility=f.tools.visibility();
    f.tools.mode(MeshSelectMode::whole);f.tools.sync(f.state);
    CHECK_FALSE(f.tools.component_mode());
    CHECK(f.tools.selected().empty());CHECK(f.tools.vertices(f.state.document.mesh).empty());
    f.tools.select_all(f.state.document.mesh);f.tools.select(1,false);
    CHECK(f.tools.selected().empty());CHECK(f.tools.hide_selected()==0);
    CHECK(f.tools.visibility()==visibility);
    gfx::Camera camera;camera.set_position({0,0,5}).look_at({});
    CHECK_FALSE(f.tools.pick(f.state,{.5F,.5F},{600,600},camera,{0,0,600,600}));
    CHECK(f.tools.box(f.state,{0,0,1,1},{600,600},camera).empty());
}
TEST_CASE("Grouped component movement expands coincident records without changing selection order","[editor][ui][mesh]") {
    Fixture f;
    auto d=f.state.document.mesh.document();++d.vertex_count;
    auto& positions=std::get<std::vector<f32>>(d.vertex_fields[0].values);
    positions.insert(positions.end(),{-1,-1,0});
    auto mesh=editor::EditableMesh::create(std::move(d));REQUIRE(mesh);
    f.state.document.mesh=std::move(*mesh);++f.state.document.revision;f.tools.sync(f.state);
    f.tools.select(1,true);
    CHECK(f.tools.vertices(f.state.document.mesh)==std::vector<u32>{0,1});
    CHECK(f.tools.vertices(f.state.document.mesh,true)==std::vector<u32>{0,1,4});
    CHECK(f.tools.vertices(f.state.document.mesh)==std::vector<u32>{0,1});
}

TEST_CASE("Mesh box selection respects mode and X-ray occlusion", "[editor][ui][multiselect]") {
    Fixture f;
    gfx::Camera camera;camera.set_position({0,0,5}).look_at({0,0,0});
    constexpr Extent2D extent{600,600};
    const auto revision=f.state.document.revision;
    auto d=f.state.document.mesh.document();d.vertex_count=8;
    auto& p=std::get<std::vector<f32>>(d.vertex_fields[0].values);
    p.insert(p.end(),{-1,-1,-1,1,-1,-1,1,1,-1,-1,1,-1});
    d.faces.emplace_back(4,5,6);d.faces.emplace_back(4,6,7);
    f.state.document.mesh=*editor::EditableMesh::create(std::move(d));++f.state.document.revision;f.tools.sync(f.state);
    auto hits=f.tools.box(f.state,{0,0,1,1},extent,camera);
    CHECK(hits==std::vector<u32>{0,1,2,3});
    f.tools.xray(true);hits=f.tools.box(f.state,{0,0,1,1},extent,camera);CHECK(hits.size()==8);
    f.tools.select(hits,editor::SelectionMode::replace);CHECK(f.tools.selected().size()==8);
    const std::array<u32,2> remove{0,4};f.tools.select(remove,editor::SelectionMode::remove);CHECK(f.tools.selected().size()==6);
    f.tools.mode(MeshSelectMode::edge);CHECK(f.tools.box(f.state,{0,0,1,1},extent,camera).size()==f.tools.all_edges().size());
    f.tools.xray(false);f.tools.mode(MeshSelectMode::face);
    CHECK(f.tools.box(f.state,{0,0,1,1},extent,camera)==std::vector<u32>{0,1});
    CHECK(f.tools.box(f.state,{0,0,.01F,.01F},extent,camera).empty());
    CHECK(f.state.document.revision==revision+1); // picking did not mutate it
}

TEST_CASE("Dense spaceship box selection stays local and avoids all-pairs visibility", "[editor][ui][multiselect]") {
    Fixture f;
    const auto path=std::filesystem::path{__FILE__}.parent_path()/"../../examples/assets/spaceship.vmesh";
    auto mesh=editor::EditableMesh::load(path);REQUIRE(mesh);
    REQUIRE(mesh->size()>20000);
    f.state.document.mesh=std::move(*mesh);++f.state.document.revision;f.tools.sync(f.state);
    gfx::Camera camera;camera.set_position({7,5,10}).look_at({0,0,0});
    const auto start=std::chrono::steady_clock::now();
    const auto selected=f.tools.box(f.state,{0,0,1,1},{1200,900},camera);
    f.tools.select(selected,editor::SelectionMode::replace);
    const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    std::cout<<"Ship box selection: "<<elapsed<<" ms / "<<selected.size()<<" visible records\n";
    CHECK_FALSE(selected.empty());CHECK(selected.size()<f.state.document.mesh.size());
    f.tools.xray(true);
    const auto all=f.tools.box(f.state,{0,0,1,1},{1200,900},camera);
    CHECK(all.size()==f.state.document.mesh.size());
    f.tools.select(all,editor::SelectionMode::add);CHECK(f.tools.selected().size()==all.size());
    f.tools.select(all,editor::SelectionMode::remove);CHECK(f.tools.selected().empty());
    CHECK(f.state.document.mesh_drafts.empty());
}

TEST_CASE("H hides incident faces by component mode without authoring geometry", "[editor][ui][mesh][hide]") {
    Fixture f;const auto document=f.state.document.mesh.document();const auto revision=f.state.document.revision;
    f.tools.mode(MeshSelectMode::face);f.tools.select(0,false);
    CHECK(f.tools.hide_selected()==1);CHECK(f.tools.selected().empty());
    CHECK(f.tools.visibility().hidden_faces==std::vector<u32>{0});
    CHECK_FALSE(f.tools.visible(MeshSelectMode::vertex,1));
    CHECK(f.tools.visible(MeshSelectMode::vertex,0)); // shared with the remaining face
    f.tools.select_all(f.state.document.mesh);CHECK(f.tools.selected().size()==1);
    CHECK(f.tools.selected()[0]==1);
    CHECK(f.tools.hide_selected()==1);CHECK(f.tools.visibility().hidden_faces==std::vector<u32>{0,1});
    for(auto mode:{MeshSelectMode::vertex,MeshSelectMode::edge,MeshSelectMode::face}) {
        f.tools.mode(mode);f.tools.select_all(f.state.document.mesh);CHECK(f.tools.selected().empty());
        f.tools.select(0,false);CHECK(f.tools.selected().empty());
    }
    CHECK(f.tools.hide_selected()==0);CHECK(f.tools.reveal_hidden()==2);CHECK(f.tools.reveal_hidden()==0);
    f.tools.mode(MeshSelectMode::vertex);f.tools.select(1,false);
    CHECK(f.tools.hide_selected()==1);CHECK(f.tools.visibility().hidden_faces==std::vector<u32>{0});
    REQUIRE(f.tools.reveal_hidden()==1);
    f.tools.select(0,false);CHECK(f.tools.hide_selected()==2); // all faces incident to the shared corner
    REQUIRE(f.tools.reveal_hidden()==2);
    f.tools.mode(MeshSelectMode::edge);
    const auto edges=f.tools.all_edges();
    const auto border=std::ranges::find_if(edges,[](auto e){return (e[0]==0&&e[1]==1)||(e[0]==1&&e[1]==0);});
    REQUIRE(border!=edges.end());f.tools.select(static_cast<u32>(border-edges.begin()),false);
    CHECK(f.tools.hide_selected()==1);CHECK(f.tools.visibility().hidden_faces==std::vector<u32>{0});
    REQUIRE(f.tools.reveal_hidden()==1);
    const auto diagonal=std::ranges::find_if(edges,[](auto e){return (e[0]==0&&e[1]==2)||(e[0]==2&&e[1]==0);});
    REQUIRE(diagonal!=edges.end());f.tools.select(static_cast<u32>(diagonal-edges.begin()),false);
    CHECK(f.tools.hide_selected()==2);
    CHECK(f.state.document.mesh.document()==document);CHECK(f.state.document.revision==revision);
    CHECK(f.state.document.mesh_drafts.empty());
}
TEST_CASE("Hidden mesh faces do not occlude picks or box selection even with X-ray", "[editor][ui][mesh][hide]") {
    Fixture f;gfx::Camera camera;camera.set_position({0,0,5}).look_at({0,0,0});
    constexpr Extent2D extent{600,600};const ui::Rect bounds{0,0,600,600};
    auto d=f.state.document.mesh.document();d.vertex_count=8;
    auto& p=std::get<std::vector<f32>>(d.vertex_fields[0].values);
    p.insert(p.end(),{-1,-1,-1,1,-1,-1,1,1,-1,-1,1,-1});
    d.faces.emplace_back(4,5,6);d.faces.emplace_back(4,6,7);
    f.state.document.mesh=*editor::EditableMesh::create(std::move(d));++f.state.document.revision;f.tools.sync(f.state);
    f.tools.mode(MeshSelectMode::face);const std::array<u32,2> front{0,1};
    f.tools.select(front,editor::SelectionMode::replace);REQUIRE(f.tools.hide_selected()==2);
    for(bool xray:{false,true}) {
        f.tools.xray(xray);
        f.tools.mode(MeshSelectMode::face);
        CHECK(f.tools.pick(f.state,{.5F,.5F},extent,camera,bounds)==2);
        CHECK(f.tools.box(f.state,{0,0,1,1},extent,camera)==std::vector<u32>{2,3});
        f.tools.mode(MeshSelectMode::vertex);
        CHECK(f.tools.box(f.state,{0,0,1,1},extent,camera)==std::vector<u32>{4,5,6,7});
        f.tools.mode(MeshSelectMode::edge);CHECK(f.tools.box(f.state,{0,0,1,1},extent,camera).size()==5);
    }
    const auto mask=f.tools.visibility();const auto mask_revision=f.tools.visibility_revision();
    ++f.state.viewport.sequence;f.tools.sync(f.state);CHECK(f.tools.visibility_revision()==mask_revision);
    REQUIRE(f.state.document.mesh.set_position(4,{-1.2F,-1,-1}));++f.state.document.revision;f.tools.sync(f.state);
    CHECK(f.tools.visibility()==mask);CHECK(f.tools.visibility_revision()==mask_revision);
    const std::array edge{gfx::Edge{4,5}};REQUIRE(f.state.document.mesh.subdivide(edge));
    ++f.state.document.revision;f.tools.sync(f.state);CHECK(f.tools.visibility().hidden_faces.empty());
    CHECK(f.tools.visibility().topology!=mask.topology);
}
TEST_CASE("Mesh visibility packets are bounded by input and validate their face ordering", "[editor][ui][mesh][hide]") {
    Fixture f;f.tools.mode(MeshSelectMode::face);f.tools.select(1,false);REQUIRE(f.tools.hide_selected()==1);
    const MeshVisibilityRequest request{f.state.document.revision,f.tools.visibility()};
    const auto bytes=encode_mesh_visibility(request);REQUIRE(bytes.size()==40);
    auto decoded=decode_mesh_visibility(bytes);REQUIRE(decoded);
    CHECK(decoded->visibility==request.visibility);CHECK(decoded->required_document_revision==request.required_document_revision);
    CHECK_FALSE(decode_mesh_visibility(bytes.substr(0,35)));CHECK_FALSE(decode_mesh_visibility(bytes+"x"));
    auto invalid=request;invalid.visibility.hidden_faces={1,1};CHECK_FALSE(decode_mesh_visibility(encode_mesh_visibility(invalid)));
    invalid.visibility.hidden_faces={1,0};CHECK_FALSE(decode_mesh_visibility(encode_mesh_visibility(invalid)));
    invalid.visibility.hidden_faces.clear();invalid.required_document_revision=0;
    CHECK_FALSE(decode_mesh_visibility(encode_mesh_visibility(invalid)));
    f.tools.reset();const auto revision=f.tools.visibility_revision();f.tools.reset();CHECK(f.tools.visibility_revision()==revision);
}
