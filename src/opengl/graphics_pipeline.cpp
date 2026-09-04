#include <vng/opengl/graphics_pipeline.hpp>

#include <vng/glsl/emitter.hpp>
#include <vng/opengl/glsl_source.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace vng::opengl {
namespace {

[[nodiscard]] Diagnostic emission_diagnostic(shader::Diagnostic diagnostic)
{
    std::string message = "GLSL emission failed: " + diagnostic.message;
    for (const auto& note : diagnostic.notes) {
        message += "\n" + note;
    }
    return Diagnostic{
        .code = ErrorCode::invalid_argument,
        .message = std::move(message),
        .generated_source = std::move(diagnostic.generated_source),
    };
}

void attach_stage_evidence(
    Diagnostic& diagnostic,
    const glsl::StageSource& source)
{
    if (diagnostic.generated_source.empty()) {
        diagnostic.generated_source = source.source;
    }
    if (diagnostic.source_map.empty()) {
        diagnostic.source_map.reserve(source.source_map.size());
        for (const auto& mapping : source.source_map) {
            diagnostic.source_map.push_back(SourceMapEntry{
                .generated_line = mapping.generated_line,
                .ir_node = mapping.operation.value,
            });
        }
    }
}

void attach_program_evidence(
    Diagnostic& diagnostic,
    const glsl::ProgramSource& source)
{
    if (!diagnostic.generated_source.empty()) {
        return;
    }

    const auto append_stage = [&](
        std::string_view heading,
        const glsl::StageSource& stage) {
        diagnostic.generated_source += heading;
        const auto first_line = static_cast<std::uint64_t>(
            std::ranges::count(diagnostic.generated_source, '\n') + 1);
        for (const auto& mapping : stage.source_map) {
            if (mapping.generated_line == 0) {
                continue;
            }
            const auto combined_line =
                first_line + mapping.generated_line - 1;
            if (combined_line <= std::numeric_limits<std::uint32_t>::max()) {
                diagnostic.source_map.push_back(SourceMapEntry{
                    .generated_line = static_cast<std::uint32_t>(combined_line),
                    .ir_node = mapping.operation.value,
                });
            }
        }
        diagnostic.generated_source += stage.source;
        if (!diagnostic.generated_source.ends_with('\n')) {
            diagnostic.generated_source += '\n';
        }
    };

    append_stage("// ---- vertex shader ----\n", source.vertex);
    append_stage("// ---- fragment shader ----\n", source.fragment);
}

[[nodiscard]] std::expected<Program, Diagnostic> compile_program(
    const Device& device,
    const glsl::ProgramSource& source)
{
    auto vertex = Shader::compile(device, from_glsl(source.vertex));
    if (!vertex) {
        auto diagnostic = std::move(vertex.error());
        attach_stage_evidence(diagnostic, source.vertex);
        return std::unexpected(std::move(diagnostic));
    }
    auto fragment = Shader::compile(device, from_glsl(source.fragment));
    if (!fragment) {
        auto diagnostic = std::move(fragment.error());
        attach_stage_evidence(diagnostic, source.fragment);
        return std::unexpected(std::move(diagnostic));
    }
    auto program = Program::link_graphics(device, *vertex, *fragment);
    if (!program) {
        auto diagnostic = std::move(program.error());
        attach_program_evidence(diagnostic, source);
        return std::unexpected(std::move(diagnostic));
    }
    return program;
}

[[nodiscard]] std::unexpected<Diagnostic> invalid_enum(
    std::string_view member)
{
    return std::unexpected(Diagnostic{
        .code = ErrorCode::invalid_argument,
        .message = "GraphicsPipeline::realize received an invalid "
            + std::string{member},
    });
}

[[nodiscard]] std::expected<DepthCompare, Diagnostic> translate_compare(
    render::DepthCompare compare)
{
    switch (compare) {
    case render::DepthCompare::never:
        return DepthCompare::never;
    case render::DepthCompare::less:
        return DepthCompare::less;
    case render::DepthCompare::less_equal:
        return DepthCompare::less_equal;
    case render::DepthCompare::equal:
        return DepthCompare::equal;
    case render::DepthCompare::greater_equal:
        return DepthCompare::greater_equal;
    case render::DepthCompare::greater:
        return DepthCompare::greater;
    case render::DepthCompare::not_equal:
        return DepthCompare::not_equal;
    case render::DepthCompare::always:
        return DepthCompare::always;
    }
    return invalid_enum("depth comparison");
}

[[nodiscard]] std::expected<CullMode, Diagnostic> translate_cull(
    render::CullMode cull)
{
    switch (cull) {
    case render::CullMode::none:
        return CullMode::none;
    case render::CullMode::front:
        return CullMode::front;
    case render::CullMode::back:
        return CullMode::back;
    }
    return invalid_enum("cull mode");
}

[[nodiscard]] std::expected<FrontFaceWinding, Diagnostic> translate_front_face(
    render::FrontFace front_face)
{
    switch (front_face) {
    case render::FrontFace::clockwise:
        return FrontFaceWinding::clockwise;
    case render::FrontFace::counter_clockwise:
        return FrontFaceWinding::counter_clockwise;
    }
    return invalid_enum("front-face winding");
}

[[nodiscard]] std::expected<bool, Diagnostic> translate_output_encoding(
    render::ColorEncoding encoding)
{
    switch (encoding) {
    case render::ColorEncoding::linear:
        return false;
    case render::ColorEncoding::srgb:
        return true;
    }
    return invalid_enum("output color encoding");
}

} // namespace

