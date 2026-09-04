#pragma once

#include <cstdint>

#include <vng/render/color.hpp>

namespace vng::opengl {

struct ContextVersion final {
    std::uint32_t major{4};
    std::uint32_t minor{6};

    constexpr bool operator==(const ContextVersion&) const = default;
};

// Requested properties for an OpenGL context and its default presentation
// surface. A window-system integration translates this neutral OpenGL request
// into its native context-creation API.
struct ContextDesc final {
    ContextVersion version{};
    bool debug{true};
    bool forward_compatible{true};
    std::uint32_t samples{};

    // Requests the physical encoding of the default framebuffer from the
    // window-system integration. It is a creation hint; Device queries the
    // actual attachment encoding after loading OpenGL.
    render::ColorEncoding default_framebuffer_encoding{
        render::ColorEncoding::srgb};

    // 0 presents immediately, 1 synchronizes to the display. Other values are
    // forwarded when supported by the selected window-system integration.
    std::int32_t swap_interval{1};

    constexpr bool operator==(const ContextDesc&) const = default;
};

} // namespace vng::opengl
