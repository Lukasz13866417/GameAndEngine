#include <vng/opengl/shader.hpp>

#include <vng/opengl/device.hpp>

#include "context_state.hpp"

#include <glad/gl.h>

#include <limits>
#include <utility>

namespace vng::opengl {
namespace {

[[nodiscard]] GLenum gl_stage(ShaderStage stage) noexcept {
    switch (stage) {
    case ShaderStage::vertex: return GL_VERTEX_SHADER;
    case ShaderStage::fragment: return GL_FRAGMENT_SHADER;
    }
    return GL_VERTEX_SHADER;
}

[[nodiscard]] std::string shader_log(GLuint shader) {
    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    if (length <= 1) {
        return {};
    }
    std::string log(static_cast<std::size_t>(length), '\0');
    GLsizei written = 0;
    glGetShaderInfoLog(shader, length, &written, log.data());
    log.resize(written > 0 ? static_cast<std::size_t>(written) : 0);
    return log;
}

} // namespace

Shader::Shader(
    std::shared_ptr<detail::ContextState> state,
    std::uint32_t handle,
    ShaderSource source,
    std::string driver_log) noexcept
    : state_(std::move(state)),
      handle_(handle),
      stage_(source.stage),
      source_(std::move(source)),
      driver_log_(std::move(driver_log)) {}

Shader::Shader(Shader&& other) noexcept
    : state_(std::move(other.state_)),
      handle_(std::exchange(other.handle_, 0)),
      stage_(other.stage_),
      source_(std::move(other.source_)),
      driver_log_(std::move(other.driver_log_)) {}

Shader& Shader::operator=(Shader&& other) noexcept {
    if (this != &other) {
        release_noexcept();
        state_ = std::move(other.state_);
        handle_ = std::exchange(other.handle_, 0);
        stage_ = other.stage_;
        source_ = std::move(other.source_);
        driver_log_ = std::move(other.driver_log_);
    }
    return *this;
}

Shader::~Shader() {
    release_noexcept();
}

std::expected<Shader, Diagnostic> Shader::compile(
    const Device& device,
    ShaderSource source) {
    source.validate_typed_metadata();
    if (!device.state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_context_access,
            .message = "Shader::compile received an empty device",
        });
    }
    if (source.text.empty()) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Shader::compile received empty generated source",
        });
    }
    if (source.text.size() > static_cast<std::size_t>(std::numeric_limits<GLint>::max())) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Shader::compile source exceeds the OpenGL GLint length range",
            .generated_source = source.text,
            .source_map = source.source_map,
        });
    }
    if (auto current = device.state_->require_current("Shader::compile"); !current) {
        return std::unexpected(std::move(current.error()));
    }

    const GLuint handle = glCreateShader(gl_stage(source.stage));
    if (handle == 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::shader_creation_failed,
            .message = "glCreateShader returned zero",
            .generated_source = source.text,
            .source_map = source.source_map,
        });
    }

    const GLchar* text = source.text.c_str();
    const auto length = static_cast<GLint>(source.text.size());
    glShaderSource(handle, 1, &text, &length);
    glCompileShader(handle);

    GLint successful = GL_FALSE;
    glGetShaderiv(handle, GL_COMPILE_STATUS, &successful);
    auto log = shader_log(handle);
    if (successful != GL_TRUE) {
        glDeleteShader(handle);
        return std::unexpected(Diagnostic{
            .code = ErrorCode::shader_compilation_failed,
            .message = source.stage == ShaderStage::vertex
                ? "OpenGL vertex shader compilation failed"
                : "OpenGL fragment shader compilation failed",
            .driver_log = std::move(log),
            .generated_source = source.text,
            .source_map = source.source_map,
        });
    }

    return Shader(device.state_, handle, std::move(source), std::move(log));
}

std::expected<void, Diagnostic> Shader::destroy() {
    if (handle_ == 0) {
        return {};
    }
    if (!state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_context_access,
            .message = "Shader::destroy has no device state",
        });
    }
    if (auto current = state_->require_current("Shader::destroy"); !current) {
        return current;
    }
    glDeleteShader(handle_);
    handle_ = 0;
    state_.reset();
    return {};
}

void Shader::release_noexcept() noexcept {
    if (handle_ == 0) {
        return;
    }
    if (state_ && state_->is_current()) {
        glDeleteShader(handle_);
    } else if (state_) {
        state_->record_lifecycle_failure(
            "Shader destroyed without its owning context current; native handle was leaked safely");
    }
    handle_ = 0;
    state_.reset();
}

} // namespace vng::opengl
