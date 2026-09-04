#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>

#include <vng/analysis/capture.hpp>
#include <vng/analysis/evidence.hpp>

namespace vng::analysis {

struct CaptureComparisonOptions final {
    u8 color_absolute_tolerance{};
    f32 depth_absolute_tolerance{};
    bool compare_color{true};
    bool compare_device_depth{true};
    bool compare_surface_keys{true};
    bool compare_manifest{true};
    // Frame IDs are allocation identities rather than image contents, so two
    // otherwise identical captures compare equal by default.
    bool compare_frame_id{};

    friend constexpr bool operator==(const CaptureComparisonOptions&,
                                     const CaptureComparisonOptions&) = default;
};

enum class ComparisonDiagnosticCode {
    invalid_depth_tolerance,
};

struct ComparisonDiagnostic final {
    ComparisonDiagnosticCode code{};
    std::string message;

    friend bool operator==(const ComparisonDiagnostic&,
                           const ComparisonDiagnostic&) = default;
};

struct CaptureComparison final {
    Extent2D left_extent{};
    Extent2D right_extent{};
    bool extent_matches{};
    bool frame_matches{true};
    bool manifest_matches{true};
    u64 compared_pixel_count{};
    u64 color_difference_count{};
    u64 depth_difference_count{};
    u64 surface_key_difference_count{};
    u8 maximum_color_component_difference{};
    f32 maximum_depth_difference{};
    std::optional<Pixel> first_color_difference;
    std::optional<Pixel> first_depth_difference;
    std::optional<Pixel> first_surface_key_difference;

    [[nodiscard]] bool equivalent() const noexcept
    {
        return extent_matches && frame_matches && manifest_matches
            && color_difference_count == 0
            && depth_difference_count == 0
            && surface_key_difference_count == 0;
    }

    friend bool operator==(const CaptureComparison&,
                           const CaptureComparison&) = default;
};

namespace detail {

[[nodiscard]] inline bool manifests_equal(
    const AnalysisManifest& left,
    const AnalysisManifest& right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.items().size(); ++index) {
        const auto& a = left.items()[index];
        const auto& b = right.items()[index];
        if (a.id != b.id || a.provenance != b.provenance
            || static_cast<bool>(a.primitives)
                != static_cast<bool>(b.primitives)) {
            return false;
        }
        if (!a.primitives) {
            continue;
        }
        if (!std::ranges::equal(
                a.primitives->sources(), b.primitives->sources())) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr u8 component_difference(u8 left, u8 right) noexcept
{
    return left > right
        ? static_cast<u8>(left - right)
        : static_cast<u8>(right - left);
}

[[nodiscard]] inline u8 color_difference(Rgba8 left, Rgba8 right) noexcept
{
    return std::max({
        component_difference(left.r, right.r),
        component_difference(left.g, right.g),
        component_difference(left.b, right.b),
        component_difference(left.a, right.a),
    });
}

[[nodiscard]] inline std::pair<bool, f32> compare_depth(
    f32 left,
    f32 right,
    f32 tolerance) noexcept
{
    if (std::bit_cast<u32>(left) == std::bit_cast<u32>(right)) {
        return {true, 0.0F};
    }
    if (!std::isfinite(left) || !std::isfinite(right)) {
        return {false, std::numeric_limits<f32>::infinity()};
    }
    const auto difference = std::abs(left - right);
    return {difference <= tolerance, difference};
}

} // namespace detail

[[nodiscard]] inline std::expected<CaptureComparison, ComparisonDiagnostic>
compare_captures(
    const AnalysisCapture& left,
    const AnalysisCapture& right,
    CaptureComparisonOptions options = {})
{
    if (!std::isfinite(options.depth_absolute_tolerance)
        || options.depth_absolute_tolerance < 0.0F) {
        return std::unexpected(ComparisonDiagnostic{
            .code = ComparisonDiagnosticCode::invalid_depth_tolerance,
            .message = "capture comparison depth tolerance must be finite and nonnegative",
        });
    }

    CaptureComparison result;
    result.left_extent = left.extent();
    result.right_extent = right.extent();
    result.extent_matches = left.extent() == right.extent();
    if (options.compare_frame_id) {
        result.frame_matches = left.frame() == right.frame();
    }
    if (options.compare_manifest) {
        result.manifest_matches = detail::manifests_equal(
            left.manifest(), right.manifest());
    }
    if (!result.extent_matches) {
        return result;
    }

    result.compared_pixel_count = static_cast<u64>(left.color().size());
    const auto width = left.extent().width;
    for (std::size_t index = 0; index < left.color().size(); ++index) {
        const Pixel pixel{
            static_cast<u32>(index % width),
            static_cast<u32>(index / width),
        };

        if (options.compare_color) {
            const auto difference = detail::color_difference(
                left.color().pixels()[index], right.color().pixels()[index]);
            result.maximum_color_component_difference = std::max(
                result.maximum_color_component_difference, difference);
            if (difference > options.color_absolute_tolerance) {
                ++result.color_difference_count;
                if (!result.first_color_difference) {
                    result.first_color_difference = pixel;
                }
            }
        }

        if (options.compare_device_depth) {
            const auto [equal, difference] = detail::compare_depth(
                left.device_depth().pixels()[index],
                right.device_depth().pixels()[index],
                options.depth_absolute_tolerance);
            result.maximum_depth_difference = std::max(
                result.maximum_depth_difference, difference);
            if (!equal) {
                ++result.depth_difference_count;
                if (!result.first_depth_difference) {
                    result.first_depth_difference = pixel;
                }
            }
        }

        if (options.compare_surface_keys
            && left.surface_keys().pixels()[index]
                != right.surface_keys().pixels()[index]) {
            ++result.surface_key_difference_count;
            if (!result.first_surface_key_difference) {
                result.first_surface_key_difference = pixel;
            }
        }
    }
    return result;
}

struct EvidenceComparisonOptions final {
    CaptureComparisonOptions capture;
    bool compare_request{true};
    bool compare_metadata{true};
    bool compare_additional_channels{true};
    // Backend artifacts can contain driver or machine information. Portable
    // comparisons therefore ignore them unless a caller explicitly asks for
    // an exact backend-artifact comparison.
    bool compare_backend_artifacts{};

