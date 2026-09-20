#include "../../examples/editor/vertex_drag.hpp"
#include "../../examples/editor/edits.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
using namespace vng;
using namespace editor_example;
State scene() {
    content::vmesh::Document document;
    document.vertex_count = 3;
    document.vertex_fields = {{"position",
                               {content::vmesh::ScalarType::Float32, 3},
                               std::vector<f32>{0, 0, 0, 1, 0, 0, 0, 1, 0}}};
    document.faces = {{0, 1, 2}};
    auto mesh = editor::EditableMesh::create(std::move(document));
    REQUIRE(mesh);
    return {.document = {.mesh = std::move(*mesh)}, .viewport = {.selected_object = 1}};
}
void moves_by(State& state, Vec2 pixels, Extent2D extent) {
    const Vec2 size{static_cast<f32>(extent.width), static_cast<f32>(extent.height)};
    const auto before = project_vertex(state, 0, extent);
    REQUIRE(before);
    const auto delta = vertex_drag_delta(state, 0, pixels, size);
    REQUIRE(delta);
    auto* mesh = editable_mesh(state);
    REQUIRE(mesh);
    const auto original = mesh->position(0);
    REQUIRE(mesh->set_position(
        0, {original.x + delta->x, original.y + delta->y, original.z + delta->z}));
    const auto after = project_vertex(state, 0, extent);
    REQUIRE(after);
    CHECK((after->x - before->x) * size.x == Catch::Approx(pixels.x).margin(.001));
    CHECK((after->y - before->y) * size.y == Catch::Approx(pixels.y).margin(.001));
}
} // namespace

TEST_CASE("Vertex drag tracks exact logical pixels through orbit pan and model transforms",
          "[editor][vertex-drag]") {
    for (const auto mode : {ViewMode::scene, ViewMode::mesh}) {
        for (const auto scale : {.05F, .7F, 3.0F}) {
            auto state = scene();
            state.viewport.mode = mode;
            (*editor_example::instance_transform(state, 1)).scale = scale;
            (*editor_example::instance_transform(state, 1)).position = {.3F, -.1F, .4F};
            instance_transform(state, 1)->rotation = {20, -35, 15};
            state.viewport.editor_camera.target = {.2F, .15F, -.3F};
            state.viewport.editor_camera.yaw = 37;
            state.viewport.editor_camera.pitch = -23;
            REQUIRE(state.document.mesh.set_position(0, {.1F, -.05F, .2F}));
            moves_by(state, {23, -17}, {960, 540});
            moves_by(state, {-9, 11}, {752, 564});
        }
    }
}

TEST_CASE("Vertex drag uses near vertex depth rather than camera pivot distance",
          "[editor][vertex-drag]") {
    auto state = scene();
    state.viewport.mode = ViewMode::mesh;
    (*editor_example::instance_transform(state, 1)).scale = 1;
    state.viewport.editor_camera.yaw = state.viewport.editor_camera.pitch = 0;
    state.viewport.editor_camera.distance = .2F;
    REQUIRE(state.document.mesh.set_position(0, {0, 0, .04F}));
    const auto delta = vertex_drag_delta(state, 0, {20, 0}, {800, 600});
    REQUIRE(delta);
    const auto true_depth = .2 * .52 - .04;
    const auto expected =
        20 * 2 * true_depth * std::tan(camera_vertical_fov * std::numbers::pi / 360) / 600;
    CHECK(delta->x == Catch::Approx(expected));
    CHECK(delta->y == 0.0F);
    CHECK(delta->z == 0.0F);
    moves_by(state, {20, -12}, {800, 600});
}

TEST_CASE("Blueprint drags ignore scene selection visibility and transforms after importing",
          "[editor][vertex-drag][target]") {
    auto state = scene();
    const auto imported = static_cast<BlueprintId>(3);
    auto other = scene();
    REQUIRE(other.document.mesh.set_position(0, {.1F, .2F, .3F}));
    state.document.mesh_assets.push_back({imported, "Imported", std::move(other.document.mesh), {}});
    state.document.next_blueprint_id = 4;
    const auto instance = instantiate(state, imported);
    REQUIRE(instance);
    mesh_settings(state, *instance)->visible = false;
    instance_transform(state, *instance)->position = {30, 40, 50};
    instance_transform(state, *instance)->rotation = {100, -130, 150};
    instance_transform(state, *instance)->scale = 3;
    state.viewport.mode = ViewMode::mesh;
    state.viewport.inspected_mesh = BlueprintId::mesh;
    const auto imported_document = state.document.mesh_assets.front().geometry.document();
    moves_by(state, {14, -8}, {800, 600});
    CHECK(state.document.mesh_assets.front().geometry.document() == imported_document);

    state.viewport.inspected_mesh = imported;
    const auto builtin_document = state.document.mesh.document();
    moves_by(state, {-11, 18}, {800, 600});
    CHECK(state.document.mesh.document() == builtin_document);
    state.document.instances.clear();
    state.viewport.selected_object = 0;
    moves_by(state, {7, -4}, {800, 600});
    CHECK(state.document.mesh.document() == builtin_document);

    state.viewport.mode = ViewMode::scene;
    CHECK_FALSE(vertex_drag_delta(state, 0, {1, 1}, {800, 600}));
    state = scene();
    state.viewport.selected_object = 2;
    CHECK_FALSE(vertex_drag_delta(state, 0, {1, 1}, {800, 600}));
    state.viewport.selected_object = 0;
    CHECK_FALSE(vertex_drag_delta(state, 0, {1, 1}, {800, 600}));
}

