#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>

#include <vng/opengl/diagnostic.hpp>

namespace vng::opengl {

namespace detail { struct ContextState; }
class Device;
class Framebuffer;

// The storage format is part of an image's type-erased runtime contract.  It
// is intentionally independent from vertex formats: framebuffer images and
// vertex delivery obey different OpenGL rules.
enum class ImageFormat {
    rgba8,
    rg32ui,
    rgba32f,
    rgba32i,
    rgba32ui,
    depth32f,
};

[[nodiscard]] constexpr bool is_color_format(ImageFormat format) noexcept {
    return format == ImageFormat::rgba8
        || format == ImageFormat::rg32ui
        || format == ImageFormat::rgba32f
        || format == ImageFormat::rgba32i
        || format == ImageFormat::rgba32ui;
}

[[nodiscard]] constexpr bool is_depth_format(ImageFormat format) noexcept {
    return format == ImageFormat::depth32f;
}

// A single-level, single-sample, sampleable two-dimensional image.  It is the
// texture-backed building block for render targets; sampling policy can later
// move into a separate Sampler object without changing image ownership.
class Image2D final {
public:
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
    [[nodiscard]] bool belongs_to(const Device& device) const noexcept;

    [[nodiscard]] std::expected<void, Diagnostic> bind_to_unit(
        std::uint32_t unit) const;

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
        ImageFormat format) noexcept;

    void release_noexcept() noexcept;

    std::shared_ptr<detail::ContextState> state_;
    std::uint32_t handle_{};
    std::uint32_t width_{};
    std::uint32_t height_{};
    ImageFormat format_{ImageFormat::rgba8};

    friend class Framebuffer;
};

} // namespace vng::opengl