    friend constexpr bool operator==(const EvidenceComparisonOptions&,
                                     const EvidenceComparisonOptions&) = default;
};

struct EvidenceComparison final {
    CaptureComparison capture;
    bool request_matches{true};
    bool metadata_matches{true};
    bool channel_layout_matches{true};
    bool backend_artifacts_match{true};
    u64 additional_value_difference_count{};
    std::optional<std::string> first_additional_channel_difference;
    std::optional<Pixel> first_additional_pixel_difference;
    std::optional<std::string> first_backend_artifact_difference;

    [[nodiscard]] bool equivalent() const noexcept
    {
        return capture.equivalent()
            && request_matches
            && metadata_matches
            && channel_layout_matches
            && backend_artifacts_match
            && additional_value_difference_count == 0;
    }

    friend bool operator==(const EvidenceComparison&,
                           const EvidenceComparison&) = default;
};

[[nodiscard]] inline std::expected<EvidenceComparison, ComparisonDiagnostic>
compare_evidence(
    const FrameEvidence& left,
    const FrameEvidence& right,
    EvidenceComparisonOptions options = {})
{
    auto capture = compare_captures(
        left.capture(), right.capture(), options.capture);
    if (!capture) {
        return std::unexpected(std::move(capture.error()));
    }

    EvidenceComparison result;
    result.capture = std::move(*capture);
    if (options.compare_request) {
        result.request_matches = left.request() == right.request();
    }
    if (options.compare_metadata) {
        result.metadata_matches = left.metadata() == right.metadata();
    }
    if (options.compare_backend_artifacts) {
        const auto left_artifacts = left.backend_artifacts();
        const auto right_artifacts = right.backend_artifacts();
        result.backend_artifacts_match = std::ranges::equal(
            left_artifacts, right_artifacts);
        if (!result.backend_artifacts_match) {
            const auto common_size = std::min(
                left_artifacts.size(), right_artifacts.size());
            std::size_t index = 0;
            while (index < common_size
                   && left_artifacts[index] == right_artifacts[index]) {
                ++index;
            }
            if (index < left_artifacts.size()) {
                result.first_backend_artifact_difference =
                    left_artifacts[index].name;
            } else if (index < right_artifacts.size()) {
                result.first_backend_artifact_difference =
                    right_artifacts[index].name;
            }
        }
    }
    if (!options.compare_additional_channels) {
        return result;
    }

    const auto left_channels = left.additional_channels();
    const auto right_channels = right.additional_channels();
    if (left_channels.size() != right_channels.size()) {
        result.channel_layout_matches = false;
        return result;
    }
    for (std::size_t channel_index = 0;
         channel_index < left_channels.size();
         ++channel_index) {
        const auto& a = left_channels[channel_index];
        const auto& b = right_channels[channel_index];
        if (a.name() != b.name()
            || a.value_kind() != b.value_kind()
            || a.extent() != b.extent()) {
            result.channel_layout_matches = false;
            return result;
        }
    }
    if (!result.capture.extent_matches) {
        return result;
    }

    const auto extent = left.capture().extent();
    for (std::size_t channel_index = 0;
         channel_index < left_channels.size();
         ++channel_index) {
        const auto& a = left_channels[channel_index];
        const auto& b = right_channels[channel_index];
        for (u32 y = 0; y < extent.height; ++y) {
            for (u32 x = 0; x < extent.width; ++x) {
                const Pixel pixel{x, y};
                if (a.at(pixel) == b.at(pixel)) {
                    continue;
                }
                ++result.additional_value_difference_count;
                if (!result.first_additional_channel_difference) {
                    result.first_additional_channel_difference = a.name();
                    result.first_additional_pixel_difference = pixel;
                }
            }
        }
    }
    return result;
}

} // namespace vng::analysis
