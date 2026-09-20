#include "../../examples/editor/rotation_edits.hpp"
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
RotationEdit keyed_edit() {
    return {1, 2, {1, {1, 2, 3},
        timeline::Track{{1, "rotation"}, "Motion", "Objects",
                         {{0, Vec3{1, 2, 3}, timeline::Interpolation::hold},
                          {4, Vec3{4, 5, 6}, timeline::Interpolation::linear}}}}};
}
std::string encoded(const RotationEdit& edit) {
    const auto result = encode_rotation_edit(edit);
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

TEST_CASE("Unkeyed rotation packets are deterministic 48-byte complete properties",
          "[editor][rotation][wire]") {
    const RotationEdit edit{0x010203040506ULL, 0x010203040510ULL, {17, {-1, .5F, 3}, {}}};
    const auto wire = encoded(edit);
    REQUIRE(wire.size() == 48);
    CHECK(wire.substr(0, 8) == std::string("VNGROT\0\1", 8));
    CHECK(static_cast<unsigned char>(wire[8]) == 6);
    CHECK(static_cast<unsigned char>(wire[24]) == 17);
    CHECK(static_cast<unsigned char>(wire[40]) == 0);
    CHECK(static_cast<unsigned char>(wire[44]) == 0);
    const auto decoded = decode_rotation_edit(wire);
    REQUIRE(decoded);
    CHECK(*decoded == edit);
    CHECK(encoded(*decoded) == wire);
    CHECK_FALSE(decode_position_edit(wire)); // Distinct typed wire identities.
    const auto position = encode_position_edit({edit.base_revision, edit.revision, {17, {}, {}}});
    REQUIRE(position);
    CHECK_FALSE(decode_rotation_edit(*position));
    for (std::size_t n = 0; n < wire.size(); ++n)
        CHECK_FALSE(decode_rotation_edit(std::string_view(wire).substr(0, n)));
    CHECK_FALSE(decode_rotation_edit(wire + 'x'));
}

TEST_CASE("Keyed rotation wire preserves the complete property track and rejects malformed payloads",
          "[editor][rotation][wire]") {
    const auto edit = keyed_edit();
    const auto wire = encoded(edit);
    CHECK(wire.size() == 48 + 8 + 6 + 7 + 2 * 20);
    const auto decoded = decode_rotation_edit(wire);
    REQUIRE(decoded);
    CHECK(*decoded == edit);
    for (std::size_t n = 0; n < wire.size(); ++n)
        CHECK_FALSE(decode_rotation_edit(std::string_view(wire).substr(0, n)));
    const auto rejected = [&](std::size_t offset, u64 value, unsigned width) {
        auto corrupt = wire;
        word(corrupt, offset, value, width);
        CHECK_FALSE(decode_rotation_edit(corrupt));
    };
    rejected(0, 'X', 1);
    rejected(7, 2, 1);
    rejected(8, 0, 8);
    rejected(16, 1, 8);
    rejected(16, u64{1} << 53, 8);
    rejected(24, 0, 4);
    rejected(28, std::bit_cast<u32>(361.F), 4);
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
    invalid.rotation.track->target.object = 7;
    CHECK_FALSE(encode_rotation_edit(invalid));
    invalid = edit;
    invalid.rotation.track->keys[0].value = 1.F;
    CHECK_FALSE(encode_rotation_edit(invalid));
    invalid = edit;
    invalid.rotation.track->keys[1].time = 0;
    CHECK_FALSE(encode_rotation_edit(invalid));
    invalid = edit;
    invalid.rotation.track->keys.clear();
    CHECK_FALSE(encode_rotation_edit(invalid));
}

TEST_CASE("Rotation edits are atomic and never reallocate geometry or unrelated tracks",
          "[editor][rotation]") {
    auto state = scene(65536);
    REQUIRE(state.document.timeline.set({2, "radius"}, {0, 1.F}));
    REQUIRE(state.document.timeline.set({2, "radius"}, {3, 2.F}));
    const auto* geometry = storage(state);
    const auto* unrelated = state.document.timeline.find({2, "radius"})->keys.data();
    const auto original = state;
    auto edit = keyed_edit();
    REQUIRE(apply_rotation_edit(state, edit));
    CHECK(instance_transform(state, 1)->rotation == Vec3{1, 2, 3});
    CHECK(state.document.revision == 2);
    CHECK(storage(state) == geometry);
    CHECK(state.document.timeline.find({2, "radius"})->keys.data() == unrelated);
    CHECK(state.document.mesh.document() == original.document.mesh.document());
    const auto committed = encode(state);
    REQUIRE(committed);
    CHECK_FALSE(apply_rotation_edit(state, edit));
    edit.base_revision = 2;
    edit.revision = 3;
    edit.rotation.track->keys.back().time = state.document.timeline_duration + 1;
    CHECK_FALSE(apply_rotation_edit(state, edit));
    edit = {2, 3, {99, {}, {}}};
    CHECK_FALSE(apply_rotation_edit(state, edit));
    CHECK(*encode(state) == *committed);
    CHECK(storage(state) == geometry);
    CHECK(state.document.timeline.find({2, "radius"})->keys.data() == unrelated);
    const RotationEdit unkeyed{2, 3, {1, {-1, -2, -3}, {}}};
    REQUIRE(apply_rotation_edit(state, unkeyed));
    CHECK_FALSE(state.document.timeline.find({1, "rotation"}));
    CHECK(state.document.timeline.find({2, "radius"})->keys.data() == unrelated);
    CHECK(storage(state) == geometry);
}

TEST_CASE("Live local rotations preserve keyed semantics and cancel only their own property",
          "[editor][rotation]") {
    auto state = scene();
    REQUIRE(state.document.timeline.set({2, "radius"}, {0, 1.F}));
    const auto* unrelated = state.document.timeline.find({2, "radius"})->keys.data();
    const auto* geometry = storage(state);
    auto before = capture_rotation(state, 1);
    REQUIRE(before);
    auto changed = apply_rotation_value(state, 1, {3, 2, 1});
    REQUIRE(changed);
    CHECK(*changed);
    CHECK(state.document.revision == 1);
    CHECK_FALSE(*apply_rotation_value(state, 1, {3, 2, 1}));
    REQUIRE(restore_rotation(state, *before));
    CHECK(*capture_rotation(state, 1) == *before);
    CHECK_FALSE(apply_rotation_value(state, 1, {361, 0, 0}));
    CHECK_FALSE(apply_rotation_value(state, 77, {}));
    REQUIRE(state.document.timeline.replace_track(*keyed_edit().rotation.track));
    before = capture_rotation(state, 1);
    REQUIRE(before);
    state.viewport.time = 4;
    REQUIRE(apply_rotation_value(state, 1, {5, 6, 7}));
    CHECK(state.document.timeline.find({1, "rotation"})->keys.back().incoming == timeline::Interpolation::linear);
    state.viewport.time = 0;
    REQUIRE(apply_rotation_value(state, 1, {-1, -2, -3}));
    CHECK(state.document.timeline.find({1, "rotation"})->keys.front().incoming == timeline::Interpolation::hold);
    state.viewport.time = 2;
    state.document.keyframe_names[2] = "Editable pose";
    const auto current = std::get<Vec3>(*state.document.timeline.sample({1, "rotation"}, 2));
    CHECK_FALSE(*apply_rotation_value(state, 1, current));
    CHECK(state.document.timeline.find({1, "rotation"})->keys.size() == 2);
    REQUIRE(apply_rotation_value(state, 1, {1, 2, 3}));
    CHECK(state.document.timeline.find({1, "rotation"})->keys.size() == 3);
    CHECK(instance_transform(state, 1)->rotation == before->base_rotation);
    REQUIRE(restore_rotation(state, *before));
    CHECK(*capture_rotation(state, 1) == *before);
    CHECK(storage(state) == geometry);
    CHECK(state.document.timeline.find({2, "radius"})->keys.data() == unrelated);
    state.viewport.paused = false;
    CHECK_FALSE(apply_rotation_value(state, 1, {}));
}

TEST_CASE("Rotation updates coalesce while in flight and final cancellation cannot be lost",
          "[editor][rotation][updates]") {
    auto state = scene();
    const auto baseline = capture_rotation(state, 1);
    REQUIRE(baseline);
    PreviewUpdates updates;
    updates.add(1);
    REQUIRE(packet(updates, state).starts_with("snapshot\n"));
    updates.acknowledge(1, 1);
    REQUIRE(apply_rotation_value(state, 1, {1, 2, 3}));
    ++state.document.revision;
    updates.rotation_changed(1);
    const auto first = packet(updates, state);
    CHECK(first.starts_with("rotation\n"));
    CHECK(first.size() == 9 + 48);
    for (int i = 0; i < 100; ++i) {
        REQUIRE(apply_rotation_value(state, 1, {static_cast<f32>(i), 3, 4}));
        ++state.document.revision;
        updates.rotation_changed(1);
        const auto next = updates.next(1, state);
        REQUIRE(next);
        CHECK_FALSE(*next);
    }
    REQUIRE(restore_rotation(state, *baseline));
    ++state.document.revision;
    updates.rotation_changed(1);
    updates.acknowledge(1, 2);
    const auto final = packet(updates, state);
    REQUIRE(final.starts_with("rotation\n"));
    const auto decoded = decode_rotation_edit(std::string_view(final).substr(9));
    REQUIRE(decoded);
    CHECK(decoded->base_revision == 2);
    CHECK(decoded->revision == state.document.revision);
    CHECK(decoded->rotation == *baseline);
    updates.acknowledge(1, state.document.revision);
    CHECK(updates.ready(1, state.document.revision));
}

TEST_CASE("Rotation edits preserve mixed property and geometry targets",
          "[editor][rotation][updates]") {
    auto state = scene();
    PreviewUpdates updates;
    updates.add(1);
    (void)packet(updates, state);
    updates.acknowledge(1, 1);
    ++state.document.revision;
    updates.rotation_changed(1);
    bool full{};
    const std::array<u32, 1> vertex{0};
    SECTION("another rotation object") { updates.rotation_changed(2); }
    SECTION("position") { updates.position_changed(1); }
    SECTION("camera") { updates.camera_changed(); }
    SECTION("legacy selection") { updates.selection_changed(); full = true; }
    SECTION("legacy playback") { updates.playback_changed(); full = true; }
    SECTION("geometry") { updates.changed(vertex); }
    SECTION("structural edit") { updates.changed(); full = true; }
    SECTION("rejected snapshot followed by new rotation") {
        updates.reject(1);
        updates.rotation_changed(1);
        full = true;
    }
    CHECK(packet(updates, state).starts_with(full ? "snapshot\n" : "patch\n"));
    updates.acknowledge(1, state.document.revision);
    CHECK(updates.ready(1, state.document.revision));
}

TEST_CASE("Rotation limits and instance ownership match the authored transform API",
          "[editor][rotation]") {
    auto state = scene();
    const auto cube = state.document.mesh.document();
    const auto original_mesh_transform = *instance_transform(state, 1);
    const auto original_sun_transform = *instance_transform(state, 2);
    REQUIRE(apply_rotation_value(state, 1, {-360, 360, 180}));
    CHECK(instance_transform(state, 1)->position == original_mesh_transform.position);
    CHECK(instance_transform(state, 1)->scale == original_mesh_transform.scale);
    CHECK(*instance_transform(state, 2) == original_sun_transform);
    CHECK(state.document.mesh.document() == cube);
    const auto rejected = apply_rotation_value(state, 1, {-361, 0, 0});
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().message == "Rotation values must be finite and within +/-360");
    REQUIRE(apply_rotation_value(state, 2, {15, 20, 25}));
    CHECK(instance_transform(state, 2)->rotation == Vec3{15, 20, 25});
    CHECK(instance_transform(state, 1)->rotation == Vec3{-360, 360, 180});
    CHECK(state.document.mesh.document() == cube);
}
