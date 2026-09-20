#include <vng/rig/rig.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <type_traits>

namespace {
using namespace vng;
using namespace vng::rig;
using Catch::Approx;

struct Position : gfx::Semantic<Vec3> {};
using Vertex = gfx::Record<Position>;
using Mesh = gfx::Mesh<Vertex>;

void same(Vec3 a, Vec3 b, float epsilon = 1e-5F)
{
    CHECK(a.x == Approx(b.x).margin(epsilon));
    CHECK(a.y == Approx(b.y).margin(epsilon));
    CHECK(a.z == Approx(b.z).margin(epsilon));
}

void same(const Mat4& a, const Mat4& b, float epsilon = 1e-5F)
{
    for (std::size_t c = 0; c < 4; ++c) {
        for (std::size_t r = 0; r < 4; ++r) { CHECK(a[c][r] == Approx(b[c][r]).margin(epsilon)); }
    }
}

struct Chain {
    Armature armature;
    BoneId root;
    BoneId child;
};

Chain chain()
{
    ArmatureBuilder builder;
    auto root = builder.add_bone("root", {}, {.translation = {1, 0, 0}});
    auto child = builder.add_bone("child", root, {.translation = {0, 1, 0}});
    auto armature = builder.build();
    REQUIRE(armature);
    return {*armature, root, child};
}
}

TEST_CASE("rig transforms use column vectors and validated positive uniform scales", "[rig][math]")
{
    same(matrix(Transform{}), Mat4::identity());
    auto q = rotation({0, 0, 5}, degrees(90));
    REQUIRE(q);
    Transform t{.translation = {1, 2, 3}, .rotation = *q, .scale = 2};
    REQUIRE(validate(t));
    const auto m = matrix(t);
    same(transform_point(m, {1, 0, 0}), {1, 4, 3});
    same(transform_vector(m, {1, 0, 0}), {0, 2, 0});
    auto inverse = inverse_affine(m);
    REQUIRE(inverse);
    same(multiply(*inverse, m), Mat4::identity());
    same(multiply(m, *inverse), Mat4::identity());
    auto normal = normal_matrix(m);
    REQUIRE(normal);
    same(transform_vector(*normal, {1, 0, 0}), {0, 0.5F, 0});
    CHECK_FALSE(rotation({0, 0, 0}, degrees(20)));
    CHECK_FALSE(rotation({0, 1, 0}, std::numeric_limits<float>::infinity()));
    CHECK_FALSE(validate(Transform{.scale = 0}));
    CHECK_FALSE(validate(Transform{.scale = -1}));
    CHECK_FALSE(validate(Transform{.rotation = {0, 0, 0, 2}}));
    CHECK_FALSE(validate(Transform{.translation = {std::numeric_limits<float>::quiet_NaN(), 0, 0}}));
    CHECK_FALSE(inverse_affine(Mat4{}));
    auto projective = Mat4::identity();
    projective[0][3] = 1;
    CHECK_FALSE(inverse_affine(projective));
    auto singular = Mat4::identity();
    singular[1] = singular[0];
    CHECK_FALSE(inverse_affine(singular));
}