GraphicsPipeline::GraphicsPipeline(
    Program program,
    render::GraphicsPipelineDesc description,
    DepthState depth,
    CullState cull,
    bool framebuffer_srgb) noexcept
    : program_(std::move(program)),
      description_(description),
      depth_(depth),
      cull_(cull),
      framebuffer_srgb_(framebuffer_srgb)
{
}

std::expected<GraphicsPipeline, Diagnostic> GraphicsPipeline::realize(
    const Device& device,
    Program&& program,
    render::GraphicsPipelineDesc description)
{
    auto compare = translate_compare(description.depth.compare);
    if (!compare) {
        return std::unexpected(std::move(compare.error()));
    }
    auto cull = translate_cull(description.cull);
    if (!cull) {
        return std::unexpected(std::move(cull.error()));
    }
    auto front_face = translate_front_face(description.front_face);
    if (!front_face) {
        return std::unexpected(std::move(front_face.error()));
    }
    auto output_encoding = translate_output_encoding(
        description.output_encoding);
    if (!output_encoding) {
        return std::unexpected(std::move(output_encoding.error()));
    }

    if (program.native_handle() == 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "GraphicsPipeline::realize received an empty program",
        });
    }
    if (!program.belongs_to(device)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::incompatible_device,
            .message = "GraphicsPipeline::realize received a program from a different device/context",
        });
    }
    return GraphicsPipeline{
        std::move(program),
        description,
        DepthState{
            .test_enabled = description.depth.test,
            .write_enabled = description.depth.write,
            .compare = *compare,
        },
        CullState{
            .mode = *cull,
            .front_face = *front_face,
        },
        *output_encoding,
    };
}

std::expected<void, Diagnostic>
GraphicsPipeline::adopt_emitted_fragment_outputs(
    const glsl::ProgramSource& source)
{
    fragment_color_output_locations_.emplace();
    for (const auto& output : source.fragment.interface.outputs) {
        // Fragment depth is not a draw-buffer output. Every other emitted
        // fragment output is, including renderer-injected analysis and
        // observation attachments whose neutral Builtin is intentionally
        // `none`.
        if (output.builtin == shader::Builtin::fragment_depth) {
            continue;
        }
        if (!output.location) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_argument,
                .message = "GLSL emission produced a non-depth fragment output without a location",
            });
        }
        fragment_color_output_locations_->push_back(*output.location);
    }
    std::ranges::sort(*fragment_color_output_locations_);
    const auto unique_end = std::ranges::unique(
        *fragment_color_output_locations_).begin();
    fragment_color_output_locations_->erase(
        unique_end,
        fragment_color_output_locations_->end());
    return {};
}

