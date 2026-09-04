#include <vng/render/opengl_program_runtime.hpp>

#include <algorithm>
#include <bit>
#include <limits>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vng/opengl/glsl_source.hpp>

namespace vng::render {
namespace {

class StableFingerprint final {
public:
    explicit StableFingerprint(std::string_view domain) noexcept
    {
        append(domain);
    }

    void append(std::string_view value) noexcept
    {
        append(static_cast<u64>(value.size()));
        for (const auto character : value) {
            mix(static_cast<u8>(character));
        }
    }

    void append(u64 value) noexcept
    {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            mix(static_cast<u8>(value >> shift));
        }
    }

    void append(bool value) noexcept
    {
        mix(value ? u8{1} : u8{0});
    }

    void append(f32 value) noexcept
    {
        append(static_cast<u64>(std::bit_cast<u32>(value)));
    }

    [[nodiscard]] std::string finish() const
    {
        constexpr char digits[] = "0123456789abcdef";
        const auto word = [&](u64 value) {
            std::string result(16, '0');
            for (std::size_t index = 0; index < result.size(); ++index) {
                const auto shift = static_cast<unsigned>(
                    (result.size() - index - 1) * 4);
                result[index] = digits[(value >> shift) & 0xFU];
            }
            return result;
        };
        return word(first_) + word(second_);
    }

private:
    void mix(u8 value) noexcept
    {
        first_ ^= value;
        first_ *= 0x100000001B3ULL;

        second_ += static_cast<u64>(value) + 0x9E3779B97F4A7C15ULL;
        second_ ^= second_ >> 29U;
        second_ *= 0xBF58476D1CE4E5B9ULL;
        second_ ^= second_ >> 31U;
    }

    u64 first_{0xCBF29CE484222325ULL};
    u64 second_{0x6A09E667F3BCC909ULL};
};

void append(StableFingerprint& fingerprint, Vec3 value) noexcept
{
    fingerprint.append(value.x);
    fingerprint.append(value.y);
    fingerprint.append(value.z);
}

void append(StableFingerprint& fingerprint, const Mat4& value) noexcept
{
    for (const auto& column : value.columns) {
        for (std::size_t row = 0; row < 4; ++row) {
            fingerprint.append(column[row]);
        }
    }
}

[[nodiscard]] opengl::Diagnostic emission_diagnostic(
    shader::Diagnostic diagnostic)
{
    std::string message = "GLSL emission failed: " + diagnostic.message;
    for (const auto& note : diagnostic.notes) {
        message += "\n" + note;
    }
    return opengl::Diagnostic{
        .code = opengl::ErrorCode::invalid_argument,
        .message = std::move(message),
        .generated_source = std::move(diagnostic.generated_source),
    };
}

[[nodiscard]] std::expected<opengl::Program, opengl::Diagnostic> compile_program(
    const opengl::Device& device,
    const glsl::ProgramSource& source)
{
    auto vertex = opengl::Shader::compile(
        device, opengl::from_glsl(source.vertex));
    if (!vertex) {
        return std::unexpected(std::move(vertex.error()));
    }
    auto fragment = opengl::Shader::compile(
        device, opengl::from_glsl(source.fragment));
    if (!fragment) {
        return std::unexpected(std::move(fragment.error()));
    }
    return opengl::Program::link_graphics(device, *vertex, *fragment);
}

template<class Pixel>
void flip_rows(std::vector<Pixel>& pixels, analysis::Extent2D extent)
{
    const auto width = static_cast<std::size_t>(extent.width);
    for (std::size_t top = 0, bottom = static_cast<std::size_t>(extent.height) - 1;
         top < bottom;
         ++top, --bottom) {
        const auto top_begin = pixels.begin()
            + static_cast<std::ptrdiff_t>(top * width);
        const auto bottom_begin = pixels.begin()
            + static_cast<std::ptrdiff_t>(bottom * width);
        std::swap_ranges(top_begin, top_begin + static_cast<std::ptrdiff_t>(width), bottom_begin);
    }
}

[[nodiscard]] opengl::Diagnostic capture_diagnostic(
    const analysis::CaptureDiagnostic& diagnostic)
{
    return opengl::Diagnostic{
        .code = opengl::ErrorCode::operation_failed,
        .message = "OpenGL analysis rendering produced an invalid capture: "
            + diagnostic.message,
    };
}

[[nodiscard]] opengl::Diagnostic evidence_diagnostic(
    const analysis::EvidenceDiagnostic& diagnostic)
{
    return opengl::Diagnostic{
        .code = opengl::ErrorCode::operation_failed,
        .message = "OpenGL analysis rendering produced invalid frame evidence: "
            + diagnostic.message,
    };
}

[[nodiscard]] std::string_view compare_name(DepthCompare compare) noexcept
{
    switch (compare) {
    case DepthCompare::never: return "never";
    case DepthCompare::less: return "less";
    case DepthCompare::less_equal: return "less_equal";
    case DepthCompare::equal: return "equal";
    case DepthCompare::greater_equal: return "greater_equal";
    case DepthCompare::greater: return "greater";
    case DepthCompare::not_equal: return "not_equal";
    case DepthCompare::always: return "always";
    }
    return "unknown";
}

[[nodiscard]] std::string_view cull_name(CullMode cull) noexcept
{
    switch (cull) {
    case CullMode::none: return "none";
    case CullMode::front: return "front";
    case CullMode::back: return "back";
    }
    return "unknown";
}

[[nodiscard]] std::string_view front_face_name(FrontFace front_face) noexcept
{
    switch (front_face) {
    case FrontFace::clockwise: return "clockwise";
    case FrontFace::counter_clockwise: return "counter_clockwise";
    }
    return "unknown";
}

[[nodiscard]] std::string_view encoding_name(ColorEncoding encoding) noexcept
{
    switch (encoding) {
    case ColorEncoding::linear: return "linear";
    case ColorEncoding::srgb: return "srgb";
    }
    return "unknown";
}

[[nodiscard]] std::string source_map_text(const glsl::StageSource& source)
{
    std::ostringstream output;
    output << "generated_line\tir_node\tsource\n";
    for (const auto& mapping : source.source_map) {
        output << mapping.generated_line << '\t' << mapping.operation.value
               << '\t' << mapping.origin.file << ':' << mapping.origin.line
               << ':' << mapping.origin.column << '\n';
    }
    return output.str();
}

[[nodiscard]] std::string debug_messages_text(
    std::span<const opengl::DebugMessage> messages)
{
    std::ostringstream output;
    output << "id\tseverity\tsource\ttype\tmessage\n";
    for (const auto& message : messages) {
        output << message.id << '\t' << static_cast<int>(message.severity)
               << '\t' << message.source << '\t'
               << message.type << '\t' << message.message << '\n';
    }
    return output.str();
}

[[nodiscard]] std::string lifecycle_diagnostics_text(
    std::span<const opengl::Diagnostic> diagnostics)
{
    std::ostringstream output;
    for (const auto& diagnostic : diagnostics) {
        output << "code: " << static_cast<int>(diagnostic.code) << '\n'
               << "message: " << diagnostic.message << '\n';
        if (!diagnostic.driver_log.empty()) {
            output << "driver log:\n" << diagnostic.driver_log << '\n';
        }
        if (!diagnostic.generated_source.empty()) {
            output << "generated source:\n"
                   << diagnostic.generated_source << '\n';
        }
        output << '\n';
    }
    return output.str();
}

template<class Raw, class Result, class Convert>
[[nodiscard]] std::vector<Result> convert_pixels(
    const std::vector<Raw>& source,
    Convert convert)
{
    std::vector<Result> result;
    result.reserve(source.size());
    for (const auto& pixel : source) {
        result.push_back(convert(pixel));
    }
    return result;
}

} // namespace

