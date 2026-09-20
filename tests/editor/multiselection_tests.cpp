#include <vng/editor/selection.hpp>
#include "../../examples/editor/editing_session.hpp"
#include "../../examples/editor/edit_clipboard.hpp"
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace vng;
using namespace editor_example;
State scene() {
    content::vmesh::Document document;
    document.vertex_count=3;
    document.vertex_fields={{"position",{content::vmesh::ScalarType::Float32,3},std::vector<f32>(9)}};
    document.faces={{0,1,2}};
    return {.document={.mesh=*editor::EditableMesh::create(std::move(document))}};
}
}
TEST_CASE("Selection keeps an active item and stable range anchor", "[editor][multiselect]") {
    editor::Selection<u32> s;using Mode=editor::SelectionMode;
    const std::array<u32,5> order{2,4,6,8,10};
    s.select(4);s.select(8,Mode::range,order);
    CHECK(std::vector(s.items().begin(),s.items().end())==std::vector<u32>{4,6,8});
    CHECK(s.active()==8);
    s.select(10,Mode::range,order); // original 4 anchor, not previous endpoint 8
    CHECK(s.size()==4);
    s.select(6,Mode::toggle);CHECK_FALSE(s.contains(6));CHECK(s.active()==10);
    s.select(10,Mode::toggle);CHECK(s.active()==8);
    s.select(2,Mode::add);CHECK(s.active()==2);
    s.retain([](u32 id){return id==4;});CHECK(s.active()==4);CHECK(s.size()==1);
    s.select(4,Mode::toggle);CHECK_FALSE(s.active());CHECK(s.size()==0);
    s.select(order);s.select(std::span{order}.subspan(1,2),Mode::remove);
    CHECK(s.size()==3);CHECK_FALSE(s.contains(4));CHECK_FALSE(s.contains(6));
    const auto before=s;
    s.select(s.items());CHECK(s.items().size()==before.items().size());
}
TEST_CASE("Group translation is atomic, animated, and one undo entry", "[editor][multiselect]") {
    auto initial=scene();const auto mesh=initial.document.mesh.document();
    REQUIRE(key_property(initial,{2,"position"},0,Vec3{0,0,0}));
    REQUIRE(key_property(initial,{2,"position"},4,Vec3{4,0,0}));
    initial.viewport.time=2;
    initial.document.keyframe_names[2] = "Editable pose";
    EditingSession session{std::move(initial)}; session.select_keyframe(session.state().viewport.time);
    const auto& state=session.state();
    const auto first=instance_transform(state,1)->position;
    const auto second_base=instance_transform(state,2)->position;
    const std::array<u32,2> ids{1,2};
    REQUIRE(session.begin_move(1,ids));
    REQUIRE(session.move({first.x+1,first.y+2,first.z+3}));
    CHECK(evaluate_instance(state,*find_instance(state,2),2).transform.position==Vec3{3,2,3});
    CHECK(instance_transform(state,2)->position==second_base);
    REQUIRE(session.commit());
    const auto change=session.take_changes(); REQUIRE(change);
    CHECK(change->changes.properties.size()==2);CHECK_FALSE(change->changes.full);
    REQUIRE(session.undo());CHECK(instance_transform(state,1)->position==first);
    CHECK(state.document.timeline.find({2,"position"})->keys.size()==2);
    REQUIRE(session.redo());
    CHECK(evaluate_instance(state,*find_instance(state,2),2).transform.position==Vec3{3,2,3});
    CHECK(state.document.mesh.document()==mesh);
    REQUIRE(session.begin_move(1,ids));
    const auto before=state.document.timeline;
    const auto position=instance_transform(state,1)->position;
    CHECK_FALSE(session.move({scene_coordinate_limit + 1,0,0}));
    CHECK(state.document.timeline==before);CHECK(instance_transform(state,1)->position==position);
    REQUIRE(session.move({position.x+1,position.y,position.z}));
    REQUIRE(session.cancel());CHECK(instance_transform(state,1)->position==position);
    CHECK(state.document.timeline==before);
}
TEST_CASE("Group clipboard copies instances and spaced keyframes transactionally", "[editor][multiselect]") {
    auto state=scene();EditClipboard clipboard;
    const std::array<u32,2> ids{1,2};
    REQUIRE(clipboard.copy_instances(state,ids));
    const auto pasted=clipboard.paste(state);REQUIRE(pasted);CHECK(pasted->objects.size()==2);
    CHECK(state.document.instances.size()==4);
    CHECK(find_instance(state,pasted->objects[0])->blueprint==BlueprintId::mesh);
    CHECK(find_instance(state,pasted->objects[1])->blueprint==BlueprintId::sun);
    REQUIRE(key_property(state,{1,"scale"},1,1.F));
    REQUIRE(key_property(state,{1,"scale"},3,2.F,timeline::Interpolation::hold));
    state.document.keyframe_names[1]="start";state.document.keyframe_names[3]="end";
    const std::array<f32,2> keys{3,1};
    REQUIRE(clipboard.copy_keyframes(state,keys));state.viewport.time=6;
    const auto copied=clipboard.paste(state);REQUIRE(copied);
    CHECK(copied->keyframes==std::vector<f32>{6,8});
    CHECK(keyframe_name(state,6)=="start");CHECK(keyframe_name(state,8)=="end");
    CHECK(state.document.timeline.find({1,"scale"})->keys.back().incoming==timeline::Interpolation::hold);
    const auto before=state.document.timeline;
    state.viewport.time=9;CHECK_FALSE(clipboard.paste(state));CHECK(state.document.timeline==before);
    state.viewport.time=4;CHECK_FALSE(clipboard.paste(state)); // second destination 6 conflicts
    CHECK(state.document.timeline==before);CHECK_FALSE(state.document.keyframe_names.contains(4));
}
