#include "../../examples/character/character.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <random>

namespace {
using namespace vng;

// A two-bone rig with one looping clip that turns the child a quarter turn.
constexpr std::string_view small_rig = R"(vrig 1.0
rig = 1;
name = "arm";
bones = [
    { name = "root"; parent = ""; translation = [0, 0, 0]; rotation = [0, 0, 0, 1]; scale = 1; },
    { name = "tip"; parent = "root"; translation = [0, 1, 0]; rotation = [0, 0, 0, 1]; scale = 1; },
];
clips = [
    {
        name = "turn";
        fps = 2;
        frames = 2;
        loop = true;
        speed = 0.5;
        tracks = [
            { bone = "tip"; translation = [0, 1, 0, 0, 3, 0]; rotation = [0, 0, 0, 1, 0, 0, 0.70710677, 0.70710677]; scale = [1, 2]; },
        ];
    },
];
)";
std::string with(std::string_view from, std::string_view to) {
    std::string text{small_rig};
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    return text.replace(at, from.size(), to);
}
Vec3 point(const character::Mesh& mesh, std::size_t v) { return mesh.vertices()[v].get(gfx::Position{}); }
std::vector<Vec3> positions(const character::Mesh& mesh) {
    std::vector<Vec3> out;
    for (std::size_t v = 0; v < mesh.vertex_count(); ++v) out.push_back(point(mesh, v));
    return out;
}
f32 distance(Vec3 a, Vec3 b) { return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z); }
} // namespace

TEST_CASE("A rig document reads bones, clips and tracks", "[character][file]") {
    const auto rig = character::parse_rig(small_rig, "arm.vrig");
    REQUIRE(rig);
    REQUIRE(rig->bones.size() == 2);
    CHECK(rig->bones[1].parent == "root");
    CHECK(rig->bones[1].rest.translation == Vec3{0, 1, 0});
    const auto* turn = rig->clip("turn");
    REQUIRE(turn);
    CHECK(turn->fps == 2.F);
    CHECK(turn->loop);
    CHECK(turn->speed == Catch::Approx(0.5));
    CHECK(turn->duration() == Catch::Approx(1));
    REQUIRE(turn->tracks.size() == 1);
    CHECK(turn->tracks[0].bone == 1);
    CHECK(turn->tracks[0].frames[1].translation == Vec3{0, 3, 0});
    CHECK(turn->tracks[0].frames[1].scale == 2.F);
    CHECK_FALSE(rig->clip("walk"));
}

TEST_CASE("Rig documents that break the rules are refused", "[character][file]") {
    CHECK_FALSE(character::parse_rig(with("vrig 1.0", "vmesh 1.0"), "x"));
    CHECK_FALSE(character::parse_rig(with("rig = 1;", "rig = 2;"), "x"));
    CHECK_FALSE(character::parse_rig(with(R"(name = "tip"; parent = "root")", R"(name = "root"; parent = "root")"), "x"));
    CHECK_FALSE(character::parse_rig(with(R"(name = "tip"; parent = "root")", R"(name = "tip"; parent = "later")"), "x"));
    CHECK_FALSE(character::parse_rig(with("rotation = [0, 0, 0, 1]; scale = 1; },\n];", "rotation = [0, 0, 0, 2]; scale = 1; },\n];"), "x"));
    CHECK_FALSE(character::parse_rig(with("rotation = [0, 0, 0, 1]; scale = 1; },\n];", "rotation = [0, 0, 0, 1]; scale = -1; },\n];"), "x"));
    CHECK_FALSE(character::parse_rig(with("fps = 2;", "fps = 0;"), "x"));
    CHECK_FALSE(character::parse_rig(with("frames = 2;", "frames = 3;"), "x"));
    CHECK_FALSE(character::parse_rig(with(R"(bone = "tip")", R"(bone = "elbow")"), "x"));
    CHECK_FALSE(character::parse_rig(with("scale = [1, 2]", "scale = [1, 0]"), "x"));
    CHECK_FALSE(character::read_rig("missing.vrig"));
}