std::expected<OpenGLProgramRuntime, opengl::Diagnostic> OpenGLProgramRuntime::create(
    const opengl::Device& device,
    shader::GraphicsProgram program,
    GraphicsPipelineDesc pipeline)
{
    auto normal_pipeline = compile_pipeline(
        device, program, pipeline);
    if (!normal_pipeline) {
        return std::unexpected(std::move(normal_pipeline.error()));
    }
    const auto* normal_source = normal_pipeline->generated_source();
    if (normal_source == nullptr) {
        return std::unexpected(opengl::Diagnostic{
            .code = opengl::ErrorCode::operation_failed,
            .message = "the OpenGL pipeline compiler returned no generated source",
        });
    }
    return OpenGLProgramRuntime{
        std::move(program),
        std::move(*normal_pipeline),
    };
}

std::expected<gfx::CameraSnapshot, opengl::Diagnostic>
OpenGLProgramRuntime::snapshot_camera(
    const gfx::Camera& camera,
    Extent2D extent)
{
    auto snapshot = camera.snapshot(extent);
    if (!snapshot) {
        return std::unexpected(opengl::Diagnostic{
            .code = opengl::ErrorCode::invalid_argument,
            .message = "invalid camera: " + snapshot.error().message,
        });
    }
    return std::move(*snapshot);
}

float OpenGLProgramRuntime::default_analysis_clear_depth(
    const GraphicsPipelineDesc& pipeline) noexcept
{
    if (pipeline.depth.test
        && (pipeline.depth.compare == DepthCompare::greater
            || pipeline.depth.compare == DepthCompare::greater_equal)) {
        return 0.0F;
    }
    return 1.0F;
}

opengl::Diagnostic OpenGLProgramRuntime::sweep_diagnostic(
    const analysis::SweepDiagnostic& diagnostic)
{
    return opengl::Diagnostic{
        .code = opengl::ErrorCode::operation_failed,
        .message = "could not assemble diagnostic sweep: "
            + diagnostic.message,
    };
}

std::expected<void, opengl::Diagnostic>
OpenGLProgramRuntime::bind_camera_parameters(
    const opengl::Program& program,
    const glsl::ProgramSource& source,
    const gfx::CameraSnapshot* camera)
{
    for (const auto& parameter : source.parameters) {
        switch (parameter.kind) {
        case shader::ParameterKind::camera_view_projection:
            if (camera == nullptr) {
                return std::unexpected(opengl::Diagnostic{
                    .code = opengl::ErrorCode::invalid_argument,
                    .message = "shader reads camera parameters; pass Camera and the render-target extent to render()",
                });
            }
            if (auto uploaded = program.set_uniform_mat4(
                    parameter.location,
                    camera->view_projection);
                !uploaded) {
                return uploaded;
            }
            break;
        }
    }
    return {};
}

