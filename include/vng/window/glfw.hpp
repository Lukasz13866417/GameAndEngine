#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <thread>
#include <memory>

#include <vng/core/types.hpp>
#include <vng/window/window.hpp>

namespace vng::window {

namespace detail {
class GlfwWindowAccess;
struct InputQueue;
}

class GlfwWindow final {
public:
    static std::expected<GlfwWindow, Diagnostic> create(
        const WindowDesc& description = {});

    GlfwWindow(GlfwWindow&& other) noexcept;
    GlfwWindow& operator=(GlfwWindow&& other) noexcept;
    GlfwWindow(const GlfwWindow&) = delete;
    GlfwWindow& operator=(const GlfwWindow&) = delete;
    ~GlfwWindow();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool should_close() const noexcept;
    // Snapshot from the most recent event pump. Empty windows, unknown keys,
    // and calls from outside the owning event thread return false.
    [[nodiscard]] bool key_down(Key key) const noexcept;
    // Drain this window's ordered events. poll_events() itself never drains.
    [[nodiscard]] input::Frame take_input();
    [[nodiscard]] std::string clipboard_text() const;
    void set_clipboard_text(std::string_view);
    void request_close() noexcept;
    // Visibility and close requests are independent of window/context
    // lifetime. An editor can hide a Play window and later show it again.
    void cancel_close() noexcept;
    [[nodiscard]] bool visible() const noexcept;
    void show() noexcept;
    void hide() noexcept;
    void maximize() noexcept;
    void restore() noexcept;
    [[nodiscard]] bool maximized() const noexcept;
    [[nodiscard]] bool fullscreen() const noexcept;
    // Switches the existing window; its graphics context/resources survive.
    // Leaving fullscreen restores the preceding windowed/maximized mode.
    [[nodiscard]] std::expected<void, Diagnostic> set_fullscreen(bool enabled);
    [[nodiscard]] std::array<std::int32_t, 2> framebuffer_size() const noexcept;
    [[nodiscard]] Extent2D framebuffer_extent() const noexcept;
    void set_title(std::string_view title);

    // GLFW owns one process-wide event queue, but exposing the pump through
    // the window keeps application code independent of that backend detail.
    // Calling either function services events for every live GLFW window.
    // A moved-from/empty object never services the queue. The first live
    // window establishes the event thread; subsequent windows must be created
    // and events processed on that same thread.
    void poll_events() const noexcept;
    void wait_events() const noexcept;

private:
    friend class detail::GlfwWindowAccess;

    explicit GlfwWindow(void* handle);
    void release_noexcept() noexcept;

    void* handle_{};
    bool owns_glfw_reference_{};
    std::thread::id owner_thread_{};
    std::unique_ptr<detail::InputQueue> input_;
    std::array<int, 4> windowed_bounds_{};
    bool windowed_maximized_{};
};

static_assert(Window<GlfwWindow>);

} // namespace vng::window
