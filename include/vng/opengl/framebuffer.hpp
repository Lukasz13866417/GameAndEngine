#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include <vng/opengl/diagnostic.hpp>

namespace vng::opengl {

namespace detail { struct ContextState; }
class Device;
class Renderbuffer;
class Image2D;
enum class ImageFormat;

struct Rgba8Pixel final {
    std::uint8_t r{};
    std::uint8_t g{};
    std::uint8_t b{};
    std::uint8_t a{};

    friend constexpr bool operator==(const Rgba8Pixel&, const Rgba8Pixel&) = default;
};

struct Rg32uiPixel final {
    std::uint32_t r{};
    std::uint32_t g{};

    friend constexpr bool operator==(const Rg32uiPixel&, const Rg32uiPixel&) = default;
};

struct Rgba32fPixel final {
    float r{};
    float g{};
    float b{};
    float a{};
};

struct Rgba32iPixel final {
    std::int32_t r{};
    std::int32_t g{};
    std::int32_t b{};
    std::int32_t a{};
};

struct Rgba32uiPixel final {
    std::uint32_t r{};
    std::uint32_t g{};
    std::uint32_t b{};
    std::uint32_t a{};
};

class Framebuffer final {
public:
    static std::expected<Framebuffer, Diagnostic> create(const Device& device);

    Framebuffer(Framebuffer&& other) noexcept;
    Framebuffer& operator=(Framebuffer&& other) noexcept;
    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;
    ~Framebuffer();

    [[nodiscard]] std::uint32_t native_handle() const noexcept { return handle_; }

    [[nodiscard]] std::expected<void, Diagnostic> attach_color(
        std::uint32_t attachment,
        const Renderbuffer& renderbuffer);

    [[nodiscard]] std::expected<void, Diagnostic> attach_color(
        std::uint32_t attachment,
        const Image2D& image);

    [[nodiscard]] std::expected<void, Diagnostic> attach_depth_stencil(
        const Renderbuffer& renderbuffer);

    [[nodiscard]] std::expected<void, Diagnostic> attach_depth(
        const Image2D& image);

    [[nodiscard]] std::expected<void, Diagnostic> check_complete() const;
    [[nodiscard]] std::expected<void, Diagnostic> bind() const;

    [[nodiscard]] std::expected<void, Diagnostic> clear_color(
        std::uint32_t attachment,
        const std::array<float, 4>& color) const;

    [[nodiscard]] std::expected<void, Diagnostic> clear_color(
        std::uint32_t attachment,
        Rg32uiPixel color) const;

    [[nodiscard]] std::expected<void, Diagnostic> clear_depth(float depth) const;

    [[nodiscard]] std::expected<std::vector<std::byte>, Diagnostic> read_rgba8(
        std::uint32_t attachment,
        std::int32_t x,
        std::int32_t y,
        std::uint32_t width,
        std::uint32_t height) const;

    [[nodiscard]] std::expected<std::vector<Rgba8Pixel>, Diagnostic>
    read_rgba8_pixels(
        std::uint32_t attachment,
        std::int32_t x,
        std::int32_t y,
        std::uint32_t width,
        std::uint32_t height) const;

    [[nodiscard]] std::expected<std::vector<Rg32uiPixel>, Diagnostic> read_rg32ui(
        std::uint32_t attachment,
        std::int32_t x,
        std::int32_t y,
        std::uint32_t width,
        std::uint32_t height) const;

    [[nodiscard]] std::expected<std::vector<Rgba32fPixel>, Diagnostic> read_rgba32f(
        std::uint32_t attachment,
        std::int32_t x,
        std::int32_t y,
        std::uint32_t width,
        std::uint32_t height) const;

    [[nodiscard]] std::expected<std::vector<Rgba32iPixel>, Diagnostic> read_rgba32i(
        std::uint32_t attachment,
        std::int32_t x,
        std::int32_t y,
        std::uint32_t width,
        std::uint32_t height) const;

    [[nodiscard]] std::expected<std::vector<Rgba32uiPixel>, Diagnostic> read_rgba32ui(
        std::uint32_t attachment,
        std::int32_t x,
        std::int32_t y,
        std::uint32_t width,
        std::uint32_t height) const;

    [[nodiscard]] std::expected<std::vector<float>, Diagnostic> read_depth32f(
        std::int32_t x,
        std::int32_t y,
        std::uint32_t width,
        std::uint32_t height) const;

    [[nodiscard]] std::expected<void, Diagnostic> destroy();

private:
    enum class AttachmentObject : std::uint8_t { renderbuffer, image };
    enum class DepthAttachmentFormat : std::uint8_t {
        depth24_stencil8,
        depth32f,
    };

    Framebuffer(
        std::shared_ptr<detail::ContextState> state,
        std::uint32_t handle) noexcept;

    [[nodiscard]] std::expected<void, Diagnostic> attach_color_storage(
        std::uint32_t attachment,
        std::uint32_t storage_handle,
        std::uint32_t samples,
        ImageFormat format,
        AttachmentObject object);

    void release_noexcept() noexcept;

    std::shared_ptr<detail::ContextState> state_;
    std::uint32_t handle_{};
    std::vector<std::uint32_t> color_attachments_;
    std::unordered_map<std::uint32_t, std::uint32_t> color_attachment_samples_;
    std::unordered_map<std::uint32_t, std::uint32_t> color_attachment_handles_;
    std::unordered_map<std::uint32_t, ImageFormat> color_attachment_formats_;
    std::unordered_map<std::uint32_t, AttachmentObject> color_attachment_objects_;
    std::optional<DepthAttachmentFormat> depth_attachment_format_;
};

} // namespace vng::opengl