std::expected<void, opengl::Diagnostic>
OpenGLProgramRuntime::ensure_analysis_program(const opengl::Device& device)
{
    if (!normal_pipeline_.belongs_to(device)) {
        return std::unexpected(opengl::Diagnostic{
            .code = opengl::ErrorCode::incompatible_device,
            .message = "OpenGLProgramRuntime belongs to a different device/context",
        });
    }
    if (analysis_pipeline_) {
        if (!analysis_pipeline_->belongs_to(device)) {
            return std::unexpected(opengl::Diagnostic{
                .code = opengl::ErrorCode::incompatible_device,
                .message = "OpenGLProgramRuntime analysis pipeline belongs to a different device/context",
            });
        }
        return {};
    }

    const auto& normal_description = normal_pipeline_.description();
    if (normal_description.depth.test && !normal_description.depth.write) {
        return std::unexpected(opengl::Diagnostic{
            .code = opengl::ErrorCode::invalid_argument,
            .message = "OpenGLProgramRuntime analysis currently requires depth-writing opaque pipeline state",
        });
    }

    auto source = glsl::emit(program_, glsl::AnalysisEmission{});
    if (!source) {
        return std::unexpected(emission_diagnostic(std::move(source.error())));
    }
    if (!source->analysis) {
        return std::unexpected(opengl::Diagnostic{
            .code = opengl::ErrorCode::operation_failed,
            .message = "analysis emission returned no runtime metadata",
        });
    }

    const auto primary_color = std::ranges::find_if(
        source->fragment.interface.outputs,
        [](const glsl::InterfaceVariable& output) {
            return output.builtin == shader::Builtin::color
                && output.builtin_index == 0
                && output.location == 0
                && output.glsl_type == "vec4";
        });
    if (primary_color == source->fragment.interface.outputs.end()) {
        return std::unexpected(opengl::Diagnostic{
            .code = opengl::ErrorCode::invalid_argument,
            .message = "OpenGLProgramRuntime analysis requires a vec4 shader::Color<0> output for its color image",
        });
    }

    auto program = compile_program(device, *source);
    if (!program) {
        return std::unexpected(std::move(program.error()));
    }
    auto analysis_description = normal_description;
    // OpenGL only updates a depth attachment while depth testing is enabled.
    // ALWAYS reproduces opaque draw-order visibility for a technique whose
    // production state disables depth testing while still capturing the
    // winning fragment's depth.
    if (!analysis_description.depth.test) {
        analysis_description.depth.test = true;
        analysis_description.depth.write = true;
        analysis_description.depth.compare = DepthCompare::always;
    }
    // Analysis color has a backend-neutral linear RGBA8 contract.
    analysis_description.output_encoding = ColorEncoding::linear;
    auto analysis_pipeline = opengl::GraphicsPipeline::realize_emitted(
        device,
        std::move(*program),
        *source,
        analysis_description);
    if (!analysis_pipeline) {
        return std::unexpected(std::move(analysis_pipeline.error()));
    }
    analysis_pipeline_.emplace(std::move(*analysis_pipeline));
    return {};
}

std::expected<OpenGLProgramRuntime::ObservationProduct*, opengl::Diagnostic>
OpenGLProgramRuntime::ensure_observation_product(
    const opengl::Device& device,
    const analysis::ObservationRequest& requested)
{
    const auto existing = std::ranges::find(
        observation_products_,
        requested.semantic_type,
        &ObservationProduct::semantic_type);
    if (existing != observation_products_.end()) {
        if (!existing->pipeline.belongs_to(device)) {
            return std::unexpected(opengl::Diagnostic{
                .code = opengl::ErrorCode::incompatible_device,
                .message = "cached shader observation belongs to a different device/context",
            });
        }
        return &*existing;
    }

    auto source = glsl::emit(
        program_,
        glsl::ObservationEmission{
            .stage = shader::StageKind::fragment,
            .semantic_type = requested.semantic_type,
            .value_type = requested.value_type,
            .semantic_name = requested.semantic_name,
        });
    if (!source) {
        return std::unexpected(emission_diagnostic(std::move(source.error())));
    }
    if (!source->observation) {
        return std::unexpected(opengl::Diagnostic{
            .code = opengl::ErrorCode::operation_failed,
            .message = "observation emission returned no runtime metadata",
        });
    }

    auto program = compile_program(device, *source);
    if (!program) {
        return std::unexpected(std::move(program.error()));
    }
    auto description = normal_pipeline_.description();
    if (!description.depth.test) {
        description.depth.test = true;
        description.depth.write = true;
        description.depth.compare = DepthCompare::always;
    }
    description.output_encoding = ColorEncoding::linear;
    auto pipeline = opengl::GraphicsPipeline::realize_emitted(
        device,
        std::move(*program),
        *source,
        description);
    if (!pipeline) {
        return std::unexpected(std::move(pipeline.error()));
    }

    observation_products_.push_back(ObservationProduct{
        .semantic_type = requested.semantic_type,
        .pipeline = std::move(*pipeline),
        .target = std::nullopt,
    });
    return &observation_products_.back();
}

