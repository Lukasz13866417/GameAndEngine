#include "../../examples/editor/position_edits.hpp"
#include "../../examples/editor/preview_updates.hpp"
#include "../../examples/editor/project.hpp"
#include "../../examples/editor/animation.hpp"

#include <catch2/catch_test_macros.hpp>
#include <array>
#include <bit>
#include <limits>

namespace {
using namespace vng;
using namespace editor_example;
State scene(std::size_t vertices = 3) {
    content::vmesh::Document document;
    document.vertex_count = vertices;
    document.vertex_fields = {{"position", {content::vmesh::ScalarType::Float32, 3},
                               std::vector<f32>(vertices * 3)}};
    document.faces = {{0, 1, 2}};
    auto mesh = editor::EditableMesh::create(std::move(document));
    REQUIRE(mesh);
    return {.document = {.mesh = std::move(*mesh)}};
}
const f32* storage(const State& state) {
    return std::get<std::vector<f32>>(state.document.mesh.document().vertex_fields[0].values).data();
}
void word(std::string& bytes, std::size_t offset, u64 value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        bytes[offset + i] = static_cast<char>((value >> (i * 8U)) & 255U);
}
PositionEdit keyed_edit() {
    return {1, 2, {1, {1, 2, 3},
        timeline::Track{{1, "position"}, "Motion", "Objects",
                         {{0, Vec3{1, 2, 3}, timeline::Interpolation::hold},
                          {4, Vec3{4, 5, 6}, timeline::Interpolation::linear}}}}};
}
std::string encoded(const PositionEdit& edit) {
    const auto result = encode_position_edit(edit);
    REQUIRE(result);
    return *result;
}
std::string packet(PreviewUpdates& updates, const State& state) {
    auto result = updates.next(1, state);
    REQUIRE(result);
    REQUIRE(*result);
    return std::move(**result);
}
} // namespace

TEST_CASE("Unkeyed position packets are deterministic 48-byte complete properties",
          "[editor][position][wire]") {
    const PositionEdit edit{0x010203040506ULL, 0x010203040510ULL, {17, {-1, .5F, 3}, {}}};
    const auto wire = encoded(edit);
    REQUIRE(wire.size() == 48);
    CHECK(wire.substr(0, 8) == std::string("VNGPOS\0\1", 8));
    CHECK(static_cast<unsigned char>(wire[8]) == 6);
    CHECK(static_cast<unsigned char>(wire[24]) == 17);
    CHECK(static_cast<unsigned char>(wire[40]) == 0);
    CHECK(static_cast<unsigned char>(wire[44]) == 0);
    const auto decoded = decode_position_edit(wire);
    REQUIRE(decoded);
    CHECK(*decoded == edit);
    CHECK(encoded(*decoded) == wire);
    for (std::size_t n = 0; n < wire.size(); ++n)
        CHECK_FALSE(decode_position_edit(std::string_view(wire).substr(0, n)));
    CHECK_FALSE(decode_position_edit(wire + 'x'));
}

TEST_CASE("Keyed position wire preserves the complete property track and rejects malformed payloads",
          "[editor][position][wire]") {
    const auto edit = keyed_edit();
    const auto wire = encoded(edit);
    CHECK(wire.size() == 48 + 8 + 6 + 7 + 2 * 20);
    const auto decoded = decode_position_edit(wire);
    REQUIRE(decoded);
    CHECK(*decoded == edit);
    for (std::size_t n = 0; n < wire.size(); ++n)
        CHECK_FALSE(decode_position_edit(std::string_view(wire).substr(0, n)));
    const auto rejected = [&](std::size_t offset, u64 value, unsigned width) {
        auto corrupt = wire;
        word(corrupt, offset, value, width);
        CHECK_FALSE(decode_position_edit(corrupt));
    };
    rejected(0, 'X', 1);
    rejected(7, 2, 1);
    rejected(8, 0, 8);
    rejected(16, 1, 8);
    rejected(16, u64{1} << 53, 8);
    rejected(24, 0, 4);
    rejected(28, std::bit_cast<u32>(scene_coordinate_limit + 1), 4);
    rejected(32, std::bit_cast<u32>(std::numeric_limits<f32>::quiet_NaN()), 4);
    rejected(40, 0, 4);
    rejected(40, 4097, 4);
    rejected(44, 2, 4);
    rejected(48, 257, 4);
    rejected(52, 129, 4);
    rejected(56, 0, 1); // NUL in track label.
    rejected(69, std::bit_cast<u32>(5.F), 4); // Keys no longer sorted.
    rejected(85, 2, 4); // Unknown interpolation.
    auto invalid = edit;
    invalid.position.track->target.object = 7;
    CHECK_FALSE(encode_position_edit(invalid));
    invalid = edit;
    invalid.position.track->keys[0].value = 1.F;
    CHECK_FALSE(encode_position_edit(invalid));
    invalid = edit;
    invalid.position.track->keys[1].time = 0;
    CHECK_FALSE(encode_position_edit(invalid));
    invalid = edit;
    invalid.position.track->keys.clear();
    CHECK_FALSE(encode_position_edit(invalid));
}

