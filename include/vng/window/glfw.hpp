#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <thread>

#include <vng/core/types.hpp>
#include <vng/window/window.hpp>

namespace vng::window {

namespace detail {
class GlfwWindowAccess;
}

enum class ErrorCode {
    initialization_failed,
    window_creation_failed,
    invalid_window,
    context_not_current,
    wrong_thread,
    operation_failed,
};

struct Diagnostic final {
    ErrorCode code{ErrorCode::operation_failed};
    std::string message{};
    int native_code{};
};

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
    void request_close() noexcept;
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

    explicit GlfwWindow(void* handle) noexcept;
    void release_noexcept() noexcept;

    void* handle_{};
    bool owns_glfw_reference_{};
    std::thread::id owner_thread_{};
};

static_assert(Window<GlfwWindow>);

} // namespace vng::window
