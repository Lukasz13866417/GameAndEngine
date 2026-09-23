#include <vng/editor/mesh.hpp>
#include "../../examples/editor/project.hpp"
#include "../../examples/editor/editing_session.hpp"
#include "../../examples/editor/animation.hpp"
#include "../../examples/editor/preview_values.hpp"
#include "../../examples/editor/document_patch.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <vng/content/document.hpp>
#include <variant>
#include <filesystem>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <cmath>
#include <limits>
#include <string>
#include <unistd.h>

namespace {
using namespace vng;
namespace vm = content::vmesh;
namespace project = editor_example;
template<class T> concept HasPosition = requires(T value) { value.position; };
template<class T> concept HasScale = requires(T value) { value.scale; };
template<class T> concept HasTransform = requires(T value) { value.transform; };
static_assert(HasPosition<project::InstanceTransform> && HasScale<project::InstanceTransform>);
static_assert(HasTransform<project::SceneInstance>);
static_assert(!HasPosition<project::MeshSettings> && !HasScale<project::MeshSettings>);
static_assert(!HasPosition<project::SunSettings> && !HasScale<project::SunSettings>);
static_assert(!HasTransform<project::MeshBlueprint>);

vm::Document source_mesh() {
    vm::Document result;
    result.metadata = {{"name", "Seam test"},
                       {"author", "quoted \"label\"\nsecond line"},
                       {"custom/purpose", "preserve every field"}};
    result.vertex_count = 4;
    result.vertex_fields = {
        {"custom/material", {vm::ScalarType::UInt32, 1}, std::vector<u32>{7, 8, 9, 10}},
        {"color/0",
         {vm::ScalarType::Float32, 4},
         std::vector<f32>{1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, .25F, .5F, .75F, 1}},
        {"position",
         {vm::ScalarType::Float32, 3},
         std::vector<f32>{0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0}},
        {"normal",
         {vm::ScalarType::Float32, 3},
         std::vector<f32>{0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, -1}},
        {"custom/signed",
         {vm::ScalarType::Int32, 2},
         std::vector<i32>{-8, 1, -4, 2, -2, 3, -1, 4}}};
    result.faces = {{0, 1, 2}, {3, 2, 1}};
    result.edges = std::vector<gfx::Edge>{{0, 1}, {1, 2}, {2, 3}};
    return result;
}
editor::EditableMesh mesh() {
    auto result = editor::EditableMesh::create(source_mesh());
    INFO((result ? "mesh created" : result.error().message));
    REQUIRE(result);
    return std::move(*result);
}
project::State state() {
    return {.document = {.mesh = mesh()}};
}
std::string encoded(const project::State& value) {
    auto text = project::encode(value);
    INFO((text ? "scene encoded" : text.error().message));
    REQUIRE(text);
    return std::move(*text);
}
std::string replace_value(std::string text, std::string_view key, std::string_view replacement) {
    const auto marker = std::string(key) + " = ";
    auto start = text.find(marker);
    REQUIRE(start != std::string::npos);
    start += marker.size();
    const auto end = text.find(';', start);
    REQUIRE(end != std::string::npos);
    text.replace(start, end - start, replacement);
    return text;
}
std::string omit_value(std::string text, std::string_view key) {
    const auto begin = text.find(std::string(key) + " = ");
    REQUIRE(begin != std::string::npos);
    const auto end = text.find(';', begin);
    REQUIRE(end != std::string::npos);
    text.erase(begin, end - begin + 1);
    return text;
}
} // namespace

TEST_CASE("Editable meshes round trip metadata arbitrary attributes faces and explicit edges",
          "[editor][mesh]") {
    const auto original = source_mesh();
    auto edited = editor::EditableMesh::create(original);
    REQUIRE(edited);
    CHECK(edited->document() == original);
    CHECK(edited->size() == 4);
    CHECK(edited->position(1) == Vec3{1, 0, 0});
    REQUIRE(edited->set_position(1, {1.125F, -2.5F, 3.75F}));
    auto expected = original;
    auto& values = std::get<std::vector<f32>>(expected.vertex_fields[2].values);
    values[3] = 1.125F;
    values[4] = -2.5F;
    values[5] = 3.75F;
    CHECK(edited->document() == expected);
    auto bytes = vm::write_vmesh(edited->document());
    REQUIRE(bytes);
    auto reloaded = vm::parse_vmesh(*bytes);
    REQUIRE(reloaded);
    CHECK(*reloaded == expected);
    auto again = editor::EditableMesh::create(std::move(*reloaded));
    REQUIRE(again);
    CHECK(again->document() == expected);

    auto no_edges = original;
    no_edges.edges.reset();
    auto no_edges_text = vm::write_vmesh(no_edges);
    REQUIRE(no_edges_text);
    auto no_edges_reload = vm::parse_vmesh(*no_edges_text);
    REQUIRE(no_edges_reload);
    CHECK_FALSE(no_edges_reload->edges.has_value());
    no_edges.edges = std::vector<gfx::Edge>{};
    auto empty_edges_text = vm::write_vmesh(no_edges);
    REQUIRE(empty_edges_text);
    auto empty_edges_reload = vm::parse_vmesh(*empty_edges_text);
    REQUIRE(empty_edges_reload);
    REQUIRE(empty_edges_reload->edges.has_value());
    CHECK(empty_edges_reload->edges->empty());
}

TEST_CASE("Authored environment settings round trip independently of private editor state",
          "[editor][project][environment]") {
    auto value = state();
    const project::EnvironmentSettings settings{.stars = 6000,
        .star_seed = std::numeric_limits<u32>::max(), .exposure = 1.3F,
        .bloom_threshold = 3.25F, .bloom_strength = .4F};
    value.document.environment = settings;
    const auto wire = encoded(value);
    auto restored = project::decode(wire);
    REQUIRE(restored);
    CHECK(restored->document.environment == settings);
    CHECK(encoded(*restored) == wire);
    auto saved = project::encode_scene(value);
    REQUIRE(saved);
    auto loaded = project::decode(*saved);
    REQUIRE(loaded);
    CHECK(loaded->document.environment == settings);
    CHECK(loaded->document.mesh.document() == value.document.mesh.document());
}

TEST_CASE("Old scene files and partial environment sections retain appearance defaults",
          "[editor][project][environment]") {
    const auto original = encoded(state());
    const auto begin = original.find("environment = {");
    REQUIRE(begin != std::string::npos);
    const auto end = original.find("};", begin);
    REQUIRE(end != std::string::npos);
    auto legacy = original;
    legacy.erase(begin, end + 2 - begin);
    auto decoded = project::decode(legacy);
    REQUIRE(decoded);
    CHECK(decoded->document.environment == project::EnvironmentSettings{});
    CHECK(decoded->document.environment.exposure == .9F);
    CHECK(decoded->document.environment.bloom_threshold == 6.5F);
    auto partial = original;
    partial.replace(begin, end + 2 - begin, "environment = { exposure = 1.25; };");
    decoded = project::decode(partial);
    REQUIRE(decoded);
    CHECK(decoded->document.environment == project::EnvironmentSettings{.exposure = 1.25F});
    auto empty = original;
    empty.replace(begin, end + 2 - begin, "environment = {};");
    decoded = project::decode(empty);
    REQUIRE(decoded);
    CHECK(decoded->document.environment == project::EnvironmentSettings{});
}

TEST_CASE("Malformed authored environment values are rejected before becoming scene state",
          "[editor][project][environment]") {
    const auto original = encoded(state());
    for (const auto& [key, replacement] : std::array<std::pair<std::string_view, std::string_view>, 12>{{
             {"stars", "20001"}, {"stars", "-1"}, {"stars", "true"},
             {"star_seed", "4294967296"}, {"star_seed", "-1"},
             {"exposure", "0"}, {"exposure", "10.01"}, {"exposure", "\"dim\""},
             {"bloom_threshold", "-0.01"}, {"bloom_threshold", "100.01"},
             {"bloom_strength", "-0.01"}, {"bloom_strength", "1.01"}}}) {
        INFO(key << " = " << replacement);
        CHECK_FALSE(project::decode(replace_value(original, key, replacement)));
    }
    for (const auto& bad : std::array{
             project::EnvironmentSettings{.stars = 20001},
             project::EnvironmentSettings{.exposure = std::numeric_limits<f32>::quiet_NaN()},
             project::EnvironmentSettings{.bloom_threshold = std::numeric_limits<f32>::infinity()},
             project::EnvironmentSettings{.bloom_strength = -1}}) {
        auto value = state();
        value.document.environment = bad;
        CHECK_FALSE(project::encode(value));
        CHECK_FALSE(project::encode_scene(value));
    }
}

TEST_CASE("Camera undo snapshots keep mesh storage and compose with document history",
          "[editor][project][camera][history]") {
    auto initial=state();
    const auto camera=project::ensure_camera(initial,{0,0,8,{}});
    REQUIRE(camera);
    REQUIRE(project::key_camera(initial,*camera,0,{0,0,8,{}}));
    REQUIRE(project::key_camera(initial,*camera,5,{30,10,6,{1,2,3}}));
    REQUIRE(project::key_property(initial,{1,"position"},4,Vec3{3,4,5}));
    initial.viewport.time=5;
    project::EditingSession editing{std::move(initial)}; editing.select_keyframe(editing.state().viewport.time);
    const auto& value=editing.state();
    const auto before_camera=project::evaluate_camera(value,5);
    const auto mesh_address=std::get<std::vector<f32>>(value.document.mesh.document().vertex_fields[2].values).data();
    const auto other_track=*value.document.timeline.find({1,"position"});
    REQUIRE(editing.set_camera(*camera,{60,20,9,{3,4,5}}));
    const auto changed_camera=project::evaluate_camera(value,5);
    CHECK(changed_camera!=before_camera);
    const auto viewport=value.viewport;
    const auto instance_identity=value.document.next_instance_id;
    REQUIRE(editing.undo());
    CHECK(project::evaluate_camera(value,5)==before_camera);
    CHECK(value.document.revision==3);
    CHECK(value.viewport.sequence==viewport.sequence+1);
    CHECK(value.viewport.editor_camera==viewport.editor_camera);
    CHECK(value.document.next_instance_id==instance_identity);
    CHECK(*value.document.timeline.find({1,"position"})==other_track);
    CHECK(std::get<std::vector<f32>>(value.document.mesh.document().vertex_fields[2].values).data()==mesh_address);
    REQUIRE(editing.redo());
    CHECK(project::evaluate_camera(value,5)==changed_camera);
    CHECK(std::get<std::vector<f32>>(value.document.mesh.document().vertex_fields[2].values).data()==mesh_address);

    // Structural and sparse edits share the same ordered undo stack.
    const auto created=editing.instantiate(project::BlueprintId::mesh);
    REQUIRE(created);
    const auto allocated=value.document.next_instance_id;
    REQUIRE(editing.undo());
    CHECK_FALSE(project::find_instance(value,*created));
    CHECK(project::evaluate_camera(value,5)==changed_camera);
    REQUIRE(editing.undo());
    CHECK(project::evaluate_camera(value,5)==before_camera);
    CHECK(value.document.next_instance_id==allocated);
    REQUIRE(editing.redo());
    CHECK(project::evaluate_camera(value,5)==changed_camera);
    REQUIRE(editing.redo());
    CHECK(project::find_instance(value,*created));
    CHECK(value.document.next_instance_id==allocated);
}

