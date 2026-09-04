#pragma once

#include <cstddef>

#include <vng/analysis/capture.hpp>

namespace vng::analysis {

struct VisualizationPalette final {
    Rgba8 background{0, 0, 0, 255};
    Rgba8 covered{255, 255, 255, 255};

    friend constexpr bool operator==(const VisualizationPalette&,
                                     const VisualizationPalette&) = default;
};

struct DerivedVisualizations final {
    AnalysisCapture::ColorImage coverage;
    AnalysisCapture::ColorImage items;
    AnalysisCapture::ColorImage primitives;
};

namespace detail {

[[nodiscard]] constexpr u64 mix_visualization_id(u64 value) noexcept
{
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] constexpr u8 visible_color_component(u64 value) noexcept
{
    // Keep generated identifiers away from black so every covered pixel is
    // visibly distinguishable from the default background.
    return static_cast<u8>(64U + (value & 0xBFU));
}

[[nodiscard]] constexpr Rgba8 identifier_color(u64 identifier) noexcept
{
    const auto mixed = mix_visualization_id(identifier);
    return {
        visible_color_component(mixed),
        visible_color_component(mixed >> 16U),
        visible_color_component(mixed >> 32U),
        255,
    };
}

} // namespace detail

[[nodiscard]] inline AnalysisCapture::ColorImage make_coverage_visualization(
    const AnalysisCapture& capture,
    VisualizationPalette palette = {})
{
    AnalysisCapture::ColorImage result{
        capture.extent(), palette.background};
    const auto keys = capture.surface_keys().pixels();
    auto pixels = result.pixels();
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        if (keys[index].has_surface()) {
            pixels[index] = palette.covered;
        }
    }
    return result;
}

[[nodiscard]] inline AnalysisCapture::ColorImage make_item_visualization(
    const AnalysisCapture& capture,
    VisualizationPalette palette = {})
{
    AnalysisCapture::ColorImage result{
        capture.extent(), palette.background};
    const auto keys = capture.surface_keys().pixels();
    auto pixels = result.pixels();
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        const auto key = keys[index];
        if (key.has_surface()) {
            pixels[index] = detail::identifier_color(key.item.value);
        }
    }
    return result;
}

[[nodiscard]] inline AnalysisCapture::ColorImage make_primitive_visualization(
    const AnalysisCapture& capture,
    VisualizationPalette palette = {})
{
    AnalysisCapture::ColorImage result{
        capture.extent(), palette.background};
    const auto keys = capture.surface_keys().pixels();
    auto pixels = result.pixels();
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        const auto key = keys[index];
        if (key.has_surface()) {
            const auto identifier =
                (static_cast<u64>(key.item.value) << 32U)
                | static_cast<u64>(key.primitive.value);
            pixels[index] = detail::identifier_color(identifier);
        }
    }
    return result;
}

[[nodiscard]] inline DerivedVisualizations make_visualizations(
    const AnalysisCapture& capture,
    VisualizationPalette palette = {})
{
    return {
        .coverage = make_coverage_visualization(capture, palette),
        .items = make_item_visualization(capture, palette),
        .primitives = make_primitive_visualization(capture, palette),
    };
}

} // namespace vng::analysis
