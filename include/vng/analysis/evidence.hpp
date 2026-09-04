#pragma once

#include <algorithm>
#include <concepts>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <variant>
#include <vector>

#include <vng/analysis/capture.hpp>
#include <vng/analysis/request.hpp>
#include <vng/gfx/camera.hpp>

namespace vng::analysis {

enum class EvidenceValueKind {
    i32,
    u32,
    f32,
    ivec2,
    ivec3,
    ivec4,
    uvec2,
    uvec3,
    uvec4,
    vec2,
    vec3,
    vec4,
    rgba8,
};

using EvidenceValue = std::variant<
    i32,
    u32,
    f32,
    IVec2,
    IVec3,
    IVec4,
    UVec2,
    UVec3,
    UVec4,
    Vec2,
    Vec3,
    Vec4,
    Rgba8>;

template<class T>
concept EvidencePixel = std::same_as<std::remove_cv_t<T>, i32>
    || std::same_as<std::remove_cv_t<T>, u32>
    || std::same_as<std::remove_cv_t<T>, f32>
    || std::same_as<std::remove_cv_t<T>, IVec2>
    || std::same_as<std::remove_cv_t<T>, IVec3>
    || std::same_as<std::remove_cv_t<T>, IVec4>
    || std::same_as<std::remove_cv_t<T>, UVec2>
    || std::same_as<std::remove_cv_t<T>, UVec3>
    || std::same_as<std::remove_cv_t<T>, UVec4>
    || std::same_as<std::remove_cv_t<T>, Vec2>
    || std::same_as<std::remove_cv_t<T>, Vec3>
    || std::same_as<std::remove_cv_t<T>, Vec4>
    || std::same_as<std::remove_cv_t<T>, Rgba8>;

using EvidenceImage = std::variant<
    Image<i32>,
    Image<u32>,
    Image<f32>,
    Image<IVec2>,
    Image<IVec3>,
    Image<IVec4>,
    Image<UVec2>,
    Image<UVec3>,
    Image<UVec4>,
    Image<Vec2>,
    Image<Vec3>,
    Image<Vec4>,
    Image<Rgba8>>;

class EvidenceChannel final {
public:
    template<EvidencePixel T>
    EvidenceChannel(std::string name, Image<T> image)
        : name_(std::move(name)), image_(std::move(image))
    {}

    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    [[nodiscard]] Extent2D extent() const noexcept
    {
        return std::visit(
            [](const auto& image) { return image.extent(); }, image_);
    }

    [[nodiscard]] EvidenceValueKind value_kind() const noexcept
    {
        return std::visit([]<class T>(const Image<T>&) {
            if constexpr (std::same_as<T, i32>) {
                return EvidenceValueKind::i32;
            } else if constexpr (std::same_as<T, u32>) {
                return EvidenceValueKind::u32;
            } else if constexpr (std::same_as<T, f32>) {
                return EvidenceValueKind::f32;
            } else if constexpr (std::same_as<T, IVec2>) {
                return EvidenceValueKind::ivec2;
            } else if constexpr (std::same_as<T, IVec3>) {
                return EvidenceValueKind::ivec3;
            } else if constexpr (std::same_as<T, IVec4>) {
                return EvidenceValueKind::ivec4;
            } else if constexpr (std::same_as<T, UVec2>) {
                return EvidenceValueKind::uvec2;
            } else if constexpr (std::same_as<T, UVec3>) {
                return EvidenceValueKind::uvec3;
            } else if constexpr (std::same_as<T, UVec4>) {
                return EvidenceValueKind::uvec4;
            } else if constexpr (std::same_as<T, Vec2>) {
                return EvidenceValueKind::vec2;
            } else if constexpr (std::same_as<T, Vec3>) {
                return EvidenceValueKind::vec3;
            } else if constexpr (std::same_as<T, Vec4>) {
                return EvidenceValueKind::vec4;
            } else {
                static_assert(std::same_as<T, Rgba8>);
                return EvidenceValueKind::rgba8;
            }
        }, image_);
    }

    // The C++ logical pixel type is part of the evidence contract. The
    // coarser value_kind() is useful for serialization, but is deliberately
    // not used to satisfy a typed shader-observation request.
    [[nodiscard]] std::type_index value_type() const noexcept
    {
        return std::visit([]<class T>(const Image<T>&) {
            return std::type_index{typeid(T)};
        }, image_);
    }