TEST_CASE("Position edits are atomic and never reallocate geometry or unrelated tracks",
          "[editor][position]") {
    auto state = scene(65536);
    REQUIRE(state.document.timeline.set({2, "radius"}, {0, 1.F}));
    REQUIRE(state.document.timeline.set({2, "radius"}, {3, 2.F}));
    const auto* geometry = storage(state);
    const auto* unrelated = state.document.timeline.find({2, "radius"})->keys.data();
    const auto original = state;
    auto edit = keyed_edit();
    REQUIRE(apply_position_edit(state, edit));
    CHECK(instance_transform(state, 1)->position == Vec3{1, 2, 3});
    CHECK(state.document.revision == 2);
    CHECK(storage(state) == geometry);
    CHECK(state.document.timeline.find({2, "radius"})->keys.data() == unrelated);
    CHECK(state.document.mesh.document() == original.document.mesh.document());
    const auto committed = encode(state);
    REQUIRE(committed);
    CHECK_FALSE(apply_position_edit(state, edit));
    edit.base_revision = 2;
    edit.revision = 3;
    edit.position.track->keys.back().time = state.document.timeline_duration + 1;
    CHECK_FALSE(apply_position_edit(state, edit));
    edit = {2, 3, {99, {}, {}}};
    CHECK_FALSE(apply_position_edit(state, edit));
    CHECK(*encode(state) == *committed);
    CHECK(storage(state) == geometry);
    CHECK(state.document.timeline.find({2, "radius"})->keys.data() == unrelated);
    const PositionEdit unkeyed{2, 3, {1, {-1, -2, -3}, {}}};
    REQUIRE(apply_position_edit(state, unkeyed));
    CHECK_FALSE(state.document.timeline.find({1, "position"}));
    CHECK(state.document.timeline.find({2, "radius"})->keys.data() == unrelated);
    CHECK(storage(state) == geometry);
}

TEST_CASE("Live local positions preserve keyed semantics and cancel only their own property",
          "[editor][position]") {
    auto state = scene();
    REQUIRE(state.document.timeline.set({2, "radius"}, {0, 1.F}));
    const auto* unrelated = state.document.timeline.find({2, "radius"})->keys.data();
    const auto* geometry = storage(state);
    auto before = capture_position(state, 1);
    REQUIRE(before);
    auto changed = apply_position_value(state, 1, {3, 2, 1});
    REQUIRE(changed);
    CHECK(*changed);
    CHECK(state.document.revision == 1);
    CHECK_FALSE(*apply_position_value(state, 1, {3, 2, 1}));
    REQUIRE(restore_position(state, *before));
    CHECK(*capture_position(state, 1) == *before);
    CHECK_FALSE(apply_position_value(state, 1, {scene_coordinate_limit + 1, 0, 0}));
    CHECK_FALSE(apply_position_value(state, 77, {}));
    REQUIRE(state.document.timeline.replace_track(*keyed_edit().position.track));
    before = capture_position(state, 1);
    REQUIRE(before);
    state.viewport.time = 4;
    REQUIRE(apply_position_value(state, 1, {5, 6, 7}));
    CHECK(state.document.timeline.find({1, "position"})->keys.back().incoming == timeline::Interpolation::linear);
    state.viewport.time = 0;
    REQUIRE(apply_position_value(state, 1, {-1, -2, -3}));
    CHECK(state.document.timeline.find({1, "position"})->keys.front().incoming == timeline::Interpolation::hold);
    state.viewport.time = 2;
    state.document.keyframe_names[2] = "Editable pose";
    const auto current = std::get<Vec3>(*state.document.timeline.sample({1, "position"}, 2));
    CHECK_FALSE(*apply_position_value(state, 1, current));
    CHECK(state.document.timeline.find({1, "position"})->keys.size() == 2);
    REQUIRE(apply_position_value(state, 1, {1, 2, 3}));
    CHECK(state.document.timeline.find({1, "position"})->keys.size() == 3);
    CHECK(instance_transform(state, 1)->position == before->base_position);
    REQUIRE(restore_position(state, *before));
    CHECK(*capture_position(state, 1) == *before);
    CHECK(storage(state) == geometry);
    CHECK(state.document.timeline.find({2, "radius"})->keys.data() == unrelated);
    state.viewport.paused = false;
    CHECK_FALSE(apply_position_value(state, 1, {}));
}

