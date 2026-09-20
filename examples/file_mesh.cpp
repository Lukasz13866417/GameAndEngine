#include "file_mesh_renderer.hpp"
#include "support/analysis_report.hpp"
#include "support/diagnostics.hpp"
#include "support/glfw_opengl_session.hpp"
#include "support/options.hpp"
#include "support/window_loop.hpp"

#include <vng/content/content.hpp>
#include <vng/gfx/gfx.hpp>
#include <vng/render/opengl.hpp>

#include <array>
#include <filesystem>
#include <iostream>
#include <utility>

#ifndef VNG_EXAMPLE_MESH_PATH
#define VNG_EXAMPLE_MESH_PATH "examples/assets/colored_cube.vmesh"
#endif

int main(int argc, char** argv)
{
    using namespace file_mesh_example;

    auto options = example::parse_mesh_options(
        argc, argv, std::filesystem::path{VNG_EXAMPLE_MESH_PATH});
    if (!options) {
        return example::fail(options.error());
    }

    namespace vmesh = vng::content::vmesh;
    auto vertex_schema = vmesh::schema<Vertex>();
    vertex_schema.map("position", Position{});
    vertex_schema.map("color/0", Color{});

    // Stable field names from the file become typed semantic records. Decode
    // also converts the logical f32 colors into this Vertex's unorm8x4 codec.
    auto cpu_mesh = vmesh::load(options->mesh_path, vertex_schema);
    if (!cpu_mesh) {
        return example::fail(cpu_mesh.error());
    }
    std::cout << "loaded: " << options->mesh_path << '\n'
              << "name: " << cpu_mesh->info().name << '\n'
              << "vertices: " << cpu_mesh->vertex_count() << '\n'
              << "faces: " << cpu_mesh->face_count() << '\n'
              << "explicit edges: " << cpu_mesh->explicit_edge_count() << '\n';

    auto app = example::GlfwOpenGLSession::create(
        vng::window::WindowDesc{
            .width = 700,
            .height = 700,
            .title = "Vibe Engine - file-backed mesh",
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

    // Shader definitions, compilation, mesh upload, and VAO prewarming are
    // renderer-owned setup. Draw tickets choose depth/culling per submission.
    auto renderer = FileMeshRenderer::create(
        app->device(),
        std::move(*cpu_mesh));
    if (!renderer) {
        return example::fail(renderer.error());
    }
    const std::array draws{FileMeshDraw{}};

    vng::gfx::Camera camera;
    camera.set_position({2.6F, 2.0F, 3.2F})
        .look_at({0.0F, 0.0F, 0.0F})
        .set_perspective({
            .vertical_fov = vng::degrees(50.0F),
            .near_plane = 0.1F,
            .far_plane = 100.0F,
        });

    bool analyzed = false;
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

        if (options->analyze && !analyzed) {
            auto request = vng::analysis::CaptureRequest::diagnostic()
                .observe(SurfaceColor{});
            auto sweep = renderer->diagnose(*frame, *view, draws, request);
            if (!sweep) {
                return example::fail(sweep.error());
            }
            example::print_analysis(*sweep, SurfaceColor{});
            analyzed = true;
        }

        if (auto drawn = renderer->render(*frame, *view, draws); !drawn) {
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
