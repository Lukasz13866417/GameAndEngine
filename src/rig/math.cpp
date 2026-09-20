#include <vng/rig/math.hpp>

#include <cmath>
#include <limits>

namespace vng::rig {
namespace {
bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool finite(const Mat4& m)
{
    for (const auto& c : m.columns) {
        for (std::size_t r = 0; r < 4; ++r) { if (!std::isfinite(c[r])) { return false; } }
    }
    return true;
}
Diagnostic invalid(std::string message) { return {ErrorCode::invalid_transform, std::move(message), {}}; }
}

std::expected<Quat, Diagnostic> rotation(Vec3 axis, Angle angle)
{
    const double norm = std::sqrt(static_cast<double>(axis.x) * axis.x
        + static_cast<double>(axis.y) * axis.y + static_cast<double>(axis.z) * axis.z);
    if (!finite(axis) || !(norm > 0) || !std::isfinite(angle.radians())) {
        return std::unexpected(invalid("rotation requires a finite nonzero axis and a finite angle"));
    }
    const double half = static_cast<double>(angle.radians()) * 0.5;
    const double factor = std::sin(half) / norm;
    return Quat{static_cast<f32>(axis.x * factor), static_cast<f32>(axis.y * factor),
        static_cast<f32>(axis.z * factor), static_cast<f32>(std::cos(half))};
}

std::expected<void, Diagnostic> validate(const Transform& transform)
{
    const auto q = transform.rotation;
    const double norm2 = static_cast<double>(q.x) * q.x + static_cast<double>(q.y) * q.y
        + static_cast<double>(q.z) * q.z + static_cast<double>(q.w) * q.w;
    if (!finite(transform.translation) || !std::isfinite(norm2) || std::abs(norm2 - 1.0) > 1e-4
        || !std::isfinite(transform.scale) || transform.scale <= 0) {
        return std::unexpected(invalid("transforms require finite translation, a unit quaternion, and positive uniform scale"));
    }
    if (!finite(matrix(transform))) {
        return std::unexpected(invalid("transform matrix exceeds the finite f32 range"));
    }
    return {};
}

Mat4 matrix(const Transform& t) noexcept
{
    const double x = t.rotation.x, y = t.rotation.y, z = t.rotation.z, w = t.rotation.w;
    // A tiny normalization avoids accumulating allowed input roundoff as scale.
    const double factor = 2.0 / (x*x + y*y + z*z + w*w);
    const double s = t.scale;
    Mat4 result = Mat4::identity();
    result[0] = {static_cast<f32>((1.0 - factor * (y*y + z*z)) * s),
        static_cast<f32>(factor * (x*y + w*z) * s), static_cast<f32>(factor * (x*z - w*y) * s), 0};
    result[1] = {static_cast<f32>(factor * (x*y - w*z) * s),
        static_cast<f32>((1.0 - factor * (x*x + z*z)) * s), static_cast<f32>(factor * (y*z + w*x) * s), 0};
    result[2] = {static_cast<f32>(factor * (x*z + w*y) * s),
        static_cast<f32>(factor * (y*z - w*x) * s), static_cast<f32>((1.0 - factor * (x*x + y*y)) * s), 0};
    result[3] = {t.translation.x, t.translation.y, t.translation.z, 1};
    return result;
}

Mat4 multiply(const Mat4& left, const Mat4& right) noexcept
{
    Mat4 result{};
    for (std::size_t c = 0; c < 4; ++c) {
        for (std::size_t r = 0; r < 4; ++r) {
            double value = 0;
            for (std::size_t k = 0; k < 4; ++k) { value += static_cast<double>(left[k][r]) * right[c][k]; }
            result[c][r] = static_cast<f32>(value);
        }
    }
    return result;
}

Vec3 transform_vector(const Mat4& m, Vec3 vector) noexcept
{
    Vec3 result;
    for (std::size_t r = 0; r < 3; ++r) {
        result[r] = static_cast<f32>(static_cast<double>(m[0][r]) * vector.x
            + static_cast<double>(m[1][r]) * vector.y + static_cast<double>(m[2][r]) * vector.z);
    }
    return result;
}

Vec3 transform_point(const Mat4& m, Vec3 point) noexcept
{
    Vec3 result;
    for (std::size_t r = 0; r < 3; ++r) {
        result[r] = static_cast<f32>(static_cast<double>(m[0][r]) * point.x
            + static_cast<double>(m[1][r]) * point.y + static_cast<double>(m[2][r]) * point.z + m[3][r]);
    }
    return result;
}

std::expected<Mat4, Diagnostic> inverse_affine(const Mat4& m)
{
    auto failure = [] { return std::unexpected(Diagnostic{ErrorCode::invalid_matrix,
        "matrix must be finite, affine, invertible, and have a representable inverse", {}}); };
    if (!finite(m) || m[0][3] != 0 || m[1][3] != 0 || m[2][3] != 0 || m[3][3] != 1) { return failure(); }
    const double a=m[0][0], b=m[1][0], c=m[2][0], d=m[0][1], e=m[1][1], f=m[2][1];
    const double g=m[0][2], h=m[1][2], i=m[2][2];
    const double det = a*(e*i-f*h) - b*(d*i-f*g) + c*(d*h-e*g);
    if (det == 0 || !std::isfinite(det)) { return failure(); }
    Mat4 result = Mat4::identity();
    const double rows[3][3] = {{(e*i-f*h)/det, (c*h-b*i)/det, (b*f-c*e)/det},
        {(f*g-d*i)/det, (a*i-c*g)/det, (c*d-a*f)/det},
        {(d*h-e*g)/det, (b*g-a*h)/det, (a*e-b*d)/det}};
    for (std::size_t r=0; r<3; ++r) {
        for (std::size_t col=0; col<3; ++col) { result[col][r]=static_cast<f32>(rows[r][col]); }
        result[3][r]=static_cast<f32>(-(rows[r][0]*m[3][0]+rows[r][1]*m[3][1]+rows[r][2]*m[3][2]));
    }
    if (!finite(result)) { return failure(); }
    return result;
}

std::expected<Mat4, Diagnostic> normal_matrix(const Mat4& transform)
{
    auto inverse = inverse_affine(transform);
    if (!inverse) { return std::unexpected(inverse.error()); }
    Mat4 result = Mat4::identity();
    for (std::size_t c = 0; c < 3; ++c) {
        for (std::size_t r = 0; r < 3; ++r) { result[c][r] = (*inverse)[r][c]; }
    }
    return result;
}

} // namespace vng::rig
