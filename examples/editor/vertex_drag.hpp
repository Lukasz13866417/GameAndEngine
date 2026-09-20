#pragma once

#include "project.hpp"

namespace editor_example {
// Captured projection basis, not a frozen copy of the scene or mesh.
struct VertexDragPlane {
    vng::Vec3 horizontal, vertical;
    [[nodiscard]] vng::Vec3 delta(vng::Vec2 pixels) const {
        return {horizontal.x * pixels.x + vertical.x * pixels.y,
                horizontal.y * pixels.x + vertical.y * pixels.y,
                horizontal.z * pixels.x + vertical.z * pixels.y};
    }
};
[[nodiscard]] std::optional<VertexDragPlane> vertex_drag_plane(const State&, vng::u32,
    vng::Vec2 viewport_size, const vng::gfx::Camera&);
// Convert a logical pixel drag to a local-space displacement on the camera
// plane through this vertex. The frozen state is the gesture's starting pose;
// callers apply the returned delta to the original vertex/selection positions.
// No copying, mutation, or revision bookkeeping occurs here.
[[nodiscard]] std::optional<vng::Vec3> vertex_drag_delta(const State& frozen, vng::u32 vertex,
                                                         vng::Vec2 logical_pixel_delta,
                                                         vng::Vec2 viewport_size, const vng::gfx::Camera* = nullptr);
} // namespace editor_example