TEST_CASE("Mesh edits distinguish unique vertices from position-welded seam copies",
          "[editor][mesh]") {
    auto single = mesh();
    CHECK(single.coincident(0) == std::vector<u32>{0, 3});
    REQUIRE(single.set_position(0, {2, 3, 4}));
    CHECK(single.position(0) == Vec3{2, 3, 4});
    CHECK(single.position(3) == Vec3{});
    CHECK(single.coincident(0) == std::vector<u32>{0});

    auto welded = mesh();
    auto selected = welded.coincident(0);
    selected.push_back(0); // Duplicated selections must not translate twice.
    selected.push_back(3);
    const auto old = welded.document();
    REQUIRE(welded.translate(selected, {.5F, 1.25F, -.5F}));
    CHECK(welded.position(0) == Vec3{.5F, 1.25F, -.5F});
    CHECK(welded.position(3) == welded.position(0));
    CHECK(welded.position(1) == Vec3{1, 0, 0});
    CHECK(welded.document().faces == old.faces);
    CHECK(welded.document().edges == old.edges);
    CHECK(welded.document().vertex_fields[1] == old.vertex_fields[1]);
    CHECK(welded.document().vertex_fields[3] == old.vertex_fields[3]);
    REQUIRE(welded.set_position(3, {.500004F, 1.25F, -.5F}));
    CHECK(welded.coincident(0).size() == 2);
    CHECK(welded.coincident(0, 0).size() == 1);
    CHECK_THROWS_AS(welded.coincident(0, -1), std::invalid_argument);
    CHECK_THROWS_AS(welded.coincident(0, std::numeric_limits<f32>::quiet_NaN()),
                    std::invalid_argument);
}

TEST_CASE("Invalid mesh edits fail atomically before modifying any vertex", "[editor][mesh]") {
    auto edited = mesh();
    const auto original = edited.document();
    const std::array<u32, 3> invalid_selection{0, 1, 999};
    CHECK_FALSE(edited.translate(invalid_selection, {1, 2, 3}));
    CHECK(edited.document() == original);
    CHECK_FALSE(edited.set_position(999, {}));
    CHECK_FALSE(edited.set_position(0, {std::numeric_limits<f32>::infinity(), 0, 0}));
    CHECK_FALSE(edited.set_position(0, {0, std::numeric_limits<f32>::quiet_NaN(), 0}));
    CHECK_FALSE(
        edited.translate(std::array<u32, 2>{0, 1}, {0, 0, std::numeric_limits<f32>::quiet_NaN()}));
    CHECK(edited.document() == original);
    REQUIRE(edited.set_position(1, {1000000, 0, 0}));
    const auto bounded = edited.document();
    CHECK_FALSE(edited.translate(std::array<u32, 2>{0, 1}, {1, 0, 0}));
    CHECK(edited.document() == bounded);
    REQUIRE(edited.translate({}, {1, 2, 3}));
    CHECK(edited.document() == bounded);
    REQUIRE(edited.translate(std::array<u32, 1>{1}, {-2000000, 0, 0}));
    CHECK(edited.position(1) == Vec3{-1000000, 0, 0});
    CHECK_THROWS_AS(edited.position(999), std::out_of_range);
}

TEST_CASE("Editable meshes reject invalid schemas topology and resource limits", "[editor][mesh]") {
    auto missing = source_mesh();
    missing.vertex_fields.erase(missing.vertex_fields.begin() + 2);
    CHECK_FALSE(editor::EditableMesh::create(std::move(missing)));
    auto wrong_type = source_mesh();
    wrong_type.vertex_fields[2] = {"position", {vm::ScalarType::Float32, 2}, std::vector<f32>(8)};
    CHECK_FALSE(editor::EditableMesh::create(std::move(wrong_type)));
    auto wrong_scalar = source_mesh();
    wrong_scalar.vertex_fields[2] = {"position", {vm::ScalarType::Int32, 3}, std::vector<i32>(12)};
    CHECK_FALSE(editor::EditableMesh::create(std::move(wrong_scalar)));
    auto bad_face = source_mesh();
    bad_face.faces.front()[0] = 4;
    CHECK_FALSE(editor::EditableMesh::create(std::move(bad_face)));
    auto bad_edge = source_mesh();
    bad_edge.edges->front()[1] = 4;
    CHECK_FALSE(editor::EditableMesh::create(std::move(bad_edge)));
    auto bad_position = source_mesh();
    std::get<std::vector<f32>>(bad_position.vertex_fields[2].values)[0] = 1000001;
    CHECK_FALSE(editor::EditableMesh::create(std::move(bad_position)));
    vm::Document oversized;
    oversized.vertex_count = 65537;
    oversized.vertex_fields.push_back(
        {"position", {vm::ScalarType::Float32, 3}, std::vector<f32>(65537 * 3)});
    CHECK_FALSE(editor::EditableMesh::create(std::move(oversized)));
    vm::Document empty;
    empty.vertex_fields.push_back({"position", {vm::ScalarType::Float32, 3}, std::vector<f32>{}});
    CHECK_FALSE(editor::EditableMesh::create(std::move(empty)));
}

TEST_CASE("Editor project serialization round trips authored scene and complete mesh",
          "[editor][project]") {
    auto original = state();
    original.document.revision = 10000000001ULL;
    (*editor_example::mesh_settings(original, 1)) = {.75F, false, true};
    (*editor_example::sun_settings(original, 2)) = {2.25F, .75F, .125F, true, false};
    *project::instance_transform(original, 1) = {{-3.5F, .25F, 7}, {17, -23, 90}, 1.125F};
    *project::instance_transform(original, 2) = {{4, -1, 3}, {-90, 180, 360}, .5F};
    original.viewport.mode = project::ViewMode::mesh;
    original.viewport.selected_object = 1;
    original.viewport.selected_vertex = 3;
    original.viewport.weld = false;
    original.viewport.paused = false;
    original.viewport.time = 7.125F;
    original.viewport.editor_camera.yaw = -33;
    original.viewport.editor_camera.pitch = 23;
    original.viewport.editor_camera.distance = 4.5F;
    REQUIRE(project::ensure_camera(original, {70, -15, 12, {1, -2, 3}}));
    REQUIRE(original.document.mesh.set_position(0, {-.5F, .25F, 1.125F}));
    const auto text = encoded(original);
    auto round_trip = project::decode(text);
    INFO((round_trip ? "scene decoded" : round_trip.error().message));
    REQUIRE(round_trip);
    CHECK(round_trip->document.mesh.document() == original.document.mesh.document());
    CHECK((*editor_example::mesh_settings(*round_trip, 1)) == (*editor_example::mesh_settings(original, 1)));
    CHECK((*editor_example::sun_settings(*round_trip, 2)) == (*editor_example::sun_settings(original, 2)));
    CHECK(round_trip->document.instances == original.document.instances);
    CHECK(text.find("editor_project = 5;") != std::string::npos);
    CHECK(text.find("animation_camera") == std::string::npos);
    CHECK(round_trip->document.revision == original.document.revision);
    CHECK(round_trip->viewport.mode == original.viewport.mode);
    CHECK(round_trip->viewport.selected_object == original.viewport.selected_object);
    CHECK(round_trip->viewport.selected_vertex == original.viewport.selected_vertex);
    CHECK(round_trip->viewport.weld == original.viewport.weld);
    CHECK(round_trip->viewport.paused == original.viewport.paused);
    CHECK(round_trip->viewport.time == original.viewport.time);
    CHECK(round_trip->viewport.editor_camera.yaw == original.viewport.editor_camera.yaw);
    CHECK(round_trip->viewport.editor_camera.pitch == original.viewport.editor_camera.pitch);
    CHECK(round_trip->viewport.editor_camera.distance == original.viewport.editor_camera.distance);
    CHECK(encoded(*round_trip) == text);
}

TEST_CASE("Deleting scene instances retains blueprints without resurrecting animation",
          "[editor][project][instances]") {
    auto initial = state();
    const auto geometry = initial.document.mesh.document();
    initial.document.mesh_blueprint.brightness = 2;
    REQUIRE(project::key_property(initial, {1, "position"}, 4, Vec3{2, 3, 1}));
    REQUIRE(project::key_property(initial, {2, "radius"}, 4, 2.F));
    initial.viewport.selected_object = 1;
    project::EditingSession editing{std::move(initial)}; editing.select_keyframe(editing.state().viewport.time);
    const auto& value = editing.state();
    REQUIRE(editing.erase_instances(std::array<u32,1>{1}));
    CHECK(value.viewport.selected_object == 0);
    CHECK_FALSE(project::find_instance(value, 1));
    CHECK_FALSE(value.document.timeline.find({1, "position"}));
    CHECK(value.document.timeline.find({2, "radius"}));
    CHECK(value.document.mesh.document() == geometry);
    CHECK(project::blueprint_catalog(value).size() == 4);
    CHECK_FALSE(editing.erase_instances(std::array<u32,1>{1}));
    REQUIRE(editing.undo());
    REQUIRE(project::find_instance(value, 1));
    CHECK(value.document.timeline.find({1, "position"}));
    REQUIRE(editing.redo());
    REQUIRE(editing.erase_instances(std::array<u32,1>{2}));
    CHECK(project::scene_instances(value).empty());
    CHECK(value.document.timeline.tracks().empty());
    auto persisted = project::decode(encoded(value));
    REQUIRE(persisted);
    CHECK(persisted->document.instances.empty());
    CHECK(persisted->document.mesh.document() == geometry);
    const auto first = project::instantiate(*persisted, project::BlueprintId::mesh);
    REQUIRE(first);
    const auto second = project::instantiate(*persisted, project::BlueprintId::mesh);
    REQUIRE(second);
    CHECK(*first >= 3);
    CHECK(*second > *first);
    CHECK(project::mesh_settings(*persisted, *first)->brightness == 2.0F);
    project::instance_transform(*persisted, *first)->position = {3, 2, 1};
    CHECK(project::instance_transform(*persisted, *second)->position != Vec3{3, 2, 1});
    CHECK(persisted->document.timeline.tracks().empty());
    CHECK(project::animation_properties(*persisted).size() == 14);
    const auto restored = project::decode(encoded(*persisted));
    REQUIRE(restored);
    CHECK(restored->document.instances == persisted->document.instances);
    CHECK(restored->document.next_instance_id == persisted->document.next_instance_id);
    CHECK_FALSE(editing.instantiate(static_cast<project::BlueprintId>(7)));
}

TEST_CASE("Scene instances own transforms independently of shared blueprints", "[editor][project][transform]") {
    auto value = state();
    CHECK(*project::instance_transform(value, 1) == project::InstanceTransform{{1.7F, 0, 0}, {}, .7F});
    CHECK(*project::instance_transform(value, 2) == project::InstanceTransform{{-1.6F, 0, 0}, {}, 1});
    CHECK_FALSE(project::instance_transform(value, 999));
    CHECK_FALSE(project::instance_transform(std::as_const(value), 999));
    value.document.mesh_blueprint.brightness = 2;
    const auto geometry = value.document.mesh.document();
    const auto first = project::instantiate(value, project::BlueprintId::mesh);
    REQUIRE(first);
    const auto second = project::instantiate(value, project::BlueprintId::mesh);
    REQUIRE(second);
    CHECK(project::instance_mesh(value, *first) == project::instance_mesh(value, *second));
    const auto untouched = *project::find_instance(value, *second);
    *project::instance_transform(value, *first) = {{3, -4, 5}, {20, -40, 70}, 1.75F};
    CHECK(*project::find_instance(value, *second) == untouched);
    CHECK(value.document.mesh.document() == geometry);
    CHECK(value.document.mesh_blueprint.brightness == 2.F);
    CHECK(project::mesh_settings(value, *first)->brightness == 2.F);
    const auto text = encoded(value);
    const auto begin = text.find("blueprints = ");
    const auto end = text.find("instances = ");
    REQUIRE(begin < end);
    const auto definitions = text.substr(begin, end - begin);
    CHECK(definitions.find("position = ") == std::string::npos);
    CHECK(definitions.find("rotation = ") == std::string::npos);
    CHECK(definitions.find("scale = ") == std::string::npos);
    REQUIRE(project::decode(text));
    project::EditingSession editing{std::move(value)}; editing.select_keyframe(editing.state().viewport.time);
    REQUIRE(editing.set_transform(*first, {-10, 20, -30}, 1.75F));
    REQUIRE(editing.undo());
    CHECK(project::instance_transform(editing.state(), *first)->rotation == Vec3{20, -40, 70});
    CHECK(*project::find_instance(editing.state(), *second) == untouched);
    REQUIRE(editing.redo());
    CHECK(project::instance_transform(editing.state(), *first)->rotation == Vec3{-10, 20, -30});
}

