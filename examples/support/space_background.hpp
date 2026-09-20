#pragma once

#include <vng/gfx/geometry.hpp>
#include <vng/opengl/frame.hpp>
#include <vng/opengl/gpu_mesh.hpp>
#include <vng/opengl/program.hpp>
#include <vng/resources/resources.hpp>

namespace example::spaceflight {

struct ScreenPosition : vng::gfx::Semantic<vng::Vec2> {};
struct SpriteCoordinate : vng::gfx::Semantic<vng::Vec2> {};
using SkyVertex = vng::gfx::Record<ScreenPosition, SpriteCoordinate, vng::gfx::Color>;

// A static backdrop for this fixed-camera shot, not a general skybox system.
// One cached mesh draws both the faint dust band and anti-aliased star sprites.
class SpaceBackground final {
public:
    [[nodiscard]] static vng::resources::Result<SpaceBackground> create(
        vng::opengl::Device& device, vng::Extent2D extent, vng::u32 count, vng::u32 seed);
    [[nodiscard]] vng::resources::Result<void> resize(
        vng::opengl::Device& device, vng::Extent2D extent);
    [[nodiscard]] vng::resources::Result<void> render(vng::opengl::Frame& frame);
private:
    SpaceBackground(vng::opengl::Program program, vng::opengl::GpuMesh<SkyVertex> mesh,
        vng::Extent2D extent, vng::u32 count, vng::u32 seed)
        : program_(std::move(program)), mesh_(std::move(mesh)), extent_(extent), count_(count), seed_(seed) {}
    vng::opengl::Program program_;
    vng::opengl::GpuMesh<SkyVertex> mesh_;
    vng::Extent2D extent_;
    vng::u32 count_, seed_;
};

} // namespace example::spaceflight
