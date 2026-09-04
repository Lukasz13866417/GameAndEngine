#pragma once

#include <algorithm>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <vng/analysis/evidence.hpp>

namespace vng::analysis {

// Each counterfactual changes one renderer-owned state variable while keeping
// tickets, camera, shader IR, target extent, and readback semantics fixed.
enum class DiagnosticVariant {
    production,
    cull_disabled,
    depth_always,
};

struct VariantEvidence final {
    DiagnosticVariant variant{DiagnosticVariant::production};
    FrameEvidence evidence;
};

enum class FindingCode {
    no_visible_geometry,
    likely_culling,
    likely_depth_rejection,
    covered_but_near_black,
    non_finite_depth,
    invisible_items,
};

enum class FindingConfidence {
    hint,
    strong,
};

struct DiagnosticFinding final {
    FindingCode code{};
    FindingConfidence confidence{FindingConfidence::hint};
    std::string message;

    friend bool operator==(const DiagnosticFinding&,
                           const DiagnosticFinding&) = default;
};

enum class SweepDiagnosticCode {
    missing_production,
    duplicate_variant,
    mismatched_extent,
    missing_invocation_identity,
    mismatched_invocation_identity,
};

enum class InvocationIdentityComponent {
    workload,
    view,
    renderer,
    target,
};

struct SweepDiagnostic final {
    SweepDiagnosticCode code{};
    std::string message;
    std::optional<DiagnosticVariant> variant;
    std::optional<InvocationIdentityComponent> identity_component;
};

class DiagnosticSweep final {
public:
    [[nodiscard]] static std::expected<DiagnosticSweep, SweepDiagnostic> create(
        std::vector<VariantEvidence> variants)
    {
        std::ranges::sort(variants, {}, &VariantEvidence::variant);
        for (std::size_t index = 1; index < variants.size(); ++index) {
            if (variants[index - 1].variant == variants[index].variant) {
                return std::unexpected(SweepDiagnostic{
                    .code = SweepDiagnosticCode::duplicate_variant,
                    .message = "diagnostic sweep contains the same variant more than once",
                    .variant = variants[index].variant,
                    .identity_component = std::nullopt,
                });
            }
        }

        const auto production = std::ranges::find(
            variants,
            DiagnosticVariant::production,
            &VariantEvidence::variant);
        if (production == variants.end()) {
            return std::unexpected(SweepDiagnostic{
                .code = SweepDiagnosticCode::missing_production,
                .message = "diagnostic sweep requires production evidence",
                .variant = std::nullopt,
                .identity_component = std::nullopt,
            });
        }
        const auto extent = production->evidence.capture().extent();
        for (const auto& variant : variants) {
            if (variant.evidence.capture().extent() != extent) {
                return std::unexpected(SweepDiagnostic{
                    .code = SweepDiagnosticCode::mismatched_extent,
                    .message = "all diagnostic sweep variants must use the same extent",
                    .variant = variant.variant,
                    .identity_component = std::nullopt,
                });
            }
        }

        const auto* production_identity = identity_of(*production);
        if (production_identity == nullptr) {
            return std::unexpected(missing_identity_diagnostic(*production));
        }
        for (const auto& variant : variants) {
            const auto* identity = identity_of(variant);
            if (identity == nullptr) {
                return std::unexpected(missing_identity_diagnostic(variant));
            }
            if (const auto mismatch = first_identity_mismatch(
                    *production_identity, *identity)) {
                return std::unexpected(SweepDiagnostic{
                    .code = SweepDiagnosticCode::mismatched_invocation_identity,
                    .message = "diagnostic sweep variant belongs to a different render invocation",
                    .variant = variant.variant,
                    .identity_component = mismatch,
                });
            }
        }

        auto findings = infer_findings(variants);
        return DiagnosticSweep{
            std::move(variants), std::move(findings)};
    }

    [[nodiscard]] const FrameEvidence* find(
        DiagnosticVariant variant) const noexcept
    {
        const auto found = std::ranges::lower_bound(
            variants_, variant, {}, &VariantEvidence::variant);
        return found != variants_.end() && found->variant == variant
            ? &found->evidence : nullptr;
    }

    [[nodiscard]] const FrameEvidence& production() const noexcept
    {
        return *find(DiagnosticVariant::production);
    }

    [[nodiscard]] std::span<const VariantEvidence> variants() const noexcept
    {
        return variants_;
    }

    [[nodiscard]] std::span<const DiagnosticFinding> findings() const noexcept
    {
        return findings_;
    }

private:
    DiagnosticSweep(
        std::vector<VariantEvidence> variants,
        std::vector<DiagnosticFinding> findings) noexcept
        : variants_(std::move(variants)), findings_(std::move(findings))
    {}

    [[nodiscard]] static const RenderInvocationIdentity* identity_of(
        const VariantEvidence& variant) noexcept
    {
        const auto& identity = variant.evidence.metadata().invocation;
        return identity && identity->complete() ? &*identity : nullptr;
    }

