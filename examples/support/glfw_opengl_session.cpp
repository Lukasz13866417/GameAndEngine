#include "glfw_opengl_session.hpp"

#include <utility>

namespace example {

GlfwOpenGLSession::GlfwOpenGLSession(
    vng::glfw_opengl::Window owned_window,
    vng::opengl::Device owned_device) noexcept
    : window_(std::move(owned_window)),
      device_(std::move(owned_device))
{}

std::expected<GlfwOpenGLSession, StartupDiagnostic>
GlfwOpenGLSession::create(
    const vng::window::WindowDesc& window_description,
    const vng::opengl::ContextDesc& context_description,
    const vng::window::PresentationDesc& presentation)
{
    auto window = vng::glfw_opengl::create_window(
        window_description, context_description, presentation);
    if (!window) {
        return std::unexpected(StartupDiagnostic{
            std::in_place_type<vng::window::Diagnostic>,
            std::move(window.error()),
        });
    }

    auto context = window->make_current();
    if (!context) {
        return std::unexpected(StartupDiagnostic{
            std::in_place_type<vng::window::Diagnostic>,
            std::move(context.error()),
        });
    }
    auto device = vng::opengl::Device::create(*context);
    if (!device) {
        return std::unexpected(StartupDiagnostic{
            std::in_place_type<vng::opengl::Diagnostic>,
            std::move(device.error()),
        });
    }

    return GlfwOpenGLSession{
        std::move(*window),
        std::move(*device),
    };
}

} // namespace example