TEST_CASE("armatures retain owner tagged bone IDs and immutable hierarchy", "[rig][armature]")
{
    const auto a = chain();
    const auto b = chain();
    REQUIRE(a.armature.valid());
    CHECK(a.armature.bone_count() == 2);
    CHECK(a.armature.identity() != b.armature.identity());
    CHECK(a.root != b.root);
    CHECK(a.armature.bone("child") == a.child);
    CHECK_FALSE(a.armature.bone("missing"));
    CHECK_FALSE(a.armature.index(b.root));
    CHECK_FALSE(a.armature.index(BoneId{}));
    CHECK(a.armature.bones()[1].parent == a.root);
    same(transform_point(a.armature.rest_globals()[1], {}), {1, 1, 0});
    auto copy = a.armature;
    CHECK(copy.identity() == a.armature.identity());
    CHECK(copy.bones().data() == a.armature.bones().data());

    ArmatureBuilder empty;
    CHECK_FALSE(empty.build());
    ArmatureBuilder duplicate;
    REQUIRE(duplicate.add_bone("root"));
    CHECK_FALSE(duplicate.add_bone("root"));
    auto duplicate_result = duplicate.build();
    REQUIRE_FALSE(duplicate_result);
    CHECK(duplicate_result.error().code == ErrorCode::duplicate_name);
    ArmatureBuilder foreign;
    CHECK_FALSE(foreign.add_bone("child", a.root));
    CHECK_FALSE(foreign.build());
    ArmatureBuilder finished;
    REQUIRE(finished.add_bone("root"));
    auto first = finished.build();
    REQUIRE(first);
    CHECK(finished.build()->identity() == first->identity());
    CHECK_FALSE(finished.add_bone("late"));
    CHECK_FALSE(finished.build());
    CHECK(first->bone_count() == 1);
    ArmatureBuilder invalid;
    CHECK_FALSE(invalid.add_bone("bad", {}, {.scale = -1}));
    CHECK_FALSE(invalid.build());
}

TEST_CASE("poses are independent mutable instances with absolute local rotations", "[rig][pose]")
{
    const auto a = chain();
    auto pose = a.armature.rest_pose();
    auto copy = pose;
    REQUIRE(pose.globals());
    auto q = rotation({0, 0, 1}, degrees(90));
    REQUIRE(q);
    REQUIRE(pose.set_local_rotation(a.child, *q));
    CHECK(pose.revision() == 1);
    CHECK(copy.revision() == 0);
    CHECK(copy.local(a.child)->rotation == Quat{});
    CHECK(pose.local(a.child)->translation == Vec3{0, 1, 0});
    auto globals = pose.globals();
    REQUIRE(globals);
    same(transform_point((*globals)[1], {1, 0, 0}), {1, 2, 0});
    REQUIRE(pose.set_local_rotation(a.child, *q));
    CHECK(pose.revision() == 1); // unchanged writes are inert
    auto before = pose.local(a.child);
    CHECK_FALSE(pose.set_local_rotation(a.child, Quat{0, 0, 0, 0}));
    CHECK(pose.local(a.child) == before);
    CHECK_FALSE(pose.set_local(chain().child, Transform{}));
    pose.reset();
    CHECK(pose.revision() == 2);
    CHECK(pose.local(a.child)->rotation == Quat{});
    pose.reset();
    CHECK(pose.revision() == 2);
    CHECK_FALSE(Pose{}.globals());
}

TEST_CASE("binding owns a frozen mesh snapshot without modifying vertex records", "[rig][binding]")
{
    const auto a = chain();
    Mesh source(3);
    source.vertices()[0].set(Position{}, {1, 2, 3});
    source.add_face(0, 1, 2);
    auto binding = rig::bind(source, a.armature);
    REQUIRE(binding);
    static_assert(std::is_same_v<decltype(binding->mesh()), const Mesh&>);
    source.vertices()[0].set(Position{}, {8, 9, 10});
    source.resize_vertices(4);
    source.faces().clear();
    CHECK(binding->mesh().vertex_count() == 3);
    CHECK(binding->mesh().face_count() == 1);
    CHECK(binding->mesh().vertices()[0].get(Position{}) == Vec3{1, 2, 3});
    auto copied = *binding;
    CHECK(copied.snapshot_identity() == binding->snapshot_identity());
    CHECK(&copied.mesh() == &binding->mesh());
    auto another = rig::bind(Mesh(3), a.armature);
    REQUIRE(another);
    CHECK(another->snapshot_identity() != binding->snapshot_identity());
    CHECK_FALSE(rig::bind(source, Armature{}));
    CHECK_FALSE(rig::bind(source, a.armature, {.mesh_to_armature = {.scale = 0}}));
    source.add_face(0, 1, 100);
    CHECK_FALSE(rig::bind(source, a.armature));
}

