#include <vng/opengl/image.hpp>

#include <vng/opengl/device.hpp>

#include "context_state.hpp"
#include "gl_error.hpp"

#include <glad/gl.h>

#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace vng::opengl {
namespace {

[[nodiscard]] GLenum gl_internal_format(ImageFormat format) noexcept {
    switch (format) {
    case ImageFormat::rgba8: return GL_RGBA8;
    case ImageFormat::rg32ui: return GL_RG32UI;
    case ImageFormat::rgba32f: return GL_RGBA32F;
    case ImageFormat::rgba32i: return GL_RGBA32I;
    case ImageFormat::rgba32ui: return GL_RGBA32UI;
    case ImageFormat::depth32f: return GL_DEPTH_COMPONENT32F;
    }
    return GL_NONE;
}

void delete_failed_image(GLuint handle) noexcept {
    if (handle != 0) {
        glDeleteTextures(1, &handle);
        detail::clear_gl_errors();
    }
}

} // namespace

Image2D::Image2D(
    std::shared_ptr<detail::ContextState> state,
    std::uint32_t handle,
    std::uint32_t width,
    std::uint32_t height,
    ImageFormat format) noexcept
    : state_(std::move(state)),
      handle_(handle),
      width_(width),
      height_(height),
      format_(format) {}

Image2D::Image2D(Image2D&& other) noexcept
    : state_(std::move(other.state_)),
      handle_(std::exchange(other.handle_, 0)),
      width_(std::exchange(other.width_, 0)),
      height_(std::exchange(other.height_, 0)),
      format_(other.format_) {}

Image2D& Image2D::operator=(Image2D&& other) noexcept {
    if (this != &other) {
        release_noexcept();
        state_ = std::move(other.state_);
        handle_ = std::exchange(other.handle_, 0);
        width_ = std::exchange(other.width_, 0);
        height_ = std::exchange(other.height_, 0);
        format_ = other.format_;
    }
    return *this;
}

Image2D::~Image2D() {
    release_noexcept();
}

bool Image2D::belongs_to(const Device& device) const noexcept {
    return handle_ != 0 && state_ && state_.get() == device.state_.get();
}

std::expected<Image2D, Diagnostic> Image2D::create(
    const Device& device,
    std::uint32_t width,
    std::uint32_t height,
    ImageFormat format) {
    if (!device.state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_context_access,
            .message = "Image2D::create received an empty device",
        });
    }
    if (width == 0 || height == 0
        || width > static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max())
        || height > static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max())) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D::create requires non-zero GLsizei-sized dimensions",
        });
    }
    if (gl_internal_format(format) == GL_NONE) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D::create received an unsupported format",
        });
    }
    if (auto current = device.state_->require_current("Image2D::create"); !current) {
        return std::unexpected(std::move(current.error()));
    }

    GLint max_size = 0;
    if (auto queried = detail::checked_gl_call(
            "glGetIntegerv(GL_MAX_TEXTURE_SIZE)",
            [&] { glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size); });
        !queried) {
        return std::unexpected(std::move(queried.error()));
    }
    if (max_size <= 0
        || width > static_cast<std::uint32_t>(max_size)
        || height > static_cast<std::uint32_t>(max_size)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D dimensions exceed GL_MAX_TEXTURE_SIZE ("
                + std::to_string(max_size) + ')',
        });
    }

    GLuint handle = 0;
    if (auto created = detail::checked_gl_call(
            "glCreateTextures(GL_TEXTURE_2D)",
            [&] { glCreateTextures(GL_TEXTURE_2D, 1, &handle); },
            ErrorCode::object_creation_failed);
        !created) {
        delete_failed_image(handle);
        return std::unexpected(std::move(created.error()));
    }
    if (handle == 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::object_creation_failed,
            .message = "glCreateTextures returned zero",
        });
    }

    if (auto allocated = detail::checked_gl_call(
            "glTextureStorage2D",
            [&] {
                glTextureStorage2D(
                    handle,
                    1,
                    gl_internal_format(format),
                    static_cast<GLsizei>(width),
                    static_cast<GLsizei>(height));
            });
        !allocated) {
        delete_failed_image(handle);
        return std::unexpected(std::move(allocated.error()));
    }

    if (auto configured = detail::checked_gl_call(
            "glTextureParameteri(render-target defaults)",
            [&] {
                glTextureParameteri(handle, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTextureParameteri(handle, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTextureParameteri(handle, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTextureParameteri(handle, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                if (is_depth_format(format)) {
                    glTextureParameteri(
                        handle,
                        GL_TEXTURE_COMPARE_MODE,
                        GL_NONE);
                }
            });
        !configured) {
        delete_failed_image(handle);
        return std::unexpected(std::move(configured.error()));
    }

    return Image2D(device.state_, handle, width, height, format);
}

std::expected<void, Diagnostic> Image2D::bind_to_unit(std::uint32_t unit) const {
    if (handle_ == 0 || !state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D::bind_to_unit called on an empty image",
        });
    }
    if (auto current = state_->require_current("Image2D::bind_to_unit"); !current) {
        return current;
    }

    GLint max_units = 0;
    if (auto queried = detail::checked_gl_call(
            "glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS)",
            [&] {
                glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &max_units);
            });
        !queried) {
        return queried;
    }
    if (max_units <= 0 || unit >= static_cast<std::uint32_t>(max_units)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D texture unit exceeds GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS",
        });
    }
    return detail::checked_gl_call(
        "glBindTextureUnit",
        [&] { glBindTextureUnit(unit, handle_); });
}