TEST_CASE("Vertex drag is frozen-state pure and independent of framebuffer density",
          "[editor][vertex-drag]") {
    auto state = scene();
    const auto document = state.document.mesh.document();
    const auto revision = state.document.revision;
    const auto a = vertex_drag_delta(state, 0, {25, -35}, {752, 564});
    const auto b = vertex_drag_delta(state, 0, {50, -70}, {1504, 1128});
    REQUIRE(a);
    REQUIRE(b);
    CHECK(*a == *b);
    const auto zero = vertex_drag_delta(state, 0, {}, {752.5F, 564.25F});
    REQUIRE(zero);
    CHECK(*zero == Vec3{});
    CHECK(state.document.mesh.document() == document);
    CHECK(state.document.revision == revision);
}

TEST_CASE("Vertex drag rejects invalid inputs hidden vertices and clipped depth",
          "[editor][vertex-drag]") {
    auto state = scene();
    const auto nan = std::numeric_limits<f32>::quiet_NaN();
    const auto infinity = std::numeric_limits<f32>::infinity();
    const auto huge = std::numeric_limits<f32>::max();
    CHECK_FALSE(vertex_drag_delta(state, 3, {1, 1}, {800, 600}));
    CHECK_FALSE(vertex_drag_delta(state, 0, {nan, 1}, {800, 600}));
    CHECK_FALSE(vertex_drag_delta(state, 0, {1, infinity}, {800, 600}));
    CHECK_FALSE(vertex_drag_delta(state, 0, {1, 1}, {0, 600}));
    CHECK_FALSE(vertex_drag_delta(state, 0, {1, 1}, {800, -1}));
    CHECK_FALSE(vertex_drag_delta(state, 0, {1, 1}, {800, nan}));
    CHECK_FALSE(vertex_drag_delta(state, 0, {1, 1}, {infinity, 600}));
    (*editor_example::mesh_settings(state, 1)).visible = false;
    CHECK_FALSE(vertex_drag_delta(state, 0, {1, 1}, {800, 600}));
    (*editor_example::mesh_settings(state, 1)).visible = true;
    state.viewport.mode = ViewMode::sun;
    CHECK_FALSE(vertex_drag_delta(state, 0, {1, 1}, {800, 600}));
    state.viewport.mode = ViewMode::scene;
    for (const auto scale : {0.0F, -1.0F, nan, infinity}) {
        (*editor_example::instance_transform(state, 1)).scale = scale;
        CHECK_FALSE(vertex_drag_delta(state, 0, {1, 1}, {800, 600}));
    }
    (*editor_example::instance_transform(state, 1)).scale = 1;
    state.viewport.mode = ViewMode::mesh;
    state.viewport.editor_camera.yaw = state.viewport.editor_camera.pitch = 0;
    state.viewport.editor_camera.distance = 8;
    for (const auto z : {10.0F, 4.15F, -201.0F}) {
        REQUIRE(state.document.mesh.set_position(0, {0, 0, z}));
        CHECK_FALSE(vertex_drag_delta(state, 0, {1, 1}, {800, 600}));
    }
    REQUIRE(state.document.mesh.set_position(0, {}));
    CHECK_FALSE(vertex_drag_delta(state, 0, {huge, huge}, {800, .000001F}));
    state.viewport.editor_camera.target.x = nan;
    CHECK_FALSE(vertex_drag_delta(state, 0, {1, 1}, {800, 600}));
}

TEST_CASE("Position-only edit inference rejects a changed camera pivot", "[editor][vertex-drag]") {
    const auto before = scene();
    auto after = before;
    ++after.document.revision;
    after.viewport.editor_camera.target = {1, 2, 3};
    CHECK_FALSE(vertex_edit(before, after));
    after.viewport.editor_camera.target = before.viewport.editor_camera.target;
    REQUIRE(vertex_edit(before, after));
}
