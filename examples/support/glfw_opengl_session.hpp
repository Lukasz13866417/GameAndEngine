#pragma once

#include "diagnostics.hpp"

#include <expected>

#include <vng/opengl/device.hpp>
#include <vng/glfw_opengl/glfw_opengl.hpp>

namespace example {

// Owns the exact backend pair used by these demos. Member order matters:
// Device is destroyed while its GLFW-owned OpenGL context still exists.
class GlfwOpenGLSession final {
public:
    [[nodiscard]] static std::expected<
        GlfwOpenGLSession,
        StartupDiagnostic>
    create(
        const vng::window::WindowDesc& window_description,
        const vng::opengl::ContextDesc& context_description,
        const vng::window::PresentationDesc& presentation = {});

    GlfwOpenGLSession(GlfwOpenGLSession&&) noexcept = default;
    GlfwOpenGLSession& operator=(GlfwOpenGLSession&&) = delete;
    GlfwOpenGLSession(const GlfwOpenGLSession&) = delete;
    GlfwOpenGLSession& operator=(const GlfwOpenGLSession&) = delete;

    [[nodiscard]] vng::glfw_opengl::Window& window() noexcept
    {
        return window_;
    }

    [[nodiscard]] vng::opengl::Device& device() noexcept
    {
        return device_;
    }

private:
    GlfwOpenGLSession(
        vng::glfw_opengl::Window owned_window,
        vng::opengl::Device owned_device) noexcept;

    // Keep this order: reverse destruction releases the device before the
    // GLFW-owned context and native window.
    vng::glfw_opengl::Window window_;
    vng::opengl::Device device_;
};

} // namespace example
