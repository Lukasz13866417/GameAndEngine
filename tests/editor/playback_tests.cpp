#include "../../examples/editor/playback.hpp"
#include "../../examples/editor/preview_updates.hpp"
#include "../../examples/editor/animation.hpp"
#include "../../examples/editor/selection.hpp"
#include "../../examples/editor/vertex_drag.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <array>
#include <limits>

namespace {
using namespace vng;
using namespace editor_example;
State source() {
    content::vmesh::Document d;
    d.vertex_count = 3;
    d.vertex_fields = {{"position",
                        {content::vmesh::ScalarType::Float32, 3},
                        std::vector<f32>{0, 0, 0, 1, 0, 0, 0, 1, 0}}};
    d.faces = {{0, 1, 2}};
    auto mesh = editor::EditableMesh::create(std::move(d));
    REQUIRE(mesh);
    return {.document = {.mesh = std::move(*mesh)}};
}
std::string packet(PreviewUpdates& updates, const State& state) {
    auto result = updates.next(1, state);
    REQUIRE(result);
    REQUIRE(*result);
    return std::move(**result);
}
} // namespace
TEST_CASE("Playback messages are compact, validated and transactional") {
    auto state = source();
    REQUIRE(key_property(state, {1, "brightness"}, 4, 2.0F));
    const auto before = state;
    PlaybackEdit edit{1, 2, 2, true};
    auto bytes = encode_playback_edit(edit);
    REQUIRE(bytes);
    REQUIRE(bytes->size() == 29);
    const auto decoded = decode_playback_edit(*bytes);
    REQUIRE(decoded);
    REQUIRE(apply_playback_edit(state, *decoded));
    REQUIRE(state.viewport.time == 2.0F);
    REQUIRE(state.document.revision == 2);
    REQUIRE(state.document.mesh.document() == before.document.mesh.document());
    REQUIRE(state.document.timeline == before.document.timeline);
    REQUIRE((*editor_example::mesh_settings(state, 1)) == (*editor_example::mesh_settings(before, 1)));
    REQUIRE(evaluate_scene(state, state.viewport.time).model.brightness == Catch::Approx(1.5F));
    REQUIRE_FALSE(apply_playback_edit(state, *decoded));
    REQUIRE_FALSE(apply_playback_edit(state, {2, 3, 11, false}));
    REQUIRE(state.viewport.time == 2.0F);
    REQUIRE(state.document.revision == 2);
    for (std::size_t n = 0; n < bytes->size(); ++n)
        REQUIRE_FALSE(decode_playback_edit(std::string_view(*bytes).substr(0, n)));
    REQUIRE_FALSE(decode_playback_edit(*bytes + "x"));
    (*bytes)[0] = 'X';
    REQUIRE_FALSE(decode_playback_edit(*bytes));
    bytes = encode_playback_edit(edit);
    bytes->back() = 2;
    REQUIRE_FALSE(decode_playback_edit(*bytes));
    REQUIRE_FALSE(encode_playback_edit({1, 1, 2, true}));
    REQUIRE_FALSE(encode_playback_edit({1, 2, -1, true}));
    REQUIRE_FALSE(encode_playback_edit({1, 2, std::numeric_limits<f32>::quiet_NaN(), true}));
}
TEST_CASE("Continuous timeline scrubbing coalesces to the latest playhead") {
    auto state = source();
    PreviewUpdates updates;
    updates.add(1);
    REQUIRE(packet(updates, state).starts_with("snapshot\n"));
    for (u32 n = 1; n <= 100; ++n) {
        state.viewport.time = static_cast<f32>(n) / 10;
        ++state.document.revision;
        updates.playback_changed();
        auto pending = updates.next(1, state);
        REQUIRE(pending);
        REQUIRE_FALSE(*pending);
    }
    updates.acknowledge(1, 1);
    auto wire = packet(updates, state);
    REQUIRE(wire.starts_with("playback\n"));
    REQUIRE(wire.size() == 38);
    auto decoded = decode_playback_edit(std::string_view(wire).substr(9));
    REQUIRE(decoded);
    REQUIRE(decoded->base_revision == 1);
    REQUIRE(decoded->revision == 101);
    REQUIRE(decoded->time == 10.0F);
    state.viewport.time = 0;
    ++state.document.revision;
    updates.playback_changed();
    updates.acknowledge(1, 101);
    wire = packet(updates, state);
    decoded = decode_playback_edit(std::string_view(wire).substr(9));
    REQUIRE(decoded);
    REQUIRE(decoded->time == 0.0F);
    updates.acknowledge(1, 102);
    REQUIRE(updates.ready(1, 102));
}
TEST_CASE("Mixed unsent playback and vertex or camera edits become a full snapshot") {
    for (const bool playback_first : {false, true}) {
        for (const bool camera : {false, true}) {
            auto state = source();
            PreviewUpdates updates;
            updates.accepted(1, 1);
            const auto other = [&] {
                ++state.document.revision;
                if (camera) {
                    state.viewport.editor_camera.yaw = 3;
                    updates.camera_changed();
                } else {
                    REQUIRE(state.document.mesh.set_position(0, {2, 0, 0}));
                    updates.changed(std::array<u32, 1>{0});
                }
            };
            const auto seek = [&] {
                ++state.document.revision;
                state.viewport.time = 6;
                updates.playback_changed();
            };
            if (playback_first) {
                seek();
                other();
            } else {
                other();
                seek();
            }
            auto wire = packet(updates, state);
            REQUIRE(wire.starts_with("snapshot\n"));
            auto decoded = decode(std::string_view(wire).substr(9));
            REQUIRE(decoded);
            REQUIRE(decoded->viewport.time == 6.0F);
            REQUIRE(decoded->viewport.editor_camera.yaw == state.viewport.editor_camera.yaw);
            REQUIRE(decoded->document.mesh.document() == state.document.mesh.document());
        }
    }
}
TEST_CASE("Timeline changes cannot masquerade as vertex-only patches") {
    auto state = source();
    auto changed = state;
    changed.document.revision++;
    changed.document.timeline_duration = 12;
    REQUIRE_FALSE(vertex_edit(state, changed));
    changed.document.timeline_duration = state.document.timeline_duration;
    REQUIRE(key_property(changed, {1, "scale"}, 4, 2.0F));
    REQUIRE_FALSE(vertex_edit(state, changed));
}
TEST_CASE("Playback loops over the duration without losing fractional time") {
    REQUIRE(advance_playback(9.9, .2, 10) == Catch::Approx(.1));
    REQUIRE(advance_playback(10, 0, 10) == 0);
    REQUIRE(advance_playback(3, 22.5, 10) == Catch::Approx(5.5));
    REQUIRE(advance_playback(3, -1, 10) == 3);
    REQUIRE(advance_playback(3, 1, 0) == 3);
}
TEST_CASE("Animated visibility and scale also drive CPU picking and vertex drags") {
    auto state = source();
    state.viewport.mode = ViewMode::scene;
    instance_transform(state, 1)->position = {};
    sun_settings(state, 2)->visible = false;
    state.viewport.selected_object = 1;
    state.viewport.editor_camera.yaw = state.viewport.editor_camera.pitch = 0;
    state.viewport.time = 0;
    REQUIRE(key_property(state, {1, "scale"}, 2, 1.4F));
    const auto first = vertex_drag_delta(state, 0, {30, 20}, {640, 480});
    REQUIRE(first);
    state.viewport.time = 2;
    const auto scaled = vertex_drag_delta(state, 0, {30, 20}, {640, 480});
    REQUIRE(scaled);
    CHECK(scaled->x == Catch::Approx(first->x * .5F));
    CHECK(scaled->y == Catch::Approx(first->y * .5F));
    const auto a = project_vertex(state, 0, {640, 480});
    const auto b = project_vertex(state, 1, {640, 480});
    const auto c = project_vertex(state, 2, {640, 480});
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(c);
    const Vec2 center{(a->x + b->x + c->x) / 3, (a->y + b->y + c->y) / 3};
    CHECK(pick_object(state, center, {640, 480}) == 1U);
    REQUIRE(key_property(state, {1, "visible"}, 2, false));
    CHECK_FALSE(pick_object(state, center, {640, 480}));
    CHECK_FALSE(vertex_drag_delta(state, 0, {30, 20}, {640, 480}));
    CHECK((*editor_example::mesh_settings(state, 1)).visible);
}
TEST_CASE("Bundled editor timeline scene is readable and actually animates") {
    auto state = load_scene(VNG_TIMELINE_SCENE_PATH);
    REQUIRE(state);
    REQUIRE(state->document.timeline.tracks().size() == 5);
    const auto properties = animation_properties(*state);
    CHECK(std::ranges::any_of(properties, [](const auto& property) {
        return property.target == timeline::Target{2, "radius"} && property.layer == "Effects" &&
               property.label == "Sun radius";
    }));
    const auto first = evaluate_scene(*state, 0);
    const auto middle = evaluate_scene(*state, 3);
    CHECK(first.model_transform.position != middle.model_transform.position);
    CHECK(evaluate_scene(*state, 7.99F).model.visible);
    CHECK_FALSE(evaluate_scene(*state, 8).model.visible);
    CHECK(evaluate_scene(*state, 9).model.visible);
    CHECK(evaluate_scene(*state, 10) == first);
}