TEST_CASE("weights validate ownership normalization and vertex coverage explicitly", "[rig][weights]")
{
    const auto a = chain();
    auto binding = rig::bind(Mesh(2), a.armature);
    REQUIRE(binding);
    CHECK_FALSE(binding->validate());
    CHECK_FALSE(binding->normalize_weights());
    REQUIRE(binding->set_weights(0, {{a.root, 2}, {a.child, 1}}));
    REQUIRE(binding->set_weights(1, {{a.child, 1}, {a.root, 0}}));
    auto invalid_sum = binding->validate();
    REQUIRE_FALSE(invalid_sum);
    CHECK(invalid_sum.error().code == ErrorCode::unnormalized_weights);
    CHECK_FALSE(binding->set_weights(0, {{a.root, -1}}));
    CHECK_FALSE(binding->set_weights(0, {{a.root, std::numeric_limits<float>::quiet_NaN()}}));
    CHECK_FALSE(binding->set_weights(0, {{chain().root, 1}}));
    CHECK_FALSE(binding->set_weights(0, {{a.root, 0.5F}, {a.root, 0.5F}}));
    CHECK_FALSE(binding->set_weights(100, {{a.root, 1}}));
    CHECK(binding->weights().influences(0)->front().weight == 2.0F);
    REQUIRE(binding->normalize_weights());
    REQUIRE(binding->validate());
    const auto values = binding->weights().influences(0);
    REQUIRE(values);
    CHECK((*values)[0].weight == Approx(2.0F / 3));
    CHECK((*values)[1].weight == Approx(1.0F / 3));
    CHECK_FALSE(binding->weights().influences(2));
    auto packed = binding->pack();
    REQUIRE(packed);
    CHECK(packed->max_influences == 4);
    CHECK(packed->vertices.size() == 2);
    CHECK(packed->vertices[1].joints0.x == 1);
    CHECK(packed->vertices[1].weights0.x == 1.0F);
    CHECK(packed->vertices[1].weights0.y == 0.0F);
    CHECK(packed->vertices[1].weights1 == Vec4{});
}

TEST_CASE("source weights retain arbitrary influences and pruning reports lost weight", "[rig][weights]")
{
    ArmatureBuilder builder;
    std::array<BoneId, 9> bones;
    for (std::size_t i = 0; i < bones.size(); ++i) { bones[i] = builder.add_bone("bone" + std::to_string(i)); }
    auto armature = builder.build();
    REQUIRE(armature);
    auto binding = rig::bind(Mesh(1), *armature);
    REQUIRE(binding);
    std::array<Influence, 9> values;
    for (std::size_t i = 0; i < values.size(); ++i) { values[i] = {bones[i], 1.0F / 9}; }
    REQUIRE(binding->set_weights(0, values));
    REQUIRE(binding->validate());
    CHECK_FALSE(binding->pack(4));
    CHECK_FALSE(binding->pack(8));
    CHECK_FALSE(binding->pack(5));
    CHECK_FALSE(binding->prune_weights(0));
    CHECK(binding->weights().influences(0)->size() == 9);
    auto pruned = binding->prune_weights(8);
    REQUIRE(pruned);
    REQUIRE(pruned->vertices.size() == 1);
    CHECK(pruned->vertices[0].removed_influences == 1);
    CHECK(pruned->maximum_discarded_weight == Approx(1.0F / 9));
    auto packed = binding->pack(8);
    REQUIRE(packed);
    CHECK(packed->vertices[0].joints0 == UVec4{0, 1, 2, 3});
    CHECK(packed->vertices[0].joints1 == UVec4{4, 5, 6, 7});
    CHECK(packed->vertices[0].weights1.w == Approx(1.0F / 8));
    auto reduced = binding->prune_weights(4);
    REQUIRE(reduced);
    CHECK(reduced->maximum_discarded_weight == Approx(0.5F));
    REQUIRE(binding->pack(4));
}

