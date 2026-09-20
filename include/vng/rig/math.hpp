#pragma once

#include <expected>

#include <vng/core/angle.hpp>
#include <vng/core/types.hpp>
#include <vng/rig/diagnostic.hpp>

namespace vng::rig {

struct Quat final {
    f32 x{};
    f32 y{};
    f32 z{};
    f32 w{1.0F};
    friend constexpr bool operator==(const Quat&, const Quat&) = default;
};

// Only positive uniform scales are supported. This keeps the deformation and
// normal contracts explicit; arbitrary affine/shearing authoring is deferred.
struct Transform final {
    Vec3 translation{};
    Quat rotation{};
    f32 scale{1.0F};
    friend constexpr bool operator==(const Transform&, const Transform&) = default;
};

[[nodiscard]] std::expected<Quat, Diagnostic> rotation(Vec3 axis, Angle angle);
[[nodiscard]] inline std::expected<Quat, Diagnostic> rotation(Vec3 axis, f32 angle_radians)
{
    return rotation(axis, radians(angle_radians));
}

[[nodiscard]] std::expected<void, Diagnostic> validate(const Transform& transform);
// Column-major matrices multiplying column vectors. Call validate() first for
// an externally supplied Transform; validated asset and pose APIs do this for you.
[[nodiscard]] Mat4 matrix(const Transform& transform) noexcept;
[[nodiscard]] Mat4 multiply(const Mat4& left, const Mat4& right) noexcept;
[[nodiscard]] Vec3 transform_point(const Mat4& transform, Vec3 point) noexcept;
[[nodiscard]] Vec3 transform_vector(const Mat4& transform, Vec3 vector) noexcept;
[[nodiscard]] std::expected<Mat4, Diagnostic> inverse_affine(const Mat4& transform);
// Upper 3x3 is inverse-transpose; translation is zero. Apply to directions.
[[nodiscard]] std::expected<Mat4, Diagnostic> normal_matrix(const Mat4& transform);

} // namespace vng::rig
