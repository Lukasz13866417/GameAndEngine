#include <vng/opengl/renderbuffer.hpp>

#include <vng/opengl/device.hpp>

#include "context_state.hpp"
#include "gl_error.hpp"

#include <glad/gl.h>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace vng::opengl {
namespace {

[[nodiscard]] GLenum gl_format(RenderbufferFormat format) noexcept {
    switch (format) {
    case RenderbufferFormat::rgba8: return GL_RGBA8;
    case RenderbufferFormat::depth24_stencil8: return GL_DEPTH24_STENCIL8;
    }
    return GL_NONE;
}

void delete_failed_renderbuffer(GLuint handle) noexcept {
    if (handle != 0) {
        glDeleteRenderbuffers(1, &handle);
        detail::clear_gl_errors();
    }
}

} // namespace

Renderbuffer::Renderbuffer(
    std::shared_ptr<detail::ContextState> state,
    std::uint32_t handle,
    std::uint32_t width,
    std::uint32_t height,
    RenderbufferFormat format,
    std::uint32_t samples) noexcept
    : state_(std::move(state)),
      handle_(handle),
      width_(width),
      height_(height),
      samples_(samples),
      format_(format) {}

Renderbuffer::Renderbuffer(Renderbuffer&& other) noexcept
    : state_(std::move(other.state_)),
      handle_(std::exchange(other.handle_, 0)),
      width_(std::exchange(other.width_, 0)),
      height_(std::exchange(other.height_, 0)),
      samples_(std::exchange(other.samples_, 0)),
      format_(other.format_) {}

Renderbuffer& Renderbuffer::operator=(Renderbuffer&& other) noexcept {
    if (this != &other) {
        release_noexcept();
        state_ = std::move(other.state_);
        handle_ = std::exchange(other.handle_, 0);
        width_ = std::exchange(other.width_, 0);
        height_ = std::exchange(other.height_, 0);
        samples_ = std::exchange(other.samples_, 0);
        format_ = other.format_;
    }
    return *this;
}

Renderbuffer::~Renderbuffer() {
    release_noexcept();
}

