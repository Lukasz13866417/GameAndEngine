#include "file_mesh_types.hpp"
#include "support/diagnostics.hpp"
#include "support/glfw_opengl_session.hpp"
#include "support/options.hpp"
#include "support/window_loop.hpp"

#include <vng/content/content.hpp>
#include <vng/gfx/gfx.hpp>
#include <vng/opengl/opengl.hpp>
#include <vng/render/opengl.hpp>
#include <vng/shader/shader.hpp>

#include <array>
#include <filesystem>
#include <iostream>
#include <utility>

#ifndef VNG_EXAMPLE_MESH_PATH
#define VNG_EXAMPLE_MESH_PATH "examples/assets/colored_cube.vmesh"
#endif

namespace {

using namespace file_mesh_example;

using VertexIn = vng::shader::VertexInputs<Position, Color>;
using VertexOut = vng::shader::VertexOutputs<
    vng::shader::ClipPosition,
    vng::shader::smooth<Color>>;
using FragmentIn = vng::shader::FragmentInputs<vng::shader::smooth<Color>>;
using FragmentOut = vng::shader::FragmentOutputs<vng::shader::Color<0>>;

using vng::dsl::field;

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

    const std::filesystem::path mesh_path{VNG_EXAMPLE_MESH_PATH};
    auto cpu_mesh = vmesh::load(mesh_path, vertex_schema);
    if (!cpu_mesh) {
        return example::fail(cpu_mesh.error());
    }
    std::cout << "loaded: " << mesh_path << '\n';

    // This demo owns the two shader stages directly. The renderer-backed
    // example deliberately keeps this same work inside FileMeshRenderer.
    auto vertex = vng::shader::vertex<VertexIn, VertexOut>(
        "direct_file_mesh_vertex",
        [](auto& stage) {
            return stage.output(
                field<vng::shader::ClipPosition>(
                    stage.camera().project(stage.input(Position{}))),
                field<Color>(stage.input(Color{})));
        });
    if (!vertex) {
        return example::fail(vertex.error());
    }

    auto fragment = vng::shader::fragment<FragmentIn, FragmentOut>(
        "direct_file_mesh_fragment",
        [](auto& stage) {
            const auto color = stage.input(Color{});
            stage.observe(SurfaceColor{}, color);
            return stage.output(
                field<vng::shader::Color<0>>(color));
        });
    if (!fragment) {
        return example::fail(fragment.error());
    }

    auto shader_program = vng::shader::link(
        std::move(*vertex), std::move(*fragment));
    if (!shader_program) {
        return example::fail(shader_program.error());
    }

    auto app = example::GlfwOpenGLSession::create(
        vng::window::WindowDesc{
            .width = 700,
            .height = 700,
            .title = "Vibe Engine - direct file mesh",
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

    const vng::render::GraphicsPipelineDesc pipeline_description{
        .depth = {
            .test = true,
            .write = true,
            .compare = vng::render::DepthCompare::less,
        },
        .cull = vng::render::CullMode::back,
        .front_face = vng::render::FrontFace::counter_clockwise,
        .output_encoding = vng::render::ColorEncoding::linear,
    };
    auto pipeline = vng::render::compile_pipeline(
        app->device(), *shader_program, pipeline_description);
    if (!pipeline) {
        return example::fail(pipeline.error());
    }
    auto gpu_mesh = vng::opengl::upload_mesh(app->device(), *cpu_mesh);
    if (!gpu_mesh) {
        return example::fail(gpu_mesh.error());
    }
    if (auto prepared = gpu_mesh->prepare_vertex_input(
            app->device(), *pipeline);
        !prepared) {
        return example::fail(prepared.error());
    }

    vng::gfx::Camera camera;
    camera.set_position({2.6F, 2.0F, 3.2F})
        .look_at({0.0F, 0.0F, 0.0F})
        .set_perspective({
            .vertical_fov = vng::degrees(50.0F),
            .near_plane = 0.1F,
            .far_plane = 100.0F,
        });

    example::WindowLoop loop{app->window(), options->frame_limit};
    while (const auto extent = loop.next_extent()) {
        auto frame = vng::render::begin_frame(
            app->device(),
            vng::render::FrameDesc{
                .extent = *extent,
                .color_encoding = vng::render::ColorEncoding::linear,
                .clear_color = std::array<vng::f32, 4>{
                    0.018F, 0.024F, 0.045F, 1.0F},
                .clear_depth = 1.0F,
            });
        if (!frame) {
            return example::fail(frame.error());
        }
        auto view = vng::render::RenderView::create(camera, *extent);
        if (!view) {
            return example::fail(view.error());
        }

        auto commands = frame->commands();
        if (auto bound = commands.bind(*pipeline); !bound) {
            return example::fail(bound.error());
        }
        if (auto viewed = commands.view(*view); !viewed) {
            return example::fail(viewed.error());
        }
        if (auto drawn = commands.draw(*gpu_mesh); !drawn) {
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
