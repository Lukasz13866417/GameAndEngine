#include "../../examples/editor/edits.hpp"
#include "../../examples/editor/project.hpp"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <limits>

namespace {
using namespace vng;
namespace project = editor_example;
namespace vm = content::vmesh;
project::State state(std::size_t count = 4) {
    vm::Document document;
    document.metadata = {{"name", "Vertex patch fixture"}, {"custom/info", "Retain all metadata"}};
    document.vertex_count = count;
    std::vector<f32> positions, colors;
    for (std::size_t i = 0; i < count; ++i) {
        positions.insert(positions.end(),
                         {static_cast<f32>(i) * .01F, static_cast<f32>(i % 17) * .02F, 0});
        colors.insert(colors.end(), {.25F, .5F, 1, 1});
    }
    document.vertex_fields = {
        {"color/0", {vm::ScalarType::Float32, 4}, std::move(colors)},
        {"position", {vm::ScalarType::Float32, 3}, std::move(positions)},
        {"custom/tag", {vm::ScalarType::UInt32, 1}, std::vector<u32>(count, 17)}};
    document.faces = {{0, 1, 2}, {2, 1, 3}};
    document.edges = std::vector<gfx::Edge>{{0, 1}, {1, 3}};
    auto mesh = editor::EditableMesh::create(std::move(document));
    REQUIRE(mesh);
    return {.document = {.mesh = std::move(*mesh)}, .viewport = {.selected_object = 1}};
}
std::string encoded(const project::VertexEdit& edit) {
    auto bytes = project::encode_edit(edit);
    REQUIRE(bytes);
    return *bytes;
}
void word(std::string& bytes, std::size_t offset, u64 value, unsigned size) {
    for (unsigned i = 0; i < size; ++i)
        bytes[offset + i] = static_cast<char>((value >> (i * 8)) & 255);
}
} // namespace

TEST_CASE("Vertex patch wire format is deterministic bounded little endian binary",
          "[editor][mesh][wire]") {
    const project::VertexEdit edit{1, 2, {{2, {1, -2, .5F}}}};
    const auto bytes = encoded(edit);
    CHECK(bytes.size() == 44);
    CHECK(bytes.substr(0, 8) == std::string("VNGVTX\0\1", 8));
    std::string expected("VNGVTX\0\1", 8);
    expected.resize(44, '\0');
    word(expected, 8, 1, 8);
    word(expected, 16, 2, 8);
    word(expected, 24, 1, 4);
    word(expected, 28, 2, 4);
    word(expected, 32, 0x3f800000, 4);
    word(expected, 36, 0xc0000000, 4);
    word(expected, 40, 0x3f000000, 4);
    CHECK(bytes == expected);
    const auto decoded = project::decode_edit(bytes);
    REQUIRE(decoded);
    CHECK(*decoded == edit);
    CHECK(encoded(*decoded) == bytes);
    CHECK(bytes.find('\0') != std::string::npos); // IPC must carry explicit lengths.
    const auto zeros = project::decode_edit(encoded({1, 2, {{0, {-0.0F, 0, -1000000}}}}));
    REQUIRE(zeros);
    CHECK(std::signbit(zeros->vertices[0].position.x));
    const auto empty = project::decode_edit(encoded({1, 7, {}}));
    REQUIRE(empty);
    CHECK(empty->vertices.empty());

    project::VertexEdit maximum{1, 2, {}};
    for (u32 i = 0; i < 65536; ++i)
        maximum.vertices.push_back({i, {static_cast<f32>(i), 0, 0}});
    const auto maximum_bytes = encoded(maximum);
    CHECK(maximum_bytes.size() == 28 + 65536 * 16);
    auto roundtrip = project::decode_edit(maximum_bytes);
    REQUIRE(roundtrip);
    CHECK(*roundtrip == maximum);
    maximum.vertices.push_back({0, {}});
    CHECK_FALSE(project::encode_edit(maximum));
}