TEST_CASE("Compact position packets accept placements outside the old small-world range", "[editor][position][wire][regression]") {
    for (const Vec3 position : {Vec3{250,0,-500},Vec3{-250,300,500},Vec3{scene_coordinate_limit,0,-scene_coordinate_limit}}) {
        auto author=scene(), worker=author;
        REQUIRE(apply_position_value(author,1,position));
        ++author.document.revision;
        auto edit=position_edit(worker.document.revision,author,1); REQUIRE(edit);
        auto bytes=encode_position_edit(*edit); REQUIRE(bytes);
        auto decoded=decode_position_edit(*bytes); REQUIRE(decoded);
        REQUIRE(apply_position_edit(worker,*decoded));
        CHECK(instance_transform(worker,1)->position==position);
        const auto snapshot=encode(worker); REQUIRE(snapshot); REQUIRE(editor_example::decode(*snapshot));
    }
}

TEST_CASE("Position updates coalesce while in flight and final cancellation cannot be lost",
          "[editor][position][updates]") {
    auto state = scene();
    const auto baseline = capture_position(state, 1);
    REQUIRE(baseline);
    PreviewUpdates updates;
    updates.add(1);
    REQUIRE(packet(updates, state).starts_with("snapshot\n"));
    updates.acknowledge(1, 1);
    REQUIRE(apply_position_value(state, 1, {1, 2, 3}));
    ++state.document.revision;
    updates.position_changed(1);
    const auto first = packet(updates, state);
    CHECK(first.starts_with("position\n"));
    CHECK(first.size() == 9 + 48);
    for (int i = 0; i < 100; ++i) {
        REQUIRE(apply_position_value(state, 1, {static_cast<f32>(i), 3, 4}));
        ++state.document.revision;
        updates.position_changed(1);
        const auto next = updates.next(1, state);
        REQUIRE(next);
        CHECK_FALSE(*next);
    }
    REQUIRE(restore_position(state, *baseline));
    ++state.document.revision;
    updates.position_changed(1);
    updates.acknowledge(1, 2);
    const auto final = packet(updates, state);
    REQUIRE(final.starts_with("position\n"));
    const auto decoded = decode_position_edit(std::string_view(final).substr(9));
    REQUIRE(decoded);
    CHECK(decoded->base_revision == 2);
    CHECK(decoded->revision == state.document.revision);
    CHECK(decoded->position == *baseline);
    updates.acknowledge(1, state.document.revision);
    CHECK(updates.ready(1, state.document.revision));
}

TEST_CASE("Position edits retain mixed property and geometry targets",
          "[editor][position][updates]") {
    auto state = scene();
    PreviewUpdates updates;
    updates.add(1);
    (void)packet(updates, state);
    updates.acknowledge(1, 1);
    ++state.document.revision;
    updates.position_changed(1);
    const std::array<u32, 1> vertex{0};
    bool full{};
    SECTION("another position object") { updates.position_changed(2); }
    SECTION("legacy playback") { updates.playback_changed(); full = true; }
    SECTION("geometry") { updates.changed(vertex); }
    SECTION("structural edit") { updates.changed(); full = true; }
    SECTION("rejected snapshot followed by new position") {
        updates.reject(1);
        updates.position_changed(1);
        full = true;
    }
    CHECK(packet(updates, state).starts_with(full ? "snapshot\n" : "patch\n"));
    updates.acknowledge(1, state.document.revision);
    CHECK(updates.ready(1, state.document.revision));
}
