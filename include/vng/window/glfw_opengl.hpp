#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>

#include <vng/opengl/context.hpp>
#include <vng/opengl/context_access.hpp>
#include <vng/window/glfw.hpp>
#include <vng/window/presentation.hpp>

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
    [[nodiscard]] bool key_down(vng::window::Key key) const noexcept;
    [[nodiscard]] input::Frame take_input();
    [[nodiscard]] std::string clipboard_text() const;
    void set_clipboard_text(std::string_view);
    void request_close() noexcept;
    void cancel_close() noexcept;
    [[nodiscard]] bool visible() const noexcept;
    void show() noexcept;
    void hide() noexcept;
    void maximize() noexcept;
    void restore() noexcept;
    [[nodiscard]] bool maximized() const noexcept;
    [[nodiscard]] bool fullscreen() const noexcept;
    [[nodiscard]] std::expected<void, vng::window::Diagnostic> set_fullscreen(bool enabled);
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
    // Requires this window's context current on its owning thread. No hidden
    // context switches. Success stores the request, not measured driver state.
    [[nodiscard]] std::expected<void, vng::window::Diagnostic> set_vsync(vng::window::VSync mode);
    [[nodiscard]] vng::window::VSync vsync() const noexcept { return vsync_; }
    void release_current() noexcept;

private:
    friend std::expected<Window, vng::window::Diagnostic> create_window(
        const vng::window::WindowDesc&,
        const vng::opengl::ContextDesc&,
        const vng::window::PresentationDesc&);

    Window(
        vng::window::GlfwWindow window,
        std::shared_ptr<vng::opengl::ContextLifetime> context_lifetime,
        vng::render::ColorEncoding required_default_framebuffer_encoding,
        vng::window::VSync vsync) noexcept;
    void release_context_noexcept() noexcept;

    vng::window::GlfwWindow window_;
    std::shared_ptr<vng::opengl::ContextLifetime> context_lifetime_;
    vng::render::ColorEncoding required_default_framebuffer_encoding_{
        vng::render::ColorEncoding::srgb};
    vng::window::VSync vsync_{vng::window::VSync::on};
};

[[nodiscard]] std::expected<Window, vng::window::Diagnostic> create_window(
    const vng::window::WindowDesc& window = {},
    const vng::opengl::ContextDesc& context = {},
    const vng::window::PresentationDesc& presentation = {});

static_assert(vng::window::Window<Window>);
static_assert(vng::window::Presentable<Window>);

} // namespace vng::glfw_opengl