TEST_CASE("Malformed vertex patch bytes and invalid records are rejected before mutation",
          "[editor][mesh][wire]") {
    const auto bytes = encoded({1, 2, {{0, {1, 2, 3}}, {1, {4, 5, 6}}}});
    for (std::size_t length = 0; length < bytes.size(); ++length)
        CHECK_FALSE(project::decode_edit(std::string_view(bytes).substr(0, length)));
    CHECK_FALSE(project::decode_edit(bytes + 'x'));
    for (std::size_t index = 0; index < 8; ++index) {
        auto corrupt = bytes;
        corrupt[index] ^= 0x7f;
        CHECK_FALSE(project::decode_edit(corrupt));
    }
    const auto malformed_word = [&](std::size_t offset, u64 value, unsigned size) {
        auto corrupt = bytes;
        word(corrupt, offset, value, size);
        CHECK_FALSE(project::decode_edit(corrupt));
    };
    malformed_word(8, 0, 8);
    malformed_word(8, 2, 8);
    malformed_word(16, 1, 8);
    malformed_word(16, u64{1} << 53, 8);
    malformed_word(24, 65537, 4);
    malformed_word(24, 0xffffffff, 4);
    malformed_word(28, 65536, 4);
    malformed_word(44, 0, 4);          // duplicate of first index
    malformed_word(32, 0x7fc00000, 4); // NaN
    malformed_word(36, 0x7f800000, 4); // +inf
    malformed_word(40, std::bit_cast<u32>(1000001.0F), 4);
    CHECK_FALSE(project::encode_edit({0, 2, {}}));
    CHECK_FALSE(project::encode_edit({1, 1, {}}));
    CHECK_FALSE(project::encode_edit({1, 2, {{0, {}}, {0, {}}}}));
    CHECK_FALSE(project::encode_edit({1, 2, {{65536, {}}}}));
    CHECK_FALSE(project::encode_edit({1, 2, {{0, {std::numeric_limits<f32>::infinity(), 0, 0}}}}));

    // Deterministic mutation corpus reaches framing, every integer byte and
    // every float payload byte. Accepted payloads must round-trip exactly.
    for (std::size_t i = 0; i < bytes.size(); ++i)
        for (const u32 replacement : {0U, 0x7fU, 0x80U, 0xffU}) {
            auto corrupt = bytes;
            corrupt[i] = static_cast<char>(replacement);
            auto decoded = project::decode_edit(corrupt);
            if (decoded)
                CHECK(encoded(*decoded) == corrupt);
        }
}

TEST_CASE("Imported mesh patches carry explicit asset identity and support high vertex indices",
          "[editor][mesh][wire][import]") {
    auto value = state();
    auto large = state(10000);
    const auto blueprint = static_cast<project::BlueprintId>(3);
    value.document.mesh_assets.push_back({blueprint, "Imported", std::move(large.document.mesh), {}});
    value.document.next_blueprint_id = 4;
    const auto instance = project::instantiate(value, blueprint);
    REQUIRE(instance);
    auto before = value;
    REQUIRE(project::editable_mesh(value)->set_position(9000, {3, 4, 5}));
    ++value.document.revision;
    const auto delta = project::vertex_edit(before, value);
    REQUIRE(delta);
    CHECK(delta->blueprint == 3);
    CHECK(delta->vertices == std::vector<project::VertexPosition>{{9000, {3, 4, 5}}});
    const auto wire = encoded(*delta);
    CHECK(wire.size() == 32 + 16);
    CHECK(wire.substr(0, 8) == std::string("VNGVTX\0\2", 8));
    const auto decoded = project::decode_edit(wire);
    REQUIRE(decoded);
    CHECK(*decoded == *delta);
    const auto builtin = before.document.mesh.document();
    REQUIRE(project::apply_edit(before, *decoded));
    CHECK(before.document.mesh.document() == builtin);
    CHECK(project::editable_mesh(before)->position(9000) == Vec3{3, 4, 5});
    const std::array<u32, 1> touched{9000};
    auto fast = project::vertex_edit(value.document.revision - 1, value, touched);
    REQUIRE(fast);
    CHECK(*fast == *delta);
    auto absent = *decoded;
    absent.base_revision = before.document.revision;
    absent.revision = before.document.revision + 1;
    absent.blueprint = 77;
    CHECK_FALSE(project::apply_edit(before, absent));
    CHECK(before.document.revision == value.document.revision);
    auto bad = wire;
    word(bad, 28, 2, 4);
    CHECK_FALSE(project::decode_edit(bad));
    for (std::size_t size = 0; size < wire.size(); ++size)
        CHECK_FALSE(project::decode_edit(std::string_view(wire).substr(0, size)));
    auto another = value;
    REQUIRE(another.document.mesh.set_position(0, {2, 3, 4}));
    ++another.document.revision;
    CHECK_FALSE(project::vertex_edit(value, another));
}

