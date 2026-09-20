#include "../../examples/editor/editing_session.hpp"
#include "../../examples/editor/rotation_edits.hpp"
#include "../../examples/editor/preview_updates.hpp"
#include "../../examples/editor/preview_viewport.hpp"
#include "../../examples/editor/animation.hpp"
#include "../../examples/editor/blueprint_gizmos.hpp"
#include "../../examples/editor/rotation_math.hpp"
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace vng;
using namespace editor_example;
State scene() {
    content::vmesh::Document document;
    document.vertex_count = 3;
    document.vertex_fields = {{"position", {content::vmesh::ScalarType::Float32, 3},
                               std::vector<f32>{0, 0, 0, 1, 0, 0, 0, 1, 0}}};
    document.faces = {{0, 1, 2}};
    auto mesh = editor::EditableMesh::create(std::move(document));
    REQUIRE(mesh);
    return {.document = {.mesh = std::move(*mesh)}};
}
} // namespace

TEST_CASE("Ship turns compose true local axes even at Euler gimbal lock", "[editor][rotation][attitude][math]") {
    constexpr std::array<Vec3,3> axes{{{0,1,0},{1,0,0},{0,0,-1}}};
    for(auto start: {Vec3{},Vec3{35,47,19},Vec3{20,90,-45},Vec3{-10,-90,40},Vec3{350,160,-355}})
        for(auto axis:axes) for(double degrees:{0.,30.,-70.,180.,360.,400.}) {
            const auto rotated=rotation_math::turn(start,axis,degrees);
            for(unsigned i=0;i<3;++i) CHECK(std::isfinite(rotated[i]));
            if(degrees==0 || degrees==360) CHECK(rotated==start);
            // Independently use the renderer's matrix and Rodrigues' formula
            // in world space, rather than comparing our math against itself.
            const auto before=mesh_transform(ViewMode::scene,{.rotation=start});
            const auto after=mesh_transform(ViewMode::scene,{.rotation=rotated});
            Vec3 world{};
            for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)world[r]+=before[c][r]*axis[c];
            const auto s=std::sin(degrees*std::numbers::pi/180),c=std::cos(degrees*std::numbers::pi/180);
            for(unsigned col=0;col<3;++col) {
                Vec3 v{before[col][0],before[col][1],before[col][2]};
                const auto cross=gfx::camera_detail::cross(world,v);
                const auto dot=gfx::camera_detail::dot(world,v);
                for(unsigned r=0;r<3;++r) CHECK(std::abs(after[col][r]-(v[r]*c+cross[r]*s+world[r]*dot*(1-c)))<.00003);
            }
        }
}

