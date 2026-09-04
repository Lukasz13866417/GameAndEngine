#pragma once

#include <cstdint>
#include <expected>
#include <optional>

#include <vng/core/types.hpp>
#include <vng/window/glfw_opengl.hpp>

namespace example {

// Handles only native-window repetition. The demos keep frame creation,
// renderer submission, and frame completion visible in their own loops.
class WindowLoop final {
public:
    explicit WindowLoop(
        vng::glfw_opengl::Window& window,
        std::optional<std::uint64_t> frame_limit = std::nullopt) noexcept;

    [[nodiscard]] std::optional<vng::Extent2D> next_extent();
    [[nodiscard]] std::expected<void, vng::window::Diagnostic> present();

private:
    vng::glfw_opengl::Window& window_;
    std::optional<std::uint64_t> frames_remaining_;
};

} // namespace example
