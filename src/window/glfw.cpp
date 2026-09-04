#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <vng/window/glfw.hpp>

#include "glfw_access.hpp"

#include <exception>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace vng::window {
namespace {

std::mutex glfw_lifetime_mutex;
std::size_t glfw_reference_count = 0;
std::thread::id glfw_event_thread;

[[nodiscard]] Diagnostic glfw_diagnostic(ErrorCode code, std::string prefix) {
    const char* description = nullptr;
    const int native_code = glfwGetError(&description);
    if (description != nullptr && *description != '\0') {
        prefix += ": ";
        prefix += description;
    }
    return Diagnostic{
        .code = code,
        .message = std::move(prefix),
        .native_code = native_code,
    };
}

[[nodiscard]] std::expected<void, Diagnostic> acquire_glfw() {
    std::lock_guard lock(glfw_lifetime_mutex);
    const auto caller = std::this_thread::get_id();
    if (glfw_reference_count != 0 && caller != glfw_event_thread) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::wrong_thread,
            .message = "GLFW window creation must remain on the thread that initialized GLFW and owns its event queue",
        });
    }
    if (glfw_reference_count == 0 && glfwInit() != GLFW_TRUE) {
        return std::unexpected(glfw_diagnostic(
            ErrorCode::initialization_failed,
            "GLFW initialization failed"));
    }
    if (glfw_reference_count == 0) {
        glfw_event_thread = caller;
    }
    ++glfw_reference_count;
    return {};
}

void release_glfw() noexcept {
    std::lock_guard lock(glfw_lifetime_mutex);
    if (glfw_reference_count == 0
        || std::this_thread::get_id() != glfw_event_thread) {
        return;
    }
    --glfw_reference_count;
    if (glfw_reference_count == 0) {
        glfwTerminate();
        glfw_event_thread = {};
    }
}

[[nodiscard]] bool may_process_glfw_events() noexcept {
    std::lock_guard lock(glfw_lifetime_mutex);
    return glfw_reference_count != 0
        && std::this_thread::get_id() == glfw_event_thread;
}

[[nodiscard]] GLFWwindow* native_window(void* handle) noexcept {
    return static_cast<GLFWwindow*>(handle);
}

void configure_no_client_api(const void*) noexcept {
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
}

} // namespace

GlfwWindow::GlfwWindow(void* handle) noexcept
    : handle_(handle),
      owns_glfw_reference_(true),
      owner_thread_(std::this_thread::get_id()) {}

GlfwWindow::GlfwWindow(GlfwWindow&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)),
      owns_glfw_reference_(std::exchange(other.owns_glfw_reference_, false)),
      owner_thread_(other.owner_thread_) {}

GlfwWindow& GlfwWindow::operator=(GlfwWindow&& other) noexcept {
    if (this != &other) {
        release_noexcept();
        handle_ = std::exchange(other.handle_, nullptr);
        owns_glfw_reference_ = std::exchange(other.owns_glfw_reference_, false);
        owner_thread_ = other.owner_thread_;
    }
    return *this;
}

GlfwWindow::~GlfwWindow() {
    release_noexcept();
}

std::expected<GlfwWindow, Diagnostic> GlfwWindow::create(
    const WindowDesc& description) {
    return detail::GlfwWindowAccess::create(
        description,
        configure_no_client_api);
}

std::expected<GlfwWindow, Diagnostic> detail::GlfwWindowAccess::create(
    const WindowDesc& description,
    GlfwHintConfigurer configure_client_api,
    const void* user_data) {
    if (description.width == 0 || description.height == 0
        || description.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
        || description.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::window_creation_failed,
            .message = "GLFW window dimensions must be positive int-sized values",
        });
    }
    if (configure_client_api == nullptr) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::window_creation_failed,
            .message = "GLFW client-API configuration is missing",
        });
    }
    if (auto acquired = acquire_glfw(); !acquired) {
        return std::unexpected(std::move(acquired.error()));
    }

    glfwDefaultWindowHints();
    configure_client_api(user_data);
    glfwWindowHint(GLFW_VISIBLE, description.visible ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, description.resizable ? GLFW_TRUE : GLFW_FALSE);

    auto* handle = glfwCreateWindow(
        static_cast<int>(description.width),
        static_cast<int>(description.height),
        description.title.c_str(),
        nullptr,
        nullptr);
    if (handle == nullptr) {
        auto diagnostic = glfw_diagnostic(
            ErrorCode::window_creation_failed,
            "Failed to create a GLFW window");
        release_glfw();
        return std::unexpected(std::move(diagnostic));
    }

    return GlfwWindow(handle);
}

void* detail::GlfwWindowAccess::native_handle(const GlfwWindow& window) noexcept {
    return window.handle_;
}

bool detail::GlfwWindowAccess::on_owner_thread(
    const GlfwWindow& window) noexcept {
    return window.handle_ != nullptr
        && std::this_thread::get_id() == window.owner_thread_;
}

bool GlfwWindow::valid() const noexcept {
    return handle_ != nullptr;
}

bool GlfwWindow::should_close() const noexcept {
    return handle_ == nullptr
        || std::this_thread::get_id() != owner_thread_
        || glfwWindowShouldClose(native_window(handle_)) == GLFW_TRUE;
}

void GlfwWindow::request_close() noexcept {
    if (handle_ != nullptr && std::this_thread::get_id() == owner_thread_) {
        glfwSetWindowShouldClose(native_window(handle_), GLFW_TRUE);
    }
}

std::array<std::int32_t, 2> GlfwWindow::framebuffer_size() const noexcept {
    if (handle_ == nullptr || std::this_thread::get_id() != owner_thread_) {
        return {0, 0};
    }
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(native_window(handle_), &width, &height);
    return {width, height};
}

Extent2D GlfwWindow::framebuffer_extent() const noexcept {
    const auto size = framebuffer_size();
    return {
        size[0] > 0 ? static_cast<u32>(size[0]) : 0U,
        size[1] > 0 ? static_cast<u32>(size[1]) : 0U,
    };
}

void GlfwWindow::set_title(std::string_view title) {
    if (handle_ != nullptr && std::this_thread::get_id() == owner_thread_) {
        const std::string null_terminated(title);
        glfwSetWindowTitle(native_window(handle_), null_terminated.c_str());
    }
}

void GlfwWindow::poll_events() const noexcept {
    // GLFW has one process-wide queue. Requiring a valid receiver prevents a
    // moved-from window from accidentally behaving like a live one.
    if (handle_ != nullptr && may_process_glfw_events()) {
        glfwPollEvents();
    }
}

void GlfwWindow::wait_events() const noexcept {
    if (handle_ != nullptr && may_process_glfw_events()) {
        glfwWaitEvents();
    }
}

void GlfwWindow::release_noexcept() noexcept {
    if (handle_ != nullptr) {
        if (std::this_thread::get_id() != owner_thread_) {
            // Destruction cannot report an error and GLFW forbids destroying a
            // window from another thread. Terminating makes this lifetime bug
            // deterministic instead of silently leaking the native window.
            std::terminate();
        }
        glfwDestroyWindow(native_window(handle_));
        handle_ = nullptr;
    }
    if (owns_glfw_reference_) {
        release_glfw();
        owns_glfw_reference_ = false;
    }
}

} // namespace vng::window
