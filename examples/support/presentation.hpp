#pragma once

#include <utility>
#include <filesystem>
#include <span>

#include <vng/opengl/render_target.hpp>
#include <vng/resources/diagnostic.hpp>

namespace example {

// Hardware sRGB encoding even when the window only has a linear framebuffer.
// Render the final linear color here with ColorEncoding::srgb, then copy the
// already-encoded bytes to the window without a second color conversion.
class DisplaySurface final {
public:
    [[nodiscard]] static vng::resources::Result<DisplaySurface> create(
        vng::opengl::Device& device, vng::Extent2D extent, bool depth = false);
    [[nodiscard]] vng::resources::Result<void> resize(
        vng::opengl::Device& device, vng::Extent2D extent);
    [[nodiscard]] const vng::opengl::RenderTarget& target() const noexcept { return target_; }

    // Call after ending the frame and before present(). Resize from the window
    // extent first: Device cannot query window-system drawable dimensions.
    [[nodiscard]] vng::resources::Result<void> copy_to_window(
        const vng::opengl::Device& device) const;

private:
    explicit DisplaySurface(vng::opengl::RenderTarget target) : target_(std::move(target)) {}
    vng::opengl::RenderTarget target_;
};

// Capture the default back buffer after composition, before present(). This
// includes every composed layer and effect, unlike per-draw shader evidence.
[[nodiscard]] vng::resources::Result<vng::gfx::ImageData> capture_screenshot(
    const vng::opengl::Device& device, vng::Extent2D extent);

// Existing files are never overwritten.
[[nodiscard]] vng::resources::Result<void> save_screenshot(
    const vng::opengl::Device& device, vng::Extent2D extent,
    const std::filesystem::path& destination);

// Write top-left-origin, tightly packed RGBA8 pixels with exclusive creation.
// Shared by screenshots and diagnostic visualizations; no hidden GL readback.
[[nodiscard]] vng::resources::Result<void> write_rgba8_png(
    const std::filesystem::path& destination, vng::Extent2D extent,
    std::span<const vng::u8> pixels);

} // namespace example
