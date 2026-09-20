#include <vng/opengl/framebuffer.hpp>

#include <vng/opengl/device.hpp>
#include <vng/opengl/image.hpp>
#include <vng/opengl/renderbuffer.hpp>

#include "context_state.hpp"
#include "gl_error.hpp"
#include "readback_state.hpp"

#include <glad/gl.h>

#include <algorithm>
#include <cmath>
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

using detail::ReadbackState;

template<class Pixel, std::size_t ElementsPerPixel = 1>
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
    static_assert(ElementsPerPixel > 0);
    if (width == 0 || height == 0
        || width > static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max())
        || height > static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max())
        || width > std::numeric_limits<std::size_t>::max() / sizeof(Pixel) / ElementsPerPixel / height) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = std::string(operation) + " received invalid dimensions",
        });
    }

    std::vector<Pixel> pixels(static_cast<std::size_t>(width) * height * ElementsPerPixel);
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

template<class Pixel, std::size_t ElementsPerPixel = 1>
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
    if (formats.at(attachment) != required_format
        && !(required_format == ImageFormat::rgba8 && formats.at(attachment) == ImageFormat::srgb8_alpha8)
        && !(required_format == ImageFormat::rgba32f && formats.at(attachment) == ImageFormat::rgba16f)) {
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
    return read_pixels<Pixel, ElementsPerPixel>(
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
      color_attachment_extents_(std::move(other.color_attachment_extents_)),
      depth_attachment_format_(std::exchange(other.depth_attachment_format_, std::nullopt)),
      depth_extent_(std::exchange(other.depth_extent_, {})),
      depth_image_handle_(std::exchange(other.depth_image_handle_, 0)) {}

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
        color_attachment_extents_ = std::move(other.color_attachment_extents_);
        depth_attachment_format_ = std::exchange(
            other.depth_attachment_format_, std::nullopt);
        depth_extent_ = std::exchange(other.depth_extent_, {});
        depth_image_handle_ = std::exchange(other.depth_image_handle_, 0);
    }
    return *this;
}

Framebuffer::~Framebuffer() {
    release_noexcept();
}

bool Framebuffer::belongs_to(const Device& device) const noexcept {
    return handle_ && state_ && state_.get() == device.state_.get();
}

Extent2D Framebuffer::extent() const noexcept {
    if (!handle_) return {};
    auto result = depth_extent_;
    for (const auto& [attachment, area] : color_attachment_extents_) {
        (void)attachment;
        result = result.empty() ? area : Extent2D{
            std::min(result.width, area.width), std::min(result.height, area.height)};
    }
    return result;
}

std::optional<render::ColorEncoding> Framebuffer::color_encoding() const noexcept {
    std::optional<render::ColorEncoding> encoding;
    for (const auto& [attachment, format] : color_attachment_formats_) {
        (void)attachment;
        const auto next = format == ImageFormat::srgb8_alpha8
            ? render::ColorEncoding::srgb : render::ColorEncoding::linear;
        if (encoding && *encoding != next) return std::nullopt;
        encoding = next;
    }
    return encoding;
}

bool Framebuffer::has_color_image(u32 handle) const noexcept {
    if (!handle) return false;
    for (const auto& [attachment, object] : color_attachment_objects_)
        if (object == AttachmentObject::image && color_attachment_handles_.at(attachment) == handle)
            return true;
    return false;
}

std::vector<u32> Framebuffer::attachment_image_handles() const {
    std::vector<u32> handles;
    for (const auto& [attachment, object] : color_attachment_objects_)
        if (object == AttachmentObject::image) handles.push_back(color_attachment_handles_.at(attachment));
    if (depth_image_handle_) handles.push_back(depth_image_handle_);
    return handles;
}

bool Framebuffer::has_integer_color() const noexcept {
    return std::ranges::any_of(color_attachment_formats_, [](const auto& entry) {
        return gfx::is_integer_format(entry.second);
    });
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
        AttachmentObject::renderbuffer,
        {renderbuffer.width_, renderbuffer.height_});
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
        AttachmentObject::image,
        image.extent());
}

std::expected<void, Diagnostic> Framebuffer::attach_color_storage(
    std::uint32_t attachment,
    std::uint32_t storage_handle,
    std::uint32_t samples,
    ImageFormat format,
    AttachmentObject object,
    Extent2D extent) {
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
    auto next_extents = color_attachment_extents_;
    next_extents[attachment] = extent;
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
    color_attachment_extents_ = std::move(next_extents);
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
    depth_extent_ = {renderbuffer.width_, renderbuffer.height_};
    depth_image_handle_ = 0;
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
    depth_extent_ = image.extent();
    depth_image_handle_ = image.native_handle();
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
    GLenum status{};
    if (auto checked = detail::checked_gl_call("glCheckNamedFramebufferStatus", [&] {
            status = glCheckNamedFramebufferStatus(handle_, GL_FRAMEBUFFER);
        }); !checked) return checked;
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
    return detail::checked_gl_call("glBindFramebuffer", [&] {
        glBindFramebuffer(GL_FRAMEBUFFER, handle_);
    });
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
    if (gfx::is_integer_format(color_attachment_formats_.at(attachment))) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Framebuffer::clear_color(float) requires normalized or floating color storage",
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
    return read_typed_color_pixels<Rgba8Pixel>(
        handle_,
        state_, color_attachments_, color_attachment_samples_, color_attachment_formats_,
        attachment,
        ImageFormat::rgba8,
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
    // Read directly into the returned byte allocation. Do not construct a
    // temporary array of pixel structs and copy the entire image afterwards.
    return read_typed_color_pixels<std::byte, 4>(
        handle_, state_, color_attachments_, color_attachment_samples_, color_attachment_formats_,
        attachment, ImageFormat::rgba8, x, y, width, height,
        GL_RGBA, GL_UNSIGNED_BYTE, "Framebuffer::read_rgba8");
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
    color_attachment_extents_.clear();
    depth_attachment_format_.reset();
    depth_extent_ = {};
    depth_image_handle_ = 0;
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
    color_attachment_extents_.clear();
    depth_attachment_format_.reset();
    depth_extent_ = {};
    depth_image_handle_ = 0;
    state_.reset();
}

} // namespace vng::opengl
