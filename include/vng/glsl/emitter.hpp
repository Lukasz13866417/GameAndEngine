#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <vector>

#include <vng/gfx/semantic.hpp>
#include <vng/shader/program.hpp>

namespace vng::glsl {

struct SourceMapEntry {
    std::uint32_t generated_line{};
    shader::OperationId operation;
    shader::SourceOrigin origin;
};

struct InterfaceVariable {
    std::type_index semantic_type{typeid(void)};
    std::string semantic_name;
    std::string glsl_type;
    std::optional<std::uint32_t> location;
    shader::Interpolation interpolation{shader::Interpolation::none};
    shader::Builtin builtin{shader::Builtin::none};
    std::uint32_t builtin_index{};

    friend bool operator==(const InterfaceVariable&, const InterfaceVariable&) = default;
};

struct InterfaceMetadata {
    std::vector<InterfaceVariable> inputs;
    std::vector<InterfaceVariable> outputs;

    friend bool operator==(const InterfaceMetadata&, const InterfaceMetadata&) = default;
};

// Uniform storage is a backend lowering choice, never a CPU record ABI.
// Records are flattened into logical leaves and rebuilt in the shader.
struct ParameterLeaf final {
    std::string name;
    shader::ScalarKind scalar{shader::ScalarKind::f32};
    std::uint32_t columns{1};
    std::uint32_t rows{1};
    std::uint32_t word_offset{};
    std::uint32_t location{};
    friend bool operator==(const ParameterLeaf&, const ParameterLeaf&) = default;
};

struct ParameterMetadata final {
    shader::ParameterKind kind{shader::ParameterKind::camera_view_projection};
    std::string name;
    std::string glsl_type;
    std::uint32_t location{};
    std::uint32_t argument_index{};
    std::type_index argument_type{typeid(void)};
    std::uint32_t word_count{};
    std::vector<ParameterLeaf> leaves;

    friend bool operator==(const ParameterMetadata&, const ParameterMetadata&) = default;
};

struct StageSource {
    shader::StageKind stage{shader::StageKind::vertex};
    std::string debug_name;
    std::string source;
    std::vector<SourceMapEntry> source_map;
    InterfaceMetadata interface;
    // Sorted, unique slots of the sampler2D resources used by this emission.
    std::vector<std::uint32_t> texture_bindings{};
    // Sorted, unique read-only Mat4 buffer slots used by this emission.
    std::vector<std::uint32_t> matrix_buffer_bindings{};

    [[nodiscard]] const SourceMapEntry* mapping_for_line(
        std::uint32_t generated_line) const noexcept;
};

// Requests the inspection variant of a linked graphics program. The shader
// program itself is not rebuilt or modified: the GLSL backend adds the
// renderer-owned plumbing needed to identify the visible draw instance and
// primitive at each fragment.
struct AnalysisEmission final {};

struct AnalysisEmissionMetadata final {
    // Location shared by the generated flat vertex output and fragment input.
    std::uint32_t item_id_varying_location{};

    // Fragment color attachment receiving uvec2(item id, primitive id).
    std::uint32_t surface_key_location{};

    // Set this once per draw to the first logical item represented by that
    // draw. The generated vertex shader adds gl_InstanceID.
    std::string first_item_uniform_name;
    std::uint32_t first_item_uniform_location{};

    friend bool operator==(const AnalysisEmissionMetadata&,
                           const AnalysisEmissionMetadata&) = default;
};

// Selects one semantic value previously exposed with StageContext::observe.
// The descriptor is deliberately backend-facing rather than a new shader IR
// construct: observations stay inert until an emitter is explicitly asked to
// materialize one.
struct ObservationEmission final {
    shader::StageKind stage{shader::StageKind::fragment};
    std::type_index semantic_type{typeid(void)};
    std::type_index value_type{typeid(void)};
    std::string semantic_name;