TEST_CASE("Instance transforms apply uniform scale then Rx Ry Rz then translation", "[editor][project][transform]") {
    const auto point = [](const Mat4& matrix, Vec3 position) {
        Vec3 result{};
        for (std::size_t row = 0; row < 3; ++row) {
            result[row] = matrix[3][row];
            for (std::size_t column = 0; column < 3; ++column)
                result[row] += matrix[column][row] * position[column];
        }
        return result;
    };
    const project::InstanceTransform transform{{1, 2, 3}, {90, 90, 90}, 2};
    const auto matrix = project::mesh_transform(project::ViewMode::scene, transform);
    const auto result = point(matrix, {1, 2, 3});
    CHECK(result.x == Catch::Approx(7).margin(.00001F));
    CHECK(result.y == Catch::Approx(6).margin(.00001F));
    CHECK(result.z == Catch::Approx(1).margin(.00001F));
    CHECK(matrix[3] == Vec4{1, 2, 3, 1});
    CHECK(project::mesh_transform(project::ViewMode::mesh, transform) == Mat4::identity());
    CHECK(project::mesh_transform(project::ViewMode::scene, project::InstanceTransform{}) == Mat4::identity());
    for (std::size_t axis = 0; axis < 3; ++axis) {
        project::InstanceTransform rotation;
        rotation.rotation[axis] = 90;
        Vec3 input{};
        input[(axis + 1) % 3] = 1;
        const auto rotated = point(project::mesh_transform(project::ViewMode::scene, rotation), input);
        CHECK(rotated[axis] == Catch::Approx(0).margin(.00001F));
        CHECK(rotated[(axis + 1) % 3] == Catch::Approx(0).margin(.00001F));
        CHECK(rotated[(axis + 2) % 3] == Catch::Approx(1).margin(.00001F));
    }
    auto value = state();
    value.viewport.editor_camera = {0, 0, 8};
    value.viewport.selected_object = 1;
    *project::instance_transform(value, 1) = {{}, {}, 1};
    const auto origin = project::project_vertex(value, 0, {640, 480});
    const auto before = project::project_vertex(value, 1, {640, 480});
    REQUIRE(origin);
    REQUIRE(before);
    CHECK(before->x > origin->x);
    project::instance_transform(value, 1)->rotation.z = 90;
    const auto after = project::project_vertex(value, 1, {640, 480});
    REQUIRE(after);
    CHECK(after->x == Catch::Approx(origin->x).margin(.00001F));
    CHECK(after->y < origin->y);
    value.viewport.mode = project::ViewMode::mesh;
    CHECK(project::mesh_transform(value) == Mat4::identity());
}

TEST_CASE("Legacy scene layouts migrate positions and scales out of appearance settings", "[editor][project][legacy][transform]") {
    auto text = omit_value(replace_value(encoded(state()), "editor_project", "1"), "inspected_mesh");
    const auto begin = text.find("blueprints = ");
    const auto end = text.find("world_bounds = ");
    const auto mesh_begin = text.find("mesh_data = ");
    REQUIRE(mesh_begin < begin);
    REQUIRE(begin < end);
    const auto mesh_field = text.substr(mesh_begin, begin - mesh_begin);
    const std::string old_mesh = "{ position = [-3,4,5]; scale = 1.25; brightness = 0.5; visible = true; wireframe = true; }";
    const std::string old_sun = "{ position = [6,-7,8]; radius = 2; displacement = 0.5; bloom = 0.3; white_spots = true; visible = false; }";
    std::string body;
    bool imported{};
    SECTION("Original single-mesh and sun scene") {
        body = "model = " + old_mesh + ";\nsun = " + old_sun + ";\n";
    }
    SECTION("Instances and imported reusable mesh blueprints") {
        imported = true;
        body = "blueprints = { mesh = { position = [90,90,90]; scale = 0.1; brightness = 2; visible = true; wireframe = false; }; sun = " + old_sun + "; };\n"
            "next_blueprint_id = 8; next_instance_id = 7; mesh_assets = [{ id = 7; name = \"Imported\"; " + mesh_field +
            "settings = { position = [88,88,88]; scale = 0.2; brightness = 3; visible = true; wireframe = false; }; }];\n"
            "instances = [{ id = 1; blueprint = 1; name = \"Mesh\"; settings = " + old_mesh +
            "; }, { id = 2; blueprint = 2; name = \"Sun\"; settings = " + old_sun +
            "; }, { id = 6; blueprint = 7; name = \"Imported instance\"; settings = { position = [9,-10,11]; scale = 2.75; brightness = 4; visible = true; wireframe = true; }; }];\n";
    }
    text.replace(begin, end - begin, body);
    if (imported) text = replace_value(std::move(text), "selected", "6");
    auto loaded = project::decode(text);
    INFO((loaded ? "legacy scene migrated" : loaded.error().message));
    REQUIRE(loaded);
    CHECK(loaded->viewport.inspected_mesh == (imported ? static_cast<project::BlueprintId>(7)
                                             : project::BlueprintId::mesh));
    CHECK(*project::instance_transform(*loaded, 1) == project::InstanceTransform{{-3, 4, 5}, {}, 1.25F});
    CHECK(*project::instance_transform(*loaded, 2) == project::InstanceTransform{{6, -7, 8}, {}, 1});
    CHECK(*project::mesh_settings(*loaded, 1) == project::MeshSettings{.5F, true, true});
    CHECK(*project::sun_settings(*loaded, 2) == project::SunSettings{2, .5F, .3F, true, false});
    if (imported) {
        CHECK(*project::instance_transform(*loaded, 6) == project::InstanceTransform{{9, -10, 11}, {}, 2.75F});
        CHECK(project::mesh_settings(*loaded, 6)->brightness == 4.F);
        CHECK(loaded->document.mesh_assets.front().settings.brightness == 3.F);
        const auto spawned = project::instantiate(*loaded, static_cast<project::BlueprintId>(7));
        REQUIRE(spawned);
        CHECK(*project::instance_transform(*loaded, *spawned) == project::InstanceTransform{});
        CHECK(project::mesh_settings(*loaded, *spawned)->brightness == 3.F);
        CHECK(project::instance_mesh(*loaded, *spawned) == project::instance_mesh(*loaded, 6));
    }
    const auto modern = encoded(*loaded);
        CHECK(modern.find("editor_project = 5;") != std::string::npos);
    auto again = project::decode(modern);
    REQUIRE(again);
    CHECK(again->document.instances == loaded->document.instances);
    CHECK(encoded(*again) == modern);
}

TEST_CASE("Isolated blueprint geometry remains editable when its scene instance is hidden and keyed", "[editor][project][transform][preview]") {
    auto value = state();
    value.viewport.selected_object = 1;
    value.viewport.editor_camera = {0, 0, 8};
    value.document.mesh_blueprint = {1.25F, false, true};
    *project::instance_transform(value, 1) = {{40, 50, 60}, {90, 180, 270}, .5F};
    *project::mesh_settings(value, 1) = {3.F, false, false};
    REQUIRE(project::key_property(value, {1, "position"}, 4, Vec3{-50, -40, -30}));
    REQUIRE(project::key_property(value, {1, "rotation"}, 4, Vec3{180, 90, 0}));
    REQUIRE(project::key_property(value, {1, "scale"}, 4, 2.F));
    REQUIRE(project::key_property(value, {1, "brightness"}, 4, 4.F));
    REQUIRE(project::key_property(value, {1, "visible"}, 4, false));
    const auto original = encoded(value);
    CHECK_FALSE(project::project_vertex(value, 0, {640, 480}));
    value.viewport.mode = project::ViewMode::mesh;
    for (const auto time : {0.F, 2.F, 4.F, 8.F}) {
        const auto preview = project::preview_instance(value, *project::find_instance(value, 1), time);
        CHECK(preview.transform == project::InstanceTransform{});
        CHECK(std::get<project::MeshSettings>(preview.settings) == project::MeshSettings{1.25F, true, true});
    }
    const auto point = project::project_vertex(value, 0, {640, 480});
    REQUIRE(point);
    CHECK(point->x == .5F);
    CHECK(point->y == .5F);
    const auto points = project::project_vertices(value, {640, 480});
    REQUIRE(points[0]);
    CHECK(points[0] == point);
    CHECK(project::pick_vertex(value, {.5F, .5F}, {640, 480}) == 0);
    CHECK(project::mesh_transform(value) == Mat4::identity());
    value.viewport.mode = project::ViewMode::scene;
    CHECK(encoded(value) == original);
    CHECK(project::preview_instance(value, *project::find_instance(value, 1), 4) ==
          project::evaluate_instance(value, *project::find_instance(value, 1), 4));
    value.viewport.mode = project::ViewMode::sun;
    CHECK(project::preview_instance(value, *project::find_instance(value, 2), 4) ==
          project::evaluate_instance(value, *project::find_instance(value, 2), 4));
    CHECK(project::mesh_blueprint_settings(value, project::BlueprintId::mesh) == &value.document.mesh_blueprint);
    CHECK_FALSE(project::mesh_blueprint_settings(value, project::BlueprintId::sun));
    CHECK_FALSE(project::mesh_blueprint_settings(value, static_cast<project::BlueprintId>(99)));
}

TEST_CASE("Scene instance identities are not reused when creation is undone",
          "[editor][project][instances]") {
    project::EditingSession editing{state()}; editing.select_keyframe(editing.state().viewport.time);
    const auto& value = editing.state();
    const auto original = editing.instantiate(project::BlueprintId::mesh);
    REQUIRE(original);
    REQUIRE(editing.undo());
    CHECK_FALSE(project::find_instance(value, *original));
    const auto replacement = editing.instantiate(project::BlueprintId::sun);
    REQUIRE(replacement);
    CHECK(*replacement > *original);
    CHECK(project::sun_settings(value, *replacement));
    CHECK_FALSE(project::is_mesh_instance(value, *replacement));
}

TEST_CASE("Scene instances reject malformed identities and references",
          "[editor][project][instances]") {
    auto value = state();
    SECTION("Duplicate ID") { value.document.instances[1].id = 1; }
    SECTION("Zero ID") { value.document.instances[0].id = 0; }
    SECTION("Next ID would reuse a live ID") { value.document.next_instance_id = 2; }
    SECTION("Unknown blueprint") { value.document.instances[0].blueprint = static_cast<project::BlueprintId>(7); }
    SECTION("Mismatched settings") { value.document.instances[0].settings = project::SunSettings{}; }
    SECTION("Deleted selected ID") { value.document.instances.erase(value.document.instances.begin() + 1); }
    SECTION("Animation cannot reference an absent instance") {
        REQUIRE(value.document.timeline.set({77, "radius"}, {0, 1.F, timeline::Interpolation::linear}));
    }
    CHECK_FALSE(project::encode(value));
}