TEST_CASE("A multi-ship attitude gesture turns each local frame and undoes atomically", "[editor][rotation][attitude][drag]") {
    auto initial=scene();
    auto document=initial.document.mesh.document();
    document.metadata={{"coordinates/forward","-Z"},{"coordinates/up","+Y"}};
    initial.document.mesh=*editor::EditableMesh::create(document);
    const auto other=instantiate(initial,BlueprintId::mesh); REQUIRE(other);
    // A differently authored ship has different local forward/up axes but the
    // same semantic capabilities, so the group still exposes attitude.
    document.metadata={{"coordinates/forward","+X"},{"coordinates/up","+Z"}};
    const auto alternate=static_cast<BlueprintId>(initial.document.next_blueprint_id++);
    initial.document.mesh_assets.push_back({alternate,"Alternate frame",*editor::EditableMesh::create(document)});
    find_instance(initial,*other)->blueprint=alternate;
    instance_transform(initial,1)->rotation={23,48,12};
    instance_transform(initial,*other)->rotation={-32,16,77};
    initial.viewport.time=2;
    initial.document.keyframe_names[2] = "Editable pose";
    REQUIRE(key_property(initial,{1,"rotation"},0,Vec3{23,48,12},timeline::Interpolation::hold));
    REQUIRE(key_property(initial,{1,"rotation"},4,Vec3{23,48,12},timeline::Interpolation::hold));
    const std::array<u32,2> selected{*other,1}; // primary is not the smallest ID
    const auto original=evaluate_scene(initial,2).instances;
    EditingSession session{initial}; session.select_keyframe(session.state().viewport.time);
    REQUIRE(session.begin_attitude(*other,selected,{PivotMode::individual}));
    for(unsigned i=0;i<100;++i) REQUIRE(session.attitude(0,static_cast<double>(i)*.5));
    REQUIRE(session.attitude(0,40));
    for(auto id:selected) {
        const auto& before=*std::ranges::find(original,id,&SceneInstance::id);
        const auto after=evaluate_instance(session.state(),*find_instance(session.state(),id),2);
        const auto basis=blueprint_attitude_axes(initial,before.blueprint); REQUIRE(basis);
        CHECK(after.transform.rotation==rotation_math::turn(before.transform.rotation,(*basis)[0],40));
        const auto before_center=instance_centers(initial,id,{})[0].world;
        const auto after_center=instance_centers(session.state(),id,{})[0].world;
        for(unsigned c=0;c<3;++c)CHECK(std::abs(before_center[c]-after_center[c])<.00001F);
        CHECK(after.transform.scale==before.transform.scale);
    }
    CHECK(session.state().document.mesh.document()==initial.document.mesh.document());
    CHECK(*find_instance(session.state(),2)==*find_instance(initial,2));
    CHECK(instance_transform(session.state(),1)->rotation==instance_transform(initial,1)->rotation);
    auto notice=session.take_changes(); REQUIRE(notice); CHECK_FALSE(notice->changes.full);
    CHECK(notice->changes.properties.size()==4);
    auto patch=capture_patch(initial.document.revision,session.state(),notice->changes); REQUIRE(patch);
    auto wire=encode_patch(*patch); REQUIRE(wire); CHECK(wire->size()<1024);
    auto decoded=decode_patch(*wire); REQUIRE(decoded);
    auto worker=initial; REQUIRE(editor_example::apply_patch(worker,*decoded));
    CHECK(worker.document.timeline==session.state().document.timeline);
    CHECK(worker.document.instances==session.state().document.instances);
    REQUIRE(session.commit()); REQUIRE(session.undo()); CHECK_FALSE(session.can_undo());
    CHECK(session.state().document.timeline==initial.document.timeline);
    CHECK(session.state().document.instances==initial.document.instances);
    REQUIRE(session.redo());
    const auto saved=encode(session.state()); REQUIRE(saved);
    const auto loaded=decode(*saved); REQUIRE(loaded);
    CHECK(loaded->document.instances==session.state().document.instances);
    REQUIRE(session.begin_attitude(*other,selected)); REQUIRE(session.attitude(2,-60)); REQUIRE(session.cancel());
    CHECK(session.state().document.instances==loaded->document.instances);
    CHECK(session.state().document.timeline==loaded->document.timeline);
    const std::array<u32,2> mixed{1,2};
    CHECK_FALSE(session.begin_attitude(1,mixed)); CHECK_FALSE(session.busy());
}

TEST_CASE("Generic rotation and scale gizmos also operate on the complete selection", "[editor][rotation][scale][drag]") {
    auto initial=scene();
    instance_transform(initial,1)->rotation={10,20,30};
    instance_transform(initial,2)->rotation={40,50,60};
    const std::array<u32,2> selected{2,1};
    EditingSession s{initial}; s.select_keyframe(s.state().viewport.time);
    REQUIRE(s.begin_rotation(2,selected,{PivotMode::individual})); REQUIRE(s.rotate({45,55,65})); REQUIRE(s.commit());
    CHECK(instance_transform(s.state(),1)->rotation==Vec3{15,25,35});
    CHECK(instance_transform(s.state(),2)->rotation==Vec3{45,55,65});
    REQUIRE(s.undo()); CHECK(s.state().document.instances==initial.document.instances);
    REQUIRE(s.begin_scale(2,selected)); REQUIRE(s.scale(2)); REQUIRE(s.commit());
    CHECK(instance_transform(s.state(),1)->scale==instance_transform(initial,1)->scale*2);
    CHECK(instance_transform(s.state(),2)->scale==2.F);
    CHECK(instance_transform(s.state(),1)->position==instance_transform(initial,1)->position);
    REQUIRE(s.undo()); CHECK(s.state().document.instances==initial.document.instances);
}

