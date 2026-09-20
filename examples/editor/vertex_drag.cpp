#include "vertex_drag.hpp"
#include "animation.hpp"
#include "preview_values.hpp"
#include "../support/mesh_frame.hpp"

#include <cmath>
#include <limits>

namespace editor_example {
std::optional<VertexDragPlane> vertex_drag_plane(const State& state, vng::u32 vertex,
    vng::Vec2 size, const vng::gfx::Camera& camera) {
    const auto x = vertex_drag_delta(state, vertex, {1, 0}, size, &camera);
    const auto y = vertex_drag_delta(state, vertex, {0, 1}, size, &camera);
    if (!x || !y) return {};
    return VertexDragPlane{*x, *y};
}
std::optional<vng::Vec3> vertex_drag_delta(const State& frozen, vng::u32 vertex,
                                           vng::Vec2 logical_pixel_delta, vng::Vec2 viewport_size,
                                           const vng::gfx::Camera* presented) {
    using namespace vng;
    const auto finite = [](Vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); };
    const auto target = mesh_target(frozen);
    const auto* geometry = editable_mesh(frozen);
    if (!target || !geometry) return {};
    InstanceTransform instance_transform{};
    if (target->instance) {
        const auto* instance = find_instance(frozen, *target->instance);
        if (!instance) return {};
        const auto evaluated = preview_instance(frozen, *instance, frozen.viewport.time);
        if (!std::get<MeshSettings>(evaluated.settings).visible) return {};
        instance_transform = evaluated.transform;
    }
    // A blueprint has no instance transform, visibility or animation. Its
    // retained geometry remains editable even after every instance is deleted.
    const auto scale = instance_transform.scale;
    if (vertex >= geometry->size() ||
        !finite(logical_pixel_delta) || !finite(viewport_size) || viewport_size.x <= 0 ||
        viewport_size.y <= 0 || !std::isfinite(scale) || scale <= 0)
        return {};
    const auto view = presented ? *presented : camera(frozen);
    // Only the world-space camera basis is needed. Logical viewport dimensions
    // below set the pixel scale; neither framebuffer DPI nor integer rounding
    // should change mouse sensitivity.
    const auto snapshot = view.snapshot({1, 1});
    if (!snapshot)
        return {};
    const auto transform = mesh_transform(frozen,target->blueprint,instance_transform);
    const auto inverse=example::mesh_frame::inverse(transform);
    if(!inverse)return {};
    const auto position = geometry->position(vertex);
    f64 depth{};
    for (std::size_t row = 0; row < 3; ++row) {
        f64 world = transform[3][row];
        for (std::size_t column = 0; column < 3; ++column)
            world += static_cast<f64>(transform[column][row]) * position[column];
        depth += (world - snapshot->position[row]) * snapshot->forward[row];
    }
    const auto& lens = std::get<gfx::PerspectiveLens>(view.lens());
    if (!std::isfinite(depth) || depth < lens.near_plane || depth > lens.far_plane)
        return {};
    const auto units = 2 * depth * std::tan(camera_vertical_fov * std::numbers::pi / 360) /
                       viewport_size.y;
    Vec3 delta{};
    for (std::size_t i = 0; i < 3; ++i) {
        // Includes both instance placement and the unbaked blueprint transform.
        f64 value{};
        for (std::size_t row = 0; row < 3; ++row)
            value += (*inverse)[row][i] *
                     (static_cast<f64>(snapshot->right[row]) * logical_pixel_delta.x -
                      static_cast<f64>(snapshot->up[row]) * logical_pixel_delta.y) * units;
        if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<f32>::max())
            return {};
        delta[i] = static_cast<f32>(value);
    }
    return delta;
}
} // namespace editor_example
