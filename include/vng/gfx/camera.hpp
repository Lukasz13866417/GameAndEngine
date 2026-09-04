#pragma once

#include <cmath>
#include <cstddef>
#include <expected>
#include <numbers>
#include <string>
#include <utility>
#include <variant>

#include <vng/core/angle.hpp>
#include <vng/core/types.hpp>

namespace vng::gfx {

struct PerspectiveLens final {
    Angle vertical_fov{degrees(60.0F)};
    f32 near_plane{0.1F};
    f32 far_plane{1000.0F};

    friend constexpr bool operator==(const PerspectiveLens&, const PerspectiveLens&) = default;
};

struct OrthographicLens final {
    f32 vertical_height{10.0F};
    f32 near_plane{0.1F};
    f32 far_plane{1000.0F};

    friend constexpr bool operator==(const OrthographicLens&, const OrthographicLens&) = default;
};

using CameraLens = std::variant<PerspectiveLens, OrthographicLens>;

enum class CameraDiagnosticCode {
    invalid_extent,
    non_finite_pose,
    degenerate_direction,
    degenerate_up_hint,
    parallel_direction_and_up,
    invalid_perspective_lens,
    invalid_orthographic_lens,
    unrepresentable_matrix,
};

struct CameraDiagnostic final {
    CameraDiagnosticCode code{CameraDiagnosticCode::non_finite_pose};
    std::string message{};
};

// A snapshot is the complete camera input for one render extent.
// Matrices are column-major and transform column vectors. The engine's
// canonical view is right-handed: forward becomes view-space -Z, +Y is up,
// and projection depth covers [-1, +1]. Backends with another clip convention
// adapt this value at their boundary rather than changing Camera.
struct CameraSnapshot final {
    Vec3 position{};
    Vec3 right{1.0F, 0.0F, 0.0F};
    Vec3 up{0.0F, 1.0F, 0.0F};
    Vec3 forward{0.0F, 0.0F, -1.0F};
    Extent2D extent{};
    f32 aspect_ratio{1.0F};
    Mat4 view{Mat4::identity()};
    Mat4 projection{Mat4::identity()};
    Mat4 view_projection{Mat4::identity()};

