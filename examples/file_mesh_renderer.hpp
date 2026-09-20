#pragma once

#include "file_mesh_types.hpp"

#include <expected>
#include <span>
#include <variant>

#include <vng/opengl/opengl.hpp>
#include <vng/opengl/renderer.hpp>
#include <vng/render/opengl.hpp>
#include <vng/render/render.hpp>
#include <vng/shader/diagnostic.hpp>
#include <vng/shader/program.hpp>

namespace file_mesh_example {

using FileMeshRendererDiagnostic = std::variant<
    vng::shader::Diagnostic,
    vng::opengl::Diagnostic>;

// This ticket contains only per-submission choices. The renderer owns the
// persistent mesh, shader products, and GPU resources.
struct FileMeshDraw final {
    vng::u32 instance_count{1};
    bool depth_test{true};
    bool depth_write{true};
    vng::render::CullMode cull{vng::render::CullMode::back};
    vng::f32 brightness{1.0F};
};

// A real application renderer: it owns its policy and resources, and directly
// decides which state to establish for every ticket. opengl::Renderer<Ticket> is only
// a zero-cost compile-time identity; there is no virtual dispatch or hidden
// one-technique implementation inside the base.
class FileMeshRenderer final
    : public vng::opengl::Renderer<FileMeshDraw> {
public:
    using Mesh = vng::gfx::Mesh<Vertex>;

    [[nodiscard]] static std::expected<
        FileMeshRenderer,
        FileMeshRendererDiagnostic>
    create(
        vng::opengl::Device& device,
        Mesh mesh);

    [[nodiscard]] std::expected<void, vng::opengl::Diagnostic> render(
        vng::opengl::Frame& frame,
        const vng::render::RenderView& view,
        std::span<const FileMeshDraw> draws);

    [[nodiscard]] std::expected<
        vng::analysis::FrameEvidence,
        vng::opengl::Diagnostic>
    capture(
        vng::opengl::Frame& frame,
        const vng::render::RenderView& view,
        std::span<const FileMeshDraw> draws,
        const vng::analysis::CaptureRequest& request);

    [[nodiscard]] std::expected<
        vng::analysis::DiagnosticSweep,
        vng::opengl::Diagnostic>
    diagnose(
        vng::opengl::Frame& frame,
        const vng::render::RenderView& view,
        std::span<const FileMeshDraw> draws,
        const vng::analysis::CaptureRequest& request);

private:
    using ShaderRuntime = vng::render::TypedOpenGLProgramRuntime<vng::f32>;
    [[nodiscard]] static vng::shader::Result<vng::shader::TypedGraphicsProgram<vng::f32>>
    create_shader_program();

    FileMeshRenderer(
        ShaderRuntime shader_runtime,
        Mesh source,
        vng::opengl::GpuMesh<Vertex> gpu_mesh) noexcept;

    [[nodiscard]] static std::expected<void, vng::opengl::Diagnostic>
    validate_frame(
        const vng::opengl::Frame& frame,
        const vng::render::RenderView& view);

    [[nodiscard]] std::expected<void, vng::opengl::Diagnostic>
    validate_diagnostic(
        const vng::opengl::Frame& frame,
        const vng::render::RenderView& view,
        std::span<const FileMeshDraw> draws) const;

    // Destruction is reverse declaration order: GPU resources are released
    // before the CPU provenance and shader/diagnostic runtime they reference.
    ShaderRuntime shader_runtime_;
    Mesh source_;
    vng::opengl::GpuMesh<Vertex> gpu_mesh_;
};

static_assert(vng::render::RendererFor<
    FileMeshRenderer,
    vng::opengl::Frame>);

} // namespace file_mesh_example
