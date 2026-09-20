#include "spaceflight_inspection.hpp"
#include "spaceship_renderer.hpp"
#include "analysis_report.hpp"

#include <vng/analysis/export.hpp>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace example::spaceflight {
namespace {
using namespace vng;
using resources::Result;

resources::Diagnostic failure(std::string message)
{ return {.code = resources::ErrorCode::operation_failed, .message = std::move(message)}; }

Result<void> write_text(const std::filesystem::path& path, std::string_view text)
{
    auto* file = std::fopen(path.c_str(), "wbx");
    if (!file) return std::unexpected(failure("Cannot create " + path.string()
        + ": " + std::strerror(errno)));
    const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed) return std::unexpected(failure("Writing " + path.string() + " failed"));
    return {};
}

u8 byte(f32 value)
{
    if (!std::isfinite(value)) return 255;
    return static_cast<u8>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}
} // namespace

Result<void> export_diagnostics(const analysis::DiagnosticSweep& sweep,
    const std::filesystem::path& directory)
{
    const auto& evidence = sweep.production();
    const auto* positions = evidence.observation(WorldPosition{});
    const auto* normals = evidence.observation(WorldNormal{});
    const auto* radiance = evidence.observation(Radiance{});
    if (!positions || !normals || !radiance)
        return std::unexpected(failure("Spaceflight diagnostics need WorldPosition, WorldNormal and Radiance observations"));
    auto exported = analysis::export_evidence_directory(evidence, directory, {
        .write_pixel_table = false, .write_additional_channel_table = false,
        .write_all_visualizations = true});
    if (!exported) return std::unexpected(resources::to_diagnostic(std::move(exported.error())));
    const auto extent = evidence.capture().extent();
    const auto keys = evidence.capture().surface_keys().pixels();
    const auto depth = evidence.capture().device_depth().pixels();
    std::vector<u8> normal_pixels(keys.size() * 4, 0), radiance_pixels(keys.size() * 4, 0);
    std::ostringstream probes;
    probes.imbue(std::locale::classic());
    probes << std::setprecision(std::numeric_limits<float>::max_digits10)
        << "x,y,item,face,device_depth,world_x,world_y,world_z,normal_x,normal_y,normal_z,radiance_r,radiance_g,radiance_b,radiance_a\n";
    for (u32 y = 0; y < extent.height; ++y) {
        for (u32 x = 0; x < extent.width; ++x) {
            const auto index = static_cast<std::size_t>(y) * extent.width + x;
            normal_pixels[index*4+3] = 255;
            radiance_pixels[index*4+3] = 255;
            if (!keys[index].has_surface()) continue;
            const auto& normal = normals->pixels()[index];
            const auto& value = radiance->pixels()[index];
            for (u32 c = 0; c < 3; ++c) {
                normal_pixels[index*4+c] = byte(normal[c] * 0.5F + 0.5F);
                const auto positive = std::max(value[c], 0.0F);
                radiance_pixels[index*4+c] = byte(positive / (1.0F + positive));
            }
            if (x % 8 != 0 || y % 8 != 0) continue;
            const auto& position = positions->pixels()[index];
            probes << x << ',' << y << ',' << keys[index].item.value << ','
                << keys[index].primitive.value << ',' << depth[index] << ','
                << position.x << ',' << position.y << ',' << position.z << ','
                << normal.x << ',' << normal.y << ',' << normal.z << ','
                << value.x << ',' << value.y << ',' << value.z << ',' << value.w << '\n';
        }
    }
    if (auto saved = write_rgba8_png(directory / "normals.png", extent, normal_pixels); !saved) return saved;
    if (auto saved = write_rgba8_png(directory / "radiance.png", extent, radiance_pixels); !saved) return saved;
    if (auto saved = write_text(directory / "probes.csv", probes.str()); !saved) return saved;
    std::ostringstream report;
    report.imbue(std::locale::classic());
    report << "Ship-only enhanced rendering; excludes background, bloom and tone mapping.\n"
        << "Canonical color is RGBA8 and can clip HDR; Radiance observations preserve floating-point values.\n"
        << "normals.png: normalized world normal mapped by N * 0.5 + 0.5.\n"
        << "radiance.png: max(Radiance, 0) / (1 + max(Radiance, 0)), for display only.\n"
        << "probes.csv: exact float samples on the 8-pixel grid, top-left image coordinates.\n\n";
    for (const auto& variant : sweep.variants()) {
        const auto name = variant.variant == analysis::DiagnosticVariant::production ? "production"
            : variant.variant == analysis::DiagnosticVariant::cull_disabled ? "no-cull" : "depth-always";
        report << name << ": " << variant.evidence.summary().covered_pixel_count
            << '/' << variant.evidence.summary().pixel_count << " covered pixels\n";
    }
    if (sweep.findings().empty()) report << "No issue exposed by the cull/depth sweep.\n";
    for (const auto& finding : sweep.findings()) report << finding.message << '\n';
    if (auto saved = write_text(directory / "spaceflight.txt", report.str()); !saved) return saved;
    example::print_analysis(sweep, Radiance{});
    std::cout << "Ship-only pre-bloom evidence: " << directory << '\n';
    return {};
}
} // namespace example::spaceflight
