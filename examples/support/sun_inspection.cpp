#include "sun_inspection.hpp"
#include "sun_renderer.hpp"
#include "presentation.hpp"

#include <vng/analysis/export.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace example::sun {
namespace {
using namespace vng;
u8 encode(f32 value)
{
    return static_cast<u8>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}
} // namespace

vng::resources::Result<void> export_diagnostics(const vng::analysis::DiagnosticSweep& sweep,
    const std::filesystem::path& directory, vng::Vec3 center)
{
    const auto& evidence = sweep.production();
    auto exported = analysis::export_evidence_directory(evidence, directory, {
        .write_pixel_table = false, .write_additional_channel_table = false,
        .write_all_visualizations = true});
    if (!exported) return std::unexpected(resources::Diagnostic{
        .code = resources::ErrorCode::invalid_argument, .message = exported.error().message});
    const auto* normals = evidence.observation(SurfaceNormal{});
    const auto* radiance = evidence.observation(Radiance{});
    const auto* positions = evidence.observation(WorldPosition{});
    if (!normals || !radiance || !positions) return std::unexpected(resources::Diagnostic{
        .code = resources::ErrorCode::invalid_argument, .message = "Solar diagnostic observations are incomplete"});
    const auto extent = evidence.capture().extent();
    std::vector<u8> normal_pixels(static_cast<std::size_t>(extent.width) * extent.height * 4);
    for (std::size_t i = 3; i < normal_pixels.size(); i += 4) normal_pixels[i] = 255;
    auto light_pixels = normal_pixels;
    f32 min_radius = 1000, max_radius = 0, peak = 0;
    for (u32 y = 0; y < extent.height; ++y) for (u32 x = 0; x < extent.width; ++x) {
        const analysis::Pixel pixel{x, y};
        if (!evidence.capture().surface_at(pixel)) continue;
        const auto n = normals->at(pixel);
        const auto light = radiance->at(pixel);
        const auto p = positions->at(pixel);
        const auto dx = p.x-center.x, dy = p.y-center.y, dz = p.z-center.z;
        const auto radius = std::sqrt(dx*dx + dy*dy + dz*dz);
        min_radius = std::min(min_radius, radius); max_radius = std::max(max_radius, radius);
        // Enhanced captures have already been normalized to top-left origin.
        const auto offset = (static_cast<std::size_t>(y) * extent.width + x) * 4;
        for (u32 component = 0; component < 3; ++component) {
            normal_pixels[offset + component] = encode(n[component] * 0.5F + 0.5F);
            const auto value = std::max(light[component], 0.0F);
            peak = std::max(peak, value);
            light_pixels[offset + component] = encode(std::pow(value / (1.0F + value), 1.0F / 2.2F));
        }
    }
    if (auto done = example::write_rgba8_png(directory / "normals.png", extent, normal_pixels); !done) return done;
    if (auto done = example::write_rgba8_png(directory / "radiance.png", extent, light_pixels); !done) return done;
    std::cout << "Solar diagnostics: " << evidence.summary().covered_pixel_count
        << " covered pixels; surface radius [" << min_radius << ", " << max_radius
        << "]; peak linear radiance " << peak << '\n';
    for (const auto& finding : sweep.findings()) std::cout << "  " << finding.message << '\n';
    std::cout << "Evidence (unfiltered surface, before corona/bloom): " << directory << '\n';
    return {};
}
} // namespace example::sun
