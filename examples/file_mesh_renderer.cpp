#include "file_mesh_renderer.hpp"

#include <vng/shader/shader.hpp>

#include <utility>

namespace file_mesh_example {

vng::shader::Result<vng::shader::GraphicsProgram>
FileMeshRenderer::create_shader_program()
{
    using VertexIn = vng::shader::VertexInputs<Position, Color>;
    using VertexOut = vng::shader::VertexOutputs<
        vng::shader::ClipPosition,
        vng::shader::smooth<Color>>;
    using FragmentIn = vng::shader::FragmentInputs<
        vng::shader::smooth<Color>>;
    using FragmentOut = vng::shader::FragmentOutputs<
        vng::shader::Color<0>>;
    using vng::dsl::field;

    auto vertex = vng::shader::vertex<VertexIn, VertexOut>(
        "file_mesh_vertex",
        [](auto& stage) {
            return stage.output(
                field<vng::shader::ClipPosition>(
                    stage.camera().project(stage.input(Position{}))),
                field<Color>(stage.input(Color{})));
        });
    if (!vertex) {
        return std::unexpected(std::move(vertex.error()));
    }

    auto fragment = vng::shader::fragment<FragmentIn, FragmentOut>(
        "file_mesh_fragment",
        [](auto& stage) {
            const auto color = stage.input(Color{});
            stage.observe(SurfaceColor{}, color);
            return stage.output(
                field<vng::shader::Color<0>>(color));
        });
    if (!fragment) {
        return std::unexpected(std::move(fragment.error()));
    }

    return vng::shader::link(
        std::move(*vertex), std::move(*fragment));
}

std::expected<FileMeshRenderer, FileMeshRendererDiagnostic>
FileMeshRenderer::create(
    vng::opengl::Device& device,
    Mesh mesh,
    vng::render::GraphicsPipelineDesc baseline)
{
    auto program = create_shader_program();
    if (!program) {
        return std::unexpected(FileMeshRendererDiagnostic{
            std::move(program.error())});
    }
    auto shader_runtime = vng::render::OpenGLProgramRuntime::create(
        device, std::move(*program), baseline);
    if (!shader_runtime) {
        return std::unexpected(FileMeshRendererDiagnostic{
            std::move(shader_runtime.error())});
    }
    auto gpu_mesh = vng::opengl::upload_mesh(device, mesh);
    if (!gpu_mesh) {
        return std::unexpected(FileMeshRendererDiagnostic{
            std::move(gpu_mesh.error())});
    }
    if (auto prepared = gpu_mesh->prepare_vertex_input(
            device, shader_runtime->normal_pipeline());
        !prepared) {
        return std::unexpected(FileMeshRendererDiagnostic{
            std::move(prepared.error())});
    }
    return FileMeshRenderer{
        std::move(*shader_runtime),
        std::move(mesh),
        std::move(*gpu_mesh),
        baseline,
    };
}

std::expected<void, vng::opengl::Diagnostic> FileMeshRenderer::render(
    vng::opengl::Frame& frame,
    const vng::render::RenderView& view,
    std::span<const FileMeshDraw> draws)
{
    if (auto valid = validate_frame(frame, view); !valid) {
        return valid;
    }
    if (draws.empty()) {
        return {};
    }

    auto commands = frame.commands();
    if (auto bound = commands.bind(shader_runtime_.normal_pipeline());
        !bound) {
        return bound;
    }
    if (auto bound_view = commands.view(view); !bound_view) {
        return bound_view;
    }

    // These decisions belong to this renderer. A different ticket can
    // change them between draws without constructing another renderer.
    auto active_state = baseline_;
    for (const auto& draw : draws) {
        auto requested = baseline_;
        requested.depth.test = draw.depth_test;
        requested.depth.write = draw.depth_write;
        requested.cull = draw.cull;
        if (requested.depth != active_state.depth) {
            if (auto changed = commands.depth(requested.depth); !changed) {
                return changed;
            }
        }
        if (requested.cull != active_state.cull
            || requested.front_face != active_state.front_face) {
            if (auto changed = commands.cull(
                    requested.cull, requested.front_face);
                !changed) {
                return changed;
            }
        }
        active_state = requested;
        if (auto drawn = commands.draw(gpu_mesh_, draw.instance_count);
            !drawn) {
            return drawn;
        }
    }
    return {};
}

std::expected<vng::analysis::FrameEvidence, vng::opengl::Diagnostic>
FileMeshRenderer::capture(
    vng::opengl::Frame& frame,
    const vng::render::RenderView& view,
    std::span<const FileMeshDraw> draws,
    const vng::analysis::CaptureRequest& request)
{
    if (auto valid = validate_diagnostic(frame, view, draws); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    return shader_runtime_.capture(
        frame.device(), source_, gpu_mesh_, view, request);
}

std::expected<vng::analysis::DiagnosticSweep, vng::opengl::Diagnostic>
FileMeshRenderer::diagnose(
    vng::opengl::Frame& frame,
    const vng::render::RenderView& view,
    std::span<const FileMeshDraw> draws,
    const vng::analysis::CaptureRequest& request)
{
    if (auto valid = validate_diagnostic(frame, view, draws); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    return shader_runtime_.diagnose(
        frame.device(), source_, gpu_mesh_, view, request);
}

FileMeshRenderer::FileMeshRenderer(
    vng::render::OpenGLProgramRuntime shader_runtime,
    Mesh source,
    vng::opengl::GpuMesh<Vertex> gpu_mesh,
    vng::render::GraphicsPipelineDesc baseline) noexcept
    : shader_runtime_(std::move(shader_runtime)),
      source_(std::move(source)),
      gpu_mesh_(std::move(gpu_mesh)),
      baseline_(baseline)
{}

std::expected<void, vng::opengl::Diagnostic>
FileMeshRenderer::validate_frame(
    const vng::opengl::Frame& frame,
    const vng::render::RenderView& view)
{
    if (!frame.active()) {
        return std::unexpected(vng::opengl::Diagnostic{
            .code = vng::opengl::ErrorCode::invalid_argument,
            .message = "FileMeshRenderer received an ended frame",
        });
    }
    if (frame.extent() != view.extent()) {
        return std::unexpected(vng::opengl::Diagnostic{
            .code = vng::opengl::ErrorCode::invalid_argument,
            .message = "FileMeshRenderer requires matching frame and view extents",
        });
    }
    return {};
}

std::expected<void, vng::opengl::Diagnostic>
FileMeshRenderer::validate_diagnostic(
    const vng::opengl::Frame& frame,
    const vng::render::RenderView& view,
    std::span<const FileMeshDraw> draws) const
{
    if (auto valid = validate_frame(frame, view); !valid) {
        return valid;
    }
    if (draws.size() != 1 || draws.front().instance_count != 1) {
        return std::unexpected(vng::opengl::Diagnostic{
            .code = vng::opengl::ErrorCode::invalid_argument,
            .message = "FileMeshRenderer diagnostics currently require one non-instanced ticket",
        });
    }
    auto requested = baseline_;
    requested.depth.test = draws.front().depth_test;
    requested.depth.write = draws.front().depth_write;
    requested.cull = draws.front().cull;
    if (requested != baseline_) {
        return std::unexpected(vng::opengl::Diagnostic{
            .code = vng::opengl::ErrorCode::invalid_argument,
            .message = "FileMeshRenderer diagnostics currently require its production baseline state",
        });
    }
    return {};
}

} // namespace file_mesh_example