TEST_CASE("Rotation pivots preserve geometric centers and rigid group shape", "[editor][rotation][pivot]") {
    auto initial=scene();
    const auto second=instantiate(initial,BlueprintId::mesh);REQUIRE(second);
    instance_transform(initial,1)->position={-3,1,0};
    instance_transform(initial,*second)->position={5,1,0};
    instance_transform(initial,*second)->rotation={15,20,25};
    instance_transform(initial,*second)->scale=2;
    const std::array<u32,2> selected{1,*second};
    const auto before=instance_centers(initial,1,selected);
    for(auto mode:{PivotMode::selection,PivotMode::individual,PivotMode::custom}) {
        EditingSession session{initial}; session.select_keyframe(session.state().viewport.time);
        const Vec3 custom{-4,6,2};
        REQUIRE(session.begin_rotation(1,selected,{mode,custom}));
        REQUIRE(session.rotate({0,0,90}));
        const auto after=instance_centers(session.state(),1,selected);
        const auto pivot=mode==PivotMode::custom?custom:selection_center(before);
        for(std::size_t i=0;i<after.size();++i) {
            const auto p=before[i].world;
            const auto expected=mode==PivotMode::individual?p:Vec3{pivot.x-(p.y-pivot.y),pivot.y+(p.x-pivot.x),p.z};
            for(unsigned c=0;c<3;++c)CHECK(std::abs(after[i].world[c]-expected[c])<.00001F);
        }
        REQUIRE(session.commit());REQUIRE(session.undo());
        CHECK(session.state().document.instances==initial.document.instances);
        REQUIRE(session.redo());
        REQUIRE(session.begin_rotation(1,selected,{mode,custom}));
        REQUIRE(session.rotate({12,42,73}));REQUIRE(session.cancel());
        CHECK(instance_centers(session.state(),1,selected)[0].world==after[0].world);
    }
}

TEST_CASE("A live rotation drag changes no geometry and commits just one undo entry",
          "[editor][rotation][drag]") {
    EditingSession session{scene()}; session.select_keyframe(session.state().viewport.time);
    const auto& state = session.state();
    const auto original = capture_rotation(state, 1);
    REQUIRE(original);
    const auto* storage = std::get<std::vector<f32>>(state.document.mesh.document().vertex_fields[0].values).data();
    REQUIRE(session.begin_rotation(1));
    CHECK(session.active(EditGesture::rotation));
    CHECK_FALSE(session.begin_rotation(2));
    CHECK(session.active_object() == 1);
    for (unsigned i = 0; i < 1000; ++i) {
        const auto moved = session.rotate({static_cast<f32>(i) * .01F, 2, 3});
        REQUIRE(moved);
        CHECK_FALSE(session.can_undo());
        CHECK(std::get<std::vector<f32>>(state.document.mesh.document().vertex_fields[0].values).data() == storage);
    }
    const auto final = capture_rotation(state, 1);
    REQUIRE(final);
    const auto committed = session.commit();
    REQUIRE(committed);
    CHECK(*committed);
    CHECK_FALSE(session.active(EditGesture::rotation));
    REQUIRE(session.undo());
    const auto undone = capture_rotation(state, 1);
    REQUIRE(undone);
    CHECK(*undone == *original);
    CHECK_FALSE(session.can_undo());
    REQUIRE(session.redo());
    const auto redone = capture_rotation(state, 1);
    REQUIRE(redone);
    CHECK(*redone == *final);
}

