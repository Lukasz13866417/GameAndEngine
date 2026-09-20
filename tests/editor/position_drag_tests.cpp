#include "../../examples/editor/editing_session.hpp"
#include "../../examples/editor/position_edits.hpp"
#include "../../examples/editor/preview_updates.hpp"
#include "../../examples/editor/preview_viewport.hpp"
#include "../../examples/editor/animation.hpp"
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

TEST_CASE("A live position drag changes no geometry and commits just one undo entry",
          "[editor][position][drag]") {
    EditingSession session{scene()}; session.select_keyframe(session.state().viewport.time);
    const auto& state = session.state();
    const auto original = capture_position(state, 1);
    REQUIRE(original);
    const auto* storage = std::get<std::vector<f32>>(state.document.mesh.document().vertex_fields[0].values).data();
    REQUIRE(session.begin_move(1, {}));
    CHECK(session.active(EditGesture::move));
    CHECK_FALSE(session.begin_move(2, {}));
    CHECK(session.active_object() == 1);
    for (unsigned i = 0; i < 1000; ++i) {
        const auto moved = session.move({static_cast<f32>(i) * .01F, 2, 3});
        REQUIRE(moved);
        CHECK_FALSE(session.can_undo());
        CHECK(std::get<std::vector<f32>>(state.document.mesh.document().vertex_fields[0].values).data() == storage);
    }
    const auto final = capture_position(state, 1);
    REQUIRE(final);
    const auto committed = session.commit();
    REQUIRE(committed);
    CHECK(*committed);
    CHECK_FALSE(session.active(EditGesture::move));
    REQUIRE(session.undo());
    const auto undone = capture_position(state, 1);
    REQUIRE(undone);
    CHECK(*undone == *original);
    CHECK_FALSE(session.can_undo());
    REQUIRE(session.redo());
    const auto redone = capture_position(state, 1);
    REQUIRE(redone);
    CHECK(*redone == *final);
}

TEST_CASE("Position drag cancellation restores the exact keyed property without undo entries",
          "[editor][position][drag]") {
    auto initial = scene();
    REQUIRE(key_property(initial, {1, "position"}, 8, Vec3{4, 5, 6}, timeline::Interpolation::hold));
    REQUIRE(key_property(initial, {2, "radius"}, 6, 2.F));
    initial.document.keyframe_names[3] = "Other content at this time";
    initial.viewport.time = 3;
    EditingSession session{std::move(initial)}; session.select_keyframe(session.state().viewport.time);
    const auto& state = session.state();
    const auto original = capture_position(state, 1);
    const auto original_timeline = state.document.timeline;
    const auto names = state.document.keyframe_names;
    REQUIRE(original);
    REQUIRE(session.begin_move(1, {}));
    for (unsigned i = 0; i < 50; ++i) REQUIRE(session.move({static_cast<f32>(i), 2, 3}));
    REQUIRE(state.document.timeline.find({1, "position"})->keys.size() == 3);
    const auto cancelled = session.cancel();
    REQUIRE(cancelled);
    CHECK(*cancelled);
    const auto restored = capture_position(state, 1);
    REQUIRE(restored);
    CHECK(*restored == *original);
    CHECK(state.document.timeline == original_timeline);
    CHECK(state.document.keyframe_names == names);
    CHECK_FALSE(session.can_undo());
    REQUIRE(session.begin_move(1, {}));
    const auto current = evaluate_scene(state, state.viewport.time).model_transform.position;
    REQUIRE(session.move(current));
    const auto no_op = session.commit();
    REQUIRE(no_op);
    CHECK_FALSE(*no_op);
    CHECK_FALSE(session.can_undo());
}

TEST_CASE("Position drag coalescing keeps delayed completed frames visible until release",
          "[editor][position][drag][updates]") {
    EditingSession session{scene()}; session.select_keyframe(session.state().viewport.time);
    const auto& state = session.state();
    auto worker = state;
    PreviewUpdates updates;
    updates.add(7);
    auto initial = updates.next(7, state);
    REQUIRE(initial);
    REQUIRE(*initial);
    updates.acknowledge(7, state.document.revision);
    REQUIRE(session.begin_move(1, {}));
    REQUIRE(session.move({2, 0, 0}));
    updates.changed(session.take_changes()->changes);
    auto first = updates.next(7, state);
    REQUIRE(first);
    REQUIRE(*first);
    REQUIRE((**first).starts_with("position\n"));
    auto edit = decode_position_edit(std::string_view(**first).substr(9));
    REQUIRE(edit);
    REQUIRE(apply_position_edit(worker, *edit));
    for (unsigned i = 0; i < 60; ++i) {
        REQUIRE(session.move({3 + static_cast<f32>(i), 0, 0}));
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
    REQUIRE((**newest).size() == 9 + 48);
    edit = decode_position_edit(std::string_view(**newest).substr(9));
    REQUIRE(edit);
    REQUIRE(apply_position_edit(worker, *edit));
    CHECK(instance_transform(worker, 1)->position == instance_transform(state, 1)->position);
    REQUIRE(session.commit());
    CHECK(session.can_undo());
}