std::expected<void, opengl::Diagnostic>
OpenGLProgramRuntime::ensure_observation_target(
    const opengl::Device& device,
    ObservationProduct& product,
    analysis::Extent2D extent)
{
    const auto& source = product.source();
    if (!source.observation) {
        return std::unexpected(opengl::Diagnostic{
            .code = opengl::ErrorCode::operation_failed,
            .message = "shader observation product has no emission metadata",
        });
    }
    if (extent.empty()) {
        return std::unexpected(opengl::Diagnostic{
            .code = opengl::ErrorCode::invalid_argument,
            .message = "shader observation capture requires a non-empty extent",
        });
    }

    opengl::ImageFormat format{};
    switch (source.observation->scalar_kind) {
    case shader::ScalarKind::f32:
        format = opengl::ImageFormat::rgba32f;
        break;
    case shader::ScalarKind::i32:
        format = opengl::ImageFormat::rgba32i;
        break;
    case shader::ScalarKind::u32:
        format = opengl::ImageFormat::rgba32ui;
        break;
    case shader::ScalarKind::boolean:
        return std::unexpected(opengl::Diagnostic{
            .code = opengl::ErrorCode::invalid_argument,
            .message = "boolean shader observations are not framebuffer-readable",
        });
    }
    const auto attachment = source.observation->attachment_location;
    if (product.target
        && product.target->extent == extent
        && product.target->attachment == attachment
        && product.target->format == format) {
        if (!product.target->value.belongs_to(device)) {
            return std::unexpected(opengl::Diagnostic{
                .code = opengl::ErrorCode::incompatible_device,
                .message = "cached shader observation target belongs to a different device/context",
            });
        }
        return {};
    }

    auto value = opengl::Image2D::create(
        device, extent.width, extent.height, format);
    if (!value) {
        return std::unexpected(std::move(value.error()));
    }
    auto depth = opengl::Image2D::create(
        device, extent.width, extent.height, opengl::ImageFormat::depth32f);
    if (!depth) {
        return std::unexpected(std::move(depth.error()));
    }
    auto framebuffer = opengl::Framebuffer::create(device);
    if (!framebuffer) {
        return std::unexpected(std::move(framebuffer.error()));
    }
    if (auto attached = framebuffer->attach_color(attachment, *value);
        !attached) {
        return std::unexpected(std::move(attached.error()));
    }
    if (auto attached = framebuffer->attach_depth(*depth); !attached) {
        return std::unexpected(std::move(attached.error()));
    }
    if (auto complete = framebuffer->check_complete(); !complete) {
        return std::unexpected(std::move(complete.error()));
    }

    product.target.emplace(ObservationTarget{
        .extent = extent,
        .attachment = attachment,
        .format = format,
        .value = std::move(*value),
        .depth = std::move(*depth),
        .framebuffer = std::move(*framebuffer),
    });
    return {};
}

std::expected<void, opengl::Diagnostic> OpenGLProgramRuntime::begin_observation(
    const opengl::Device& device,
    ObservationProduct& product,
    analysis::Extent2D extent,
    float clear_depth)
{
    if (!product.target || product.target->extent != extent) {
        return std::unexpected(opengl::Diagnostic{
            .code = opengl::ErrorCode::operation_failed,
            .message = "shader observation target was not prepared for this extent",
        });
    }
    auto& target = *product.target;
    if (auto bound = target.framebuffer.bind(); !bound) {
        return bound;
    }
    if (auto viewport = device.viewport(
            0,
            0,
            static_cast<std::int32_t>(extent.width),
            static_cast<std::int32_t>(extent.height));
        !viewport) {
        return viewport;
    }
    if (auto pipeline = product.pipeline.bind(device); !pipeline) {
        return pipeline;
    }
    if (auto blend = device.set_blend_enabled(target.attachment, false);
        !blend) {
        return blend;
    }
    if (auto mask = device.set_color_write_mask(
            target.attachment, {true, true, true, true});
        !mask) {
        return mask;
    }

    switch (target.format) {
    case opengl::ImageFormat::rgba32f:
        if (auto clear = target.value.clear_rgba32f({0.0F, 0.0F, 0.0F, 0.0F});
            !clear) {
            return clear;
        }
        break;
    case opengl::ImageFormat::rgba32i:
        if (auto clear = target.value.clear_rgba32i({0, 0, 0, 0}); !clear) {
            return clear;
        }
        break;
    case opengl::ImageFormat::rgba32ui:
        if (auto clear = target.value.clear_rgba32ui({0U, 0U, 0U, 0U});
            !clear) {
            return clear;
        }
        break;
    default:
        return std::unexpected(opengl::Diagnostic{
            .code = opengl::ErrorCode::operation_failed,
            .message = "shader observation target has an unsupported image format",
        });
    }
    return target.depth.clear_depth32f(clear_depth);
}

