#include <vng/glsl/emitter.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vng::glsl {
namespace {

using shader::Builtin;
using shader::Diagnostic;
using shader::DiagnosticCode;
using shader::Effect;
using shader::InterfaceField;
using shader::ModuleIR;
using shader::OpCode;
using shader::Operation;
using shader::OperationId;
using shader::ParameterField;
using shader::ParameterKind;
using shader::ScalarKind;
using shader::StageKind;
using shader::TypeDescription;
using shader::TypeId;
using shader::TypeKind;
using shader::ValueId;

class SourceWriter {
public:
    void line(std::string_view text = {})
    {
        source_.append(text);
        source_.push_back('\n');
        ++next_line_;
    }

    [[nodiscard]] std::uint32_t next_line() const noexcept { return next_line_; }
    [[nodiscard]] const std::string& source() const noexcept { return source_; }
    [[nodiscard]] std::string take() && { return std::move(source_); }

private:
    std::string source_;
    std::uint32_t next_line_{1};
};

[[nodiscard]] Diagnostic diagnostic(
    DiagnosticCode code,
    std::string message,
    const std::string& source,
    const Operation* operation = nullptr)
{
    Diagnostic result{
        .code = code,
        .message = std::move(message),
        .notes = {},
        .origin = {},
        .generated_source = source,
    };
    if (operation != nullptr) {
        result.origin = operation->origin;
    }
    return result;
}

[[nodiscard]] std::string input_name(std::size_t index)
{
    return "vng_in_" + std::to_string(index);
}

[[nodiscard]] std::string output_name(std::size_t index)
{
    return "vng_out_" + std::to_string(index);
}

[[nodiscard]] std::string texture_name(u32 binding)
{
    return "vng_texture_" + std::to_string(binding);
}

[[nodiscard]] std::string matrix_buffer_name(u32 binding)
{
    return "vng_matrix_" + std::to_string(binding);
}

[[nodiscard]] std::vector<u32> collect_matrix_buffer_bindings(
    const StageSource& vertex, const StageSource& fragment)
{
    auto result = vertex.matrix_buffer_bindings;
    result.insert(result.end(), fragment.matrix_buffer_bindings.begin(),
                  fragment.matrix_buffer_bindings.end());
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

[[nodiscard]] std::vector<u32> collect_texture_bindings(
    const StageSource& vertex, const StageSource& fragment)
{
    auto result = vertex.texture_bindings;
    result.insert(result.end(), fragment.texture_bindings.begin(),
                  fragment.texture_bindings.end());
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

[[nodiscard]] std::string record_name(TypeId id)
{
    return "vng_record_" + std::to_string(id.value);
}

[[nodiscard]] std::string member_name(std::size_t index)
{
    return "m" + std::to_string(index);
}

constexpr std::string_view analysis_item_id_name = "vng_analysis_item_id";
constexpr std::string_view analysis_surface_key_name = "vng_analysis_surface_key";
constexpr std::string_view analysis_first_item_uniform_name =
    "vng_analysis_first_item_id";
constexpr std::string_view observation_output_name = "vng_observation_value";

[[nodiscard]] std::string parameter_uniform_name(
    const ParameterField& parameter)
{
    switch (parameter.kind) {
    case ParameterKind::camera_view_projection:
        return "vng_camera_view_projection";
    case ParameterKind::argument:
        return "vng_argument_" + std::to_string(parameter.argument_index);
    }
    return "vng_unknown_parameter";
}

[[nodiscard]] u32 parameter_location_count(const ModuleIR& module, TypeId id)
{
    const auto& type = module.types[id];
    if (type.kind != TypeKind::record) return 1;
    u32 result{};
    for (const auto& member : type.members) {
        result += parameter_location_count(module, member.type);
    }
    return result;
}

void collect_parameter_leaves(
    const ModuleIR& module, TypeId id, u32& word, u32& location,
    std::vector<ParameterLeaf>& leaves, const std::string& name)
{
    const auto& type = module.types[id];
    if (type.kind == TypeKind::record) {
        for (std::size_t index = 0; index < type.members.size(); ++index) {
            collect_parameter_leaves(module, type.members[index].type, word, location,
                leaves, name + "_" + member_name(index));
        }
        return;
    }
    leaves.push_back(ParameterLeaf{
        .name = name,
        .scalar = type.scalar,
        .columns = type.kind == TypeKind::vector ? 1U : type.columns,
        .rows = type.kind == TypeKind::vector ? type.columns : type.rows,
        .word_offset = word, .location = location++,
    });
    word += type.columns * type.rows;
}

struct StageEmissionPlan {
    const AnalysisEmissionMetadata* analysis{};
    const ObservationEmissionMetadata* observation{};
    const shader::Observation* selected_observation{};
};

struct LocationRange final {
    std::uint64_t begin{};
    std::uint64_t end{};
};

template<class Predicate>
[[nodiscard]] shader::Result<void> append_occupied_locations(
    const ModuleIR& module,
    const std::vector<InterfaceField>& fields,
    Predicate include,
    std::vector<LocationRange>& occupied)
{
    for (const auto& field : fields) {
        if (!include(field) || !field.location) {
            continue;
        }
        if (!field.type.valid() || field.type.value >= module.types.size()) {
            return std::unexpected(diagnostic(
                DiagnosticCode::invalid_ir,
                "cannot allocate an injected GLSL location after an invalid interface type",
                {}));
        }

        const auto& type = module.types[field.type];
        const auto width = type.kind == TypeKind::matrix
            ? static_cast<std::uint64_t>(type.columns)
            : std::uint64_t{1};
        if (width == 0) {
            return std::unexpected(diagnostic(
                DiagnosticCode::invalid_ir,
                "cannot allocate an injected GLSL location around a zero-width interface type",
                {}));
        }
        const auto end = static_cast<std::uint64_t>(*field.location) + width;
        if (end > static_cast<std::uint64_t>(std::numeric_limits<u32>::max()) + 1) {
            return std::unexpected(diagnostic(
                DiagnosticCode::interface_mismatch,
                "an existing interface field exceeds the representable GLSL location range",
                {}));
        }
        occupied.push_back(LocationRange{
            .begin = *field.location,
            .end = end,
        });
    }
    return {};
}

[[nodiscard]] shader::Result<void> append_occupied_parameter_locations(
    const ModuleIR& module,
    std::vector<LocationRange>& occupied)
{
    for (const auto& parameter : module.parameters) {
        if (!parameter.location) {
            return std::unexpected(diagnostic(
                DiagnosticCode::interface_mismatch,
                "cannot allocate an injected GLSL uniform before shader parameters are linked",
                {}));
        }
        if (!parameter.type.valid() || parameter.type.value >= module.types.size()) {
            return std::unexpected(diagnostic(
                DiagnosticCode::invalid_ir,
                "cannot allocate an injected GLSL uniform after an invalid parameter type",
                {}));
        }
        occupied.push_back(LocationRange{
            .begin = *parameter.location,
            .end = static_cast<std::uint64_t>(*parameter.location)
                + parameter_location_count(module, parameter.type),
        });
    }
    return {};
}

[[nodiscard]] shader::Result<u32> first_free_location(
    std::vector<LocationRange> occupied,
    u32 required_width = 1)
{
    if (required_width == 0) {
        return std::unexpected(diagnostic(
            DiagnosticCode::invalid_ir,
            "cannot allocate a zero-width GLSL interface location range",
            {}));
    }

    std::ranges::sort(occupied, {}, &LocationRange::begin);
    std::uint64_t candidate = 0;
    const auto width = static_cast<std::uint64_t>(required_width);
    for (const auto range : occupied) {
        if (range.end <= candidate) {
            continue;
        }
        if (range.begin >= candidate + width) {
            return static_cast<u32>(candidate);
        }
        candidate = range.end;
    }

    if (candidate + width
        > static_cast<std::uint64_t>(std::numeric_limits<u32>::max()) + 1) {
        return std::unexpected(diagnostic(
            DiagnosticCode::interface_mismatch,
            "no representable interface location remains for analysis emission",
            {}));
    }
    return static_cast<u32>(candidate);
}

[[nodiscard]] shader::Result<AnalysisEmissionMetadata> make_analysis_metadata(
    const shader::GraphicsProgram& program)
{
    const auto& vertex = program.vertex().ir();
    const auto& fragment = program.fragment().ir();
    auto is_varying = [](const InterfaceField& field) {
        return field.builtin == Builtin::none;
    };
    auto is_fragment_target = [](const InterfaceField& field) {
        return field.builtin == Builtin::none || field.builtin == Builtin::color;
    };

    std::vector<LocationRange> varying_locations;
    if (auto result = append_occupied_locations(
            vertex, vertex.outputs, is_varying, varying_locations);
        !result) {
        return std::unexpected(std::move(result.error()));
    }
    if (auto result = append_occupied_locations(
            fragment, fragment.inputs, is_varying, varying_locations);
        !result) {
        return std::unexpected(std::move(result.error()));
    }
    auto item_id_varying_location = first_free_location(
        std::move(varying_locations));
    if (!item_id_varying_location) {
        return std::unexpected(std::move(item_id_varying_location.error()));
    }

    std::vector<LocationRange> fragment_target_locations;
    if (auto result = append_occupied_locations(
            fragment,
            fragment.outputs,
            is_fragment_target,
            fragment_target_locations);
        !result) {
        return std::unexpected(std::move(result.error()));
    }
    auto surface_key_location = first_free_location(
        std::move(fragment_target_locations));
    if (!surface_key_location) {
        return std::unexpected(std::move(surface_key_location.error()));
    }

    std::vector<LocationRange> parameter_locations;
    if (auto result = append_occupied_parameter_locations(
            vertex, parameter_locations);
        !result) {
        return std::unexpected(std::move(result.error()));
    }
    if (auto result = append_occupied_parameter_locations(
            fragment, parameter_locations);
        !result) {
        return std::unexpected(std::move(result.error()));
    }
    auto first_item_uniform_location = first_free_location(
        std::move(parameter_locations));
    if (!first_item_uniform_location) {
        return std::unexpected(std::move(first_item_uniform_location.error()));
    }

    return AnalysisEmissionMetadata{
        .item_id_varying_location = *item_id_varying_location,
        .surface_key_location = *surface_key_location,
        .first_item_uniform_name = std::string(analysis_first_item_uniform_name),
        .first_item_uniform_location = *first_item_uniform_location,
    };
}

[[nodiscard]] shader::Result<std::string> glsl_type(
    const ModuleIR& module,
    TypeId id,
    const std::string& source,
    const std::vector<std::optional<u32>>* emitted_type_ids = nullptr)
{
    if (!id.valid() || id.value >= module.types.size()) {
        return std::unexpected(diagnostic(
            DiagnosticCode::invalid_ir,
            "GLSL emission encountered an invalid type id",
            source));
    }

    const auto& type = module.types[id];
    switch (type.kind) {
    case TypeKind::scalar:
        switch (type.scalar) {
        case ScalarKind::boolean: return "bool";
        case ScalarKind::i32: return "int";
        case ScalarKind::u32: return "uint";
        case ScalarKind::f32: return "float";
        }
        break;
    case TypeKind::vector: {
        std::string prefix;
        switch (type.scalar) {
        case ScalarKind::boolean: prefix = "bvec"; break;
        case ScalarKind::i32: prefix = "ivec"; break;
        case ScalarKind::u32: prefix = "uvec"; break;
        case ScalarKind::f32: prefix = "vec"; break;
        }
        if (type.columns < 2 || type.columns > 4) {
            return std::unexpected(diagnostic(
                DiagnosticCode::unsupported_operation,
                "GLSL supports shader vectors with two to four components",
                source));
        }
        return prefix + std::to_string(type.columns);
    }
    case TypeKind::matrix:
        if (type.scalar != ScalarKind::f32 || type.columns < 2 || type.columns > 4
            || type.rows < 2 || type.rows > 4) {
            return std::unexpected(diagnostic(
                DiagnosticCode::unsupported_operation,
                "GLSL lowering currently supports only float matrices from 2x2 to 4x4",
                source));
        }
        if (type.columns == type.rows) {
            return "mat" + std::to_string(type.columns);
        }
        return "mat" + std::to_string(type.columns) + "x" + std::to_string(type.rows);
    case TypeKind::record:
        if (emitted_type_ids != nullptr) {
            if (id.value >= emitted_type_ids->size()
                || !(*emitted_type_ids)[id.value]) {
                return std::unexpected(diagnostic(
                    DiagnosticCode::invalid_ir,
                    "GLSL emission encountered a record outside the executable type slice",
                    source));
            }
            return record_name(TypeId{*(*emitted_type_ids)[id.value]});
        }
        return record_name(id);
    case TypeKind::poison:
        return std::unexpected(diagnostic(
            DiagnosticCode::invalid_ir,
            "cannot emit the poison type as GLSL",
            source));
    }

    return std::unexpected(diagnostic(
        DiagnosticCode::unsupported_operation,
        "unrecognized shader type during GLSL lowering",
        source));
}

struct SelectedObservation final {
    const shader::Observation* observation{};
    ObservationEmissionMetadata metadata;
};

[[nodiscard]] shader::Result<SelectedObservation> select_observation(
    const shader::GraphicsProgram& program,
    const ObservationEmission& request)
{
    if (request.stage != StageKind::fragment) {
        return std::unexpected(diagnostic(
            DiagnosticCode::unsupported_operation,
            "GLSL observation emission currently supports fragment-stage "
            "observations only; a vertex observation needs an explicit "
            "interpolation contract",
            {}));
    }

    const auto& module = program.fragment().ir();
    if (auto validity = shader::validate(module); !validity) {
        return std::unexpected(std::move(validity.error()));
    }
    if (request.semantic_type == typeid(void)
        || request.value_type == typeid(void)) {
        return std::unexpected(diagnostic(
            DiagnosticCode::interface_mismatch,
            "GLSL observation emission requires a semantic and its value type",
            {}));
    }

    const auto selected = std::ranges::find(
        module.observations,
        request.semantic_type,
        &shader::Observation::semantic_type);
    if (selected == module.observations.end()) {
        return std::unexpected(diagnostic(
            DiagnosticCode::interface_mismatch,
            "fragment stage does not expose observation "
                + (request.semantic_name.empty()
                       ? std::string{"<unnamed semantic>"}
                       : request.semantic_name),
            {}));
    }
    if (selected->value_type != request.value_type) {
        return std::unexpected(diagnostic(
            DiagnosticCode::interface_mismatch,
            "requested observation type disagrees with the value type exposed "
            "for " + selected->semantic_name,
            {}));
    }

    const auto& type = module.types[selected->type];
    const bool scalar = type.kind == TypeKind::scalar;
    const bool vector = type.kind == TypeKind::vector;
    if ((!scalar && !vector) || type.scalar == ScalarKind::boolean) {
        return std::unexpected(diagnostic(
            DiagnosticCode::unsupported_operation,
            "observation " + selected->semantic_name
                + " cannot be represented directly by a framebuffer attachment; "
                  "only float, signed-integer, and unsigned-integer scalars or "
                  "vectors are supported",
            {}));
    }

    const auto components = scalar ? u32{1} : type.columns;
    if (components == 0 || components > 4) {
        return std::unexpected(diagnostic(
            DiagnosticCode::unsupported_operation,
            "observation " + selected->semantic_name
                + " has an unsupported component count",
            {}));
    }

    const auto producer = module.values[selected->value.value].producer;
    const auto& root = module.regions[module.root_region.value].operations;
    if (std::ranges::find(root, producer) == root.end()) {
        return std::unexpected(diagnostic(
            DiagnosticCode::unsupported_operation,
            "observation " + selected->semantic_name
                + " is defined in a nested region and cannot yet be exported "
                  "from fragment-stage scope",
            {}));
    }

    std::vector<LocationRange> occupied;
    const auto is_fragment_target = [](const InterfaceField& field) {
        return field.builtin == Builtin::none || field.builtin == Builtin::color;
    };
    if (auto result = append_occupied_locations(
            module, module.outputs, is_fragment_target, occupied);
        !result) {
        return std::unexpected(std::move(result.error()));
    }
    auto location = first_free_location(std::move(occupied));
    if (!location) {
        return std::unexpected(std::move(location.error()));
    }

    auto emitted_type = glsl_type(module, selected->type, {});
    if (!emitted_type) {
        return std::unexpected(std::move(emitted_type.error()));
    }
    const auto attachment_type = [&]() -> std::string {
        switch (type.scalar) {
        case ScalarKind::f32: return "vec4";
        case ScalarKind::i32: return "ivec4";
        case ScalarKind::u32: return "uvec4";
        case ScalarKind::boolean: break;
        }
        return {};
    }();

    return SelectedObservation{
        .observation = &*selected,
        .metadata = ObservationEmissionMetadata{
            .source_stage = StageKind::fragment,
            .semantic_type = selected->semantic_type,
            .value_type = selected->value_type,
            .semantic_name = selected->semantic_name,
            .glsl_type = std::move(*emitted_type),
            .attachment_glsl_type = attachment_type,
            .scalar_kind = type.scalar,
            .component_count = components,
            .attachment_location = *location,
            .output_name = std::string(observation_output_name),
            .origin = selected->origin,
        },
    };
}

[[nodiscard]] shader::Result<std::vector<ParameterMetadata>>
collect_parameter_metadata(const shader::GraphicsProgram& program)
{
    std::vector<ParameterMetadata> result;
    auto collect = [&](const ModuleIR& module) -> shader::Result<void> {
        for (const auto& parameter : module.parameters) {
            if (!parameter.location) {
                return std::unexpected(diagnostic(
                    DiagnosticCode::interface_mismatch,
                    "GLSL emission requires linked locations for every shader parameter",
                    {}));
            }
            auto type = glsl_type(module, parameter.type, {});
            if (!type) {
                return std::unexpected(std::move(type.error()));
            }

            const auto existing = std::ranges::find_if(result, [&](const auto& item) {
                return item.kind == parameter.kind
                    && (parameter.kind != ParameterKind::argument
                        || item.argument_index == parameter.argument_index);
            });
            u32 word{};
            auto location = *parameter.location;
            std::vector<ParameterLeaf> leaves;
            collect_parameter_leaves(module, parameter.type, word, location, leaves,
                parameter_uniform_name(parameter));
            if (existing != result.end()) {
                if (existing->location != *parameter.location ||
                    existing->argument_type != parameter.argument_type ||
                    existing->leaves != leaves) {
                    return std::unexpected(diagnostic(
                        DiagnosticCode::interface_mismatch,
                        "matching shader parameters disagree across linked stages",
                        {}));
                }
                continue;
            }

            result.push_back(ParameterMetadata{
                .kind = parameter.kind,
                .name = parameter_uniform_name(parameter),
                .glsl_type = std::move(*type),
                .location = *parameter.location,
                .argument_index = parameter.argument_index,
                .argument_type = parameter.argument_type,
                .word_count = word,
                .leaves = std::move(leaves),
            });
        }
        return {};
    };

    if (auto vertex = collect(program.vertex().ir()); !vertex) {
        return std::unexpected(std::move(vertex.error()));
    }
    if (auto fragment = collect(program.fragment().ir()); !fragment) {
        return std::unexpected(std::move(fragment.error()));
    }
    return result;
}

[[nodiscard]] std::string hex_u32(std::uint32_t value)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::nouppercase << std::setw(8)
           << std::setfill('0') << value << 'u';
    return output.str();
}

[[nodiscard]] std::string float_literal(f32 value)
{
    if (!std::isfinite(value)) {
        return "uintBitsToFloat(" + hex_u32(std::bit_cast<std::uint32_t>(value)) + ')';
    }

    std::array<char, 64> buffer{};
    const auto result = std::to_chars(
        buffer.data(),
        buffer.data() + buffer.size(),
        value,
        std::chars_format::general,
        std::numeric_limits<f32>::max_digits10);
    std::string literal(buffer.data(), result.ptr);
    if (literal.find_first_of(".eE") == std::string::npos) {
        literal += ".0";
    }
    return literal;
}

[[nodiscard]] std::string constant_literal(const shader::ConstantPayload& payload)
{
    return std::visit([]<class T>(T value) -> std::string {
        if constexpr (std::same_as<T, bool>) {
            return value ? "true" : "false";
        } else if constexpr (std::same_as<T, i32>) {
            if (value == std::numeric_limits<i32>::min()) {
                return "(-2147483647 - 1)";
            }
            return std::to_string(value);
        } else if constexpr (std::same_as<T, u32>) {
            return std::to_string(value) + 'u';
        } else {
            return float_literal(value);
        }
    }, payload.value);
}

[[nodiscard]] bool is_integral_or_boolean(const TypeDescription& type) noexcept
{
    return (type.kind == TypeKind::scalar || type.kind == TypeKind::vector)
           && type.scalar != ScalarKind::f32;
}

[[nodiscard]] std::string interpolation_qualifier(
    const ModuleIR& module,
    const InterfaceField& field)
{
    switch (field.interpolation) {
    case shader::Interpolation::smooth: return "smooth ";
    case shader::Interpolation::flat: return "flat ";
    case shader::Interpolation::no_perspective: return "noperspective ";
    case shader::Interpolation::none:
        if (is_integral_or_boolean(module.types[field.type])) {
            return "flat ";
        }
        return {};
    }
    return {};
}

[[nodiscard]] std::optional<std::string_view> builtin_name(Builtin builtin) noexcept
{
    switch (builtin) {
    case Builtin::clip_position: return "gl_Position";
    case Builtin::fragment_depth: return "gl_FragDepth";
    case Builtin::fragment_coordinate: return "gl_FragCoord";
    case Builtin::front_facing: return "gl_FrontFacing";
    case Builtin::vertex_index: return "gl_VertexID";
    case Builtin::instance_index: return "gl_InstanceID";
    case Builtin::none:
    case Builtin::color:
        return std::nullopt;
    }
    return std::nullopt;
}

class StageEmitter {
public:
    explicit StageEmitter(
        const shader::ShaderStage& stage,
        StageEmissionPlan plan = {})
        : module_(stage.ir()), plan_(plan)
    {
        artifact_.stage = stage.kind();
        artifact_.debug_name = module_.debug_name;
    }

    [[nodiscard]] shader::Result<StageSource> run()
    {
        if (auto validity = shader::validate(module_); !validity) {
            auto error = std::move(validity.error());
            error.generated_source = writer_.source();
            return std::unexpected(std::move(error));
        }
        if (auto valid = validate_interface(); !valid) {
            return std::unexpected(std::move(valid.error()));
        }

        // An observed expression is deliberately retained in the neutral IR
        // so a diagnostic emission can select it later. It must not make the
        // ordinary shader execute extra work, though. Recompute executable
        // liveness from effectful roots instead of blindly emitting every IR
        // operation retained by the optimizer.
        mark_execution_live();
        mark_emitted_types();
        if (auto metadata = collect_interface_metadata(); !metadata) {
            return std::unexpected(std::move(metadata.error()));
        }

        writer_.line("#version 460 core");
        writer_.line();

        if (auto records = emit_record_declarations(); !records) {
            return std::unexpected(std::move(records.error()));
        }
        if (auto interfaces = emit_interface_declarations(); !interfaces) {
            return std::unexpected(std::move(interfaces.error()));
        }

        writer_.line("void main()");
        writer_.line("{");
        const auto& root = module_.regions[module_.root_region.value];
        for (const auto id : root.operations) {
            if (!id.valid() || id.value >= execution_live_.size()
                || !execution_live_[id.value]) {
                continue;
            }
            if (plan_.analysis != nullptr
                && module_.operations[id.value].opcode == OpCode::return_) {
                emit_analysis_epilogue();
            }
            if (plan_.selected_observation != nullptr
                && module_.operations[id.value].opcode == OpCode::return_) {
                if (auto observed = emit_observation_epilogue(); !observed) {
                    return std::unexpected(std::move(observed.error()));
                }
            }
            if (auto operation = emit_operation(id); !operation) {
                return std::unexpected(std::move(operation.error()));
            }
        }
        writer_.line("}");

        artifact_.source = std::move(writer_).take();
        return std::move(artifact_);
    }

private:
    [[nodiscard]] static constexpr bool is_execution_root(
        const Operation& operation) noexcept
    {
        // Result-less operations are statements even if malformed/test IR
        // happens to label them pure; keep them visible to validation and
        // lowering diagnostics. Observation-only calculations always have a
        // result and are therefore excluded unless reached from a statement.
        return !operation.result.valid()
            || (operation.effect != Effect::pure
                && operation.effect != Effect::input_read
                && operation.effect != Effect::parameter_read
                && operation.effect != Effect::texture_read
                // This opcode is explicitly read-only. Future mutable storage
                // operations must not inherit its dead-read treatment.
                && !(operation.opcode == OpCode::matrix_buffer_read
                     && operation.effect == Effect::storage_read));
    }

    void mark_operation_live(OperationId id)
    {
        if (!id.valid() || id.value >= module_.operations.size()
            || execution_live_[id.value]) {
            return;
        }
        execution_live_[id.value] = true;

        const auto& operation = module_.operations[id.value];
        for (const auto operand : operation.operands) {
            if (!operand.valid() || operand.value >= module_.values.size()) {
                continue;
            }
            mark_operation_live(module_.values[operand.value].producer);
        }
        // Structured control flow is not emitted in this milestone, but
        // include its regions in liveness now so adding that lowering cannot
        // silently drop their yielded/effectful work.
        for (const auto region_id : operation.regions) {
            if (!region_id.valid() || region_id.value >= module_.regions.size()) {
                continue;
            }
            for (const auto nested : module_.regions[region_id.value].operations) {
                mark_operation_live(nested);
            }
        }
    }

    void mark_execution_live()
    {
        execution_live_.assign(module_.operations.size(), false);
        for (u32 index = 0; index < module_.operations.size(); ++index) {
            if (is_execution_root(module_.operations[index])) {
                mark_operation_live(OperationId{index});
            }
        }
        if (plan_.selected_observation != nullptr) {
            const auto value = plan_.selected_observation->value;
            if (module_.owns(value) && value.value < module_.values.size()) {
                mark_operation_live(module_.values[value.value].producer);
            }
        }

        execution_value_names_.assign(module_.values.size(), std::nullopt);
        u32 next_name = 0;
        for (u32 index = 0; index < module_.operations.size(); ++index) {
            if (!execution_live_[index]) {
                continue;
            }
            const auto result = module_.operations[index].result;
            if (result.valid() && result.value < execution_value_names_.size()) {
                execution_value_names_[result.value] = next_name++;
            }
        }
    }

    void mark_emitted_type(TypeId type)
    {
        if (!type.valid() || type.value >= module_.types.size()
            || emitted_type_ids_[type.value]) {
            return;
        }
        // A temporary value marks the type while recursively walking records;
        // the stable compact ids are assigned after the complete slice is known.
        emitted_type_ids_[type.value] = 0;
        const auto& description = module_.types[type];
        if (description.kind == TypeKind::vector
            || description.kind == TypeKind::matrix) {
            for (u32 index = 0; index < module_.types.size(); ++index) {
                const auto candidate = TypeId{index};
                const auto& scalar = module_.types[candidate];
                if (scalar.kind == TypeKind::scalar
                    && scalar.scalar == description.scalar) {
                    mark_emitted_type(candidate);
                    break;
                }
            }
        }
        for (const auto& member : description.members) {
            mark_emitted_type(member.type);
        }
    }

    void mark_emitted_types()
    {
        emitted_type_ids_.assign(module_.types.size(), std::nullopt);
        if (module_.types.size() != 0) {
            // TypeTable always contains poison at zero. Count that permanent
            // entry so compact names match a module that never registered the
            // types belonging exclusively to inert observations.
            mark_emitted_type(module_.types.poison());
        }
        for (const auto& field : module_.inputs) {
            // Non-builtin inputs are part of the linked interface and are
            // always declared. Builtins are implicit GLSL values and only
            // belong to the emitted slice when a live input operation reads
            // them (covered by the operation walk below).
            if (field.builtin == Builtin::none) {
                mark_emitted_type(field.type);
            }
        }
        for (const auto& field : module_.outputs) {
            mark_emitted_type(field.type);
        }
        for (const auto& parameter : module_.parameters) {
            mark_emitted_type(parameter.type);
        }
        for (u32 index = 0; index < module_.operations.size(); ++index) {
            if (!execution_live_[index]) {
                continue;
            }
            const auto result = module_.operations[index].result;
            if (result.valid() && result.value < module_.values.size()) {
                mark_emitted_type(module_.values[result.value].type);
            }
        }

        u32 next_id = 0;
        for (auto& id : emitted_type_ids_) {
            if (id) {
                id = next_id++;
            }
        }
    }

    [[nodiscard]] shader::Result<std::string> emitted_glsl_type(TypeId type)
    {
        return glsl_type(
            module_, type, writer_.source(), &emitted_type_ids_);
    }

    [[nodiscard]] std::string emitted_value_name(ValueId value) const
    {
        return "v" + std::to_string(*execution_value_names_[value.value]);
    }

    [[nodiscard]] shader::Result<void> collect_interface_metadata()
    {
        auto collect = [&](const std::vector<InterfaceField>& fields,
                           std::vector<InterfaceVariable>& destination)
            -> shader::Result<void> {
            destination.reserve(fields.size());
            for (const auto& field : fields) {
                auto type = emitted_glsl_type(field.type);
                if (!type) {
                    return std::unexpected(std::move(type.error()));
                }
                destination.push_back(InterfaceVariable{
                    .semantic_type = field.semantic_type,
                    .semantic_name = field.semantic_name,
                    .glsl_type = std::move(*type),
                    .location = field.location,
                    .interpolation = field.interpolation,
                    .builtin = field.builtin,
                    .builtin_index = field.builtin_index,
                });
            }
            return {};
        };

        if (auto inputs = collect(module_.inputs, artifact_.interface.inputs); !inputs) {
            return inputs;
        }
        if (auto outputs = collect(module_.outputs, artifact_.interface.outputs); !outputs) {
            return outputs;
        }

        if (plan_.analysis != nullptr) {
            const auto item_id = InterfaceVariable{
                .semantic_type = typeid(void),
                .semantic_name = "vng.analysis.item_id",
                .glsl_type = "uint",
                .location = plan_.analysis->item_id_varying_location,
                .interpolation = shader::Interpolation::flat,
                .builtin = shader::Builtin::none,
                .builtin_index = 0,
            };
            if (module_.stage == StageKind::vertex) {
                artifact_.interface.outputs.push_back(item_id);
            } else {
                artifact_.interface.inputs.push_back(item_id);
                artifact_.interface.outputs.push_back(InterfaceVariable{
                    .semantic_type = typeid(void),
                    .semantic_name = "vng.analysis.surface_key",
                    .glsl_type = "uvec2",
                    .location = plan_.analysis->surface_key_location,
                    .interpolation = shader::Interpolation::none,
                    .builtin = shader::Builtin::none,
                    .builtin_index = 0,
                });
            }
        }

        if (plan_.observation != nullptr) {
            artifact_.interface.outputs.push_back(InterfaceVariable{
                .semantic_type = plan_.observation->semantic_type,
                .semantic_name = plan_.observation->semantic_name,
                .glsl_type = plan_.observation->attachment_glsl_type,
                .location = plan_.observation->attachment_location,
                .interpolation = shader::Interpolation::none,
                .builtin = shader::Builtin::none,
                .builtin_index = 0,
            });
        }
        return {};
    }

    [[nodiscard]] shader::Result<void> validate_interface()
    {
        auto validate_fields = [&](const std::vector<InterfaceField>& fields,
                                   bool outputs) -> shader::Result<void> {
            for (const auto& field : fields) {
                const bool declared_variable = field.builtin == Builtin::none
                    || (outputs && field.builtin == Builtin::color);
                if (declared_variable && !field.location) {
                    return std::unexpected(diagnostic(
                        DiagnosticCode::interface_mismatch,
                        "GLSL emission requires linked locations for every non-builtin interface field",
                        writer_.source()));
                }
                const bool is_varying = (module_.stage == StageKind::vertex && outputs)
                    || (module_.stage == StageKind::fragment && !outputs);
                if (is_varying
                    && (field.interpolation == shader::Interpolation::smooth
                     || field.interpolation == shader::Interpolation::no_perspective)
                    && is_integral_or_boolean(module_.types[field.type])) {
                    return std::unexpected(diagnostic(
                        DiagnosticCode::interface_mismatch,
                        "integer and boolean varyings must use flat interpolation",
                        writer_.source()));
                }
            }
            return {};
        };

        if (auto result = validate_fields(module_.inputs, false); !result) {
            return result;
        }
        if (auto result = validate_fields(module_.outputs, true); !result) {
            return result;
        }
        for (const auto& parameter : module_.parameters) {
            if (!parameter.location) {
                return std::unexpected(diagnostic(
                    DiagnosticCode::interface_mismatch,
                    "GLSL emission requires linked locations for every shader parameter",
                    writer_.source()));
            }
        }
        return {};
    }

    [[nodiscard]] shader::Result<void> emit_record_declarations()
    {
        for (u32 index = 0; index < module_.types.size(); ++index) {
            const auto id = TypeId{index};
            const auto& type = module_.types[id];
            if (type.kind != TypeKind::record
                || index >= emitted_type_ids_.size()
                || !emitted_type_ids_[index]) {
                continue;
            }

            writer_.line("struct " + record_name(
                TypeId{*emitted_type_ids_[index]}));
            writer_.line("{");
            for (std::size_t member = 0; member < type.members.size(); ++member) {
                auto member_type = emitted_glsl_type(type.members[member].type);
                if (!member_type) {
                    return std::unexpected(std::move(member_type.error()));
                }
                writer_.line("    " + *member_type + ' ' + member_name(member) + ";");
            }
            writer_.line("};");
            writer_.line();
        }
        return {};
    }

    [[nodiscard]] shader::Result<void> emit_parameter_declaration(
        TypeId id, const std::string& name, u32& location)
    {
        const auto& description = module_.types[id];
        if (description.kind == TypeKind::record) {
            for (std::size_t index = 0; index < description.members.size(); ++index) {
                if (auto result = emit_parameter_declaration(
                        description.members[index].type,
                        name + "_" + member_name(index), location); !result) return result;
            }
            return {};
        }
        auto type = emitted_glsl_type(id);
        if (!type) return std::unexpected(std::move(type.error()));
        writer_.line("layout(location = " + std::to_string(location++)
            + ") uniform " + *type + ' ' + name + ";");
        return {};
    }

    [[nodiscard]] shader::Result<std::string> parameter_expression(
        TypeId id, const std::string& name)
    {
        const auto& description = module_.types[id];
        if (description.kind != TypeKind::record) return name;
        auto type = emitted_glsl_type(id);
        if (!type) return std::unexpected(std::move(type.error()));
        auto expression = *type + "(";
        for (std::size_t index = 0; index < description.members.size(); ++index) {
            auto member = parameter_expression(description.members[index].type,
                name + "_" + member_name(index));
            if (!member) return std::unexpected(std::move(member.error()));
            if (index != 0) expression += ", ";
            expression += *member;
        }
        return expression + ")";
    }

    [[nodiscard]] shader::Result<void> emit_interface_declarations()
    {
        bool emitted = false;
        for (std::size_t index = 0; index < module_.inputs.size(); ++index) {
            const auto& field = module_.inputs[index];
            if (field.builtin != Builtin::none) {
                continue;
            }
            auto type = emitted_glsl_type(field.type);
            if (!type) {
                return std::unexpected(std::move(type.error()));
            }
            std::string qualifier;
            if (module_.stage == StageKind::fragment) {
                qualifier = interpolation_qualifier(module_, field);
            }
            writer_.line("layout(location = " + std::to_string(*field.location)
                         + ") " + qualifier + "in " + *type + ' '
                         + input_name(index) + ";");
            emitted = true;
        }

        for (std::size_t index = 0; index < module_.outputs.size(); ++index) {
            const auto& field = module_.outputs[index];
            if (field.builtin != Builtin::none && field.builtin != Builtin::color) {
                continue;
            }
            auto type = emitted_glsl_type(field.type);
            if (!type) {
                return std::unexpected(std::move(type.error()));
            }
            std::string qualifier;
            if (module_.stage == StageKind::vertex) {
                qualifier = interpolation_qualifier(module_, field);
            }
            writer_.line("layout(location = " + std::to_string(*field.location)
                         + ") " + qualifier + "out " + *type + ' '
                         + output_name(index) + ";");
            emitted = true;
        }

        for (const auto& parameter : module_.parameters) {
            auto location = *parameter.location;
            if (auto declared = emit_parameter_declaration(
                    parameter.type, parameter_uniform_name(parameter), location);
                !declared) return declared;
            emitted = true;
        }

        for (u32 index = 0; index < module_.operations.size(); ++index) {
            const auto& operation = module_.operations[index];
            if (execution_live_[index] && (operation.opcode == OpCode::texture_sample ||
                                           operation.opcode == OpCode::texture_sample_lod)) {
                artifact_.texture_bindings.push_back(
                    std::get<shader::TextureSamplePayload>(operation.payload).binding);
            }
            if (execution_live_[index] && operation.opcode == OpCode::matrix_buffer_read) {
                artifact_.matrix_buffer_bindings.push_back(
                    std::get<shader::MatrixBufferPayload>(operation.payload).binding);
            }
        }
        std::ranges::sort(artifact_.texture_bindings);
        auto& bindings = artifact_.texture_bindings;
        bindings.erase(std::unique(bindings.begin(), bindings.end()), bindings.end());
        for (const auto binding : bindings) {
            writer_.line("layout(binding = " + std::to_string(binding)
                         + ") uniform sampler2D " + texture_name(binding) + ";");
            emitted = true;
        }

        auto& matrix_bindings = artifact_.matrix_buffer_bindings;
        std::ranges::sort(matrix_bindings);
        matrix_bindings.erase(std::unique(matrix_bindings.begin(), matrix_bindings.end()),
                              matrix_bindings.end());
        for (const auto binding : matrix_bindings) {
            writer_.line("layout(std430, binding = " + std::to_string(binding)
                         + ") readonly buffer vng_matrices_" + std::to_string(binding));
            writer_.line("{");
            writer_.line("    mat4 " + matrix_buffer_name(binding) + "[];");
            writer_.line("};");
            emitted = true;
        }

        if (plan_.analysis != nullptr) {
            if (module_.stage == StageKind::vertex) {
                writer_.line(
                    "layout(location = "
                    + std::to_string(plan_.analysis->first_item_uniform_location)
                    + ") uniform uint " + plan_.analysis->first_item_uniform_name + ";");
                writer_.line(
                    "layout(location = "
                    + std::to_string(plan_.analysis->item_id_varying_location)
                    + ") flat out uint " + std::string(analysis_item_id_name) + ";");
            } else {
                writer_.line(
                    "layout(location = "
                    + std::to_string(plan_.analysis->item_id_varying_location)
                    + ") flat in uint " + std::string(analysis_item_id_name) + ";");
                writer_.line(
                    "layout(location = "
                    + std::to_string(plan_.analysis->surface_key_location)
                    + ") out uvec2 " + std::string(analysis_surface_key_name) + ";");
            }
            emitted = true;
        }
        if (plan_.observation != nullptr) {
            writer_.line(
                "layout(location = "
                + std::to_string(plan_.observation->attachment_location)
                + ") out " + plan_.observation->attachment_glsl_type + ' '
                + plan_.observation->output_name + ";");
            emitted = true;
        }
        if (emitted) {
            writer_.line();
        }
        return {};
    }

    void emit_analysis_epilogue()
    {
        if (module_.stage == StageKind::vertex) {
            writer_.line(
                "    " + std::string(analysis_item_id_name) + " = "
                + plan_.analysis->first_item_uniform_name
                + " + uint(gl_InstanceID);");
        } else {
            writer_.line(
                "    " + std::string(analysis_surface_key_name)
                + " = uvec2(" + std::string(analysis_item_id_name)
                + ", uint(gl_PrimitiveID));");
        }
    }

    [[nodiscard]] shader::Result<void> emit_observation_epilogue()
    {
        const auto value = plan_.selected_observation->value;
        if (!module_.owns(value) || value.value >= module_.values.size()) {
            return std::unexpected(diagnostic(
                DiagnosticCode::invalid_ir,
                "selected GLSL observation refers to an invalid value",
                writer_.source()));
        }
        const auto producer = module_.values[value.value].producer;
        if (!producer.valid() || producer.value >= module_.operations.size()) {
            return std::unexpected(diagnostic(
                DiagnosticCode::invalid_ir,
                "selected GLSL observation has no valid producer",
                writer_.source()));
        }
        if (value.value >= execution_value_names_.size()
            || !execution_value_names_[value.value]) {
            return std::unexpected(diagnostic(
                DiagnosticCode::invalid_ir,
                "selected GLSL observation is outside the executable slice",
                writer_.source(),
                &module_.operations[producer.value]));
        }

        const auto generated_line = writer_.next_line();
        const auto zero = [&]() -> std::string_view {
            switch (plan_.observation->scalar_kind) {
            case ScalarKind::f32: return "0.0";
            case ScalarKind::i32: return "0";
            case ScalarKind::u32: return "0u";
            case ScalarKind::boolean: break;
            }
            return "0";
        }();
        std::string padded = plan_.observation->attachment_glsl_type + '('
            + emitted_value_name(value);
        for (u32 component = plan_.observation->component_count;
             component < 4;
             ++component) {
            padded += ", ";
            padded += zero;
        }
        padded += ')';
        writer_.line(
            "    " + plan_.observation->output_name + " = " + padded + ";");
        artifact_.source_map.push_back(SourceMapEntry{
            .generated_line = generated_line,
            .operation = producer,
            .origin = plan_.selected_observation->origin,
        });
        return {};
    }

    [[nodiscard]] shader::Result<std::string> operand(ValueId value, const Operation& operation)
    {
        if (!value.valid() || value.value >= module_.values.size()) {
            return std::unexpected(diagnostic(
                DiagnosticCode::invalid_ir,
                "GLSL operation references an invalid value",
                writer_.source(),
                &operation));
        }
        if (value.value >= execution_value_names_.size()
            || !execution_value_names_[value.value]) {
            return std::unexpected(diagnostic(
                DiagnosticCode::invalid_ir,
                "GLSL operation references a value outside the executable slice",
                writer_.source(),
                &operation));
        }
        return emitted_value_name(value);
    }

    [[nodiscard]] shader::Result<std::string> unary_expression(
        const Operation& operation,
        std::string_view symbol)
    {
        if (operation.operands.size() != 1) {
            return bad_arity(operation, 1);
        }
        auto value = operand(operation.operands[0], operation);
        if (!value) {
            return value;
        }
        return '(' + std::string(symbol) + *value + ')';
    }

    [[nodiscard]] shader::Result<std::string> binary_expression(
        const Operation& operation,
        std::string_view symbol)
    {
        if (operation.operands.size() != 2) {
            return bad_arity(operation, 2);
        }
        auto left = operand(operation.operands[0], operation);
        if (!left) return left;
        auto right = operand(operation.operands[1], operation);
        if (!right) return right;
        return '(' + *left + ' ' + std::string(symbol) + ' ' + *right + ')';
    }

    [[nodiscard]] shader::Result<std::string> call_expression(
        const Operation& operation,
        std::string_view function)
    {
        std::string result(function);
        result.push_back('(');
        for (std::size_t index = 0; index < operation.operands.size(); ++index) {
            auto value = operand(operation.operands[index], operation);
            if (!value) return value;
            if (index != 0) result += ", ";
            result += *value;
        }
        result.push_back(')');
        return result;
    }

    [[nodiscard]] shader::Result<std::string> bad_arity(
        const Operation& operation,
        std::size_t expected)
    {
        return std::unexpected(diagnostic(
            DiagnosticCode::invalid_ir,
            "GLSL " + std::string(shader::opcode_name(operation.opcode))
                + " expects " + std::to_string(expected) + " operands",
            writer_.source(),
            &operation));
    }

    [[nodiscard]] shader::Result<std::string> comparison_expression(
        const Operation& operation,
        std::string_view scalar_symbol,
        std::string_view vector_function)
    {
        if (operation.operands.size() != 2) {
            return bad_arity(operation, 2);
        }
        const auto operand_type = module_.values[operation.operands[0].value].type;
        if (module_.types[operand_type].kind == TypeKind::vector) {
            return call_expression(operation, vector_function);
        }
        return binary_expression(operation, scalar_symbol);
    }

    [[nodiscard]] shader::Result<std::string> expression(const Operation& operation)
    {
        switch (operation.opcode) {
        case OpCode::constant: {
            const auto* payload = std::get_if<shader::ConstantPayload>(&operation.payload);
            if (payload == nullptr || !operation.operands.empty()) {
                return std::unexpected(diagnostic(
                    DiagnosticCode::invalid_ir,
                    "malformed constant operation",
                    writer_.source(),
                    &operation));
            }
            return constant_literal(*payload);
        }
        case OpCode::input: {
            const auto* payload = std::get_if<shader::InterfacePayload>(&operation.payload);
            if (payload == nullptr || payload->field >= module_.inputs.size()
                || !operation.operands.empty()) {
                return std::unexpected(diagnostic(
                    DiagnosticCode::invalid_ir,
                    "malformed shader input operation",
                    writer_.source(),
                    &operation));
            }
            const auto& field = module_.inputs[payload->field];
            if (const auto builtin = builtin_name(field.builtin)) {
                return std::string(*builtin);
            }
            if (field.builtin != Builtin::none) {
                return std::unexpected(diagnostic(
                    DiagnosticCode::unsupported_operation,
                    "shader input uses an unsupported GLSL builtin",
                    writer_.source(),
                    &operation));
            }
            return input_name(payload->field);
        }
        case OpCode::parameter: {
            const auto* payload = std::get_if<shader::ParameterPayload>(
                &operation.payload);
            if (payload == nullptr ||
                payload->parameter >= module_.parameters.size() ||
                !operation.operands.empty()) {
                return std::unexpected(diagnostic(
                    DiagnosticCode::invalid_ir,
                    "malformed shader parameter operation",
                    writer_.source(),
                    &operation));
            }
            const auto& parameter = module_.parameters[payload->parameter];
            return parameter_expression(parameter.type, parameter_uniform_name(parameter));
        }
        case OpCode::texture_sample: {
            if (operation.operands.size() != 1) return bad_arity(operation, 1);
            const auto& sample = std::get<shader::TextureSamplePayload>(operation.payload);
            auto coordinates = operand(operation.operands[0], operation);
            if (!coordinates) return coordinates;
            return "texture(" + texture_name(sample.binding) + ", " + *coordinates + ")";
        }
        case OpCode::texture_sample_lod: {
            if (operation.operands.size() != 2) return bad_arity(operation, 2);
            const auto& sample = std::get<shader::TextureSamplePayload>(operation.payload);
            auto coordinates = operand(operation.operands[0], operation);
            if (!coordinates) return coordinates;
            auto level = operand(operation.operands[1], operation);
            if (!level) return level;
            return "textureLod(" + texture_name(sample.binding) + ", " + *coordinates + ", " + *level + ")";
        }
        case OpCode::matrix_buffer_read: {
            if (operation.operands.size() != 1) return bad_arity(operation, 1);
            const auto& buffer = std::get<shader::MatrixBufferPayload>(operation.payload);
            auto index = operand(operation.operands[0], operation);
            if (!index) return index;
            return matrix_buffer_name(buffer.binding) + "[" + *index + "]";
        }
        case OpCode::construct:
        case OpCode::cast: {
            auto type = emitted_glsl_type(
                module_.values[operation.result.value].type);
            if (!type) return type;
            return call_expression(operation, *type);
        }
        case OpCode::extract_field: {
            if (operation.operands.size() != 1) return bad_arity(operation, 1);
            const auto* payload = std::get_if<shader::FieldPayload>(&operation.payload);
            if (payload == nullptr) {
                return std::unexpected(diagnostic(
                    DiagnosticCode::invalid_ir,
                    "malformed record extraction operation",
                    writer_.source(),
                    &operation));
            }
            auto record = operand(operation.operands[0], operation);
            if (!record) return record;
            const auto record_type = module_.values[operation.operands[0].value].type;
            const auto& description = module_.types[record_type];
            if (description.kind != TypeKind::record || payload->field >= description.members.size()) {
                return std::unexpected(diagnostic(
                    DiagnosticCode::invalid_ir,
                    "record extraction refers to a missing field",
                    writer_.source(),
                    &operation));
            }
            return *record + '.' + member_name(payload->field);
        }
        case OpCode::swizzle: {
            if (operation.operands.size() != 1) return bad_arity(operation, 1);
            const auto* payload = std::get_if<shader::SwizzlePayload>(&operation.payload);
            if (payload == nullptr || payload->components.empty() || payload->components.size() > 4) {
                return std::unexpected(diagnostic(
                    DiagnosticCode::invalid_ir,
                    "malformed swizzle operation",
                    writer_.source(),
                    &operation));
            }
            auto value = operand(operation.operands[0], operation);
            if (!value) return value;
            constexpr std::string_view names = "xyzw";
            std::string result = *value + '.';
            for (const auto component : payload->components) {
                if (component >= names.size()) {
                    return std::unexpected(diagnostic(
                        DiagnosticCode::invalid_ir,
                        "swizzle component is outside xyzw",
                        writer_.source(),
                        &operation));
                }
                result.push_back(names[component]);
            }
            return result;
        }
        case OpCode::negate: return unary_expression(operation, "-");
        case OpCode::logical_not: return unary_expression(operation, "!");
        case OpCode::bit_not: return unary_expression(operation, "~");
        case OpCode::add: return binary_expression(operation, "+");
        case OpCode::subtract: return binary_expression(operation, "-");
        case OpCode::multiply: return binary_expression(operation, "*");
        case OpCode::divide: return binary_expression(operation, "/");
        case OpCode::remainder: return binary_expression(operation, "%");
        case OpCode::equal: return comparison_expression(operation, "==", "equal");
        case OpCode::not_equal: return comparison_expression(operation, "!=", "notEqual");
        case OpCode::less: return comparison_expression(operation, "<", "lessThan");
        case OpCode::less_equal: return comparison_expression(operation, "<=", "lessThanEqual");
        case OpCode::greater: return comparison_expression(operation, ">", "greaterThan");
        case OpCode::greater_equal: return comparison_expression(operation, ">=", "greaterThanEqual");
        case OpCode::bit_and: return binary_expression(operation, "&");
        case OpCode::bit_or: return binary_expression(operation, "|");
        case OpCode::bit_xor: return binary_expression(operation, "^");
        case OpCode::shift_left: return binary_expression(operation, "<<");
        case OpCode::shift_right: return binary_expression(operation, ">>");
        case OpCode::dot: return call_expression(operation, "dot");
        case OpCode::cross: return call_expression(operation, "cross");
        case OpCode::normalize: return call_expression(operation, "normalize");
        case OpCode::minimum: return call_expression(operation, "min");
        case OpCode::maximum: return call_expression(operation, "max");
        case OpCode::clamp: return call_expression(operation, "clamp");
        case OpCode::mix: return call_expression(operation, "mix");
        case OpCode::square_root: return call_expression(operation, "sqrt");
        case OpCode::sine: return call_expression(operation, "sin");
        case OpCode::cosine: return call_expression(operation, "cos");
        case OpCode::absolute: return call_expression(operation, "abs");
        case OpCode::floor: return call_expression(operation, "floor");
        case OpCode::fract: return call_expression(operation, "fract");
        case OpCode::exponential: return call_expression(operation, "exp");
        case OpCode::power: return call_expression(operation, "pow");
        case OpCode::all: return call_expression(operation, "all");
        case OpCode::any: return call_expression(operation, "any");
        case OpCode::select: {
            if (operation.operands.size() != 3) return bad_arity(operation, 3);
            auto condition = operand(operation.operands[0], operation);
            if (!condition) return condition;
            auto when_true = operand(operation.operands[1], operation);
            if (!when_true) return when_true;
            auto when_false = operand(operation.operands[2], operation);
            if (!when_false) return when_false;
            const auto condition_type = module_.values[operation.operands[0].value].type;
            if (module_.types[condition_type].kind == TypeKind::vector) {
                return "mix(" + *when_false + ", " + *when_true + ", " + *condition + ')';
            }
            return '(' + *condition + " ? " + *when_true + " : " + *when_false + ')';
        }
        case OpCode::poison:
            return std::unexpected(diagnostic(
                DiagnosticCode::invalid_expression,
                "cannot lower a poison expression to GLSL",
                writer_.source(),
                &operation));
        case OpCode::stage_output:
        case OpCode::return_:
        case OpCode::if_region:
        case OpCode::loop_region:
        case OpCode::yield:
            return std::unexpected(diagnostic(
                DiagnosticCode::unsupported_operation,
                "operation is not a GLSL value expression: "
                    + std::string(shader::opcode_name(operation.opcode)),
                writer_.source(),
                &operation));
        }
        return std::unexpected(diagnostic(
            DiagnosticCode::unsupported_operation,
            "unknown operation during GLSL lowering",
            writer_.source(),
            &operation));
    }

    [[nodiscard]] shader::Result<void> emit_operation(OperationId id)
    {
        if (!id.valid() || id.value >= module_.operations.size()) {
            return std::unexpected(diagnostic(
                DiagnosticCode::invalid_ir,
                "root region references an invalid operation",
                writer_.source()));
        }
        const auto& operation = module_.operations[id.value];
        const auto generated_line = writer_.next_line();

        std::string line;
        if (operation.result.valid()) {
            if (operation.result.value >= module_.values.size()) {
                return std::unexpected(diagnostic(
                    DiagnosticCode::invalid_ir,
                    "operation result refers to an invalid value",
                    writer_.source(),
                    &operation));
            }
            auto type = emitted_glsl_type(
                module_.values[operation.result.value].type);
            if (!type) return std::unexpected(std::move(type.error()));
            auto value = expression(operation);
            if (!value) return std::unexpected(std::move(value.error()));
            line = "    " + *type + ' ' + emitted_value_name(operation.result)
                + " = " + *value + ';';
        } else if (operation.opcode == OpCode::stage_output) {
            const auto* payload = std::get_if<shader::InterfacePayload>(&operation.payload);
            if (payload == nullptr || payload->field >= module_.outputs.size()
                || operation.operands.size() != 1) {
                return std::unexpected(diagnostic(
                    DiagnosticCode::invalid_ir,
                    "malformed stage output operation",
                    writer_.source(),
                    &operation));
            }
            const auto& field = module_.outputs[payload->field];
            std::string destination;
            if (const auto builtin = builtin_name(field.builtin)) {
                destination = *builtin;
            } else if (field.builtin == Builtin::none || field.builtin == Builtin::color) {
                destination = output_name(payload->field);
            } else {
                return std::unexpected(diagnostic(
                    DiagnosticCode::unsupported_operation,
                    "shader output uses an unsupported GLSL builtin",
                    writer_.source(),
                    &operation));
            }
            auto value = operand(operation.operands.front(), operation);
            if (!value) return std::unexpected(std::move(value.error()));
            line = "    " + destination + " = " + *value + ';';
        } else if (operation.opcode == OpCode::return_) {
            if (!operation.operands.empty()) {
                return std::unexpected(diagnostic(
                    DiagnosticCode::invalid_ir,
                    "stage return operation cannot carry a value",
                    writer_.source(),
                    &operation));
            }
            line = "    return;";
        } else {
            return std::unexpected(diagnostic(
                DiagnosticCode::unsupported_operation,
                "statement operation is not supported by the GLSL 4.60 emitter: "
                    + std::string(shader::opcode_name(operation.opcode)),
                writer_.source(),
                &operation));
        }

        writer_.line(line);
        artifact_.source_map.push_back(SourceMapEntry{
            .generated_line = generated_line,
            .operation = id,
            .origin = operation.origin,
        });
        return {};
    }

    const ModuleIR& module_;
    StageEmissionPlan plan_;
    SourceWriter writer_;
    StageSource artifact_;
    std::vector<bool> execution_live_;
    std::vector<std::optional<u32>> execution_value_names_;
    std::vector<std::optional<u32>> emitted_type_ids_;
};

} // namespace

const SourceMapEntry* StageSource::mapping_for_line(
    std::uint32_t generated_line) const noexcept
{
    const auto found = std::lower_bound(
        source_map.begin(),
        source_map.end(),
        generated_line,
        [](const SourceMapEntry& entry, std::uint32_t line) {
            return entry.generated_line < line;
        });
    return found != source_map.end() && found->generated_line == generated_line
        ? &*found
        : nullptr;
}

std::string ProgramSource::dump() const
{
    return dump_source(*this);
}

shader::Result<StageSource> emit(const shader::ShaderStage& stage)
{
    return StageEmitter{stage}.run();
}

shader::Result<ProgramSource> emit(const shader::GraphicsProgram& program)
{
    auto vertex = emit(program.vertex());
    if (!vertex) {
        return std::unexpected(std::move(vertex.error()));
    }
    auto fragment = emit(program.fragment());
    if (!fragment) {
        return std::unexpected(std::move(fragment.error()));
    }
    auto parameters = collect_parameter_metadata(program);
    if (!parameters) {
        return std::unexpected(std::move(parameters.error()));
    }
    auto texture_bindings = collect_texture_bindings(*vertex, *fragment);
    auto matrix_buffer_bindings = collect_matrix_buffer_bindings(*vertex, *fragment);
    return ProgramSource{
        .vertex = std::move(*vertex),
        .fragment = std::move(*fragment),
        .parameters = std::move(*parameters),
        .analysis = std::nullopt,
        .observation = std::nullopt,
        .texture_bindings = std::move(texture_bindings),
        .matrix_buffer_bindings = std::move(matrix_buffer_bindings),
    };
}

shader::Result<ProgramSource> emit(
    const shader::GraphicsProgram& program,
    AnalysisEmission)
{
    auto metadata = make_analysis_metadata(program);
    if (!metadata) {
        return std::unexpected(std::move(metadata.error()));
    }

    const auto plan = StageEmissionPlan{
        .analysis = &*metadata,
        .observation = nullptr,
        .selected_observation = nullptr,
    };
    auto vertex = StageEmitter{program.vertex(), plan}.run();
    if (!vertex) {
        return std::unexpected(std::move(vertex.error()));
    }
    auto fragment = StageEmitter{program.fragment(), plan}.run();
    if (!fragment) {
        return std::unexpected(std::move(fragment.error()));
    }
    auto parameters = collect_parameter_metadata(program);
    if (!parameters) {
        return std::unexpected(std::move(parameters.error()));
    }
    auto texture_bindings = collect_texture_bindings(*vertex, *fragment);
    auto matrix_buffer_bindings = collect_matrix_buffer_bindings(*vertex, *fragment);
    return ProgramSource{
        .vertex = std::move(*vertex),
        .fragment = std::move(*fragment),
        .parameters = std::move(*parameters),
        .analysis = std::move(*metadata),
        .observation = std::nullopt,
        .texture_bindings = std::move(texture_bindings),
        .matrix_buffer_bindings = std::move(matrix_buffer_bindings),
    };
}

shader::Result<ProgramSource> emit(
    const shader::GraphicsProgram& program,
    const ObservationEmission& request)
{
    auto selected = select_observation(program, request);
    if (!selected) {
        return std::unexpected(std::move(selected.error()));
    }

    auto vertex = StageEmitter{program.vertex()}.run();
    if (!vertex) {
        return std::unexpected(std::move(vertex.error()));
    }
    const auto fragment_plan = StageEmissionPlan{
        .analysis = nullptr,
        .observation = &selected->metadata,
        .selected_observation = selected->observation,
    };
    auto fragment = StageEmitter{program.fragment(), fragment_plan}.run();
    if (!fragment) {
        return std::unexpected(std::move(fragment.error()));
    }
    auto parameters = collect_parameter_metadata(program);
    if (!parameters) {
        return std::unexpected(std::move(parameters.error()));
    }

    auto texture_bindings = collect_texture_bindings(*vertex, *fragment);
    auto matrix_buffer_bindings = collect_matrix_buffer_bindings(*vertex, *fragment);
    return ProgramSource{
        .vertex = std::move(*vertex),
        .fragment = std::move(*fragment),
        .parameters = std::move(*parameters),
        .analysis = std::nullopt,
        .observation = std::move(selected->metadata),
        .texture_bindings = std::move(texture_bindings),
        .matrix_buffer_bindings = std::move(matrix_buffer_bindings),
    };
}

std::string dump_source(const ProgramSource& program)
{
    std::string result;
    result.reserve(program.vertex.source.size() + program.fragment.source.size() + 32);
    result += "vertex:\n";
    result += program.vertex.source;
    result += "fragment:\n";
    result += program.fragment.source;
    return result;
}

} // namespace vng::glsl