    friend constexpr bool operator==(const CameraSnapshot&, const CameraSnapshot&) = default;
};

namespace camera_detail {

[[nodiscard]] inline bool finite(Vec3 value) noexcept
{
    return std::isfinite(value.x)
        && std::isfinite(value.y)
        && std::isfinite(value.z);
}

[[nodiscard]] inline f64 length_squared(Vec3 value) noexcept
{
    const auto x = static_cast<f64>(value.x);
    const auto y = static_cast<f64>(value.y);
    const auto z = static_cast<f64>(value.z);
    return (x * x) + (y * y) + (z * z);
}

[[nodiscard]] inline Vec3 normalize(Vec3 value, f64 squared_length) noexcept
{
    const auto inverse_length = 1.0 / std::sqrt(squared_length);
    return {
        static_cast<f32>(static_cast<f64>(value.x) * inverse_length),
        static_cast<f32>(static_cast<f64>(value.y) * inverse_length),
        static_cast<f32>(static_cast<f64>(value.z) * inverse_length),
    };
}

[[nodiscard]] inline Vec3 cross(Vec3 left, Vec3 right) noexcept
{
    return {
        (left.y * right.z) - (left.z * right.y),
        (left.z * right.x) - (left.x * right.z),
        (left.x * right.y) - (left.y * right.x),
    };
}

[[nodiscard]] inline f64 dot(Vec3 left, Vec3 right) noexcept
{
    return (static_cast<f64>(left.x) * static_cast<f64>(right.x))
        + (static_cast<f64>(left.y) * static_cast<f64>(right.y))
        + (static_cast<f64>(left.z) * static_cast<f64>(right.z));
}

[[nodiscard]] inline bool finite(const Mat4& value) noexcept
{
    for (const auto& column : value.columns) {
        for (std::size_t row = 0; row < 4; ++row) {
            if (!std::isfinite(column[row])) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] inline Mat4 multiply(const Mat4& left, const Mat4& right) noexcept
{
    Mat4 result{};
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            f64 value = 0.0;
            for (std::size_t index = 0; index < 4; ++index) {
                value += static_cast<f64>(left[index][row])
                    * static_cast<f64>(right[column][index]);
            }
            result[column][row] = static_cast<f32>(value);
        }
    }
    return result;
}

[[nodiscard]] inline Mat4 make_view(
    Vec3 position,
    Vec3 right,
    Vec3 up,
    Vec3 forward) noexcept
{
    Mat4 result{};
    result.columns = {
        Vec4{right.x, up.x, -forward.x, 0.0F},
        Vec4{right.y, up.y, -forward.y, 0.0F},
        Vec4{right.z, up.z, -forward.z, 0.0F},
        Vec4{
            static_cast<f32>(-dot(right, position)),
            static_cast<f32>(-dot(up, position)),
            static_cast<f32>(dot(forward, position)),
            1.0F,
        },
    };
    return result;
}

[[nodiscard]] inline std::expected<Mat4, CameraDiagnostic>
make_projection(const PerspectiveLens& lens, f32 aspect_ratio)
{
    const auto fov = lens.vertical_fov.radians();
    if (!std::isfinite(fov)
        || fov <= 0.0F
        || fov >= std::numbers::pi_v<f32>
        || !std::isfinite(lens.near_plane)
        || !std::isfinite(lens.far_plane)
        || lens.near_plane <= 0.0F
        || lens.far_plane <= lens.near_plane) {
        return std::unexpected(CameraDiagnostic{
            .code = CameraDiagnosticCode::invalid_perspective_lens,
            .message = "a perspective lens requires a finite vertical FOV "
                "in (0, pi) and finite 0 < near < far planes",
        });
    }

    const auto focal_length = 1.0 / std::tan(static_cast<f64>(fov) * 0.5);
    const auto near_plane = static_cast<f64>(lens.near_plane);
    const auto far_plane = static_cast<f64>(lens.far_plane);
    const auto depth = far_plane - near_plane;

    Mat4 result{};
    result[0][0] = static_cast<f32>(focal_length / aspect_ratio);
    result[1][1] = static_cast<f32>(focal_length);
    result[2][2] = static_cast<f32>(-(far_plane + near_plane) / depth);
    result[2][3] = -1.0F;
    result[3][2] = static_cast<f32>(-(2.0 * far_plane * near_plane) / depth);

    if (!finite(result)
        || result[0][0] == 0.0F
        || result[1][1] == 0.0F
        || result[2][2] == 0.0F) {
        return std::unexpected(CameraDiagnostic{
            .code = CameraDiagnosticCode::unrepresentable_matrix,
            .message = "the perspective lens and viewport produce a matrix "
                "that cannot be represented by Mat4",
        });
    }
    return result;
}

[[nodiscard]] inline std::expected<Mat4, CameraDiagnostic>
make_projection(const OrthographicLens& lens, f32 aspect_ratio)
{
    if (!std::isfinite(lens.vertical_height)
        || !std::isfinite(lens.near_plane)
        || !std::isfinite(lens.far_plane)
        || lens.vertical_height <= 0.0F
        || lens.near_plane <= 0.0F
        || lens.far_plane <= lens.near_plane) {
        return std::unexpected(CameraDiagnostic{
            .code = CameraDiagnosticCode::invalid_orthographic_lens,
            .message = "an orthographic lens requires a finite positive height "
                "and finite 0 < near < far planes",
        });
    }

    const auto width = static_cast<f64>(lens.vertical_height)
        * static_cast<f64>(aspect_ratio);
    const auto depth = static_cast<f64>(lens.far_plane)
        - static_cast<f64>(lens.near_plane);

    Mat4 result{};
    result[0][0] = static_cast<f32>(2.0 / width);
    result[1][1] = static_cast<f32>(2.0 / lens.vertical_height);
    result[2][2] = static_cast<f32>(-2.0 / depth);
    result[3][2] = static_cast<f32>(
        -(static_cast<f64>(lens.far_plane)
          + static_cast<f64>(lens.near_plane)) / depth);
    result[3][3] = 1.0F;

    if (!(width > 0.0)
        || !std::isfinite(width)
        || !finite(result)
        || result[0][0] == 0.0F
        || result[1][1] == 0.0F
        || result[2][2] == 0.0F) {
        return std::unexpected(CameraDiagnostic{
            .code = CameraDiagnosticCode::unrepresentable_matrix,
            .message = "the orthographic lens and viewport produce a matrix "
                "that cannot be represented by Mat4",
        });
    }
    return result;
}

} // namespace camera_detail

// Camera retains only backend-neutral authored state. It deliberately does
// not retain a viewport, window, GPU buffer, or a live look-at target.
class Camera final {
public:
    Camera() = default;

    [[nodiscard]] const Vec3& position() const noexcept { return position_; }
    [[nodiscard]] const Vec3& direction() const noexcept { return direction_; }
    [[nodiscard]] const Vec3& up_hint() const noexcept { return up_hint_; }
    [[nodiscard]] const CameraLens& lens() const noexcept { return lens_; }

    Camera& set_position(Vec3 position) noexcept
    {
        position_ = position;
        return *this;
    }

    Camera& move_by(Vec3 offset) noexcept
    {
        position_.x += offset.x;
        position_.y += offset.y;
        position_.z += offset.z;
        return *this;
    }

