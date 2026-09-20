#include "../../examples/editor/selection_edits.hpp"
#include "../../examples/editor/preview_updates.hpp"
#include "../../examples/editor/project.hpp"

#include <catch2/catch_test_macros.hpp>
#include <array>
#include <limits>

namespace {
using namespace vng;
using namespace editor_example;
editor::EditableMesh mesh(std::size_t vertices) {
    content::vmesh::Document document;
    document.vertex_count = vertices;
    document.vertex_fields = {{"position", {content::vmesh::ScalarType::Float32, 3},
                               std::vector<f32>(vertices * 3)}};
    document.faces = {{0, 1, 2}};
    auto created = editor::EditableMesh::create(std::move(document));
    REQUIRE(created);
    return std::move(*created);
}
State scene() {
    State state{.document = {.mesh = mesh(3)}};
    state.document.mesh_assets.push_back({static_cast<BlueprintId>(3), "Large imported mesh", mesh(65536), {}});
    state.document.next_blueprint_id = 4;
    state.document.instances.push_back({8, static_cast<BlueprintId>(3), "Imported instance", MeshSettings{}});
    state.document.next_instance_id = 9;
    return state;
}
const f32* storage(const editor::EditableMesh& value) {
    return std::get<std::vector<f32>>(value.document().vertex_fields[0].values).data();
}
std::string encode(const SelectionEdit& edit) {
    const auto bytes = encode_selection_edit(edit);
    REQUIRE(bytes);
    return *bytes;
}
std::string packet(PreviewUpdates& updates, const State& state, u64 generation = 1) {
    auto next = updates.next(generation, state);
    REQUIRE(next);
    REQUIRE(*next);
    return std::move(**next);
}
void word(std::string& bytes, std::size_t offset, u64 value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        bytes[offset + i] = static_cast<char>((value >> (i * 8U)) & 255U);
}
} // namespace

TEST_CASE("Selection packets are deterministic fixed-size view state", "[editor][selection][wire]") {
    const SelectionEdit edit{0x010203040506ULL, 0x010203040510ULL, 0x0708090a, 65535};
    const auto bytes = encode(edit);
    REQUIRE(bytes.size() == 32);
    CHECK(bytes.substr(0, 8) == std::string("VNGSEL\0\1", 8));
    CHECK(static_cast<unsigned char>(bytes[8]) == 6);
    CHECK(static_cast<unsigned char>(bytes[24]) == 10);
    CHECK(static_cast<unsigned char>(bytes[27]) == 7);
    CHECK(static_cast<unsigned char>(bytes[28]) == 255);
    auto decoded = decode_selection_edit(bytes);
    REQUIRE(decoded);
    CHECK(*decoded == edit);
    CHECK(encode(*decoded) == bytes);
    for (std::size_t size = 0; size < bytes.size(); ++size)
        CHECK_FALSE(decode_selection_edit(std::string_view(bytes).substr(0, size)));
    CHECK_FALSE(decode_selection_edit(bytes + 'x'));
    const auto rejected = [&](std::size_t offset, u64 value, unsigned width) {
        auto bad = bytes;
        word(bad, offset, value, width);
        CHECK_FALSE(decode_selection_edit(bad));
    };
    rejected(0, 'X', 1);
    rejected(7, 2, 1);
    rejected(8, 0, 8);
    rejected(16, edit.base_revision, 8);
    rejected(16, u64{1} << 53, 8);
    rejected(28, 65536, 4);
    CHECK_FALSE(encode_selection_edit({1, 1, 0, 0}));
    CHECK_FALSE(encode_selection_edit({1, 2, 0, std::numeric_limits<u32>::max()}));
    REQUIRE(decode_selection_edit(encode({1, 2, 0, 0})));
}

TEST_CASE("Selection changes only two IDs and revision with no geometry work", "[editor][selection]") {
    auto state = scene();
    REQUIRE(state.document.timeline.set({8, "position"}, {0, Vec3{1, 2, 3}}));
    const auto baseline = encode(state);
    REQUIRE(baseline);
    const auto* base_storage = storage(state.document.mesh);
    const auto* import_storage = storage(state.document.mesh_assets.front().geometry);
    const auto* track_storage = state.document.timeline.find({8, "position"})->keys.data();
    // Bounds must use the incoming selected mesh, not the previous tiny one.
    REQUIRE(apply_selection_edit(state, {1, 2, 8, 60000}));
    CHECK(state.viewport.selected_object == 8);
    CHECK(state.viewport.selected_vertex == 60000);
    CHECK(state.document.revision == 2);
    REQUIRE(apply_selection_edit(state, {2, 3, 1, 2}));
    CHECK(state.viewport.selected_object == 1);
    CHECK(state.viewport.selected_vertex == 2);
    REQUIRE(apply_selection_edit(state, {3, 4, 0, 0}));
    CHECK(state.viewport.selected_object == 0);
    REQUIRE(apply_selection_edit(state, {4, 5, 2, 0}));
    state.document.revision = 1;
    const auto after = encode(state);
    REQUIRE(after);
    CHECK(*after == *baseline);
    CHECK(storage(state.document.mesh) == base_storage);
    CHECK(storage(state.document.mesh_assets.front().geometry) == import_storage);
    CHECK(state.document.timeline.find({8, "position"})->keys.data() == track_storage);
}

