#include <vng/opengl/frame.hpp>
#include <vng/opengl/framebuffer.hpp>
#include <vng/opengl/image.hpp>

#include "context_state.hpp"
#include "gl_error.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace vng::opengl {

Frame::Frame(Frame&& other) noexcept
    : device_(std::move(other.device_)),
      extent_(std::exchange(other.extent_, {})),
      color_encoding_(std::exchange(
          other.color_encoding_,
          render::ColorEncoding::srgb)),
      generation_(std::exchange(other.generation_, 0)),
      attachment_image_handles_(std::move(other.attachment_image_handles_))
{}

Frame& Frame::operator=(Frame&& other) noexcept
{
    if (this != &other) {
        release_noexcept();
        device_ = std::move(other.device_);
        extent_ = std::exchange(other.extent_, {});
        color_encoding_ = std::exchange(
            other.color_encoding_,
            render::ColorEncoding::srgb);
        generation_ = std::exchange(other.generation_, 0);
        attachment_image_handles_ = std::move(other.attachment_image_handles_);
    }
    return *this;
}

Frame::~Frame()
{
    release_noexcept();
}

bool Frame::active() const noexcept
{
    return device_.state_
        && device_.state_->access.valid()
        && device_.state_->frame_is_active(generation_);
}

std::expected<void, Diagnostic> Frame::end()
{
    if (generation_ == 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "OpenGL frame is already ended or empty",
        });
    }
    if (auto current = device_.require_current("OpenGL Frame::end"); !current) {
        return current;
    }
    if (!device_.state_ || !device_.state_->end_frame(generation_)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "OpenGL frame is stale and no longer owns the active scope",
        });
    }
    generation_ = 0;
    return {};
}

std::expected<Frame, Diagnostic> Frame::acquire(
    const Device& device,
    Extent2D extent,
    render::ColorEncoding color_encoding)
{
    if (!device.state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_context_access,
            .message = "begin_frame received an empty OpenGL device",
        });
    }
    const auto generation = device.state_->try_begin_frame();
    if (generation == 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "begin_frame rejected an overlapping active frame on this context",
        });
    }
    return Frame{
        device,
        extent,
        color_encoding,
        generation,
    };
}

void Frame::release_noexcept() noexcept
{
    if (generation_ != 0 && device_.state_) {
        (void)device_.state_->end_frame(generation_);
    }
    generation_ = 0;
}

Commands Frame::commands() noexcept
{
    return Commands{
        device_,
        generation_,
        extent_,
        color_encoding_,
    };
}

bool Frame::belongs_to(const Device& device) const noexcept
{
    return device_.state_ && device.state_
        && device_.state_.get() == device.state_.get();
}

bool Frame::uses_image(const Image2D& image) const noexcept
{
    return active() && image.belongs_to(device_)
        && std::ranges::find(attachment_image_handles_, image.native_handle())
            != attachment_image_handles_.end();
}

std::expected<Frame, Diagnostic> begin_backend_frame(
    Device& device,
    render::DefaultTarget,
    const render::FrameDesc& description)
{
    if (description.extent.empty()
        || description.extent.width
            > static_cast<u32>(std::numeric_limits<std::int32_t>::max())
        || description.extent.height
            > static_cast<u32>(std::numeric_limits<std::int32_t>::max())) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "begin_frame requires a non-empty, representable target extent",
        });
    }
    if (description.clear_color) {
        for (const auto component : *description.clear_color) {
            if (!std::isfinite(component)) {
                return std::unexpected(Diagnostic{
                    .code = ErrorCode::invalid_argument,
                    .message = "begin_frame requires finite clear-color components",
                });
            }
        }
    }
    if (description.clear_depth
        && (!std::isfinite(*description.clear_depth)
            || *description.clear_depth < 0.0F
            || *description.clear_depth > 1.0F)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "begin_frame requires clear depth in [0, 1]",
        });
    }

    bool framebuffer_srgb = false;
    switch (description.color_encoding) {
    case render::ColorEncoding::linear:
        break;
    case render::ColorEncoding::srgb:
        framebuffer_srgb = true;
        break;
    default:
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "begin_frame received an invalid target color encoding",
        });
    }

    if (auto current = device.require_current("begin_frame"); !current) {
        return std::unexpected(std::move(current.error()));
    }
    const auto default_framebuffer =
        device.default_framebuffer_capabilities();
    if (!default_framebuffer.supports(description.color_encoding)) {
        const auto requested = description.color_encoding
                == render::ColorEncoding::srgb
            ? "sRGB"
            : "linear";
        const auto actual = !default_framebuffer.color_encoding
            ? "unavailable"
            : *default_framebuffer.color_encoding
                    == render::ColorEncoding::srgb
                ? "sRGB"
                : "linear";
        return std::unexpected(Diagnostic{
            .code = ErrorCode::unsupported_feature,
            .message = std::string("begin_frame requested a ") + requested
                + " default target, but the physical default framebuffer is "
                + actual
                + (description.color_encoding == render::ColorEncoding::srgb
                        && !default_framebuffer.srgb_conversion_supported
                    ? " and framebuffer sRGB conversion is unavailable"
                    : ""),
        });
    }
    auto frame = Frame::acquire(
        device,
        description.extent,
        description.color_encoding);
    if (!frame) {
        return std::unexpected(std::move(frame.error()));
    }
    if (auto bound = device.bind_default_framebuffer(); !bound) {
        return std::unexpected(std::move(bound.error()));
    }
    if (auto encoding = device.set_framebuffer_srgb_enabled(
            framebuffer_srgb);
        !encoding) {
        return std::unexpected(std::move(encoding.error()));
    }
    if (auto scissor = device.set_scissor_enabled(false); !scissor) {
        return std::unexpected(std::move(scissor.error()));
    }
    if (auto viewport = device.viewport(
            0,
            0,
            static_cast<std::int32_t>(description.extent.width),
            static_cast<std::int32_t>(description.extent.height));
        !viewport) {
        return std::unexpected(std::move(viewport.error()));
    }

    if (description.clear_color && description.clear_depth) {
        if (auto clear = device.clear_default(
                *description.clear_color, *description.clear_depth);
            !clear) {
            return std::unexpected(std::move(clear.error()));
        }
    } else if (description.clear_color) {
        if (auto clear = device.clear_default_color(*description.clear_color);
            !clear) {
            return std::unexpected(std::move(clear.error()));
        }
    } else if (description.clear_depth) {
        if (auto clear = device.clear_default_depth(*description.clear_depth);
            !clear) {
            return std::unexpected(std::move(clear.error()));
        }
    }

    return std::move(*frame);
}

