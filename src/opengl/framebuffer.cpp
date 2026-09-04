#include <vng/opengl/framebuffer.hpp>

#include <vng/opengl/device.hpp>
#include <vng/opengl/image.hpp>
#include <vng/opengl/renderbuffer.hpp>

#include "context_state.hpp"
#include "gl_error.hpp"

#include <glad/gl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <utility>

namespace vng::opengl {
namespace {

[[nodiscard]] const char* framebuffer_status_name(GLenum status) noexcept {
    switch (status) {
    case GL_FRAMEBUFFER_COMPLETE: return "complete";
    case GL_FRAMEBUFFER_UNDEFINED: return "undefined";
    case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT: return "incomplete attachment";
    case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT: return "missing attachment";
    case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER: return "incomplete draw buffer";
    case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER: return "incomplete read buffer";
    case GL_FRAMEBUFFER_UNSUPPORTED: return "unsupported";
    case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE: return "incomplete multisample";
    case GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS: return "incomplete layer targets";
    default: return "unknown";
    }
}

[[nodiscard]] std::expected<std::uint32_t, Diagnostic> query_positive_limit(
    GLenum name,
    std::string_view label) {
    GLint value = 0;
    if (auto queried = detail::checked_gl_call(
            std::string("glGetIntegerv(") + std::string(label) + ')',
            [&] { glGetIntegerv(name, &value); });
        !queried) {
        return std::unexpected(std::move(queried.error()));
    }
    if (value <= 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::operation_failed,
            .message = "OpenGL reported an invalid " + std::string(label),
        });
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] std::vector<GLenum> draw_buffers_for(
    const std::vector<std::uint32_t>& attachments) {
    std::vector<GLenum> draw_buffers(attachments.back() + 1, GL_NONE);
    for (const auto index : attachments) {
        draw_buffers[index] = GL_COLOR_ATTACHMENT0 + index;
    }
    return draw_buffers;
}

class ReadbackState final {
public:
    explicit ReadbackState(GLuint framebuffer) noexcept {
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read_framebuffer_);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previous_pixel_pack_buffer_);
        glGetIntegerv(GL_PACK_ALIGNMENT, &previous_pack_alignment_);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &previous_pack_row_length_);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &previous_pack_skip_pixels_);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &previous_pack_skip_rows_);
        glGetIntegerv(GL_PACK_IMAGE_HEIGHT, &previous_pack_image_height_);
        glGetIntegerv(GL_PACK_SKIP_IMAGES, &previous_pack_skip_images_);
        glGetIntegerv(GL_PACK_SWAP_BYTES, &previous_pack_swap_bytes_);
        glGetIntegerv(GL_PACK_LSB_FIRST, &previous_pack_lsb_first_);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
        glGetIntegerv(GL_READ_BUFFER, &framebuffer_read_buffer_);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_IMAGE_HEIGHT, 0);
        glPixelStorei(GL_PACK_SKIP_IMAGES, 0);
        glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
        glPixelStorei(GL_PACK_LSB_FIRST, GL_FALSE);
    }

    ReadbackState(const ReadbackState&) = delete;
    ReadbackState& operator=(const ReadbackState&) = delete;

    ~ReadbackState() {
        glPixelStorei(GL_PACK_ALIGNMENT, previous_pack_alignment_);
        glPixelStorei(GL_PACK_ROW_LENGTH, previous_pack_row_length_);
        glPixelStorei(GL_PACK_SKIP_PIXELS, previous_pack_skip_pixels_);
        glPixelStorei(GL_PACK_SKIP_ROWS, previous_pack_skip_rows_);
        glPixelStorei(GL_PACK_IMAGE_HEIGHT, previous_pack_image_height_);
        glPixelStorei(GL_PACK_SKIP_IMAGES, previous_pack_skip_images_);
        glPixelStorei(GL_PACK_SWAP_BYTES, previous_pack_swap_bytes_);
        glPixelStorei(GL_PACK_LSB_FIRST, previous_pack_lsb_first_);
        glBindBuffer(
            GL_PIXEL_PACK_BUFFER,
            static_cast<GLuint>(previous_pixel_pack_buffer_));
        glReadBuffer(static_cast<GLenum>(framebuffer_read_buffer_));
        glBindFramebuffer(
            GL_READ_FRAMEBUFFER,
            static_cast<GLuint>(previous_read_framebuffer_));
    }