    friend bool operator==(const ObservationEmission&,
                           const ObservationEmission&) = default;
};

template<gfx::SemanticType Semantic>
[[nodiscard]] ObservationEmission observation(
    Semantic,
    shader::StageKind stage = shader::StageKind::fragment)
{
    return ObservationEmission{
        .stage = stage,
        .semantic_type = typeid(Semantic),
        .value_type = typeid(gfx::semantic_value_t<Semantic>),
        .semantic_name = shader::detail::type_name<Semantic>(),
    };
}

template<gfx::SemanticType Semantic>
[[nodiscard]] ObservationEmission observation(
    shader::StageKind stage = shader::StageKind::fragment)
{
    return observation(Semantic{}, stage);
}

struct ObservationEmissionMetadata final {
    shader::StageKind source_stage{shader::StageKind::fragment};
    std::type_index semantic_type{typeid(void)};
    std::type_index value_type{typeid(void)};
    std::string semantic_name;
    // Type of the expression before attachment padding (for example vec2).
    std::string glsl_type;
    // Always vec4, ivec4, or uvec4 so a backend needs only three readback
    // texture formats. component_count identifies the meaningful prefix.
    std::string attachment_glsl_type;
    shader::ScalarKind scalar_kind{shader::ScalarKind::f32};
    std::uint32_t component_count{};
    std::uint32_t attachment_location{};
    std::string output_name;
    shader::SourceOrigin origin;

    friend bool operator==(const ObservationEmissionMetadata& left,
                           const ObservationEmissionMetadata& right)
    {
        return left.source_stage == right.source_stage
            && left.semantic_type == right.semantic_type
            && left.value_type == right.value_type
            && left.semantic_name == right.semantic_name
            && left.glsl_type == right.glsl_type
            && left.attachment_glsl_type == right.attachment_glsl_type
            && left.scalar_kind == right.scalar_kind
            && left.component_count == right.component_count
            && left.attachment_location == right.attachment_location
            && left.output_name == right.output_name
            && left.origin.file == right.origin.file
            && left.origin.function == right.origin.function
            && left.origin.line == right.origin.line
            && left.origin.column == right.origin.column;
    }
};

struct ProgramSource {
    StageSource vertex;
    StageSource fragment;
    std::vector<ParameterMetadata> parameters;
    std::optional<AnalysisEmissionMetadata> analysis;
    std::optional<ObservationEmissionMetadata> observation;
    std::vector<std::uint32_t> texture_bindings{};
    std::vector<std::uint32_t> matrix_buffer_bindings{};

    [[nodiscard]] std::string dump() const;
};

// A stage must already have locations assigned by shader::link. The function
// is public for inspection and tests; normal callers should emit a linked
// GraphicsProgram through the overload below.
[[nodiscard]] shader::Result<StageSource> emit(const shader::ShaderStage& stage);

[[nodiscard]] shader::Result<ProgramSource> emit(
    const shader::GraphicsProgram& program);

// Emits the same linked program with an additional RG32UI-compatible surface
// key output. Existing expressions and outputs use the normal lowering path;
// only structured declarations and terminal writes are added.
[[nodiscard]] shader::Result<ProgramSource> emit(
    const shader::GraphicsProgram& program,
    AnalysisEmission);

// Emits one fragment-stage observation to a newly allocated color attachment.
// Integer observations produce integer fragment outputs; float observations
// produce floating-point outputs. Bool, matrix, and record observations are
// rejected because they have no direct framebuffer attachment representation.
[[nodiscard]] shader::Result<ProgramSource> emit(
    const shader::GraphicsProgram& program,
    const ObservationEmission& request);

[[nodiscard]] std::string dump_source(const ProgramSource& program);

template<shader::Argument... Args, class... Options>
[[nodiscard]] auto emit(const shader::TypedGraphicsProgram<Args...>& program,
                        const Options&... options)
    -> decltype(emit(program.untyped(), options...))
{
    return emit(program.untyped(), options...);
}

} // namespace vng::glsl
