#include <vng/opengl/program.hpp>

#include <vng/opengl/device.hpp>
#include <vng/opengl/shader.hpp>

#include "context_state.hpp"
#include "gl_error.hpp"

#include <glad/gl.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace vng::opengl {
namespace {

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
      vertex_inputs_(std::move(other.vertex_inputs_)) {}

Program& Program::operator=(Program&& other) noexcept {
    if (this != &other) {
        release_noexcept();
        state_ = std::move(other.state_);
        handle_ = std::exchange(other.handle_, 0);
        driver_log_ = std::move(other.driver_log_);
        vertex_inputs_ = std::move(other.vertex_inputs_);
    }
    return *this;
}

Program::~Program() {
    release_noexcept();
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
    glDeleteProgram(handle_);
    handle_ = 0;
    vertex_inputs_.reset();
    state_.reset();
    return {};
}

void Program::release_noexcept() noexcept {
    if (handle_ == 0) {
        return;
    }
    if (state_ && state_->is_current()) {
        glDeleteProgram(handle_);
    } else if (state_) {
        state_->record_lifecycle_failure(
            "Program destroyed without its owning context current; native handle was leaked safely");
    }
    handle_ = 0;
    state_.reset();
}

} // namespace vng::opengl