std::expected<analysis::EvidenceChannel, opengl::Diagnostic>
OpenGLProgramRuntime::finish_observation(ObservationProduct& product)
{
    const auto& source = product.source();
    if (!product.target || !source.observation) {
        return std::unexpected(opengl::Diagnostic{
            .code = opengl::ErrorCode::operation_failed,
            .message = "shader observation readback has no prepared target",
        });
    }
    auto& target = *product.target;
    const auto extent = target.extent;
    const auto attachment = target.attachment;
    const auto components = source.observation->component_count;
    const auto name = analysis::ObservationRequest{
        .semantic_type = source.observation->semantic_type,
        .value_type = source.observation->value_type,
        .semantic_name = source.observation->semantic_name,
        .value_name = {},
    }.channel_name();

    if (target.format == opengl::ImageFormat::rgba32f) {
        auto raw = target.framebuffer.read_rgba32f(
            attachment, 0, 0, extent.width, extent.height);
        if (!raw) return std::unexpected(std::move(raw.error()));
        flip_rows(*raw, extent);
        if (components == 1) {
            auto values = convert_pixels<opengl::Rgba32fPixel, f32>(
                *raw, [](auto value) { return value.r; });
            return analysis::EvidenceChannel{
                name, analysis::Image<f32>{extent, std::move(values)}};
        }
        if (components == 2) {
            auto values = convert_pixels<opengl::Rgba32fPixel, Vec2>(
                *raw, [](auto value) { return Vec2{value.r, value.g}; });
            return analysis::EvidenceChannel{
                name, analysis::Image<Vec2>{extent, std::move(values)}};
        }
        if (components == 3) {
            auto values = convert_pixels<opengl::Rgba32fPixel, Vec3>(
                *raw, [](auto value) { return Vec3{value.r, value.g, value.b}; });
            return analysis::EvidenceChannel{
                name, analysis::Image<Vec3>{extent, std::move(values)}};
        }
        if (components == 4) {
            auto values = convert_pixels<opengl::Rgba32fPixel, Vec4>(
                *raw, [](auto value) {
                    return Vec4{value.r, value.g, value.b, value.a};
                });
            return analysis::EvidenceChannel{
                name, analysis::Image<Vec4>{extent, std::move(values)}};
        }
    } else if (target.format == opengl::ImageFormat::rgba32i) {
        auto raw = target.framebuffer.read_rgba32i(
            attachment, 0, 0, extent.width, extent.height);
        if (!raw) return std::unexpected(std::move(raw.error()));
        flip_rows(*raw, extent);
        if (components == 1) {
            auto values = convert_pixels<opengl::Rgba32iPixel, i32>(
                *raw, [](auto value) { return value.r; });
            return analysis::EvidenceChannel{
                name, analysis::Image<i32>{extent, std::move(values)}};
        }
        if (components == 2) {
            auto values = convert_pixels<opengl::Rgba32iPixel, IVec2>(
                *raw, [](auto value) { return IVec2{value.r, value.g}; });
            return analysis::EvidenceChannel{
                name, analysis::Image<IVec2>{extent, std::move(values)}};
        }
        if (components == 3) {
            auto values = convert_pixels<opengl::Rgba32iPixel, IVec3>(
                *raw, [](auto value) { return IVec3{value.r, value.g, value.b}; });
            return analysis::EvidenceChannel{
                name, analysis::Image<IVec3>{extent, std::move(values)}};
        }
        if (components == 4) {
            auto values = convert_pixels<opengl::Rgba32iPixel, IVec4>(
                *raw, [](auto value) {
                    return IVec4{value.r, value.g, value.b, value.a};
                });
            return analysis::EvidenceChannel{
                name, analysis::Image<IVec4>{extent, std::move(values)}};
        }
    } else if (target.format == opengl::ImageFormat::rgba32ui) {
        auto raw = target.framebuffer.read_rgba32ui(
            attachment, 0, 0, extent.width, extent.height);
        if (!raw) return std::unexpected(std::move(raw.error()));
        flip_rows(*raw, extent);
        if (components == 1) {
            auto values = convert_pixels<opengl::Rgba32uiPixel, u32>(
                *raw, [](auto value) { return value.r; });
            return analysis::EvidenceChannel{
                name, analysis::Image<u32>{extent, std::move(values)}};
        }
        if (components == 2) {
            auto values = convert_pixels<opengl::Rgba32uiPixel, UVec2>(
                *raw, [](auto value) { return UVec2{value.r, value.g}; });
            return analysis::EvidenceChannel{
                name, analysis::Image<UVec2>{extent, std::move(values)}};
        }
        if (components == 3) {
            auto values = convert_pixels<opengl::Rgba32uiPixel, UVec3>(
                *raw, [](auto value) { return UVec3{value.r, value.g, value.b}; });
            return analysis::EvidenceChannel{
                name, analysis::Image<UVec3>{extent, std::move(values)}};
        }
        if (components == 4) {
            auto values = convert_pixels<opengl::Rgba32uiPixel, UVec4>(
                *raw, [](auto value) {
                    return UVec4{value.r, value.g, value.b, value.a};
                });
            return analysis::EvidenceChannel{
                name, analysis::Image<UVec4>{extent, std::move(values)}};
        }
    }

    return std::unexpected(opengl::Diagnostic{
        .code = opengl::ErrorCode::operation_failed,
        .message = "shader observation readback has an unsupported logical type",
    });
}