    [[nodiscard]] EvidenceValue at(Pixel pixel) const
    {
        return std::visit([pixel](const auto& image) -> EvidenceValue {
            return image.at(pixel);
        }, image_);
    }

    template<EvidencePixel T>
    [[nodiscard]] const Image<T>* get_if() const noexcept
    {
        return std::get_if<Image<T>>(&image_);
    }

    [[nodiscard]] const EvidenceImage& image() const noexcept { return image_; }

private:
    std::string name_;
    EvidenceImage image_;
};

struct MetadataEntry final {
    std::string key;
    std::string value;

    friend bool operator==(const MetadataEntry&, const MetadataEntry&) = default;
};

// IR and interface dumps are deliberately stored as backend-neutral text.
// Generated GLSL, SPIR-V, driver names, and native handles belong in optional
// backend artifacts rather than this portable frame description.
struct ShaderEvidence final {
    std::string program_name;
    std::string vertex_ir;
    std::string fragment_ir;
    std::string interface_description;

    friend bool operator==(const ShaderEvidence&, const ShaderEvidence&) = default;
};

// Equality tokens for the parts of a render invocation that must remain fixed
// across a diagnostic counterfactual. They are opaque so every renderer can
// choose an appropriate deterministic representation (for example a stable
// serialization or a collision-resistant digest) without leaking its backend
// objects into the analysis API.
//
// renderer_fingerprint describes the renderer definition, shader program, and
// persistent state, but excludes the one raster-state variable intentionally
// changed by a DiagnosticVariant. target_fingerprint describes the logical
// target contract, not a transient native image handle.
struct RenderInvocationIdentity final {
    std::string workload_fingerprint;
    std::string view_fingerprint;
    std::string renderer_fingerprint;
    std::string target_fingerprint;

    [[nodiscard]] bool complete() const noexcept
    {
        return !workload_fingerprint.empty()
            && !view_fingerprint.empty()
            && !renderer_fingerprint.empty()
            && !target_fingerprint.empty();
    }

    friend bool operator==(const RenderInvocationIdentity&,
                           const RenderInvocationIdentity&) = default;
};

struct FrameEvidenceMetadata final {
    std::string label;
    std::string renderer;
    std::optional<RenderInvocationIdentity> invocation;
    std::optional<gfx::CameraSnapshot> camera;
    std::optional<ShaderEvidence> shader;
    std::vector<MetadataEntry> properties;

    friend bool operator==(const FrameEvidenceMetadata&,
                           const FrameEvidenceMetadata&) = default;
};

// Optional native/backend diagnostics travel with the evidence without
// entering its portable metadata model. Names are safe relative paths such as
// "shader/vertex.glsl"; export places them below an artifacts directory.
struct BackendArtifact final {
    std::string name;
    std::string media_type;
    std::string text;

    friend bool operator==(const BackendArtifact&,
                           const BackendArtifact&) = default;
};

struct NamedEvidenceValue final {
    std::string name;
    EvidenceValue value;
};

struct EvidenceInspection final {
    PixelInspection canonical;
    std::vector<NamedEvidenceValue> additional;

    [[nodiscard]] const EvidenceValue* find(
        std::string_view name) const noexcept
    {
        const auto found = std::ranges::find(
            additional, name, &NamedEvidenceValue::name);
        return found == additional.end() ? nullptr : &found->value;
    }

    template<gfx::SemanticType Semantic>
        requires EvidencePixel<gfx::semantic_value_t<Semantic>>
    [[nodiscard]] const gfx::semantic_value_t<Semantic>* observation(
        Semantic semantic = {}) const
    {
        const auto* value = find(observation_channel_name(semantic));
        return value == nullptr
            ? nullptr
            : std::get_if<gfx::semantic_value_t<Semantic>>(value);
    }
};

enum class EvidenceDiagnosticCode {
    malformed_probe_request,
    probe_out_of_bounds,
    empty_channel_name,
    reserved_channel_name,
    duplicate_channel_name,
    mismatched_channel_extent,
    missing_requested_observation,
    mismatched_observation_type,
    mismatched_camera_extent,
    empty_metadata_key,
    duplicate_metadata_key,
    empty_artifact_name,
    invalid_artifact_name,
    duplicate_artifact_name,
    empty_artifact_media_type,
};

struct EvidenceDiagnostic final {
    EvidenceDiagnosticCode code{};
    std::string message;
    std::optional<std::string> name;
    std::optional<Extent2D> expected_extent;
    std::optional<Extent2D> actual_extent;
    std::optional<Pixel> pixel;

