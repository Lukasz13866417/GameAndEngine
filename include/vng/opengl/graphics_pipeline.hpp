#pragma once

#include <expected>
#include <optional>
#include <span>
#include <vector>

#include <vng/glsl/emitter.hpp>
#include <vng/opengl/device.hpp>
#include <vng/opengl/diagnostic.hpp>
#include <vng/opengl/program.hpp>
#include <vng/render/pipeline.hpp>

namespace vng::render { class OpenGLProgramRuntime; }

namespace vng::opengl {

// The OpenGL realization of a backend-neutral graphics-pipeline
// description. OpenGL has no single native object containing all of this
// state, so the realization owns the program and stores the translated
// fixed-function state that bind() establishes as one coherent unit.
class GraphicsPipeline final {
public:
    // The explicit rvalue denotes eventual ownership transfer. Validation is
    // completed before the program is moved, so failure leaves it intact.
    [[nodiscard]] static std::expected<GraphicsPipeline, Diagnostic> realize(
        const Device& device,
        Program&& program,
        render::GraphicsPipelineDesc description = {});

    GraphicsPipeline(GraphicsPipeline&&) noexcept = default;
    GraphicsPipeline& operator=(GraphicsPipeline&&) noexcept = default;
    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;
    ~GraphicsPipeline() = default;

    [[nodiscard]] const Program& program() const noexcept { return program_; }

    [[nodiscard]] const render::GraphicsPipelineDesc& description() const noexcept
    {
        return description_;
    }

    // Present for pipelines compiled from backend-neutral shader IR. Expert
    // realization from a raw Program has no generated-source artifact.
    [[nodiscard]] const glsl::ProgramSource* generated_source() const noexcept
    {
        return generated_source_ ? &*generated_source_ : nullptr;
    }

    [[nodiscard]] bool belongs_to(const Device& device) const noexcept
    {
        return program_.belongs_to(device);
    }

    // A present value is the complete set of sparse fragment color locations
    // known from emitted shader metadata. nullopt means the pipeline came from
    // a raw expert Program and bind() must use its conservative fallback.
    [[nodiscard]] std::optional<std::span<const std::uint32_t>>
    fragment_color_output_locations() const noexcept
    {
        if (!fragment_color_output_locations_) {
            return std::nullopt;
        }
        return std::span<const std::uint32_t>{
            *fragment_color_output_locations_};
    }

    // Establishes the complete fixed-function baseline for every fragment
    // color location emitted from neutral shader IR and binds the owned
    // program. Expert pipelines made from a raw Program conservatively reset
    // every supported draw-buffer slot because they have no neutral interface
    // metadata. Render-target, viewport, vertex-input, and resource bindings
    // remain the caller's responsibility.
    [[nodiscard]] std::expected<void, Diagnostic> bind(
        const Device& device) const;

private:
    GraphicsPipeline(
        Program program,
        render::GraphicsPipelineDesc description,
        DepthState depth,
        CullState cull,
        bool framebuffer_srgb) noexcept;

    [[nodiscard]] std::expected<void, Diagnostic>
    adopt_emitted_fragment_outputs(const glsl::ProgramSource& source);

    // Renderer-owned augmented GLSL has already passed through neutral
    // emission, but is compiled lazily. Keep that trusted source metadata on
    // its realization without exposing a second application-facing factory.
    [[nodiscard]] static std::expected<GraphicsPipeline, Diagnostic>
    realize_emitted(
        const Device& device,
        Program&& program,
        const glsl::ProgramSource& source,
        render::GraphicsPipelineDesc description);

    Program program_;
    render::GraphicsPipelineDesc description_{};
    DepthState depth_{};
    CullState cull_{};
    bool framebuffer_srgb_{};
    std::optional<std::vector<std::uint32_t>>
        fragment_color_output_locations_;
    std::optional<glsl::ProgramSource> generated_source_{};

    friend std::expected<GraphicsPipeline, Diagnostic>
    compile_graphics_pipeline(
        const Device&,
        const shader::GraphicsProgram&,
        render::GraphicsPipelineDesc);
    friend class ::vng::render::OpenGLProgramRuntime;
};

// Backend customization selected by
// render::compile_pipeline(device, neutral_program, desc). It performs GLSL
// lowering, driver compilation/linking, and fixed-function realization.
[[nodiscard]] std::expected<GraphicsPipeline, Diagnostic> compile_graphics_pipeline(
    const Device& device,
    const shader::GraphicsProgram& program,
    render::GraphicsPipelineDesc description = {});

// Expert customization selected by
// render::expert::realize_pipeline(device, backend_program, desc). Validation
// finishes before ownership moves, so a failure leaves `program` intact.
[[nodiscard]] std::expected<GraphicsPipeline, Diagnostic> realize_graphics_pipeline(
    const Device& device,
    Program&& program,
    render::GraphicsPipelineDesc description = {});

} // namespace vng::opengl
