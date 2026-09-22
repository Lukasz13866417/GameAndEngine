#include <vng/opengl/render_target.hpp>

namespace vng::opengl {

std::expected<RenderTarget, Diagnostic> RenderTarget::create(
    const Device& device, const render::TargetDesc& description, Extent2D extent)
{
    if (auto allowed = device.require_resource_update("RenderTarget::create"); !allowed)
        return std::unexpected(std::move(allowed.error()));
    if (!gfx::is_color_format(description.color))
        return std::unexpected(Diagnostic{.code = ErrorCode::invalid_argument,
            .message = "RenderTarget requires color image storage"});
    auto color = Image2D::create(device, gfx::ImageDesc{
        .extent = extent, .format = description.color,
    });
    if (!color) return std::unexpected(std::move(color.error()));
    if (!gfx::is_integer_format(description.color)) {
        if (auto filtered = color->use_linear_filtering(); !filtered)
            return std::unexpected(std::move(filtered.error()));
    }
    std::optional<Image2D> depth;
    if (description.depth) {
        auto image = Image2D::create(device, gfx::ImageDesc{
            .extent = extent, .format = ImageFormat::depth32f,
        });
        if (!image) return std::unexpected(std::move(image.error()));
        depth.emplace(std::move(*image));
    }
    auto framebuffer = Framebuffer::create(device);
    if (!framebuffer) return std::unexpected(std::move(framebuffer.error()));
    if (auto attached = framebuffer->attach_color(0, *color); !attached)
        return std::unexpected(std::move(attached.error()));
    if (depth) {
        if (auto attached = framebuffer->attach_depth(*depth); !attached)
            return std::unexpected(std::move(attached.error()));
    }
    if (auto complete = framebuffer->check_complete(); !complete)
        return std::unexpected(std::move(complete.error()));
    return RenderTarget{description, std::move(*color),
        std::move(depth), std::move(*framebuffer)};
}

RenderTarget& RenderTarget::operator=(RenderTarget&& other) noexcept
{
    if (this != &other) {
        // Release the old attachment references before releasing old images.
        framebuffer_ = std::move(other.framebuffer_);
        color_ = std::move(other.color_);
        depth_ = std::move(other.depth_);
        description_ = other.description_;
    }
    return *this;
}

resources::Result<void> RenderTarget::resize(const Device& device, Extent2D extent)
{
    if (!belongs_to(device))
        return std::unexpected(resources::Diagnostic{
            .code = resources::ErrorCode::invalid_argument,
            .message = "RenderTarget::resize requires the target's creating device",
        });
    if (auto allowed = device.require_resource_update("RenderTarget::resize"); !allowed)
        return std::unexpected(resources::to_diagnostic(std::move(allowed.error())));
    if (extent == this->extent()) return {};
    auto candidate = create(device, description_, extent);
    if (!candidate) return std::unexpected(resources::to_diagnostic(std::move(candidate.error())));
    *this = std::move(*candidate);
    return {};
}

resources::Result<void> RenderTarget::reload(const Device& device)
{
    if (!belongs_to(device))
        return std::unexpected(resources::Diagnostic{
            .code = resources::ErrorCode::invalid_argument,
            .message = "RenderTarget::reload requires the target's creating device",
        });
    if (auto allowed = device.require_resource_update("RenderTarget::reload"); !allowed)
        return std::unexpected(resources::to_diagnostic(std::move(allowed.error())));
    auto candidate = create(device, description_, extent());
    if (!candidate) return std::unexpected(resources::to_diagnostic(std::move(candidate.error())));
    *this = std::move(*candidate);
    return {};
}

std::expected<RenderTarget, Diagnostic> make_backend_target(
    const Device& device, const render::TargetDesc& description, Extent2D extent)
{
    return RenderTarget::create(device, description, extent);
}

std::expected<Frame, Diagnostic> begin_backend_frame(
    Device& device, const RenderTarget& target, const render::FrameDesc& description)
{
    return begin_backend_frame(device, target.framebuffer(), description);
}

} // namespace vng::opengl
