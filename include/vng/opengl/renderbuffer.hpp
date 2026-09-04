#pragma once

#include <cstdint>
#include <expected>
#include <memory>

#include <vng/opengl/diagnostic.hpp>

namespace vng::opengl {

namespace detail { struct ContextState; }
class Device;
class Framebuffer;

enum class RenderbufferFormat { rgba8, depth24_stencil8 };

class Renderbuffer final {
public:
    static std::expected<Renderbuffer, Diagnostic> create(
        const Device& device,
        std::uint32_t width,
        std::uint32_t height,
        RenderbufferFormat format = RenderbufferFormat::rgba8,
        std::uint32_t samples = 0);

    Renderbuffer(Renderbuffer&& other) noexcept;
    Renderbuffer& operator=(Renderbuffer&& other) noexcept;
    Renderbuffer(const Renderbuffer&) = delete;
    Renderbuffer& operator=(const Renderbuffer&) = delete;
    ~Renderbuffer();

    [[nodiscard]] std::uint32_t native_handle() const noexcept { return handle_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::uint32_t samples() const noexcept { return samples_; }
    [[nodiscard]] RenderbufferFormat format() const noexcept { return format_; }
    [[nodiscard]] std::expected<void, Diagnostic> destroy();

private:
    Renderbuffer(
        std::shared_ptr<detail::ContextState> state,
        std::uint32_t handle,
        std::uint32_t width,
        std::uint32_t height,
        RenderbufferFormat format,
        std::uint32_t samples) noexcept;
    void release_noexcept() noexcept;

    std::shared_ptr<detail::ContextState> state_;
    std::uint32_t handle_{};
    std::uint32_t width_{};
    std::uint32_t height_{};
    std::uint32_t samples_{};
    RenderbufferFormat format_{RenderbufferFormat::rgba8};

    friend class Framebuffer;
};

} // namespace vng::opengl
