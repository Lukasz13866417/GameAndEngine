#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <vng/analysis/evidence.hpp>
#include <vng/analysis/visualization.hpp>

namespace vng::analysis {

enum class ExportDiagnosticCode {
    destination_is_not_directory,
    destination_not_empty,
    create_directory_failed,
    open_file_failed,
    write_file_failed,
};

struct ExportDiagnostic final {
    ExportDiagnosticCode code{};
    std::string message;
    std::filesystem::path path;

    friend bool operator==(const ExportDiagnostic&,
                           const ExportDiagnostic&) = default;
};

struct ExportOptions final {
    // Exact canonical values are written together as pixels.csv. This is more
    // useful for programs and agents than reverse-engineering display images.
    bool write_pixel_table{true};
    bool write_additional_channel_table{true};
    // Requested diagnostic visualizations are always honored. This flag can
    // additionally force all three for a standard/probe request.
    bool write_all_visualizations{};

    friend constexpr bool operator==(const ExportOptions&,
                                     const ExportOptions&) = default;
};

struct ExportResult final {
    std::filesystem::path directory;
    std::vector<std::filesystem::path> files;
};

namespace export_detail {

template<std::integral T>
void append_integer(std::string& destination, T value)
{
    char buffer[std::numeric_limits<T>::digits10 + 4]{};
    const auto result = std::to_chars(
        std::begin(buffer), std::end(buffer), value);
    destination.append(buffer, result.ptr);
}

inline void append_float(std::string& destination, f32 value)
{
    if (std::isnan(value)) {
        destination += "nan";
        return;
    }
    if (value == std::numeric_limits<f32>::infinity()) {
        destination += "inf";
        return;
    }
    if (value == -std::numeric_limits<f32>::infinity()) {
        destination += "-inf";
        return;
    }

    char buffer[64]{};
    const auto result = std::to_chars(
        std::begin(buffer),
        std::end(buffer),
        value,
        std::chars_format::general,
        std::numeric_limits<f32>::max_digits10);
    destination.append(buffer, result.ptr);
}

inline void append_json_string(std::string& destination, std::string_view value)
{
    constexpr char hexadecimal[] = "0123456789abcdef";
    destination.push_back('"');
    for (const auto raw : value) {
        const auto character = static_cast<unsigned char>(raw);
        switch (character) {
        case '"': destination += "\\\""; break;
        case '\\': destination += "\\\\"; break;
        case '\b': destination += "\\b"; break;
        case '\f': destination += "\\f"; break;
        case '\n': destination += "\\n"; break;
        case '\r': destination += "\\r"; break;
        case '\t': destination += "\\t"; break;
        default:
            if (character < 0x20U) {
                destination += "\\u00";
                destination.push_back(hexadecimal[character >> 4U]);
                destination.push_back(hexadecimal[character & 0x0FU]);
            } else {
                destination.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    destination.push_back('"');
}

inline void append_json_float(std::string& destination, f32 value)
{
    if (std::isfinite(value)) {
        append_float(destination, value);
    } else {
        // JSON has no non-finite number syntax. Preserve the fact without
        // producing an invalid document.
        append_json_string(
            destination,
            std::isnan(value) ? "nan" : (value > 0.0F ? "inf" : "-inf"));
    }
}

inline std::string_view preset_name(CapturePreset preset) noexcept
{
    switch (preset) {
    case CapturePreset::standard: return "standard";
    case CapturePreset::diagnostic: return "diagnostic";
    case CapturePreset::probe: return "probe";
    }
    return "unknown";
}

inline std::string_view value_kind_name(EvidenceValueKind kind) noexcept
{
    switch (kind) {
    case EvidenceValueKind::i32: return "i32";
    case EvidenceValueKind::u32: return "u32";
    case EvidenceValueKind::f32: return "f32";
    case EvidenceValueKind::ivec2: return "ivec2";
    case EvidenceValueKind::ivec3: return "ivec3";
    case EvidenceValueKind::ivec4: return "ivec4";
    case EvidenceValueKind::uvec2: return "uvec2";
    case EvidenceValueKind::uvec3: return "uvec3";
    case EvidenceValueKind::uvec4: return "uvec4";
    case EvidenceValueKind::vec2: return "vec2";
    case EvidenceValueKind::vec3: return "vec3";
    case EvidenceValueKind::vec4: return "vec4";
    case EvidenceValueKind::rgba8: return "rgba8";
    }
    return "unknown";
}

template<class Vector>
void append_vector(std::string& destination, const Vector& value)
{
    destination.push_back('[');
    for (std::size_t index = 0; index < Vector::component_count; ++index) {
        if (index != 0) {
            destination.push_back(',');
        }
        using Component = typename Vector::value_type;
        if constexpr (std::same_as<Component, f32>) {
            append_float(destination, value[index]);
        } else {
            append_integer(destination, value[index]);
        }
    }
    destination.push_back(']');
}

inline void append_evidence_value(
    std::string& destination,
    const EvidenceValue& value)
{
    std::visit([&destination](const auto& typed) {
        using T = std::remove_cvref_t<decltype(typed)>;
        if constexpr (std::same_as<T, i32> || std::same_as<T, u32>) {
            append_integer(destination, typed);
        } else if constexpr (std::same_as<T, f32>) {
            append_float(destination, typed);
        } else if constexpr (std::same_as<T, Rgba8>) {
            destination.push_back('[');
            append_integer(destination, typed.r);
            destination.push_back(',');
            append_integer(destination, typed.g);
            destination.push_back(',');
            append_integer(destination, typed.b);
            destination.push_back(',');
            append_integer(destination, typed.a);
            destination.push_back(']');
        } else {
            append_vector(destination, typed);
        }
    }, value);
}

inline void append_vec3_json(std::string& destination, Vec3 value)
{
    append_vector(destination, value);
}

inline void append_mat4_json(std::string& destination, const Mat4& value)
{
    destination.push_back('[');
    for (std::size_t column = 0; column < 4; ++column) {
        if (column != 0) {
            destination.push_back(',');
        }
        append_vector(destination, value[column]);
    }
    destination.push_back(']');
}

inline void append_optional_id(
    std::string& destination,
    const std::optional<EntityId>& value)
{
    if (value) {
        append_integer(destination, value->value);
    } else {
        destination += "null";
    }
}

inline void append_optional_id(
    std::string& destination,
    const std::optional<MaterialId>& value)
{
    if (value) {
        append_integer(destination, value->value);
    } else {
        destination += "null";
    }
}

inline void append_depth_summary(
    std::string& destination,
    const std::optional<DepthSummary>& value)
{
    if (!value) {
        destination += "null";
        return;
    }
    destination += "{\"minimum\":";
    append_json_float(destination, value->minimum);
    destination += ",\"maximum\":";
    append_json_float(destination, value->maximum);
    destination += ",\"minimum_pixel\":[";
    append_integer(destination, value->minimum_pixel.x);
    destination.push_back(',');
    append_integer(destination, value->minimum_pixel.y);
    destination += "],\"maximum_pixel\":[";
    append_integer(destination, value->maximum_pixel.x);
    destination.push_back(',');
    append_integer(destination, value->maximum_pixel.y);
    destination += "],\"finite_pixel_count\":";
    append_integer(destination, value->finite_pixel_count);
    destination.push_back('}');
}

inline void append_bounds(
    std::string& destination,
    const std::optional<PixelBounds>& value)
{
    if (!value) {
        destination += "null";
        return;
    }
    destination += "{\"minimum\":[";
    append_integer(destination, value->minimum.x);
    destination.push_back(',');
    append_integer(destination, value->minimum.y);
    destination += "],\"maximum\":[";
    append_integer(destination, value->maximum.x);
    destination.push_back(',');
    append_integer(destination, value->maximum.y);
    destination += "]}";
}

inline std::string make_json(const FrameEvidence& evidence)
{
    const auto& capture = evidence.capture();
    const auto& summary = evidence.summary();
    const auto& metadata = evidence.metadata();
    const auto& request = evidence.request();

    std::string result;
    result.reserve(4096);
    result += "{\n  \"format\":\"vng-frame-evidence\",\n  \"version\":1,\n";
    result += "  \"frame\":";
    append_integer(result, capture.frame().value);
    result += ",\n  \"extent\":[";
    append_integer(result, capture.extent().width);
    result.push_back(',');
    append_integer(result, capture.extent().height);
    result += "],\n  \"request\":{\"preset\":";
    append_json_string(result, preset_name(request.preset));
    result += ",\"scope\":";
    append_json_string(
        result,
        request.scope == CaptureScope::pixel ? "pixel" : "full_frame");
    result += ",\"probe_pixel\":";
    if (request.probe_pixel) {
        result.push_back('[');
        append_integer(result, request.probe_pixel->x);
        result.push_back(',');
        append_integer(result, request.probe_pixel->y);
        result.push_back(']');
    } else {
        result += "null";
    }
    result += ",\"channels\":[";
    bool first_channel = true;
    const auto append_channel = [&](std::string_view name, bool requested) {
        if (!requested) {
            return;
        }
        if (!first_channel) {
            result.push_back(',');
        }
        append_json_string(result, name);
        first_channel = false;
    };
    append_channel("color", request.color);
    append_channel("device_depth", request.device_depth);
    append_channel("surface_key", request.surface_key);
    result += "],\"visualizations\":[";
    bool first_visualization = true;
    const auto append_visualization = [&](std::string_view name, bool requested) {
        if (!requested) {
            return;
        }
        if (!first_visualization) {
            result.push_back(',');
        }
        append_json_string(result, name);
        first_visualization = false;
    };
    append_visualization("coverage", request.coverage_visualization);
    append_visualization("item", request.item_visualization);
    append_visualization("primitive", request.primitive_visualization);
    result += "],\"observations\":[";
    for (std::size_t index = 0; index < request.observations().size(); ++index) {
        if (index != 0) {
            result.push_back(',');
        }
        const auto& observation = request.observations()[index];
        result += "{\"semantic\":";
        append_json_string(result, observation.semantic_name);
        result += ",\"value_type\":";
        append_json_string(result, observation.value_name);
        result.push_back('}');
    }
    result.push_back(']');
    result += "},\n  \"metadata\":{\"label\":";
    append_json_string(result, metadata.label);
    result += ",\"renderer\":";
    append_json_string(result, metadata.renderer);
    result += ",\"invocation\":";
    if (metadata.invocation) {
        result += "{\"workload_fingerprint\":";
        append_json_string(
            result, metadata.invocation->workload_fingerprint);
        result += ",\"view_fingerprint\":";
        append_json_string(result, metadata.invocation->view_fingerprint);
        result += ",\"renderer_fingerprint\":";
        append_json_string(
            result, metadata.invocation->renderer_fingerprint);
        result += ",\"target_fingerprint\":";
        append_json_string(result, metadata.invocation->target_fingerprint);
        result.push_back('}');
    } else {
        result += "null";
    }
    result += ",\"properties\":[";
    for (std::size_t index = 0; index < metadata.properties.size(); ++index) {
        if (index != 0) {
            result.push_back(',');
        }
        result += "{\"key\":";
        append_json_string(result, metadata.properties[index].key);
        result += ",\"value\":";
        append_json_string(result, metadata.properties[index].value);
        result.push_back('}');
    }
    result += "],\"camera\":";
    if (metadata.camera) {
        const auto& camera = *metadata.camera;
        result += "{\"position\":";
        append_vec3_json(result, camera.position);
        result += ",\"right\":";
        append_vec3_json(result, camera.right);
        result += ",\"up\":";
        append_vec3_json(result, camera.up);
        result += ",\"forward\":";
        append_vec3_json(result, camera.forward);
        result += ",\"aspect_ratio\":";
        append_json_float(result, camera.aspect_ratio);
        result += ",\"view\":";
        append_mat4_json(result, camera.view);
        result += ",\"projection\":";
        append_mat4_json(result, camera.projection);
        result += ",\"view_projection\":";
        append_mat4_json(result, camera.view_projection);
        result.push_back('}');
    } else {
        result += "null";
    }
    result += ",\"shader\":";
    if (metadata.shader) {
        result += "{\"program_name\":";
        append_json_string(result, metadata.shader->program_name);
        result += ",\"vertex_ir\":";
        append_json_string(result, metadata.shader->vertex_ir);
        result += ",\"fragment_ir\":";
        append_json_string(result, metadata.shader->fragment_ir);
        result += ",\"interface\":";
        append_json_string(result, metadata.shader->interface_description);
        result.push_back('}');
    } else {
        result += "null";
    }
    result += "},\n  \"summary\":{\"pixel_count\":";
    append_integer(result, summary.pixel_count);
    result += ",\"covered_pixel_count\":";
    append_integer(result, summary.covered_pixel_count);
    result += ",\"background_pixel_count\":";
    append_integer(result, summary.background_pixel_count);
    result += ",\"visible_item_count\":";
    append_integer(result, summary.visible_item_count);
    result += ",\"visible_primitive_count\":";
    append_integer(result, summary.visible_primitive_count);
    result += ",\"non_finite_depth_count\":";
    append_integer(result, summary.non_finite_depth_count);
    result += ",\"covered_bounds\":";
    append_bounds(result, summary.covered_bounds);
    result += ",\"device_depth\":";
    append_depth_summary(result, summary.device_depth);
    result += ",\"items\":[";
    for (std::size_t index = 0; index < summary.items.size(); ++index) {
        if (index != 0) {
            result.push_back(',');
        }
        const auto& item = summary.items[index];
        result += "{\"item\":";
        append_integer(result, item.item.value);
        result += ",\"covered_pixel_count\":";
        append_integer(result, item.covered_pixel_count);
        result += ",\"visible_primitive_count\":";
        append_integer(result, item.visible_primitive_count);
        result += ",\"non_finite_depth_count\":";
        append_integer(result, item.non_finite_depth_count);
        result += ",\"bounds\":";
        append_bounds(result, item.bounds);
        result += ",\"device_depth\":";
        append_depth_summary(result, item.device_depth);
        result.push_back('}');
    }
    result += "]},\n  \"manifest\":[";
    for (std::size_t index = 0; index < capture.manifest().items().size(); ++index) {
        if (index != 0) {
            result.push_back(',');
        }
        const auto& item = capture.manifest().items()[index];
        result += "{\"item\":";
        append_integer(result, item.id.value);
        result += ",\"entity\":";
        append_optional_id(result, item.provenance.entity);
        result += ",\"mesh\":";
        append_integer(result, item.provenance.mesh.value);
        result += ",\"mesh_revision\":";
        append_integer(result, item.provenance.mesh_revision.value);
        result += ",\"material\":";
        append_optional_id(result, item.provenance.material);
        result += ",\"object_to_world\":";
        append_mat4_json(result, item.provenance.object_to_world);
        result += ",\"primitives\":[";
        const auto primitives = item.primitives->sources();
        for (std::size_t primitive = 0; primitive < primitives.size(); ++primitive) {
            if (primitive != 0) {
                result.push_back(',');
            }
            result += "{\"id\":";
            append_integer(result, primitive);
            result += ",\"face\":";
            append_integer(result, primitives[primitive].face);
            result += ",\"vertices\":[";
            append_integer(result, primitives[primitive].vertices[0]);
            result.push_back(',');
            append_integer(result, primitives[primitive].vertices[1]);
            result.push_back(',');
            append_integer(result, primitives[primitive].vertices[2]);
            result += "]}";
        }
        result += "]}";
    }
    result += "],\n  \"additional_channels\":[";
    for (std::size_t index = 0; index < evidence.additional_channels().size(); ++index) {
        if (index != 0) {
            result.push_back(',');
        }
        const auto& channel = evidence.additional_channels()[index];
        result += "{\"name\":";
        append_json_string(result, channel.name());
        result += ",\"type\":";
        append_json_string(result, value_kind_name(channel.value_kind()));
        result.push_back('}');
    }
    result += "],\n  \"backend_artifacts\":[";
    for (std::size_t index = 0; index < evidence.backend_artifacts().size(); ++index) {
        if (index != 0) {
            result.push_back(',');
        }
        const auto& artifact = evidence.backend_artifacts()[index];
        result += "{\"name\":";
        append_json_string(result, artifact.name);
        result += ",\"media_type\":";
        append_json_string(result, artifact.media_type);
        result += ",\"path\":";
        append_json_string(result, std::string{"artifacts/"} + artifact.name);
        result += ",\"byte_count\":";
        append_integer(result, artifact.text.size());
        result.push_back('}');
    }
    result += "]\n}\n";
    return result;
}

inline std::string make_summary_text(const FrameEvidence& evidence)
{
    const auto& summary = evidence.summary();
    std::string result;
    result += "VNG frame evidence 1\nframe: ";
    append_integer(result, evidence.capture().frame().value);
    result += "\nextent: ";
    append_integer(result, summary.extent.width);
    result.push_back('x');
    append_integer(result, summary.extent.height);
    result += "\npreset: ";
    result += preset_name(evidence.request().preset);
    result += "\npixels: ";
    append_integer(result, summary.pixel_count);
    result += "\ncovered pixels: ";
    append_integer(result, summary.covered_pixel_count);
    result += "\nbackground pixels: ";
    append_integer(result, summary.background_pixel_count);
    result += "\nvisible items: ";
    append_integer(result, summary.visible_item_count);
    result += "\nvisible primitives: ";
    append_integer(result, summary.visible_primitive_count);
    result += "\nnon-finite covered depths: ";
    append_integer(result, summary.non_finite_depth_count);
    result.push_back('\n');
    if (summary.device_depth) {
        result += "covered depth range: ";
        append_float(result, summary.device_depth->minimum);
        result += " .. ";
        append_float(result, summary.device_depth->maximum);
        result.push_back('\n');
    } else {
        result += "covered depth range: none\n";
    }
    for (const auto& item : summary.items) {
        result += "item ";
        append_integer(result, item.item.value);
        result += ": pixels=";
        append_integer(result, item.covered_pixel_count);
        result += ", primitives=";
        append_integer(result, item.visible_primitive_count);
        result.push_back('\n');
    }
    return result;
}

inline std::string make_pixel_table(const AnalysisCapture& capture)
{
    std::string result{
        "x,y,r,g,b,a,device_depth,item,primitive\n"};
    for (u32 y = 0; y < capture.extent().height; ++y) {
        for (u32 x = 0; x < capture.extent().width; ++x) {
            const Pixel pixel{x, y};
            const auto color = capture.color().at(pixel);
            const auto depth = capture.device_depth().at(pixel);
            const auto key = capture.surface_keys().at(pixel);
            append_integer(result, x);
            result.push_back(',');
            append_integer(result, y);
            result.push_back(',');
            append_integer(result, color.r);
            result.push_back(',');
            append_integer(result, color.g);
            result.push_back(',');
            append_integer(result, color.b);
            result.push_back(',');
            append_integer(result, color.a);
            result.push_back(',');
            append_float(result, depth);
            result.push_back(',');
            append_integer(result, key.item.value);
            result.push_back(',');
            append_integer(result, key.primitive.value);
            result.push_back('\n');
        }
    }
    return result;
}

inline std::string make_channel_table(const FrameEvidence& evidence)
{
    std::string result{"channel\ttype\tx\ty\tvalue\n"};
    for (const auto& channel : evidence.additional_channels()) {
        for (u32 y = 0; y < channel.extent().height; ++y) {
            for (u32 x = 0; x < channel.extent().width; ++x) {
                // JSON quoting is also an unambiguous single-field escaping
                // scheme for the channel name in this tab-separated file.
                append_json_string(result, channel.name());
                result.push_back('\t');
                result += value_kind_name(channel.value_kind());
                result.push_back('\t');
                append_integer(result, x);
                result.push_back('\t');
                append_integer(result, y);
                result.push_back('\t');
                append_evidence_value(result, channel.at({x, y}));
                result.push_back('\n');
            }
        }
    }
    return result;
}

inline std::string ppm_header(Extent2D extent, std::string_view magic)
{
    std::string result{magic};
    result.push_back('\n');
    append_integer(result, extent.width);
    result.push_back(' ');
    append_integer(result, extent.height);
    result += "\n255\n";
    return result;
}

template<class Writer>
[[nodiscard]] std::optional<ExportDiagnostic> write_file(
    const std::filesystem::path& directory,
    const std::filesystem::path& filename,
    Writer&& writer)
{
    const auto path = directory / filename;
    std::error_code error;
    const auto parent = path.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent, error)) {
        if (error || !std::filesystem::create_directories(parent, error)) {
            return ExportDiagnostic{
                .code = ExportDiagnosticCode::create_directory_failed,
                .message = "could not create evidence export subdirectory",
                .path = parent,
            };
        }
    } else if (error || !std::filesystem::is_directory(parent, error)) {
        return ExportDiagnostic{
            .code = ExportDiagnosticCode::create_directory_failed,
            .message = "evidence export parent is not a directory",
            .path = parent,
        };
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return ExportDiagnostic{
            .code = ExportDiagnosticCode::open_file_failed,
            .message = "could not open evidence export file",
            .path = path,
        };
    }
    writer(stream);
    stream.flush();
    if (!stream) {
        return ExportDiagnostic{
            .code = ExportDiagnosticCode::write_file_failed,
            .message = "could not completely write evidence export file",
            .path = path,
        };
    }
    return std::nullopt;
}

inline void write_bytes(std::ofstream& stream, std::string_view bytes)
{
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

inline void write_ppm(
    std::ofstream& stream,
    const AnalysisCapture::ColorImage& image)
{
    write_bytes(stream, ppm_header(image.extent(), "P6"));
    for (const auto pixel : image.pixels()) {
        const char bytes[]{
            static_cast<char>(pixel.r),
            static_cast<char>(pixel.g),
            static_cast<char>(pixel.b),
        };
        stream.write(bytes, 3);
    }
}

inline void write_depth_pgm(
    std::ofstream& stream,
    const AnalysisCapture::DepthImage& image)
{
    write_bytes(stream, ppm_header(image.extent(), "P5"));
    for (const auto depth : image.pixels()) {
        const auto normalized = std::isfinite(depth)
            ? std::clamp(depth, 0.0F, 1.0F) : 0.0F;
        const auto byte = static_cast<u8>(std::lround(normalized * 255.0F));
        const char encoded = static_cast<char>(byte);
        stream.write(&encoded, 1);
    }
}

} // namespace export_detail

// Export never removes or silently coexists with old files. Requiring a new or
// empty destination prevents stale artifacts from being mistaken for part of
// the current deterministic evidence bundle.
[[nodiscard]] inline std::expected<ExportResult, ExportDiagnostic>
export_evidence_directory(
    const FrameEvidence& evidence,
    const std::filesystem::path& directory,
    ExportOptions options = {})
{
    std::error_code error;
    if (std::filesystem::exists(directory, error)) {
        if (error || !std::filesystem::is_directory(directory, error)) {
            return std::unexpected(ExportDiagnostic{
                .code = ExportDiagnosticCode::destination_is_not_directory,
                .message = "evidence export destination is not a directory",
                .path = directory,
            });
        }
        if (error || !std::filesystem::is_empty(directory, error)) {
            return std::unexpected(ExportDiagnostic{
                .code = ExportDiagnosticCode::destination_not_empty,
                .message = "evidence export destination must be empty",
                .path = directory,
            });
        }
    } else {
        if (error || !std::filesystem::create_directories(directory, error)) {
            return std::unexpected(ExportDiagnostic{
                .code = ExportDiagnosticCode::create_directory_failed,
                .message = "could not create evidence export directory",
                .path = directory,
            });
        }
    }

    ExportResult result;
    result.directory = directory;
    auto write = [&]<class Writer>(
                     const std::filesystem::path& filename,
                     Writer&& writer)
        -> std::optional<ExportDiagnostic> {
        auto failure = export_detail::write_file(
            directory, filename, std::forward<Writer>(writer));
        if (!failure) {
            result.files.emplace_back(filename);
        }
        return failure;
    };

    const auto json = export_detail::make_json(evidence);
    if (auto failure = write("evidence.json", [&](std::ofstream& stream) {
            export_detail::write_bytes(stream, json);
        })) {
        return std::unexpected(std::move(*failure));
    }
    const auto summary = export_detail::make_summary_text(evidence);
    if (auto failure = write("summary.txt", [&](std::ofstream& stream) {
            export_detail::write_bytes(stream, summary);
        })) {
        return std::unexpected(std::move(*failure));
    }
    if (auto failure = write("color.ppm", [&](std::ofstream& stream) {
            export_detail::write_ppm(stream, evidence.capture().color());
        })) {
        return std::unexpected(std::move(*failure));
    }
    if (auto failure = write("depth.pgm", [&](std::ofstream& stream) {
            export_detail::write_depth_pgm(
                stream, evidence.capture().device_depth());
        })) {
        return std::unexpected(std::move(*failure));
    }
    if (options.write_pixel_table) {
        const auto pixels = export_detail::make_pixel_table(evidence.capture());
        if (auto failure = write("pixels.csv", [&](std::ofstream& stream) {
                export_detail::write_bytes(stream, pixels);
            })) {
            return std::unexpected(std::move(*failure));
        }
    }
    if (options.write_additional_channel_table
        && !evidence.additional_channels().empty()) {
        const auto channels = export_detail::make_channel_table(evidence);
        if (auto failure = write("channels.tsv", [&](std::ofstream& stream) {
                export_detail::write_bytes(stream, channels);
            })) {
            return std::unexpected(std::move(*failure));
        }
    }

    for (const auto& artifact : evidence.backend_artifacts()) {
        const auto relative = std::filesystem::path{"artifacts"}
            / std::filesystem::path{artifact.name};
        if (auto failure = write(relative, [&](std::ofstream& stream) {
                export_detail::write_bytes(stream, artifact.text);
            })) {
            return std::unexpected(std::move(*failure));
        }
    }

    const auto write_coverage = options.write_all_visualizations
        || evidence.request().requests(DerivedVisualization::coverage);
    const auto write_items = options.write_all_visualizations
        || evidence.request().requests(DerivedVisualization::item);
    const auto write_primitives = options.write_all_visualizations
        || evidence.request().requests(DerivedVisualization::primitive);
    if (write_coverage) {
        const auto image = make_coverage_visualization(evidence.capture());
        if (auto failure = write("coverage.ppm", [&](std::ofstream& stream) {
                export_detail::write_ppm(stream, image);
            })) {
            return std::unexpected(std::move(*failure));
        }
    }
    if (write_items) {
        const auto image = make_item_visualization(evidence.capture());
        if (auto failure = write("items.ppm", [&](std::ofstream& stream) {
                export_detail::write_ppm(stream, image);
            })) {
            return std::unexpected(std::move(*failure));
        }
    }
    if (write_primitives) {
        const auto image = make_primitive_visualization(evidence.capture());
        if (auto failure = write("primitives.ppm", [&](std::ofstream& stream) {
                export_detail::write_ppm(stream, image);
            })) {
            return std::unexpected(std::move(*failure));
        }
    }

    std::ranges::sort(result.files);
    return result;
}

} // namespace vng::analysis