std::expected<analysis::FrameEvidence, opengl::Diagnostic>
OpenGLProgramRuntime::make_frame_evidence(
    const opengl::Device& device,
    analysis::AnalysisCapture capture,
    analysis::CaptureRequest request,
    const gfx::CameraSnapshot* camera,
    std::string label,
    analysis::RenderInvocationIdentity invocation,
    std::vector<analysis::EvidenceChannel> observations,
    const opengl::DiagnosticCursor& diagnostic_cursor,
    RasterOverride raster_override) const
{
    const auto& production_pipeline = normal_pipeline_.description();
    auto effective_raster = analysis_pipeline_->description();
    std::string raster_name;
    switch (raster_override) {
    case RasterOverride::production:
        raster_name = "production";
        break;
    case RasterOverride::cull_disabled:
        raster_name = "cull_disabled";
        effective_raster.cull = CullMode::none;
        break;
    case RasterOverride::depth_always:
        raster_name = "depth_always";
        effective_raster.depth = {
            .test = true,
            .write = true,
            .compare = DepthCompare::always,
        };
        break;
    }

    const auto& production_source = normal_source();
    auto program_name = production_source.vertex.debug_name;
    if (!production_source.fragment.debug_name.empty()) {
        if (!program_name.empty()) {
            program_name += " + ";
        }
        program_name += production_source.fragment.debug_name;
    }
    if (program_name.empty()) {
        program_name = "graphics_program";
    }

    analysis::FrameEvidenceMetadata metadata{
        .label = std::move(label),
        .renderer = "vng::render::OpenGLProgramRuntime",
        .invocation = std::move(invocation),
        .camera = std::nullopt,
        .shader = analysis::ShaderEvidence{
            .program_name = std::move(program_name),
            .vertex_ir = program_.vertex().dump_ir(),
            .fragment_ir = program_.fragment().dump_ir(),
            .interface_description = program_.dump_interface(),
        },
        .properties = {
            {"backend", "OpenGL"},
            {"backend.opengl.version",
             std::to_string(device.major_version()) + "."
                 + std::to_string(device.minor_version())},
            {"pipeline.production.color_encoding", std::string{encoding_name(
                 production_pipeline.output_encoding)}},
            {"pipeline.production.cull", std::string{cull_name(
                 production_pipeline.cull)}},
            {"pipeline.production.depth.compare", std::string{compare_name(
                 production_pipeline.depth.compare)}},
            {"pipeline.production.depth.test",
             production_pipeline.depth.test ? "true" : "false"},
            {"pipeline.production.depth.write",
             production_pipeline.depth.write ? "true" : "false"},
            {"pipeline.production.front_face", std::string{front_face_name(
                 production_pipeline.front_face)}},
            {"raster.effective.color_encoding", std::string{encoding_name(
                 effective_raster.output_encoding)}},
            {"raster.effective.cull", std::string{cull_name(
                 effective_raster.cull)}},
            {"raster.effective.depth.compare", std::string{compare_name(
                 effective_raster.depth.compare)}},
            {"raster.effective.depth.test",
             effective_raster.depth.test ? "true" : "false"},
            {"raster.effective.depth.write",
             effective_raster.depth.write ? "true" : "false"},
            {"raster.effective.front_face", std::string{front_face_name(
                 effective_raster.front_face)}},
            {"raster.variant", raster_name},
        },
    };
    if (camera != nullptr) {
        metadata.camera = *camera;
    }

    std::vector<analysis::BackendArtifact> artifacts;
    const auto add_stage_artifacts = [&artifacts](
        std::string_view prefix,
        const glsl::ProgramSource& source) {
        artifacts.push_back({
            .name = std::string{prefix} + "/vertex.glsl",
            .media_type = "text/x-glsl",
            .text = source.vertex.source,
        });
        artifacts.push_back({
            .name = std::string{prefix} + "/fragment.glsl",
            .media_type = "text/x-glsl",
            .text = source.fragment.source,
        });
        artifacts.push_back({
            .name = std::string{prefix} + "/vertex-source-map.tsv",
            .media_type = "text/tab-separated-values",
            .text = source_map_text(source.vertex),
        });
        artifacts.push_back({
            .name = std::string{prefix} + "/fragment-source-map.tsv",
            .media_type = "text/tab-separated-values",
            .text = source_map_text(source.fragment),
        });
    };

    add_stage_artifacts("shader/normal", production_source);
    if (const auto* source = analysis_source()) {
        add_stage_artifacts("shader/analysis", *source);
    }

    std::size_t observation_index = 0;
    for (const auto& requested : request.observations()) {
        const auto product = std::ranges::find(
            observation_products_,
            requested.semantic_type,
            &ObservationProduct::semantic_type);
        if (product == observation_products_.end()) {
            continue;
        }
        const auto prefix = "shader/observation-"
            + std::to_string(observation_index++);
        add_stage_artifacts(prefix, product->source());
        artifacts.push_back({
            .name = prefix + "/semantic.txt",
            .media_type = "text/plain",
            .text = requested.semantic_name + "\n",
        });
    }

    std::ostringstream pipeline_text;
    pipeline_text
        << "raster.variant=" << raster_name << '\n'
        << "pipeline.production.depth.test="
        << (production_pipeline.depth.test ? "true" : "false") << '\n'
        << "pipeline.production.depth.write="
        << (production_pipeline.depth.write ? "true" : "false") << '\n'
        << "pipeline.production.depth.compare="
        << compare_name(production_pipeline.depth.compare) << '\n'
        << "pipeline.production.cull="
        << cull_name(production_pipeline.cull) << '\n'
        << "pipeline.production.front_face="
        << front_face_name(production_pipeline.front_face) << '\n'
        << "pipeline.production.color_encoding="
        << encoding_name(production_pipeline.output_encoding) << '\n'
        << "raster.effective.depth.test="
        << (effective_raster.depth.test ? "true" : "false") << '\n'
        << "raster.effective.depth.write="
        << (effective_raster.depth.write ? "true" : "false") << '\n'
        << "raster.effective.depth.compare="
        << compare_name(effective_raster.depth.compare) << '\n'
        << "raster.effective.cull=" << cull_name(effective_raster.cull) << '\n'
        << "raster.effective.front_face="
        << front_face_name(effective_raster.front_face) << '\n'
        << "raster.effective.color_encoding="
        << encoding_name(effective_raster.output_encoding) << '\n';
    artifacts.push_back({
        .name = "pipeline/state.txt",
        .media_type = "text/plain",
        .text = std::move(pipeline_text).str(),
    });

    auto diagnostic_snapshot = device.diagnostics_since(diagnostic_cursor);
    if (!diagnostic_snapshot) {
        return std::unexpected(std::move(diagnostic_snapshot.error()));
    }
    if (!diagnostic_snapshot->debug_messages.empty()) {
        artifacts.push_back({
            .name = "backend/debug-messages.tsv",
            .media_type = "text/tab-separated-values",
            .text = debug_messages_text(diagnostic_snapshot->debug_messages),
        });
    }
    if (!diagnostic_snapshot->lifecycle_diagnostics.empty()) {
        artifacts.push_back({
            .name = "backend/lifecycle-diagnostics.txt",
            .media_type = "text/plain",
            .text = lifecycle_diagnostics_text(
                diagnostic_snapshot->lifecycle_diagnostics),
        });
    }

    auto evidence = analysis::FrameEvidence::create(
        std::move(capture),
        std::move(request),
        std::move(metadata),
        std::move(observations),
        std::move(artifacts));
    if (!evidence) {
        return std::unexpected(evidence_diagnostic(evidence.error()));
    }
    return std::move(*evidence);
}

