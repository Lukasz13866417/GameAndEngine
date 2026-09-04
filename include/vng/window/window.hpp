#pragma once

#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>

#include <vng/core/types.hpp>

namespace vng::window {

// Properties of the native window itself. Graphics-API context and
// presentation choices deliberately live with the selected graphics backend.
struct WindowDesc final {
    std::uint32_t width{1280};
    std::uint32_t height{720};
    std::string title{"Vibe Engine"};
    bool visible{true};
    bool resizable{true};

    constexpr bool operator==(const WindowDesc&) const = default;
};

// Zero-overhead common surface for platform-window implementations. Graphics
// context creation and presentation deliberately stay outside this concept:
// those operations differ substantially between OpenGL, Vulkan, and other
// graphics backends.
template<class T>
concept Window = requires(T& value, const T& constant, std::string_view title) {
    { constant.valid() } noexcept -> std::same_as<bool>;
    { constant.should_close() } noexcept -> std::same_as<bool>;
    { value.request_close() } noexcept -> std::same_as<void>;
    { constant.framebuffer_extent() } noexcept -> std::same_as<Extent2D>;
    { value.set_title(title) } -> std::same_as<void>;
    { constant.poll_events() } noexcept -> std::same_as<void>;
    { constant.wait_events() } noexcept -> std::same_as<void>;
};

} // namespace vng::window