    friend bool operator==(const EvidenceDiagnostic&,
                           const EvidenceDiagnostic&) = default;
};

class FrameEvidence final {
public:
    [[nodiscard]] static std::expected<FrameEvidence, EvidenceDiagnostic> create(
        AnalysisCapture capture,
        CaptureRequest request = CaptureRequest::standard(),
        FrameEvidenceMetadata metadata = {},
        std::vector<EvidenceChannel> additional_channels = {},
        std::vector<BackendArtifact> backend_artifacts = {})
    {
        if (request.scope == CaptureScope::pixel
            && !request.probe_pixel) {
            return std::unexpected(EvidenceDiagnostic{
                .code = EvidenceDiagnosticCode::malformed_probe_request,
                .message = "pixel-scoped capture request requires a probe pixel",
                .name = std::nullopt,
                .expected_extent = std::nullopt,
                .actual_extent = std::nullopt,
                .pixel = std::nullopt,
            });
        }
        if (request.scope == CaptureScope::full_frame
            && request.probe_pixel) {
            return std::unexpected(EvidenceDiagnostic{
                .code = EvidenceDiagnosticCode::malformed_probe_request,
                .message = "full-frame capture request cannot contain a probe pixel",
                .name = std::nullopt,
                .expected_extent = std::nullopt,
                .actual_extent = std::nullopt,
                .pixel = request.probe_pixel,
            });
        }
        if (request.probe_pixel
            && !capture.color().contains(*request.probe_pixel)) {
            return std::unexpected(EvidenceDiagnostic{
                .code = EvidenceDiagnosticCode::probe_out_of_bounds,
                .message = "capture probe pixel is outside the evidence extent",
                .name = std::nullopt,
                .expected_extent = capture.extent(),
                .actual_extent = std::nullopt,
                .pixel = request.probe_pixel,
            });
        }
        if (metadata.camera
            && metadata.camera->extent != capture.extent()) {
            return std::unexpected(EvidenceDiagnostic{
                .code = EvidenceDiagnosticCode::mismatched_camera_extent,
                .message = "camera snapshot extent does not match the captured evidence",
                .name = std::nullopt,
                .expected_extent = capture.extent(),
                .actual_extent = metadata.camera->extent,
                .pixel = std::nullopt,
            });
        }

        std::ranges::sort(
            additional_channels, {}, &EvidenceChannel::name);
        for (std::size_t index = 0; index < additional_channels.size(); ++index) {
            const auto& channel = additional_channels[index];
            if (channel.name().empty()) {
                return std::unexpected(EvidenceDiagnostic{
                    .code = EvidenceDiagnosticCode::empty_channel_name,
                    .message = "additional evidence channel requires a nonempty name",
                    .name = channel.name(),
                    .expected_extent = std::nullopt,
                    .actual_extent = std::nullopt,
                    .pixel = std::nullopt,
                });
            }
            if (reserved_channel_name(channel.name())) {
                return std::unexpected(EvidenceDiagnostic{
                    .code = EvidenceDiagnosticCode::reserved_channel_name,
                    .message = "additional evidence channel uses a canonical channel name",
                    .name = channel.name(),
                    .expected_extent = std::nullopt,
                    .actual_extent = std::nullopt,
                    .pixel = std::nullopt,
                });
            }
            if (index != 0
                && additional_channels[index - 1].name() == channel.name()) {
                return std::unexpected(EvidenceDiagnostic{
                    .code = EvidenceDiagnosticCode::duplicate_channel_name,
                    .message = "additional evidence channel names must be unique",
                    .name = channel.name(),
                    .expected_extent = std::nullopt,
                    .actual_extent = std::nullopt,
                    .pixel = std::nullopt,
                });
            }
            if (channel.extent() != capture.extent()) {
                return std::unexpected(EvidenceDiagnostic{
                    .code = EvidenceDiagnosticCode::mismatched_channel_extent,
                    .message = "additional evidence channel extent does not match the capture",
                    .name = channel.name(),
                    .expected_extent = capture.extent(),
                    .actual_extent = channel.extent(),
                    .pixel = std::nullopt,
                });
            }
        }

        // Observation requests are promises made by FrameEvidence itself, not
        // best-effort hints to a backend. Channel-name uniqueness above gives
        // the "exactly once" half of the invariant; this loop enforces
        // presence at the canonical name and the exact requested C++ type.
        for (const auto& observation : request.observations()) {
            const auto canonical_name = observation.channel_name();
            const auto found = std::ranges::lower_bound(
                additional_channels,
                std::string_view{canonical_name},
                {},
                &EvidenceChannel::name);
            if (found == additional_channels.end()
                || found->name() != canonical_name) {
                return std::unexpected(EvidenceDiagnostic{
                    .code = EvidenceDiagnosticCode::missing_requested_observation,
                    .message = "requested shader observation is absent from frame evidence",
                    .name = canonical_name,
                    .expected_extent = std::nullopt,
                    .actual_extent = std::nullopt,
                    .pixel = std::nullopt,
                });
            }
            if (found->value_type() != observation.value_type) {
                return std::unexpected(EvidenceDiagnostic{
                    .code = EvidenceDiagnosticCode::mismatched_observation_type,
                    .message = "shader observation channel has the wrong C++ logical type",
                    .name = canonical_name,
                    .expected_extent = std::nullopt,
                    .actual_extent = std::nullopt,
                    .pixel = std::nullopt,
                });
            }
        }

        std::ranges::sort(
            metadata.properties, {}, &MetadataEntry::key);
        for (std::size_t index = 0; index < metadata.properties.size(); ++index) {
            const auto& property = metadata.properties[index];
            if (property.key.empty()) {
                return std::unexpected(EvidenceDiagnostic{
                    .code = EvidenceDiagnosticCode::empty_metadata_key,
                    .message = "frame evidence metadata key cannot be empty",
                    .name = std::nullopt,
                    .expected_extent = std::nullopt,
                    .actual_extent = std::nullopt,
                    .pixel = std::nullopt,
                });
            }
            if (index != 0
                && metadata.properties[index - 1].key == property.key) {
                return std::unexpected(EvidenceDiagnostic{
                    .code = EvidenceDiagnosticCode::duplicate_metadata_key,
                    .message = "frame evidence metadata keys must be unique",
                    .name = property.key,
                    .expected_extent = std::nullopt,
                    .actual_extent = std::nullopt,
                    .pixel = std::nullopt,
                });
            }
        }

        std::ranges::sort(
            backend_artifacts, {}, &BackendArtifact::name);
        for (std::size_t index = 0; index < backend_artifacts.size(); ++index) {
            const auto& artifact = backend_artifacts[index];
            if (artifact.name.empty()) {
                return std::unexpected(EvidenceDiagnostic{
                    .code = EvidenceDiagnosticCode::empty_artifact_name,
                    .message = "backend text artifact requires a nonempty name",
                    .name = artifact.name,
                    .expected_extent = std::nullopt,
                    .actual_extent = std::nullopt,
                    .pixel = std::nullopt,
                });
            }
            if (!valid_artifact_name(artifact.name)) {
                return std::unexpected(EvidenceDiagnostic{
                    .code = EvidenceDiagnosticCode::invalid_artifact_name,
                    .message = "backend text artifact name is not a safe relative path",
                    .name = artifact.name,
                    .expected_extent = std::nullopt,
                    .actual_extent = std::nullopt,
                    .pixel = std::nullopt,
                });
            }
            if (index != 0
                && backend_artifacts[index - 1].name == artifact.name) {
                return std::unexpected(EvidenceDiagnostic{
                    .code = EvidenceDiagnosticCode::duplicate_artifact_name,
                    .message = "backend text artifact names must be unique",
                    .name = artifact.name,
                    .expected_extent = std::nullopt,
                    .actual_extent = std::nullopt,
                    .pixel = std::nullopt,
                });
            }
            if (artifact.media_type.empty()) {
                return std::unexpected(EvidenceDiagnostic{
                    .code = EvidenceDiagnosticCode::empty_artifact_media_type,
                    .message = "backend text artifact requires a media type",
                    .name = artifact.name,
                    .expected_extent = std::nullopt,
                    .actual_extent = std::nullopt,
                    .pixel = std::nullopt,
                });
            }
        }

        return FrameEvidence{
            std::move(capture),
            std::move(request),
            std::move(metadata),
            std::move(additional_channels),
            std::move(backend_artifacts),
        };
    }

