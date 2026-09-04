#pragma once

#include <expected>
#include <utility>

#include <vng/core/types.hpp>
#include <vng/opengl/device.hpp>
#include <vng/opengl/diagnostic.hpp>
#include <vng/opengl/gpu_mesh.hpp>
#include <vng/render/pipeline.hpp>
#include <vng/render/view.hpp>

namespace vng::opengl {

class Frame;
class GraphicsPipeline;

// A frame-scoped OpenGL command stream. It is intentionally backend-specific:
// a custom renderer can choose shaders and fixed-function state dynamically,
// while Frame still owns the target, viewport, encoding, and lifetime scope.
//
// Obtain one with `auto commands = frame.commands();`. Only the newest stream
// obtained from a Frame remains valid. Moving/ending/destroying that Frame also
// revokes the stream, so this value never relies on a pointer back to Frame.
// While it is in use, it exclusively owns the program and fixed-function state
// relevant to its draws. Calling lower-level OpenGL escape hatches may disturb
// that state; bind the pipeline and re-establish the desired state afterward.
class Commands final {
public:
    Commands(Commands&& other) noexcept;
    Commands& operator=(Commands&& other) noexcept;
    Commands(const Commands&) = delete;
    Commands& operator=(const Commands&) = delete;
    ~Commands() = default;

    [[nodiscard]] bool active() const noexcept;

    // Selects the compiled shader and establishes its complete baseline state.
    // The pipeline remains owned by the renderer and must outlive its use by
    // this command stream. Target encoding is checked before OpenGL is touched.
    [[nodiscard]] std::expected<void, Diagnostic> bind(
        const GraphicsPipeline& pipeline);

    // Commands retains a non-owning reference for the rest of the stream.
    // Reject temporaries at compile time; a named pipeline must remain alive
    // and unmoved until the command stream is no longer used.
    std::expected<void, Diagnostic> bind(GraphicsPipeline&&) = delete;
    std::expected<void, Diagnostic> bind(const GraphicsPipeline&&) = delete;

    // Uploads view parameters declared by the currently bound DSL program.
    // Drawing is rejected when a camera-reading shader has not received a
    // matching RenderView since it was bound.
    [[nodiscard]] std::expected<void, Diagnostic> view(
        const render::RenderView& view);

    // Replaces all portable fixed-function choices while retaining the bound
    // shader. `output_encoding` describes the selected target and therefore
    // must match this Frame; it cannot be changed mid-frame.
    [[nodiscard]] std::expected<void, Diagnostic> state(
        render::GraphicsPipelineDesc state);

    // Fine-grained state changes for renderers that make decisions on the fly.
    // A pipeline must be bound first so a later bind cannot silently overwrite
    // the requested state.
    [[nodiscard]] std::expected<void, Diagnostic> depth(
        render::DepthState state);

    [[nodiscard]] std::expected<void, Diagnostic> cull(
        render::CullMode mode,
        render::FrontFace front_face =
            render::FrontFace::counter_clockwise);

    template<gfx::RecordType... Records>
    [[nodiscard]] std::expected<void, Diagnostic> draw(
        GpuMesh<Records...>& mesh,
        u32 instance_count = 1)
    {
        auto pipeline = prepare_draw();
        if (!pipeline) {
            return std::unexpected(std::move(pipeline.error()));
        }
        return mesh.draw_bound(device_, **pipeline, instance_count);
    }

private:
    Commands(
        const Device& device,
        u64 frame_generation,
        u64 command_epoch,
        Extent2D extent,
        render::ColorEncoding color_encoding) noexcept;

    [[nodiscard]] std::expected<void, Diagnostic> validate(
        const char* operation) const;
    [[nodiscard]] std::expected<void, Diagnostic> require_pipeline(
        const char* operation) const;
    [[nodiscard]] std::expected<const GraphicsPipeline*, Diagnostic>
    prepare_draw() const;
    [[nodiscard]] std::expected<void, Diagnostic> validate_encoding(
        render::ColorEncoding encoding,
        const char* operation) const;
    void invalidate_binding() noexcept;

    Device device_;
    u64 frame_generation_{};
    u64 command_epoch_{};
    Extent2D extent_{};
    render::ColorEncoding color_encoding_{render::ColorEncoding::srgb};
    const GraphicsPipeline* pipeline_{};
    bool view_ready_{};

    friend class Frame;
};

} // namespace vng::opengl
