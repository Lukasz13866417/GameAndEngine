#include "file_mesh_types.hpp"
#include "support/diagnostics.hpp"
#include "support/glfw_opengl_session.hpp"
#include "support/options.hpp"
#include "support/window_loop.hpp"

#include <vng/content/content.hpp>
#include <vng/gfx/gfx.hpp>
#include <vng/opengl/opengl.hpp>
#include <vng/render/render.hpp>
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
        [](auto& stage, vng::dsl::Float4x4 model) {
            const auto world = model * vng::dsl::vec4(stage.input(Position{}), 1.0F);
            return stage.output(
                field<vng::shader::ClipPosition>(
                    stage.camera().project(world.xyz())),
                field<Color>(stage.input(Color{})));
        });
    if (!vertex) {
        return example::fail(vertex.error());
    }

    auto fragment = vng::shader::fragment<FragmentIn, FragmentOut>(
        "direct_file_mesh_fragment",
        [](auto& stage, vng::dsl::Float brightness) {
            const auto source = stage.input(Color{});
            const auto color = vng::dsl::vec4(source.xyz() * brightness, source.w());
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
        });
    if (!app) {
        return example::fail(app.error());
    }

    auto program = vng::render::compile_program(app->device(), *shader_program);
    if (!program) {
        return example::fail(program.error());
    }
    auto gpu_mesh = vng::opengl::upload_mesh(app->device(), *cpu_mesh);
    if (!gpu_mesh) {
        return example::fail(gpu_mesh.error());
    }
    if (auto prepared = gpu_mesh->prepare_vertex_input(
            app->device(), *program);
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

    // The shader lambdas run once above. These ordinary CPU values are read
    // again at each run() call, so changing either affects the next draw.
    auto model = vng::Mat4::identity();
    float brightness = 1.0F;
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

        auto commands = frame->render_context();
        auto graphics = commands.graphics_state();
        if (auto changed = graphics.set(vng::render::DepthTest{true}); !changed) {
            return example::fail(changed.error());
        }
        if (auto changed = graphics.set(vng::render::DepthWrite{true}); !changed) {
            return example::fail(changed.error());
        }
        if (auto changed = graphics.set(vng::render::CullMode::back); !changed) {
            return example::fail(changed.error());
        }
        if (auto bound = commands.run(*program, model, brightness); !bound) {
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
