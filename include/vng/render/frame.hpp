#pragma once

#include <array>
#include <concepts>
#include <optional>
#include <utility>

#include <vng/core/types.hpp>
#include <vng/render/color.hpp>

namespace vng::render {

// The window-system framebuffer selected by a graphics-context integration.
// Off-screen backends provide their own target value and begin_backend_frame
// overload; render core does not need to know those target types.
struct DefaultTarget final {};
inline constexpr DefaultTarget default_target{};

// Backend-neutral beginning-of-frame intent. Omitted clear values preserve the
// corresponding attachment. The extent is always explicit because it defines
// the viewport and the camera snapshot used by RenderView.
struct FrameDesc final {
    Extent2D extent{};

    // Encoding of the selected color target. Clear-color components use the
    // same linear working space as fragment outputs; the backend applies the
    // requested target conversion. This state is explicit even when color is
    // preserved so work never inherits an ambient conversion mode.
    ColorEncoding color_encoding{ColorEncoding::srgb};

    std::optional<std::array<f32, 4>> clear_color;
    std::optional<f32> clear_depth;

    friend constexpr bool operator==(const FrameDesc&, const FrameDesc&) = default;
};

namespace detail {

template<class Device, class Target>
[[nodiscard]] constexpr auto dispatch_begin_frame(
    Device& device,
    Target&& target,
    const FrameDesc& description)
    noexcept(noexcept(begin_backend_frame(
        device,
        std::forward<Target>(target),
        description)))
    -> decltype(begin_backend_frame(
        device,
        std::forward<Target>(target),
        description))
{
    // Deliberately unqualified: Device or Target selects the backend through
    // ADL, just like render::compile_pipeline.
    return begin_backend_frame(
        device,
        std::forward<Target>(target),
        description);
}

} // namespace detail

struct BeginFrame final {
    template<class Device, class Target>
    [[nodiscard]] constexpr auto operator()(
        Device& device,
        Target&& target,
        const FrameDesc& description) const
        noexcept(noexcept(detail::dispatch_begin_frame(
            device,
            std::forward<Target>(target),
            description)))
        -> decltype(detail::dispatch_begin_frame(
            device,
            std::forward<Target>(target),
            description))
    {
        return detail::dispatch_begin_frame(
            device,
            std::forward<Target>(target),
            description);
    }

    template<class Device>
    [[nodiscard]] constexpr auto operator()(
        Device& device,
        const FrameDesc& description) const
        noexcept(noexcept((*this)(device, default_target, description)))
        -> decltype((*this)(device, default_target, description))
    {
        return (*this)(device, default_target, description);
    }
};

inline constexpr BeginFrame begin_frame{};

template<class T>
concept Frame = requires(T& value, const T& constant) {
    { constant.active() } noexcept -> std::same_as<bool>;
    { constant.extent() } noexcept -> std::same_as<Extent2D>;
    value.end();
};

} // namespace vng::render
