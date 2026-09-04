#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>

#include <vng/opengl/context.hpp>
#include <vng/opengl/context_access.hpp>
#include <vng/window/glfw.hpp>

namespace vng::glfw_opengl {

// The statically selected GLFW + OpenGL integration. Window-system operations
// remain available directly on this object, while context and presentation
// operations live here rather than on window::GlfwWindow.
class Window final {
public:
    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    ~Window();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool should_close() const noexcept;
    void request_close() noexcept;
    [[nodiscard]] std::array<std::int32_t, 2> framebuffer_size() const noexcept;
    [[nodiscard]] Extent2D framebuffer_extent() const noexcept;
    void set_title(std::string_view title);
    void poll_events() const noexcept;
    void wait_events() const noexcept;

    [[nodiscard]] std::expected<vng::opengl::CurrentContextAccess,
                                vng::window::Diagnostic>
    make_current();

    [[nodiscard]] std::expected<vng::opengl::CurrentContextAccess,
                                vng::window::Diagnostic>
    current_context_access() const;

    // Presentation is intentionally named as an operation rather than as a
    // native buffer swap. A future Frame object can own/forward this boundary
    // without changing the platform window abstraction.
    [[nodiscard]] std::expected<void, vng::window::Diagnostic> present();
    void release_current() noexcept;

private:
    friend std::expected<Window, vng::window::Diagnostic> create_window(
        const vng::window::WindowDesc&,
        const vng::opengl::ContextDesc&);

    Window(
        vng::window::GlfwWindow window,
        std::shared_ptr<vng::opengl::ContextLifetime> context_lifetime,
        vng::render::ColorEncoding required_default_framebuffer_encoding,
        std::int32_t swap_interval) noexcept;
    void release_context_noexcept() noexcept;

    vng::window::GlfwWindow window_;
    std::shared_ptr<vng::opengl::ContextLifetime> context_lifetime_;
    vng::render::ColorEncoding required_default_framebuffer_encoding_{
        vng::render::ColorEncoding::srgb};
    std::int32_t swap_interval_{1};
};

[[nodiscard]] std::expected<Window, vng::window::Diagnostic> create_window(
    const vng::window::WindowDesc& window = {},
    const vng::opengl::ContextDesc& context = {});

static_assert(vng::window::Window<Window>);

} // namespace vng::glfw_opengl