std::expected<Renderbuffer, Diagnostic> Renderbuffer::create(
    const Device& device,
    std::uint32_t width,
    std::uint32_t height,
    RenderbufferFormat format,
    std::uint32_t samples) {
    if (!device.state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_context_access,
            .message = "Renderbuffer::create received an empty device",
        });
    }
    if (width == 0 || height == 0
        || width > static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max())
        || height > static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max())) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Renderbuffer::create requires non-zero GLsizei-sized dimensions",
        });
    }
    const GLenum internal_format = gl_format(format);
    if (internal_format == GL_NONE) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Renderbuffer::create received an unsupported format",
        });
    }
    if (auto current = device.state_->require_current("Renderbuffer::create"); !current) {
        return std::unexpected(std::move(current.error()));
    }

    GLint max_size = 0;
    if (auto queried = detail::checked_gl_call(
            "glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE)",
            [&] { glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_size); });
        !queried) {
        return std::unexpected(std::move(queried.error()));
    }
    if (max_size <= 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::operation_failed,
            .message = "OpenGL reported an invalid GL_MAX_RENDERBUFFER_SIZE",
        });
    }
    if (width > static_cast<std::uint32_t>(max_size)
        || height > static_cast<std::uint32_t>(max_size)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Renderbuffer dimensions exceed GL_MAX_RENDERBUFFER_SIZE ("
                + std::to_string(max_size) + ')',
        });
    }

    GLint max_samples = 0;
    if (auto queried = detail::checked_gl_call(
            "glGetIntegerv(GL_MAX_SAMPLES)",
            [&] { glGetIntegerv(GL_MAX_SAMPLES, &max_samples); });
        !queried) {
        return std::unexpected(std::move(queried.error()));
    }
    if (max_samples < 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::operation_failed,
            .message = "OpenGL reported an invalid GL_MAX_SAMPLES",
        });
    }
    if (samples > static_cast<std::uint32_t>(max_samples)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Renderbuffer sample count exceeds GL_MAX_SAMPLES",
        });
    }
    if (samples > 0) {
        GLint sample_count = 0;
        if (auto queried = detail::checked_gl_call(
                "glGetInternalformativ(GL_NUM_SAMPLE_COUNTS)",
                [&] {
                    glGetInternalformativ(
                        GL_RENDERBUFFER,
                        internal_format,
                        GL_NUM_SAMPLE_COUNTS,
                        1,
                        &sample_count);
                });
            !queried) {
            return std::unexpected(std::move(queried.error()));
        }
        if (sample_count <= 0) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_argument,
                .message = "The requested renderbuffer format has no multisample counts",
            });
        }

        std::vector<GLint> supported_samples(static_cast<std::size_t>(sample_count));
        if (auto queried = detail::checked_gl_call(
                "glGetInternalformativ(GL_SAMPLES)",
                [&] {
                    glGetInternalformativ(
                        GL_RENDERBUFFER,
                        internal_format,
                        GL_SAMPLES,
                        sample_count,
                        supported_samples.data());
                });
            !queried) {
            return std::unexpected(std::move(queried.error()));
        }
        if (std::ranges::find(
                supported_samples,
                static_cast<GLint>(samples)) == supported_samples.end()) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_argument,
                .message = "The requested sample count is not supported for this renderbuffer format",
            });
        }
    }

    GLuint handle = 0;
    if (auto created = detail::checked_gl_call(
            "glCreateRenderbuffers",
            [&] { glCreateRenderbuffers(1, &handle); },
            ErrorCode::object_creation_failed);
        !created) {
        delete_failed_renderbuffer(handle);
        return std::unexpected(std::move(created.error()));
    }
    if (handle == 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::object_creation_failed,
            .message = "glCreateRenderbuffers returned zero",
        });
    }
    auto allocated = detail::checked_gl_call(
        samples > 0
            ? "glNamedRenderbufferStorageMultisample"
            : "glNamedRenderbufferStorage",
        [&] {
            if (samples > 0) {
                glNamedRenderbufferStorageMultisample(
                    handle,
                    static_cast<GLsizei>(samples),
                    internal_format,
                    static_cast<GLsizei>(width),
                    static_cast<GLsizei>(height));
            } else {
                glNamedRenderbufferStorage(
                    handle,
                    internal_format,
                    static_cast<GLsizei>(width),
                    static_cast<GLsizei>(height));
            }
        });
    if (!allocated) {
        delete_failed_renderbuffer(handle);
        return std::unexpected(std::move(allocated.error()));
    }

    GLint actual_samples = 0;
    if (auto queried = detail::checked_gl_call(
            "glGetNamedRenderbufferParameteriv(GL_RENDERBUFFER_SAMPLES)",
            [&] {
                glGetNamedRenderbufferParameteriv(
                    handle, GL_RENDERBUFFER_SAMPLES, &actual_samples);
            });
        !queried) {
        delete_failed_renderbuffer(handle);
        return std::unexpected(std::move(queried.error()));
    }
    if (actual_samples < 0) {
        delete_failed_renderbuffer(handle);
        return std::unexpected(Diagnostic{
            .code = ErrorCode::operation_failed,
            .message = "OpenGL reported an invalid renderbuffer sample count",
        });
    }
    return Renderbuffer(
        device.state_,
        handle,
        width,
        height,
        format,
        static_cast<std::uint32_t>(actual_samples));
}

std::expected<void, Diagnostic> Renderbuffer::destroy() {
    if (handle_ == 0) {
        return {};
    }
    if (!state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_context_access,
            .message = "Renderbuffer::destroy has no device state",
        });
    }
    if (auto current = state_->require_current("Renderbuffer::destroy"); !current) {
        return current;
    }
    const GLuint handle = handle_;
    glDeleteRenderbuffers(1, &handle);
    handle_ = 0;
    width_ = 0;
    height_ = 0;
    samples_ = 0;
    state_.reset();
    return {};
}

void Renderbuffer::release_noexcept() noexcept {
    if (handle_ == 0) {
        return;
    }
    if (state_ && state_->is_current()) {
        const GLuint handle = handle_;
        glDeleteRenderbuffers(1, &handle);
    } else if (state_) {
        state_->record_lifecycle_failure(
            "Renderbuffer destroyed without its owning context current; native handle was leaked safely");
    }
    handle_ = 0;
    width_ = 0;
    height_ = 0;
    samples_ = 0;
    state_.reset();
}

} // namespace vng::opengl