TEST_CASE("rest pose reproduces geometry across translated rotated scaled bind spaces", "[rig][skinning]")
{
    auto q = rotation({1, 2, 3}, degrees(47));
    REQUIRE(q);
    ArmatureBuilder builder;
    const auto root = builder.add_bone("root", {}, {.translation = {2, 3, -1}, .rotation = *q, .scale = 1.5F});
    const auto child = builder.add_bone("child", root, {.translation = {1, 2, 0}, .rotation = *q, .scale = 0.5F});
    auto armature = builder.build();
    REQUIRE(armature);
    auto binding = rig::bind(Mesh(2), *armature,
        {.mesh_to_armature = {.translation = {-2, 1, 3}, .rotation = *q, .scale = 2.5F}});
    REQUIRE(binding);
    REQUIRE(binding->set_weights(0, {{root, 1}}));
    REQUIRE(binding->set_weights(1, {{root, 0.3F}, {child, 0.7F}}));
    auto palette = binding->palette(armature->rest_pose());
    REQUIRE(palette);
    REQUIRE(palette->size() == 2);
    same((*palette)[0], Mat4::identity());
    same((*palette)[1], Mat4::identity());
    const std::array points{Vec3{1, 4, 7}, Vec3{-2, 8, 3}};
    auto deformed = binding->deform_points(points, armature->rest_pose());
    REQUIRE(deformed);
    same((*deformed)[0], points[0]);
    same((*deformed)[1], points[1]);
    CHECK_FALSE(binding->palette(chain().armature.rest_pose()));
    CHECK_FALSE(binding->deform_points(std::span<const Vec3>{}, armature->rest_pose()));
}

TEST_CASE("CPU reference applies hierarchy and weighted deformation including normals", "[rig][skinning]")
{
    const auto a = chain();
    auto binding = rig::bind(Mesh(3), a.armature);
    REQUIRE(binding);
    REQUIRE(binding->set_weights(0, {{a.root, 1}}));
    REQUIRE(binding->set_weights(1, {{a.child, 1}}));
    REQUIRE(binding->set_weights(2, {{a.root, 0.25F}, {a.child, 0.75F}}));
    auto pose = a.armature.rest_pose();
    auto q = rotation({0, 0, 1}, degrees(90));
    REQUIRE(q);
    REQUIRE(pose.set_local_rotation(a.child, *q));
    const std::array points{Vec3{2, 1, 0}, Vec3{2, 1, 0}, Vec3{2, 1, 0}};
    auto deformed = binding->deform_points(points, pose);
    REQUIRE(deformed);
    same((*deformed)[0], {2, 1, 0});
    same((*deformed)[1], {1, 2, 0});
    same((*deformed)[2], {1.25F, 1.75F, 0});
    const std::array normals{Vec3{1, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 0, 0}};
    auto rotated = binding->deform_normals(normals, pose);
    REQUIRE(rotated);
    same((*rotated)[0], {1, 0, 0});
    same((*rotated)[1], {0, 1, 0});
    same((*rotated)[2], {1.0F/std::sqrt(10.0F), 3.0F/std::sqrt(10.0F), 0});
    REQUIRE(pose.set_local(a.child, {.translation = {0, 1, 0}, .rotation = *q, .scale = 2}));
    rotated = binding->deform_normals(normals, pose);
    REQUIRE(rotated);
    // inverse transpose makes the child's contribution half-strength
    same((*rotated)[2], {2.0F/std::sqrt(13.0F), 3.0F/std::sqrt(13.0F), 0});
    const std::array zero_normals{Vec3{}, Vec3{}, Vec3{}};
    CHECK_FALSE(binding->deform_normals(zero_normals, pose));
}

