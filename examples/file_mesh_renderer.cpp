#include "file_mesh_renderer.hpp"

#include <vng/shader/shader.hpp>

#include <utility>
#include <cmath>

namespace file_mesh_example {
namespace {
constexpr vng::opengl::GraphicsStateSnapshot raster(const FileMeshDraw& draw)
{
    return vng::opengl::GraphicsStateSnapshot{
        .depth = {draw.depth_test, draw.depth_write, vng::render::DepthCompare::less},
        .cull = draw.cull, .front_face = vng::render::FrontFace::counter_clockwise,
        .blend = vng::render::BlendMode::disabled, .polygon = vng::opengl::PolygonMode::fill};
}
}

vng::shader::Result<vng::shader::TypedGraphicsProgram<vng::f32>>
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
        [](auto& stage, vng::dsl::Float brightness) {
            const auto source = stage.input(Color{});
            const auto color = vng::dsl::vec4(source.xyz() * brightness, source.w());
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
    Mesh mesh)
{
    auto program = create_shader_program();
    if (!program) {
        return std::unexpected(FileMeshRendererDiagnostic{
            std::move(program.error())});
    }
    auto shader_runtime = ShaderRuntime::create(
        device, std::move(*program));
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
            device, shader_runtime->production());
        !prepared) {
        return std::unexpected(FileMeshRendererDiagnostic{
            std::move(prepared.error())});
    }
    return FileMeshRenderer{
        std::move(*shader_runtime),
        std::move(mesh),
        std::move(*gpu_mesh),
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

    auto commands = frame.render_context();
    for (const auto& draw : draws) {
        if (!std::isfinite(draw.brightness) || draw.brightness < 0.0F)
            return std::unexpected(vng::opengl::Diagnostic{
                .code = vng::opengl::ErrorCode::invalid_argument,
                .message = "Brightness must be finite and nonnegative"});
    }

    // These decisions belong to this renderer. A different ticket can
    // change them between draws without constructing another renderer.
    for (const auto& draw : draws) {
        if (auto configured = commands.graphics_state().set(raster(draw)); !configured) return configured;
        if (auto bound = commands.run(shader_runtime_.production(), draw.brightness); !bound) return bound;
        if (auto bound_view = commands.view(view); !bound_view) return bound_view;
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
    if (auto supplied = shader_runtime_.set_arguments(draws.front().brightness); !supplied)
        return std::unexpected(std::move(supplied.error()));
    return shader_runtime_.capture(
        frame.device(), source_, gpu_mesh_, view,
        vng::opengl::CaptureState{raster(draws.front()), frame.color_encoding()}, request);
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
    if (auto supplied = shader_runtime_.set_arguments(draws.front().brightness); !supplied)
        return std::unexpected(std::move(supplied.error()));
    return shader_runtime_.diagnose(
        frame.device(), source_, gpu_mesh_, view,
        vng::opengl::CaptureState{raster(draws.front()), frame.color_encoding()}, request);
}

FileMeshRenderer::FileMeshRenderer(
    ShaderRuntime shader_runtime,
    Mesh source,
    vng::opengl::GpuMesh<Vertex> gpu_mesh) noexcept
    : shader_runtime_(std::move(shader_runtime)),
      source_(std::move(source)),
      gpu_mesh_(std::move(gpu_mesh))
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
    if (!std::isfinite(draws.front().brightness) || draws.front().brightness < 0.0F)
        return std::unexpected(vng::opengl::Diagnostic{
            .code = vng::opengl::ErrorCode::invalid_argument,
            .message = "Brightness must be finite and nonnegative"});
    return {};
}

} // namespace file_mesh_example
