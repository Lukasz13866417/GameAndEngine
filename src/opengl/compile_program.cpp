#include <vng/opengl/program.hpp>
#include <vng/glsl/emitter.hpp>
#include <vng/opengl/glsl_source.hpp>

#include <algorithm>
#include <limits>
#include <string_view>

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

} // namespace

std::expected<Program, Diagnostic> compile_graphics_program(
    const Device& device, const glsl::ProgramSource& source)
{
    auto compiled = compile_program(device, source);
    if (!compiled) return compiled;
    compiled->generated_source_ = source;
    return compiled;
}

std::expected<Program, Diagnostic> compile_graphics_program(
    const Device& device, const shader::GraphicsProgram& program)
{
    auto source = glsl::emit(program);
    if (!source) {
        return std::unexpected(emission_diagnostic(std::move(source.error())));
    }
    return compile_graphics_program(device, *source);
}

} // namespace vng::opengl
