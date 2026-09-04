#pragma once

#include <expected>
#include <optional>
#include <utility>

#include <vng/core/types.hpp>
#include <vng/gfx/camera.hpp>

namespace vng::render {

// Immutable, backend-neutral inputs shared by every renderer participating in
// one view. It is a value snapshot rather than a borrowed Camera, so a queued
// renderer or future render graph cannot observe a camera changing underneath
// it.
class RenderView final {
public:
    [[nodiscard]] static std::expected<RenderView, gfx::CameraDiagnostic> create(
        const gfx::Camera& camera,
        Extent2D extent)
    {
        auto snapshot = camera.snapshot(extent);
        if (!snapshot) {
            return std::unexpected(std::move(snapshot.error()));
        }
        return RenderView{std::move(*snapshot)};
    }

    // Useful for renderers whose shaders do not consume camera parameters.
    // The target extent is still explicit because it is part of the view.
    [[nodiscard]] static RenderView without_camera(Extent2D extent) noexcept
    {
        return RenderView{extent};
    }

    [[nodiscard]] Extent2D extent() const noexcept { return extent_; }

    [[nodiscard]] const std::optional<gfx::CameraSnapshot>& camera() const noexcept
    {
        return camera_;
    }

private:
    explicit RenderView(Extent2D extent) noexcept : extent_(extent) {}

    explicit RenderView(gfx::CameraSnapshot camera) noexcept
        : extent_(camera.extent), camera_(std::move(camera))
    {}

    Extent2D extent_{};
    std::optional<gfx::CameraSnapshot> camera_;
};

} // namespace vng::render
