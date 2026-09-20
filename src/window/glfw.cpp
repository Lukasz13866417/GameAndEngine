#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <vng/window/glfw.hpp>
#include <vng/core/monotonic_clock.hpp>

#include "glfw_access.hpp"

#include <exception>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace vng::window {
namespace detail {
struct InputQueue { std::vector<input::Event> events; bool overflow{}; };
}
namespace {

int native_key(Key key) noexcept {
    if (key >= Key::a && key <= Key::z) return GLFW_KEY_A + static_cast<int>(key)-static_cast<int>(Key::a);
    switch (key) {
    case Key::escape: return GLFW_KEY_ESCAPE; case Key::space: return GLFW_KEY_SPACE;
    case Key::enter: return GLFW_KEY_ENTER; case Key::tab: return GLFW_KEY_TAB;
    case Key::backspace: return GLFW_KEY_BACKSPACE; case Key::del: return GLFW_KEY_DELETE;
    case Key::left: return GLFW_KEY_LEFT; case Key::right: return GLFW_KEY_RIGHT;
    case Key::up: return GLFW_KEY_UP; case Key::down: return GLFW_KEY_DOWN;
    case Key::home: return GLFW_KEY_HOME; case Key::end: return GLFW_KEY_END;
    case Key::page_up: return GLFW_KEY_PAGE_UP; case Key::page_down: return GLFW_KEY_PAGE_DOWN;
    case Key::f11: return GLFW_KEY_F11;
    case Key::one: return GLFW_KEY_1; case Key::two: return GLFW_KEY_2; case Key::three: return GLFW_KEY_3;
    case Key::four: return GLFW_KEY_4;
    case Key::five: return GLFW_KEY_5;
    default: return GLFW_KEY_UNKNOWN;
    }
}
Key logical_key(int key) noexcept {
    for (int i = 1; i < static_cast<int>(Key::count); ++i)
        if (native_key(static_cast<Key>(i)) == key) return static_cast<Key>(i);
    return Key::unknown;
}
input::Modifiers modifiers(int m) {
    return {bool(m & GLFW_MOD_SHIFT), bool(m & GLFW_MOD_CONTROL),
        bool(m & GLFW_MOD_ALT), bool(m & GLFW_MOD_SUPER)};
}
Vec2 pointer(GLFWwindow* w) {
    double x{}, y{}; glfwGetCursorPos(w, &x, &y);
    return {static_cast<f32>(x), static_cast<f32>(y)};
}
bool trace_scroll() {
    static const bool enabled = [] {
        const auto* value = std::getenv("VNG_TRACE_SCROLL");
        return value && std::string_view(value) == "1";
    }();
    return enabled;
}
template<class F> void enqueue(GLFWwindow* w, F&& make) noexcept {
    auto* q = static_cast<detail::InputQueue*>(glfwGetWindowUserPointer(w));
    if (!q) return;
    try {
        if (q->events.size() < 65536) {
            const auto received = monotonic_ns();
            auto event = make();
            // Capture at callback time, including pointer motion and scrolling
            // (GLFW supplies no modifier mask for those callbacks).
            event.modifiers.left_alt = glfwGetKey(w, GLFW_KEY_LEFT_ALT) == GLFW_PRESS;
            event.received_ns = received;
            if (event.kind == input::EventKind::scroll && trace_scroll())
                std::fprintf(stderr, "[scroll/glfw] received_ns=%llu dx=%.9g dy=%.9g at=%.3f,%.3f\n",
                    static_cast<unsigned long long>(received), static_cast<double>(event.scroll.x),
                    static_cast<double>(event.scroll.y), static_cast<double>(event.position.x),
                    static_cast<double>(event.position.y));
            q->events.push_back(std::move(event));
        }
        else q->overflow = true;
    } catch (...) { q->overflow = true; } // never unwind across a C callback
}
void install_input(GLFWwindow* w, detail::InputQueue* queue) {
    glfwSetWindowUserPointer(w, queue);
    glfwSetCursorPosCallback(w, [](GLFWwindow* window, double x, double y) {
        enqueue(window, [&] { input::Event e; e.kind = input::EventKind::pointer_move;
            e.position = {static_cast<f32>(x), static_cast<f32>(y)}; return e; });
    });
    glfwSetMouseButtonCallback(w, [](GLFWwindow* window, int button, int action, int mods) {
        enqueue(window, [&] { input::Event e; e.kind = action == GLFW_PRESS
                ? input::EventKind::pointer_down : input::EventKind::pointer_up;
            e.position = pointer(window); e.button = static_cast<u32>(button);
            e.modifiers = modifiers(mods); return e; });
    });
    glfwSetScrollCallback(w, [](GLFWwindow* window, double x, double y) {
        enqueue(window, [&] { input::Event e; e.kind = input::EventKind::scroll;
            e.position = pointer(window); e.scroll = {static_cast<f32>(x), static_cast<f32>(y)}; return e; });
    });
    glfwSetKeyCallback(w, [](GLFWwindow* window, int key, int, int action, int mods) {
        enqueue(window, [&] { input::Event e; e.kind = action == GLFW_RELEASE
                ? input::EventKind::key_up : input::EventKind::key_down;
            e.key = logical_key(key); e.repeat = action == GLFW_REPEAT; e.position = pointer(window);
            e.modifiers = modifiers(mods); return e; });
    });
    glfwSetCharCallback(w, [](GLFWwindow* window, unsigned int code) {
        enqueue(window, [&] { input::Event e; e.kind = input::EventKind::text;
            if (code <= 0x7f) e.text += static_cast<char>(code);
            else if (code <= 0x7ff) { e.text += static_cast<char>(0xc0 | code>>6); e.text += static_cast<char>(0x80 | (code&63)); }
            else if (code <= 0xffff) { e.text += static_cast<char>(0xe0 | code>>12); e.text += static_cast<char>(0x80 | ((code>>6)&63)); e.text += static_cast<char>(0x80 | (code&63)); }
            else { e.text += static_cast<char>(0xf0 | code>>18); e.text += static_cast<char>(0x80 | ((code>>12)&63)); e.text += static_cast<char>(0x80 | ((code>>6)&63)); e.text += static_cast<char>(0x80 | (code&63)); }
            return e; });
    });
    glfwSetWindowFocusCallback(w, [](GLFWwindow* window, int focused) {
        enqueue(window, [&] { input::Event e; e.kind = focused ? input::EventKind::focus_gained
            : input::EventKind::focus_lost; return e; });
    });
}

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

GlfwWindow::GlfwWindow(void* handle)
    : handle_(handle),
      owns_glfw_reference_(true),
      owner_thread_(std::this_thread::get_id()), input_(std::make_unique<detail::InputQueue>()) {
    install_input(native_window(handle), input_.get());
}

GlfwWindow::GlfwWindow(GlfwWindow&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)),
      owns_glfw_reference_(std::exchange(other.owns_glfw_reference_, false)),
      owner_thread_(other.owner_thread_), input_(std::move(other.input_)),
      windowed_bounds_(other.windowed_bounds_), windowed_maximized_(other.windowed_maximized_) {}

