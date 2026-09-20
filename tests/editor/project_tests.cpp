#include <vng/editor/mesh.hpp>
#include "../../examples/editor/project.hpp"
#include "../../examples/editor/editing_session.hpp"
#include "../../examples/editor/animation.hpp"
#include "../../examples/editor/preview_values.hpp"
#include "../../examples/editor/document_patch.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <array>
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
    REQUIRE(project::key_camera(initial,0,{0,0,8,{}}));
    REQUIRE(project::key_camera(initial,5,{30,10,6,{1,2,3}}));
    REQUIRE(project::key_property(initial,{1,"position"},4,Vec3{3,4,5}));
    initial.viewport.time=5;
    project::EditingSession editing{std::move(initial)}; editing.select_keyframe(editing.state().viewport.time);
    const auto& value=editing.state();
    const auto before_camera=project::evaluate_camera(value,5);
    const auto mesh_address=std::get<std::vector<f32>>(value.document.mesh.document().vertex_fields[2].values).data();
    const auto other_track=*value.document.timeline.find({1,"position"});
    REQUIRE(editing.begin_camera());
    REQUIRE(editing.camera({60,20,9,{3,4,5}}));
    REQUIRE(editing.commit());
    const auto changed_camera=project::evaluate_camera(value,5);
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
    REQUIRE(editing.begin_camera());
    REQUIRE(editing.camera(project::evaluate_camera(value,5)));
    REQUIRE(editing.commit());
    REQUIRE(editing.undo()); // No-op camera gesture did not create an entry.
    CHECK_FALSE(project::find_instance(value,*created));
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
    original.document.animation_camera = {70, -15, 12, {1, -2, 3}};
    original.viewport.pilot_camera = true;
    REQUIRE(original.document.mesh.set_position(0, {-.5F, .25F, 1.125F}));
    const auto text = encoded(original);
    auto round_trip = project::decode(text);
    INFO((round_trip ? "scene decoded" : round_trip.error().message));
    REQUIRE(round_trip);
    CHECK(round_trip->document.mesh.document() == original.document.mesh.document());
    CHECK((*editor_example::mesh_settings(*round_trip, 1)) == (*editor_example::mesh_settings(original, 1)));
    CHECK((*editor_example::sun_settings(*round_trip, 2)) == (*editor_example::sun_settings(original, 2)));
    CHECK(round_trip->document.instances == original.document.instances);
    CHECK(text.find("editor_project = 3;") != std::string::npos);
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
    CHECK(round_trip->document.animation_camera == original.document.animation_camera);
    CHECK(round_trip->viewport.pilot_camera == original.viewport.pilot_camera);
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
    CHECK(project::blueprint_catalog(value).size() == 3);
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
    CHECK(project::animation_properties(*persisted).size() == 19);
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
    const auto end = text.find("animation_camera = ");
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
        CHECK(modern.find("editor_project = 3;") != std::string::npos);
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
    CHECK(project::blueprint_catalog(value).size() == 4);
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
    CHECK(project::blueprint_catalog(value).size() == 4);
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

TEST_CASE("Scene serialization saves the animation camera but excludes inspection navigation",
          "[editor][project][camera]") {
    auto value = state();
    value.document.animation_camera = {72, -32, 14, {1, -2, 3}};
    value.viewport.editor_camera = {-16, 9, 6, {2, 3, 4}};
    value.viewport.pilot_camera = true;
    const auto saved = project::encode_scene(value);
    REQUIRE(saved);
    CHECK(saved->find("animation_camera =") != std::string::npos);
    CHECK(saved->find("pilot_camera") == std::string::npos);
    value.viewport.editor_camera = {90, 15, 3, {}};
    value.viewport.pilot_camera = false;
    const auto after_navigation = project::encode_scene(value);
    REQUIRE(after_navigation);
    CHECK(*after_navigation == *saved);
    auto loaded = project::decode(*saved);
    REQUIRE(loaded);
    CHECK(loaded->document.animation_camera == value.document.animation_camera);
    CHECK(loaded->viewport.editor_camera == value.document.animation_camera);
    CHECK_FALSE(loaded->viewport.pilot_camera);
    CHECK(loaded->document.mesh.document() == value.document.mesh.document());
    CHECK((*editor_example::mesh_settings(*loaded, 1)) == (*editor_example::mesh_settings(value, 1)));
    CHECK((*editor_example::sun_settings(*loaded, 2)) == (*editor_example::sun_settings(value, 2)));
}

