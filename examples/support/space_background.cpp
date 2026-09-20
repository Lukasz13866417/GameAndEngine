#include "space_background.hpp"

#include <algorithm>
#include <cmath>
#include <vng/render/program.hpp>
#include <vng/shader/shader.hpp>

namespace example::spaceflight {
namespace {
using namespace vng;

gfx::Mesh<SkyVertex> make_background(Extent2D extent, u32 count, u32 seed)
{
    constexpr u32 columns = 48, rows = 32;
    gfx::Mesh<SkyVertex> mesh{4U * (columns * rows + count)};
    u32 vertex{};
    const auto quad = [&](Vec2 lo, Vec2 hi, const std::array<Vec4, 4>& colors, bool sprite) {
        const std::array points{lo, Vec2{hi.x, lo.y}, hi, Vec2{lo.x, hi.y}};
        const std::array uv{Vec2{-1, -1}, Vec2{1, -1}, Vec2{1, 1}, Vec2{-1, 1}};
        for (u32 i = 0; i < 4; ++i) {
            auto& out = mesh.vertices()[vertex + i];
            out.set(ScreenPosition{}, points[i]);
            out.set(SpriteCoordinate{}, sprite ? uv[i] : Vec2{});
            out.set(gfx::Color{}, colors[i]);
        }
        mesh.add_face(vertex, vertex + 1, vertex + 2);
        mesh.add_face(vertex, vertex + 2, vertex + 3);
        vertex += 4;
    };
    const auto dust = [](Vec2 p) -> Vec4 {
        const float band = p.y - .32F * p.x - .42F;
        const float cloud = std::exp(-band * band * 10.0F)
            * (.55F + .2F * std::sin(p.x * 8 + p.y * 7)
                + .12F * std::sin(p.x * 19 - p.y * 13));
        return {.0015F + cloud * .004F, .002F + cloud * .005F,
            .004F + cloud * .012F, 1};
    };
    for (u32 y = 0; y < rows; ++y) {
        for (u32 x = 0; x < columns; ++x) {
            const Vec2 lo{2.0F * static_cast<float>(x) / columns - 1,
                2.0F * static_cast<float>(y) / rows - 1};
            const Vec2 hi{lo.x + 2.0F / columns, lo.y + 2.0F / rows};
            quad(lo, hi, {dust(lo), dust({hi.x, lo.y}), dust(hi), dust({lo.x, hi.y})}, false);
        }
    }
    // Integer PRNG makes placements identical on repeated runs and resizes.
    u32 state = seed;
    const auto random = [&]() -> float {
        state = state * 1664525U + 1013904223U;
        return static_cast<float>(state >> 8U) / 16777216.0F;
    };
    for (u32 i = 0; i < count; ++i) {
        const Vec2 center{random() * 2 - 1, random() * 2 - 1};
        const auto intensity = random();
        const auto radius = .65F + intensity * intensity * 1.4F;
        const Vec2 half{radius * 2 / static_cast<float>(extent.width),
            radius * 2 / static_cast<float>(extent.height)};
        const float energy = .18F + intensity * intensity * 1.3F;
        const auto temperature = random();
        const Vec4 color{energy * (.7F + .3F * temperature),
            energy * (.8F + .15F * temperature), energy, 1};
        quad({center.x - half.x, center.y - half.y},
            {center.x + half.x, center.y + half.y}, {color, color, color, color}, true);
    }
    return mesh;
}

shader::Result<shader::GraphicsProgram> make_program()
{
    using Inputs = shader::VertexInputs<ScreenPosition, SpriteCoordinate, gfx::Color>;
    using Outputs = shader::VertexOutputs<shader::ClipPosition,
        shader::smooth<SpriteCoordinate>, shader::smooth<gfx::Color>>;
    using FragmentInputs = shader::FragmentInputs<shader::smooth<SpriteCoordinate>, shader::smooth<gfx::Color>>;
    using FragmentOutputs = shader::FragmentOutputs<shader::Color<0>>;
    auto vertex = shader::vertex<Inputs, Outputs>("space_background_vertex", [](auto& s) {
        return s.output(dsl::field<shader::ClipPosition>(dsl::vec4(s.input(ScreenPosition{}), 0.0F, 1.0F)),
            dsl::field<SpriteCoordinate>(s.input(SpriteCoordinate{})),
            dsl::field<gfx::Color>(s.input(gfx::Color{})));
    });
    if (!vertex) return std::unexpected(std::move(vertex.error()));
    auto fragment = shader::fragment<FragmentInputs, FragmentOutputs>("space_background_fragment", [](auto& s) {
        const auto uv = s.input(SpriteCoordinate{});
        const auto radial = dsl::max(1.0F - dsl::dot(uv, uv), s.constant(0.0F));
        const auto mask = radial * radial * radial;
        return s.output(dsl::field<shader::Color<0>>(s.input(gfx::Color{}) * mask));
    });
    if (!fragment) return std::unexpected(std::move(fragment.error()));
    return shader::link(std::move(*vertex), std::move(*fragment));
}
} // namespace

vng::resources::Result<SpaceBackground> SpaceBackground::create(
    vng::opengl::Device& device, vng::Extent2D extent, vng::u32 count, vng::u32 seed)
{
    using vng::resources::into_result;
    if (auto allowed = into_result(device.require_resource_update("SpaceBackground::create")); !allowed)
        return std::unexpected(std::move(allowed.error()));
    if (extent.empty() || count > 20000)
        return std::unexpected(vng::resources::Diagnostic{
            .code = vng::resources::ErrorCode::invalid_argument,
            .message = "Space backdrop needs a nonempty extent and at most 20000 stars"});
    auto shader = into_result(make_program());
    if (!shader) return std::unexpected(std::move(shader.error()));
    auto program = into_result(vng::render::compile_program(device, *shader));
    if (!program) return std::unexpected(std::move(program.error()));
    auto mesh = into_result(vng::opengl::upload_mesh(device, make_background(extent, count, seed)));
    if (!mesh) return std::unexpected(std::move(mesh.error()));
    if (auto prepared = into_result(mesh->prepare_vertex_input(device, *program)); !prepared)
        return std::unexpected(std::move(prepared.error()));
    return SpaceBackground{std::move(*program), std::move(*mesh), extent, count, seed};
}

vng::resources::Result<void> SpaceBackground::resize(vng::opengl::Device& device, vng::Extent2D extent)
{
    if (auto allowed = device.require_resource_update("SpaceBackground::resize"); !allowed)
        return std::unexpected(vng::resources::to_diagnostic(std::move(allowed.error())));
    if (extent.empty() || !program_.belongs_to(device))
        return std::unexpected(vng::resources::Diagnostic{
            .code = vng::resources::ErrorCode::invalid_argument, .message = "Invalid backdrop resize"});
    if (extent == extent_) return {};
    auto mesh = vng::resources::into_result(vng::opengl::upload_mesh(device, make_background(extent, count_, seed_)));
    if (!mesh) return std::unexpected(std::move(mesh.error()));
    if (auto prepared = mesh->prepare_vertex_input(device, program_); !prepared)
        return std::unexpected(vng::resources::to_diagnostic(std::move(prepared.error())));
    mesh_ = std::move(*mesh);
    extent_ = extent;
    return {};
}

vng::resources::Result<void> SpaceBackground::render(vng::opengl::Frame& frame)
{
    if (!frame.active() || frame.extent() != extent_)
        return std::unexpected(vng::resources::Diagnostic{
            .code = vng::resources::ErrorCode::invalid_argument, .message = "Backdrop/frame extent mismatch"});
    auto commands = frame.render_context();
    auto graphics = commands.graphics_state();
    if (auto selected = commands.run(program_); !selected)
        return std::unexpected(vng::resources::to_diagnostic(std::move(selected.error())));
    if (auto set = graphics.set(vng::render::DepthState{false, false}); !set)
        return std::unexpected(vng::resources::to_diagnostic(std::move(set.error())));
    if (auto set = graphics.set(vng::render::CullMode::none); !set)
        return std::unexpected(vng::resources::to_diagnostic(std::move(set.error())));
    if (auto set = graphics.set(vng::render::BlendMode::premultiplied_alpha); !set)
        return std::unexpected(vng::resources::to_diagnostic(std::move(set.error())));
    if (auto set = graphics.set(vng::opengl::PolygonMode::fill); !set)
        return std::unexpected(vng::resources::to_diagnostic(std::move(set.error())));
    return vng::resources::into_result(commands.draw(mesh_));
}

} // namespace example::spaceflight