TEST_CASE("Mesh import adds independent retained blueprints without replacing the scene",
          "[editor][project][import]") {
    const auto previous = state();
    project::EditingSession editing{previous}; editing.select_keyframe(editing.state().viewport.time);
    const auto source = std::filesystem::path(VNG_TIMELINE_SCENE_PATH).parent_path() / "colored_cube.vmesh";
    auto original_mesh = editor::EditableMesh::load(source);
    REQUIRE(original_mesh);
    const auto created = editing.import_mesh(source);
    REQUIRE(created);
    REQUIRE(editing.undo());
    CHECK(editing.state().document.mesh_assets.empty());
    CHECK(editing.state().viewport.inspected_mesh == previous.viewport.inspected_mesh);
    CHECK(editing.state().document.instances == previous.document.instances);
    REQUIRE(editing.redo());
    auto value = editing.state();
    const auto blueprint = project::find_instance(value, *created)->blueprint;
    REQUIRE(blueprint != project::BlueprintId::mesh);
    CHECK(value.document.instances.size() == 3);
    CHECK(value.document.instances[0] == previous.document.instances[0]);
    CHECK(value.document.instances[1] == previous.document.instances[1]);
    CHECK(value.document.mesh.document() == previous.document.mesh.document());
    CHECK(value.document.revision > previous.document.revision);
    CHECK(value.document.timeline == previous.document.timeline);
    CHECK(value.viewport.selected_object == *created);
    CHECK(value.viewport.selected_vertex == 0);
    CHECK(value.viewport.inspected_mesh == blueprint);
    CHECK(project::editable_mesh(value)->document() == original_mesh->document());
    CHECK(project::blueprint_catalog(value).size() == 5);
    CHECK(project::is_mesh_instance(value, *created));
    REQUIRE(project::view_instance(value, project::BlueprintKind::mesh));
    CHECK(project::view_instance(value, project::BlueprintKind::mesh)->id == *created);
    auto duplicate = project::instantiate(value, blueprint);
    REQUIRE(duplicate);
    CHECK(project::instance_mesh(value, *created) == project::instance_mesh(value, *duplicate));
    REQUIRE(project::editable_mesh(value)->set_position(0, {3, 2, 1}));
    CHECK(project::instance_mesh(value, *created)->position(0) == Vec3{3, 2, 1});
    CHECK(value.document.mesh.document() == previous.document.mesh.document());
    REQUIRE(project::erase_instance(value, *created));
    REQUIRE(project::erase_instance(value, *duplicate));
    CHECK(project::mesh_geometry(value, blueprint));
    CHECK(project::blueprint_catalog(value).size() == 5);
    const auto serialized = encoded(value);
    const auto decoded = project::decode(serialized);
    REQUIRE(decoded);
    CHECK(project::mesh_geometry(*decoded, blueprint)->position(0) == Vec3{3, 2, 1});
    CHECK(decoded->document.instances == value.document.instances);
    CHECK(encoded(*decoded) == serialized);
    const auto next = project::import_mesh(value, source);
    REQUIRE(next);
    CHECK(*next > *duplicate);
    CHECK(static_cast<u32>(project::find_instance(value, *next)->blueprint) > static_cast<u32>(blueprint));
    const auto second_import = project::import_mesh(value, source);
    REQUIRE(second_import);
    CHECK(project::instance_mesh(value, *next) != project::instance_mesh(value, *second_import));
}

TEST_CASE("Mesh import errors preserve every authored value and identity counter",
          "[editor][project][import]") {
    auto value = state();
    const auto original = encoded(value);
    const auto directory = std::filesystem::path(VNG_TIMELINE_SCENE_PATH).parent_path();
    CHECK_FALSE(project::import_mesh(value, directory / "missing.vmesh"));
    CHECK_FALSE(project::import_mesh(value, directory / "editor_timeline.vscene"));
    CHECK_FALSE(project::import_mesh(value, directory));
    CHECK_FALSE(project::import_mesh(value, std::filesystem::path(std::string("a\0.vmesh", 8))));
    CHECK(encoded(value) == original);
    value.document.next_blueprint_id = std::numeric_limits<u32>::max();
    CHECK_FALSE(project::import_mesh(value, directory / "colored_cube.vmesh"));
    CHECK(value.document.mesh_assets.empty());
    CHECK(value.document.next_blueprint_id == std::numeric_limits<u32>::max());
}

TEST_CASE("Blueprint geometry targets remain explicit across scene selection and deletion",
          "[editor][project][blueprint]") {
    auto value = state();
    const auto imported_id = static_cast<project::BlueprintId>(3);
    auto imported = mesh();
    REQUIRE(imported.set_position(0, {2, 0, 0}));
    value.document.mesh_assets.push_back({imported_id, "Plane", std::move(imported), {2.F, false, true}});
    value.document.next_blueprint_id = 4;
    const auto instance = project::instantiate(value, imported_id);
    REQUIRE(instance);
    const auto instances = value.document.instances;
    REQUIRE(project::inspect_mesh(value, project::BlueprintId::mesh));
    CHECK(value.viewport.selected_object == *instance);
    CHECK(value.document.instances == instances);
    REQUIRE(project::mesh_target(value));
    CHECK(*project::mesh_target(value) == project::MeshTarget{project::BlueprintId::mesh, {}});
    CHECK(project::editable_mesh(value) == &value.document.mesh);
    CHECK_FALSE(project::view_instance(value, project::BlueprintKind::mesh));
    CHECK_FALSE(project::instance_in_view(value, *project::find_instance(value, *instance)));
    REQUIRE(project::editable_mesh(value)->set_position(0, {-.5F, 0, 0}));
    CHECK(project::mesh_geometry(value, imported_id)->position(0) == Vec3{2, 0, 0});

    REQUIRE(project::inspect_mesh(value, imported_id));
    CHECK(value.viewport.selected_object == *instance);
    CHECK(*project::mesh_target(value) == project::MeshTarget{imported_id, {}});
    CHECK(project::editable_mesh(std::as_const(value)) == project::mesh_geometry(value, imported_id));
    value.viewport.selected_object = 2; // Sun selection does not retarget an asset editor.
    CHECK(*project::mesh_target(value) == project::MeshTarget{imported_id, {}});
    value.viewport.selected_vertex = 3;
    REQUIRE(project::encode(value));
    REQUIRE(project::erase_instance(value, *instance));
    CHECK(value.viewport.inspected_mesh == imported_id);
    CHECK(project::editable_mesh(value) == project::mesh_geometry(value, imported_id));
    CHECK(value.viewport.selected_object == 2);
    const auto preview = project::preview_mesh(value, 5);
    REQUIRE(preview);
    CHECK(preview->settings == project::MeshSettings{2.F, true, true});
    CHECK(preview->transform == project::InstanceTransform{});
    const auto points = project::project_vertices(value, {800, 600});
    REQUIRE(points.size() == 4);
    REQUIRE(points[1]);
    CHECK(project::pick_vertex(value, {points[1]->x, points[1]->y}, {800, 600}) == 1);
    REQUIRE(project::encode(value));

    value.viewport.mode = project::ViewMode::scene;
    value.viewport.selected_vertex = 0;
    CHECK_FALSE(project::mesh_target(value));
    CHECK_FALSE(project::editable_mesh(value));
    CHECK_FALSE(project::view_instance(value, project::BlueprintKind::mesh));
    CHECK(project::project_vertices(value, {800, 600}).empty());
    CHECK_FALSE(project::project_vertex(value, 0, {800, 600}));
    value.viewport.selected_object = 0;
    CHECK_FALSE(project::mesh_target(value));
    value.viewport.selected_object = 1;
    CHECK(*project::mesh_target(value) == project::MeshTarget{project::BlueprintId::mesh, 1});
    CHECK(value.viewport.inspected_mesh == imported_id);
    value.viewport.mode = project::ViewMode::sun;
    CHECK_FALSE(project::mesh_target(value));
}

TEST_CASE("Explicit inspected blueprints persist and missing legacy targets migrate once",
          "[editor][project][blueprint][legacy]") {
    auto value = state();
    const auto imported_id = static_cast<project::BlueprintId>(3);
    value.document.mesh_assets.push_back({imported_id, "Imported", mesh(), {}});
    value.document.next_blueprint_id = 4;
    const auto instance = project::instantiate(value, imported_id);
    REQUIRE(instance);
    REQUIRE(project::inspect_mesh(value, imported_id));
    value.viewport.selected_object = 2; // Persist a target unrelated to scene selection.
    value.viewport.selected_vertex = 3;
    const auto full = encoded(value);
    const auto saved = project::encode_scene(value);
    REQUIRE(saved);
    for (const auto& text : {full, *saved}) {
        const auto restored = project::decode(text);
        REQUIRE(restored);
        CHECK(restored->viewport.inspected_mesh == imported_id);
        CHECK(restored->viewport.selected_object == 2);
        CHECK(restored->viewport.selected_vertex == 3);
        CHECK(project::mesh_target(*restored)->blueprint == imported_id);
    }
    value.viewport.selected_object = *instance;
    const auto legacy = omit_value(encoded(value), "inspected_mesh");
    auto migrated = project::decode(legacy);
    REQUIRE(migrated);
    CHECK(migrated->viewport.inspected_mesh == imported_id);
    migrated->viewport.selected_object = 1;
    const auto persisted = project::decode(encoded(*migrated));
    REQUIRE(persisted);
    CHECK(persisted->viewport.inspected_mesh == imported_id);
    auto old_sun = replace_value(legacy, "selected", "2");
    old_sun = replace_value(std::move(old_sun), "mode", "0");
    migrated = project::decode(old_sun);
    REQUIRE(migrated);
    CHECK(migrated->viewport.inspected_mesh == project::BlueprintId::mesh);
    CHECK(migrated->viewport.selected_vertex == 0);
    CHECK_FALSE(project::decode(replace_value(full, "inspected_mesh", "2")));
    CHECK_FALSE(project::decode(replace_value(full, "inspected_mesh", "99")));
    value.viewport.inspected_mesh = static_cast<project::BlueprintId>(99);
    CHECK_FALSE(project::mesh_target(value));
    CHECK_FALSE(project::encode(value));
}

TEST_CASE("Mesh inspection rejects invalid identities atomically and validates inactive vertex selection",
          "[editor][project][blueprint]") {
    auto value = state();
    const auto original = encoded(value);
    CHECK_FALSE(project::inspect_mesh(value, project::BlueprintId::sun));
    CHECK_FALSE(project::inspect_mesh(value, static_cast<project::BlueprintId>(999)));
    CHECK(encoded(value) == original);
    value.viewport.selected_vertex = 1;
    CHECK_FALSE(project::encode(value)); // Sun scene instance selected; no mesh target.
    value.viewport.mode = project::ViewMode::mesh;
    REQUIRE(project::encode(value));
    value.viewport.selected_vertex = static_cast<u32>(value.document.mesh.size());
    CHECK_FALSE(project::encode(value));
}

TEST_CASE("Long UTF-8 blueprint labels leave a valid bounded instance name",
          "[editor][project][import]") {
    auto value = state();
    const auto blueprint = static_cast<project::BlueprintId>(3);
    const auto name = std::string(252, 'a') + "éé";
    REQUIRE(name.size() == 256);
    value.document.mesh_assets.push_back({blueprint, name, mesh(), {}});
    value.document.next_blueprint_id = 4;
    value.document.next_instance_id = 33;
    const auto created = project::instantiate(value, blueprint);
    REQUIRE(created);
    const auto* instance = project::find_instance(value, *created);
    CHECK(instance->name == std::string(252, 'a') + " 33");
    REQUIRE(project::encode(value));
}

TEST_CASE("Scene serialization saves scene cameras but excludes the editor view",
          "[editor][project][camera]") {
    auto value = state();
    const auto camera = project::ensure_camera(value, {72, -32, 14, {1, -2, 3}});
    REQUIRE(camera);
    value.viewport.editor_camera = {-16, 9, 6, {2, 3, 4}};
    const auto saved = project::encode_scene(value);
    REQUIRE(saved);
    CHECK(saved->find("animation_camera") == std::string::npos);
    CHECK(saved->find("pilot_camera") == std::string::npos);
    value.viewport.editor_camera = {90, 15, 3, {}};
    const auto after_navigation = project::encode_scene(value);
    REQUIRE(after_navigation);
    CHECK(*after_navigation == *saved);
    auto loaded = project::decode(*saved);
    REQUIRE(loaded);
    CHECK(*project::find_instance(*loaded, *camera) == *project::find_instance(value, *camera));
    // Without a saved editor view, a scene opens looking through its camera.
    CHECK(loaded->viewport.editor_camera == *project::evaluate_camera(*loaded, 0));
    CHECK(loaded->document.mesh.document() == value.document.mesh.document());
    CHECK((*editor_example::mesh_settings(*loaded, 1)) == (*editor_example::mesh_settings(value, 1)));
    CHECK((*editor_example::sun_settings(*loaded, 2)) == (*editor_example::sun_settings(value, 2)));
}

