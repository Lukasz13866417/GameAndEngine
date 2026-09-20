#include "../../examples/editor/editing_session.hpp"
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
using namespace vng;
using namespace editor_example;
State region_scene() {
    content::vmesh::Document document;
    document.vertex_count = 3;
    document.vertex_fields = {{"position", {content::vmesh::ScalarType::Float32, 3},
        std::vector<f32>{0,0,0, 1,0,0, 0,1,0}}};
    document.faces = {{0,1,2}};
    auto mesh = editor::EditableMesh::create(std::move(document)); REQUIRE(mesh);
    State state{.document = {.mesh = std::move(*mesh)}};
    REQUIRE(instantiate(state, BlueprintId::region));
    REQUIRE(instantiate(state, BlueprintId::region));
    return state;
}
void exact_notice(EditingSession& session, u32 id, const std::set<u32>& points) {
    auto notice = session.take_changes(); REQUIRE(notice);
    CHECK_FALSE(notice->changes.full);
    REQUIRE(notice->changes.regions.size() == 1);
    const auto& scope = notice->changes.regions.at(id);
    CHECK_FALSE(scope.whole); CHECK(scope.points == points);
    const auto patch = capture_patch(1, session.state(), notice->changes); REQUIRE(patch);
    REQUIRE(patch->regions.size() == 1);
    CHECK_FALSE(patch->regions.front().replacement);
    CHECK(patch->regions.front().points.size() == points.size());
}
}

TEST_CASE("Region point history preserves exact IDs through commit undo redo and cancel", "[editor][region][precision]") {
    auto initial = region_scene();
    const auto id = initial.document.instances.back().id;
    const auto sibling = initial.document.instances[initial.document.instances.size()-2].id;
    const auto before = region_settings(initial, id)->boundary;
    const auto sibling_before = *region_settings(initial, sibling);
    EditingSession session{std::move(initial)}; session.select_keyframe(session.state().viewport.time);
    const auto* storage = region_settings(session.state(), id)->boundary.points.data();
    const auto* faces = region_settings(session.state(), id)->boundary.faces.data();
    const std::array<u32, 2> ids{0, 3};
    REQUIRE(session.begin_region_points(id, ids));
    for (unsigned step = 1; step <= 8; ++step) {
        auto a = before.points[0], b = before.points[3];
        a.x += static_cast<float>(step); b.z -= static_cast<float>(step);
        REQUIRE(session.region_points(std::array{RegionPointEdit{0,a}, RegionPointEdit{3,b}}));
        exact_notice(session, id, {0,3});
    }
    REQUIRE(session.commit());
    const auto moved = region_settings(session.state(), id)->boundary;
    REQUIRE(session.undo()); exact_notice(session, id, {0,3});
    CHECK(region_settings(session.state(), id)->boundary == before);
    REQUIRE(session.redo()); exact_notice(session, id, {0,3});
    CHECK(region_settings(session.state(), id)->boundary == moved);
    REQUIRE(session.begin_region_points(id, std::array<u32,1>{3}));
    REQUIRE(session.region_points(std::array{RegionPointEdit{3,{7,8,9}}}));
    (void)session.take_changes();
    REQUIRE(session.cancel()); exact_notice(session, id, {3});
    CHECK(region_settings(session.state(), id)->boundary == moved);
    CHECK(region_settings(session.state(), id)->boundary.points.data() == storage);
    CHECK(region_settings(session.state(), id)->boundary.faces.data() == faces);
    CHECK(*region_settings(session.state(), sibling) == sibling_before);
}

TEST_CASE("Invalid region point batches cannot escape their gesture scope or partly mutate", "[editor][region][precision]") {
    EditingSession session{region_scene()}; session.select_keyframe(session.state().viewport.time);
    const auto id = session.state().document.instances.back().id;
    const auto before = region_settings(session.state(), id)->boundary;
    const auto revision = session.state().document.revision;
    CHECK_FALSE(session.begin_region_points(id, {}));
    CHECK_FALSE(session.begin_region_points(id, std::array<u32,1>{9999}));
    CHECK_FALSE(session.begin_region_points(9999, std::array<u32,1>{0}));
    REQUIRE(session.begin_region_points(id, std::array<u32,2>{0,1}));
    std::vector<RegionPointEdit> values{{0,{2,3,4}}, {1,{5,6,7}}};
    SECTION("unselected") { values[1].index = 2; }
    SECTION("duplicate") { values[1].index = 0; }
    SECTION("missing") { values.pop_back(); }
    SECTION("nonfinite") { values[1].position.z = std::numeric_limits<f32>::quiet_NaN(); }
    SECTION("out of bounds") { values[1].position.z = scene_coordinate_limit * 2; }
    CHECK_FALSE(session.region_points(values));
    auto whole = region_world_snapshot(session.state(), id); REQUIRE(whole);
    CHECK_FALSE(session.region(*whole));
    CHECK(region_settings(session.state(), id)->boundary == before);
    CHECK(session.state().document.revision == revision);
    CHECK_FALSE(session.take_changes()); CHECK_FALSE(session.dirty());
    REQUIRE(session.cancel());
    CHECK_FALSE(session.can_undo());
}

TEST_CASE("Region topology checkpoints contain only the affected instance", "[editor][region][precision]") {
    EditingSession session{region_scene()}; session.select_keyframe(session.state().viewport.time);
    const auto id = session.state().document.instances.back().id;
    auto boundary = region_world_snapshot(session.state(), id); REQUIRE(boundary);
    const auto original = *boundary;
    REQUIRE(edit_region_geometry(*boundary, RegionAction::subdivide, editor::CageElement::face,
        std::array<u32,1>{0}));
    REQUIRE(session.begin_region(id));
    CHECK_FALSE(session.region_points(std::array{RegionPointEdit{0,{1,2,3}}}));
    REQUIRE(session.region(*boundary)); REQUIRE(session.commit());
    auto notice = session.take_changes(); REQUIRE(notice);
    REQUIRE(notice->changes.regions.size() == 1);
    CHECK(notice->changes.regions.at(id).whole);
    const auto patch = capture_patch(1, session.state(), notice->changes); REQUIRE(patch);
    REQUIRE(patch->regions.size() == 1); REQUIRE(patch->regions.front().replacement);
    CHECK(patch->regions.front().replacement->id == id);
    REQUIRE(session.undo());
    CHECK(region_world_snapshot(session.state(), id) == original);
    REQUIRE(session.redo());
    CHECK(region_world_snapshot(session.state(), id) == boundary);
}
