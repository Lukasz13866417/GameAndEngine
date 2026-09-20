#pragma once

#include <vng/glsl/emitter.hpp>
#include <vng/opengl/shader.hpp>

namespace vng::opengl {

// Conversion boundary between a context-free GLSL artifact and the OpenGL
// compiler. Low-level ShaderSource remains usable without this header; the
// high-level program compiler uses it internally.
[[nodiscard]] inline ShaderSource from_glsl(const glsl::StageSource& source)
{
    ShaderSource result;
    result.stage = source.stage == shader::StageKind::vertex
        ? ShaderStage::vertex
        : ShaderStage::fragment;
    result.text = source.source;
    result.source_map.reserve(source.source_map.size());
    for (const auto& mapping : source.source_map) {
        result.source_map.push_back(SourceMapEntry{
            .generated_line = mapping.generated_line,
            .ir_node = mapping.operation.value,
        });
    }
    if (source.stage == shader::StageKind::vertex) {
        std::vector<VertexInputMetadata> vertex_inputs;
        vertex_inputs.reserve(source.interface.inputs.size());
        for (const auto& input : source.interface.inputs) {
            if (input.builtin != shader::Builtin::none) {
                continue;
            }
            vertex_inputs.push_back(VertexInputMetadata{
                .semantic_type = input.semantic_type,
                .semantic_name = input.semantic_name,
                .location = input.location,
            });
        }
        result.set_typed_vertex_inputs(std::move(vertex_inputs));
    }
    return result;
}

} // namespace vng::opengl
