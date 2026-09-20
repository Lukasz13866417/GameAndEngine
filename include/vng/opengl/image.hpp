#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

#include <vng/core/types.hpp>
#include <vng/gfx/image.hpp>
#include <vng/opengl/diagnostic.hpp>

namespace vng::opengl {

namespace detail { struct ContextState; }
class Device;
class Framebuffer;

// The storage format is part of an image's type-erased runtime contract.  It
// is intentionally independent from vertex formats: framebuffer images and
// vertex delivery obey different OpenGL rules.
using gfx::ImageFormat;
using gfx::is_color_format;
using gfx::is_depth_format;

// A single-sample, sampleable two-dimensional image. It is the
// texture-backed building block for render targets; sampling policy can later
// move into a separate Sampler object without changing image ownership.
class Image2D final {
public:
    static std::expected<Image2D, Diagnostic> create(
        const Device& device, const gfx::ImageDesc& description);
    static std::expected<Image2D, Diagnostic> create(
        const Device& device,
        std::uint32_t width,
        std::uint32_t height,
        ImageFormat format);

    Image2D(Image2D&& other) noexcept;
    Image2D& operator=(Image2D&& other) noexcept;
    Image2D(const Image2D&) = delete;
    Image2D& operator=(const Image2D&) = delete;
    ~Image2D();

    [[nodiscard]] std::uint32_t native_handle() const noexcept { return handle_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] ImageFormat format() const noexcept { return format_; }
    [[nodiscard]] Extent2D extent() const noexcept { return {width_, height_}; }
    [[nodiscard]] u32 mip_levels() const noexcept { return mip_levels_; }
    [[nodiscard]] bool belongs_to(const Device& device) const noexcept;

    [[nodiscard]] std::expected<void, Diagnostic> bind_to_unit(
        std::uint32_t unit) const;

    // Tightly packed single-channel coverage upload; preserves unpack state
    // (including PBO bindings) so callers never configure pixel-store rules.
    [[nodiscard]] std::expected<void, Diagnostic> write_r8(
        u32 x, u32 y, u32 width, u32 height,
        std::span<const std::byte> pixels);
    [[nodiscard]] std::expected<void, Diagnostic> clear_r8(u8 value = 0) const;
    [[nodiscard]] std::expected<void, Diagnostic> use_linear_filtering() const;
    [[nodiscard]] std::expected<void, Diagnostic> set_sampler(
        const gfx::SamplerDesc& description) const;
    [[nodiscard]] std::expected<void, Diagnostic> generate_mipmaps() const;
    [[nodiscard]] std::expected<void, Diagnostic> write_rgba8(
        u32 x, u32 y, u32 width, u32 height, std::span<const std::byte> pixels);
    [[nodiscard]] std::expected<void, Diagnostic> write_rgba32f(
        u32 x, u32 y, u32 width, u32 height, std::span<const float> components);

    // Storage clears do not depend on framebuffer bindings, scissor state, or
    // color/depth write masks. They are useful for deterministic tool targets
    // such as analysis captures.
    [[nodiscard]] std::expected<void, Diagnostic> clear_rgba8(
        const std::array<float, 4>& color) const;
    [[nodiscard]] std::expected<void, Diagnostic> clear_rg32ui(
        const std::array<std::uint32_t, 2>& value) const;
    [[nodiscard]] std::expected<void, Diagnostic> clear_rgba32f(
        const std::array<float, 4>& value) const;
    [[nodiscard]] std::expected<void, Diagnostic> clear_rgba32i(
        const std::array<std::int32_t, 4>& value) const;
    [[nodiscard]] std::expected<void, Diagnostic> clear_rgba32ui(
        const std::array<std::uint32_t, 4>& value) const;
    [[nodiscard]] std::expected<void, Diagnostic> clear_depth32f(float depth) const;

    [[nodiscard]] std::expected<void, Diagnostic> destroy();

private:
    Image2D(
        std::shared_ptr<detail::ContextState> state,
        std::uint32_t handle,
        std::uint32_t width,
        std::uint32_t height,
        ImageFormat format, u32 mip_levels) noexcept;

    void release_noexcept() noexcept;

    std::shared_ptr<detail::ContextState> state_;
    std::uint32_t handle_{};
    std::uint32_t width_{};
    std::uint32_t height_{};
    ImageFormat format_{ImageFormat::rgba8};
    u32 mip_levels_{};

    friend class Framebuffer;
};

[[nodiscard]] std::expected<Image2D, Diagnostic> make_backend_image(
    const Device& device, const gfx::ImageDesc& description);
[[nodiscard]] std::expected<Image2D, Diagnostic> upload_backend_image(
    const Device& device, const gfx::ImageData& data,
    const gfx::ImageUploadOptions& options = {});

} // namespace vng::opengl