private:
    GLint previous_read_framebuffer_{};
    GLint previous_pixel_pack_buffer_{};
    GLint previous_pack_alignment_{};
    GLint previous_pack_row_length_{};
    GLint previous_pack_skip_pixels_{};
    GLint previous_pack_skip_rows_{};
    GLint previous_pack_image_height_{};
    GLint previous_pack_skip_images_{};
    GLint previous_pack_swap_bytes_{};
    GLint previous_pack_lsb_first_{};
    GLint framebuffer_read_buffer_{};
};

template<class Pixel>
[[nodiscard]] std::expected<std::vector<Pixel>, Diagnostic> read_pixels(
    GLuint framebuffer,
    std::optional<std::uint32_t> color_attachment,
    std::int32_t x,
    std::int32_t y,
    std::uint32_t width,
    std::uint32_t height,
    GLenum format,
    GLenum type,
    std::string_view operation) {
    static_assert(std::is_trivially_copyable_v<Pixel>);
    if (width == 0 || height == 0
        || width > static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max())
        || height > static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max())
        || width > std::numeric_limits<std::size_t>::max() / sizeof(Pixel) / height) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = std::string(operation) + " received invalid dimensions",
        });
    }

    std::vector<Pixel> pixels(static_cast<std::size_t>(width) * height);
    ReadbackState state(framebuffer);
    if (color_attachment) {
        glReadBuffer(GL_COLOR_ATTACHMENT0 + *color_attachment);
    }
    if (auto read = detail::checked_gl_call(
            operation,
            [&] {
                glReadPixels(
                    x,
                    y,
                    static_cast<GLsizei>(width),
                    static_cast<GLsizei>(height),
                    format,
                    type,
                    pixels.data());
            });
        !read) {
        return std::unexpected(std::move(read.error()));
    }
    return pixels;
}

template<class Pixel>
[[nodiscard]] std::expected<std::vector<Pixel>, Diagnostic>
read_typed_color_pixels(
    GLuint framebuffer,
    const std::shared_ptr<detail::ContextState>& state,
    const std::vector<std::uint32_t>& attachments,
    const std::unordered_map<std::uint32_t, std::uint32_t>& samples,
    const std::unordered_map<std::uint32_t, ImageFormat>& formats,
    std::uint32_t attachment,
    ImageFormat required_format,
    std::int32_t x,
    std::int32_t y,
    std::uint32_t width,
    std::uint32_t height,
    GLenum pixel_format,
    GLenum component_type,
    std::string_view operation)
{
    if (framebuffer == 0 || !state) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = std::string(operation) + " called on an empty framebuffer",
        });
    }
    if (std::ranges::find(attachments, attachment) == attachments.end()) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = std::string(operation) + " references an unattached color target",
        });
    }
    if (formats.at(attachment) != required_format) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = std::string(operation) + " received the wrong attachment format",
        });
    }
    if (samples.at(attachment) != 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = std::string(operation)
                + " cannot read a multisample attachment directly; resolve it first",
        });
    }
    if (auto current = state->require_current(operation); !current) {
        return std::unexpected(std::move(current.error()));
    }
    return read_pixels<Pixel>(
        framebuffer,
        attachment,
        x,
        y,
        width,
        height,
        pixel_format,
        component_type,
        operation);
}

} // namespace

Framebuffer::Framebuffer(
    std::shared_ptr<detail::ContextState> state,
    std::uint32_t handle) noexcept
    : state_(std::move(state)), handle_(handle) {}

