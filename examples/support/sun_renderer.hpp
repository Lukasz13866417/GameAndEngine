#pragma once

#include "sun_assets.hpp"

#include <memory>
#include <span>
#include <vng/analysis/analysis.hpp>
#include <vng/opengl/frame.hpp>
#include <vng/opengl/renderer.hpp>
#include <vng/resources/diagnostic.hpp>

namespace example::sun {

struct WorldPosition : vng::gfx::Semantic<vng::Vec3> {};
struct SurfaceNormal : vng::gfx::Semantic<vng::Vec3> {};
struct Radiance : vng::gfx::Semantic<vng::Vec4> {};

struct SunDraw final {
    // Drives local animation and a shared ten-minute surface/arc revolution.
    vng::f32 time{};
    vng::f32 displacement{1.0F};
    // Only white-hot surface emission; independent of arcs, corona and bloom.
    bool white_spots{false};
    vng::Vec3 position{};
    vng::f32 radius{1.0F};
    // Instance orientation, composed before the sun's own animated rotation.
    vng::Mat3 orientation{vng::Mat3::identity()};
    // Positive local-axis stretch, applied after the effect's animated spin.
    vng::Vec3 axis_scale{1,1,1};
};

// Example-specific rendering policy: resources and both stages of each
// shader belong here. A draw changes values, never geometry or shader code.
class SunRenderer final : public vng::opengl::Renderer<SunDraw> {
public:
    [[nodiscard]] static vng::resources::Result<SunRenderer> create(vng::opengl::Device&);
    SunRenderer(SunRenderer&&) noexcept;
    SunRenderer& operator=(SunRenderer&&) noexcept;
    SunRenderer(const SunRenderer&) = delete;
    SunRenderer& operator=(const SunRenderer&) = delete;
    ~SunRenderer();

    [[nodiscard]] vng::resources::Result<void> render(vng::opengl::Frame&,
        const vng::render::RenderView&, const SunDraw&);
    [[nodiscard]] vng::resources::Result<void> render(vng::opengl::Frame&,
        const vng::render::RenderView&, std::span<const SunDraw>);
    // Isolates the displaced surface before corona, loops and bloom. Color
    // is diagnostic RGBA8; Radiance preserves the actual linear HDR values.
    [[nodiscard]] vng::resources::Result<vng::analysis::DiagnosticSweep> diagnose(
        vng::opengl::Frame&, const vng::render::RenderView&, const SunDraw&,
        const vng::analysis::CaptureRequest& = vng::analysis::CaptureRequest::diagnostic());
    [[nodiscard]] const SunMesh& source_mesh() const;

private:
    struct Impl;
    explicit SunRenderer(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace example::sun
