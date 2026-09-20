#include <vng/opengl/program.hpp>

#include <vng/opengl/device.hpp>
#include <vng/opengl/shader.hpp>

#include "context_state.hpp"
#include "gl_error.hpp"

#include <glad/gl.h>

#include <array>
#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace vng::opengl {
namespace {

void delete_program(detail::ContextState& state, GLuint program) noexcept
{
    // Deleting the current program only marks it for deferred deletion. A
    // later state scope could capture that handle, unbind it (finally deleting
    // it), and then try to restore a name that no longer exists. End the native
    // binding before relinquishing ownership, as buffer/VAO deletion does.
    GLint current{};
    glGetIntegerv(GL_CURRENT_PROGRAM, &current);
    if (static_cast<GLuint>(current) == program) {
        glUseProgram(0);
        state.graphics_synchronized = false;
    }
    glDeleteProgram(program);
}

[[nodiscard]] std::string program_log(GLuint program) {
    GLint length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    if (length <= 1) {
        return {};
    }
    std::string log(static_cast<std::size_t>(length), '\0');
    GLsizei written = 0;
    glGetProgramInfoLog(program, length, &written, log.data());
    log.resize(written > 0 ? static_cast<std::size_t>(written) : 0);
    return log;
}

struct CombinedSources final {
    std::string text;
    std::vector<SourceMapEntry> source_map;
};

[[nodiscard]] CombinedSources combined_sources(std::span<const Shader* const> shaders) {
    CombinedSources result;
    for (const auto* shader : shaders) {
        if (shader == nullptr) {
            continue;
        }
        result.text += shader->stage() == ShaderStage::vertex
            ? "// ---- vertex shader ----\n"
            : "// ---- fragment shader ----\n";

        const auto source_first_line = static_cast<std::uint64_t>(
            std::ranges::count(result.text, '\n') + 1);
        for (const auto& mapping : shader->source().source_map) {
            if (mapping.generated_line == 0) {
                continue;
            }
            const auto combined_line = source_first_line + mapping.generated_line - 1;
            if (combined_line <= std::numeric_limits<std::uint32_t>::max()) {
                result.source_map.push_back(SourceMapEntry{
                    .generated_line = static_cast<std::uint32_t>(combined_line),
                    .ir_node = mapping.ir_node,
                });
            }
        }

        result.text += shader->source().text;
        if (!result.text.ends_with('\n')) {
            result.text += '\n';
        }
    }
    return result;
}

} // namespace

Program::Program(
    std::shared_ptr<detail::ContextState> state,
    std::uint32_t handle,
    std::string driver_log,
    std::optional<std::vector<VertexInputMetadata>> vertex_inputs) noexcept
    : state_(std::move(state)),
      handle_(handle),
      driver_log_(std::move(driver_log)),
      vertex_inputs_(std::move(vertex_inputs)) {}

Program::Program(Program&& other) noexcept
    : state_(std::move(other.state_)),
      handle_(std::exchange(other.handle_, 0)),
      driver_log_(std::move(other.driver_log_)),
      vertex_inputs_(std::move(other.vertex_inputs_)),
      generated_source_(std::move(other.generated_source_)),
      arguments_(std::move(other.arguments_)),
      arguments_uploaded_(std::exchange(other.arguments_uploaded_, false)) {
    if (state_ && state_->command_program == &other) {
        state_->command_program = nullptr;
        state_->command_view_ready = false;
    }
}

Program& Program::operator=(Program&& other) noexcept {
    if (this != &other) {
        release_noexcept();
        state_ = std::move(other.state_);
        handle_ = std::exchange(other.handle_, 0);
        driver_log_ = std::move(other.driver_log_);
        vertex_inputs_ = std::move(other.vertex_inputs_);
        generated_source_ = std::move(other.generated_source_);
        arguments_ = std::move(other.arguments_);
        arguments_uploaded_ = std::exchange(other.arguments_uploaded_, false);
        if (state_ && state_->command_program == &other) {
            state_->command_program = nullptr;
            state_->command_view_ready = false;
        }
    }
    return *this;
}

Program::~Program() {
    release_noexcept();
}
void Program::invalidate_command_binding() noexcept {
    if (state_ && state_->command_program == this) {
        state_->command_program = nullptr;
        state_->command_view_ready = false;
    }
}