Framebuffer::Framebuffer(Framebuffer&& other) noexcept
    : state_(std::move(other.state_)),
      handle_(std::exchange(other.handle_, 0)),
      color_attachments_(std::move(other.color_attachments_)),
      color_attachment_samples_(std::move(other.color_attachment_samples_)),
      color_attachment_handles_(std::move(other.color_attachment_handles_)),
      color_attachment_formats_(std::move(other.color_attachment_formats_)),
      color_attachment_objects_(std::move(other.color_attachment_objects_)),
      depth_attachment_format_(std::exchange(other.depth_attachment_format_, std::nullopt)) {}

Framebuffer& Framebuffer::operator=(Framebuffer&& other) noexcept {
    if (this != &other) {
        release_noexcept();
        state_ = std::move(other.state_);
        handle_ = std::exchange(other.handle_, 0);
        color_attachments_ = std::move(other.color_attachments_);
        color_attachment_samples_ = std::move(other.color_attachment_samples_);
        color_attachment_handles_ = std::move(other.color_attachment_handles_);
        color_attachment_formats_ = std::move(other.color_attachment_formats_);
        color_attachment_objects_ = std::move(other.color_attachment_objects_);
        depth_attachment_format_ = std::exchange(
            other.depth_attachment_format_, std::nullopt);
    }
    return *this;
}

Framebuffer::~Framebuffer() {
    release_noexcept();
}

std::expected<Framebuffer, Diagnostic> Framebuffer::create(const Device& device) {
    if (!device.state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_context_access,
            .message = "Framebuffer::create received an empty device",
        });
    }
    if (auto current = device.state_->require_current("Framebuffer::create"); !current) {
        return std::unexpected(std::move(current.error()));
    }
    GLuint handle = 0;
    if (auto created = detail::checked_gl_call(
            "glCreateFramebuffers",
            [&] { glCreateFramebuffers(1, &handle); },
            ErrorCode::object_creation_failed);
        !created) {
        if (handle != 0) {
            glDeleteFramebuffers(1, &handle);
            detail::clear_gl_errors();
        }
        return std::unexpected(std::move(created.error()));
    }
    if (handle == 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::object_creation_failed,
            .message = "glCreateFramebuffers returned zero",
        });
    }
    return Framebuffer(device.state_, handle);
}

std::expected<void, Diagnostic> Framebuffer::attach_color(
    std::uint32_t attachment,
    const Renderbuffer& renderbuffer) {
    if (handle_ == 0 || !state_ || renderbuffer.handle_ == 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::attach_color received an empty object",
        });
    }
    if (renderbuffer.state_.get() != state_.get()) {
        return std::unexpected(detail::incompatible_device("Renderbuffer"));
    }
    if (renderbuffer.format_ != RenderbufferFormat::rgba8) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::attach_color requires a color renderbuffer",
        });
    }
    return attach_color_storage(
        attachment,
        renderbuffer.handle_,
        renderbuffer.samples_,
        ImageFormat::rgba8,
        AttachmentObject::renderbuffer);
}

std::expected<void, Diagnostic> Framebuffer::attach_color(
    std::uint32_t attachment,
    const Image2D& image) {
    if (handle_ == 0 || !state_ || image.handle_ == 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::attach_color received an empty object",
        });
    }
    if (image.state_.get() != state_.get()) {
        return std::unexpected(detail::incompatible_device("Image2D"));
    }
    if (!is_color_format(image.format_)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::attach_color requires a color image",
        });
    }
    return attach_color_storage(
        attachment,
        image.handle_,
        0,
        image.format_,
        AttachmentObject::image);
}

