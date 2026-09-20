#include "../../examples/editor/mesh_import_view.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace vng;
using namespace editor_example;

TEST_CASE("Import inspection frames off-center geometry without changing the authored scene",
          "[editor][import][camera]") {
    content::vmesh::Document document;
    document.vertex_count = 3;
    document.vertex_fields = {{"position", {content::vmesh::ScalarType::Float32, 3},
                              std::vector<f32>{8, 3, 2, 12, 3, 2, 10, 7, 2}}};
    document.faces = {{0, 1, 2}};
    auto mesh = editor::EditableMesh::create(document);
    REQUIRE(mesh);
    State state{.document = {.mesh = std::move(*mesh)}};
    state.viewport.selected_object = 1;
    state.viewport.selected_vertex = 2;
    state.viewport.pilot_camera = true;
    state.document.animation_camera = {45, 20, 9, {1, 2, 3}};
    const auto camera_before = state.document.animation_camera;
    const auto instances_before = state.document.instances;
    const auto geometry_before = state.document.mesh.document();
    inspect_imported_mesh(state);
    CHECK(state.viewport.mode == ViewMode::mesh);
    CHECK_FALSE(state.viewport.pilot_camera);
    CHECK(state.viewport.paused);
    CHECK(state.viewport.selected_vertex == 0);
    CHECK(state.document.animation_camera == camera_before);
    CHECK(state.document.instances == instances_before);
    CHECK(state.document.mesh.document() == geometry_before);
    CHECK(state.viewport.editor_camera.target == Vec3{10, 5, 2});
    const auto points = project_vertices(state, {800, 600});
    REQUIRE(points.size() == 3);
    for (const auto& point : points) {
        REQUIRE(point);
        CHECK(point->x > .05F);
        CHECK(point->x < .95F);
        CHECK(point->y > .05F);
        CHECK(point->y < .95F);
    }
}

TEST_CASE("Import inspection is harmless without a selected mesh", "[editor][import][camera]") {
    content::vmesh::Document document;
    document.vertex_count = 1;
    document.vertex_fields = {{"position", {content::vmesh::ScalarType::Float32, 3},
                              std::vector<f32>{0, 0, 0}}};
    auto mesh = editor::EditableMesh::create(document);
    REQUIRE(mesh);
    State state{.document = {.mesh = std::move(*mesh)}};
    const auto pose = state.viewport.editor_camera;
    inspect_imported_mesh(state); // Sun is selected by default.
    CHECK(state.viewport.mode == ViewMode::scene);
    CHECK(state.viewport.editor_camera == pose);
}

TEST_CASE("Explicit blueprint inspection frames that asset without following scene selection",
          "[editor][import][camera][blueprint]") {
    content::vmesh::Document document;
    document.vertex_count = 3;
    document.vertex_fields = {{"position", {content::vmesh::ScalarType::Float32, 3},
                              std::vector<f32>{-1, -1, 0, 1, -1, 0, 0, 1, 0}}};
    document.faces = {{0, 1, 2}};
    auto builtin = editor::EditableMesh::create(document);
    REQUIRE(builtin);
    State state{.document = {.mesh = std::move(*builtin)}};
    auto imported = editor::EditableMesh::create(document);
    REQUIRE(imported);
    REQUIRE(imported->translate(std::array<u32, 3>{0, 1, 2}, {10, 4, 2}));
    const auto blueprint = static_cast<BlueprintId>(3);
    state.document.mesh_assets.push_back({blueprint, "Plane", std::move(*imported), {}});
    state.document.next_blueprint_id = 4;
    const auto object = instantiate(state, blueprint);
    REQUIRE(object);
    const auto instances = state.document.instances;
    const auto authored_camera = state.document.animation_camera;
    inspect_imported_mesh(state);
    CHECK(state.viewport.inspected_mesh == blueprint);
    CHECK(state.viewport.editor_camera.target == Vec3{10, 4, 2});
    REQUIRE(inspect_mesh(state, BlueprintId::mesh));
    CHECK(state.viewport.selected_object == *object);
    CHECK(state.viewport.inspected_mesh == BlueprintId::mesh);
    CHECK(state.viewport.editor_camera.target == Vec3{});
    CHECK(state.document.instances == instances);
    CHECK(state.document.animation_camera == authored_camera);
    REQUIRE(erase_instance(state, *object));
    REQUIRE(inspect_mesh(state, blueprint));
    CHECK(state.viewport.selected_object == 0);
    CHECK(state.viewport.editor_camera.target == Vec3{10, 4, 2});
    CHECK(mesh_target(state) == std::optional<MeshTarget>{{blueprint, {}}});
    const auto points = project_vertices(state, {800, 600});
    REQUIRE(points.size() == 3);
    for (const auto& point : points) REQUIRE(point);
}
