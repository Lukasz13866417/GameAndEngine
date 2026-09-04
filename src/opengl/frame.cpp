#include <vng/opengl/frame.hpp>

#include "context_state.hpp"

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
      generation_(std::exchange(other.generation_, 0))
{
    (void)renew_command_stream();
}

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
        (void)renew_command_stream();
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

u64 Frame::renew_command_stream() noexcept
{
    if (!device_.state_) {
        return 0;
    }
    return device_.state_->renew_command_stream(generation_);
}

Commands Frame::commands() noexcept
{
    const auto epoch = renew_command_stream();
    return Commands{
        device_,
        generation_,
        epoch,
        extent_,
        color_encoding_,
    };
}

bool Frame::belongs_to(const Device& device) const noexcept
{
    return device_.state_ && device.state_
        && device_.state_.get() == device.state_.get();
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

} // namespace vng::opengl