std::expected<Frame, Diagnostic> begin_backend_frame(
    Device& device, Framebuffer& target, const render::FrameDesc& description)
{
    return begin_backend_frame(device, std::as_const(target), description);
}

std::expected<Frame, Diagnostic> begin_backend_frame(
    Device& device, const Framebuffer& target, const render::FrameDesc& description)
{
    if (!target.belongs_to(device))
        return std::unexpected(Diagnostic{.code = ErrorCode::invalid_argument,
            .message = "begin_frame requires an offscreen target owned by this device"});
    if (description.extent.empty() || description.extent != target.extent()
        || description.extent.width > static_cast<u32>(std::numeric_limits<GLsizei>::max())
        || description.extent.height > static_cast<u32>(std::numeric_limits<GLsizei>::max()))
        return std::unexpected(Diagnostic{.code = ErrorCode::invalid_argument,
            .message = "begin_frame extent must match the offscreen attachment area"});
    const auto encoding = target.color_attachments().empty()
        ? std::optional{render::ColorEncoding::linear} : target.color_encoding();
    if (!encoding || *encoding != description.color_encoding)
        return std::unexpected(Diagnostic{.code = ErrorCode::unsupported_feature,
            .message = "begin_frame color encoding must match every physical offscreen color attachment"});
    if (description.clear_color) {
        if (target.color_attachments().empty() || target.has_integer_color())
            return std::unexpected(Diagnostic{.code = ErrorCode::invalid_argument,
                .message = "begin_frame floating color clear requires normalized or floating color attachments"});
        for (float value : *description.clear_color)
            if (!std::isfinite(value))
                return std::unexpected(Diagnostic{.code = ErrorCode::invalid_argument,
                    .message = "begin_frame requires finite clear-color components"});
    }
    if (description.clear_depth && (!target.has_depth()
            || !std::isfinite(*description.clear_depth)
            || *description.clear_depth < 0 || *description.clear_depth > 1))
        return std::unexpected(Diagnostic{.code = ErrorCode::invalid_argument,
            .message = "begin_frame depth clear requires a depth attachment and finite depth in [0, 1]"});
    if (auto current = device.require_current("begin_frame(offscreen)"); !current)
        return std::unexpected(std::move(current.error()));
    if (auto complete = target.check_complete(); !complete)
        return std::unexpected(std::move(complete.error()));
    auto frame = Frame::acquire(device, description.extent, description.color_encoding);
    if (!frame) return frame;
    frame->attachment_image_handles_ = target.attachment_image_handles();
    if (auto bound = target.bind(); !bound)
        return std::unexpected(std::move(bound.error()));
    if (auto encoded = device.set_framebuffer_srgb_enabled(
            description.color_encoding == render::ColorEncoding::srgb); !encoded)
        return std::unexpected(std::move(encoded.error()));
    if (auto scissor = device.set_scissor_enabled(false); !scissor)
        return std::unexpected(std::move(scissor.error()));
    if (auto viewport = device.viewport(0, 0,
            static_cast<i32>(description.extent.width), static_cast<i32>(description.extent.height)); !viewport)
        return std::unexpected(std::move(viewport.error()));
    if (description.clear_color) {
        for (auto attachment : target.color_attachments()) {
            if (auto mask = device.set_color_write_mask(attachment, {true, true, true, true}); !mask)
                return std::unexpected(std::move(mask.error()));
            if (auto cleared = target.clear_color(attachment, *description.clear_color); !cleared)
                return std::unexpected(std::move(cleared.error()));
        }
    }
    if (description.clear_depth) {
        if (auto mask = detail::checked_gl_call("offscreen depth-clear mask", [] { glDepthMask(GL_TRUE); }); !mask)
            return std::unexpected(std::move(mask.error()));
        if (auto cleared = target.clear_depth(*description.clear_depth); !cleared)
            return std::unexpected(std::move(cleared.error()));
    }
    return frame;
}

} // namespace vng::opengl
