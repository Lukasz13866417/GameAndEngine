#pragma once

#include <memory>
#include <optional>
#include <span>

#include <vng/analysis/analysis.hpp>
#include <vng/gfx/geometry.hpp>
#include <vng/gfx/mesh.hpp>
#include <vng/opengl/device.hpp>
#include <vng/opengl/frame.hpp>
#include <vng/opengl/renderer.hpp>
#include <vng/resources/diagnostic.hpp>

namespace example::spaceflight {

struct Emission : vng::gfx::Semantic<vng::f32> {};
struct WorldPosition : vng::gfx::Semantic<vng::Vec3> {};
struct WorldNormal : vng::gfx::Semantic<vng::Vec3> {};
struct Radiance : vng::gfx::Semantic<vng::Vec4> {};

using Vertex = vng::gfx::Record<vng::gfx::Position, vng::gfx::Normal,
    vng::gfx::Color, Emission>;
using Mesh = vng::gfx::Mesh<Vertex>;

// This example's shader accepts rigid object transforms. The ship is authored
// at its final scale, so normals need only the transform's rotation.
struct ShipDraw final {
    vng::Mat4 transform{vng::Mat4::identity()};
    // If absent, use the original demo's distant directional key light.
    std::optional<vng::Vec3> light_position{};
    vng::Vec3 light_color{1.25F, 1.14F, 0.96F};
};

class ShipRenderer final : public vng::opengl::Renderer<ShipDraw> {
public:
    [[nodiscard]] static vng::resources::Result<ShipRenderer> create(
        vng::opengl::Device& device, Mesh mesh);

    ShipRenderer(ShipRenderer&&) noexcept;
    ShipRenderer& operator=(ShipRenderer&&) noexcept;
    ShipRenderer(const ShipRenderer&) = delete;
    ShipRenderer& operator=(const ShipRenderer&) = delete;
    ~ShipRenderer();

    [[nodiscard]] vng::resources::Result<void> render(
        vng::opengl::Frame& frame, const vng::render::RenderView& view,
        std::span<const ShipDraw> draws);
    [[nodiscard]] vng::resources::Result<void> render(
        vng::opengl::Frame& frame, const vng::render::RenderView& view,
        const ShipDraw& draw);

    // Enhanced shaders are emitted lazily from exactly the ordinary shader IR.
    // Radiance preserves HDR values; the canonical diagnostic color is RGBA8
    // and deliberately does not include the later bloom/tone-mapping pass.
    [[nodiscard]] vng::resources::Result<vng::analysis::DiagnosticSweep> diagnose(
        vng::opengl::Frame& frame, const vng::render::RenderView& view,
        const ShipDraw& draw,
        const vng::analysis::CaptureRequest& request =
            vng::analysis::CaptureRequest::diagnostic());

    [[nodiscard]] const Mesh& source_mesh() const;

private:
    struct Impl;
    explicit ShipRenderer(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace example::spaceflight