namespace {
// A version 4 file: the camera was a document-level shot with its own tracks.
std::string legacy_scene(const project::State& value, std::string_view shot, std::string_view camera_tracks = {},
                         bool with_view = true) {
    auto scene = with_view ? project::encode(value) : project::encode_scene(value);
    REQUIRE(scene);
    auto text = replace_value(std::move(*scene), "editor_project", "4");
    if (!shot.empty()) text.insert(text.find("world_bounds = "), "animation_camera = { " + std::string(shot) + " };\n");
    const auto tracks = text.find("    tracks = [\n");
    REQUIRE(tracks != std::string::npos);
    text.insert(tracks + 15, camera_tracks);
    return text;
}
bool near(const project::CameraPose& a, const project::CameraPose& b) {
    const auto close = [](f32 x, f32 y) { return std::abs(x - y) < 1e-3F; };
    return close(a.yaw, b.yaw) && close(a.pitch, b.pitch) && close(a.distance, b.distance) && close(a.zoom, b.zoom) &&
        close(a.target.x, b.target.x) && close(a.target.y, b.target.y) && close(a.target.z, b.target.z);
}
std::vector<const project::SceneInstance*> cameras(const project::State& value) {
    std::vector<const project::SceneInstance*> result;
    for (const auto& instance : value.document.instances)
        if (std::holds_alternative<project::CameraSettings>(instance.settings)) result.push_back(&instance);
    return result;
}
} // namespace

namespace {
// The old camera's exact semantics, read straight from a pre-version-5 file:
// each component samples its own track and falls back to the base shot where
// it is untracked or before its first key.
struct LegacyReference {
    project::CameraPose base;
    vng::timeline::Timeline tracks;
    project::CameraPose at(f32 time) const {
        auto pose = base;
        const auto take = [&](auto& field, std::string_view property) {
            if (const auto value = tracks.sample({vng::u64{1} << 32, std::string(property)}, time))
                field = std::get<std::remove_cvref_t<decltype(field)>>(*value);
        };
        take(pose.yaw, "yaw"); take(pose.pitch, "pitch"); take(pose.distance, "distance");
        take(pose.target, "target"); take(pose.zoom, "zoom");
        return pose;
    }
    std::vector<f32> key_times() const {
        std::vector<f32> times;
        for (const auto& track : tracks.tracks()) for (const auto& key : track.keys) times.push_back(key.time);
        std::ranges::sort(times);
        return times;
    }
};
LegacyReference legacy_reference(std::string_view text) {
    auto document = vng::content::parse_document(text, {.limits = {.max_source_bytes = 256U << 20U,
        .max_decoded_bytes = 512U << 20U, .max_string_bytes = 256U << 20U}});
    REQUIRE(document);
    auto reference = document->read([](vng::content::Reader& r) {
        LegacyReference result;
        for (const auto member : r.members()) {
            if (member.name == "view" && member.value.get_or<f32>("distance", 0) > 0)
                result.base = {member.value.get<f32>("yaw"), member.value.get<f32>("pitch"), member.value.get<f32>("distance"),
                               member.value.get_or<Vec3>("camera_target", {}), member.value.get_or<f32>("zoom", 1)};
        }
        for (const auto member : r.members())
            if (member.name == "animation_camera")
                result.base = {member.value.get<f32>("yaw"), member.value.get<f32>("pitch"), member.value.get<f32>("distance"),
                               member.value.get_or<Vec3>("camera_target", {}), member.value.get_or<f32>("zoom", 1)};
        std::vector<vng::timeline::Track> tracks;
        for (const auto track : r.child("timeline").child("tracks").elements()) {
            if (track.get<u64>("object") != (vng::u64{1} << 32)) continue;
            const auto property = track.get<std::string>("property");
            vng::timeline::Track decoded{{vng::u64{1} << 32, property}, {}, {}, {}};
            for (const auto key : track.child("keys").elements()) {
                const auto incoming = key.get<std::string>("incoming") == "hold" ? vng::timeline::Interpolation::hold
                                                                                  : vng::timeline::Interpolation::linear;
                if (property == "target") decoded.keys.push_back({key.get<f32>("time"), key.get<Vec3>("value"), incoming});
                else decoded.keys.push_back({key.get<f32>("time"), key.get<f32>("value"), incoming});
            }
            tracks.push_back(std::move(decoded));
        }
        REQUIRE(result.tracks.replace(std::move(tracks)));
        return result;
    });
    REQUIRE(reference);
    return std::move(*reference);
}
// eye: eye error relative to the orbit distance; seen: the same scaled by the
// optical zoom, which is what shows on screen (5e-4 is about half a pixel).
struct Deviation { f32 eye{}, seen{}, target{}, yaw{}, pitch{}, distance{}, zoom{}; };
// Worst differences between the migrated camera and the old one over [start, end].
Deviation deviation(const project::State& loaded, const LegacyReference& reference, f32 start, f32 end, f32 step) {
    Deviation worst;
    const auto* camera = project::active_camera(loaded, start);
    REQUIRE(camera);
    for (f32 time = start; time <= end + step * .5F; time += step) {
        const auto expected = reference.at(time);
        auto probe = *camera;
        project::place_camera(probe, expected);
        const auto eye = project::evaluate_transform(loaded, *camera, time).position;
        const auto actual = *project::evaluate_camera(loaded, time);
        const auto length = [](Vec3 v) { return std::hypot(v.x, v.y, v.z); };
        const auto eye_error = length({eye.x - probe.transform.position.x, eye.y - probe.transform.position.y,
                                       eye.z - probe.transform.position.z}) / expected.distance;
        worst.eye = std::max(worst.eye, eye_error);
        worst.seen = std::max(worst.seen, eye_error * std::max(1.F, expected.zoom));
        worst.target = std::max(worst.target, length({actual.target.x - expected.target.x, actual.target.y - expected.target.y,
                                                      actual.target.z - expected.target.z}) / expected.distance);
        worst.yaw = std::max(worst.yaw, std::abs(std::remainder(actual.yaw - expected.yaw, 360.F)));
        worst.pitch = std::max(worst.pitch, std::abs(actual.pitch - expected.pitch));
        worst.distance = std::max(worst.distance, std::abs(actual.distance - expected.distance) / expected.distance);
        worst.zoom = std::max(worst.zoom, std::abs(actual.zoom - expected.zoom));
    }
    return worst;
}
std::string number(f32 value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(9) << value;
    return out.str();
}
// One legacy track in file syntax, with f32 or Vec3 values.
struct LegacyKey { f32 time; std::variant<f32, Vec3> value; bool hold{}; };
std::string legacy_track(std::string_view property, const std::vector<LegacyKey>& keys) {
    std::string text = "        { object = 4294967296; property = \"" + std::string(property) + "\"; keys = [\n";
    for (const auto& key : keys) {
        text += "            { time = " + number(key.time) + "; value = ";
        if (const auto* scalar = std::get_if<f32>(&key.value)) text += number(*scalar);
        else {
            const auto& v = std::get<Vec3>(key.value);
            text += "[" + number(v.x) + "," + number(v.y) + "," + number(v.z) + "]";
        }
        text += std::string("; incoming = \"") + (key.hold ? "hold" : "linear") + "\"; },\n";
    }
    return text + "        ]; },\n";
}
} // namespace

TEST_CASE("Legacy camera shots keep their orbit paths, cuts and per-track interpolation",
          "[editor][project][camera][legacy]") {
    auto value = state();
    value.document.timeline_duration = 60;
    const std::string shot = "yaw = 0; pitch = 0; distance = 10; zoom = 1; camera_target = [0,0,0];";
    const auto load = [&](const std::string& tracks) {
        const auto text = legacy_scene(value, shot, tracks);
        auto loaded = project::decode(text);
        INFO((loaded ? "legacy scene loaded" : loaded.error().message));
        REQUIRE(loaded);
        return std::pair{std::move(*loaded), legacy_reference(text)};
    };
    const auto keys_of = [](const project::State& loaded, std::string_view property) {
        const auto* camera = project::active_camera(loaded, 0);
        const auto* track = loaded.document.timeline.find({camera->id, std::string(property)});
        return track ? track->keys.size() : 0;
    };

    SECTION("A quarter orbit keeps its arc and keeps looking at its target") {
        const auto [loaded, reference] = load(legacy_track("yaw", {{0, 0.F, true}, {4, 90.F}}));
        const auto* camera = project::active_camera(loaded, 0);
        const auto eye = project::evaluate_transform(loaded, *camera, 2).position;
        CHECK(eye.x == Catch::Approx(7.0710678).margin(.006));
        CHECK(eye.z == Catch::Approx(7.0710678).margin(.006));
        const auto worst = deviation(loaded, reference, 0, 4, .01F);
        CHECK(worst.eye < 6e-4F); // Relative to the orbit distance: under a pixel.
        CHECK(worst.target < 6e-4F);
        CHECK(worst.yaw < 1e-3F);
    }
    SECTION("A held yaw cut stays a cut while zoom still ramps") {
        const auto [loaded, reference] = load(legacy_track("yaw", {{0, 0.F}, {4, 90.F, true}}) +
                                              legacy_track("zoom", {{0, 1.F}, {4, 3.F}}));
        CHECK(project::evaluate_camera(loaded, 3.99F)->yaw == Catch::Approx(0).margin(1e-3));
        CHECK(project::evaluate_camera(loaded, 4)->yaw == Catch::Approx(90));
        CHECK(project::evaluate_camera(loaded, 2)->zoom == Catch::Approx(2));
        const auto worst = deviation(loaded, reference, 0, 6, .01F);
        CHECK(worst.yaw < 1e-3F);
        CHECK(worst.zoom < 1e-5F);
        CHECK(worst.eye < 1e-5F);
    }
    SECTION("A held zoom key neither freezes rotation nor takes other times") {
        const auto [loaded, reference] = load(legacy_track("yaw", {{0, 0.F}, {4, 90.F}}) +
                                              legacy_track("zoom", {{0, 1.F}, {2.5F, 2.F, true}, {4, 3.F}}));
        CHECK(project::evaluate_camera(loaded, 1)->yaw == Catch::Approx(22.5));
        CHECK(project::evaluate_camera(loaded, 3)->yaw == Catch::Approx(67.5));
        CHECK(project::evaluate_camera(loaded, 2.49F)->zoom == Catch::Approx(1));
        CHECK(project::evaluate_camera(loaded, 2.5F)->zoom == Catch::Approx(2));
        CHECK(keys_of(loaded, "zoom") == 3);
        CHECK(keys_of(loaded, "rotation") == 2); // Only yaw's own times.
        const auto worst = deviation(loaded, reference, 0, 6, .01F);
        CHECK(worst.yaw < 1e-3F);
        CHECK(worst.zoom < 1e-5F);
    }
    SECTION("A cut in yaw while pitch still moves keeps both") {
        const auto [loaded, reference] = load(legacy_track("yaw", {{0, 0.F}, {4, 90.F, true}}) +
                                              legacy_track("pitch", {{0, 0.F}, {4, 40.F}}));
        CHECK(project::evaluate_camera(loaded, 2)->yaw == Catch::Approx(0).margin(1e-3));
        CHECK(project::evaluate_camera(loaded, 2)->pitch == Catch::Approx(20));
        CHECK(project::evaluate_camera(loaded, 3.9F)->pitch == Catch::Approx(39));
        CHECK(project::evaluate_camera(loaded, 4)->yaw == Catch::Approx(90));
        CHECK(project::evaluate_camera(loaded, 4)->pitch == Catch::Approx(40));
        // Only the millisecond before the cut differs: the moving component waits there.
        const auto before = deviation(loaded, reference, 0, 3.998F, .002F);
        CHECK(before.yaw < 1e-3F);
        CHECK(before.pitch < 1e-3F);
        CHECK(deviation(loaded, reference, 4, 8, .01F).pitch < 1e-3F);
    }
    SECTION("A component's first key is a cut from the base shot") {
        const auto [loaded, reference] = load(legacy_track("yaw", {{0, 10.F}, {4, 50.F}}) +
                                              legacy_track("pitch", {{2, 30.F}, {6, 10.F}}));
        CHECK(project::evaluate_camera(loaded, 1.99F)->pitch == Catch::Approx(0).margin(1e-3));
        CHECK(project::evaluate_camera(loaded, 2)->pitch == Catch::Approx(30));
        // Yaw keeps moving into pitch's cut, so it waits only in the millisecond before it.
        for (const auto worst : {deviation(loaded, reference, 0, 1.998F, .002F), deviation(loaded, reference, 2, 8, .01F)}) {
            CHECK(worst.eye < 6e-4F);
            CHECK(worst.pitch < 1e-3F);
            CHECK(worst.yaw < 1e-3F);
        }
    }
    SECTION("A turn that returns the eye to its start before another component's cut keeps its path") {
        // Far from the origin the eye is bit-identical at both ends of a full turn.
        const std::string far = "yaw = 0; pitch = 0; distance = 10; zoom = 1; camera_target = [100,0,0];";
        const auto check = [&](const std::string& tracks) {
            const auto text = legacy_scene(value, far, tracks);
            auto loaded = project::decode(text);
            REQUIRE(loaded);
            const auto reference = legacy_reference(text);
            const auto worst = deviation(*loaded, reference, 0, 3.998F, .002F);
            CHECK(worst.eye < 6e-4F);
            CHECK(worst.target < 6e-4F);
            CHECK(worst.yaw < 1e-3F);
        };
        check(legacy_track("yaw", {{0, -180.F, true}, {4, 180.F}}) + legacy_track("pitch", {{0, 0.F, true}, {4, 30.F, true}}));
        // An orbit plus a moving target can also end where it began.
        check(legacy_track("yaw", {{0, 90.F, true}, {4, -90.F}}) +
              legacy_track("target", {{0, Vec3{0, 0, 0}, true}, {4, Vec3{20, 0, 0}}}) +
              legacy_track("pitch", {{0, 0.F, true}, {4, 30.F, true}}));
    }
    SECTION("A zoom cut inside an orbit segment keeps the orbit sharp at the new zoom") {
        const std::string tilted = "yaw = 0; pitch = 45; distance = 10; zoom = 1; camera_target = [0,0,0];";
        const auto text = legacy_scene(value, tilted, legacy_track("yaw", {{0, 0.F, true}, {10, 90.F}}) +
                                                      legacy_track("zoom", {{0, 1.F, true}, {3.603F, 10.F, true}}));
        auto loaded = project::decode(text);
        REQUIRE(loaded);
        CHECK(deviation(*loaded, legacy_reference(text), 0, 10, .005F).seen < 6e-4F);
    }
    SECTION("A long dolly-in with a turn stays within tolerance at its closest") {
        const std::string zoomed = "yaw = 0; pitch = 30; distance = 50; zoom = 10; camera_target = [0,0,0];";
        const auto text = legacy_scene(value, zoomed, legacy_track("yaw", {{0, 0.F, true}, {10, 180.F}}) +
                                                      legacy_track("distance", {{0, 50.F, true}, {10, .1F}}));
        auto loaded = project::decode(text);
        REQUIRE(loaded);
        const auto reference = legacy_reference(text);
        CHECK(deviation(*loaded, reference, 0, 9.9F, .005F).seen < 6e-4F);
        CHECK(deviation(*loaded, reference, 9.9F, 10, .0002F).seen < 6e-4F); // Where the eye is closest.
    }
    SECTION("Unsorted old keys follow the same order the old loader gave them") {
        const auto [loaded, reference] = load(legacy_track("yaw", {{4, 90.F}, {0, 0.F}}));
        CHECK(project::evaluate_camera(loaded, 2)->yaw == Catch::Approx(45));
        CHECK(deviation(loaded, reference, 0, 6, .01F).eye < 6e-4F);
    }
    SECTION("Duplicate old key times are still rejected") {
        CHECK_FALSE(project::decode(legacy_scene(value, shot, legacy_track("yaw", {{2, 0.F}, {2, 10.F}}))));
    }
    SECTION("Wild shots load quickly within the key limits") {
        std::vector<LegacyKey> yaw;
        for (int i = 0; i < 4096; ++i) yaw.push_back({static_cast<f32>(i) * .01F, i % 2 ? 170.F : -170.F});
        value.document.timeline_duration = 50;
        const auto start = std::chrono::steady_clock::now();
        const auto [loaded, reference] = load(legacy_track("yaw", yaw));
        const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::cout << "4096 wild keys migrated in " << seconds << " s\n";
        // Bounded refinement takes about 0.25 s in a debug build; unbounded took over 5 s.
        CHECK(seconds < 2);
        const auto* camera = project::active_camera(loaded, 0);
        CHECK(loaded.document.timeline.find({camera->id, "position"})->keys.size() <= vng::timeline::max_keys_per_track);
    }
    SECTION("Full-size tracks at staggered times still load within the timeline limits") {
        std::vector<LegacyKey> yaw, pitch;
        for (int i = 0; i < 4096; ++i) {
            const auto t = static_cast<f32>(i) * .01F;
            yaw.push_back({t, 30 * std::sin(t)});
            pitch.push_back({t + .005F, 10 * std::cos(t + .005F)});
        }
        value.document.timeline_duration = 50;
        const auto [loaded, reference] = load(legacy_track("yaw", yaw) + legacy_track("pitch", pitch));
        std::size_t total{};
        for (const auto& track : loaded.document.timeline.tracks()) {
            CHECK(track.keys.size() <= vng::timeline::max_keys_per_track);
            total += track.keys.size();
        }
        CHECK(total <= vng::timeline::max_total_keys);
        const auto worst = deviation(loaded, reference, 0, 40.9F, .013F);
        CHECK(worst.yaw < .05F);
        CHECK(worst.pitch < .05F);
    }
}