    Camera& look_at(
        Vec3 target,
        Vec3 up_hint = {0.0F, 1.0F, 0.0F}) noexcept
    {
        direction_ = {
            target.x - position_.x,
            target.y - position_.y,
            target.z - position_.z,
        };
        up_hint_ = up_hint;
        return *this;
    }

    Camera& set_direction(
        Vec3 direction,
        Vec3 up_hint = {0.0F, 1.0F, 0.0F}) noexcept
    {
        direction_ = direction;
        up_hint_ = up_hint;
        return *this;
    }

    Camera& set_perspective(PerspectiveLens lens) noexcept
    {
        lens_ = lens;
        return *this;
    }

    Camera& set_orthographic(OrthographicLens lens) noexcept
    {
        lens_ = lens;
        return *this;
    }

    [[nodiscard]] std::expected<CameraSnapshot, CameraDiagnostic> snapshot(
        Extent2D extent) const
    {
        if (extent.empty()) {
            return std::unexpected(CameraDiagnostic{
                .code = CameraDiagnosticCode::invalid_extent,
                .message = "Camera::snapshot requires a non-empty extent",
            });
        }
        if (!camera_detail::finite(position_)
            || !camera_detail::finite(direction_)
            || !camera_detail::finite(up_hint_)) {
            return std::unexpected(CameraDiagnostic{
                .code = CameraDiagnosticCode::non_finite_pose,
                .message = "camera position, direction, and up hint must be finite",
            });
        }

        const auto direction_length = camera_detail::length_squared(direction_);
        if (!(direction_length > 0.0) || !std::isfinite(direction_length)) {
            return std::unexpected(CameraDiagnostic{
                .code = CameraDiagnosticCode::degenerate_direction,
                .message = "camera direction must be non-zero",
            });
        }
        const auto up_length = camera_detail::length_squared(up_hint_);
        if (!(up_length > 0.0) || !std::isfinite(up_length)) {
            return std::unexpected(CameraDiagnostic{
                .code = CameraDiagnosticCode::degenerate_up_hint,
                .message = "camera up hint must be non-zero",
            });
        }

        const auto forward = camera_detail::normalize(
            direction_, direction_length);
        const auto normalized_up_hint = camera_detail::normalize(
            up_hint_, up_length);
        const auto unnormalized_right = camera_detail::cross(
            forward, normalized_up_hint);
        const auto right_length = camera_detail::length_squared(
            unnormalized_right);
        constexpr f64 minimum_basis_length_squared = 1.0e-12;
        if (!(right_length > minimum_basis_length_squared)
            || !std::isfinite(right_length)) {
            return std::unexpected(CameraDiagnostic{
                .code = CameraDiagnosticCode::parallel_direction_and_up,
                .message = "camera direction and up hint must not be parallel",
            });
        }
        const auto right = camera_detail::normalize(
            unnormalized_right, right_length);
        const auto unnormalized_up = camera_detail::cross(right, forward);
        const auto corrected_up_length = camera_detail::length_squared(
            unnormalized_up);
        const auto up = camera_detail::normalize(
            unnormalized_up, corrected_up_length);

        const auto aspect_ratio = static_cast<f32>(extent.width)
            / static_cast<f32>(extent.height);
        if (!(aspect_ratio > 0.0F) || !std::isfinite(aspect_ratio)) {
            return std::unexpected(CameraDiagnostic{
                .code = CameraDiagnosticCode::invalid_extent,
                .message = "Camera::snapshot extent produces an invalid aspect ratio",
            });
        }

        auto projection = std::visit(
            [aspect_ratio](const auto& lens) {
                return camera_detail::make_projection(lens, aspect_ratio);
            },
            lens_);
        if (!projection) {
            return std::unexpected(std::move(projection.error()));
        }

        const auto view = camera_detail::make_view(
            position_, right, up, forward);
        const auto view_projection = camera_detail::multiply(*projection, view);
        if (!camera_detail::finite(view)
            || !camera_detail::finite(view_projection)) {
            return std::unexpected(CameraDiagnostic{
                .code = CameraDiagnosticCode::unrepresentable_matrix,
                .message = "the camera pose produces a matrix that cannot be represented by Mat4",
            });
        }

        return CameraSnapshot{
            .position = position_,
            .right = right,
            .up = up,
            .forward = forward,
            .extent = extent,
            .aspect_ratio = aspect_ratio,
            .view = view,
            .projection = *projection,
            .view_projection = view_projection,
        };
    }

private:
    Vec3 position_{};
    Vec3 direction_{0.0F, 0.0F, -1.0F};
    Vec3 up_hint_{0.0F, 1.0F, 0.0F};
    CameraLens lens_{PerspectiveLens{}};
};

} // namespace vng::gfx
