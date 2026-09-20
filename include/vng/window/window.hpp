#pragma once

#include <concepts>
#include <expected>
#include <cstdint>
#include <string>
#include <string_view>

#include <vng/core/types.hpp>
#include <vng/input/input.hpp>
#include <vng/window/diagnostic.hpp>

namespace vng::window {

// Logical keys available to simple interactive applications. Native scan
// codes and platform key constants stay inside the window backend.
using Key = input::Key;

// Properties of the native window itself. Graphics-API context and
// presentation choices deliberately live with the selected graphics backend.
struct WindowDesc final {
    std::uint32_t width{1280};
    std::uint32_t height{720};
    std::string title{"Vibe Engine"};
    bool visible{true};
    bool resizable{true};
    bool maximized{false}; // Decorated window filling the desktop work area.

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
    { value.cancel_close() } noexcept -> std::same_as<void>;
    { constant.visible() } noexcept -> std::same_as<bool>;
    { value.show() } noexcept -> std::same_as<void>;
    { value.hide() } noexcept -> std::same_as<void>;
    { value.maximize() } noexcept -> std::same_as<void>;
    { value.restore() } noexcept -> std::same_as<void>;
    { constant.maximized() } noexcept -> std::same_as<bool>;
    { constant.fullscreen() } noexcept -> std::same_as<bool>;
    { value.set_fullscreen(true) } -> std::same_as<std::expected<void, Diagnostic>>;
    { constant.framebuffer_extent() } noexcept -> std::same_as<Extent2D>;
    { value.set_title(title) } -> std::same_as<void>;
    { constant.poll_events() } noexcept -> std::same_as<void>;
    { constant.wait_events() } noexcept -> std::same_as<void>;
};

} // namespace vng::window