TEST_CASE("Invalid selection is rejected atomically against the destination mesh", "[editor][selection]") {
    auto state = scene();
    state.viewport.selected_object = 8;
    state.viewport.selected_vertex = 60000;
    const auto baseline = encode(state);
    REQUIRE(baseline);
    SelectionEdit edit{1, 2, 1, 0};
    SECTION("missing instance") { edit.selected_object = 7; }
    SECTION("vertex outside destination mesh") { edit.selected_vertex = 3; }
    SECTION("deselection has no active vertex") { edit.selected_object = 0; edit.selected_vertex = 1; }
    SECTION("sun selection has no active vertex") {
        edit.selected_object = 2;
        edit.selected_vertex = 1;
    }
    SECTION("global vertex bound") { edit.selected_object = 8; edit.selected_vertex = 65536; }
    SECTION("stale revision") { edit.base_revision = 2; edit.revision = 3; }
    SECTION("zero baseline") { edit.base_revision = 0; }
    SECTION("revision does not advance") { edit.revision = 1; }
    SECTION("revision precision") { edit.revision = u64{1} << 53; }
    CHECK_FALSE(apply_selection_edit(state, edit));
    const auto after = encode(state);
    REQUIRE(after);
    CHECK(*after == *baseline);
    state.viewport.selected_object = edit.selected_object;
    state.viewport.selected_vertex = edit.selected_vertex;
    state.document.revision = edit.revision;
    if (edit.base_revision != 2) // Capturing does not know the remote baseline revision.
        CHECK_FALSE(selection_edit(edit.base_revision, state));
}

TEST_CASE("Scene selection packets cannot redirect an inspected blueprint",
          "[editor][selection][target]") {
    auto state = scene();
    state.viewport.mode = ViewMode::mesh;
    state.viewport.inspected_mesh = static_cast<BlueprintId>(3);
    const auto* imported_storage = storage(state.document.mesh_assets.front().geometry);
    // Selecting the tiny cube does not shrink or replace the independent
    // imported-asset vertex selection.
    REQUIRE(apply_selection_edit(state, {1, 2, 1, 60000}));
    CHECK(state.viewport.selected_object == 1);
    CHECK(state.viewport.selected_vertex == 60000);
    CHECK(state.viewport.inspected_mesh == static_cast<BlueprintId>(3));
    CHECK(editable_mesh(state) == &state.document.mesh_assets.front().geometry);
    REQUIRE(apply_selection_edit(state, {2, 3, 0, 65535}));
    CHECK(state.viewport.selected_object == 0);
    CHECK(storage(*editable_mesh(state)) == imported_storage);
    REQUIRE(selection_edit(2, state));
    CHECK_FALSE(apply_selection_edit(state, {3, 4, 8, 65536}));
    CHECK(state.document.revision == 3);

    state.viewport.inspected_mesh = BlueprintId::mesh;
    state.viewport.selected_vertex = 0;
    // Conversely, an imported scene selection cannot validate an out-of-range
    // vertex against its larger mesh while the cube is being edited.
    CHECK_FALSE(apply_selection_edit(state, {3, 4, 8, 3}));
    CHECK(state.viewport.selected_object == 0);
    REQUIRE(apply_selection_edit(state, {3, 4, 8, 2}));
    CHECK(editable_mesh(state) == &state.document.mesh);
    CHECK(state.viewport.inspected_mesh == BlueprintId::mesh);
}

TEST_CASE("Sun effect view selection has no mesh vertex", "[editor][selection][target]") {
    auto state = scene();
    state.viewport.mode = ViewMode::sun;
    CHECK_FALSE(apply_selection_edit(state, {1, 2, 8, 1}));
    REQUIRE(apply_selection_edit(state, {1, 2, 8, 0}));
    CHECK_FALSE(editable_mesh(state));
}

