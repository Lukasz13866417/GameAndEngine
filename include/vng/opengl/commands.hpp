#pragma once

#include <expected>
#include <utility>

#include <vng/core/types.hpp>
#include <vng/opengl/device.hpp>
#include <vng/opengl/diagnostic.hpp>
#include <vng/opengl/gpu_mesh.hpp>
#include <vng/opengl/graphics_state.hpp>
#include <vng/render/view.hpp>

namespace vng::opengl {

class Frame;

// A frame-scoped OpenGL command stream. It is intentionally backend-specific:
// a custom renderer can choose shaders and fixed-function state dynamically,
// while Frame still owns the target, viewport, encoding, and lifetime scope.
//
// Obtain a borrowed handle with `auto commands = frame.commands();`. Handles
// share one frame-owned program/view/graphics state. Acquiring, moving or
// dropping another handle never revokes this one. Moving the Frame transfers
// ownership; ending/destroying it invalidates all handles. No handle points
// back to a Frame or Commands object. Child state changes remain visible:
// reselect the parent's program/settings explicitly when needed.
// Raw OpenGL can disturb the cache; use restore() and upload the view again.
class Commands final {
public:
    using backend_type = Backend;
    Commands(Commands&& other) noexcept;
    Commands& operator=(Commands&& other) noexcept;
    Commands(const Commands&) = delete;
    Commands& operator=(const Commands&) = delete;
    ~Commands() = default;

    [[nodiscard]] bool active() const noexcept;

    // All handles share this stream's current state and validate its lifetime.
    // No program or persistent settings object is needed to change state.
    [[nodiscard]] GraphicsState graphics_state() const & noexcept;
    GraphicsState graphics_state() const && = delete;

    // Selects shaders for subsequent draws, retaining every graphics setting.
    // Repeating run on the same program avoids a redundant native bind and
    // preserves view readiness. bind always reasserts the native program.
    // Moving/destroying a selected program clears the shared binding safely.
    [[nodiscard]] std::expected<void, Diagnostic> run(const Program& program);
    [[nodiscard]] std::expected<void, Diagnostic> bind(const Program& program);
    // Granular selection may reuse an already supplied snapshot. draw() still
    // rejects a program whose required arguments have never been supplied.
    template<class ProgramType>
        requires requires(const ProgramType& program) {
            typename ProgramType::signature;
            { program.untyped() } -> std::same_as<const Program&>;
        }
    [[nodiscard]] std::expected<void, Diagnostic> bind(const ProgramType& program)
    { return bind(program.untyped()); }
    template<class ProgramType, class... Args>
        requires requires(const ProgramType& program) {
            typename ProgramType::signature;
            { program.untyped() } -> std::same_as<const Program&>;
        } && shader::arguments_match_v<typename ProgramType::signature, Args...>
    [[nodiscard]] std::expected<void, Diagnostic> run(
        const ProgramType& program, const Args&... arguments)
    {
        if (auto valid = validate("OpenGL Commands::run arguments"); !valid) return valid;
        if (!program.untyped().belongs_to(device_)) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::incompatible_device,
                .message = "OpenGL Commands::run received a program from another device",
            });
        }
        const shader::ArgumentPack<std::remove_cvref_t<Args>...> pack{arguments...};
        const auto views = pack.views();
        if (auto values = program.untyped().set_arguments(views); !values) return values;
        return run(program.untyped());
    }
    std::expected<void, Diagnostic> run(Program&&) = delete;
    std::expected<void, Diagnostic> run(const Program&&) = delete;
    std::expected<void, Diagnostic> bind(Program&&) = delete;
    std::expected<void, Diagnostic> bind(const Program&&) = delete;
    template<shader::Value... Types, class... Args>
    std::expected<void, Diagnostic> run(TypedProgram<Types...>&&, const Args&...) = delete;
    template<shader::Value... Types, class... Args>
    std::expected<void, Diagnostic> run(const TypedProgram<Types...>&&, const Args&...) = delete;
    template<shader::Argument... Types>
    std::expected<void, Diagnostic> bind(TypedProgram<Types...>&&) = delete;
    template<shader::Argument... Types>
    std::expected<void, Diagnostic> bind(const TypedProgram<Types...>&&) = delete;

    // After raw GL: reassert the selected program, desired graphics state and
    // frame encoding. A shader using camera parameters needs view() again.
    [[nodiscard]] std::expected<void, Diagnostic> restore();

    // Uploads view parameters declared by the currently bound DSL program.
    // Drawing is rejected when a camera-reading shader has not received a
    // matching RenderView since it was bound.
    [[nodiscard]] std::expected<void, Diagnostic> view(
        const render::RenderView& view);

    // Compatibility conveniences; usable before selecting a program.
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
        auto program = prepare_draw();
        if (!program) {
            return std::unexpected(std::move(program.error()));
        }
        return mesh.draw_bound(device_, **program, instance_count);
    }

    template<gfx::RecordType... Records, gfx::RecordType Instance>
    [[nodiscard]] std::expected<void, Diagnostic> draw(
        GpuMesh<Records...>& mesh, const InstanceBuffer<Instance>& instances)
    {
        auto program=prepare_draw();
        if(!program)return std::unexpected(program.error());
        return mesh.draw_bound(device_,**program,instances);
    }

private:
    Commands(
        const Device& device,
        u64 frame_generation,
        Extent2D extent,
        render::ColorEncoding color_encoding) noexcept;

    [[nodiscard]] std::expected<void, Diagnostic> validate(
        const char* operation) const;
    [[nodiscard]] std::expected<void, Diagnostic> require_program(
        const char* operation) const;
    [[nodiscard]] std::expected<const Program*, Diagnostic>
    prepare_draw() const;
    void invalidate_binding() noexcept;

    Device device_;
    u64 frame_generation_{};
    Extent2D extent_{};
    render::ColorEncoding color_encoding_{render::ColorEncoding::srgb};

    friend class Frame;
};

} // namespace vng::opengl