GlfwWindow& GlfwWindow::operator=(GlfwWindow&& other) noexcept {
    if (this != &other) {
        release_noexcept();
        handle_ = std::exchange(other.handle_, nullptr);
        owns_glfw_reference_ = std::exchange(other.owns_glfw_reference_, false);
        owner_thread_ = other.owner_thread_;
        input_ = std::move(other.input_);
        windowed_bounds_ = other.windowed_bounds_;
        windowed_maximized_ = other.windowed_maximized_;
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
    glfwWindowHint(GLFW_MAXIMIZED, description.maximized ? GLFW_TRUE : GLFW_FALSE);

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

    try {
        auto window = GlfwWindow(handle);
        // Some window managers replace the initial state while mapping the
        // window. Reinforce the requested mode after native creation.
        if (description.maximized)
            window.maximize();
        return window;
    } catch (...) {
        glfwDestroyWindow(handle);
        release_glfw();
        throw;
    }
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

bool GlfwWindow::key_down(Key key) const noexcept {
    if (handle_ == nullptr || std::this_thread::get_id() != owner_thread_) return false;
    const auto code = native_key(key);
    return code != GLFW_KEY_UNKNOWN && glfwGetKey(native_window(handle_), code) == GLFW_PRESS;
}

input::Frame GlfwWindow::take_input() {
    input::Frame result;
    if (!detail::GlfwWindowAccess::on_owner_thread(*this)) { result.focused = false; return result; }
    auto* w = native_window(handle_);
    int width{}, height{}; glfwGetWindowSize(w, &width, &height);
    result.logical_size = {static_cast<f32>(width), static_cast<f32>(height)};
    result.framebuffer = framebuffer_extent(); result.pointer = pointer(w);
    result.focused = glfwGetWindowAttrib(w, GLFW_FOCUSED) == GLFW_TRUE;
    result.overflow = std::exchange(input_->overflow, false);
    result.events.swap(input_->events);
    for (std::size_t i = 0; i < result.keys.size(); ++i) result.keys[i] = key_down(static_cast<Key>(i));
    return result;
}
std::string GlfwWindow::clipboard_text() const {
    if (!detail::GlfwWindowAccess::on_owner_thread(*this)) return {};
    const auto* text = glfwGetClipboardString(native_window(handle_));
    return text ? text : "";
}
void GlfwWindow::set_clipboard_text(std::string_view text) {
    if (!detail::GlfwWindowAccess::on_owner_thread(*this)) return;
    const std::string owned{text}; glfwSetClipboardString(native_window(handle_), owned.c_str());
}

void GlfwWindow::request_close() noexcept {
    if (handle_ != nullptr && std::this_thread::get_id() == owner_thread_) {
        glfwSetWindowShouldClose(native_window(handle_), GLFW_TRUE);
    }
}

void GlfwWindow::cancel_close() noexcept {
    if (handle_ != nullptr && std::this_thread::get_id() == owner_thread_)
        glfwSetWindowShouldClose(native_window(handle_), GLFW_FALSE);
}

bool GlfwWindow::visible() const noexcept {
    return handle_ != nullptr && std::this_thread::get_id() == owner_thread_
        && glfwGetWindowAttrib(native_window(handle_), GLFW_VISIBLE) == GLFW_TRUE;
}

void GlfwWindow::show() noexcept {
    if (handle_ != nullptr && std::this_thread::get_id() == owner_thread_) {
        const bool was_maximized = maximized();
        glfwShowWindow(native_window(handle_));
        if (was_maximized)
            maximize();
    }
}

void GlfwWindow::hide() noexcept {
    if (handle_ != nullptr && std::this_thread::get_id() == owner_thread_)
        glfwHideWindow(native_window(handle_));
}

void GlfwWindow::maximize() noexcept {
    if (detail::GlfwWindowAccess::on_owner_thread(*this) && !fullscreen())
        glfwMaximizeWindow(native_window(handle_));
}
void GlfwWindow::restore() noexcept {
    if (detail::GlfwWindowAccess::on_owner_thread(*this) && !fullscreen())
        glfwRestoreWindow(native_window(handle_));
}
bool GlfwWindow::maximized() const noexcept {
    return detail::GlfwWindowAccess::on_owner_thread(*this) &&
           glfwGetWindowAttrib(native_window(handle_), GLFW_MAXIMIZED) == GLFW_TRUE;
}
bool GlfwWindow::fullscreen() const noexcept {
    return detail::GlfwWindowAccess::on_owner_thread(*this) &&
           glfwGetWindowMonitor(native_window(handle_)) != nullptr;
}
std::expected<void, Diagnostic> GlfwWindow::set_fullscreen(bool enabled) {
    if (!handle_) return std::unexpected(Diagnostic{ErrorCode::invalid_window, "Empty window"});
    if (!detail::GlfwWindowAccess::on_owner_thread(*this))
        return std::unexpected(Diagnostic{ErrorCode::wrong_thread, "Window mode requires its owner thread"});
    if (enabled == fullscreen()) return {};
    auto* window = native_window(handle_);
    (void)glfwGetError(nullptr);
    if (enabled) {
        int count{};
        auto** monitors = glfwGetMonitors(&count);
        if (!monitors || count == 0)
            return std::unexpected(Diagnostic{ErrorCode::operation_failed, "No monitor available for fullscreen"});
        std::array<int, 4> bounds{};
        glfwGetWindowPos(window, &bounds[0], &bounds[1]);
        glfwGetWindowSize(window, &bounds[2], &bounds[3]);
        GLFWmonitor* chosen = monitors[0];
        std::int64_t best = -1;
        for (int i = 0; i < count; ++i) {
            int x{}, y{};
            glfwGetMonitorPos(monitors[i], &x, &y);
            const auto* mode = glfwGetVideoMode(monitors[i]);
            if (!mode) continue;
            const auto overlap_x = std::max<std::int64_t>(0, std::min<std::int64_t>(
                std::int64_t(bounds[0]) + bounds[2], std::int64_t(x) + mode->width) - std::max(bounds[0], x));
            const auto overlap_y = std::max<std::int64_t>(0, std::min<std::int64_t>(
                std::int64_t(bounds[1]) + bounds[3], std::int64_t(y) + mode->height) - std::max(bounds[1], y));
            if (overlap_x * overlap_y > best) { best = overlap_x * overlap_y; chosen = monitors[i]; }
        }
        const auto* mode = glfwGetVideoMode(chosen);
        if (!mode) return std::unexpected(glfw_diagnostic(ErrorCode::operation_failed, "Read monitor mode"));
        windowed_bounds_ = bounds;
        windowed_maximized_ = maximized();
        glfwSetWindowMonitor(window, chosen, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else {
        const auto& b = windowed_bounds_;
        glfwSetWindowMonitor(window, nullptr, b[0], b[1], std::max(1, b[2]), std::max(1, b[3]), GLFW_DONT_CARE);
        if (windowed_maximized_) glfwMaximizeWindow(window);
    }
    const char* description{};
    const auto error = glfwGetError(&description);
    if (error != GLFW_NO_ERROR)
        return std::unexpected(Diagnostic{ErrorCode::operation_failed,
            description ? description : "Window mode switch failed", error});
    return {};
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
        input_.reset();
    }
    if (owns_glfw_reference_) {
        release_glfw();
        owns_glfw_reference_ = false;
    }
}

} // namespace vng::window
