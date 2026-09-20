#pragma once
#include <vng/editor/preview.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>

namespace editor_example {
inline constexpr vng::Extent2D default_preview_extent{640, 480};
// A bounded, lazily backed transport allocation, not the rendering resolution.
inline constexpr vng::Extent2D preview_capacity{4096, 4096};

[[nodiscard]] constexpr vng::Extent2D fit_preview_extent(vng::Extent2D source,
                                                         vng::Extent2D limit) {
    using namespace vng;
    if (source.empty() || limit.empty())
        return {};
    if (source.width <= limit.width && source.height <= limit.height)
        return source;
    if (static_cast<u64>(source.width) * limit.height >=
        static_cast<u64>(source.height) * limit.width)
        return {limit.width, std::max(1U, static_cast<u32>(static_cast<u64>(source.height) *
                                                           limit.width / source.width))};
    return {std::max(1U, static_cast<u32>(static_cast<u64>(source.width) * limit.height /
                                          source.height)),
            limit.height};
}

// Match physical pixels, not logical UI coordinates (including HiDPI).
[[nodiscard]] inline vng::Extent2D preview_pixel_extent(vng::Vec2 image_size, vng::Vec2 window_size,
                                                        vng::Extent2D framebuffer) {
    using namespace vng;
    if (framebuffer.empty() || !std::isfinite(image_size.x) || !std::isfinite(image_size.y) ||
        !std::isfinite(window_size.x) || !std::isfinite(window_size.y) || image_size.x <= 0 ||
        image_size.y <= 0 || window_size.x <= 0 || window_size.y <= 0)
        return {};
    const auto width =
        std::round(static_cast<double>(image_size.x) * framebuffer.width / window_size.x);
    const auto height =
        std::round(static_cast<double>(image_size.y) * framebuffer.height / window_size.y);
    // Clamp before narrowing even for malformed external size reports.
    const auto scale = std::min({1.0, preview_capacity.width / std::max(1.0, width),
                                 preview_capacity.height / std::max(1.0, height)});
    return {static_cast<u32>(std::max(1.0, std::floor(width * scale))),
            static_cast<u32>(std::max(1.0, std::floor(height * scale)))};
}
[[nodiscard]] inline std::string encode_viewport_size(vng::Extent2D extent) {
    return "viewport\n" + std::to_string(extent.width) + " " + std::to_string(extent.height);
}
[[nodiscard]] inline vng::editor::preview::Result<vng::Extent2D>
decode_viewport_size(std::string_view payload, vng::Extent2D capacity) {
    using namespace vng;
    Extent2D result{};
    const auto split = payload.find(' ');
    const auto parse = [](std::string_view text, u32& value) {
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        return error == std::errc{} && end == text.data() + text.size();
    };
    if (split == std::string_view::npos || !parse(payload.substr(0, split), result.width) ||
        !parse(payload.substr(split + 1), result.height) || result.empty() ||
        result.width > capacity.width || result.height > capacity.height)
        return std::unexpected(
            editor::preview::Diagnostic{"Invalid preview viewport size or capacity"});
    return result;
}
} // namespace editor_example
