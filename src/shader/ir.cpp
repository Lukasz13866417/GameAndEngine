#include <vng/shader/ir.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>
#include <type_traits>

namespace vng::shader {
namespace {

[[nodiscard]] u64 next_value_owner() noexcept
{
    static std::atomic<u64> next{1};
    const auto owner = next.fetch_add(1, std::memory_order_relaxed);
    if (owner == 0) {
        // Exhaustion would otherwise make a stale ValueId valid again.
        std::terminate();
    }
    return owner;
}

[[nodiscard]] Diagnostic invalid_ir(std::string message)
{
    return Diagnostic{
        .code = DiagnosticCode::invalid_ir,
        .message = std::move(message),
        .notes = {},
        .origin = {},
        .generated_source = {},
    };
}

[[nodiscard]] Diagnostic invalid_operation(const ModuleIR& module, u32 index, std::string message)
{
    auto diagnostic = invalid_ir("operation " + std::to_string(index) + ": " + std::move(message));
    if (index < module.operations.size()) {
        diagnostic.origin = module.operations[index].origin;
    }
    return diagnostic;
}

[[nodiscard]] bool constant_matches(const TypeDescription& type, const ConstantPayload& constant)
{
    if (type.kind != TypeKind::scalar) {
        return false;
    }
    switch (type.scalar) {
    case ScalarKind::boolean: return std::holds_alternative<bool>(constant.value);
    case ScalarKind::i32: return std::holds_alternative<i32>(constant.value);
    case ScalarKind::u32: return std::holds_alternative<u32>(constant.value);
    case ScalarKind::f32: return std::holds_alternative<f32>(constant.value);
    }
    return false;
}

[[nodiscard]] bool is_removable(const Operation& operation)
{
    const auto effect = operation.effect;
    // Only this explicitly read-only resource operation is removable. Do not
    // treat arbitrary storage_read effects as pure across future writes.
    if (operation.opcode == OpCode::matrix_buffer_read && effect == Effect::storage_read) {
        return true;
    }
    return effect == Effect::pure || effect == Effect::input_read ||
           effect == Effect::parameter_read || effect == Effect::texture_read;
}

[[nodiscard]] const ConstantPayload* constant_for(const ModuleIR& module, ValueId value)
{
    if (!module.owns(value) || value.value >= module.values.size()) {
        return nullptr;
    }
    const auto producer = module.values[value.value].producer;
    if (!producer.valid() || producer.value >= module.operations.size()) {
        return nullptr;
    }
    const auto& operation = module.operations[producer.value];
    if (operation.opcode != OpCode::constant) {
        return nullptr;
    }
    return std::get_if<ConstantPayload>(&operation.payload);
}

template<class Function>
std::optional<ConstantPayload> fold_same_type_binary(
    const ConstantPayload& left,
    const ConstantPayload& right,
    Function&& function)
{
    return std::visit(
        [&]<class L, class R>(L l, R r) -> std::optional<ConstantPayload> {
            if constexpr (std::is_same_v<L, R> && !std::is_same_v<L, bool>) {
                return ConstantPayload{function(l, r)};
            }
            return std::nullopt;
        },
        left.value,
        right.value);
}

std::optional<ConstantPayload> fold_arithmetic(
    const ConstantPayload& left,
    const ConstantPayload& right,
    OpCode opcode)
{
    return std::visit(
        [opcode]<class L, class R>(L left_value, R right_value) -> std::optional<ConstantPayload> {
            if constexpr (!std::is_same_v<L, R> || std::is_same_v<L, bool>) {
                return std::nullopt;
            } else if constexpr (std::is_same_v<L, i32>) {
                if (opcode == OpCode::divide) {
                    if (right_value == 0 ||
                        (left_value == std::numeric_limits<i32>::min() && right_value == -1)) {
                        return std::nullopt;
                    }
                    return ConstantPayload{static_cast<i32>(left_value / right_value)};
                }

                const auto wide_left = static_cast<std::int64_t>(left_value);
                const auto wide_right = static_cast<std::int64_t>(right_value);
                std::int64_t wide_result = 0;
                switch (opcode) {
                case OpCode::add: wide_result = wide_left + wide_right; break;
                case OpCode::subtract: wide_result = wide_left - wide_right; break;
                case OpCode::multiply: wide_result = wide_left * wide_right; break;
                default: return std::nullopt;
                }
                if (wide_result < std::numeric_limits<i32>::min() ||
                    wide_result > std::numeric_limits<i32>::max()) {
                    return std::nullopt;
                }
                return ConstantPayload{static_cast<i32>(wide_result)};
            } else if constexpr (std::is_same_v<L, u32>) {
                switch (opcode) {
                case OpCode::add: return ConstantPayload{static_cast<u32>(left_value + right_value)};
                case OpCode::subtract: return ConstantPayload{static_cast<u32>(left_value - right_value)};
                case OpCode::multiply: return ConstantPayload{static_cast<u32>(left_value * right_value)};
                case OpCode::divide:
                    if (right_value == 0) return std::nullopt;
                    return ConstantPayload{static_cast<u32>(left_value / right_value)};
                default: return std::nullopt;
                }
            } else {
                switch (opcode) {
                case OpCode::add: return ConstantPayload{left_value + right_value};
                case OpCode::subtract: return ConstantPayload{left_value - right_value};
                case OpCode::multiply: return ConstantPayload{left_value * right_value};
                case OpCode::divide:
                    if (right_value == 0.0F) return std::nullopt;
                    return ConstantPayload{left_value / right_value};
                default: return std::nullopt;
                }
            }
        },
        left.value,
        right.value);
}

std::optional<ConstantPayload> fold_operation(const ModuleIR& module, const Operation& operation)
{
    if (operation.operands.empty() || operation.operands.size() > 3) {
        return std::nullopt;
    }

    std::array<const ConstantPayload*, 3> constants{};
    for (std::size_t index = 0; index < operation.operands.size(); ++index) {
        constants[index] = constant_for(module, operation.operands[index]);
        if (constants[index] == nullptr) {
            return std::nullopt;
        }
    }

    switch (operation.opcode) {
    case OpCode::negate:
        return std::visit([]<class T>(T value) -> std::optional<ConstantPayload> {
            if constexpr (std::is_same_v<T, i32>) {
                if (value == std::numeric_limits<i32>::min()) {
                    return std::nullopt;
                }
                return ConstantPayload{static_cast<i32>(-value)};
            } else if constexpr (std::is_same_v<T, f32>) {
                return ConstantPayload{-value};
            }
            return std::nullopt;
        }, constants[0]->value);
    case OpCode::logical_not:
        if (const auto* value = std::get_if<bool>(&constants[0]->value)) {
            return ConstantPayload{!(*value)};
        }
        return std::nullopt;
    case OpCode::add:
        return fold_arithmetic(*constants[0], *constants[1], operation.opcode);
    case OpCode::subtract:
        return fold_arithmetic(*constants[0], *constants[1], operation.opcode);
    case OpCode::multiply:
        return fold_arithmetic(*constants[0], *constants[1], operation.opcode);
    case OpCode::divide:
        return fold_arithmetic(*constants[0], *constants[1], operation.opcode);
    case OpCode::equal:
        return ConstantPayload{constants[0]->value == constants[1]->value};
    case OpCode::not_equal:
        return ConstantPayload{constants[0]->value != constants[1]->value};
    case OpCode::less:
        return fold_same_type_binary(*constants[0], *constants[1], [](auto a, auto b) { return a < b; });
    case OpCode::less_equal:
        return fold_same_type_binary(*constants[0], *constants[1], [](auto a, auto b) { return a <= b; });
    case OpCode::greater:
        return fold_same_type_binary(*constants[0], *constants[1], [](auto a, auto b) { return a > b; });
    case OpCode::greater_equal:
        return fold_same_type_binary(*constants[0], *constants[1], [](auto a, auto b) { return a >= b; });
    case OpCode::minimum:
        return fold_same_type_binary(*constants[0], *constants[1], [](auto a, auto b) { return std::min(a, b); });
    case OpCode::maximum:
        return fold_same_type_binary(*constants[0], *constants[1], [](auto a, auto b) { return std::max(a, b); });
    case OpCode::select:
        if (const auto* condition = std::get_if<bool>(&constants[0]->value)) {
            return *constants[*condition ? 1 : 2];
        }
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

void mark_live(const ModuleIR& module, OperationId id, std::vector<bool>& live)
{
    if (!id.valid() || id.value >= live.size() || live[id.value]) {
        return;
    }
    live[id.value] = true;
    for (const auto operand : module.operations[id.value].operands) {
        if (module.owns(operand) && operand.value < module.values.size()) {
            mark_live(module, module.values[operand.value].producer, live);
        }
    }
    for (const auto region : module.operations[id.value].regions) {
        if (region.valid() && region.value < module.regions.size()) {
            for (const auto child : module.regions[region.value].operations) {
                mark_live(module, child, live);
            }
        }
    }
}

void dump_value(std::ostringstream& stream, ValueId value)
{
    if (value.valid()) {
        stream << '%' << value.value;
    } else {
        stream << "<none>";
    }
}

void dump_constant(std::ostringstream& stream, const ConstantPayload& payload)
{
    std::visit([&]<class T>(T value) {
        if constexpr (std::is_same_v<T, bool>) {
            stream << (value ? "true" : "false");
        } else if constexpr (std::is_same_v<T, f32>) {
            stream << std::setprecision(9) << value;
        } else if constexpr (std::is_same_v<T, u32>) {
            stream << value << 'u';
        } else {
            stream << value;
        }
    }, payload.value);
}

} // namespace

void ModuleIR::rebind_values(u64 owner) noexcept
{
    value_owner_ = owner;
    auto rebind = [owner](ValueId& id) {
        if (id.value != std::numeric_limits<u32>::max()) {
            id.owner = owner;
        }
    };
    for (auto& operation : operations) {
        rebind(operation.result);
        for (auto& operand : operation.operands) {
            rebind(operand);
        }
    }
    for (auto& region : regions) {
        for (auto& argument : region.arguments) {
            rebind(argument);
        }
    }
    for (auto& observation : observations) {
        rebind(observation.value);
    }
}

FunctionBuilder::FunctionBuilder(ModuleIR& module) noexcept
    : module_(&module), owner_(next_value_owner())
{
    module_->rebind_values(owner_);
}

SourceOrigin FunctionBuilder::origin(std::source_location location)
{
    return SourceOrigin{
        .file = location.file_name(),
        .function = location.function_name(),
        .line = location.line(),
        .column = location.column(),
    };
}

void FunctionBuilder::append(Operation operation)
{
    const auto id = OperationId{static_cast<u32>(module_->operations.size())};
    if (operation.result.valid()) {
        module_->values[operation.result.value].producer = id;
    }
    module_->operations.push_back(std::move(operation));
    module_->regions[module_->root_region.value].operations.push_back(id);
}

ValueId FunctionBuilder::operation(
    OpCode opcode,
    TypeId result_type,
    std::span<const ValueId> input_operands,
    OperationPayload payload,
    Effect effect,
    std::source_location location)
{
    if (module_->value_owner_ != owner_) {
        fail(Diagnostic{
            .code = DiagnosticCode::mixed_builders,
            .message = "the shader builder no longer owns its module values",
            .notes = {},
            .origin = origin(location),
            .generated_source = {},
        });
        return {};
    }
    if (std::any_of(input_operands.begin(), input_operands.end(),
                    [this](ValueId operand) { return !owns(operand); })) {
        fail(Diagnostic{
            .code = DiagnosticCode::mixed_builders,
            .message = "an operation cannot consume a value owned by another shader builder",
            .notes = {},
            .origin = origin(location),
            .generated_source = {},
        });
        return operation(OpCode::poison, result_type, {}, {}, Effect::pure, location);
    }

    const auto result = ValueId{static_cast<u32>(module_->values.size()), owner_};
    module_->values.push_back(ValueNode{.type = result_type, .producer = {}});
    append(Operation{
        .opcode = opcode,
        .operands = {input_operands.begin(), input_operands.end()},
        .result = result,
        .regions = {},
        .payload = std::move(payload),
        .effect = effect,
        .origin = origin(location),
    });
    return result;
}

void FunctionBuilder::statement(
    OpCode opcode,
    std::span<const ValueId> input_operands,
    OperationPayload payload,
    Effect effect,
    std::source_location location)
{
    if (module_->value_owner_ != owner_ ||
        std::any_of(input_operands.begin(), input_operands.end(),
                    [this](ValueId operand) { return !owns(operand); })) {
        fail(Diagnostic{
            .code = DiagnosticCode::mixed_builders,
            .message = "a statement cannot consume a value owned by another shader builder",
            .notes = {},
            .origin = origin(location),
            .generated_source = {},
        });
        return;
    }
    append(Operation{
        .opcode = opcode,
        .operands = {input_operands.begin(), input_operands.end()},
        .result = {},
        .regions = {},
        .payload = std::move(payload),
        .effect = effect,
        .origin = origin(location),
    });
}

void FunctionBuilder::observe(
    std::type_index semantic_type,
    std::type_index value_type,
    std::string semantic_name,
    TypeId type,
    ValueId value,
    std::source_location location)
{
    if (!owns(value)) {
        fail(Diagnostic{
            .code = DiagnosticCode::mixed_builders,
            .message = "an observation cannot refer to a value owned by another shader builder",
            .notes = {},
            .origin = origin(location),
            .generated_source = {},
        });
        return;
    }
    if (!type.valid() || type.value >= module_->types.size()
        || module_->values[value.value].type != type) {
        fail(Diagnostic{
            .code = DiagnosticCode::invalid_expression,
            .message = "an observation's declared type does not match its IR value",
            .notes = {},
            .origin = origin(location),
            .generated_source = {},
        });
        return;
    }
    if (std::ranges::any_of(
            module_->observations,
            [&](const Observation& existing) {
                return existing.semantic_type == semantic_type;
            })) {
        fail(Diagnostic{
            .code = DiagnosticCode::duplicate_semantic,
            .message = "a shader stage may observe a semantic only once: "
                + semantic_name,
            .notes = {},
            .origin = origin(location),
            .generated_source = {},
        });
        return;
    }
    module_->observations.push_back(Observation{
        .semantic_type = semantic_type,
        .value_type = value_type,
        .semantic_name = std::move(semantic_name),
        .type = type,
        .value = value,
        .origin = origin(location),
    });
}

ValueId FunctionBuilder::poison(TypeId result_type, std::source_location location)
{
    return operation(OpCode::poison, result_type, {}, {}, Effect::pure, location);
}

void FunctionBuilder::fail(Diagnostic diagnostic)
{
    if (!diagnostic_) {
        diagnostic_ = std::move(diagnostic);
    }
}

Result<void> validate(const ModuleIR& module)
{
    if (!module.root_region.valid() || module.root_region.value >= module.regions.size()) {
        return std::unexpected(invalid_ir("module has no valid root region"));
    }

    auto validate_interface = [&](const std::vector<InterfaceField>& fields, std::string_view direction) -> Result<void> {
        for (u32 index = 0; index < fields.size(); ++index) {
            if (!fields[index].type.valid() || fields[index].type.value >= module.types.size()) {
                return std::unexpected(invalid_ir(
                    std::string(direction) + " interface field " + std::to_string(index) + " has an invalid type"));
            }
        }
        return {};
    };
    if (auto inputs = validate_interface(module.inputs, "input"); !inputs) return inputs;
    if (auto outputs = validate_interface(module.outputs, "output"); !outputs) return outputs;

    for (u32 index = 0; index < module.parameters.size(); ++index) {
        const auto& parameter = module.parameters[index];
        if (!parameter.type.valid() || parameter.type.value >= module.types.size()) {
            return std::unexpected(invalid_ir(
                "parameter " + std::to_string(index) + " has an invalid type"));
        }
        if (std::any_of(
                module.parameters.begin(),
                module.parameters.begin() + static_cast<std::ptrdiff_t>(index),
                [&](const ParameterField& previous) {
                    return previous.kind == parameter.kind
                        && (parameter.kind != ParameterKind::argument
                            || previous.argument_index == parameter.argument_index);
                })) {
            return std::unexpected(invalid_ir(
                "parameter " + std::to_string(index) + " duplicates parameter kind "
                + std::string(parameter_kind_name(parameter.kind))));
        }

        const auto& type = module.types[parameter.type];
        switch (parameter.kind) {
        case ParameterKind::camera_view_projection:
            if (type.kind != TypeKind::matrix || type.scalar != ScalarKind::f32 ||
                type.columns != 4 || type.rows != 4) {
                return std::unexpected(invalid_ir(
                    "camera_view_projection parameter must have type Mat4"));
            }
            break;
        case ParameterKind::argument:
            if (parameter.argument_type == typeid(void) || type.kind == TypeKind::poison
                || (type.kind == TypeKind::record && type.members.empty())) {
                return std::unexpected(invalid_ir("argument requires a nonempty logical value type"));
            }
            break;
        default:
            return std::unexpected(invalid_ir(
                "parameter " + std::to_string(index) + " has an unknown kind"));
        }
    }

    for (u32 index = 0; index < module.observations.size(); ++index) {
        const auto& observation = module.observations[index];
        if (!observation.type.valid()
            || observation.type.value >= module.types.size()) {
            return std::unexpected(invalid_ir(
                "observation " + std::to_string(index)
                + " has an invalid type"));
        }
        if (!module.owns(observation.value)
            || observation.value.value >= module.values.size()) {
            return std::unexpected(invalid_ir(
                "observation " + std::to_string(index)
                + " has an invalid value"));
        }
        if (module.values[observation.value.value].type != observation.type) {
            return std::unexpected(invalid_ir(
                "observation " + std::to_string(index)
                + " disagrees with its value type"));
        }
        if (std::any_of(
                module.observations.begin(),
                module.observations.begin()
                    + static_cast<std::ptrdiff_t>(index),
                [&](const Observation& previous) {
                    return previous.semantic_type == observation.semantic_type;
                })) {
            return std::unexpected(invalid_ir(
                "observation " + std::to_string(index)
                + " duplicates semantic " + observation.semantic_name));
        }
    }

    std::vector<u32> operation_owners(module.operations.size());
    std::vector<RegionId> operation_owner(module.operations.size());
    std::vector<u32> region_references(module.regions.size());
    std::vector<u32> argument_owners(module.values.size());
    for (u32 region_index = 0; region_index < module.regions.size(); ++region_index) {
        const auto& region = module.regions[region_index];
        for (const auto argument : region.arguments) {
            if (!module.owns(argument) || argument.value >= module.values.size()) {
                return std::unexpected(invalid_ir("region " + std::to_string(region_index) + " has an invalid argument"));
            }
            ++argument_owners[argument.value];
        }
        for (const auto operation : region.operations) {
            if (!operation.valid() || operation.value >= module.operations.size()) {
                return std::unexpected(invalid_ir("region " + std::to_string(region_index) + " has an invalid operation"));
            }
            ++operation_owners[operation.value];
            operation_owner[operation.value] = RegionId{region_index};
        }
    }

    for (u32 operation_index = 0; operation_index < module.operations.size(); ++operation_index) {
        if (operation_owners[operation_index] != 1) {
            return std::unexpected(invalid_ir(
                "operation " + std::to_string(operation_index) + " must belong to exactly one region"));
        }
        const auto& operation = module.operations[operation_index];
        const bool structural = operation.opcode == OpCode::if_region || operation.opcode == OpCode::loop_region;
        if (!structural && !operation.regions.empty()) {
            return std::unexpected(invalid_operation(
                module, operation_index, "only if and loop operations may contain nested regions"));
        }
        for (const auto region : operation.regions) {
            if (!region.valid() || region.value >= module.regions.size()) {
                return std::unexpected(invalid_operation(module, operation_index, "operation contains an invalid region"));
            }
            if (region == module.root_region || region == operation_owner[operation_index]) {
                return std::unexpected(invalid_operation(module, operation_index, "operation contains a cyclic region reference"));
            }
            ++region_references[region.value];
        }

        const bool payload_valid = [&] {
            switch (operation.opcode) {
            case OpCode::constant: return std::holds_alternative<ConstantPayload>(operation.payload);
            case OpCode::input:
            case OpCode::stage_output: return std::holds_alternative<InterfacePayload>(operation.payload);
            case OpCode::parameter: return std::holds_alternative<ParameterPayload>(operation.payload);
            case OpCode::texture_sample:
            case OpCode::texture_sample_lod: return std::holds_alternative<TextureSamplePayload>(operation.payload);
            case OpCode::matrix_buffer_read: return std::holds_alternative<MatrixBufferPayload>(operation.payload);
            case OpCode::extract_field: return std::holds_alternative<FieldPayload>(operation.payload);
            case OpCode::swizzle: return std::holds_alternative<SwizzlePayload>(operation.payload);
            default: return std::holds_alternative<std::monostate>(operation.payload);
            }
        }();
        if (!payload_valid) {
            return std::unexpected(invalid_operation(module, operation_index, "operation has the wrong payload variant"));
        }
    }

    if (!module.regions[module.root_region.value].arguments.empty()) {
        return std::unexpected(invalid_ir("the root region cannot declare block arguments"));
    }
    if (region_references[module.root_region.value] != 0) {
        return std::unexpected(invalid_ir("the root region cannot be nested in an operation"));
    }
    for (u32 region_index = 0; region_index < module.regions.size(); ++region_index) {
        if (region_index != module.root_region.value && region_references[region_index] != 1) {
            return std::unexpected(invalid_ir(
                "non-root region " + std::to_string(region_index) + " must be owned by exactly one operation"));
        }
    }
    for (u32 value_index = 0; value_index < argument_owners.size(); ++value_index) {
        if (argument_owners[value_index] > 1) {
            return std::unexpected(invalid_ir(
                "value " + std::to_string(value_index) + " is an argument of more than one region"));
        }
    }

    for (u32 value_index = 0; value_index < module.values.size(); ++value_index) {
        const auto& value = module.values[value_index];
        const auto type = value.type;
        if (!type.valid() || type.value >= module.types.size()) {
            return std::unexpected(invalid_ir("value " + std::to_string(value_index) + " has an invalid type"));
        }
        const bool region_argument = argument_owners[value_index] == 1;
        if (region_argument && value.producer.valid()) {
            return std::unexpected(invalid_ir(
                "region argument value " + std::to_string(value_index) + " cannot also have a producer"));
        }
        if (!region_argument &&
            (!value.producer.valid() || value.producer.value >= module.operations.size() ||
             module.operations[value.producer.value].result != module.value_id(value_index))) {
            return std::unexpected(invalid_ir("value " + std::to_string(value_index) + " has no valid producer"));
        }
    }

    for (u32 operation_index = 0; operation_index < module.operations.size(); ++operation_index) {
        const auto& operation = module.operations[operation_index];
        for (const auto operand : operation.operands) {
            if (!module.owns(operand) || operand.value >= module.values.size()) {
                return std::unexpected(invalid_ir("operation " + std::to_string(operation_index) + " has an invalid operand"));
            }
        }
        if (operation.result.valid()) {
            if (!module.owns(operation.result) || operation.result.value >= module.values.size() ||
                module.values[operation.result.value].producer.value != operation_index) {
                return std::unexpected(invalid_ir("operation " + std::to_string(operation_index) + " has an invalid result"));
            }
            const auto type = module.values[operation.result.value].type;
            if (!type.valid() || type.value >= module.types.size()) {
                return std::unexpected(invalid_operation(module, operation_index, "result has an invalid type"));
            }
        }

        auto require_operands = [&](std::size_t count) -> Result<void> {
            if (operation.operands.size() != count) {
                return std::unexpected(invalid_operation(
                    module,
                    operation_index,
                    "expected " + std::to_string(count) + " operands, got " + std::to_string(operation.operands.size())));
            }
            return {};
        };
        auto require_result = [&]() -> Result<void> {
            if (!operation.result.valid()) {
                return std::unexpected(invalid_operation(module, operation_index, "operation must produce a result"));
            }
            return {};
        };
        auto require_no_result = [&]() -> Result<void> {
            if (operation.result.valid()) {
                return std::unexpected(invalid_operation(module, operation_index, "statement cannot produce a result"));
            }
            return {};
        };
        auto expected_effect = [&]() -> std::optional<Effect> {
            switch (operation.opcode) {
            case OpCode::input: return Effect::input_read;
            case OpCode::parameter: return Effect::parameter_read;
            case OpCode::texture_sample:
            case OpCode::texture_sample_lod: return Effect::texture_read;
            case OpCode::matrix_buffer_read: return Effect::storage_read;
            case OpCode::stage_output: return Effect::storage_write;
            case OpCode::return_: return Effect::termination;
            case OpCode::if_region:
            case OpCode::loop_region:
            case OpCode::yield:
                return std::nullopt;
            default: return Effect::pure;
            }
        }();
        if (expected_effect && operation.effect != *expected_effect) {
            return std::unexpected(invalid_operation(module, operation_index, "operation has an invalid effect classification"));
        }
        auto value_type = [&](ValueId id) -> TypeId { return module.values[id.value].type; };
        auto description = [&](ValueId id) -> const TypeDescription& { return module.types[value_type(id)]; };
        auto result_description = [&]() -> const TypeDescription& {
            return module.types[module.values[operation.result.value].type];
        };
        auto numeric = [](const TypeDescription& type) {
            return (type.kind == TypeKind::scalar || type.kind == TypeKind::vector || type.kind == TypeKind::matrix) &&
                   type.scalar != ScalarKind::boolean;
        };
        auto integral = [](const TypeDescription& type) {
            return (type.kind == TypeKind::scalar || type.kind == TypeKind::vector) &&
                   (type.scalar == ScalarKind::i32 || type.scalar == ScalarKind::u32);
        };

        if (operation.opcode == OpCode::constant) {
            if (auto count = require_operands(0); !count) return count;
            if (auto result = require_result(); !result) return result;
            const auto* payload = std::get_if<ConstantPayload>(&operation.payload);
            if (!operation.result.valid() || payload == nullptr ||
                !constant_matches(module.types[module.values[operation.result.value].type], *payload)) {
                return std::unexpected(invalid_operation(module, operation_index, "malformed or incorrectly typed constant"));
            }
        } else if (operation.opcode == OpCode::input) {
            if (auto count = require_operands(0); !count) return count;
            if (auto result = require_result(); !result) return result;
            const auto* payload = std::get_if<InterfacePayload>(&operation.payload);
            if (!operation.result.valid() || payload == nullptr || payload->field >= module.inputs.size() ||
                module.values[operation.result.value].type != module.inputs[payload->field].type) {
                return std::unexpected(invalid_operation(module, operation_index, "malformed or incorrectly typed input"));
            }
        } else if (operation.opcode == OpCode::parameter) {
            if (auto count = require_operands(0); !count) return count;
            if (auto result = require_result(); !result) return result;
            const auto* payload = std::get_if<ParameterPayload>(&operation.payload);
            if (!operation.result.valid() || payload == nullptr ||
                payload->parameter >= module.parameters.size() ||
                module.values[operation.result.value].type !=
                    module.parameters[payload->parameter].type) {
                return std::unexpected(invalid_operation(
                    module,
                    operation_index,
                    "malformed or incorrectly typed parameter read"));
            }
        } else if (operation.opcode == OpCode::texture_sample ||
                   operation.opcode == OpCode::texture_sample_lod) {
            const bool explicit_lod = operation.opcode == OpCode::texture_sample_lod;
            if (auto count = require_operands(explicit_lod ? 2U : 1U); !count) return count;
            if (auto result = require_result(); !result) return result;
            if (!explicit_lod && module.stage != StageKind::fragment) {
                return std::unexpected(invalid_operation(
                    module, operation_index,
                    "implicit-derivative texture sampling requires a fragment stage"));
            }
            if (explicit_lod && module.stage != StageKind::vertex &&
                module.stage != StageKind::fragment) {
                return std::unexpected(invalid_operation(
                    module, operation_index, "explicit-LOD texture sampling requires a graphics stage"));
            }
            if (explicit_lod) {
                const auto& level = description(operation.operands[1]);
                if (level.kind != TypeKind::scalar || level.scalar != ScalarKind::f32) {
                    return std::unexpected(invalid_operation(
                        module, operation_index, "texture LOD must be an f32 scalar"));
                }
            }
            const auto& coordinates = description(operation.operands[0]);
            const auto& sampled = result_description();
            if (coordinates.kind != TypeKind::vector ||
                coordinates.scalar != ScalarKind::f32 || coordinates.columns != 2 ||
                sampled.kind != TypeKind::vector ||
                sampled.scalar != ScalarKind::f32 || sampled.columns != 4) {
                return std::unexpected(invalid_operation(
                    module, operation_index,
                    "2D texture sampling requires Vec2 coordinates and a Vec4 result"));
            }
        } else if (operation.opcode == OpCode::matrix_buffer_read) {
            if (auto count = require_operands(1); !count) return count;
            if (auto result = require_result(); !result) return result;
            if (module.stage != StageKind::vertex && module.stage != StageKind::fragment) {
                return std::unexpected(invalid_operation(
                    module, operation_index, "matrix buffer reads require a graphics stage"));
            }
            const auto& index = description(operation.operands[0]);
            const auto& matrix = result_description();
            if (index.kind != TypeKind::scalar || index.scalar != ScalarKind::u32 ||
                matrix.kind != TypeKind::matrix || matrix.scalar != ScalarKind::f32 ||
                matrix.columns != 4 || matrix.rows != 4) {
                return std::unexpected(invalid_operation(
                    module, operation_index,
                    "matrix buffer reads require a UInt index and a Mat4 result"));
            }
        } else if (operation.opcode == OpCode::extract_field) {
            if (auto count = require_operands(1); !count) return count;
            if (auto result = require_result(); !result) return result;
            const auto* payload = std::get_if<FieldPayload>(&operation.payload);
            const auto& aggregate = module.types[module.values[operation.operands[0].value].type];
            if (!operation.result.valid() || payload == nullptr || aggregate.kind != TypeKind::record ||
                payload->field >= aggregate.members.size() ||
                module.values[operation.result.value].type != aggregate.members[payload->field].type) {
                return std::unexpected(invalid_operation(module, operation_index, "malformed record field extraction"));
            }
        } else if (operation.opcode == OpCode::construct) {
            if (auto result = require_result(); !result) return result;
            const auto& result_type = module.types[module.values[operation.result.value].type];
            if (result_type.kind == TypeKind::record) {
                if (operation.operands.size() != result_type.members.size()) {
                    return std::unexpected(invalid_operation(module, operation_index, "record construction has the wrong field count"));
                }
                for (std::size_t field = 0; field < operation.operands.size(); ++field) {
                    if (module.values[operation.operands[field].value].type != result_type.members[field].type) {
                        return std::unexpected(invalid_operation(module, operation_index, "record construction field has the wrong type"));
                    }
                }
            } else if (result_type.kind == TypeKind::vector) {
                u32 components = 0;
                for (const auto operand : operation.operands) {
                    const auto& type = module.types[module.values[operand.value].type];
                    if (type.scalar != result_type.scalar ||
                        (type.kind != TypeKind::scalar && type.kind != TypeKind::vector)) {
                        return std::unexpected(invalid_operation(module, operation_index, "vector construction operand has the wrong type"));
                    }
                    components += type.kind == TypeKind::scalar ? 1U : type.columns;
                }
                if (components != result_type.columns) {
                    return std::unexpected(invalid_operation(module, operation_index, "vector construction has the wrong component count"));
                }
            } else if (result_type.kind == TypeKind::matrix) {
                if (operation.operands.size() != result_type.columns) {
                    return std::unexpected(invalid_operation(module, operation_index, "matrix construction has the wrong column count"));
                }
                for (const auto operand : operation.operands) {
                    const auto& type = module.types[module.values[operand.value].type];
                    if (type.kind != TypeKind::vector || type.scalar != result_type.scalar || type.columns != result_type.rows) {
                        return std::unexpected(invalid_operation(module, operation_index, "matrix construction column has the wrong type"));
                    }
                }
            } else {
                return std::unexpected(invalid_operation(module, operation_index, "construct result is not a composite type"));
            }
        } else if (operation.opcode == OpCode::swizzle) {
            if (auto count = require_operands(1); !count) return count;
            if (auto result = require_result(); !result) return result;
            const auto* payload = std::get_if<SwizzlePayload>(&operation.payload);
            const auto& source = module.types[module.values[operation.operands[0].value].type];
            if (!operation.result.valid() || payload == nullptr || source.kind != TypeKind::vector ||
                payload->components.empty() || payload->components.size() > 4 ||
                std::any_of(payload->components.begin(), payload->components.end(),
                            [&](u32 component) { return component >= source.columns; })) {
                return std::unexpected(invalid_operation(module, operation_index, "malformed swizzle"));
            }
            const auto& result = module.types[module.values[operation.result.value].type];
            const bool scalar_result = payload->components.size() == 1 &&
                result.kind == TypeKind::scalar && result.scalar == source.scalar;
            const bool vector_result = payload->components.size() > 1 &&
                result.kind == TypeKind::vector && result.scalar == source.scalar &&
                result.columns == payload->components.size();
            if (!scalar_result && !vector_result) {
                return std::unexpected(invalid_operation(module, operation_index, "swizzle result has the wrong type"));
            }
        } else if (operation.opcode == OpCode::poison) {
            if (auto count = require_operands(0); !count) return count;
            if (auto result = require_result(); !result) return result;
        } else if (operation.opcode == OpCode::negate ||
                   operation.opcode == OpCode::logical_not ||
                   operation.opcode == OpCode::bit_not ||
                   operation.opcode == OpCode::normalize ||
                   operation.opcode == OpCode::square_root ||
                   operation.opcode == OpCode::sine ||
                   operation.opcode == OpCode::cosine ||
                   operation.opcode == OpCode::absolute ||
                   operation.opcode == OpCode::floor ||
                   operation.opcode == OpCode::fract ||
                   operation.opcode == OpCode::exponential) {
            if (auto count = require_operands(1); !count) return count;
            if (auto result = require_result(); !result) return result;
            const auto& operand = description(operation.operands[0]);
            const auto& result = result_description();
            if (value_type(operation.operands[0]) != module.values[operation.result.value].type) {
                return std::unexpected(invalid_operation(module, operation_index, "unary operation changes its operand type"));
            }
            bool valid = false;
            if (operation.opcode == OpCode::negate) {
                valid = numeric(operand) && operand.scalar != ScalarKind::u32;
            } else if (operation.opcode == OpCode::logical_not) {
                valid = operand.kind == TypeKind::scalar && operand.scalar == ScalarKind::boolean;
            } else if (operation.opcode == OpCode::bit_not) {
                valid = integral(operand);
            } else if (operation.opcode == OpCode::normalize) {
                valid = operand.kind == TypeKind::vector && operand.scalar == ScalarKind::f32;
            } else if (operation.opcode == OpCode::absolute) {
                valid = (operand.kind == TypeKind::scalar || operand.kind == TypeKind::vector) &&
                        (operand.scalar == ScalarKind::f32 || operand.scalar == ScalarKind::i32);
            } else {
                valid = (operand.kind == TypeKind::scalar || operand.kind == TypeKind::vector) &&
                        operand.scalar == ScalarKind::f32;
            }
            (void)result;
            if (!valid) {
                return std::unexpected(invalid_operation(module, operation_index, "unary operation has an unsupported type"));
            }
        } else if (operation.opcode == OpCode::add ||
                   operation.opcode == OpCode::subtract ||
                   operation.opcode == OpCode::multiply ||
                   operation.opcode == OpCode::divide) {
            if (auto count = require_operands(2); !count) return count;
            if (auto result = require_result(); !result) return result;
            const auto& left = description(operation.operands[0]);
            const auto& right = description(operation.operands[1]);
            const auto& result = result_description();
            const auto left_id = value_type(operation.operands[0]);
            const auto right_id = value_type(operation.operands[1]);
            const auto result_id = module.values[operation.result.value].type;

            bool valid = numeric(left) && numeric(right) && numeric(result);
            if (left_id == right_id && result_id == left_id) {
                // Same-type scalar, vector, or matrix operation.
            } else if (left.scalar == right.scalar && left.scalar == result.scalar &&
                       ((left.kind == TypeKind::scalar && right.kind == TypeKind::vector && result_id == right_id) ||
                        (left.kind == TypeKind::vector && right.kind == TypeKind::scalar && result_id == left_id) ||
                        (left.kind == TypeKind::scalar && right.kind == TypeKind::matrix && result_id == right_id) ||
                        (left.kind == TypeKind::matrix && right.kind == TypeKind::scalar && result_id == left_id))) {
                // Component-wise scalar/composite operation.
            } else if (operation.opcode == OpCode::multiply &&
                       left.scalar == ScalarKind::f32 && right.scalar == ScalarKind::f32 &&
                       ((left.kind == TypeKind::matrix && right.kind == TypeKind::vector &&
                         left.rows == right.columns && result_id == right_id) ||
                        (left.kind == TypeKind::vector && right.kind == TypeKind::matrix &&
                         left.columns == right.columns && result_id == left_id))) {
                // Linear-algebra vector/matrix multiplication.
            } else {
                valid = false;
            }
            if (!valid) {
                return std::unexpected(invalid_operation(module, operation_index, "arithmetic operation has incompatible types"));
            }
        } else if (operation.opcode == OpCode::remainder ||
                   operation.opcode == OpCode::bit_and ||
                   operation.opcode == OpCode::bit_or ||
                   operation.opcode == OpCode::bit_xor ||
                   operation.opcode == OpCode::shift_left ||
                   operation.opcode == OpCode::shift_right) {
            if (auto count = require_operands(2); !count) return count;
            if (auto result = require_result(); !result) return result;
            const auto operand_type = value_type(operation.operands[0]);
            if (operand_type != value_type(operation.operands[1]) ||
                operand_type != module.values[operation.result.value].type ||
                !integral(description(operation.operands[0]))) {
                return std::unexpected(invalid_operation(module, operation_index, "integer operation has incompatible types"));
            }
        } else if (operation.opcode == OpCode::equal ||
                   operation.opcode == OpCode::not_equal ||
                   operation.opcode == OpCode::less ||
                   operation.opcode == OpCode::less_equal ||
                   operation.opcode == OpCode::greater ||
                   operation.opcode == OpCode::greater_equal) {
            if (auto count = require_operands(2); !count) return count;
            if (auto result = require_result(); !result) return result;
            if (value_type(operation.operands[0]) != value_type(operation.operands[1])) {
                return std::unexpected(invalid_operation(module, operation_index, "comparison operands have different types"));
            }
            const auto& operand = description(operation.operands[0]);
            const auto& result = result_description();
            const bool equality = operation.opcode == OpCode::equal || operation.opcode == OpCode::not_equal;
            const bool operand_valid = equality
                ? operand.kind != TypeKind::record && operand.kind != TypeKind::poison
                : numeric(operand) && operand.kind != TypeKind::matrix;
            const bool result_valid = operand.kind == TypeKind::vector
                ? result.kind == TypeKind::vector && result.scalar == ScalarKind::boolean && result.columns == operand.columns
                : result.kind == TypeKind::scalar && result.scalar == ScalarKind::boolean;
            if (!operand_valid || !result_valid) {
                return std::unexpected(invalid_operation(module, operation_index, "comparison has an unsupported result or operand type"));
            }
        } else if (operation.opcode == OpCode::cast) {
            if (auto count = require_operands(1); !count) return count;
            if (auto result = require_result(); !result) return result;
            const auto& from = description(operation.operands[0]);
            const auto& to = result_description();
            const bool scalar_cast = from.kind == TypeKind::scalar && to.kind == TypeKind::scalar;
            const bool vector_cast = from.kind == TypeKind::vector && to.kind == TypeKind::vector &&
                                     from.columns == to.columns;
            if (!scalar_cast && !vector_cast) {
                return std::unexpected(invalid_operation(module, operation_index, "cast changes value shape"));
            }
        } else if (operation.opcode == OpCode::dot || operation.opcode == OpCode::cross ||
                   operation.opcode == OpCode::minimum || operation.opcode == OpCode::maximum ||
                   operation.opcode == OpCode::power) {
            if (auto count = require_operands(2); !count) return count;
            if (auto result = require_result(); !result) return result;
            const auto left_id = value_type(operation.operands[0]);
            const auto& left = description(operation.operands[0]);
            bool valid = left_id == value_type(operation.operands[1]);
            if (operation.opcode == OpCode::dot) {
                const auto& result = result_description();
                valid = valid && left.kind == TypeKind::vector && left.scalar == ScalarKind::f32 &&
                        result.kind == TypeKind::scalar && result.scalar == ScalarKind::f32;
            } else if (operation.opcode == OpCode::cross) {
                valid = valid && left.kind == TypeKind::vector && left.scalar == ScalarKind::f32 &&
                        left.columns == 3 && module.values[operation.result.value].type == left_id;
            } else if (operation.opcode == OpCode::power) {
                valid = valid &&
                        (left.kind == TypeKind::scalar || left.kind == TypeKind::vector) &&
                        left.scalar == ScalarKind::f32 && module.values[operation.result.value].type == left_id;
            } else {
                valid = valid &&
                        (left.kind == TypeKind::scalar || left.kind == TypeKind::vector) &&
                        numeric(left) && module.values[operation.result.value].type == left_id;
            }
            if (!valid) {
                return std::unexpected(invalid_operation(module, operation_index, "intrinsic has incompatible operand or result types"));
            }
        } else if (operation.opcode == OpCode::clamp || operation.opcode == OpCode::mix ||
                   operation.opcode == OpCode::select) {
            if (auto count = require_operands(3); !count) return count;
            if (auto result = require_result(); !result) return result;
            const auto result_id = module.values[operation.result.value].type;
            const auto& result = result_description();
            bool valid = false;
            if (operation.opcode == OpCode::clamp) {
                const auto low_id = value_type(operation.operands[1]);
                const auto high_id = value_type(operation.operands[2]);
                valid = value_type(operation.operands[0]) == result_id && low_id == high_id &&
                        (result.kind == TypeKind::scalar || result.kind == TypeKind::vector) && numeric(result) &&
                        (low_id == result_id ||
                         (result.kind == TypeKind::vector && description(operation.operands[1]).kind == TypeKind::scalar &&
                          description(operation.operands[1]).scalar == result.scalar));
            } else if (operation.opcode == OpCode::mix) {
                const auto factor_id = value_type(operation.operands[2]);
                const auto& factor = description(operation.operands[2]);
                valid = value_type(operation.operands[0]) == result_id &&
                        value_type(operation.operands[1]) == result_id &&
                        (result.kind == TypeKind::scalar || result.kind == TypeKind::vector) &&
                        result.scalar == ScalarKind::f32 &&
                        (factor_id == result_id || (factor.kind == TypeKind::scalar && factor.scalar == ScalarKind::f32));
            } else {
                const auto& condition = description(operation.operands[0]);
                valid = value_type(operation.operands[1]) == result_id && value_type(operation.operands[2]) == result_id &&
                        ((condition.kind == TypeKind::scalar && condition.scalar == ScalarKind::boolean) ||
                         (condition.kind == TypeKind::vector && condition.scalar == ScalarKind::boolean &&
                          result.kind == TypeKind::vector && condition.columns == result.columns));
            }
            if (!valid) {
                return std::unexpected(invalid_operation(module, operation_index, "ternary intrinsic has incompatible types"));
            }
        } else if (operation.opcode == OpCode::all || operation.opcode == OpCode::any) {
            if (auto count = require_operands(1); !count) return count;
            if (auto result = require_result(); !result) return result;
            const auto& operand = description(operation.operands[0]);
            const auto& result = result_description();
            if (operand.kind != TypeKind::vector || operand.scalar != ScalarKind::boolean ||
                result.kind != TypeKind::scalar || result.scalar != ScalarKind::boolean) {
                return std::unexpected(invalid_operation(module, operation_index, "all/any requires a boolean vector and produces bool"));
            }
        } else if (operation.opcode == OpCode::stage_output) {
            if (auto count = require_operands(1); !count) return count;
            if (auto result = require_no_result(); !result) return result;
        } else if (operation.opcode == OpCode::return_) {
            if (auto count = require_operands(0); !count) return count;
            if (auto result = require_no_result(); !result) return result;
        }
    }

    std::vector<bool> visited_regions(module.regions.size());
    std::vector<bool> active_regions(module.regions.size());
    std::function<Result<void>(RegionId, std::vector<bool>)> validate_region_order;
    validate_region_order = [&](RegionId region_id, std::vector<bool> visible) -> Result<void> {
        if (active_regions[region_id.value]) {
            return std::unexpected(invalid_ir("nested regions contain a cycle"));
        }
        if (visited_regions[region_id.value]) {
            return std::unexpected(invalid_ir("a nested region is reachable through more than one path"));
        }
        active_regions[region_id.value] = true;
        visited_regions[region_id.value] = true;

        const auto& region = module.regions[region_id.value];
        if (region_id != module.root_region) {
            for (const auto argument : region.arguments) {
                visible[argument.value] = true;
            }
        }

        for (const auto operation_id : region.operations) {
            const auto& operation = module.operations[operation_id.value];
            for (const auto operand : operation.operands) {
                if (!visible[operand.value]) {
                    return std::unexpected(invalid_operation(
                        module,
                        operation_id.value,
                        region_id == module.root_region
                            ? "root operation uses a value that was not produced earlier in the root region"
                            : "region operation uses a value that is not visible at this point"));
                }
            }
            for (const auto child : operation.regions) {
                if (auto nested = validate_region_order(child, visible); !nested) {
                    return nested;
                }
            }
            if (operation.result.valid()) {
                visible[operation.result.value] = true;
            }
        }

        active_regions[region_id.value] = false;
        return {};
    };

    if (auto order = validate_region_order(
            module.root_region,
            std::vector<bool>(module.values.size()));
        !order) {
        return order;
    }
    if (std::any_of(visited_regions.begin(), visited_regions.end(), [](bool visited) { return !visited; })) {
        return std::unexpected(invalid_ir("module contains a region that is unreachable from the root region"));
    }

    std::vector<u32> writes(module.outputs.size());
    u32 returns = 0;
    for (const auto id : module.regions[module.root_region.value].operations) {
        if (!id.valid() || id.value >= module.operations.size()) {
            return std::unexpected(invalid_ir("root region contains an invalid operation"));
        }
        const auto& operation = module.operations[id.value];
        if (operation.opcode == OpCode::return_) {
            ++returns;
        }
        if (operation.opcode == OpCode::stage_output) {
            const auto* field = std::get_if<InterfacePayload>(&operation.payload);
            if (field == nullptr || field->field >= module.outputs.size() || operation.operands.size() != 1) {
                return std::unexpected(invalid_ir("malformed stage output operation"));
            }
            const auto value_type = module.values[operation.operands.front().value].type;
            if (value_type != module.outputs[field->field].type) {
                return std::unexpected(invalid_ir("stage output value has the wrong type"));
            }
            ++writes[field->field];
        }
    }

    if (std::any_of(writes.begin(), writes.end(), [](u32 count) { return count != 1; })) {
        return std::unexpected(invalid_ir("every stage output must be produced exactly once"));
    }
    const auto& root_operations = module.regions[module.root_region.value].operations;
    if (returns != 1 || root_operations.empty() ||
        module.operations[root_operations.back().value].opcode != OpCode::return_) {
        return std::unexpected(invalid_ir("the root region must end in exactly one return operation"));
    }
    return {};
}

void optimize(ModuleIR& module)
{
    for (auto& operation : module.operations) {
        if (!operation.result.valid() || operation.effect != Effect::pure) {
            continue;
        }
        if (auto folded = fold_operation(module, operation)) {
            operation.opcode = OpCode::constant;
            operation.operands.clear();
            operation.payload = std::move(*folded);
        }
    }

    std::vector<bool> live(module.operations.size());
    for (u32 index = 0; index < module.operations.size(); ++index) {
        if (!is_removable(module.operations[index])) {
            mark_live(module, OperationId{index}, live);
        }
    }
    for (const auto& observation : module.observations) {
        if (module.owns(observation.value)
            && observation.value.value < module.values.size()) {
            mark_live(
                module,
                module.values[observation.value.value].producer,
                live);
        }
    }

    std::vector<bool> live_values(module.values.size());
    for (u32 index = 0; index < module.operations.size(); ++index) {
        if (!live[index]) {
            continue;
        }
        const auto& operation = module.operations[index];
        if (operation.result.valid() && operation.result.value < live_values.size()) {
            live_values[operation.result.value] = true;
        }
        for (const auto operand : operation.operands) {
            if (operand.valid() && operand.value < live_values.size()) {
                live_values[operand.value] = true;
            }
        }
    }
    for (const auto& region : module.regions) {
        for (const auto argument : region.arguments) {
            if (argument.valid() && argument.value < live_values.size()) {
                live_values[argument.value] = true;
            }
        }
    }
    for (const auto& observation : module.observations) {
        if (observation.value.valid()
            && observation.value.value < live_values.size()) {
            live_values[observation.value.value] = true;
        }
    }

    std::vector<OperationId> operation_map(module.operations.size());
    std::vector<ValueId> value_map(module.values.size());
    u32 next_operation = 0;
    u32 next_value = 0;
    for (u32 index = 0; index < module.operations.size(); ++index) {
        if (live[index]) {
            operation_map[index] = OperationId{next_operation++};
        }
    }
    for (u32 index = 0; index < module.values.size(); ++index) {
        if (live_values[index]) {
            value_map[index] = module.value_id(next_value++);
        }
    }

    std::vector<ValueNode> compact_values;
    compact_values.reserve(next_value);
    for (u32 index = 0; index < module.values.size(); ++index) {
        if (!live_values[index]) {
            continue;
        }
        auto value = module.values[index];
        if (value.producer.valid() && value.producer.value < operation_map.size() &&
            operation_map[value.producer.value].valid()) {
            value.producer = operation_map[value.producer.value];
        } else {
            value.producer = {};
        }
        compact_values.push_back(value);
    }

    std::vector<Operation> compact_operations;
    compact_operations.reserve(next_operation);
    for (u32 index = 0; index < module.operations.size(); ++index) {
        if (!live[index]) {
            continue;
        }
        auto operation = std::move(module.operations[index]);
        for (auto& operand : operation.operands) {
            operand = value_map[operand.value];
        }
        if (operation.result.valid()) {
            operation.result = value_map[operation.result.value];
        }
        compact_operations.push_back(std::move(operation));
    }

    for (auto& region : module.regions) {
        for (auto& argument : region.arguments) {
            argument = value_map[argument.value];
        }
        std::vector<OperationId> compact_region_operations;
        compact_region_operations.reserve(region.operations.size());
        for (const auto operation : region.operations) {
            if (operation.valid() && operation.value < live.size() && live[operation.value]) {
                compact_region_operations.push_back(operation_map[operation.value]);
            }
        }
        region.operations = std::move(compact_region_operations);
    }
    for (auto& observation : module.observations) {
        observation.value = value_map[observation.value.value];
    }

    module.values = std::move(compact_values);
    module.operations = std::move(compact_operations);
}

std::string_view opcode_name(OpCode opcode) noexcept
{
    switch (opcode) {
    case OpCode::poison: return "poison";
    case OpCode::constant: return "constant";
    case OpCode::input: return "input";
    case OpCode::parameter: return "parameter";
    case OpCode::texture_sample: return "texture_sample";
    case OpCode::texture_sample_lod: return "texture_sample_lod";
    case OpCode::matrix_buffer_read: return "matrix_buffer_read";
    case OpCode::construct: return "construct";
    case OpCode::extract_field: return "extract_field";
    case OpCode::swizzle: return "swizzle";
    case OpCode::negate: return "negate";
    case OpCode::logical_not: return "not";
    case OpCode::bit_not: return "bit_not";
    case OpCode::add: return "add";
    case OpCode::subtract: return "subtract";
    case OpCode::multiply: return "multiply";
    case OpCode::divide: return "divide";
    case OpCode::remainder: return "remainder";
    case OpCode::equal: return "equal";
    case OpCode::not_equal: return "not_equal";
    case OpCode::less: return "less";
    case OpCode::less_equal: return "less_equal";
    case OpCode::greater: return "greater";
    case OpCode::greater_equal: return "greater_equal";
    case OpCode::bit_and: return "bit_and";
    case OpCode::bit_or: return "bit_or";
    case OpCode::bit_xor: return "bit_xor";
    case OpCode::shift_left: return "shift_left";
    case OpCode::shift_right: return "shift_right";
    case OpCode::cast: return "cast";
    case OpCode::dot: return "dot";
    case OpCode::cross: return "cross";
    case OpCode::normalize: return "normalize";
    case OpCode::minimum: return "min";
    case OpCode::maximum: return "max";
    case OpCode::clamp: return "clamp";
    case OpCode::mix: return "mix";
    case OpCode::square_root: return "sqrt";
    case OpCode::sine: return "sin";
    case OpCode::cosine: return "cos";
    case OpCode::absolute: return "abs";
    case OpCode::floor: return "floor";
    case OpCode::fract: return "fract";
    case OpCode::exponential: return "exp";
    case OpCode::power: return "pow";
    case OpCode::all: return "all";
    case OpCode::any: return "any";
    case OpCode::select: return "select";
    case OpCode::stage_output: return "stage_output";
    case OpCode::return_: return "return";
    case OpCode::if_region: return "if";
    case OpCode::loop_region: return "loop";
    case OpCode::yield: return "yield";
    }
    return "unknown";
}

std::string_view parameter_kind_name(ParameterKind kind) noexcept
{
    switch (kind) {
    case ParameterKind::camera_view_projection:
        return "camera_view_projection";
    case ParameterKind::argument:
        return "argument";
    }
    return "unknown";
}

std::string_view type_name(const TypeDescription& type) noexcept
{
    switch (type.kind) {
    case TypeKind::poison: return "poison";
    case TypeKind::scalar:
        switch (type.scalar) {
        case ScalarKind::boolean: return "bool";
        case ScalarKind::i32: return "i32";
        case ScalarKind::u32: return "u32";
        case ScalarKind::f32: return "f32";
        }
        break;
    default:
        return type.name;
    }
    return type.name;
}

std::string dump_ir(const ModuleIR& module)
{
    std::ostringstream stream;
    stream << (module.stage == StageKind::vertex ? "vertex" : "fragment");
    if (!module.debug_name.empty()) {
        stream << " @" << module.debug_name;
    }
    stream << " {\n";

    const auto& root = module.regions[module.root_region.value];
    for (const auto id : root.operations) {
        const auto& operation = module.operations[id.value];
        stream << "  ";
        if (operation.result.valid()) {
            dump_value(stream, operation.result);
            stream << " : " << type_name(module.types[module.values[operation.result.value].type]) << " = ";
        }
        stream << opcode_name(operation.opcode);
        for (const auto operand : operation.operands) {
            stream << ' ';
            dump_value(stream, operand);
        }
        if (const auto* constant = std::get_if<ConstantPayload>(&operation.payload)) {
            stream << ' ';
            dump_constant(stream, *constant);
        } else if (const auto* interface = std::get_if<InterfacePayload>(&operation.payload)) {
            stream << " [" << interface->field << ']';
        } else if (const auto* parameter = std::get_if<ParameterPayload>(&operation.payload)) {
            stream << " [" << parameter->parameter;
            if (parameter->parameter < module.parameters.size()) {
                stream << ':' << parameter_kind_name(
                    module.parameters[parameter->parameter].kind);
            }
            stream << ']';
        } else if (const auto* texture = std::get_if<TextureSamplePayload>(&operation.payload)) {
            stream << " [binding=" << texture->binding << ']';
        } else if (const auto* buffer = std::get_if<MatrixBufferPayload>(&operation.payload)) {
            stream << " [binding=" << buffer->binding << ']';
        } else if (const auto* field = std::get_if<FieldPayload>(&operation.payload)) {
            stream << " [" << field->field << ']';
        } else if (const auto* swizzle = std::get_if<SwizzlePayload>(&operation.payload)) {
            stream << " [";
            for (std::size_t index = 0; index < swizzle->components.size(); ++index) {
                if (index != 0) stream << ',';
                stream << swizzle->components[index];
            }
            stream << ']';
        }
        stream << '\n';
    }
    for (const auto& observation : module.observations) {
        stream << "  observe " << observation.semantic_name << " = ";
        dump_value(stream, observation.value);
        stream << '\n';
    }
    stream << "}\n";
    return stream.str();
}

std::string dump_interface(const ModuleIR& module)
{
    auto dump_fields = [&](std::ostringstream& stream, std::string_view label, const std::vector<InterfaceField>& fields) {
        stream << label << ":\n";
        for (u32 index = 0; index < fields.size(); ++index) {
            const auto& field = fields[index];
            stream << "  [" << index << "] " << field.semantic_name << " : " << type_name(module.types[field.type]);
            if (field.builtin != Builtin::none) {
                stream << " builtin=" << static_cast<unsigned>(field.builtin);
                if (field.builtin == Builtin::color) stream << '(' << field.builtin_index << ')';
            } else if (field.location) {
                stream << " location=" << *field.location;
            }
            switch (field.interpolation) {
            case Interpolation::smooth: stream << " smooth"; break;
            case Interpolation::flat: stream << " flat"; break;
            case Interpolation::no_perspective: stream << " noperspective"; break;
            case Interpolation::none: break;
            }
            stream << '\n';
        }
    };

    std::ostringstream stream;
    dump_fields(stream, "inputs", module.inputs);
    dump_fields(stream, "outputs", module.outputs);
    if (!module.parameters.empty()) {
        stream << "parameters:\n";
        for (u32 index = 0; index < module.parameters.size(); ++index) {
            const auto& parameter = module.parameters[index];
            stream << "  [" << index << "] " << parameter_kind_name(parameter.kind);
            if (parameter.kind == ParameterKind::argument) stream << '[' << parameter.argument_index << ']';
            stream << " : ";
            if (parameter.type.valid() && parameter.type.value < module.types.size()) {
                stream << type_name(module.types[parameter.type]);
            } else {
                stream << "<invalid>";
            }
            if (parameter.location) {
                stream << " location=" << *parameter.location;
            }
            stream << '\n';
        }
    }
    if (!module.observations.empty()) {
        stream << "observations:\n";
        for (u32 index = 0; index < module.observations.size(); ++index) {
            const auto& observation = module.observations[index];
            stream << "  [" << index << "] " << observation.semantic_name
                   << " : ";
            if (observation.type.valid()
                && observation.type.value < module.types.size()) {
                stream << type_name(module.types[observation.type]);
            } else {
                stream << "<invalid>";
            }
            stream << " = ";
            dump_value(stream, observation.value);
            stream << '\n';
        }
    }
    return stream.str();
}

} // namespace vng::shader
