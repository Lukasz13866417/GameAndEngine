#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <utility>

#include <vng/window/glfw_opengl.hpp>

namespace vng::test {

[[nodiscard]] inline std::expected<glfw_opengl::Window, window::Diagnostic>
create_hidden_opengl_window(
    std::uint32_t width,
    std::uint32_t height,
    std::string title)
{
    return glfw_opengl::create_window(
        window::WindowDesc{
            .width = width,
            .height = height,
            .title = std::move(title),
            .visible = false,
            .resizable = false,
        },
        opengl::ContextDesc{
            .debug = true,
            .samples = 0,
            .default_framebuffer_encoding =
                render::ColorEncoding::linear,
        }, {.vsync = window::VSync::off});
}

} // namespace vng::test