TEST_CASE("Legacy files load the way they were saved, or are rejected like before",
          "[editor][project][camera][legacy]") {
    auto value = state();
    const std::string shot = "yaw = 72; pitch = -32; distance = 14; zoom = 2; camera_target = [1,-2,3];";
    SECTION("Scene files without a saved view open through the migrated camera") {
        auto loaded = project::decode(legacy_scene(value, shot, {}, false));
        REQUIRE(loaded);
        REQUIRE(project::has_camera(*loaded));
        CHECK(near(loaded->viewport.editor_camera, {72, -32, 14, {1, -2, 3}, 2}));
        CHECK(loaded->viewport.editor_camera == *project::evaluate_camera(*loaded, 0));
    }
    SECTION("Scene files that already had a camera open through it, not the unused shot") {
        auto with_camera = value;
        REQUIRE(project::ensure_camera(with_camera, {10, 5, 8, {1, 1, 1}}));
        auto loaded = project::decode(legacy_scene(with_camera, shot, {}, false));
        REQUIRE(loaded);
        CHECK(near(loaded->viewport.editor_camera, {10, 5, 8, {1, 1, 1}}));
    }
    SECTION("Invalid shots and keys are still rejected") {
        CHECK_FALSE(project::decode(legacy_scene(value, "yaw = 181; pitch = 0; distance = 8;")));
        CHECK_FALSE(project::decode(legacy_scene(value, "yaw = 0; pitch = 89.6; distance = 8;")));
        CHECK_FALSE(project::decode(legacy_scene(value, "yaw = 0; pitch = 0; distance = 0;")));
        CHECK_FALSE(project::decode(legacy_scene(value, shot, legacy_track("pitch", {{0, 0.F}, {2, 89.6F}}))));
        CHECK_FALSE(project::decode(legacy_scene(value, shot, legacy_track("distance", {{0, 8.F}, {2, 0.F}}))));
        CHECK(project::decode(legacy_scene(value, shot, legacy_track("pitch", {{0, 0.F}, {2, 89.F}}))));
    }
    SECTION("An eye beyond the coordinate range is clamped instead of failing the load") {
        auto loaded = project::decode(legacy_scene(value, "yaw = 90; pitch = 0; distance = 10; camera_target = [999999,0,0];"));
        INFO((loaded ? "loaded" : loaded.error().message));
        REQUIRE(loaded);
        const auto* camera = project::active_camera(*loaded, 0);
        REQUIRE(camera);
        CHECK(camera->transform.position.x == project::scene_coordinate_limit);
    }
    SECTION("A scene with no identity left for a camera still loads, without one") {
        auto text = replace_value(legacy_scene(value, shot), "next_instance_id", "4294967295");
        auto loaded = project::decode(text);
        INFO((loaded ? "loaded" : loaded.error().message));
        REQUIRE(loaded);
        CHECK_FALSE(project::has_camera(*loaded));
    }
}

TEST_CASE("A camera whose pivot lies beyond the editor camera's range still reopens",
          "[editor][project][camera]") {
    auto value = state();
    const auto camera = project::ensure_camera(value, {0, 0, 8, {}});
    REQUIRE(camera);
    auto* instance = project::find_instance(value, *camera);
    instance->transform = {{-600000, 0, 0}, {0, 90, 0}, 1};
    std::get<project::CameraSettings>(instance->settings).focus = 600000;
    const auto saved = project::encode_scene(value);
    REQUIRE(saved);
    auto loaded = project::decode(*saved);
    INFO((loaded ? "reopened" : loaded.error().message));
    REQUIRE(loaded);
    CHECK(project::valid_camera_pose(loaded->viewport.editor_camera));
    CHECK(*project::find_instance(*loaded, *camera) == *instance);
}

TEST_CASE("Every shipped editor scene loads with a scene camera", "[editor][project][camera][legacy]") {
    const auto assets = std::filesystem::path(__FILE__).parent_path() / "../../examples/assets";
    unsigned scenes{};
    for (const auto& entry : std::filesystem::directory_iterator(assets)) {
        if (entry.path().extension() != ".vscene") continue;
        std::ifstream file(entry.path(), std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(file), {}};
        if (text.find("editor_project = ") == std::string::npos) continue; // Spaceflight-demo scenes.
        INFO(entry.path().filename().string());
        auto loaded = project::decode(text);
        INFO((loaded ? "loaded" : loaded.error().message));
        REQUIRE(loaded);
        CHECK(project::has_camera(*loaded));
        CHECK(project::evaluate_camera(*loaded, 0));
        ++scenes;
    }
    CHECK(scenes >= 6);
}

TEST_CASE("The shipped fleet reveal keeps its authored camera path within a pixel",
          "[editor][project][camera][legacy]") {
    const auto path = std::filesystem::path(__FILE__).parent_path() / "../../examples/assets/fleet_reveal.vscene";
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file);
    const std::string text{std::istreambuf_iterator<char>(file), {}};
    auto loaded = project::decode(text);
    INFO((loaded ? "fleet reveal loaded" : loaded.error().message));
    REQUIRE(loaded);
    const auto reference = legacy_reference(text);
    const auto times = reference.key_times();
    REQUIRE_FALSE(times.empty());
    const auto worst = deviation(*loaded, reference, 0, times.back(), .01F);
    CHECK(worst.eye < 6e-4F);
    CHECK(worst.target < 6e-4F);
    CHECK(worst.yaw < 1e-3F);
    CHECK(worst.pitch < 1e-3F);
    CHECK(worst.distance < 1e-5F);
    CHECK(worst.zoom < 1e-5F);
}

