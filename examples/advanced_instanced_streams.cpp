// Advanced escape hatch: explicit stream resolution and VAO configuration.
#include "support/diagnostics.hpp"
#include "support/glfw_opengl_session.hpp"
#include "support/inspection.hpp"
#include "support/options.hpp"
#include "support/window_loop.hpp"

#include <vng/gfx/gfx.hpp>
#include <vng/glsl/glsl.hpp>
#include <vng/opengl/glsl_source.hpp>
#include <vng/opengl/opengl.hpp>
#include <vng/shader/shader.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <utility>

namespace {

struct Position : vng::gfx::Semantic<vng::Vec2> {};
struct Color : vng::gfx::Semantic<vng::Vec4> {};
struct InstanceOffset : vng::gfx::Semantic<vng::Vec2> {};

using Vertex = vng::gfx::Record<
    Position,
    vng::gfx::as<Color, vng::gfx::unorm8x4>>;
using Instance = vng::gfx::Record<InstanceOffset>;

using Layout = vng::gfx::VertexLayout<
    vng::gfx::Stream<Vertex, vng::gfx::PerVertex>,
    vng::gfx::Stream<Instance, vng::gfx::PerInstance<1>>>;

using VertexIn = vng::shader::VertexInputs<Position, Color, InstanceOffset>;
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

    auto vertex = vng::shader::vertex<VertexIn, VertexOut>(
        "triangle_vertex",
        [](auto& stage) {
            const auto position = stage.input(Position{});
            const auto offset = stage.input(InstanceOffset{});
            const auto color = stage.input(Color{});

            return stage.output(
                field<ClipPosition>(
                    vec4(position + offset, 0.0F, 1.0F)),
                field<Color>(color));
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
    auto generated = vng::glsl::emit(*shader_program);
    if (!generated) {
        return example::fail(generated.error());
    }

    const auto resolved = vng::gfx::resolve_vertex_input<VertexIn, Layout>();
    example::print_section(
        "resolved vertex layout", vng::gfx::dump_vertex_layout(resolved));
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
            .title = "Vibe Engine - advanced explicit instanced streams",
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

    auto vertex_shader = vng::opengl::Shader::compile(
        app->device(), vng::opengl::from_glsl(generated->vertex));
    if (!vertex_shader) {
        return example::fail(vertex_shader.error());
    }
    auto fragment_shader = vng::opengl::Shader::compile(
        app->device(), vng::opengl::from_glsl(generated->fragment));
    if (!fragment_shader) {
        return example::fail(fragment_shader.error());
    }
    auto program = vng::opengl::Program::link_graphics(
        app->device(), *vertex_shader, *fragment_shader);
    if (!program) {
        return example::fail(program.error());
    }

    vng::gfx::VertexStream<Vertex> vertices(3);
    vertices[0].set(Position{}, {-0.25F, -0.35F});
    vertices[0].set(Color{}, {1.0F, 0.15F, 0.08F, 1.0F});
    vertices[1].set(Position{}, {0.25F, -0.35F});
    vertices[1].set(Color{}, {0.08F, 0.85F, 0.25F, 1.0F});
    vertices[2].set(Position{}, {0.0F, 0.35F});
    vertices[2].set(Color{}, {0.15F, 0.30F, 1.0F, 1.0F});

    vng::gfx::VertexStream<Instance> instances(2);
    instances[0].set(InstanceOffset{}, {-0.34F, 0.0F});
    instances[1].set(InstanceOffset{}, {0.34F, 0.0F});

    auto vertex_buffer = vng::opengl::Buffer::from_bytes(
        app->device(), vertices.bytes());
    if (!vertex_buffer) {
        return example::fail(vertex_buffer.error());
    }
    auto instance_buffer = vng::opengl::Buffer::from_bytes(
        app->device(), instances.bytes());
    if (!instance_buffer) {
        return example::fail(instance_buffer.error());
    }
    auto vertex_array = vng::opengl::VertexArray::create(app->device());
    if (!vertex_array) {
        return example::fail(vertex_array.error());
    }

    const std::array stream_buffers{
        vng::opengl::ResolvedStreamBuffer{0, &*vertex_buffer, 0},
        vng::opengl::ResolvedStreamBuffer{1, &*instance_buffer, 0},
    };
    if (auto configured = vng::opengl::configure_vertex_input(
            *vertex_array, resolved, stream_buffers);
        !configured) {
        return example::fail(configured.error());
    }
    if (auto bound = program->bind(); !bound) {
        return example::fail(bound.error());
    }
    if (auto bound = vertex_array->bind(); !bound) {
        return example::fail(bound.error());
    }

    example::WindowLoop loop{app->window(), options->frame_limit};
    while (const auto extent = loop.next_extent()) {
        if (auto viewport = app->device().viewport(
                0,
                0,
                static_cast<std::int32_t>(extent->width),
                static_cast<std::int32_t>(extent->height));
            !viewport) {
            return example::fail(viewport.error());
        }
        if (auto cleared = app->device().clear_default_color(
                {0.018F, 0.024F, 0.045F, 1.0F});
            !cleared) {
            return example::fail(cleared.error());
        }
        if (auto drawn = app->device().draw_arrays_instanced(
                vng::opengl::Primitive::triangles, 0, 3, 2);
            !drawn) {
            return example::fail(drawn.error());
        }
        if (auto presented = loop.present(); !presented) {
            return example::fail(presented.error());
        }
    }
}
