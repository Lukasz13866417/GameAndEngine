#pragma once

#include <optional>

#include <vng/render/color.hpp>

namespace vng::opengl {

// Facts queried from the currently associated default framebuffer. Physical
// attachment encoding and conversion support are separate: enabling
// GL_FRAMEBUFFER_SRGB cannot turn a linear attachment into an sRGB target.
struct DefaultFramebufferCapabilities final {
    std::optional<render::ColorEncoding> color_encoding;
    bool srgb_conversion_supported{};

    [[nodiscard]] constexpr bool supports(
        render::ColorEncoding requested) const noexcept {
        return color_encoding == requested
            && (requested != render::ColorEncoding::srgb
                || srgb_conversion_supported);
    }

    constexpr bool operator==(
        const DefaultFramebufferCapabilities&) const = default;
};

} // namespace vng::opengl
