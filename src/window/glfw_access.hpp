#pragma once

#include <expected>

#include <vng/window/glfw.hpp>

namespace vng::window::detail {

using GlfwHintConfigurer = void (*)(const void* user_data) noexcept;

// Private bridge used by statically linked window/graphics integrations. It
// keeps native handles and GLFW context hints out of the public window API.
class GlfwWindowAccess final {
public:
    [[nodiscard]] static std::expected<GlfwWindow, Diagnostic> create(
        const WindowDesc& description,
        GlfwHintConfigurer configure_client_api,
        const void* user_data = nullptr);

    [[nodiscard]] static void* native_handle(const GlfwWindow& window) noexcept;
    [[nodiscard]] static bool on_owner_thread(const GlfwWindow& window) noexcept;
};

} // namespace vng::window::detail
