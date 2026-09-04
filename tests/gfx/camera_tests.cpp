#include <vng/analysis/types.hpp>
#include <vng/gfx/gfx.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <type_traits>
#include <variant>

namespace {

using Catch::Approx;

[[nodiscard]] vng::Vec4 transform(const vng::Mat4& matrix, vng::Vec4 value)
{
    vng::Vec4 result{};
    for (std::size_t row = 0; row < 4; ++row) {
        result[row] = (matrix[0][row] * value.x)
            + (matrix[1][row] * value.y)
            + (matrix[2][row] * value.z)
            + (matrix[3][row] * value.w);
    }
    return result;
}

[[nodiscard]] float dot(vng::Vec3 left, vng::Vec3 right)
{
    return (left.x * right.x)
        + (left.y * right.y)
        + (left.z * right.z);
}

void check_vector(vng::Vec3 actual, vng::Vec3 expected, float margin = 1.0e-5F)
{
    CHECK(actual.x == Approx(expected.x).margin(margin));
    CHECK(actual.y == Approx(expected.y).margin(margin));
    CHECK(actual.z == Approx(expected.z).margin(margin));
}

void check_vector(vng::Vec4 actual, vng::Vec4 expected, float margin = 1.0e-5F)
{
    CHECK(actual.x == Approx(expected.x).margin(margin));
    CHECK(actual.y == Approx(expected.y).margin(margin));
    CHECK(actual.z == Approx(expected.z).margin(margin));
    CHECK(actual.w == Approx(expected.w).margin(margin));
}

void check_matrix(
    const vng::Mat4& actual,
    const vng::Mat4& expected,
    float margin = 1.0e-5F)
{
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            CHECK(actual[column][row]
                  == Approx(expected[column][row]).margin(margin));
        }
    }
}

} // namespace

static_assert(std::same_as<vng::Extent2D, vng::analysis::Extent2D>);
static_assert(vng::Extent2D{}.empty());
static_assert(!vng::Extent2D{1, 1}.empty());
static_assert(vng::degrees(180.0F).radians() > 3.14F);

TEST_CASE("angles make camera field-of-view units explicit", "[gfx][camera][angle]")
{
    CHECK(vng::degrees(180.0F).radians()
          == Approx(std::numbers::pi_v<float>));
    CHECK(vng::radians(std::numbers::pi_v<float> * 0.5F).degrees()
          == Approx(90.0F));
}

TEST_CASE("the default camera is a valid canonical right-handed view", "[gfx][camera]")
{
    const vng::gfx::Camera camera;
    auto snapshot = camera.snapshot({1600, 900});
    REQUIRE(snapshot.has_value());

    check_vector(snapshot->position, {0.0F, 0.0F, 0.0F});
    check_vector(snapshot->right, {1.0F, 0.0F, 0.0F});
    check_vector(snapshot->up, {0.0F, 1.0F, 0.0F});
    check_vector(snapshot->forward, {0.0F, 0.0F, -1.0F});
    CHECK(snapshot->extent == vng::Extent2D{1600, 900});
    CHECK(snapshot->aspect_ratio == Approx(1600.0F / 900.0F));
    check_matrix(snapshot->view, vng::Mat4::identity());

    REQUIRE(std::holds_alternative<vng::gfx::PerspectiveLens>(camera.lens()));
    const auto& lens = std::get<vng::gfx::PerspectiveLens>(camera.lens());
    CHECK(lens.vertical_fov.degrees() == Approx(60.0F));
    CHECK(lens.near_plane == Approx(0.1F));
    CHECK(lens.far_plane == Approx(1000.0F));
}

TEST_CASE("camera pose methods are fluent and moving preserves orientation", "[gfx][camera]")
{
    vng::gfx::Camera camera;
    auto* returned = &camera.set_position({1.0F, 2.0F, 3.0F})
        .look_at({1.0F, 2.0F, 2.0F})
        .move_by({2.0F, -1.0F, 4.0F});

    CHECK(returned == &camera);
    CHECK(camera.position() == vng::Vec3{3.0F, 1.0F, 7.0F});
    CHECK(camera.direction() == vng::Vec3{0.0F, 0.0F, -1.0F});
    CHECK(camera.up_hint() == vng::Vec3{0.0F, 1.0F, 0.0F});

    auto snapshot = camera.snapshot({64, 64});
    REQUIRE(snapshot.has_value());
    check_vector(
        transform(snapshot->view, {3.0F, 1.0F, 7.0F, 1.0F}),
        {0.0F, 0.0F, 0.0F, 1.0F});
    check_vector(
        transform(snapshot->view, {3.0F, 1.0F, 6.0F, 1.0F}),
        {0.0F, 0.0F, -1.0F, 1.0F});
}