std::expected<void, Diagnostic> Framebuffer::attach_color_storage(
    std::uint32_t attachment,
    std::uint32_t storage_handle,
    std::uint32_t samples,
    ImageFormat format,
    AttachmentObject object) {
    if (auto current = state_->require_current("Framebuffer::attach_color"); !current) {
        return current;
    }
    auto max_attachments = query_positive_limit(
        GL_MAX_COLOR_ATTACHMENTS, "GL_MAX_COLOR_ATTACHMENTS");
    if (!max_attachments) {
        return std::unexpected(std::move(max_attachments.error()));
    }
    auto max_draw_buffers = query_positive_limit(
        GL_MAX_DRAW_BUFFERS, "GL_MAX_DRAW_BUFFERS");
    if (!max_draw_buffers) {
        return std::unexpected(std::move(max_draw_buffers.error()));
    }
    if (attachment >= *max_attachments) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer color attachment exceeds GL_MAX_COLOR_ATTACHMENTS",
        });
    }
    if (attachment >= *max_draw_buffers) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer color attachment exceeds GL_MAX_DRAW_BUFFERS",
        });
    }

    auto next_attachments = color_attachments_;
    if (std::ranges::find(next_attachments, attachment) == next_attachments.end()) {
        next_attachments.push_back(attachment);
        std::ranges::sort(next_attachments);
    }
    auto next_samples = color_attachment_samples_;
    next_samples[attachment] = samples;
    auto next_handles = color_attachment_handles_;
    const auto previous = next_handles.find(attachment);
    const GLuint previous_handle = previous == next_handles.end() ? 0 : previous->second;
    next_handles[attachment] = storage_handle;
    auto next_formats = color_attachment_formats_;
    next_formats[attachment] = format;
    auto next_objects = color_attachment_objects_;
    const auto previous_object_it = next_objects.find(attachment);
    const auto previous_object = previous_object_it == next_objects.end()
        ? std::optional<AttachmentObject>{}
        : std::optional<AttachmentObject>{previous_object_it->second};
    next_objects[attachment] = object;
    auto draw_buffers = draw_buffers_for(next_attachments);

    const auto attach_storage = [&](AttachmentObject storage_object, GLuint storage) {
        if (storage_object == AttachmentObject::image) {
            glNamedFramebufferTexture(
                handle_,
                GL_COLOR_ATTACHMENT0 + attachment,
                storage,
                0);
        } else {
            glNamedFramebufferRenderbuffer(
                handle_,
                GL_COLOR_ATTACHMENT0 + attachment,
                GL_RENDERBUFFER,
                storage);
        }
    };

    if (auto attached = detail::checked_gl_call(
            object == AttachmentObject::image
                ? "glNamedFramebufferTexture(color)"
                : "glNamedFramebufferRenderbuffer(color)",
            [&] { attach_storage(object, storage_handle); });
        !attached) {
        return attached;
    }
    if (auto configured = detail::checked_gl_call(
            "glNamedFramebufferDrawBuffers",
            [&] {
                glNamedFramebufferDrawBuffers(
                    handle_,
                    static_cast<GLsizei>(draw_buffers.size()),
                    draw_buffers.data());
            });
        !configured) {
        auto diagnostic = std::move(configured.error());
        if (auto rolled_back = detail::checked_gl_call(
                "Framebuffer color attachment rollback",
                [&] {
                    if (previous_object) {
                        attach_storage(*previous_object, previous_handle);
                    } else {
                        glNamedFramebufferTexture(
                            handle_,
                            GL_COLOR_ATTACHMENT0 + attachment,
                            0,
                            0);
                    }
                });
            !rolled_back) {
            diagnostic.message += "; attachment rollback also failed: "
                + rolled_back.error().message;
        }
        return std::unexpected(std::move(diagnostic));
    }

    color_attachments_ = std::move(next_attachments);
    color_attachment_samples_ = std::move(next_samples);
    color_attachment_handles_ = std::move(next_handles);
    color_attachment_formats_ = std::move(next_formats);
    color_attachment_objects_ = std::move(next_objects);
    return {};
}

std::expected<void, Diagnostic> Framebuffer::attach_depth_stencil(
    const Renderbuffer& renderbuffer) {
    if (handle_ == 0 || !state_ || renderbuffer.handle_ == 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::attach_depth_stencil received an empty object",
        });
    }
    if (renderbuffer.state_.get() != state_.get()) {
        return std::unexpected(detail::incompatible_device("Renderbuffer"));
    }
    if (renderbuffer.format_ != RenderbufferFormat::depth24_stencil8) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::attach_depth_stencil requires a depth/stencil renderbuffer",
        });
    }
    if (auto current = state_->require_current("Framebuffer::attach_depth_stencil"); !current) {
        return current;
    }
    auto attached = detail::checked_gl_call(
        "glNamedFramebufferRenderbuffer(depth/stencil)",
        [&] {
            glNamedFramebufferRenderbuffer(
                handle_,
                GL_DEPTH_STENCIL_ATTACHMENT,
                GL_RENDERBUFFER,
                renderbuffer.handle_);
        });
    if (!attached) {
        return attached;
    }
    depth_attachment_format_ = DepthAttachmentFormat::depth24_stencil8;
    return {};
}

