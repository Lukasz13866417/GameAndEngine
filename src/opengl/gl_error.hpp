#pragma once

#include <vng/opengl/diagnostic.hpp>

#include <glad/gl.h>

#include <expected>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace vng::opengl::detail {

[[nodiscard]] inline std::string_view gl_error_name(GLenum error) noexcept
{
    switch (error) {
    case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
    case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
    case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
    case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
    case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
    case GL_STACK_UNDERFLOW: return "GL_STACK_UNDERFLOW";
    case GL_STACK_OVERFLOW: return "GL_STACK_OVERFLOW";
#ifdef GL_CONTEXT_LOST
    case GL_CONTEXT_LOST: return "GL_CONTEXT_LOST";
#endif
    default: return "unknown OpenGL error";
    }
}

inline void clear_gl_errors() noexcept
{
    while (glGetError() != GL_NO_ERROR) {
    }
}

template<class Function>
[[nodiscard]] std::expected<void, Diagnostic> checked_gl_call(
    std::string_view operation,
    Function&& function,
    ErrorCode code = ErrorCode::operation_failed)
{
    // Attribute only errors produced by this boundary to the operation. Raw GL
    // calls made by a host before entering vng retain no useful attribution.
    clear_gl_errors();
    std::forward<Function>(function)();

    const GLenum first_error = glGetError();
    if (first_error == GL_NO_ERROR) {
        return {};
    }

    std::ostringstream message;
    message << operation << " failed with " << gl_error_name(first_error)
            << " (0x" << std::hex << std::uppercase << first_error << ')';
    while (glGetError() != GL_NO_ERROR) {
        // Consume any additional errors so they cannot be misattributed to the
        // next checked call. The first error is the actionable one.
    }
    return std::unexpected(Diagnostic{
        .code = code,
        .message = std::move(message).str(),
    });
}

} // namespace vng::opengl::detail
