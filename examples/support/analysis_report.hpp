#pragma once

#include <concepts>
#include <iostream>

#include <vng/analysis/analysis.hpp>
#include <vng/gfx/semantic.hpp>

namespace example {

// Reporting is example UI, not part of capture itself. The semantic remains
// a parameter so the demo still shows exactly which shader value it requested.
template<vng::gfx::SemanticType Observation>
    requires std::same_as<vng::gfx::semantic_value_t<Observation>, vng::Vec4>
void print_analysis(
    const vng::analysis::DiagnosticSweep& sweep,
    Observation observation)
{
    const auto& evidence = sweep.production();
    const auto& summary = evidence.summary();
    std::cout << "analysis: " << summary.covered_pixel_count << '/'
              << summary.pixel_count << " pixels covered, "
              << summary.visible_item_count << " item, "
              << summary.visible_primitive_count << " primitives\n";

    const vng::analysis::Pixel center{
        summary.extent.width / 2,
        summary.extent.height / 2,
    };
    auto inspected = evidence.inspect(center);
    if (!inspected) {
        std::cout << "  center inspection failed: "
                  << inspected.error().message << '\n';
    } else if (inspected->canonical.surface) {
        const auto& surface = *inspected->canonical.surface;
        std::cout << "  center: source face " << surface.face
                  << ", depth " << inspected->canonical.device_depth;
        if (const auto* value = inspected->observation(observation)) {
            std::cout << ", observed value ("
                      << value->x << ", " << value->y << ", "
                      << value->z << ", " << value->w << ')';
        }
        std::cout << '\n';
    } else {
        std::cout << "  center: background\n";
    }

    std::cout << "  coverage variants:";
    for (const auto& variant : sweep.variants()) {
        const char* name = "production";
        if (variant.variant
            == vng::analysis::DiagnosticVariant::cull_disabled) {
            name = "no-cull";
        } else if (variant.variant
                   == vng::analysis::DiagnosticVariant::depth_always) {
            name = "depth-always";
        }
        std::cout << ' ' << name << '='
                  << variant.evidence.summary().covered_pixel_count;
    }
    std::cout << '\n';

    if (sweep.findings().empty()) {
        std::cout << "  diagnosis: no issue exposed by the cull/depth sweep\n";
    } else {
        for (const auto& finding : sweep.findings()) {
            std::cout << "  diagnosis: " << finding.message << '\n';
        }
    }
    std::cout << "  evidence: " << evidence.additional_channels().size()
              << " shader channel, " << evidence.backend_artifacts().size()
              << " backend text artifacts\n";
}

} // namespace example