std::expected<void, Diagnostic> Framebuffer::attach_depth(const Image2D& image) {
    if (handle_ == 0 || !state_ || image.handle_ == 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::attach_depth received an empty object",
        });
    }
    if (image.state_.get() != state_.get()) {
        return std::unexpected(detail::incompatible_device("Image2D"));
    }
    if (image.format_ != ImageFormat::depth32f) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::attach_depth requires a depth32f image",
        });
    }
    if (auto current = state_->require_current("Framebuffer::attach_depth"); !current) {
        return current;
    }
    auto attached = detail::checked_gl_call(
        "glNamedFramebufferTexture(depth)",
        [&] {
            glNamedFramebufferTexture(
                handle_,
                GL_DEPTH_ATTACHMENT,
                image.handle_,
                0);
        });
    if (!attached) {
        return attached;
    }
    depth_attachment_format_ = DepthAttachmentFormat::depth32f;
    return {};
}

std::expected<void, Diagnostic> Framebuffer::check_complete() const {
    if (handle_ == 0 || !state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::check_complete called on an empty framebuffer",
        });
    }
    if (auto current = state_->require_current("Framebuffer::check_complete"); !current) {
        return current;
    }
    const GLenum status = glCheckNamedFramebufferStatus(handle_, GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::ostringstream message;
        message << "Framebuffer is " << framebuffer_status_name(status)
                << " (0x" << std::hex << status << ')';
        return std::unexpected(Diagnostic{
            .code = ErrorCode::framebuffer_incomplete,
            .message = std::move(message).str(),
        });
    }
    return {};
}

std::expected<void, Diagnostic> Framebuffer::bind() const {
    if (handle_ == 0 || !state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::bind called on an empty framebuffer",
        });
    }
    if (auto current = state_->require_current("Framebuffer::bind"); !current) {
        return current;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, handle_);
    return {};
}

std::expected<void, Diagnostic> Framebuffer::clear_color(
    std::uint32_t attachment,
    const std::array<float, 4>& color) const {
    if (handle_ == 0 || !state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::clear_color called on an empty framebuffer",
        });
    }
    if (auto current = state_->require_current("Framebuffer::clear_color"); !current) {
        return current;
    }
    if (std::ranges::find(color_attachments_, attachment) == color_attachments_.end()) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::clear_color references an unattached color target",
        });
    }
    if (color_attachment_formats_.at(attachment) != ImageFormat::rgba8) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::clear_color(float) requires an rgba8 target",
        });
    }
    return detail::checked_gl_call(
        "glClearNamedFramebufferfv(color)",
        [&] {
            glClearNamedFramebufferfv(
                handle_,
                GL_COLOR,
                static_cast<GLint>(attachment),
                color.data());
        });
}

std::expected<void, Diagnostic> Framebuffer::clear_color(
    std::uint32_t attachment,
    Rg32uiPixel color) const {
    if (handle_ == 0 || !state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::clear_color(Rg32uiPixel) called on an empty framebuffer",
        });
    }
    if (auto current = state_->require_current("Framebuffer::clear_color(Rg32uiPixel)"); !current) {
        return current;
    }
    if (std::ranges::find(color_attachments_, attachment) == color_attachments_.end()) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::clear_color(Rg32uiPixel) references an unattached color target",
        });
    }
    if (color_attachment_formats_.at(attachment) != ImageFormat::rg32ui) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::clear_color(Rg32uiPixel) requires an rg32ui target",
        });
    }
    const std::array values{color.r, color.g, 0U, 0U};
    return detail::checked_gl_call(
        "glClearNamedFramebufferuiv(color)",
        [&] {
            glClearNamedFramebufferuiv(
                handle_,
                GL_COLOR,
                static_cast<GLint>(attachment),
                values.data());
        });
}