    [[nodiscard]] static std::optional<InvocationIdentityComponent>
    first_incomplete_component(const RenderInvocationIdentity& identity) noexcept
    {
        if (identity.workload_fingerprint.empty()) {
            return InvocationIdentityComponent::workload;
        }
        if (identity.view_fingerprint.empty()) {
            return InvocationIdentityComponent::view;
        }
        if (identity.renderer_fingerprint.empty()) {
            return InvocationIdentityComponent::renderer;
        }
        if (identity.target_fingerprint.empty()) {
            return InvocationIdentityComponent::target;
        }
        return std::nullopt;
    }

    [[nodiscard]] static SweepDiagnostic missing_identity_diagnostic(
        const VariantEvidence& variant)
    {
        const auto& identity = variant.evidence.metadata().invocation;
        return {
            .code = SweepDiagnosticCode::missing_invocation_identity,
            .message = identity
                ? "diagnostic sweep variant has an incomplete render-invocation identity"
                : "diagnostic sweep variant has no render-invocation identity",
            .variant = variant.variant,
            .identity_component = identity
                ? first_incomplete_component(*identity)
                : std::nullopt,
        };
    }

    [[nodiscard]] static std::optional<InvocationIdentityComponent>
    first_identity_mismatch(
        const RenderInvocationIdentity& expected,
        const RenderInvocationIdentity& actual) noexcept
    {
        if (expected.workload_fingerprint != actual.workload_fingerprint) {
            return InvocationIdentityComponent::workload;
        }
        if (expected.view_fingerprint != actual.view_fingerprint) {
            return InvocationIdentityComponent::view;
        }
        if (expected.renderer_fingerprint != actual.renderer_fingerprint) {
            return InvocationIdentityComponent::renderer;
        }
        if (expected.target_fingerprint != actual.target_fingerprint) {
            return InvocationIdentityComponent::target;
        }
        return std::nullopt;
    }

    [[nodiscard]] static bool all_covered_pixels_near_black(
        const AnalysisCapture& capture) noexcept
    {
        bool covered = false;
        const auto colors = capture.color().pixels();
        const auto keys = capture.surface_keys().pixels();
        for (std::size_t index = 0; index < colors.size(); ++index) {
            if (!keys[index].has_surface()) {
                continue;
            }
            covered = true;
            const auto color = colors[index];
            if (color.r > 2 || color.g > 2 || color.b > 2) {
                return false;
            }
        }
        return covered;
    }

    [[nodiscard]] static std::vector<DiagnosticFinding> infer_findings(
        const std::vector<VariantEvidence>& variants)
    {
        const auto find = [&](DiagnosticVariant variant)
            -> const FrameEvidence* {
            const auto found = std::ranges::find(
                variants, variant, &VariantEvidence::variant);
            return found == variants.end() ? nullptr : &found->evidence;
        };

        const auto& production = *find(DiagnosticVariant::production);
        const auto& summary = production.summary();
        const auto* no_cull = find(DiagnosticVariant::cull_disabled);
        const auto* depth_always = find(DiagnosticVariant::depth_always);
        std::vector<DiagnosticFinding> result;

        if (summary.covered_pixel_count == 0) {
            const bool cull_reveals = no_cull
                && no_cull->summary().covered_pixel_count != 0;
            const bool depth_reveals = depth_always
                && depth_always->summary().covered_pixel_count != 0;
            if (cull_reveals) {
                result.push_back({
                    .code = FindingCode::likely_culling,
                    .confidence = FindingConfidence::strong,
                    .message = "geometry appears when culling is disabled; check winding and cull mode",
                });
            }
            if (depth_reveals) {
                result.push_back({
                    .code = FindingCode::likely_depth_rejection,
                    .confidence = FindingConfidence::strong,
                    .message = "geometry appears with an always-pass depth test; check depth clear, comparison, and projection convention",
                });
            }
            if (!cull_reveals && !depth_reveals) {
                result.push_back({
                    .code = FindingCode::no_visible_geometry,
                    .confidence = FindingConfidence::hint,
                    .message = "no tested raster-state variant produced coverage; inspect draw counts, vertex input, transforms, and clipping",
                });
            }
        } else if (all_covered_pixels_near_black(production.capture())) {
            result.push_back({
                .code = FindingCode::covered_but_near_black,
                .confidence = FindingConfidence::strong,
                .message = "surface IDs prove raster coverage, but every covered color is near black; inspect material and fragment computations",
            });
        }

        if (summary.non_finite_depth_count != 0) {
            result.push_back({
                .code = FindingCode::non_finite_depth,
                .confidence = FindingConfidence::strong,
                .message = "covered pixels contain non-finite device depth",
            });
        }
        if (summary.visible_item_count < production.capture().manifest().size()) {
            result.push_back({
                .code = FindingCode::invisible_items,
                .confidence = FindingConfidence::hint,
                .message = "one or more submitted manifest items produced no visible pixels",
            });
        }
        return result;
    }

    std::vector<VariantEvidence> variants_;
    std::vector<DiagnosticFinding> findings_;
};

} // namespace vng::analysis
