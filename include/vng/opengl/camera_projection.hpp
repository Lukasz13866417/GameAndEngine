#pragma once
#include <concepts>
#include <type_traits>
#include <vng/gfx/camera.hpp>
#include <vng/render/depth_mapping.hpp>

namespace vng::opengl {
// OpenGL's zero-to-one clip mode preserves reversed floating-point depth.
// Rebuild Z directly from the lens: remapping a rounded canonical matrix loses
// the far-plane coefficient precisely where reversed depth is most useful.
[[nodiscard]] inline Mat4 camera_projection(
    const gfx::CameraSnapshot& camera, render::DepthMapping mapping)
{
    if (mapping == render::DepthMapping::standard) return camera.view_projection;
    auto projection = camera.projection;
    if (camera.lens) {
        std::visit([&](const auto& lens) {
            const double near = lens.near_plane, far = lens.far_plane, range = far - near;
            if constexpr (std::same_as<std::decay_t<decltype(lens)>, gfx::PerspectiveLens>) {
                projection[2][2] = static_cast<f32>(near / range);
                projection[3][2] = static_cast<f32>(near * far / range);
            } else {
                projection[2][2] = static_cast<f32>(1 / range);
                projection[3][2] = static_cast<f32>(far / range);
            }
        }, *camera.lens);
    } else {
        for (unsigned c = 0; c < 4; ++c)
            projection[c][2] = (camera.projection[c][3] - camera.projection[c][2]) * .5F;
    }
    Mat4 result{};
    for (unsigned c = 0; c < 4; ++c) for (unsigned r = 0; r < 4; ++r) {
        double value{};
        for (unsigned k = 0; k < 4; ++k)
            value += double(projection[k][r]) * camera.view[c][k];
        result[c][r] = static_cast<f32>(value);
    }
    return result;
}
} // namespace vng::opengl