analysis::RenderInvocationIdentity OpenGLProgramRuntime::make_invocation_identity(
    gfx::MeshTopologyFingerprint topology,
    const RenderView& view,
    const AnalysisOptions& options) const
{
    StableFingerprint workload{"vng.render.workload.v1"};
    workload.append(topology.low);
    workload.append(topology.high);
    workload.append(u64{1}); // one ordered draw, one instance
    workload.append(options.provenance.entity.has_value());
    workload.append(options.provenance.entity
        ? options.provenance.entity->value : u64{});
    workload.append(options.provenance.mesh.value);
    workload.append(options.provenance.mesh_revision.value);
    workload.append(options.provenance.material.has_value());
    workload.append(options.provenance.material
        ? options.provenance.material->value : u64{});
    append(workload, options.provenance.object_to_world);

    StableFingerprint view_fingerprint{"vng.render.view.v1"};
    view_fingerprint.append(static_cast<u64>(view.extent().width));
    view_fingerprint.append(static_cast<u64>(view.extent().height));
    view_fingerprint.append(view.camera().has_value());
    if (view.camera()) {
        const auto& camera = *view.camera();
        append(view_fingerprint, camera.position);
        append(view_fingerprint, camera.right);
        append(view_fingerprint, camera.up);
        append(view_fingerprint, camera.forward);
        view_fingerprint.append(camera.aspect_ratio);
        append(view_fingerprint, camera.view);
        append(view_fingerprint, camera.projection);
        append(view_fingerprint, camera.view_projection);
    }

    const auto& pipeline = normal_pipeline_.description();
    StableFingerprint renderer{"vng.render.renderer.v1"};
    renderer.append(program_.vertex().dump_ir());
    renderer.append(program_.fragment().dump_ir());
    renderer.append(program_.dump_interface());
    renderer.append(static_cast<u64>(pipeline.depth.test));
    renderer.append(static_cast<u64>(pipeline.depth.write));
    renderer.append(static_cast<u64>(pipeline.depth.compare));
    renderer.append(static_cast<u64>(pipeline.cull));
    renderer.append(static_cast<u64>(pipeline.front_face));
    renderer.append(static_cast<u64>(pipeline.output_encoding));

    StableFingerprint target{"vng.render.analysis-target.v1"};
    target.append(static_cast<u64>(view.extent().width));
    target.append(static_cast<u64>(view.extent().height));
    target.append(std::string_view{
        "rgba8-linear+depth32f+rg32ui+top-left"});
    for (const auto component : options.clear_color) {
        target.append(component);
    }
    target.append(options.clear_depth.has_value());
    target.append(options.clear_depth.value_or(
        default_analysis_clear_depth(pipeline)));

    return {
        .workload_fingerprint = workload.finish(),
        .view_fingerprint = view_fingerprint.finish(),
        .renderer_fingerprint = renderer.finish(),
        .target_fingerprint = target.finish(),
    };
}

std::expected<analysis::FrameId, opengl::Diagnostic>
OpenGLProgramRuntime::allocate_frame_id()
{
    if (next_frame_id_ == 0) {
        return std::unexpected(opengl::Diagnostic{
            .code = opengl::ErrorCode::operation_failed,
            .message = "OpenGLProgramRuntime exhausted its analysis frame-ID space",
        });
    }
    const auto result = analysis::FrameId{next_frame_id_};
    if (next_frame_id_ == std::numeric_limits<u64>::max()) {
        next_frame_id_ = 0;
    } else {
        ++next_frame_id_;
    }
    return result;
}

std::expected<void, opengl::Diagnostic> OpenGLProgramRuntime::ensure_analysis_target(
    const opengl::Device& device,
    analysis::Extent2D extent)
{
    if (auto current = device.require_current(
            "OpenGLProgramRuntime::ensure_analysis_target");
        !current) {
        return current;
    }
    const auto surface_key_attachment =
        analysis_source()->analysis->surface_key_location;
    if (analysis_target_
        && analysis_target_->extent == extent
        && analysis_target_->surface_key_attachment == surface_key_attachment) {
        if (!analysis_target_->color.belongs_to(device)) {
            return std::unexpected(opengl::Diagnostic{
                .code = opengl::ErrorCode::incompatible_device,
                .message = "OpenGLProgramRuntime analysis target belongs to a different device/context",
            });
        }
        return {};
    }
    if (extent.empty()) {
        return std::unexpected(opengl::Diagnostic{
            .code = opengl::ErrorCode::invalid_argument,
            .message = "OpenGLProgramRuntime::render_analysis requires a non-empty extent",
        });
    }

    auto color = opengl::Image2D::create(
        device, extent.width, extent.height, opengl::ImageFormat::rgba8);
    if (!color) {
        return std::unexpected(std::move(color.error()));
    }
    auto keys = opengl::Image2D::create(
        device, extent.width, extent.height, opengl::ImageFormat::rg32ui);
    if (!keys) {
        return std::unexpected(std::move(keys.error()));
    }
    auto depth = opengl::Image2D::create(
        device, extent.width, extent.height, opengl::ImageFormat::depth32f);
    if (!depth) {
        return std::unexpected(std::move(depth.error()));
    }
    auto framebuffer = opengl::Framebuffer::create(device);
    if (!framebuffer) {
        return std::unexpected(std::move(framebuffer.error()));
    }
    if (auto attached = framebuffer->attach_color(0, *color); !attached) {
        return std::unexpected(std::move(attached.error()));
    }
    if (auto attached = framebuffer->attach_color(
            surface_key_attachment, *keys);
        !attached) {
        return std::unexpected(std::move(attached.error()));
    }
    if (auto attached = framebuffer->attach_depth(*depth); !attached) {
        return std::unexpected(std::move(attached.error()));
    }
    if (auto complete = framebuffer->check_complete(); !complete) {
        return std::unexpected(std::move(complete.error()));
    }

    // All fallible work completed while the previous cache entry remained
    // intact. Replacing it is now a no-fail sequence of moves, with the old
    // resources destroyed while their context is current.
    analysis_target_.emplace(AnalysisTarget{
        .extent = extent,
        .surface_key_attachment = surface_key_attachment,
        .color = std::move(*color),
        .surface_keys = std::move(*keys),
        .depth = std::move(*depth),
        .framebuffer = std::move(*framebuffer),
    });
    return {};
}