std::expected<Program, Diagnostic> Program::link(
    const Device& device,
    std::span<const Shader* const> shaders) {
    if (!device.state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_context_access,
            .message = "Program::link received an empty device",
        });
    }
    if (shaders.empty()) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Program::link requires at least one shader",
        });
    }
    for (const auto* shader : shaders) {
        if (shader == nullptr || shader->handle_ == 0) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_argument,
                .message = "Program::link received a null or empty shader",
            });
        }
        if (shader->state_.get() != device.state_.get()) {
            return std::unexpected(detail::incompatible_device("Shader"));
        }
    }
    if (auto current = device.state_->require_current("Program::link"); !current) {
        return std::unexpected(std::move(current.error()));
    }

    const GLuint handle = glCreateProgram();
    if (handle == 0) {
        auto sources = combined_sources(shaders);
        return std::unexpected(Diagnostic{
            .code = ErrorCode::program_creation_failed,
            .message = "glCreateProgram returned zero",
            .generated_source = std::move(sources.text),
            .source_map = std::move(sources.source_map),
        });
    }
    for (const auto* shader : shaders) {
        glAttachShader(handle, shader->handle_);
    }
    glLinkProgram(handle);
    for (const auto* shader : shaders) {
        glDetachShader(handle, shader->handle_);
    }

    GLint successful = GL_FALSE;
    glGetProgramiv(handle, GL_LINK_STATUS, &successful);
    auto log = program_log(handle);
    if (successful != GL_TRUE) {
        auto sources = combined_sources(shaders);
        glDeleteProgram(handle);
        return std::unexpected(Diagnostic{
            .code = ErrorCode::program_link_failed,
            .message = "OpenGL program link failed",
            .driver_log = std::move(log),
            .generated_source = std::move(sources.text),
            .source_map = std::move(sources.source_map),
        });
    }
    std::optional<std::vector<VertexInputMetadata>> vertex_inputs;
    const Shader* vertex_shader = nullptr;
    bool multiple_vertex_shaders = false;
    for (const auto* shader : shaders) {
        if (shader->stage() == ShaderStage::vertex) {
            if (vertex_shader != nullptr) {
                multiple_vertex_shaders = true;
            } else {
                vertex_shader = shader;
            }
        }
    }
    if (vertex_shader != nullptr && !multiple_vertex_shaders) {
        vertex_inputs = vertex_shader->source().vertex_inputs_;
    }
    return Program(
        device.state_,
        handle,
        std::move(log),
        std::move(vertex_inputs));
}

std::expected<Program, Diagnostic> Program::link_graphics(
    const Device& device,
    const Shader& vertex,
    const Shader& fragment) {
    if (vertex.stage() != ShaderStage::vertex
        || fragment.stage() != ShaderStage::fragment) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Program::link_graphics expects vertex then fragment shaders",
        });
    }
    const std::array<const Shader*, 2> shaders{&vertex, &fragment};
    return link(device, shaders);
}

std::expected<void, Diagnostic> Program::bind() const {
    if (handle_ == 0 || !state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Program::bind called on an empty program",
        });
    }
    if (auto current = state_->require_current("Program::bind"); !current) {
        return current;
    }
    // Raw program binding bypasses Commands. Invalidate its shared cache even
    // on a failed native switch; Commands::bind republishes after success.
    state_->command_program = nullptr;
    state_->command_view_ready = false;
    return detail::checked_gl_call(
        "glUseProgram",
        [&] { glUseProgram(handle_); });
}

std::expected<void, Diagnostic> Program::set_uniform_u32(
    std::uint32_t location,
    std::uint32_t value) const {
    if (handle_ == 0 || !state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Program::set_uniform_u32 called on an empty program",
        });
    }
    if (location > static_cast<std::uint32_t>(std::numeric_limits<GLint>::max())) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Program::set_uniform_u32 location exceeds the OpenGL GLint range",
        });
    }
    if (auto current = state_->require_current("Program::set_uniform_u32"); !current) {
        return current;
    }
    return detail::checked_gl_call(
        "glProgramUniform1ui",
        [&] {
            glProgramUniform1ui(
                handle_,
                static_cast<GLint>(location),
                value);
        });
}

