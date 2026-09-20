#pragma once

#include <concepts>
#include <expected>
#include <vng/window/diagnostic.hpp>

namespace vng::window {

// Presentation intent, not a graphics API's native swap interval/mode.
// Drivers/compositors may override this request; it is not an FPS guarantee.
enum class VSync { off, on };

struct PresentationDesc final {
    VSync vsync{VSync::on};
    constexpr bool operator==(const PresentationDesc&) const = default;
};

// Separate from Window: an input-only native window need not present images.
// The graphics/window integration owns this capability and its policy.
template<class T>
concept Presentable = requires(T& value, const T& constant, VSync mode) {
    { value.set_vsync(mode) } -> std::same_as<std::expected<void, Diagnostic>>;
    { constant.vsync() } noexcept -> std::same_as<VSync>;
    { value.present() } -> std::same_as<std::expected<void, Diagnostic>>;
};

} // namespace vng::window