std::expected<void, Diagnostic> Framebuffer::clear_depth(float depth) const {
    if (handle_ == 0 || !state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::clear_depth called on an empty framebuffer",
        });
    }
    if (!depth_attachment_format_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::clear_depth requires an attached depth target",
        });
    }
    if (!std::isfinite(depth) || depth < 0.0F || depth > 1.0F) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::clear_depth requires a finite value in [0, 1]",
        });
    }
    if (auto current = state_->require_current("Framebuffer::clear_depth"); !current) {
        return current;
    }
    return detail::checked_gl_call(
        "glClearNamedFramebufferfv(depth)",
        [&] { glClearNamedFramebufferfv(handle_, GL_DEPTH, 0, &depth); });
}

std::expected<std::vector<Rgba8Pixel>, Diagnostic> Framebuffer::read_rgba8_pixels(
    std::uint32_t attachment,
    std::int32_t x,
    std::int32_t y,
    std::uint32_t width,
    std::uint32_t height) const {
    static_assert(sizeof(Rgba8Pixel) == 4);
    if (handle_ == 0 || !state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::read_rgba8_pixels called on an empty framebuffer",
        });
    }
    if (std::ranges::find(color_attachments_, attachment) == color_attachments_.end()) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::read_rgba8_pixels references an unattached color target",
        });
    }
    if (color_attachment_formats_.at(attachment) != ImageFormat::rgba8) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::read_rgba8_pixels requires an rgba8 target",
        });
    }
    if (color_attachment_samples_.at(attachment) != 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::read_rgba8_pixels cannot read a multisample attachment directly; resolve it first",
        });
    }
    if (auto current = state_->require_current("Framebuffer::read_rgba8_pixels"); !current) {
        return std::unexpected(std::move(current.error()));
    }
    return read_pixels<Rgba8Pixel>(
        handle_,
        attachment,
        x,
        y,
        width,
        height,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        "Framebuffer::read_rgba8_pixels");
}

std::expected<std::vector<std::byte>, Diagnostic> Framebuffer::read_rgba8(
    std::uint32_t attachment,
    std::int32_t x,
    std::int32_t y,
    std::uint32_t width,
    std::uint32_t height) const {
    auto pixels = read_rgba8_pixels(attachment, x, y, width, height);
    if (!pixels) {
        return std::unexpected(std::move(pixels.error()));
    }
    std::vector<std::byte> bytes(pixels->size() * sizeof(Rgba8Pixel));
    std::memcpy(bytes.data(), pixels->data(), bytes.size());
    return bytes;
}

std::expected<std::vector<Rg32uiPixel>, Diagnostic> Framebuffer::read_rg32ui(
    std::uint32_t attachment,
    std::int32_t x,
    std::int32_t y,
    std::uint32_t width,
    std::uint32_t height) const {
    static_assert(sizeof(Rg32uiPixel) == 2 * sizeof(std::uint32_t));
    if (handle_ == 0 || !state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::read_rg32ui called on an empty framebuffer",
        });
    }
    if (std::ranges::find(color_attachments_, attachment) == color_attachments_.end()) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::read_rg32ui references an unattached color target",
        });
    }
    if (color_attachment_formats_.at(attachment) != ImageFormat::rg32ui) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::read_rg32ui requires an rg32ui target",
        });
    }
    if (color_attachment_samples_.at(attachment) != 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::read_rg32ui cannot read a multisample attachment directly; resolve it first",
        });
    }
    if (auto current = state_->require_current("Framebuffer::read_rg32ui"); !current) {
        return std::unexpected(std::move(current.error()));
    }
    return read_pixels<Rg32uiPixel>(
        handle_,
        attachment,
        x,
        y,
        width,
        height,
        GL_RG_INTEGER,
        GL_UNSIGNED_INT,
        "Framebuffer::read_rg32ui");
}