std::expected<void, Diagnostic> Program::set_uniform_mat4(
    std::uint32_t location,
    const Mat4& value) const {
    if (handle_ == 0 || !state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Program::set_uniform_mat4 called on an empty program",
        });
    }
    if (location > static_cast<std::uint32_t>(std::numeric_limits<GLint>::max())) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Program::set_uniform_mat4 location exceeds the OpenGL GLint range",
        });
    }
    if (auto current = state_->require_current("Program::set_uniform_mat4"); !current) {
        return current;
    }

    // Mat4 is logically column-major, but flatten explicitly instead of
    // exposing the representation of Vector/Matrix to the GL driver.
    std::array<GLfloat, 16> columns{};
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            columns[column * 4 + row] = value[column][row];
        }
    }
    return detail::checked_gl_call(
        "glProgramUniformMatrix4fv",
        [&] {
            glProgramUniformMatrix4fv(
                handle_,
                static_cast<GLint>(location),
                1,
                GL_FALSE,
                columns.data());
        });
}

bool Program::arguments_ready() const noexcept
{
    return arguments_uploaded_ || !generated_source_
        || std::ranges::none_of(generated_source_->parameters, [](const auto& parameter) {
            return parameter.kind == shader::ParameterKind::argument;
        });
}

std::expected<void, Diagnostic> Program::set_arguments(
    std::span<const shader::ArgumentView> values) const
{
    arguments_uploaded_ = false;
    auto invalid = [](std::string message) -> std::expected<void, Diagnostic> {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument, .message = std::move(message),
        });
    };
    if (!state_ || handle_ == 0) return invalid("Program::set_arguments received an empty program");
    if (auto current = state_->require_current("Program::set_arguments"); !current) return current;
    const auto* source = generated_source();
    const auto count = source ? std::ranges::count_if(source->parameters, [](const auto& field) {
        return field.kind == shader::ParameterKind::argument;
    }) : 0;
    if (values.size() != static_cast<std::size_t>(count)) {
        return invalid("Program::set_arguments argument count disagrees with the shader contract");
    }
    if (source) {
        for (const auto& field : source->parameters) {
            if (field.kind != shader::ParameterKind::argument) continue;
            if (field.argument_index >= values.size()) {
                return invalid("Program::set_arguments encountered an invalid argument index");
            }
            const auto& value = values[field.argument_index];
            if (value.type != field.argument_type || value.words.size() != field.word_count) {
                return invalid("Program::set_arguments type or logical word count disagrees at argument "
                    + std::to_string(field.argument_index));
            }
            for (const auto& leaf : field.leaves) {
                if (leaf.columns == 0 || leaf.rows == 0 || leaf.columns > 4 || leaf.rows > 4
                    || leaf.word_offset > value.words.size()
                    || leaf.columns * leaf.rows > value.words.size() - leaf.word_offset
                    || (leaf.columns > 1 && (leaf.scalar != shader::ScalarKind::f32
                        || leaf.rows != leaf.columns || leaf.columns < 3))) {
                    return invalid("Program::set_arguments encountered invalid uniform leaf metadata");
                }
            }
        }
    }
    // Resolve native activity once. Unused declared arguments still belong to
    // the typed contract, but drivers may optimize their uniforms away (-1).
    if (arguments_.size() != values.size()) arguments_.resize(values.size());
    if (source) {
        for (const auto& field : source->parameters) {
            if (field.kind != shader::ParameterKind::argument) continue;
            auto& saved = arguments_[field.argument_index];
            if (saved.native_locations.size() != field.leaves.size()) {
                saved.native_locations.resize(field.leaves.size());
                auto resolved = detail::checked_gl_call("Program::set_arguments uniform locations", [&] {
                    for (std::size_t index = 0; index < field.leaves.size(); ++index) {
                        saved.native_locations[index] = glGetUniformLocation(
                            handle_, field.leaves[index].name.c_str());
                    }
                });
                if (!resolved) {
                    saved.native_locations.clear();
                    return resolved;
                }
            }
        }
    }
    auto uploaded = detail::checked_gl_call("Program::set_arguments", [&] {
        if (!source) return;
        for (const auto& field : source->parameters) {
            if (field.kind != shader::ParameterKind::argument) continue;
            const auto& words = values[field.argument_index].words;
            const auto& locations = arguments_[field.argument_index].native_locations;
            for (std::size_t index = 0; index < field.leaves.size(); ++index) {
                const auto& leaf = field.leaves[index];
                const auto location = locations[index];
                if (location < 0) continue;
                const auto count = leaf.columns * leaf.rows;
                std::array<GLfloat, 16> floats{};
                std::array<GLint, 4> signed_values{};
                std::array<GLuint, 4> unsigned_values{};
                for (u32 component = 0; component < count; ++component) {
                    const auto word = words[leaf.word_offset + component];
                    if (leaf.scalar == shader::ScalarKind::f32) {
                        floats[component] = std::bit_cast<f32>(word);
                    } else if (leaf.scalar == shader::ScalarKind::u32) {
                        unsigned_values[component] = word;
                    } else {
                        signed_values[component] = leaf.scalar == shader::ScalarKind::boolean
                            ? static_cast<GLint>(word != 0) : std::bit_cast<i32>(word);
                    }
                }
                if (leaf.columns == 3) {
                    glProgramUniformMatrix3fv(handle_, location, 1, GL_FALSE, floats.data());
                } else if (leaf.columns == 4) {
                    glProgramUniformMatrix4fv(handle_, location, 1, GL_FALSE, floats.data());
                } else if (leaf.scalar == shader::ScalarKind::f32) {
                    switch (leaf.rows) {
                    case 1: glProgramUniform1fv(handle_, location, 1, floats.data()); break;
                    case 2: glProgramUniform2fv(handle_, location, 1, floats.data()); break;
                    case 3: glProgramUniform3fv(handle_, location, 1, floats.data()); break;
                    case 4: glProgramUniform4fv(handle_, location, 1, floats.data()); break;
                    }
                } else if (leaf.scalar == shader::ScalarKind::u32) {
                    switch (leaf.rows) {
                    case 1: glProgramUniform1uiv(handle_, location, 1, unsigned_values.data()); break;
                    case 2: glProgramUniform2uiv(handle_, location, 1, unsigned_values.data()); break;
                    case 3: glProgramUniform3uiv(handle_, location, 1, unsigned_values.data()); break;
                    case 4: glProgramUniform4uiv(handle_, location, 1, unsigned_values.data()); break;
                    }
                } else {
                    switch (leaf.rows) {
                    case 1: glProgramUniform1iv(handle_, location, 1, signed_values.data()); break;
                    case 2: glProgramUniform2iv(handle_, location, 1, signed_values.data()); break;
                    case 3: glProgramUniform3iv(handle_, location, 1, signed_values.data()); break;
                    case 4: glProgramUniform4iv(handle_, location, 1, signed_values.data()); break;
                    }
                }
            }
        }
    });
    if (!uploaded) return uploaded;
    for (std::size_t index = 0; index < values.size(); ++index) {
        arguments_[index].type = values[index].type;
        arguments_[index].words.assign(values[index].words.begin(), values[index].words.end());
    }
    arguments_uploaded_ = true;
    return {};
}

