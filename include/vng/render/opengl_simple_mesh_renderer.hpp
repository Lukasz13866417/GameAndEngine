#pragma once

#include <expected>
#include <span>
#include <utility>

#include <vng/analysis/analysis.hpp>
#include <vng/gfx/mesh.hpp>
#include <vng/opengl/device.hpp>
#include <vng/opengl/diagnostic.hpp>
#include <vng/opengl/frame.hpp>
#include <vng/opengl/gpu_mesh.hpp>
#include <vng/render/opengl_program_runtime.hpp>
#include <vng/render/pipeline.hpp>
#include <vng/render/renderer.hpp>
#include <vng/render/simple_mesh_renderer.hpp>
#include <vng/render/view.hpp>
#include <vng/shader/program.hpp>

namespace vng::opengl {

// Optional convenience renderer for applications that only need to draw one
// owned mesh with one ordinary pipeline. It is a concrete renderer policy, not
// the renderer abstraction: custom renderers can own any number of pipelines,
// meshes, and other resources and issue their own Frame commands directly.
template<gfx::RecordType... Records>
class SimpleMeshRenderer final : public render::Renderer<render::MeshDraw> {
public:
    [[nodiscard]] static std::expected<SimpleMeshRenderer, Diagnostic> create(
        Device& device,
        shader::GraphicsProgram program,
        gfx::Mesh<Records...> mesh,
        render::GraphicsPipelineDesc pipeline = {})
    {
        auto runtime = render::OpenGLProgramRuntime::create(
            device, std::move(program), pipeline);
        if (!runtime) {
            return std::unexpected(std::move(runtime.error()));
        }
        auto gpu_mesh = upload_mesh(device, mesh);
        if (!gpu_mesh) {
            return std::unexpected(std::move(gpu_mesh.error()));
        }
        if (auto prepared = gpu_mesh->prepare_vertex_input(
                device, runtime->normal_pipeline());
            !prepared) {
            return std::unexpected(std::move(prepared.error()));
        }
        return SimpleMeshRenderer{
            std::move(*runtime),
            std::move(mesh),
            std::move(*gpu_mesh),
        };
    }

    SimpleMeshRenderer(SimpleMeshRenderer&&) noexcept = default;
    SimpleMeshRenderer& operator=(SimpleMeshRenderer&&) noexcept = default;
    SimpleMeshRenderer(const SimpleMeshRenderer&) = delete;
    SimpleMeshRenderer& operator=(const SimpleMeshRenderer&) = delete;
    ~SimpleMeshRenderer() = default;

    [[nodiscard]] std::expected<void, Diagnostic> render(
        Frame& frame,
        const render::RenderView& view,
        std::span<const render::MeshDraw> tickets)
    {
        if (auto valid = validate_frame(frame, view); !valid) {
            return valid;
        }
        if (tickets.empty()) {
            return {};
        }
        auto commands = frame.commands();
        if (auto bound = commands.bind(runtime_.normal_pipeline()); !bound) {
            return bound;
        }
        if (auto uploaded = commands.view(view); !uploaded) {
            return uploaded;
        }
        for (const auto& ticket : tickets) {
            if (auto drawn = commands.draw(gpu_mesh_, ticket.instance_count);
                !drawn) {
                return drawn;
            }
        }
        return {};
    }

    [[nodiscard]] std::expected<analysis::FrameEvidence, Diagnostic> capture(
        Frame& frame,
        const render::RenderView& view,
        std::span<const render::MeshDraw> tickets,
        const analysis::CaptureRequest& request)
    {
        if (auto valid = validate_analysis(frame, view, tickets); !valid) {
            return std::unexpected(std::move(valid.error()));
        }
        return runtime_.capture(
            frame.device(), source_, gpu_mesh_, view, request);
    }

    [[nodiscard]] std::expected<analysis::DiagnosticSweep, Diagnostic> diagnose(
        Frame& frame,
        const render::RenderView& view,
        std::span<const render::MeshDraw> tickets,
        const analysis::CaptureRequest& request)
    {
        if (auto valid = validate_analysis(frame, view, tickets); !valid) {
            return std::unexpected(std::move(valid.error()));
        }
        return runtime_.diagnose(
            frame.device(), source_, gpu_mesh_, view, request);
    }

private:
    SimpleMeshRenderer(
        render::OpenGLProgramRuntime runtime,
        gfx::Mesh<Records...> source,
        GpuMesh<Records...> gpu_mesh) noexcept
        : runtime_(std::move(runtime)),
          source_(std::move(source)),
          gpu_mesh_(std::move(gpu_mesh))
    {}

    [[nodiscard]] static std::expected<void, Diagnostic> validate_frame(
        const Frame& frame,
        const render::RenderView& view)
    {
        if (!frame.active()) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_argument,
                .message = "simple mesh renderer received an ended OpenGL frame",
            });
        }
        if (frame.extent() != view.extent()) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_argument,
                .message = "simple mesh renderer requires matching frame and view extents",
            });
        }
        return {};
    }

    [[nodiscard]] static std::expected<void, Diagnostic> validate_analysis(
        const Frame& frame,
        const render::RenderView& view,
        std::span<const render::MeshDraw> tickets)
    {
        if (auto valid = validate_frame(frame, view); !valid) {
            return valid;
        }
        if (tickets.size() != 1) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_argument,
                .message = "simple mesh diagnostic capture currently requires exactly one ticket",
            });
        }
        if (tickets.front().instance_count != 1) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_argument,
                .message = "simple mesh diagnostic capture currently requires "
                           "one non-instanced draw",
            });
        }
        return {};
    }

    // Destruction is reverse declaration order: the backend mesh goes first,
    // then CPU provenance, then the shader/pipeline and diagnostic caches.
    render::OpenGLProgramRuntime runtime_;
    gfx::Mesh<Records...> source_;
    GpuMesh<Records...> gpu_mesh_;
};

} // namespace vng::opengl

namespace vng::render {

// Explicit opt-in convenience for the deliberately narrow one-mesh/one-
// pipeline case. Including only the backend-neutral render headers does not
// make this factory available.
template<gfx::RecordType... Records>
[[nodiscard]] std::expected<opengl::SimpleMeshRenderer<Records...>,
                            opengl::Diagnostic>
make_simple_mesh_renderer(
    opengl::Device& device,
    shader::GraphicsProgram program,
    gfx::Mesh<Records...> mesh,
    GraphicsPipelineDesc pipeline = {})
{
    return opengl::SimpleMeshRenderer<Records...>::create(
        device, std::move(program), std::move(mesh), pipeline);
}

} // namespace vng::render