std::expected<std::vector<Rgba32fPixel>, Diagnostic> Framebuffer::read_rgba32f(
    std::uint32_t attachment,
    std::int32_t x,
    std::int32_t y,
    std::uint32_t width,
    std::uint32_t height) const {
    static_assert(sizeof(Rgba32fPixel) == 4 * sizeof(float));
    return read_typed_color_pixels<Rgba32fPixel>(
        handle_, state_, color_attachments_, color_attachment_samples_,
        color_attachment_formats_, attachment, ImageFormat::rgba32f,
        x, y, width, height, GL_RGBA, GL_FLOAT,
        "Framebuffer::read_rgba32f");
}

std::expected<std::vector<Rgba32iPixel>, Diagnostic> Framebuffer::read_rgba32i(
    std::uint32_t attachment,
    std::int32_t x,
    std::int32_t y,
    std::uint32_t width,
    std::uint32_t height) const {
    static_assert(sizeof(Rgba32iPixel) == 4 * sizeof(std::int32_t));
    return read_typed_color_pixels<Rgba32iPixel>(
        handle_, state_, color_attachments_, color_attachment_samples_,
        color_attachment_formats_, attachment, ImageFormat::rgba32i,
        x, y, width, height, GL_RGBA_INTEGER, GL_INT,
        "Framebuffer::read_rgba32i");
}

std::expected<std::vector<Rgba32uiPixel>, Diagnostic> Framebuffer::read_rgba32ui(
    std::uint32_t attachment,
    std::int32_t x,
    std::int32_t y,
    std::uint32_t width,
    std::uint32_t height) const {
    static_assert(sizeof(Rgba32uiPixel) == 4 * sizeof(std::uint32_t));
    return read_typed_color_pixels<Rgba32uiPixel>(
        handle_, state_, color_attachments_, color_attachment_samples_,
        color_attachment_formats_, attachment, ImageFormat::rgba32ui,
        x, y, width, height, GL_RGBA_INTEGER, GL_UNSIGNED_INT,
        "Framebuffer::read_rgba32ui");
}

std::expected<std::vector<float>, Diagnostic> Framebuffer::read_depth32f(
    std::int32_t x,
    std::int32_t y,
    std::uint32_t width,
    std::uint32_t height) const {
    if (handle_ == 0 || !state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::read_depth32f called on an empty framebuffer",
        });
    }
    if (depth_attachment_format_ != DepthAttachmentFormat::depth32f) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::read_depth32f requires an attached depth32f image",
        });
    }
    if (auto current = state_->require_current("Framebuffer::read_depth32f"); !current) {
        return std::unexpected(std::move(current.error()));
    }
    return read_pixels<float>(
        handle_,
        std::nullopt,
        x,
        y,
        width,
        height,
        GL_DEPTH_COMPONENT,
        GL_FLOAT,
        "Framebuffer::read_depth32f");
}

std::expected<void, Diagnostic> Framebuffer::destroy() {
    if (handle_ == 0) {
        return {};
    }
    if (!state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_context_access,
            .message = "Framebuffer::destroy has no device state",
        });
    }
    if (auto current = state_->require_current("Framebuffer::destroy"); !current) {
        return current;
    }
    const GLuint handle = handle_;
    glDeleteFramebuffers(1, &handle);
    handle_ = 0;
    color_attachments_.clear();
    color_attachment_samples_.clear();
    color_attachment_handles_.clear();
    color_attachment_formats_.clear();
    color_attachment_objects_.clear();
    depth_attachment_format_.reset();
    state_.reset();
    return {};
}

void Framebuffer::release_noexcept() noexcept {
    if (handle_ == 0) {
        return;
    }
    if (state_ && state_->is_current()) {
        const GLuint handle = handle_;
        glDeleteFramebuffers(1, &handle);
    } else if (state_) {
        state_->record_lifecycle_failure(
            "Framebuffer destroyed without its owning context current; native handle was leaked safely");
    }
    handle_ = 0;
    color_attachments_.clear();
    color_attachment_samples_.clear();
    color_attachment_handles_.clear();
    color_attachment_formats_.clear();
    color_attachment_objects_.clear();
    depth_attachment_format_.reset();
    state_.reset();
}

} // namespace vng::opengl