TEST_CASE("Frames blend linearly, rotations by the shortest path", "[character][clip]") {
    const rig::Transform a{{0, 0, 0}, {0, 0, 0, 1}, 1};
    const rig::Transform b{{2, 4, 6}, {0, 0, 0.70710677F, 0.70710677F}, 3};
    const auto half = character::blend(a, b, .5F);
    CHECK(half.translation == Vec3{1, 2, 3});
    CHECK(half.scale == Catch::Approx(2));
    CHECK(half.rotation.z == Catch::Approx(0.38268343).margin(1e-6)); // an eighth turn
    CHECK(half.rotation.w == Catch::Approx(0.92387953).margin(1e-6));
    // The same rotation written with the opposite sign blends the short way.
    const auto flipped = character::blend(a, {b.translation, {0, 0, -0.70710677F, -0.70710677F}, 3}, .5F);
    CHECK(flipped.rotation.z == Catch::Approx(half.rotation.z).margin(1e-6));
    CHECK(flipped.rotation.w == Catch::Approx(half.rotation.w).margin(1e-6));
}

TEST_CASE("The soldier loads with his armature, skin and walk", "[character][soldier]") {
    auto soldier = character::Character::load(VNG_SOLDIER_MESH);
    REQUIRE(soldier);
    CHECK(soldier->armature().bone_count() == 99);
    CHECK(soldier->binding().mesh().vertex_count() > 10'000);
    const auto* walk = soldier->clip("walk");
    REQUIRE(walk);
    CHECK(walk->frames == 24);
    CHECK(walk->fps == 24.F);
    CHECK(walk->loop);
    CHECK(walk->speed > .5F);
    CHECK(walk->speed < 1.2F);
    CHECK(walk->tracks.size() == soldier->rig().bones.size());
    // One unit is a metre: he is 1.8 m tall and stands on Y = 0.
    const auto points = positions(soldier->binding().mesh());
    const auto [low, high] = std::ranges::minmax(points, {}, &Vec3::y);
    CHECK(low.y == Catch::Approx(0).margin(1e-4));
    CHECK(high.y == Catch::Approx(1.8).margin(1e-3));
}

TEST_CASE("The soldier's rest pose leaves the mesh where it is", "[character][soldier]") {
    auto soldier = character::Character::load(VNG_SOLDIER_MESH);
    REQUIRE(soldier);
    const auto points = positions(soldier->binding().mesh());
    const auto rest = soldier->binding().deform_points(points, soldier->armature().rest_pose());
    REQUIRE(rest);
    f32 worst = 0;
    for (std::size_t v = 0; v < points.size(); ++v) worst = std::max(worst, distance(points[v], (*rest)[v]));
    CHECK(worst < 1e-5F);
}

TEST_CASE("The soldier walks on the floor with a rigid rifle, and the cycle loops", "[character][soldier]") {
    auto soldier = character::Character::load(VNG_SOLDIER_MESH);
    REQUIRE(soldier);
    const auto& walk = *soldier->clip("walk");
    const auto& mesh = soldier->binding().mesh();
    const auto points = positions(mesh);
    // The rifle is bound wholly to the left hand; find its vertices by that.
    const auto hand = soldier->armature().bone("DEF-hand.L");
    REQUIRE(hand);
    std::vector<std::size_t> rifle;
    for (std::size_t v = 0; v < points.size(); ++v) {
        const auto influences = soldier->binding().weights().influences(v);
        REQUIRE(influences);
        if (influences->size() == 1 && (*influences)[0].bone == *hand) rifle.push_back(v);
    }
    REQUIRE(rifle.size() > 100);
    std::mt19937 random{7};
    std::uniform_int_distribution<std::size_t> pick{0, rifle.size() - 1};
    std::vector<std::pair<std::size_t, std::size_t>> pairs;
    while (pairs.size() < 200) {
        const auto a = rifle[pick(random)], b = rifle[pick(random)];
        if (distance(points[a], points[b]) > .05F) pairs.emplace_back(a, b);
    }
    auto pose = soldier->armature().rest_pose();
    for (std::size_t frame = 0; frame < walk.frames; ++frame) {
        REQUIRE(soldier->pose(walk, static_cast<f32>(frame) / walk.fps, pose));
        const auto posed = soldier->binding().deform_points(points, pose);
        REQUIRE(posed);
        // Feet meet the floor: the lowest point stays within 5 cm of Y = 0.
        CHECK(std::ranges::min(*posed, {}, &Vec3::y).y == Catch::Approx(0).margin(.05));
        // The rifle keeps its shape (it may scale uniformly with the hand).
        f32 lowest = 1e9F, highest = 0;
        for (const auto& [a, b] : pairs) {
            const auto ratio = distance((*posed)[a], (*posed)[b]) / distance(points[a], points[b]);
            lowest = std::min(lowest, ratio);
            highest = std::max(highest, ratio);
        }
        CHECK(highest / lowest < 1.0005F);
    }
    // A looping clip comes back to its first frame after its duration.
    auto start = soldier->armature().rest_pose(), end = soldier->armature().rest_pose();
    REQUIRE(soldier->pose(walk, 0, start));
    REQUIRE(soldier->pose(walk, walk.duration(), end));
    const auto a = soldier->binding().deform_points(points, start), b = soldier->binding().deform_points(points, end);
    REQUIRE(a);
    REQUIRE(b);
    f32 worst = 0;
    for (std::size_t v = 0; v < points.size(); ++v) worst = std::max(worst, distance((*a)[v], (*b)[v]));
    CHECK(worst < 1e-4F);
}

TEST_CASE("The re-keyed walk plants a foot every frame, bobs the hips and stretches nothing", "[character][soldier]") {
    auto soldier = character::Character::load(VNG_SOLDIER_MESH);
    REQUIRE(soldier);
    const auto& walk = *soldier->clip("walk");
    REQUIRE(soldier->clip("walk_authored"));
    const auto points = positions(soldier->binding().mesh());
    const auto pelvis = soldier->armature().bone("DEF-spine");
    REQUIRE(pelvis);
    const auto pelvis_index = soldier->armature().index(*pelvis);
    REQUIRE(pelvis_index);
    auto pose = soldier->armature().rest_pose();
    f32 low = 1e9F, high = -1e9F;
    for (std::size_t frame = 0; frame < walk.frames; ++frame) {
        const auto seconds = static_cast<f32>(frame) / walk.fps;
        REQUIRE(soldier->pose(walk, seconds, pose));
        const auto posed = soldier->binding().deform_points(points, pose);
        REQUIRE(posed);
        // Double support or not, some foot is on the floor, and nothing is under it.
        CHECK(std::ranges::min(*posed, {}, &Vec3::y).y == Catch::Approx(0).margin(.01));
        const auto globals = pose.globals();
        REQUIRE(globals);
        const auto& hips = (*globals)[*pelvis_index];
        low = std::min(low, hips[3][1]);
        high = std::max(high, hips[3][1]);
        for (const auto& track : walk.tracks) CHECK(track.frames[frame].scale == Catch::Approx(1).margin(1e-3));
    }
    CHECK(high - low > .02F); // the hips rise and fall with each step
}

TEST_CASE("The re-keyed walk keeps every joint together and both hands on the rifle", "[character][soldier]") {
    auto soldier = character::Character::load(VNG_SOLDIER_MESH);
    REQUIRE(soldier);
    const auto& rig = soldier->rig();
    const auto& walk = *soldier->clip("walk");
    // Only the hips hang from the root. Every other bone sits at its modelled
    // place on its parent in every frame, however the clip turns it, so no
    // joint opens: the authored right arm stretched 9 cm short of the rifle.
    std::size_t loose = 0;
    for (const auto& track : walk.tracks) {
        const auto& bone = rig.bones[track.bone];
        if (bone.parent.empty()) continue;
        if (bone.parent == "root") {
            ++loose;
            continue;
        }
        f32 worst = 0;
        for (const auto& frame : track.frames) worst = std::max(worst, distance(frame.translation, bone.rest.translation));
        CHECK(worst < 1e-4F);
    }
    CHECK(loose == 1);
    // The right hand supports the rifle the left hand holds: its pose relative
    // to the left hand never changes as he walks.
    const auto left = soldier->armature().bone("DEF-hand.L"), right = soldier->armature().bone("DEF-hand.R");
    REQUIRE(left);
    REQUIRE(right);
    const auto l = soldier->armature().index(*left), r = soldier->armature().index(*right);
    REQUIRE(l);
    REQUIRE(r);
    auto pose = soldier->armature().rest_pose();
    std::optional<Mat4> first;
    f32 drift = 0;
    for (std::size_t frame = 0; frame < walk.frames; ++frame) {
        REQUIRE(soldier->pose(walk, static_cast<f32>(frame) / walk.fps, pose));
        const auto globals = pose.globals();
        REQUIRE(globals);
        const auto holding = rig::inverse_affine((*globals)[*l]);
        REQUIRE(holding);
        const auto relative = rig::multiply(*holding, (*globals)[*r]);
        if (!first) first = relative;
        for (std::size_t c = 0; c < 4; ++c)
            for (std::size_t row = 0; row < 3; ++row) drift = std::max(drift, std::abs(relative[c][row] - (*first)[c][row]));
    }
    CHECK(drift < 1e-3F);
}
