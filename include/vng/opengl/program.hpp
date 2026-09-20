#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <vng/core/types.hpp>
#include <vng/glsl/emitter.hpp>
#include <vng/opengl/diagnostic.hpp>
#include <vng/opengl/shader.hpp>
#include <vng/shader/arguments.hpp>

namespace vng::render {
template<shader::Argument... Args> class TypedOpenGLProgramRuntime;
}

namespace vng::opengl {

namespace detail { struct ContextState; }
class Device;

class Program final {
public:
    static std::expected<Program, Diagnostic> link(
        const Device& device,
        std::span<const Shader* const> shaders);

    static std::expected<Program, Diagnostic> link_graphics(
        const Device& device,
        const Shader& vertex,
        const Shader& fragment);

    Program(Program&& other) noexcept;
    Program& operator=(Program&& other) noexcept;
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;
    ~Program();

    [[nodiscard]] std::uint32_t native_handle() const noexcept { return handle_; }
    [[nodiscard]] const std::string& driver_log() const noexcept { return driver_log_; }
    [[nodiscard]] bool belongs_to(const Device& device) const noexcept;
    [[nodiscard]] const std::optional<std::vector<VertexInputMetadata>>&
    vertex_inputs() const noexcept { return vertex_inputs_; }
    [[nodiscard]] const glsl::ProgramSource* generated_source() const noexcept
    {
        return generated_source_ ? &*generated_source_ : nullptr;
    }
    [[nodiscard]] std::expected<void, Diagnostic> bind() const;
    [[nodiscard]] std::expected<void, Diagnostic> set_uniform_u32(
        std::uint32_t location,
        std::uint32_t value) const;
    [[nodiscard]] std::expected<void, Diagnostic> set_uniform_mat4(
        std::uint32_t location,
        const Mat4& value) const;
    // The erased boundary validates the complete logical contract before any
    // upload. Values are snapshots, never retained references to caller data.
    [[nodiscard]] std::expected<void, Diagnostic> set_arguments(
        std::span<const shader::ArgumentView> arguments) const;
    [[nodiscard]] bool arguments_ready() const noexcept;
    [[nodiscard]] std::expected<void, Diagnostic> copy_arguments_to(
        const Program& target) const;

    struct ArgumentSnapshot final {
        std::type_index type{typeid(void)};
        std::vector<u32> words;
        std::vector<i32> native_locations;
    };
    [[nodiscard]] std::span<const ArgumentSnapshot> argument_snapshots() const noexcept
    { return arguments_; }
    [[nodiscard]] std::expected<void, Diagnostic> destroy();

private:
    Program(
        std::shared_ptr<detail::ContextState> state,
        std::uint32_t handle,
        std::string driver_log,
        std::optional<std::vector<VertexInputMetadata>> vertex_inputs) noexcept;
    void release_noexcept() noexcept;
    void invalidate_command_binding() noexcept;

    std::shared_ptr<detail::ContextState> state_;
    std::uint32_t handle_{};
    std::string driver_log_;
    std::optional<std::vector<VertexInputMetadata>> vertex_inputs_;
    std::optional<glsl::ProgramSource> generated_source_;
    mutable std::vector<ArgumentSnapshot> arguments_;
    mutable bool arguments_uploaded_{};

    friend std::expected<Program, Diagnostic> compile_graphics_program(
        const Device&, const glsl::ProgramSource&);
    friend std::expected<Program, Diagnostic> compile_graphics_program(
        const Device&, const shader::GraphicsProgram&);
};

[[nodiscard]] std::expected<Program, Diagnostic> compile_graphics_program(
    const Device& device, const shader::GraphicsProgram& program);

// Backend lowering boundary, also used by derived diagnostic shaders.
// Retains generated source, argument/interface metadata and mapped errors.
[[nodiscard]] std::expected<Program, Diagnostic> compile_graphics_program(
    const Device& device, const glsl::ProgramSource& source);

template<shader::Argument... Args> class TypedProgram;

template<shader::Argument... Args>
class TypedProgramView final {
public:
    using signature = shader::Arguments<Args...>;
    [[nodiscard]] const Program& untyped() const noexcept { return *program_; }
    template<class... Values>
        requires shader::arguments_match_v<signature, Values...>
    [[nodiscard]] std::expected<void, Diagnostic> set_arguments(const Values&... values) const
    {
        const shader::ArgumentPack<std::remove_cvref_t<Values>...> pack{values...};
        const auto views = pack.views();
        return program_->set_arguments(views);
    }
private:
    explicit TypedProgramView(const Program& program) noexcept : program_(&program) {}
    const Program* program_;
    friend class TypedProgram<Args...>;
    friend class render::TypedOpenGLProgramRuntime<Args...>;
};

template<shader::Argument... Args>
class TypedProgram final {
public:
    using signature = shader::Arguments<Args...>;
    [[nodiscard]] const Program& untyped() const & noexcept { return program_; }
    const Program& untyped() const && = delete;
    [[nodiscard]] bool belongs_to(const Device& device) const noexcept
    { return program_.belongs_to(device); }
    [[nodiscard]] u32 native_handle() const noexcept { return program_.native_handle(); }
    [[nodiscard]] const glsl::ProgramSource* generated_source() const noexcept
    { return program_.generated_source(); }
    template<class... Values>
        requires shader::arguments_match_v<signature, Values...>
    [[nodiscard]] std::expected<void, Diagnostic> set_arguments(const Values&... values) const
    { return TypedProgramView<Args...>{program_}.set_arguments(values...); }
private:
    explicit TypedProgram(Program program) noexcept : program_(std::move(program)) {}
    Program program_;
    template<shader::Argument... Types>
    friend std::expected<TypedProgram<Types...>, Diagnostic> compile_graphics_program(
        const Device&, const shader::TypedGraphicsProgram<Types...>&);
};

template<shader::Argument... Args>
[[nodiscard]] std::expected<TypedProgram<Args...>, Diagnostic> compile_graphics_program(
    const Device& device, const shader::TypedGraphicsProgram<Args...>& program)
{
    auto compiled = compile_graphics_program(device, program.untyped());
    if (!compiled) return std::unexpected(std::move(compiled.error()));
    return TypedProgram<Args...>{std::move(*compiled)};
}

} // namespace vng::opengl
