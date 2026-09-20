#include <vng/shader/program.hpp>

#include <algorithm>
#include <limits>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

namespace vng::shader {

namespace {

[[nodiscard]] u32 location_width(const ModuleIR& module, const InterfaceField& field)
{
    const auto& type = module.types[field.type];
    return type.kind == TypeKind::matrix ? type.columns : 1;
}

[[nodiscard]] u64 parameter_locations(const TypeTable& types, TypeId id)
{
    const auto& type = types[id];
    if (type.kind != TypeKind::record) return 1;
    u64 width = 0;
    for (const auto& member : type.members) width += parameter_locations(types, member.type);
    return width;
}

[[nodiscard]] Interpolation effective_interpolation(const InterfaceField& field)
{
    if (field.interpolation != Interpolation::none) {
        return field.interpolation;
    }
    return Interpolation::smooth;
}

[[nodiscard]] bool same_logical_type(
    const ModuleIR& left_module,
    const InterfaceField& left,
    const ModuleIR& right_module,
    const InterfaceField& right)
{
    return left.value_type == right.value_type &&
           left_module.types[left.type].canonical_key == right_module.types[right.type].canonical_key;
}

[[nodiscard]] Diagnostic mismatch(std::string message, const InterfaceField& field)
{
    Diagnostic diagnostic{
        .code = DiagnosticCode::interface_mismatch,
        .message = std::move(message),
        .notes = {},
        .origin = {},
        .generated_source = {},
    };
    diagnostic.notes.push_back("semantic: " + field.semantic_name);
    return diagnostic;
}

} // namespace

Result<GraphicsProgram> link(ShaderStage vertex, ShaderStage fragment)
{
    return detail::link_stages(std::move(vertex), std::move(fragment), false);
}

Result<GraphicsProgram> detail::link_stages(ShaderStage vertex, ShaderStage fragment, bool shared)
{
    if (vertex.kind() != StageKind::vertex || fragment.kind() != StageKind::fragment) {
        return std::unexpected(Diagnostic{
            .code = DiagnosticCode::interface_mismatch,
            .message = "a graphics program requires one vertex stage and one fragment stage",
            .notes = {},
            .origin = {},
            .generated_source = {},
        });
    }

    auto& vertex_ir = detail::ShaderStageAccess::ir(vertex);
    auto& fragment_ir = detail::ShaderStageAccess::ir(fragment);

    auto clear_locations = [](ModuleIR& module) {
        for (auto& input : module.inputs) {
            input.location.reset();
        }
        for (auto& output : module.outputs) {
            output.location.reset();
        }
        for (auto& parameter : module.parameters) {
            parameter.location.reset();
        }
    };
    clear_locations(vertex_ir);
    clear_locations(fragment_ir);

    if (auto validity = validate(vertex_ir); !validity) {
        return std::unexpected(std::move(validity.error()));
    }
    if (auto validity = validate(fragment_ir); !validity) {
        return std::unexpected(std::move(validity.error()));
    }

    // Stage factories number their own arguments. Re-linking copies is also
    // safe: normalize from declaration order instead of retaining old slots.
    u32 next_argument = 0;
    for (auto& field : vertex_ir.parameters)
        if (field.kind == ParameterKind::argument) field.argument_index = next_argument++;
    const auto vertex_arguments = next_argument;
    if (shared) next_argument = 0;
    for (auto& field : fragment_ir.parameters)
        if (field.kind == ParameterKind::argument) field.argument_index = next_argument++;
    if (shared && vertex_arguments != next_argument) {
        return std::unexpected(Diagnostic{
            .code = DiagnosticCode::interface_mismatch,
            .message = "Shared stage arguments require identical signatures",
            .notes = {}, .origin = {}, .generated_source = {}});
    }

    const auto clip_positions = std::count_if(
        vertex_ir.outputs.begin(), vertex_ir.outputs.end(),
        [](const InterfaceField& field) { return field.builtin == Builtin::clip_position; });
    if (clip_positions != 1) {
        return std::unexpected(Diagnostic{
            .code = DiagnosticCode::invalid_builtin,
            .message = "a linked vertex stage must produce exactly one clip position",
            .notes = {},
            .origin = {},
            .generated_source = {},
        });
    }

    std::set<u32> color_indices;
    for (const auto& output : fragment_ir.outputs) {
        if (output.builtin == Builtin::color && !color_indices.insert(output.builtin_index).second) {
            return std::unexpected(mismatch("fragment outputs contain a duplicate color index", output));
        }
    }
    if (color_indices.empty()) {
        return std::unexpected(Diagnostic{
            .code = DiagnosticCode::invalid_builtin,
            .message = "a linked fragment stage must produce at least one color output",
            .notes = {},
            .origin = {},
            .generated_source = {},
        });
    }

    struct LinkedParameter final {
        ParameterKind kind;
        const ModuleIR* module;
        TypeId type;
        u32 location;
        u32 argument_index;
        std::type_index argument_type;
    };
    std::vector<LinkedParameter> linked_parameters;
    u32 next_parameter_location = 0;
    auto assign_parameter_locations = [&](ModuleIR& module) -> Result<void> {
        for (auto& parameter : module.parameters) {
            const auto match = std::ranges::find_if(linked_parameters,
                [&](const LinkedParameter& previous) {
                    return previous.kind == parameter.kind
                        && (parameter.kind != ParameterKind::argument
                            || previous.argument_index == parameter.argument_index);
                });
            if (match != linked_parameters.end()) {
                if (module.types[parameter.type].canonical_key !=
                    match->module->types[match->type].canonical_key
                    || (parameter.kind == ParameterKind::argument
                        && parameter.argument_type != match->argument_type)) {
                    return std::unexpected(Diagnostic{
                        .code = DiagnosticCode::interface_mismatch,
                        .message = "linked shader parameter has incompatible stage types",
                        .notes = {
                            "parameter: "
                                + std::string(parameter_kind_name(parameter.kind)),
                        },
                        .origin = {},
                        .generated_source = {},
                    });
                }
                parameter.location = match->location;
                continue;
            }

            const auto width = parameter_locations(module.types, parameter.type);
            if (width > std::numeric_limits<u32>::max() - next_parameter_location) {
                return std::unexpected(Diagnostic{
                    .code = DiagnosticCode::interface_mismatch,
                    .message = "no representable shader parameter location remains",
                    .notes = {},
                    .origin = {},
                    .generated_source = {},
                });
            }
            parameter.location = next_parameter_location;
            linked_parameters.push_back(LinkedParameter{
                .kind = parameter.kind,
                .module = &module,
                .type = parameter.type,
                .location = next_parameter_location,
                .argument_index = parameter.argument_index,
                .argument_type = parameter.argument_type,
            });
            next_parameter_location += static_cast<u32>(width);
        }
        return {};
    };
    if (auto parameters = assign_parameter_locations(vertex_ir); !parameters) {
        return std::unexpected(std::move(parameters.error()));
    }
    if (auto parameters = assign_parameter_locations(fragment_ir); !parameters) {
        return std::unexpected(std::move(parameters.error()));
    }

    u32 vertex_location = 0;
    for (auto& input : vertex_ir.inputs) {
        if (input.builtin != Builtin::none) {
            continue;
        }
        input.location = vertex_location;
        vertex_location += location_width(vertex_ir, input);
    }

    u32 varying_location = 0;
    for (auto& fragment_input : fragment_ir.inputs) {
        if (fragment_input.builtin != Builtin::none) {
            continue;
        }
        const auto match = std::find_if(vertex_ir.outputs.begin(), vertex_ir.outputs.end(), [&](const InterfaceField& output) {
            return output.builtin == Builtin::none && output.semantic_type == fragment_input.semantic_type;
        });
        if (match == vertex_ir.outputs.end()) {
            return std::unexpected(mismatch("fragment input has no matching vertex output", fragment_input));
        }
        if (!same_logical_type(vertex_ir, *match, fragment_ir, fragment_input)) {
            return std::unexpected(mismatch("linked varying has different logical types in the two stages", fragment_input));
        }
        if (effective_interpolation(*match) != effective_interpolation(fragment_input)) {
            return std::unexpected(mismatch("linked varying uses incompatible interpolation", fragment_input));
        }
        match->location = varying_location;
        fragment_input.location = varying_location;
        varying_location += location_width(fragment_ir, fragment_input);
    }

    for (auto& output : vertex_ir.outputs) {
        if (output.builtin == Builtin::none && !output.location) {
            output.location = varying_location;
            varying_location += location_width(vertex_ir, output);
        }
    }

    for (auto& output : fragment_ir.outputs) {
        if (output.builtin == Builtin::color) {
            output.location = output.builtin_index;
        } else if (output.builtin == Builtin::none) {
            return std::unexpected(mismatch("fragment output must use shader::Color<N> or another supported builtin", output));
        }
    }

    return GraphicsProgram{std::move(vertex), std::move(fragment)};
}

std::string GraphicsProgram::dump_interface() const
{
    std::ostringstream stream;
    stream << "vertex:\n" << vertex_.dump_interface();
    stream << "fragment:\n" << fragment_.dump_interface();
    return stream.str();
}

} // namespace vng::shader