std::expected<void, Diagnostic> Image2D::clear_rgba8(
    const std::array<float, 4>& color) const {
    if (handle_ == 0 || !state_ || format_ != ImageFormat::rgba8) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D::clear_rgba8 requires a non-empty rgba8 image",
        });
    }
    if (auto current = state_->require_current("Image2D::clear_rgba8"); !current) {
        return current;
    }
    return detail::checked_gl_call(
        "glClearTexImage(rgba8)",
        [&] { glClearTexImage(handle_, 0, GL_RGBA, GL_FLOAT, color.data()); });
}

std::expected<void, Diagnostic> Image2D::clear_rg32ui(
    const std::array<std::uint32_t, 2>& value) const {
    if (handle_ == 0 || !state_ || format_ != ImageFormat::rg32ui) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D::clear_rg32ui requires a non-empty rg32ui image",
        });
    }
    if (auto current = state_->require_current("Image2D::clear_rg32ui"); !current) {
        return current;
    }
    return detail::checked_gl_call(
        "glClearTexImage(rg32ui)",
        [&] {
            glClearTexImage(
                handle_, 0, GL_RG_INTEGER, GL_UNSIGNED_INT, value.data());
        });
}

std::expected<void, Diagnostic> Image2D::clear_rgba32f(
    const std::array<float, 4>& value) const {
    if (handle_ == 0 || !state_ || format_ != ImageFormat::rgba32f) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D::clear_rgba32f requires a non-empty rgba32f image",
        });
    }
    if (auto current = state_->require_current("Image2D::clear_rgba32f"); !current) {
        return current;
    }
    return detail::checked_gl_call(
        "glClearTexImage(rgba32f)",
        [&] { glClearTexImage(handle_, 0, GL_RGBA, GL_FLOAT, value.data()); });
}

std::expected<void, Diagnostic> Image2D::clear_rgba32i(
    const std::array<std::int32_t, 4>& value) const {
    if (handle_ == 0 || !state_ || format_ != ImageFormat::rgba32i) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D::clear_rgba32i requires a non-empty rgba32i image",
        });
    }
    if (auto current = state_->require_current("Image2D::clear_rgba32i"); !current) {
        return current;
    }
    return detail::checked_gl_call(
        "glClearTexImage(rgba32i)",
        [&] { glClearTexImage(handle_, 0, GL_RGBA_INTEGER, GL_INT, value.data()); });
}

std::expected<void, Diagnostic> Image2D::clear_rgba32ui(
    const std::array<std::uint32_t, 4>& value) const {
    if (handle_ == 0 || !state_ || format_ != ImageFormat::rgba32ui) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D::clear_rgba32ui requires a non-empty rgba32ui image",
        });
    }
    if (auto current = state_->require_current("Image2D::clear_rgba32ui"); !current) {
        return current;
    }
    return detail::checked_gl_call(
        "glClearTexImage(rgba32ui)",
        [&] {
            glClearTexImage(
                handle_, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, value.data());
        });
}

std::expected<void, Diagnostic> Image2D::clear_depth32f(float depth) const {
    if (handle_ == 0 || !state_ || format_ != ImageFormat::depth32f) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D::clear_depth32f requires a non-empty depth32f image",
        });
    }
    if (!std::isfinite(depth) || depth < 0.0F || depth > 1.0F) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Image2D::clear_depth32f requires a finite value in [0, 1]",
        });
    }
    if (auto current = state_->require_current("Image2D::clear_depth32f"); !current) {
        return current;
    }
    return detail::checked_gl_call(
        "glClearTexImage(depth32f)",
        [&] {
            glClearTexImage(
                handle_, 0, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
        });
}

std::expected<void, Diagnostic> Image2D::destroy() {
    if (handle_ == 0) {
        return {};
    }
    if (!state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_context_access,
            .message = "Image2D::destroy has no device state",
        });
    }
    if (auto current = state_->require_current("Image2D::destroy"); !current) {
        return current;
    }
    const GLuint handle = handle_;
    glDeleteTextures(1, &handle);
    handle_ = 0;
    width_ = 0;
    height_ = 0;
    state_.reset();
    return {};
}

void Image2D::release_noexcept() noexcept {
    if (handle_ == 0) {
        return;
    }
    if (state_ && state_->is_current()) {
        const GLuint handle = handle_;
        glDeleteTextures(1, &handle);
    } else if (state_) {
        state_->record_lifecycle_failure(
            "Image2D destroyed without its owning context current; native handle was leaked safely");
    }
    handle_ = 0;
    width_ = 0;
    height_ = 0;
    state_.reset();
}

} // namespace vng::opengl