TEST_CASE("Rotation drag cancellation restores the exact keyed property without undo entries",
          "[editor][rotation][drag]") {
    auto initial = scene();
    REQUIRE(key_property(initial, {1, "rotation"}, 8, Vec3{4, 5, 6}, timeline::Interpolation::hold));
    REQUIRE(key_property(initial, {2, "radius"}, 6, 2.F));
    initial.document.keyframe_names[3] = "Other content at this time";
    initial.viewport.time = 3;
    EditingSession session{std::move(initial)}; session.select_keyframe(session.state().viewport.time);
    const auto& state = session.state();
    const auto original = capture_rotation(state, 1);
    const auto original_timeline = state.document.timeline;
    const auto names = state.document.keyframe_names;
    REQUIRE(original);
    REQUIRE(session.begin_rotation(1));
    for (unsigned i = 0; i < 50; ++i) REQUIRE(session.rotate({static_cast<f32>(i), 2, 3}));
    REQUIRE(state.document.timeline.find({1, "rotation"})->keys.size() == 3);
    const auto cancelled = session.cancel();
    REQUIRE(cancelled);
    CHECK(*cancelled);
    const auto restored = capture_rotation(state, 1);
    REQUIRE(restored);
    CHECK(*restored == *original);
    CHECK(state.document.timeline == original_timeline);
    CHECK(state.document.keyframe_names == names);
    CHECK_FALSE(session.can_undo());
    REQUIRE(session.begin_rotation(1));
    const auto current = evaluate_scene(state, state.viewport.time).model_transform.rotation;
    REQUIRE(session.rotate(current));
    const auto no_op = session.commit();
    REQUIRE(no_op);
    CHECK_FALSE(*no_op);
    CHECK_FALSE(session.can_undo());
}

TEST_CASE("Rotation drag coalescing keeps delayed completed frames visible until release",
          "[editor][rotation][drag][updates]") {
    EditingSession session{scene()}; session.select_keyframe(session.state().viewport.time);
    const auto& state = session.state();
    auto worker = state;
    PreviewUpdates updates;
    updates.add(7);
    auto initial = updates.next(7, state);
    REQUIRE(initial);
    REQUIRE(*initial);
    updates.acknowledge(7, state.document.revision);
    REQUIRE(session.begin_rotation(1));
    REQUIRE(session.rotate({2, 0, 0}));
    updates.changed(session.take_changes()->changes);
    auto first = updates.next(7, state);
    REQUIRE(first);
    REQUIRE(*first);
    REQUIRE((**first).starts_with("patch\n"));
    auto edit = decode_patch(std::string_view(**first).substr(6));
    REQUIRE(edit);
    REQUIRE(editor_example::apply_patch(worker, *edit));
    for (unsigned i = 0; i < 60; ++i) {
        REQUIRE(session.rotate({3 + static_cast<f32>(i), 0, 0}));
        updates.changed(session.take_changes()->changes);
        const auto blocked = updates.next(7, state);
        REQUIRE(blocked);
        CHECK_FALSE(*blocked);
        CHECK(accepts_completed_revision(worker.document.revision, 1, 1, state.document.revision));
    }
    updates.acknowledge(7, worker.document.revision);
    const auto newest = updates.next(7, state);
    REQUIRE(newest);
    REQUIRE(*newest);
    REQUIRE((**newest).size() < 1024);
    edit = decode_patch(std::string_view(**newest).substr(6));
    REQUIRE(edit);
    REQUIRE(editor_example::apply_patch(worker, *edit));
    CHECK(instance_transform(worker, 1)->rotation == instance_transform(state, 1)->rotation);
    REQUIRE(session.commit());
    CHECK(session.can_undo());
}
