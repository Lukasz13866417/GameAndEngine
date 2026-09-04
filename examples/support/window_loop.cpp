#include "window_loop.hpp"

namespace example {

WindowLoop::WindowLoop(
    vng::glfw_opengl::Window& window,
    std::optional<std::uint64_t> frame_limit) noexcept
    : window_(window),
      frames_remaining_(frame_limit)
{
    if (frames_remaining_ && *frames_remaining_ == 0) {
        window_.request_close();
    }
}

std::optional<vng::Extent2D> WindowLoop::next_extent()
{
    while (!window_.should_close()) {
        window_.poll_events();
        if (window_.should_close()) {
            return std::nullopt;
        }

        const auto extent = window_.framebuffer_extent();
        if (!extent.empty()) {
            return extent;
        }
        window_.wait_events();
    }
    return std::nullopt;
}

std::expected<void, vng::window::Diagnostic> WindowLoop::present()
{
    auto presented = window_.present();
    if (!presented) {
        return presented;
    }
    if (frames_remaining_ && --*frames_remaining_ == 0) {
        window_.request_close();
    }
    return {};
}

} // namespace example