TEST_CASE("Applying vertex patches is transactional and preserves all untouched storage",
          "[editor][mesh]") {
    auto current = state();
    const auto original = current.document.mesh.document();
    const auto* live_positions =
        std::get<std::vector<f32>>(current.document.mesh.document().vertex_fields[1].values).data();
    const auto* live_colors =
        std::get<std::vector<f32>>(current.document.mesh.document().vertex_fields[0].values).data();
    const auto* live_faces = current.document.mesh.document().faces.data();
    const std::array invalid{
        project::VertexEdit{2, 3, {{0, {1, 2, 3}}}},                 // stale/future base
        project::VertexEdit{1, 2, {{0, {1, 2, 3}}, {4, {1, 2, 3}}}}, // actual mesh index
        project::VertexEdit{1, 2, {{0, {1, 2, 3}}, {1, {1000001, 2, 3}}}},
        project::VertexEdit{1, 2, {{0, {1, 2, 3}}, {0, {4, 5, 6}}}}};
    for (const auto& edit : invalid) {
        CHECK_FALSE(project::apply_edit(current, edit));
        CHECK(current.document.revision == 1);
        CHECK(current.document.mesh.document() == original);
    }
    const project::VertexEdit edit{1, 4, {{1, {2, 3, 4}}, {3, {-1, -2, -3}}}};
    REQUIRE(project::apply_edit(current, edit));
    CHECK(current.document.revision == 4);
    CHECK(current.document.mesh.position(1) == Vec3{2, 3, 4});
    CHECK(current.document.mesh.position(3) == Vec3{-1, -2, -3});
    CHECK(current.document.mesh.document().metadata == original.metadata);
    CHECK(current.document.mesh.document().faces == original.faces);
    CHECK(current.document.mesh.document().edges == original.edges);
    CHECK(current.document.mesh.document().vertex_fields[0] == original.vertex_fields[0]);
    CHECK(current.document.mesh.document().vertex_fields[2] == original.vertex_fields[2]);
    CHECK(std::get<std::vector<f32>>(current.document.mesh.document().vertex_fields[1].values).data() ==
          live_positions);
    CHECK(std::get<std::vector<f32>>(current.document.mesh.document().vertex_fields[0].values).data() ==
          live_colors);
    CHECK(current.document.mesh.document().faces.data() == live_faces);
    CHECK_FALSE(project::apply_edit(current, edit)); // replay poisons no state
    CHECK(current.document.revision == 4);
    CHECK(current.document.mesh.position(1) == Vec3{2, 3, 4});
    REQUIRE(project::apply_edit(current, {4, 5, {}}));
    CHECK(current.document.revision == 5);
}

TEST_CASE("Explicit blueprint targets edit only that asset regardless of scene selection",
          "[editor][mesh][target]") {
    auto current = state();
    const auto imported = static_cast<project::BlueprintId>(3);
    current.document.mesh_assets.push_back({imported, "Imported", std::move(state(32).document.mesh), {}});
    current.document.next_blueprint_id = 4;
    REQUIRE(project::instantiate(current, imported));
    current.viewport.mode = project::ViewMode::mesh;
    current.viewport.inspected_mesh = project::BlueprintId::mesh;
    REQUIRE(project::mesh_target(current));
    CHECK(project::editable_mesh(current) == &current.document.mesh);
    auto before = current;
    const auto imported_document = current.document.mesh_assets.front().geometry.document();
    REQUIRE(project::editable_mesh(current)->set_position(0, {7, 8, 9}));
    ++current.document.revision;
    const auto edit = project::vertex_edit(before, current);
    REQUIRE(edit);
    CHECK(edit->blueprint == 1);
    const std::array<u32, 1> touched{0};
    const auto compact = project::vertex_edit(before.document.revision, current, touched);
    REQUIRE(compact);
    CHECK(*compact == *edit);
    REQUIRE(project::apply_edit(before, *compact));
    CHECK(before.document.mesh.document() == current.document.mesh.document());
    CHECK(before.document.mesh_assets.front().geometry.document() == imported_document);

    // Switching the inspected asset is not representable by a vertex packet.
    auto switched = current;
    switched.viewport.inspected_mesh = imported;
    ++switched.document.revision;
    CHECK_FALSE(project::vertex_edit(current, switched));

    // Blueprint ownership does not depend on any surviving scene instance.
    current = std::move(switched);
    current.document.instances.clear();
    current.viewport.selected_object = 0;
    const auto builtin_document = current.document.mesh.document();
    before = current;
    REQUIRE(project::editable_mesh(current)->set_position(17, {2, 3, 4}));
    ++current.document.revision;
    auto imported_edit = project::vertex_edit(before, current);
    REQUIRE(imported_edit);
    CHECK(imported_edit->blueprint == 3);
    REQUIRE(project::apply_edit(before, *imported_edit));
    CHECK(before.document.mesh.document() == builtin_document);
    CHECK(before.document.mesh_assets.front().geometry.position(17) == Vec3{2, 3, 4});
}

