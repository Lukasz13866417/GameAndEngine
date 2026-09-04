#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <vng/window/glfw_opengl.hpp>

#include "../window/glfw_access.hpp"

#include <exception>
#include <limits>
#include <string>
#include <thread>
#include <utility>

namespace vng::glfw_opengl {
namespace {

[[nodiscard]] GLFWwindow* native_window(
    const vng::window::GlfwWindow& window) noexcept {
    return static_cast<GLFWwindow*>(
        vng::window::detail::GlfwWindowAccess::native_handle(window));
}

[[nodiscard]] vng::opengl::ProcedureAddress resolve_procedure(
    const char* name) {
    return reinterpret_cast<vng::opengl::ProcedureAddress>(
        glfwGetProcAddress(name));
}

[[nodiscard]] bool is_context_current(const void* identity) noexcept {
    return static_cast<const void*>(glfwGetCurrentContext()) == identity;
}

void configure_opengl(const void* user_data) noexcept {
    const auto& context = *static_cast<const vng::opengl::ContextDesc*>(user_data);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
    glfwWindowHint(
        GLFW_CONTEXT_VERSION_MAJOR,
        static_cast<int>(context.version.major));
    glfwWindowHint(
        GLFW_CONTEXT_VERSION_MINOR,
        static_cast<int>(context.version.minor));
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(
        GLFW_OPENGL_FORWARD_COMPAT,
        context.forward_compatible ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(
        GLFW_OPENGL_DEBUG_CONTEXT,
        context.debug ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_SAMPLES, static_cast<int>(context.samples));
    glfwWindowHint(
        GLFW_SRGB_CAPABLE,
        context.default_framebuffer_encoding
                == vng::render::ColorEncoding::srgb
            ? GLFW_TRUE
            : GLFW_FALSE);
}

[[nodiscard]] vng::window::Diagnostic diagnostic(
    vng::window::ErrorCode code,
    std::string message) {
    const char* native_message = nullptr;
    const int native_code = glfwGetError(&native_message);
    if (native_message != nullptr && *native_message != '\0') {
        message += ": ";
        message += native_message;
    }
    return {
        .code = code,
        .message = std::move(message),
        .native_code = native_code,
    };
}

} // namespace

Window::Window(
    vng::window::GlfwWindow window,
    std::shared_ptr<vng::opengl::ContextLifetime> context_lifetime,
    vng::render::ColorEncoding required_default_framebuffer_encoding,
    std::int32_t swap_interval) noexcept
    : window_(std::move(window)),
      context_lifetime_(std::move(context_lifetime)),
      required_default_framebuffer_encoding_(
          required_default_framebuffer_encoding),
      swap_interval_(swap_interval) {}

Window::Window(Window&& other) noexcept
    : window_(std::move(other.window_)),
      context_lifetime_(std::move(other.context_lifetime_)),
      required_default_framebuffer_encoding_(
          other.required_default_framebuffer_encoding_),
      swap_interval_(other.swap_interval_) {}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        release_context_noexcept();
        window_ = std::move(other.window_);
        context_lifetime_ = std::move(other.context_lifetime_);
        required_default_framebuffer_encoding_ =
            other.required_default_framebuffer_encoding_;
        swap_interval_ = other.swap_interval_;
    }
    return *this;
}

Window::~Window() {
    release_context_noexcept();
}

bool Window::valid() const noexcept {
    return window_.valid() && context_lifetime_ && context_lifetime_->alive();
}

bool Window::should_close() const noexcept {
    return window_.should_close();
}

void Window::request_close() noexcept {
    window_.request_close();
}

std::array<std::int32_t, 2> Window::framebuffer_size() const noexcept {
    return window_.framebuffer_size();
}

Extent2D Window::framebuffer_extent() const noexcept {
    return window_.framebuffer_extent();
}

void Window::set_title(std::string_view title) {
    window_.set_title(title);
}

void Window::poll_events() const noexcept {
    window_.poll_events();
}

void Window::wait_events() const noexcept {
    window_.wait_events();
}

std::expected<vng::opengl::CurrentContextAccess, vng::window::Diagnostic>
Window::make_current() {
    if (!valid()) {
        return std::unexpected(vng::window::Diagnostic{
            .code = vng::window::ErrorCode::invalid_window,
            .message = "glfw_opengl::Window::make_current called on an empty window",
        });
    }
    if (!vng::window::detail::GlfwWindowAccess::on_owner_thread(window_)) {
        return std::unexpected(vng::window::Diagnostic{
            .code = vng::window::ErrorCode::wrong_thread,
            .message = "glfw_opengl::Window::make_current must run on the window's owning thread",
        });
    }

    glfwMakeContextCurrent(native_window(window_));
    if (glfwGetCurrentContext() != native_window(window_)) {
        return std::unexpected(diagnostic(
            vng::window::ErrorCode::operation_failed,
            "GLFW did not make the requested OpenGL context current"));
    }
    glfwSwapInterval(swap_interval_);
    return current_context_access();
}