TEST_CASE("Legacy scene shots load as an active camera instance with the same pose and keys",
          "[editor][project][camera][legacy]") {
    auto value = state();
    value.viewport.editor_camera = {-55, 7, 3, {2, -3, 4}};
    const project::CameraPose shot{72, -32, 14, {1, -2, 3}, 2};
    const std::string shot_text = "yaw = 72; pitch = -32; distance = 14; zoom = 2; camera_target = [1,-2,3];";
    const std::string yaw_track =
        "        { object = 4294967296; property = \"yaw\"; label = \"Orbit (deg)\"; layer = \"Camera\"; keys = [\n"
        "            { time = 0; value = 72; incoming = \"hold\"; },\n"
        "            { time = 4; value = 12; incoming = \"linear\"; },\n"
        "            { time = 6; value = -30; incoming = \"hold\"; },\n        ]; },\n";

    SECTION("A static shot") {
        auto loaded = project::decode(legacy_scene(value, shot_text));
        INFO((loaded ? "legacy scene loaded" : loaded.error().message));
        REQUIRE(loaded);
        const auto found = cameras(*loaded);
        REQUIRE(found.size() == 1);
        CHECK(found.front()->name == "Animation camera");
        CHECK(std::get<project::CameraSettings>(found.front()->settings).active);
        CHECK(near(*project::evaluate_camera(*loaded, 0), shot));
        CHECK(loaded->viewport.editor_camera == value.viewport.editor_camera); // A saved editor view is kept.
        CHECK(loaded->document.timeline.tracks().empty());
        // Saving writes the current format, and loading it again adds nothing.
        const auto modern = encoded(*loaded);
        CHECK(modern.find("editor_project = 5;") != std::string::npos);
        CHECK(modern.find("animation_camera") == std::string::npos);
        auto again = project::decode(modern);
        REQUIRE(again);
        CHECK(again->document.instances == loaded->document.instances);
    }
    SECTION("An animated shot keeps its keys, interpolation and cuts") {
        auto loaded = project::decode(legacy_scene(value, shot_text, yaw_track));
        INFO((loaded ? "legacy scene loaded" : loaded.error().message));
        REQUIRE(loaded);
        REQUIRE(cameras(*loaded).size() == 1);
        CHECK(near(*project::evaluate_camera(*loaded, 0), shot));
        CHECK(project::evaluate_camera(*loaded, 2)->yaw == Catch::Approx(42));
        CHECK(project::evaluate_camera(*loaded, 4)->yaw == Catch::Approx(12));
        CHECK(project::evaluate_camera(*loaded, 5.9F)->yaw == Catch::Approx(12)); // Held until the cut.
        CHECK(project::evaluate_camera(*loaded, 6)->yaw == Catch::Approx(-30));
        for (const auto time : {0.F, 4.F, 6.F}) {
            CHECK(project::evaluate_camera(*loaded, time)->distance == Catch::Approx(14));
            CHECK(project::evaluate_camera(*loaded, time)->zoom == Catch::Approx(2));
        }
        for (const auto& track : loaded->document.timeline.tracks())
            CHECK(track.target.object == cameras(*loaded).front()->id);
    }
    SECTION("The oldest files kept their only camera in the view") {
        auto loaded = project::decode(legacy_scene(value, {}));
        REQUIRE(loaded);
        REQUIRE(cameras(*loaded).size() == 1);
        CHECK(near(*project::evaluate_camera(*loaded, 0), value.viewport.editor_camera));
        CHECK(loaded->viewport.editor_camera == value.viewport.editor_camera);
    }
    SECTION("An invalid shot is dropped unread when the scene already has cameras") {
        auto with_camera = value;
        const auto camera = project::ensure_camera(with_camera, {10, 5, 8, {}});
        REQUIRE(camera);
        CHECK(project::decode(legacy_scene(with_camera, "yaw = 400; pitch = 0; distance = 8;")));
        // Even without a saved view, and with a camera the editor cannot look through.
        auto* far = project::find_instance(with_camera, *camera);
        far->transform = {{-600000, 0, 0}, {0, 90, 0}, 1};
        std::get<project::CameraSettings>(far->settings).focus = 600000;
        auto loaded = project::decode(legacy_scene(with_camera, "yaw = 400; pitch = 0; distance = 8;", {}, false));
        INFO((loaded ? "loaded" : loaded.error().message));
        REQUIRE(loaded);
        CHECK(project::valid_camera_pose(loaded->viewport.editor_camera));
    }
    SECTION("A scene at the total key limit still loads its shot") {
        auto full = value;
        full.document.timeline_duration = 5000;
        std::vector<vng::timeline::Track> tracks;
        std::size_t keys{};
        for (const auto* property : {"position", "rotation", "axis_scale", "scale"}) {
            vng::timeline::Track track{{1, property}, {}, {}, {}};
            const bool scalar = std::string_view(property) == "scale";
            for (int i = 0; i < 4096 && keys + 1 < vng::timeline::max_total_keys; ++i, ++keys)
                track.keys.push_back({static_cast<f32>(i), scalar ? vng::timeline::Value{1.F} : vng::timeline::Value{Vec3{1, 1, 1}},
                                      vng::timeline::Interpolation::linear});
            tracks.push_back(std::move(track));
        }
        REQUIRE(full.document.timeline.replace(std::move(tracks)));
        auto loaded = project::decode(legacy_scene(full, shot_text, legacy_track("yaw", {{3, 40.F}})));
        INFO((loaded ? "loaded" : loaded.error().message));
        REQUIRE(loaded);
        CHECK(project::has_camera(*loaded));
    }
    SECTION("A scene that already has cameras never used its shot") {
        auto with_camera = value;
        const auto existing = project::ensure_camera(with_camera, {10, 5, 8, {}});
        REQUIRE(existing);
        auto loaded = project::decode(legacy_scene(with_camera, shot_text, yaw_track));
        REQUIRE(loaded);
        const auto found = cameras(*loaded);
        REQUIRE(found.size() == 1);
        CHECK(found.front()->id == *existing);
        CHECK(loaded->document.timeline.tracks().empty());
    }
    SECTION("Current files cannot carry the old camera tracks") {
        auto current = replace_value(legacy_scene(value, {}, yaw_track), "editor_project", "5");
        CHECK_FALSE(project::decode(current));
    }
}

TEST_CASE("Undo and redo saved cameras retain the editor view",
          "[editor][project][camera][history]") {
    auto initial = state();
    const auto camera = project::ensure_camera(initial, {0, 0, 8, {}});
    REQUIRE(camera);
    project::EditingSession editing{std::move(initial)}; editing.select_keyframe(editing.state().viewport.time);
    const auto& value = editing.state();
    const auto original = *project::find_instance(value, *camera);
    REQUIRE(editing.set_camera(*camera, {45, -20, 4, {2, 0, 1}}));
    const auto edited = *project::find_instance(value, *camera);
    CHECK(edited != original);
    editing.viewport().editor_camera = {-60, 35, 10, {3, 4, 5}};
    const auto inspection = value.viewport.editor_camera;
    REQUIRE(editing.undo());
    CHECK(*project::find_instance(value, *camera) == original);
    CHECK(value.viewport.editor_camera == inspection);
    REQUIRE(editing.redo());
    CHECK(*project::find_instance(value, *camera) == edited);
    CHECK(value.viewport.editor_camera == inspection);
}

TEST_CASE("Undo and redo retain the current isolation view and editor camera",
          "[editor][project][camera][history]") {
    project::EditingSession editing{state()}; editing.select_keyframe(editing.state().viewport.time);
    const auto& value = editing.state();
    auto& view = editing.viewport();
    view.mode = project::ViewMode::mesh;
    const auto original_vertex = value.document.mesh.position(0);
    REQUIRE(editing.translate_vertices(project::BlueprintId::mesh, std::array<u32,1>{0}, {2, 3, 4}));

    // An edit made in isolated mesh view must not drag the user back there
    // when undo is invoked later from the scene view.
    view.mode = project::ViewMode::scene;
    view.editor_camera = {-60, 35, 10, {3, 4, 5}};
    const auto inspection = value.viewport.editor_camera;
    const auto scene_position = project::camera(value).position();
    REQUIRE(editing.undo());
    CHECK(value.document.mesh.position(0) == original_vertex);
    CHECK(value.viewport.mode == project::ViewMode::scene);
    CHECK(value.viewport.editor_camera == inspection);
    CHECK(project::camera(value).position() == scene_position);

    // Redo is equally independent of which editor-only view is now active.
    view.mode = project::ViewMode::sun;
    const auto inspection_position = project::camera(value).position();
    REQUIRE(editing.redo());
    CHECK(project::mesh_edit_geometry(value, project::BlueprintId::mesh)->position(0) == Vec3{2, 3, 4});
    CHECK(value.document.mesh.position(0) == original_vertex);
    CHECK(value.viewport.mode == project::ViewMode::sun);
    CHECK(value.viewport.editor_camera == inspection);
    CHECK(project::camera(value).position() == inspection_position);
}

TEST_CASE("Editor project decoding rejects missing wrong-type out-of-range and oversized values",
          "[editor][project]") {
    const auto source = encoded(state());
    CHECK_FALSE(project::decode("not a scene"));
    CHECK_FALSE(project::decode(replace_value(source, "editor_project", "6")));
    CHECK_FALSE(project::decode(replace_value(source, "revision", "0")));
    CHECK_FALSE(project::decode(replace_value(source, "revision", "-1")));
    CHECK_FALSE(project::decode(replace_value(source, "mode", "3")));
    CHECK_FALSE(project::decode(replace_value(source, "selected", "3")));
    CHECK_FALSE(project::decode(replace_value(source, "vertex", "4")));
    CHECK_FALSE(project::decode(replace_value(source, "scale", "0")));
    CHECK_FALSE(project::decode(replace_value(source, "scale", "1000001")));
    CHECK_FALSE(project::decode(replace_value(source, "brightness", "11")));
    CHECK_FALSE(project::decode(replace_value(source, "brightness", "5.1")));
    CHECK_FALSE(project::decode(replace_value(source, "radius", "-1")));
    CHECK_FALSE(project::decode(replace_value(source, "displacement", "3")));
    CHECK_FALSE(project::decode(replace_value(source, "displacement", "1.01")));
    CHECK_FALSE(project::decode(replace_value(source, "bloom", "2")));
    CHECK_FALSE(project::decode(replace_value(source, "time", "86401")));
    CHECK_FALSE(project::decode(replace_value(source, "distance", "0.0001")));
    CHECK_FALSE(project::decode(replace_value(source, "distance", "1000001")));
    CHECK_FALSE(project::decode(replace_value(source, "pitch", "89.6")));
    CHECK_FALSE(project::decode(replace_value(source, "yaw", "3601")));
    CHECK_FALSE(project::decode(replace_value(source, "yaw", "181")));
    CHECK_FALSE(project::decode(replace_value(source, "position", "[1000001, 0, 0]")));
    CHECK_FALSE(project::decode(replace_value(source, "rotation", "[361, 0, 0]")));
    CHECK_FALSE(project::decode(replace_value(source, "rotation", "[0, -361, 0]")));
    CHECK_FALSE(project::decode(replace_value(source, "paused", "\"not a boolean\"")));
    CHECK_FALSE(project::decode(replace_value(source, "brightness", "1e100")));
    CHECK_FALSE(project::decode(replace_value(source, "brightness", "nan")));
    CHECK_FALSE(project::decode(replace_value(source, "mesh_data", "\"not a mesh\"")));
    auto missing = source;
    const auto at = missing.find("brightness = ");
    REQUIRE(at != std::string::npos);
    missing.replace(at, std::string_view("brightness").size(), "unrelated");
    CHECK_FALSE(project::decode(missing));
    CHECK_FALSE(project::decode(std::string(32 * 1024 * 1024 + 1, ' ')));
    CHECK_FALSE(project::decode(replace_value(source, "revision", "18446744073709551615")));
    CHECK_FALSE(project::decode(replace_value(source, "revision", "9007199254740992")));
    auto too_many_vertices = source;
    const auto vertices = too_many_vertices.find("vertices 4 {");
    REQUIRE(vertices != std::string::npos);
    too_many_vertices.replace(vertices, std::string_view("vertices 4 {").size(), "vertices 65537 {");
    auto rejected_vertices = project::decode(too_many_vertices);
    REQUIRE_FALSE(rejected_vertices);
    CHECK(rejected_vertices.error().code == content::ErrorCode::limit_exceeded);
}

TEST_CASE("Editor encoding validates authored state instead of emitting unreadable scenes",
          "[editor][project]") {
    auto value = state();
    const std::array<f32*, 13> fields{&project::instance_transform(value, 1)->scale, &(*editor_example::mesh_settings(value, 1)).brightness,
                                      &(*editor_example::sun_settings(value, 2)).radius,  &(*editor_example::sun_settings(value, 2)).displacement,
                                      &(*editor_example::sun_settings(value, 2)).bloom,   &value.viewport.time,
                                      &value.viewport.editor_camera.distance,    &value.viewport.editor_camera.yaw,
                                      &value.viewport.editor_camera.pitch,       &project::instance_transform(value, 1)->position.x,
                                      &project::instance_transform(value, 1)->rotation.x,
                                      &project::instance_transform(value, 1)->rotation.y,
                                      &project::instance_transform(value, 1)->rotation.z};
    for (auto* field : fields) {
        const auto original = *field;
        *field = std::numeric_limits<f32>::quiet_NaN();
        CHECK_FALSE(project::encode(value));
        *field = std::numeric_limits<f32>::infinity();
        CHECK_FALSE(project::encode(value));
        *field = original;
    }
    project::instance_transform(value, 1)->scale = project::max_instance_scale+1.F;
    CHECK_FALSE(project::encode(value));
    project::instance_transform(value, 1)->scale = 3;
    (*editor_example::mesh_settings(value, 1)).brightness = 5.01F;
    CHECK_FALSE(project::encode(value));
    (*editor_example::mesh_settings(value, 1)).brightness = 5;
    (*editor_example::sun_settings(value, 2)).radius = 4;
    (*editor_example::sun_settings(value, 2)).displacement = 1;
    (*editor_example::sun_settings(value, 2)).bloom = 1;
    REQUIRE(project::decode(encoded(value)));
    value.viewport.mode = static_cast<project::ViewMode>(-1);
    CHECK_FALSE(project::encode(value));
    value.viewport.mode = project::ViewMode::scene;
    value.viewport.selected_vertex = static_cast<u32>(value.document.mesh.size());
    CHECK_FALSE(project::encode(value));
    value.viewport.selected_vertex = 0;
    value.document.revision = std::numeric_limits<u64>::max();
    CHECK_FALSE(project::encode(value));
}

