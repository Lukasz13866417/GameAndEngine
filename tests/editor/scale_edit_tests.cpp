#include "../../examples/editor/editing_session.hpp"
#include "../../examples/editor/preview_updates.hpp"
#include "../../examples/editor/animation.hpp"
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
using namespace vng;
using namespace editor_example;
State scene() {
    content::vmesh::Document d;
    d.vertex_count=3;
    d.vertex_fields={{"position",{content::vmesh::ScalarType::Float32,3},std::vector<f32>(9)}};
    d.faces={{0,1,2}};
    auto mesh=editor::EditableMesh::create(std::move(d)); REQUIRE(mesh);
    State state{.document={.mesh=std::move(*mesh)}};
    state.viewport.selected_object=1;
    return state;
}
const void* storage(const State& s) {
    return std::get<std::vector<f32>>(s.document.mesh.document().vertex_fields[0].values).data();
}
}
TEST_CASE("Scale packets are bounded scalar properties, not serialized scenes", "[editor][scale]") {
    ScaleEdit edit{1,2,{1,1.2F,{}}};
    const auto bytes=encode_scale_edit(edit); REQUIRE(bytes);
    CHECK(bytes->size()==40);
    REQUIRE(decode_scale_edit(*bytes)); CHECK(*decode_scale_edit(*bytes)==edit);
    for(std::size_t n=0;n<bytes->size();++n) CHECK_FALSE(decode_scale_edit(std::string_view(*bytes).substr(0,n)));
    CHECK_FALSE(decode_scale_edit(*bytes+"x"));
    for(const auto value:{0.F,-1.F,max_instance_scale+1.F,std::numeric_limits<f32>::quiet_NaN()}) {
        edit.scale.base_scale=value; CHECK_FALSE(encode_scale_edit(edit));
    }
    auto state=scene(); const auto ptr=storage(state);
    REQUIRE(apply_scale_edit(state,{1,2,{1,2,{}}})); CHECK(storage(state)==ptr);
    CHECK_FALSE(apply_scale_edit(state,{1,3,{1,1,{}}}));
    CHECK(instance_transform(state,1)->scale==2.F);
}
TEST_CASE("Adjustable scale limits allow large transforms through history files and preview packets", "[editor][scale][scale-limits]") {
    auto initial=scene();
    initial.document.keyframe_names[0.F]="Start";
    find_instance(initial,1)->transform.scale=1.F;
    EditingSession session{std::move(initial)};session.select_keyframe(0.F);
    const auto ptr=storage(session.state());
    REQUIRE(session.begin_scale(1,{},true));
    REQUIRE(session.scale_factor(10.F));
    CHECK(evaluate_transform(session.state(),*find_instance(session.state(),1),0).scale==3.F);
    REQUIRE(session.scale_factor(10.F,-1,ScaleLimits{.instance=25}));
    CHECK(evaluate_transform(session.state(),*find_instance(session.state(),1),0).scale==10.F);
    REQUIRE(session.commit());
    CHECK(storage(session.state())==ptr);
    auto bytes=encode(session.state());REQUIRE(bytes);
    auto loaded=decode(*bytes);REQUIRE(loaded);
    CHECK(evaluate_transform(*loaded,*find_instance(*loaded,1),0).scale==10.F);
    auto packet=encode_scale_edit(*scale_edit(1,session.state(),1));REQUIRE(packet);
    REQUIRE(decode_scale_edit(*packet));
    REQUIRE(session.undo());REQUIRE(session.redo());
    REQUIRE(session.begin_scale(1,{},true));
    REQUIRE(session.scale_factor(30.F,0,ScaleLimits{.axis=40}));
    REQUIRE(session.commit());
    CHECK(evaluate_transform(session.state(),*find_instance(session.state(),1),0).axis_scale.x==30.F);
    REQUIRE(session.begin_scale(1,{},true));
    CHECK(session.scale_factor(1.F)); // Default lower cap must not snap existing objects.
    CHECK(evaluate_transform(session.state(),*find_instance(session.state(),1),0).scale==10.F);
    CHECK_FALSE(session.scale_factor(2.F,-1,ScaleLimits{.instance=0}));
    REQUIRE(session.cancel());
}
TEST_CASE("Scale gestures and undo preserve geometry and animated property semantics", "[editor][scale]") {
    auto initial=scene();
    REQUIRE(key_property(initial,{1,"scale"},0,.5F,timeline::Interpolation::hold));
    REQUIRE(key_property(initial,{1,"scale"},4,1.F,timeline::Interpolation::linear));
    initial.viewport.time=2;
    initial.document.keyframe_names[2] = "Editable pose";
    EditingSession session{std::move(initial)}; session.select_keyframe(session.state().viewport.time);
    const auto& state=session.state();
    const auto ptr=storage(state); const auto original=*capture_scale(state,1);
    REQUIRE(session.begin_scale(1)); REQUIRE(session.scale(1.5F)); REQUIRE(session.scale(2.F));
    REQUIRE(session.commit());
    CHECK(storage(state)==ptr); CHECK(session.take_changes()->changes.properties.contains({1,"scale"}));
    CHECK(instance_transform(state,1)->scale==original.base_scale);
    CHECK(state.document.timeline.sample({1,"scale"},2)==timeline::Value{2.F});
    auto wire=encode_scale_edit(*scale_edit(1,state,1)); REQUIRE(wire);
    REQUIRE(decode_scale_edit(*wire)); CHECK(decode_scale_edit(*wire)->scale==*capture_scale(state,1));
    REQUIRE(session.undo()); CHECK(*capture_scale(state,1)==original); CHECK(storage(state)==ptr);
    REQUIRE(session.redo()); CHECK(storage(state)==ptr);
    const auto committed=*capture_scale(state,1);
    REQUIRE(session.begin_scale(1)); REQUIRE(session.scale(.1F)); REQUIRE(session.cancel());
    CHECK(*capture_scale(state,1)==committed); CHECK(storage(state)==ptr);
    REQUIRE(session.undo()); CHECK(*capture_scale(state,1)==original);
}
TEST_CASE("Scale backpressure coalesces and sends the final small update", "[editor][scale]") {
    auto state=scene(); PreviewUpdates updates; updates.add(1); updates.accepted(1,1);
    REQUIRE(apply_scale_value(state,1,1.2F)); ++state.document.revision; updates.scale_changed(1);
    auto first=updates.next(1,state); REQUIRE(first); REQUIRE(*first); CHECK((**first).starts_with("scale\n"));
    REQUIRE(apply_scale_value(state,1,1.8F)); ++state.document.revision; updates.scale_changed(1);
    REQUIRE(apply_scale_value(state,1,2.F)); ++state.document.revision; updates.scale_changed(1);
    CHECK_FALSE(*updates.next(1,state)); updates.acknowledge(1,2);
    auto final=updates.next(1,state); REQUIRE(final); REQUIRE(*final); CHECK((**final).size()==46);
    const auto edit=decode_scale_edit(std::string_view(**final).substr(6)); REQUIRE(edit);
    CHECK(edit->scale.base_scale==2.F); CHECK(edit->base_revision==2); CHECK(edit->revision==4);
}
TEST_CASE("Pointer scaling limits a group together without changing its proportions", "[editor][scale][scale-limits]") {
    auto initial=scene();initial.document.keyframe_names[0.F]="Start";
    find_instance(initial,1)->transform.scale=1;
    find_instance(initial,2)->transform.scale=2;
    EditingSession session{std::move(initial)};session.select_keyframe(0.F);
    const std::array<u32,2> selection{1,2};
    REQUIRE(session.begin_scale(1,selection));
    REQUIRE(session.scale(20.F,8.F));
    CHECK(evaluate_transform(session.state(),*find_instance(session.state(),1),0).scale==4.F);
    CHECK(evaluate_transform(session.state(),*find_instance(session.state(),2),0).scale==8.F);
    REQUIRE(session.commit());
    REQUIRE(session.undo());
    CHECK(evaluate_transform(session.state(),*find_instance(session.state(),2),0).scale==2.F);
}