TEST_CASE("look_at produces an orthonormal camera basis", "[gfx][camera]")
{
    vng::gfx::Camera camera;
    camera.set_position({3.0F, 4.0F, 5.0F})
        .look_at({-2.0F, 1.0F, 0.5F}, {0.0F, 1.0F, 0.25F});

    auto snapshot = camera.snapshot({1024, 768});
    REQUIRE(snapshot.has_value());

    CHECK(dot(snapshot->right, snapshot->right) == Approx(1.0F));
    CHECK(dot(snapshot->up, snapshot->up) == Approx(1.0F));
    CHECK(dot(snapshot->forward, snapshot->forward) == Approx(1.0F));
    CHECK(dot(snapshot->right, snapshot->up) == Approx(0.0F).margin(1.0e-6F));
    CHECK(dot(snapshot->right, snapshot->forward) == Approx(0.0F).margin(1.0e-6F));
    CHECK(dot(snapshot->up, snapshot->forward) == Approx(0.0F).margin(1.0e-6F));

    check_vector(
        transform(snapshot->view, {
            snapshot->position.x,
            snapshot->position.y,
            snapshot->position.z,
            1.0F,
        }),
        {0.0F, 0.0F, 0.0F, 1.0F});
    check_vector(
        transform(snapshot->view, {
            snapshot->position.x + snapshot->right.x,
            snapshot->position.y + snapshot->right.y,
            snapshot->position.z + snapshot->right.z,
            1.0F,
        }),
        {1.0F, 0.0F, 0.0F, 1.0F});
    check_vector(
        transform(snapshot->view, {
            snapshot->position.x + snapshot->up.x,
            snapshot->position.y + snapshot->up.y,
            snapshot->position.z + snapshot->up.z,
            1.0F,
        }),
        {0.0F, 1.0F, 0.0F, 1.0F});
    check_vector(
        transform(snapshot->view, {
            snapshot->position.x + snapshot->forward.x,
            snapshot->position.y + snapshot->forward.y,
            snapshot->position.z + snapshot->forward.z,
            1.0F,
        }),
        {0.0F, 0.0F, -1.0F, 1.0F});
}

TEST_CASE("perspective projection maps its planes to canonical OpenGL depth", "[gfx][camera][projection]")
{
    vng::gfx::Camera camera;
    camera.set_perspective({
        .vertical_fov = vng::degrees(90.0F),
        .near_plane = 2.0F,
        .far_plane = 10.0F,
    });

    auto snapshot = camera.snapshot({800, 400});
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->aspect_ratio == Approx(2.0F));

    const auto near_clip = transform(
        snapshot->projection, {0.0F, 0.0F, -2.0F, 1.0F});
    const auto far_clip = transform(
        snapshot->projection, {0.0F, 0.0F, -10.0F, 1.0F});
    CHECK(near_clip.z / near_clip.w == Approx(-1.0F));
    CHECK(far_clip.z / far_clip.w == Approx(1.0F));

    // At 90 degrees, the vertical half-extent at the near plane is near;
    // horizontal extent is additionally scaled by the viewport aspect.
    const auto top = transform(
        snapshot->projection, {0.0F, 2.0F, -2.0F, 1.0F});
    const auto right = transform(
        snapshot->projection, {4.0F, 0.0F, -2.0F, 1.0F});
    CHECK(top.y / top.w == Approx(1.0F));
    CHECK(right.x / right.w == Approx(1.0F));
}

TEST_CASE("orthographic projection uses vertical height and viewport aspect", "[gfx][camera][projection]")
{
    vng::gfx::Camera camera;
    auto* returned = &camera.set_orthographic({
        .vertical_height = 4.0F,
        .near_plane = 2.0F,
        .far_plane = 10.0F,
    });
    CHECK(returned == &camera);
    REQUIRE(std::holds_alternative<vng::gfx::OrthographicLens>(camera.lens()));

    auto snapshot = camera.snapshot({800, 400});
    REQUIRE(snapshot.has_value());
    const auto corner = transform(
        snapshot->projection, {4.0F, 2.0F, -2.0F, 1.0F});
    const auto far = transform(
        snapshot->projection, {0.0F, 0.0F, -10.0F, 1.0F});
    check_vector(corner, {1.0F, 1.0F, -1.0F, 1.0F});
    CHECK(far.z == Approx(1.0F));
    CHECK(far.w == Approx(1.0F));
}

TEST_CASE("view_projection composes projection after view", "[gfx][camera]")
{
    vng::gfx::Camera camera;
    camera.set_position({4.0F, 3.0F, 2.0F})
        .look_at({-1.0F, 0.5F, -2.0F})
        .set_perspective({
            .vertical_fov = vng::degrees(55.0F),
            .near_plane = 0.2F,
            .far_plane = 500.0F,
        });

    auto snapshot = camera.snapshot({1920, 1080});
    REQUIRE(snapshot.has_value());
    const vng::Vec4 world_point{-3.0F, 2.0F, -8.0F, 1.0F};
    const auto combined = transform(snapshot->view_projection, world_point);
    const auto separate = transform(
        snapshot->projection,
        transform(snapshot->view, world_point));
    check_vector(combined, separate, 2.0e-5F);
}

