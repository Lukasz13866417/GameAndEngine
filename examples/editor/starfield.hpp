#pragma once

#include <vng/gfx/geometry.hpp>
#include <vng/opengl/frame.hpp>
#include <vng/opengl/gpu_mesh.hpp>
#include <vng/render/program.hpp>
#include <vng/render/view.hpp>
#include <vng/resources/resources.hpp>
#include <vng/shader/shader.hpp>
#include <cmath>
#include <numbers>

namespace editor_example {
// Immutable world directions, not a camera-attached screen-space texture.
// Translation has no parallax; rotation/camera cuts reveal the correct stars.
// Pixel-sized sprites retain their size across viewport changes without a
// vertex upload. This deliberately omits nebulae and bright cinematic flares.
class Starfield final {
    using f32 = vng::f32;
    using u32 = vng::u32;
    struct Direction : vng::gfx::Semantic<vng::Vec3> {};
    struct Corner : vng::gfx::Semantic<vng::Vec2> {};
    struct Coordinate : vng::gfx::Semantic<vng::Vec2> {};
    using Vertex = vng::gfx::Record<Direction, Corner, Coordinate, vng::gfx::Color>;
    using Mesh = vng::gfx::Mesh<Vertex>;
    using Program = vng::opengl::TypedProgram<vng::Vec3, vng::Vec2>;
public:
    static vng::resources::Result<Starfield> create(vng::opengl::Device& device, u32 count, u32 seed) {
        using namespace vng;
        using resources::into_result;
        if (count > 20000)
            return std::unexpected(resources::Diagnostic{
                .code = resources::ErrorCode::invalid_argument,
                .message = "Editor starfield supports at most 20000 stars"});
        using VI = shader::VertexInputs<Direction, Corner, Coordinate, gfx::Color>;
        using VO = shader::VertexOutputs<shader::ClipPosition,
            shader::smooth<Coordinate>, shader::smooth<gfx::Color>>;
        using FI = shader::FragmentInputs<shader::smooth<Coordinate>, shader::smooth<gfx::Color>>;
        using FO = shader::FragmentOutputs<shader::Color<0>>;
        auto vertex = shader::vertex<VI, VO>("editor_starfield",
            [](auto& s, dsl::Float3 eye, dsl::Float2 pixel) {
                const auto clip = s.camera().project(eye + s.input(Direction{}) * 20.0F);
                const auto xy = clip.xy() + s.input(Corner{}) * pixel * clip.w();
                // Background depth, with no writes; scene geometry occludes it.
                return s.output(dsl::field<shader::ClipPosition>(dsl::vec4(xy, clip.w(), clip.w())),
                    dsl::field<Coordinate>(s.input(Coordinate{})),
                    dsl::field<gfx::Color>(s.input(gfx::Color{})));
            });
        if (!vertex) return std::unexpected(resources::to_diagnostic(vertex.error()));
        auto fragment = shader::fragment<FI, FO>("editor_starfield", [](auto& s) {
            const auto uv = s.input(Coordinate{});
            const auto radial = dsl::max(1.0F - dsl::dot(uv, uv), 0.0F);
            const auto mask = radial * radial;
            return s.output(dsl::field<shader::Color<0>>(s.input(gfx::Color{}) * mask));
        });
        if (!fragment) return std::unexpected(resources::to_diagnostic(fragment.error()));
        auto shader = into_result(shader::link(std::move(*vertex), std::move(*fragment)));
        if (!shader) return std::unexpected(shader.error());
        auto program = into_result(render::compile_program(device, std::move(*shader)));
        if (!program) return std::unexpected(program.error());
        Mesh mesh{static_cast<std::size_t>(count) * 4};
        u32 state = seed;
        const auto random = [&]() {
            state = state * 1664525U + 1013904223U;
            return static_cast<f32>(state >> 8) / 16777216.F;
        };
        const std::array corners{Vec2{-1,-1}, Vec2{1,-1}, Vec2{1,1}, Vec2{-1,1}};
        for (u32 i = 0; i < count; ++i) {
            const auto z = 2.F * random() - 1.F;
            const auto azimuth = 2.F * std::numbers::pi_v<f32> * random();
            const auto circle = std::sqrt(std::max(0.F, 1.F - z*z));
            const Vec3 direction{circle * std::cos(azimuth), z, circle * std::sin(azimuth)};
            const auto intensity = random();
            const auto radius = .6F + intensity * intensity * .8F;
            const auto energy = .08F + intensity * intensity * .65F;
            const auto temperature = random();
            const Vec4 color{energy * (.72F + .28F * temperature),
                energy * (.82F + .13F * temperature), energy, 1};
            for (u32 j = 0; j < 4; ++j) {
                auto& record = mesh.vertices()[i*4+j];
                record.set(Direction{}, direction);
                record.set(Corner{}, Vec2{corners[j].x * radius, corners[j].y * radius});
                record.set(Coordinate{}, corners[j]);
                record.set(gfx::Color{}, color);
            }
            mesh.add_face(i*4, i*4+1, i*4+2);
            mesh.add_face(i*4, i*4+2, i*4+3);
        }
        auto gpu = into_result(opengl::upload_mesh(device, mesh));
        if (!gpu) return std::unexpected(gpu.error());
        return Starfield{std::move(*program), std::move(*gpu)};
    }
    vng::resources::Result<void> render(vng::opengl::Frame& frame, const vng::render::RenderView& view) {
        using namespace vng;
        using resources::into_result;
        auto commands = frame.render_context();
        auto graphics = commands.graphics_state();
        const Vec2 pixel{2.F / static_cast<f32>(view.extent().width),
            2.F / static_cast<f32>(view.extent().height)};
        if (auto v = into_result(commands.run(program_, view.camera()->position, pixel)); !v) return v;
        if (auto v = into_result(commands.view(view)); !v) return v;
        if (auto v = into_result(graphics.set(render::DepthState{true, false, render::DepthCompare::less_equal})); !v) return v;
        if (auto v = into_result(graphics.set(render::CullMode::none)); !v) return v;
        if (auto v = into_result(graphics.set(render::BlendMode::premultiplied_alpha)); !v) return v;
        if (auto v = into_result(graphics.set(opengl::PolygonMode::fill)); !v) return v;
        return into_result(commands.draw(mesh_));
    }
private:
    Starfield(Program program, vng::opengl::GpuMesh<Vertex> mesh)
        : program_(std::move(program)), mesh_(std::move(mesh)) {}
    Program program_;
    vng::opengl::GpuMesh<Vertex> mesh_;
};
} // namespace editor_example