TEST_CASE("Inferred vertex packets never use a fallback mesh when no target is editable",
          "[editor][mesh][target]") {
    auto current = state();
    SECTION("no scene selection") { current.viewport.selected_object = 0; }
    SECTION("sun scene selection") { current.viewport.selected_object = 2; }
    SECTION("sun effect view") { current.viewport.mode = project::ViewMode::sun; }
    SECTION("invalid blueprint selection") {
        current.viewport.mode = project::ViewMode::mesh;
        current.viewport.inspected_mesh = static_cast<project::BlueprintId>(99);
    }
    const auto before = current;
    ++current.document.revision;
    const std::array<u32, 1> touched{0};
    CHECK_FALSE(project::vertex_edit(before, current));
    CHECK_FALSE(project::vertex_edit(before.document.revision, current, touched));
    CHECK_FALSE(project::editable_mesh(current));
    CHECK(current.document.mesh.document() == before.document.mesh.document());
}

TEST_CASE("Absolute position patches coalesce from acknowledged baselines without replaying "
          "intermediates",
          "[editor][mesh]") {
    const auto before = state();
    auto middle = before;
    REQUIRE(middle.document.mesh.set_position(0, {2, 3, 4}));
    middle.document.revision = 2;
    auto after = middle;
    REQUIRE(after.document.mesh.set_position(0, {4, 5, 6}));
    REQUIRE(after.document.mesh.set_position(1, {8, 9, 10}));
    after.document.revision = 8;
    auto cumulative = project::vertex_edit(before, after);
    REQUIRE(cumulative);
    CHECK(cumulative->vertices.size() == 2);
    auto replica = before;
    REQUIRE(project::apply_edit(replica, *cumulative));
    CHECK(replica.document.mesh.document() == after.document.mesh.document());
    CHECK(replica.document.revision == after.document.revision);
    auto first = project::vertex_edit(before, middle);
    REQUIRE(first);
    replica = before;
    REQUIRE(project::apply_edit(replica, *first));
    CHECK_FALSE(project::apply_edit(replica, *cumulative));
    const std::array<u32, 3> touched{0, 1, 0};
    auto rebased = project::vertex_edit(replica.document.revision, after, touched);
    REQUIRE(rebased);
    CHECK(rebased->vertices.size() == 2);
    REQUIRE(project::apply_edit(replica, *rebased));
    CHECK(replica.document.mesh.document() == after.document.mesh.document());
    CHECK_FALSE(project::vertex_edit(replica.document.revision, after, touched));
    const std::array<u32, 1> wrong{99};
    CHECK_FALSE(project::vertex_edit(1, after, wrong));

    // A drag can return to its start after the worker accepted an intermediate.
    // Diff against the acknowledged mesh (not the gesture start) must restore it.
    auto restored = before;
    restored.document.revision = 9;
    auto restore =
        project::vertex_edit(middle.document.mesh, restored.document.mesh, middle.document.revision, restored.document.revision);
    REQUIRE(restore);
    CHECK(restore->vertices.size() == 1);
    REQUIRE(project::apply_edit(middle, *restore));
    CHECK(middle.document.mesh.document() == before.document.mesh.document());
}

TEST_CASE("Position diffs reject unrepresentable changes and beat full snapshot payloads",
          "[editor][mesh]") {
    const auto before = state(256);
    auto after = before;
    after.document.revision = 2;
    REQUIRE(after.document.mesh.set_position(17, {12.5F, 2.25F, -3}));
    auto edit = project::vertex_edit(before, after);
    REQUIRE(edit);
    CHECK(edit->vertices.size() == 1);
    const auto compact = encoded(*edit);
    const auto full = project::encode(after);
    REQUIRE(full);
    INFO("One vertex: " << compact.size() << " bytes, full scene: " << full->size() << " bytes");
    CHECK(compact.size() == 44);
    CHECK(full->size() > compact.size() * 100);
    auto settings = after;
    settings.viewport.editor_camera.yaw += 1;
    CHECK_FALSE(project::vertex_edit(before, settings));
    settings = after;
    settings.viewport.selected_vertex = 1;
    CHECK_FALSE(project::vertex_edit(before, settings));
    const auto changed_document = [&](auto change) {
        auto document = after.document.mesh.document();
        change(document);
        auto mesh = editor::EditableMesh::create(std::move(document));
        REQUIRE(mesh);
        CHECK_FALSE(project::vertex_edit(before.document.mesh, *mesh, 1, 2));
    };
    changed_document([](auto& document) { document.metadata["name"] = "Changed"; });
    changed_document([](auto& document) { document.faces[0] = {0, 2, 1}; });
    changed_document([](auto& document) { document.edges.reset(); });
    changed_document([](auto& document) {
        std::get<std::vector<f32>>(document.vertex_fields[0].values)[0] = .75F;
    });
}