std::expected<vng::opengl::CurrentContextAccess, vng::window::Diagnostic>
Window::current_context_access() const {
    if (!valid()) {
        return std::unexpected(vng::window::Diagnostic{
            .code = vng::window::ErrorCode::invalid_window,
            .message = "glfw_opengl::Window::current_context_access called on an empty window",
        });
    }
    if (!vng::window::detail::GlfwWindowAccess::on_owner_thread(window_)) {
        return std::unexpected(vng::window::Diagnostic{
            .code = vng::window::ErrorCode::wrong_thread,
            .message = "OpenGL context access must be requested on the window's owning thread",
        });
    }
    if (glfwGetCurrentContext() != native_window(window_)) {
        return std::unexpected(vng::window::Diagnostic{
            .code = vng::window::ErrorCode::context_not_current,
            .message = "The GLFW window's OpenGL context is not current",
        });
    }
    return vng::opengl::CurrentContextAccess{
        .lifetime = context_lifetime_,
        .resolve = resolve_procedure,
        .is_current = is_context_current,
        .owner_thread = std::this_thread::get_id(),
        .required_default_framebuffer_encoding =
            required_default_framebuffer_encoding_,
    };
}

std::expected<void, vng::window::Diagnostic> Window::present() {
    if (!valid()) {
        return std::unexpected(vng::window::Diagnostic{
            .code = vng::window::ErrorCode::invalid_window,
            .message = "glfw_opengl::Window::present called on an empty window",
        });
    }
    if (!vng::window::detail::GlfwWindowAccess::on_owner_thread(window_)) {
        return std::unexpected(vng::window::Diagnostic{
            .code = vng::window::ErrorCode::wrong_thread,
            .message = "glfw_opengl::Window::present must run on the window's owning thread",
        });
    }
    if (glfwGetCurrentContext() != native_window(window_)) {
        return std::unexpected(vng::window::Diagnostic{
            .code = vng::window::ErrorCode::context_not_current,
            .message = "glfw_opengl::Window::present requires its context to be current",
        });
    }
    glfwSwapBuffers(native_window(window_));
    return {};
}

void Window::release_current() noexcept {
    if (window_.valid()
        && vng::window::detail::GlfwWindowAccess::on_owner_thread(window_)
        && glfwGetCurrentContext() == native_window(window_)) {
        glfwMakeContextCurrent(nullptr);
    }
}

void Window::release_context_noexcept() noexcept {
    if (!context_lifetime_) {
        return;
    }
    if (window_.valid()
        && !vng::window::detail::GlfwWindowAccess::on_owner_thread(window_)) {
        // See GlfwWindow's matching invariant: GLFW cannot safely detach or
        // destroy this context on a foreign thread.
        std::terminate();
    }
    context_lifetime_->invalidate();
    release_current();
    context_lifetime_.reset();
}

std::expected<Window, vng::window::Diagnostic> create_window(
    const vng::window::WindowDesc& window,
    const vng::opengl::ContextDesc& context) {
    constexpr auto maximum_int =
        static_cast<std::uint32_t>(std::numeric_limits<int>::max());
    if (context.version.major > maximum_int
        || context.version.minor > maximum_int
        || context.samples > maximum_int) {
        return std::unexpected(vng::window::Diagnostic{
            .code = vng::window::ErrorCode::window_creation_failed,
            .message = "OpenGL context version and sample count must fit a native int",
        });
    }
    if (context.version.major < 4
        || (context.version.major == 4 && context.version.minor < 6)) {
        return std::unexpected(vng::window::Diagnostic{
            .code = vng::window::ErrorCode::window_creation_failed,
            .message = "The vng OpenGL backend requires an OpenGL 4.6 or newer context",
        });
    }
    switch (context.default_framebuffer_encoding) {
    case vng::render::ColorEncoding::linear:
    case vng::render::ColorEncoding::srgb:
        break;
    default:
        return std::unexpected(vng::window::Diagnostic{
            .code = vng::window::ErrorCode::window_creation_failed,
            .message = "OpenGL default framebuffer has an invalid requested color encoding",
        });
    }

    auto created = vng::window::detail::GlfwWindowAccess::create(
        window,
        configure_opengl,
        &context);
    if (!created) {
        return std::unexpected(std::move(created.error()));
    }

    auto* handle = native_window(*created);
    auto lifetime = std::make_shared<vng::opengl::ContextLifetime>(
        static_cast<const void*>(handle));
    return Window(
        std::move(*created),
        std::move(lifetime),
        context.default_framebuffer_encoding,
        context.swap_interval);
}

} // namespace vng::glfw_opengl