TEST_CASE("Selection packets cannot repair or silently replace a missing inspected asset",
          "[editor][selection][target]") {
    auto state = scene();
    state.viewport.mode = ViewMode::mesh;
    state.viewport.inspected_mesh = static_cast<BlueprintId>(99);
    CHECK_FALSE(apply_selection_edit(state, {1, 2, 1, 0}));
    CHECK(state.document.revision == 1);
    CHECK(state.viewport.selected_object == 2);
    CHECK(state.viewport.inspected_mesh == static_cast<BlueprintId>(99));
}

TEST_CASE("Selection coalescing sends the newest click after the in-flight acknowledgement", "[editor][selection][updates]") {
    auto state = scene();
    auto worker = state;
    PreviewUpdates updates;
    updates.add(1);
    CHECK(packet(updates, state).starts_with("snapshot\n"));
    updates.acknowledge(1, state.document.revision);
    state.viewport.selected_object = 1;
    ++state.document.revision;
    updates.selection_changed();
    const auto first = packet(updates, state);
    REQUIRE(first.starts_with("selection\n"));
    REQUIRE(first.size() == 10 + 32);
    auto edit = decode_selection_edit(std::string_view(first).substr(10));
    REQUIRE(edit);
    REQUIRE(apply_selection_edit(worker, *edit));
    for (u32 i = 0; i < 100; ++i) {
        state.viewport.selected_object = i % 2 ? 8 : 2;
        state.viewport.selected_vertex = i % 2 ? 60000 + i : 0;
        ++state.document.revision;
        updates.selection_changed();
        const auto blocked = updates.next(1, state);
        REQUIRE(blocked);
        CHECK_FALSE(*blocked);
    }
    CHECK_FALSE(updates.ready(1, state.document.revision));
    updates.acknowledge(1, worker.document.revision);
    const auto newest = packet(updates, state);
    REQUIRE(newest.starts_with("selection\n"));
    edit = decode_selection_edit(std::string_view(newest).substr(10));
    REQUIRE(edit);
    CHECK(edit->base_revision == worker.document.revision);
    REQUIRE(apply_selection_edit(worker, *edit));
    CHECK(worker.viewport.selected_object == state.viewport.selected_object);
    CHECK(worker.viewport.selected_vertex == state.viewport.selected_vertex);
    updates.acknowledge(1, state.document.revision);
    CHECK(updates.ready(1, state.document.revision));
}

TEST_CASE("Mixed selection changes retain every pending edit in either order", "[editor][selection][updates]") {
    auto state = scene();
    PreviewUpdates updates;
    updates.add(1);
    (void)packet(updates, state);
    updates.acknowledge(1, 1);
    ++state.document.revision;
    const std::array<u32, 1> vertex{0};
    const auto other = [&] {
        SECTION("camera") { updates.camera_changed(); }
        SECTION("playback") { updates.playback_changed(); }
        SECTION("position") { updates.position_changed(1); }
        SECTION("vertices") { updates.changed(vertex); }
        SECTION("full edit") { updates.changed(); }
    };
    SECTION("selection first") { updates.selection_changed(); other(); }
    SECTION("selection last") { other(); updates.selection_changed(); }
    CHECK(packet(updates, state).starts_with("snapshot\n"));
    updates.acknowledge(1, state.document.revision);
    CHECK(updates.ready(1, state.document.revision));
}

TEST_CASE("Selection retry and independent peers retain the newest object", "[editor][selection][updates]") {
    auto state = scene();
    PreviewUpdates updates;
    updates.add(1);
    updates.add(2);
    (void)packet(updates, state, 1);
    (void)packet(updates, state, 2);
    updates.acknowledge(1, 1);
    updates.acknowledge(2, 1);
    state.viewport.selected_object = 8;
    state.viewport.selected_vertex = 65535;
    ++state.document.revision;
    updates.selection_changed();
    CHECK(packet(updates, state, 1).starts_with("selection\n"));
    updates.reset(1); // Transport failure must retry a full safe baseline.
    CHECK(packet(updates, state, 1).starts_with("snapshot\n"));
    CHECK(packet(updates, state, 2).starts_with("selection\n"));
    updates.acknowledge(1, state.document.revision);
    updates.acknowledge(2, state.document.revision);
    CHECK(updates.ready(1, state.document.revision));
    CHECK(updates.ready(2, state.document.revision));
    updates.reject(1);
    ++state.document.revision;
    state.viewport.selected_object = 0;
    state.viewport.selected_vertex = 0;
    updates.selection_changed();
    CHECK(packet(updates, state, 1).starts_with("snapshot\n"));
    CHECK(packet(updates, state, 2).starts_with("selection\n"));
}
