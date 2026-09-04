#include "support/diagnostics.hpp"
#include "support/glfw_opengl_session.hpp"
#include "support/inspection.hpp"
#include "support/options.hpp"
#include "support/window_loop.hpp"

#include <vng/content/content.hpp>
#include <vng/gfx/gfx.hpp>
#include <vng/glsl/glsl.hpp>
#include <vng/render/opengl.hpp>
#include <vng/shader/shader.hpp>

#include <array>
#include <filesystem>
#include <iostream>
#include <utility>

#ifndef VNG_EXAMPLE_TRIANGLE_MESH_PATH
#define VNG_EXAMPLE_TRIANGLE_MESH_PATH "examples/assets/colored_triangle.vmesh"
#endif

namespace {

struct Position : vng::gfx::Semantic<vng::Vec2> {};
struct Color : vng::gfx::Semantic<vng::Vec4> {};

using Vertex = vng::gfx::Record<
    Position,
    vng::gfx::as<Color, vng::gfx::unorm8x4>>;

using VertexIn = vng::shader::VertexInputs<Position, Color>;
using VertexOut = vng::shader::VertexOutputs<
    vng::shader::ClipPosition,
    vng::shader::smooth<Color>>;
using FragmentIn = vng::shader::FragmentInputs<vng::shader::smooth<Color>>;
using FragmentOut = vng::shader::FragmentOutputs<vng::shader::Color<0>>;

using vng::dsl::field;
using vng::dsl::vec4;
using vng::shader::ClipPosition;

} // namespace

int main(int argc, char** argv)
{
    auto options = example::parse_frame_options(argc, argv);
    if (!options) {
        return example::fail(options.error());
    }

    namespace vmesh = vng::content::vmesh;
    auto vertex_schema = vmesh::schema<Vertex>();
    vertex_schema.map("position", Position{});
    vertex_schema.map("color/0", Color{});

    const std::filesystem::path mesh_path{VNG_EXAMPLE_TRIANGLE_MESH_PATH};
    auto cpu_mesh = vmesh::load(mesh_path, vertex_schema);
    if (!cpu_mesh) {
        return example::fail(cpu_mesh.error());
    }
    std::cout << "loaded: " << mesh_path << '\n';

    auto vertex = vng::shader::vertex<VertexIn, VertexOut>(
        "triangle_vertex",
        [](auto& stage) {
            return stage.output(
                field<ClipPosition>(
                    vec4(stage.input(Position{}), 0.0F, 1.0F)),
                field<Color>(stage.input(Color{})));
        });
    if (!vertex) {
        return example::fail(vertex.error());
    }

    auto fragment = vng::shader::fragment<FragmentIn, FragmentOut>(
        "triangle_fragment",
        [](auto& stage) {
            return stage.output(
                field<vng::shader::Color<0>>(stage.input(Color{})));
        });
    if (!fragment) {
        return example::fail(fragment.error());
    }

    auto shader_program = vng::shader::link(
        std::move(*vertex), std::move(*fragment));
    if (!shader_program) {
        return example::fail(shader_program.error());
    }

    // Inspection is optional and backend-free. The renderer compiles the same
    // neutral program for OpenGL below.
    auto generated = vng::glsl::emit(*shader_program);
    if (!generated) {
        return example::fail(generated.error());
    }
    example::print_section(
        "vertex interface", shader_program->vertex().dump_interface());
    example::print_section(
        "fragment interface", shader_program->fragment().dump_interface());
    example::print_section("vertex IR", shader_program->vertex().dump_ir());
    example::print_section("fragment IR", shader_program->fragment().dump_ir());
    example::print_section("vertex GLSL 4.60", generated->vertex.source);
    example::print_section("fragment GLSL 4.60", generated->fragment.source);
    std::cout << std::flush;

    auto app = example::GlfwOpenGLSession::create(
        vng::window::WindowDesc{
            .width = 960,
            .height = 540,
            .title = "Vibe Engine - renderer-backed vmesh triangle",
        },
        vng::opengl::ContextDesc{
            .debug = true,
            .samples = 0,
            .default_framebuffer_encoding =
                vng::render::ColorEncoding::linear,
            .swap_interval = 1,
        });
    if (!app) {
        return example::fail(app.error());
    }

    // This convenience owns one mesh and one pipeline. See
    // file_mesh_renderer.cpp for a Renderer<Ticket> with its own shaders and
    // command policy, or file_mesh_direct.cpp for the renderer-free path.
    auto renderer = vng::render::make_simple_mesh_renderer(
        app->device(),
        std::move(*shader_program),
        std::move(*cpu_mesh),
        vng::render::GraphicsPipelineDesc{
            .output_encoding = vng::render::ColorEncoding::linear,
        });
    if (!renderer) {
        return example::fail(renderer.error());
    }
    const vng::render::MeshDraw triangle;

    example::WindowLoop loop{app->window(), options->frame_limit};
    while (const auto extent = loop.next_extent()) {
        auto frame = vng::render::begin_frame(
            app->device(),
            vng::render::FrameDesc{
                .extent = *extent,
                .color_encoding = vng::render::ColorEncoding::linear,
                .clear_color = std::array<vng::f32, 4>{
                    0.018F, 0.024F, 0.045F, 1.0F},
                .clear_depth = std::nullopt,
            });
        if (!frame) {
            return example::fail(frame.error());
        }

        const auto view = vng::render::RenderView::without_camera(*extent);
        if (auto drawn = vng::render::render_one(
                *renderer, *frame, view, triangle);
            !drawn) {
            return example::fail(drawn.error());
        }
        if (auto ended = frame->end(); !ended) {
            return example::fail(ended.error());
        }
        if (auto presented = loop.present(); !presented) {
            return example::fail(presented.error());
        }
    }
}
