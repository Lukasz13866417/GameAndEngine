#include "../../examples/editor/camera_edits.hpp"
#include "../../examples/editor/project.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <limits>

namespace {
using namespace vng;
using namespace editor_example;
State camera_source() {
    content::vmesh::Document document;
    document.vertex_count = 3;
    document.vertex_fields = {{"position", {content::vmesh::ScalarType::Float32, 3},
        std::vector<f32>{0, 0, 0, 1, 0, 0, 0, 1, 0}}};
    document.faces = {{0, 1, 2}};
    auto mesh = editor::EditableMesh::create(std::move(document));
    REQUIRE(mesh);
    return {.document = {.mesh = std::move(*mesh)}};
}
void overwrite_float(std::string& bytes, std::size_t offset, f32 value) {
    const auto bits = std::bit_cast<u32>(value);
    for (unsigned i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<char>((bits >> (i * 8U)) & 255U);
}
} // namespace

TEST_CASE("Camera edits are fixed-size deterministic complete pose packets", "[editor][camera][wire]") {
    const CameraEdit edit{0x010203040506ULL, 0x010203040510ULL,
        {-35.0F, 42.5F, 8.0F, {2, -3, 4}, 2}, {60, -20, 12, {1, 3, -2}, .5F}, true};
    auto bytes = encode_camera_edit(edit);
    REQUIRE(bytes);
    REQUIRE(bytes->size() == 84);
    CHECK(bytes->substr(0, 8) == std::string("VNGCAM\0\3", 8));
    CHECK(static_cast<unsigned char>((*bytes)[8]) == 6);
    auto again = encode_camera_edit(edit);
    REQUIRE(again);
    CHECK(*again == *bytes);
    auto decoded = decode_camera_edit(*bytes);
    REQUIRE(decoded);
    CHECK(*decoded == edit);
    for (std::size_t length = 0; length < bytes->size(); ++length)
        CHECK_FALSE(decode_camera_edit(std::string_view(*bytes).substr(0, length)));
    CHECK_FALSE(decode_camera_edit(*bytes + "extra"));
    auto version = *bytes; version[7] = 1;
    CHECK_FALSE(decode_camera_edit(version));
    auto magic = *bytes; magic[0] = 'X';
    CHECK_FALSE(decode_camera_edit(magic));
}

TEST_CASE("Camera edit application is atomic and leaves mesh storage and scene state alone", "[editor][camera]") {
    auto state = camera_source();
    const auto before = state;
    const auto* storage = std::get<std::vector<f32>>(state.document.mesh.document().vertex_fields[0].values).data();
    const CameraEdit edit{state.document.revision, state.document.revision + 9,
        {120, -25, 4, {7, 8, -9}}, {-120, 25, 14, {-1, 2, 9}}, true};
    REQUIRE(apply_camera_edit(state, edit));
    CHECK(state.document.revision == edit.revision);
    CHECK(state.viewport.editor_camera.yaw == edit.editor_camera.yaw);
    CHECK(state.viewport.editor_camera.pitch == edit.editor_camera.pitch);
    CHECK(state.viewport.editor_camera.distance == edit.editor_camera.distance);
    CHECK(state.viewport.editor_camera.target == edit.editor_camera.target);
    CHECK(state.document.animation_camera == edit.animation_camera);
    CHECK(state.viewport.pilot_camera == edit.pilot_camera);
    CHECK(std::get<std::vector<f32>>(state.document.mesh.document().vertex_fields[0].values).data() == storage);
    CHECK(state.document.mesh.document().vertex_fields == before.document.mesh.document().vertex_fields);
    CHECK(state.document.mesh.document().faces == before.document.mesh.document().faces);
    CHECK((*editor_example::mesh_settings(state, 1)) == (*editor_example::mesh_settings(before, 1)));
    CHECK((*editor_example::sun_settings(state, 2)) == (*editor_example::sun_settings(before, 2)));
    CHECK(state.viewport.mode == before.viewport.mode);
    CHECK(state.viewport.selected_object == before.viewport.selected_object);
    CHECK(state.viewport.selected_vertex == before.viewport.selected_vertex);
    CHECK(state.viewport.paused == before.viewport.paused);
    CHECK(state.viewport.time == before.viewport.time);
    CHECK(state.viewport.weld == before.viewport.weld);
    auto from_state = camera_edit(before.document.revision, state);
    REQUIRE(from_state);
    CHECK(*from_state == edit);
    const auto committed = encode(state);
    REQUIRE(committed);
    CHECK_FALSE(apply_camera_edit(state, edit));
    auto after_replay = encode(state);
    REQUIRE(after_replay);
    CHECK(*after_replay == *committed);
}

TEST_CASE("Camera edit validation rejects invalid revisions ranges and nonfinite data before writes", "[editor][camera]") {
    auto state = camera_source();
    const CameraEdit valid{state.document.revision, state.document.revision + 1, {0, 0, 8, {}}};
    const auto original = encode(state);
    REQUIRE(original);
    auto rejects = [&](CameraEdit edit) {
        CHECK_FALSE(encode_camera_edit(edit));
        CHECK_FALSE(apply_camera_edit(state, edit));
        auto unchanged = encode(state);
        REQUIRE(unchanged);
        CHECK(*unchanged == *original);
    };
    auto bad = valid; bad.base_revision = 0; rejects(bad);
    bad = valid; bad.revision = bad.base_revision; rejects(bad);
    bad = valid; bad.revision = u64{1} << 53; rejects(bad);
    bad = valid; bad.editor_camera.yaw = 180.01F; rejects(bad);
    bad = valid; bad.editor_camera.pitch = camera_max_pitch + 0.01F; rejects(bad);
    bad = valid; bad.editor_camera.distance = camera_min_distance * 0.5F; rejects(bad);
    bad = valid; bad.editor_camera.distance = camera_max_distance + 1; rejects(bad);
    bad = valid; bad.editor_camera.target.y = camera_target_limit + 1; rejects(bad);
    bad = valid; bad.editor_camera.yaw = std::numeric_limits<f32>::infinity(); rejects(bad);
    bad = valid; bad.editor_camera.pitch = std::numeric_limits<f32>::quiet_NaN(); rejects(bad);
    bad = valid; bad.editor_camera.distance = std::numeric_limits<f32>::infinity(); rejects(bad);
    bad = valid; bad.editor_camera.target.z = std::numeric_limits<f32>::quiet_NaN(); rejects(bad);
    bad = valid; bad.animation_camera.yaw = 181; rejects(bad);
    bad = valid; bad.animation_camera.pitch = camera_max_pitch + .1F; rejects(bad);
    bad = valid; bad.animation_camera.distance = 0; rejects(bad);
    bad = valid; bad.animation_camera.target.x = std::numeric_limits<f32>::quiet_NaN(); rejects(bad);

    bad = valid; bad.base_revision = 2; bad.revision = 3;
    REQUIRE(encode_camera_edit(bad));
    CHECK_FALSE(apply_camera_edit(state, bad)); // Structurally valid, but stale for this receiver.
    auto bytes = encode_camera_edit(valid);
    REQUIRE(bytes);
    for (std::size_t offset = 24; offset < 80; offset += 4) {
        auto poisoned = *bytes;
        overwrite_float(poisoned, offset, std::numeric_limits<f32>::quiet_NaN());
        CHECK_FALSE(decode_camera_edit(poisoned));
    }
    auto invalid_flag = *bytes;
    invalid_flag[80] = 2;
    CHECK_FALSE(decode_camera_edit(invalid_flag));
    const CameraEdit limits{1, (u64{1} << 53) - 1,
        {-180, camera_max_pitch, camera_min_distance, {-camera_target_limit, camera_target_limit, 0}}};
    REQUIRE(encode_camera_edit(limits));
    CHECK_FALSE(camera_edit(state.document.revision, state)); // A changed pose needs a newer revision.
}