std::expected<void, Diagnostic> GraphicsPipeline::bind(
    const Device& device) const
{
    if (!belongs_to(device)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::incompatible_device,
            .message = "GraphicsPipeline::bind used a different device/context than create",
        });
    }
    if (auto bound = program_.bind(); !bound) {
        return bound;
    }
    if (auto raster = device.set_standard_raster_state(); !raster) {
        return raster;
    }
    if (auto depth = device.set_depth_state(depth_); !depth) {
        return depth;
    }
    const auto color_outputs = fragment_color_output_locations_
        ? std::span<const std::uint32_t>{
              *fragment_color_output_locations_}
        : std::span<const std::uint32_t>{};
    if (auto colors = device.set_pipeline_color_output_baseline(
            color_outputs,
            !fragment_color_output_locations_.has_value());
        !colors) {
        return colors;
    }
    if (auto scissor = device.set_scissor_enabled(false); !scissor) {
        return scissor;
    }
    if (auto cull = device.set_cull_state(cull_); !cull) {
        return cull;
    }
    if (auto discard = device.set_rasterizer_discard_enabled(false); !discard) {
        return discard;
    }
    return device.set_framebuffer_srgb_enabled(framebuffer_srgb_);
}

std::expected<GraphicsPipeline, Diagnostic> compile_graphics_pipeline(
    const Device& device,
    const shader::GraphicsProgram& program,
    render::GraphicsPipelineDesc description)
{
    auto source = glsl::emit(program);
    if (!source) {
        return std::unexpected(emission_diagnostic(std::move(source.error())));
    }
    auto backend_program = compile_program(device, *source);
    if (!backend_program) {
        return std::unexpected(std::move(backend_program.error()));
    }
    auto pipeline = GraphicsPipeline::realize(
        device, std::move(*backend_program), description);
    if (!pipeline) {
        auto diagnostic = std::move(pipeline.error());
        attach_program_evidence(diagnostic, *source);
        return std::unexpected(std::move(diagnostic));
    }
    if (auto adopted = pipeline->adopt_emitted_fragment_outputs(*source);
        !adopted) {
        auto diagnostic = std::move(adopted.error());
        attach_program_evidence(diagnostic, *source);
        return std::unexpected(std::move(diagnostic));
    }
    pipeline->generated_source_.emplace(std::move(*source));
    return pipeline;
}

std::expected<GraphicsPipeline, Diagnostic>
GraphicsPipeline::realize_emitted(
    const Device& device,
    Program&& program,
    const glsl::ProgramSource& source,
    render::GraphicsPipelineDesc description)
{
    auto pipeline = GraphicsPipeline::realize(
        device, std::move(program), description);
    if (!pipeline) {
        auto diagnostic = std::move(pipeline.error());
        attach_program_evidence(diagnostic, source);
        return std::unexpected(std::move(diagnostic));
    }
    if (auto adopted = pipeline->adopt_emitted_fragment_outputs(source);
        !adopted) {
        auto diagnostic = std::move(adopted.error());
        attach_program_evidence(diagnostic, source);
        return std::unexpected(std::move(diagnostic));
    }
    pipeline->generated_source_.emplace(source);
    return pipeline;
}

std::expected<GraphicsPipeline, Diagnostic> realize_graphics_pipeline(
    const Device& device,
    Program&& program,
    render::GraphicsPipelineDesc description)
{
    return GraphicsPipeline::realize(
        device, std::move(program), description);
}

} // namespace vng::opengl