std::expected<void, Diagnostic> Program::copy_arguments_to(const Program& target) const
{
    if (!arguments_ready()) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "shader arguments have not been supplied before diagnostic rendering",
        });
    }
    std::vector<shader::ArgumentView> views;
    views.reserve(arguments_.size());
    for (const auto& argument : arguments_) views.push_back({argument.type, argument.words});
    return target.set_arguments(views);
}

bool Program::belongs_to(const Device& device) const noexcept {
    return handle_ != 0 && state_ && state_.get() == device.state_.get();
}

std::expected<void, Diagnostic> Program::destroy() {
    if (handle_ == 0) {
        return {};
    }
    if (!state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_context_access,
            .message = "Program::destroy has no device state",
        });
    }
    if (auto current = state_->require_current("Program::destroy"); !current) {
        return current;
    }
    invalidate_command_binding();
    delete_program(*state_, handle_);
    handle_ = 0;
    vertex_inputs_.reset();
    generated_source_.reset();
    arguments_.clear();
    arguments_uploaded_ = false;
    state_.reset();
    return {};
}

void Program::release_noexcept() noexcept {
    invalidate_command_binding();
    if (handle_ == 0) {
        return;
    }
    if (state_ && state_->is_current()) {
        delete_program(*state_, handle_);
    } else if (state_) {
        state_->record_lifecycle_failure(
            "Program destroyed without its owning context current; native handle was leaked safely");
    }
    handle_ = 0;
    state_.reset();
}

} // namespace vng::opengl
