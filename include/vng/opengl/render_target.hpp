#pragma once

#include <optional>

#include <vng/opengl/frame.hpp>
#include <vng/opengl/framebuffer.hpp>
#include <vng/opengl/image.hpp>
#include <vng/providers/target.hpp>

namespace vng::opengl {

// One coherent renderable resource: attachment storage and the framebuffer
// referring to it move and reload together. Its provider retains construction
// choices by value; the provider or builder that created it need not survive.
class RenderTarget final {
public:
    [[nodiscard]] static std::expected<RenderTarget, Diagnostic> create(
        const Device& device, const render::TargetDesc& description, Extent2D extent);

    RenderTarget(RenderTarget&&) noexcept = default;
    RenderTarget& operator=(RenderTarget&& other) noexcept;
    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;
    ~RenderTarget() = default;

    [[nodiscard]] const Image2D& color() const noexcept { return color_; }
    [[nodiscard]] const Image2D* depth() const noexcept { return depth_ ? &*depth_ : nullptr; }
    [[nodiscard]] const Framebuffer& framebuffer() const noexcept { return framebuffer_; }
    [[nodiscard]] Extent2D extent() const noexcept { return color_.extent(); }
    [[nodiscard]] bool belongs_to(const Device& device) const noexcept { return color_.belongs_to(device); }

    [[nodiscard]] resources::Result<void> resize(const Device& device, Extent2D extent);
    [[nodiscard]] resources::Result<void> reload(const Device& device);

private:
    RenderTarget(providers::RenderTargetProvider provider, Image2D color,
        std::optional<Image2D> depth, Framebuffer framebuffer) noexcept
        : provider_(std::move(provider)), color_(std::move(color)),
          depth_(std::move(depth)), framebuffer_(std::move(framebuffer)) {}

    providers::RenderTargetProvider provider_;
    Image2D color_;
    std::optional<Image2D> depth_;
    // Destroy the framebuffer before releasing the storage it refers to.
    Framebuffer framebuffer_;
};

[[nodiscard]] std::expected<RenderTarget, Diagnostic> make_backend_target(
    const Device& device, const render::TargetDesc& description, Extent2D extent);

[[nodiscard]] std::expected<Frame, Diagnostic> begin_backend_frame(
    Device& device, const RenderTarget& target, const render::FrameDesc& description);

} // namespace vng::opengl