TEST_CASE("Legacy view camera is upgraded into both camera roles without changing its pose",
          "[editor][project][camera]") {
    auto value = state();
    value.viewport.editor_camera = {-55, 7, 3, {2, -3, 4}};
    auto legacy = encoded(value);
    const auto start = legacy.find("animation_camera = ");
    REQUIRE(start != std::string::npos);
    legacy.erase(start, legacy.find('\n', start) + 1 - start);
    auto restored = project::decode(legacy);
    REQUIRE(restored);
    CHECK(restored->viewport.editor_camera == value.viewport.editor_camera);
    CHECK(restored->document.animation_camera == value.viewport.editor_camera);
    CHECK_FALSE(restored->viewport.pilot_camera);
}

TEST_CASE("Undo and redo authored camera edits retain inspection camera and pilot mode",
          "[editor][project][camera][history]") {
    project::EditingSession editing{state()}; editing.select_keyframe(editing.state().viewport.time);
    const auto& value = editing.state();
    const auto original_animation = value.document.animation_camera;
    REQUIRE(editing.begin_camera());
    REQUIRE(editing.camera({45, -20, 4, {2, 0, 1}}));
    REQUIRE(editing.commit());
    const auto edited_animation = value.document.animation_camera;
    editing.viewport().editor_camera = {-60, 35, 10, {3, 4, 5}};
    const auto inspection = value.viewport.editor_camera;
    editing.viewport().pilot_camera = true;
    REQUIRE(editing.undo());
    CHECK(value.document.animation_camera == original_animation);
    CHECK(value.viewport.editor_camera == inspection);
    CHECK(value.viewport.pilot_camera);
    editing.viewport().pilot_camera = false;
    REQUIRE(editing.redo());
    CHECK(value.document.animation_camera == edited_animation);
    CHECK(value.viewport.editor_camera == inspection);
    CHECK_FALSE(value.viewport.pilot_camera);
}

TEST_CASE("Undo and redo retain current isolation view and animation camera routing",
          "[editor][project][camera][history]") {
    project::EditingSession editing{state()}; editing.select_keyframe(editing.state().viewport.time);
    const auto& value = editing.state();
    auto& view = editing.viewport();
    view.mode = project::ViewMode::mesh;
    const auto original_vertex = value.document.mesh.position(0);
    REQUIRE(editing.translate_vertices(project::BlueprintId::mesh, std::array<u32,1>{0}, {2, 3, 4}));

    // An edit made in isolated mesh view must not drag the user back there
    // when undo is invoked later while piloting the scene's animation camera.
    view.mode = project::ViewMode::scene;
    view.pilot_camera = true;
    view.editor_camera = {-60, 35, 10, {3, 4, 5}};
    const auto inspection = value.viewport.editor_camera;
    const auto shot_position = project::camera(value).position();
    REQUIRE(editing.undo());
    CHECK(value.document.mesh.position(0) == original_vertex);
    CHECK(value.viewport.mode == project::ViewMode::scene);
    CHECK(value.viewport.pilot_camera);
    CHECK(value.viewport.editor_camera == inspection);
    CHECK(&project::view_camera(value) == &value.document.animation_camera);
    CHECK(project::camera(value).position() == shot_position);

    // Redo is equally independent of which editor-only view is now active.
    view.mode = project::ViewMode::sun;
    view.pilot_camera = false;
    const auto inspection_position = project::camera(value).position();
    REQUIRE(editing.redo());
    CHECK(project::mesh_edit_geometry(value, project::BlueprintId::mesh)->position(0) == Vec3{2, 3, 4});
    CHECK(value.document.mesh.position(0) == original_vertex);
    CHECK(value.viewport.mode == project::ViewMode::sun);
    CHECK_FALSE(value.viewport.pilot_camera);
    CHECK(value.viewport.editor_camera == inspection);
    CHECK(&project::view_camera(value) == &value.viewport.editor_camera);
    CHECK(project::camera(value).position() == inspection_position);
}

TEST_CASE("Editor project decoding rejects missing wrong-type out-of-range and oversized values",
          "[editor][project]") {
    const auto source = encoded(state());
    CHECK_FALSE(project::decode("not a scene"));
    CHECK_FALSE(project::decode(replace_value(source, "editor_project", "4")));
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