TEST_CASE("invalid global transforms are diagnosed without producing a palette", "[rig][validation]")
{
    ArmatureBuilder huge;
    auto root = huge.add_bone("root", {}, {.scale = 1e30F});
    REQUIRE(root);
    REQUIRE(huge.add_bone("child", root, {.scale = 1e30F}));
    CHECK_FALSE(huge.build());
    const auto a = chain();
    auto pose = a.armature.rest_pose();
    REQUIRE(pose.set_local(a.root, {.scale = 1e30F}));
    REQUIRE(pose.set_local(a.child, {.scale = 1e30F}));
    CHECK_FALSE(pose.globals());
    auto binding = rig::bind(Mesh(1), a.armature);
    REQUIRE(binding);
    REQUIRE(binding->set_weights(0, {{a.root, 1}}));
    CHECK_FALSE(binding->palette(pose));
}

TEST_CASE("moved-from bindings and weights reject use while copies own independent weights", "[rig][lifetime]")
{
    const auto a = chain();
    auto bound = rig::bind(Mesh(1), a.armature);
    REQUIRE(bound);
    REQUIRE(bound->set_weights(0, {{a.root, 1}}));
    auto copy = *bound;
    REQUIRE(copy.set_weights(0, {{a.child, 1}}));
    REQUIRE(copy.validate());
    REQUIRE(bound->validate());
    CHECK(&copy.mesh() == &bound->mesh());
    CHECK(bound->weights().influences(0)->front().bone == a.root);
    CHECK(copy.weights().influences(0)->front().bone == a.child);
    auto moved = std::move(*bound);
    REQUIRE(moved.validate());
    CHECK_FALSE(bound->validate());
    CHECK_FALSE(bound->weights().validate());
    CHECK_FALSE(bound->normalize_weights());
    CHECK_FALSE(bound->prune_weights(4));
    CHECK_FALSE(bound->pack());
    CHECK_FALSE(bound->palette(a.armature.rest_pose()));
    CHECK_FALSE(bound->palette(Pose{}));
    CHECK(bound->snapshot_identity() == 0);
    CHECK_THROWS_AS(bound->mesh(), std::logic_error);
    auto weights = moved.weights();
    auto moved_weights = std::move(weights);
    REQUIRE(moved_weights.validate());
    CHECK_FALSE(weights.validate());
    CHECK_FALSE(weights.normalize_weights());
    CHECK_FALSE(weights.palette(Pose{}));
    auto pose = a.armature.rest_pose();
    auto moved_pose = std::move(pose);
    pose.reset();
    CHECK_FALSE(pose.globals());
    CHECK(moved_pose.globals().has_value());
}

TEST_CASE("normal deformation accepts finite nonzero directions regardless of magnitude", "[rig][normals]")
{
    ArmatureBuilder builder;
    const auto root = builder.add_bone("root");
    auto armature = builder.build();
    REQUIRE(armature);
    auto binding = rig::bind(Mesh(1), *armature);
    REQUIRE(binding);
    REQUIRE(binding->set_weights(0, {{root, 1}}));
    auto pose = armature->rest_pose();
    const std::array tiny{Vec3{1e-35F, 0, 0}};
    auto normalized = binding->deform_normals(tiny, pose);
    REQUIRE(normalized);
    same(normalized->front(), {1, 0, 0});
    const std::array huge{Vec3{1e35F, 0, 0}};
    normalized = binding->deform_normals(huge, pose);
    REQUIRE(normalized);
    same(normalized->front(), {1, 0, 0});
    REQUIRE(pose.set_local(root, {.scale = 1e25F}));
    const std::array unit{Vec3{0, 1, 0}};
    normalized = binding->deform_normals(unit, pose);
    REQUIRE(normalized);
    same(normalized->front(), {0, 1, 0});
    normalized = binding->deform_normals(tiny, pose);
    REQUIRE(normalized);
    same(normalized->front(), {1, 0, 0});
    REQUIRE(pose.set_local(root, {.scale = 1e-25F}));
    normalized = binding->deform_normals(huge, pose);
    REQUIRE(normalized);
    same(normalized->front(), {1, 0, 0});
}