    [[nodiscard]] const AnalysisCapture& capture() const noexcept
    {
        return capture_;
    }
    [[nodiscard]] const CaptureRequest& request() const noexcept
    {
        return request_;
    }
    [[nodiscard]] const FrameEvidenceMetadata& metadata() const noexcept
    {
        return metadata_;
    }
    [[nodiscard]] const CaptureSummary& summary() const noexcept
    {
        return capture_.summary();
    }
    [[nodiscard]] std::span<const EvidenceChannel> additional_channels() const noexcept
    {
        return additional_channels_;
    }
    [[nodiscard]] std::span<const BackendArtifact> backend_artifacts() const noexcept
    {
        return backend_artifacts_;
    }

    [[nodiscard]] const EvidenceChannel* channel(
        std::string_view name) const noexcept
    {
        const auto found = std::ranges::lower_bound(
            additional_channels_, name, {}, &EvidenceChannel::name);
        return found != additional_channels_.end() && found->name() == name
            ? &*found : nullptr;
    }

    template<gfx::SemanticType Semantic>
        requires EvidencePixel<gfx::semantic_value_t<Semantic>>
    [[nodiscard]] const Image<gfx::semantic_value_t<Semantic>>* observation(
        Semantic semantic = {}) const
    {
        const auto* found = channel(observation_channel_name(semantic));
        return found == nullptr
            ? nullptr
            : found->template get_if<gfx::semantic_value_t<Semantic>>();
    }