std::expected<void, opengl::Diagnostic> OpenGLProgramRuntime::begin_analysis(
    const opengl::Device& device,
    analysis::Extent2D extent,
    analysis::FrameItemId first_item,
    const std::array<float, 4>& clear_color,
    float clear_depth,
    RasterOverride raster_override)
{
    if (auto target = ensure_analysis_target(device, extent); !target) {
        return target;
    }
    auto& target = *analysis_target_;
    if (auto bound = target.framebuffer.bind(); !bound) {
        return bound;
    }
    if (auto viewport = device.viewport(
            0,
            0,
            static_cast<std::int32_t>(extent.width),
            static_cast<std::int32_t>(extent.height));
        !viewport) {
        return viewport;
    }
    if (auto pipeline = analysis_pipeline_->bind(device); !pipeline) {
        return pipeline;
    }
    if (raster_override == RasterOverride::cull_disabled) {
        opengl::FrontFaceWinding front_face{};
        switch (analysis_pipeline_->description().front_face) {
        case FrontFace::clockwise:
            front_face = opengl::FrontFaceWinding::clockwise;
            break;
        case FrontFace::counter_clockwise:
            front_face = opengl::FrontFaceWinding::counter_clockwise;
            break;
        default:
            return std::unexpected(opengl::Diagnostic{
                .code = opengl::ErrorCode::operation_failed,
                .message = "analysis pipeline retained an invalid front-face winding",
            });
        }
        if (auto cull = device.set_cull_state({
                .mode = opengl::CullMode::none,
                .front_face = front_face,
            });
            !cull) {
            return cull;
        }
    } else if (raster_override == RasterOverride::depth_always) {
        if (auto depth = device.set_depth_state({
                .test_enabled = true,
                .write_enabled = true,
                .compare = opengl::DepthCompare::always,
            });
            !depth) {
            return depth;
        }
    }
    if (auto blend = device.set_blend_enabled(
            target.surface_key_attachment, false);
        !blend) {
        return blend;
    }
    if (auto mask = device.set_color_write_mask(
            0, {true, true, true, true});
        !mask) {
        return mask;
    }
    if (auto mask = device.set_color_write_mask(
            target.surface_key_attachment, {true, true, true, true});
        !mask) {
        return mask;
    }
    if (auto cleared = target.color.clear_rgba8(clear_color); !cleared) {
        return cleared;
    }
    if (auto cleared = target.surface_keys.clear_rg32ui(
            std::array{
                analysis::background_item.value,
                analysis::invalid_primitive.value});
        !cleared) {
        return cleared;
    }
    if (auto cleared = target.depth.clear_depth32f(clear_depth); !cleared) {
        return cleared;
    }

    auto uniform = analysis_pipeline_->program().set_uniform_u32(
        analysis_source()->analysis->first_item_uniform_location,
        first_item.value);
    if (!uniform) {
        return uniform;
    }
    return {};
}

std::expected<analysis::AnalysisCapture, opengl::Diagnostic>
OpenGLProgramRuntime::finish_analysis(
    analysis::AnalysisManifest manifest)
{
    auto& target = *analysis_target_;
    const auto extent = target.extent;
    auto colors = target.framebuffer.read_rgba8_pixels(
        0, 0, 0, extent.width, extent.height);
    if (!colors) {
        return std::unexpected(std::move(colors.error()));
    }
    auto depths = target.framebuffer.read_depth32f(
        0, 0, extent.width, extent.height);
    if (!depths) {
        return std::unexpected(std::move(depths.error()));
    }
    auto keys = target.framebuffer.read_rg32ui(
        target.surface_key_attachment,
        0,
        0,
        extent.width,
        extent.height);
    if (!keys) {
        return std::unexpected(std::move(keys.error()));
    }

    flip_rows(*colors, extent);
    flip_rows(*depths, extent);
    flip_rows(*keys, extent);

    std::vector<analysis::Rgba8> color_pixels;
    color_pixels.reserve(colors->size());
    for (const auto color : *colors) {
        color_pixels.push_back({color.r, color.g, color.b, color.a});
    }
    std::vector<analysis::SurfaceKey> surface_pixels;
    surface_pixels.reserve(keys->size());
    for (const auto key : *keys) {
        surface_pixels.push_back(analysis::SurfaceKey::from_raw({key.r, key.g}));
    }

    auto capture = analysis::AnalysisCapture::create(
        analysis::AnalysisCapture::ColorImage{
            extent, std::move(color_pixels)},
        analysis::AnalysisCapture::DepthImage{
            extent, std::move(*depths)},
        analysis::AnalysisCapture::SurfaceImage{
            extent, std::move(surface_pixels)},
        std::move(manifest));

    if (!capture) {
        return std::unexpected(capture_diagnostic(capture.error()));
    }
    return std::move(*capture);
}

} // namespace vng::render