TEST_CASE("Mesh picking uses top-left coordinates and excludes invisible invalid and behind-camera "
          "vertices",
          "[editor][project]") {
    auto value = state();
    value.viewport.mode = project::ViewMode::mesh;
    value.viewport.editor_camera.yaw = 0;
    value.viewport.editor_camera.pitch = 0;
    constexpr Extent2D extent{640, 480};
    const auto center = project::project_vertex(value, 0, extent);
    REQUIRE(center);
    CHECK(center->x == .5F);
    CHECK(center->y == .5F);
    const auto right = project::project_vertex(value, 1, extent);
    const auto up = project::project_vertex(value, 2, extent);
    REQUIRE(right);
    REQUIRE(up);
    CHECK(right->x > center->x);
    CHECK(up->y < center->y);
    CHECK(project::pick_vertex(value, {.5F, .5F}, extent) == 0);
    CHECK_FALSE(project::project_vertex(value, 999, extent));
    CHECK_FALSE(project::project_vertex(value, 0, {0, 480}));
    CHECK_FALSE(project::pick_vertex(value, {-.1F, .5F}, extent));
    CHECK_FALSE(project::pick_vertex(value, {.5F, .5F}, extent, -1));
    CHECK_FALSE(
        project::pick_vertex(value, {.5F, .5F}, extent, std::numeric_limits<f32>::infinity()));
    (*editor_example::mesh_settings(value, 1)).visible = false;
    value.viewport.selected_object = 1;
    value.viewport.mode = project::ViewMode::scene;
    CHECK_FALSE(project::project_vertex(value, 0, extent));
    CHECK_FALSE(project::pick_vertex(value, {.5F, .5F}, extent));
    (*editor_example::mesh_settings(value, 1)).visible = true;
    value.viewport.mode = project::ViewMode::sun;
    CHECK_FALSE(project::project_vertex(value, 0, extent));
    CHECK_FALSE(project::pick_vertex(value, {.5F, .5F}, extent));
    value.viewport.mode = project::ViewMode::scene;
    project::instance_transform(value, 1)->position = {100, 0, 0};
    CHECK_FALSE(project::project_vertex(value, 0, extent));
    project::instance_transform(value, 1)->position = {0, 0, 100};
    CHECK_FALSE(project::project_vertex(value, 0, extent));
}

TEST_CASE("Editor undo redo restores complete snapshots with monotonically increasing revisions",
          "[editor][history]") {
    project::EditingSession editing{state()}; editing.select_keyframe(editing.state().viewport.time);
    const auto& value = editing.state();
    const auto original = encoded(value);
    CHECK_FALSE(*editing.undo());
    CHECK_FALSE(*editing.redo());
    REQUIRE(editing.erase_instances(std::array<u32,1>{2}));
    const auto edited = encoded(value);
    REQUIRE(editing.undo());
    CHECK(value.document.revision == 3);
    CHECK(encoded(value) == replace_value(replace_value(original, "revision", "3"), "sequence", std::to_string(value.viewport.sequence)));
    CHECK(editing.can_redo());
    CHECK_FALSE(editing.can_undo());
    REQUIRE(editing.redo());
    CHECK(value.document.revision == 4);
    CHECK(encoded(value) == replace_value(replace_value(edited, "revision", "4"), "sequence", std::to_string(value.viewport.sequence)));
    CHECK_FALSE(editing.can_redo());
    REQUIRE(editing.undo());
    CHECK(value.document.revision == 5);
    REQUIRE(editing.set_transform(1, {}, 2));
    CHECK_FALSE(editing.can_redo());
    CHECK_FALSE(*editing.redo());
    REQUIRE(editing.undo());
    CHECK(value.document.revision == 7);
    CHECK(project::instance_transform(value, 1)->scale == .7F);
}

TEST_CASE("Editor undo history has a bounded number of complete authored snapshots",
          "[editor][history]") {
    project::EditingSession editing{state()}; editing.select_keyframe(editing.state().viewport.time);
    const auto& value = editing.state();
    for (int i = 0; i < 80; ++i) {
        REQUIRE(editing.instantiate(project::BlueprintId::mesh));
    }
    auto revision = value.document.revision;
    int count{};
    while (editing.can_undo()) {
        REQUIRE(*editing.undo());
        CHECK(value.document.revision == ++revision);
        ++count;
    }
    CHECK(count == 64);
    CHECK(value.document.instances.size() == 18); // Two initial + 16 retained creations.
    count = 0;
    while (editing.can_redo()) {
        REQUIRE(*editing.redo());
        CHECK(value.document.revision == ++revision);
        ++count;
    }
    CHECK(count == 64);
    CHECK(value.document.instances.size() == 82);
}

TEST_CASE("Editor history never wraps exhausted revisions or consumes an unavailable undo",
          "[editor][history]") {
    auto initial = state();
    initial.document.revision = (u64{1} << 53) - 3;
    project::EditingSession editing{std::move(initial)}; editing.select_keyframe(editing.state().viewport.time);
    REQUIRE(editing.set_transform(1, {}, 2));
    const auto revision = editing.state().document.revision;
    CHECK_FALSE(editing.undo());
    CHECK(project::instance_transform(editing.state(), 1)->scale == 2.F);
    CHECK(editing.state().document.revision == revision);
    CHECK(editing.can_undo()); // Failed undo did not consume its entry.
    CHECK_FALSE(editing.can_redo());
    auto invalid = state();
    invalid.document.revision = std::numeric_limits<u64>::max();
    project::EditingSession exhausted{std::move(invalid)}; exhausted.select_keyframe(exhausted.state().viewport.time);
    CHECK_FALSE(exhausted.instantiate(project::BlueprintId::mesh));
    CHECK(exhausted.state().document.revision == std::numeric_limits<u64>::max());
}

TEST_CASE("Editor saves and reloads new scene and mesh files without overwriting authored data",
          "[editor][project]") {
    std::array<char, 40> name{};
    std::string_view pattern = "/tmp/vng-editor-files-XXXXXX";
    std::copy(pattern.begin(), pattern.end(), name.begin());
    REQUIRE(::mkdtemp(name.data()) != nullptr);
    const std::filesystem::path directory{name.data()};
    struct Cleanup {
        std::filesystem::path directory;
        ~Cleanup() {
            std::error_code ec;
            for (const auto* filename : {"scene.vscene", "mesh.vmesh", "invalid.vscene"})
                std::filesystem::remove(directory / filename, ec);
            std::filesystem::remove(directory, ec);
        }
    } cleanup{directory};
    auto value = state();
    REQUIRE(value.document.mesh.translate(std::array<u32, 2>{0, 3}, {.25F, .5F, .75F}));
    *project::instance_transform(value, 1) = {{3, 4, 5}, {-25, 65, 110}, 1.75F};
    *project::instance_transform(value, 2) = {{-6, 2, -1}, {15, -60, 30}, .5F};
    const auto serialized = encoded(value);
    const auto scene_path = directory / "scene.vscene";
    REQUIRE(project::save_new(scene_path, serialized));
    CHECK_FALSE(project::save_new(scene_path, "must not overwrite"));
    auto restored = project::load_scene(scene_path);
    REQUIRE(restored);
    CHECK(restored->document.instances == value.document.instances);
    CHECK(encoded(*restored) == serialized);
    auto mesh_text = vm::write_vmesh(value.document.mesh.document());
    REQUIRE(mesh_text);
    REQUIRE(project::save_new(directory / "mesh.vmesh", *mesh_text));
    auto restored_mesh = editor::EditableMesh::load(directory / "mesh.vmesh");
    REQUIRE(restored_mesh);
    CHECK(restored_mesh->document() == value.document.mesh.document());
    REQUIRE(project::save_new(directory / "invalid.vscene", "invalid scene"));
    CHECK_FALSE(project::load_scene(directory / "invalid.vscene"));
    CHECK_FALSE(project::load_scene(directory / "missing.vscene"));
    CHECK_FALSE(editor::EditableMesh::load(directory / "missing.vmesh"));
}
TEST_CASE("Mesh drafts publish explicitly and survive snapshots saves and undo", "[editor][draft]") {
    using namespace project;
    auto value=state();value.viewport.mode=ViewMode::mesh;value.viewport.selected_object=1;
    const auto original=value.document.mesh.document();
    REQUIRE(begin_mesh_draft(value,BlueprintId::mesh)==true);
    REQUIRE(begin_mesh_draft(value,BlueprintId::mesh)==false);
    REQUIRE(editable_mesh(value)->set_position(0,{3,2,1}));
    ++value.document.revision;
    const auto edited=editable_mesh(value)->document();
    CHECK(mesh_geometry(value,BlueprintId::mesh)->document()==original);
    CHECK(instance_mesh(value,1)->document()==original);
    CHECK(mesh_view_geometry(value,BlueprintId::mesh)->document()==edited);
    value.viewport.mode=ViewMode::scene;
    CHECK(mesh_view_geometry(value,BlueprintId::mesh)->document()==original);
    value.viewport.mode=ViewMode::mesh;
    CHECK(editable_mesh(value)->document()==edited);
    for(const auto bytes:{encode(value),encode_scene(value)}) {
        REQUIRE(bytes);auto loaded=decode(*bytes);REQUIRE(loaded);
        CHECK(mesh_geometry(*loaded,BlueprintId::mesh)->document()==original);
        CHECK(mesh_edit_geometry(*loaded,BlueprintId::mesh)->document()==edited);
    }
    // Small vertex patches target the draft, never the scene's published mesh.
    const auto base=value.document.revision++;
    REQUIRE(editable_mesh(value)->set_position(1,{4,2,1}));
    DocumentChanges changes;changes.vertices[1].insert(1);
    auto patch=capture_patch(base,value,changes);REQUIRE(patch);
    auto peer=value;peer.document.revision=base;
    REQUIRE(mesh_edit_geometry(peer,BlueprintId::mesh)->set_position(1,{1,0,0}));
    peer.viewport.mode=ViewMode::scene;
    REQUIRE(apply_patch(peer,*patch));
    CHECK(mesh_edit_geometry(peer,BlueprintId::mesh)->position(1)==Vec3{4,2,1});
    CHECK(instance_mesh(peer,1)->document()==original);
    EditingSession editing{std::move(value)}; editing.select_keyframe(editing.state().viewport.time);
    const auto& current = editing.state();
    REQUIRE(*editing.apply_mesh(BlueprintId::mesh));
    CHECK(current.document.mesh_drafts.empty());CHECK(instance_mesh(current,1)->position(0)==Vec3{3,2,1});
    REQUIRE(editing.undo());CHECK(instance_mesh(current,1)->document()==original);
    CHECK(editable_mesh(current)->position(0)==Vec3{3,2,1});
    REQUIRE(editing.redo());CHECK(current.document.mesh_drafts.empty());
    auto standalone = current;
    REQUIRE(begin_mesh_draft(standalone,BlueprintId::mesh));
    REQUIRE(discard_mesh_draft(standalone,BlueprintId::mesh));
    CHECK_FALSE(begin_mesh_draft(standalone,static_cast<BlueprintId>(1000)));
    CHECK_FALSE(apply_mesh_draft(standalone,static_cast<BlueprintId>(1000)));
}