    [[nodiscard]] const BackendArtifact* backend_artifact(
        std::string_view name) const noexcept
    {
        const auto found = std::ranges::lower_bound(
            backend_artifacts_, name, {}, &BackendArtifact::name);
        return found != backend_artifacts_.end() && found->name == name
            ? &*found : nullptr;
    }

    [[nodiscard]] std::expected<EvidenceInspection, CaptureDiagnostic> inspect(
        Pixel pixel) const
    {
        auto canonical = capture_.inspect(pixel);
        if (!canonical) {
            return std::unexpected(std::move(canonical.error()));
        }

        EvidenceInspection result;
        result.canonical = std::move(*canonical);
        result.additional.reserve(additional_channels_.size());
        for (const auto& channel : additional_channels_) {
            result.additional.push_back(NamedEvidenceValue{
                .name = channel.name(),
                .value = channel.at(pixel),
            });
        }
        return result;
    }

private:
    FrameEvidence(
        AnalysisCapture capture,
        CaptureRequest request,
        FrameEvidenceMetadata metadata,
        std::vector<EvidenceChannel> additional_channels,
        std::vector<BackendArtifact> backend_artifacts) noexcept
        : capture_(std::move(capture)),
          request_(std::move(request)),
          metadata_(std::move(metadata)),
          additional_channels_(std::move(additional_channels)),
          backend_artifacts_(std::move(backend_artifacts))
    {}

    [[nodiscard]] static bool valid_artifact_name(
        std::string_view name) noexcept
    {
        if (name.empty() || name.front() == '/' || name.back() == '/') {
            return false;
        }
        std::size_t segment_begin = 0;
        for (std::size_t index = 0; index <= name.size(); ++index) {
            if (index != name.size() && name[index] != '/') {
                const auto character = static_cast<unsigned char>(name[index]);
                const auto alpha_numeric =
                    (character >= 'a' && character <= 'z')
                    || (character >= 'A' && character <= 'Z')
                    || (character >= '0' && character <= '9');
                if (!alpha_numeric && character != '-' && character != '_'
                    && character != '.') {
                    return false;
                }
                continue;
            }

            const auto segment = name.substr(
                segment_begin, index - segment_begin);
            if (segment.empty() || segment == "." || segment == "..") {
                return false;
            }
            segment_begin = index + 1;
        }
        return true;
    }

    [[nodiscard]] static bool reserved_channel_name(
        std::string_view name) noexcept
    {
        return name == "color"
            || name == "device_depth"
            || name == "surface_key";
    }

    AnalysisCapture capture_;
    CaptureRequest request_;
    FrameEvidenceMetadata metadata_;
    std::vector<EvidenceChannel> additional_channels_;
    std::vector<BackendArtifact> backend_artifacts_;
};

} // namespace vng::analysis