TEST_CASE("snapshot rejects invalid extents without mutating the camera", "[gfx][camera][validation]")
{
    const vng::gfx::Camera camera;
    const auto before = camera.snapshot({320, 200});
    REQUIRE(before.has_value());

    for (const auto extent : {vng::Extent2D{0, 200}, vng::Extent2D{320, 0}}) {
        const auto invalid = camera.snapshot(extent);
        REQUIRE_FALSE(invalid.has_value());
        CHECK(invalid.error().code
              == vng::gfx::CameraDiagnosticCode::invalid_extent);
    }

    const auto after = camera.snapshot({320, 200});
    REQUIRE(after.has_value());
    CHECK(after->position == before->position);
    CHECK(after->forward == before->forward);
    CHECK(after->view == before->view);
    CHECK(after->projection == before->projection);
    CHECK(after->view_projection == before->view_projection);
}

TEST_CASE("snapshot diagnoses invalid and degenerate poses", "[gfx][camera][validation]")
{
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto infinity = std::numeric_limits<float>::infinity();

    SECTION("non-finite position") {
        vng::gfx::Camera camera;
        camera.set_position({nan, 0.0F, 0.0F});
        const auto result = camera.snapshot({10, 10});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code
              == vng::gfx::CameraDiagnosticCode::non_finite_pose);
    }
    SECTION("non-finite direction") {
        vng::gfx::Camera camera;
        camera.set_direction({0.0F, infinity, -1.0F});
        const auto result = camera.snapshot({10, 10});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code
              == vng::gfx::CameraDiagnosticCode::non_finite_pose);
    }
    SECTION("zero direction") {
        vng::gfx::Camera camera;
        camera.set_direction({});
        const auto result = camera.snapshot({10, 10});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code
              == vng::gfx::CameraDiagnosticCode::degenerate_direction);
    }
    SECTION("look_at the camera position") {
        vng::gfx::Camera camera;
        camera.set_position({1.0F, 2.0F, 3.0F})
            .look_at({1.0F, 2.0F, 3.0F});
        const auto result = camera.snapshot({10, 10});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code
              == vng::gfx::CameraDiagnosticCode::degenerate_direction);
    }
    SECTION("zero up hint") {
        vng::gfx::Camera camera;
        camera.set_direction({0.0F, 0.0F, -1.0F}, {});
        const auto result = camera.snapshot({10, 10});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code
              == vng::gfx::CameraDiagnosticCode::degenerate_up_hint);
    }
    SECTION("parallel up hint") {
        vng::gfx::Camera camera;
        camera.set_direction(
            {0.0F, 0.0F, -2.0F},
            {0.0F, 0.0F, 1.0F});
        const auto result = camera.snapshot({10, 10});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code
              == vng::gfx::CameraDiagnosticCode::parallel_direction_and_up);
    }
}

TEST_CASE("snapshot validates perspective lens parameters", "[gfx][camera][validation]")
{
    const auto check_invalid = [](vng::gfx::PerspectiveLens lens) {
        vng::gfx::Camera camera;
        camera.set_perspective(lens);
        const auto result = camera.snapshot({10, 10});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code
              == vng::gfx::CameraDiagnosticCode::invalid_perspective_lens);
    };

    check_invalid({.vertical_fov = vng::radians(0.0F)});
    check_invalid({.vertical_fov = vng::radians(std::numbers::pi_v<float>)});
    check_invalid({
        .vertical_fov = vng::radians(
            std::numeric_limits<float>::quiet_NaN()),
    });
    check_invalid({.near_plane = 0.0F});
    check_invalid({.near_plane = 4.0F, .far_plane = 4.0F});
    check_invalid({.far_plane = std::numeric_limits<float>::infinity()});
}

TEST_CASE("snapshot validates orthographic lens parameters", "[gfx][camera][validation]")
{
    const auto check_invalid = [](vng::gfx::OrthographicLens lens) {
        vng::gfx::Camera camera;
        camera.set_orthographic(lens);
        const auto result = camera.snapshot({10, 10});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code
              == vng::gfx::CameraDiagnosticCode::invalid_orthographic_lens);
    };

    check_invalid({.vertical_height = 0.0F});
    check_invalid({
        .vertical_height = std::numeric_limits<float>::quiet_NaN(),
    });
    check_invalid({.near_plane = 0.0F});
    check_invalid({.near_plane = 4.0F, .far_plane = 3.0F});
    check_invalid({.far_plane = std::numeric_limits<float>::infinity()});
}

TEST_CASE("snapshot reports finite state whose matrix cannot fit in Mat4", "[gfx][camera][validation]")
{
    vng::gfx::Camera camera;
    const auto largest = std::numeric_limits<float>::max();
    camera.set_position({largest, largest, largest})
        .set_direction({1.0F, 1.0F, 1.0F});

    const auto result = camera.snapshot({10, 10});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code
          == vng::gfx::CameraDiagnosticCode::unrepresentable_matrix);

    vng::gfx::Camera orthographic;
    orthographic.set_orthographic({
        .vertical_height = std::numeric_limits<float>::max(),
        .near_plane = 0.1F,
        .far_plane = 1000.0F,
    });
    const auto underflowed = orthographic.snapshot({
        std::numeric_limits<vng::u32>::max(),
        1,
    });
    REQUIRE_FALSE(underflowed.has_value());
    CHECK(underflowed.error().code
          == vng::gfx::CameraDiagnosticCode::unrepresentable_matrix);
}
