#pragma once

#include <vng/shader/diagnostic.hpp>
#include <vng/shader/interface.hpp>
#include <vng/shader/type.hpp>

#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <typeindex>
#include <utility>
#include <variant>
#include <vector>

namespace vng::shader {

enum class OpCode : std::uint8_t {
    poison,
    constant,
    input,
    parameter,
    texture_sample,
    texture_sample_lod,
    matrix_buffer_read,
    construct,
    extract_field,
    swizzle,
    negate,
    logical_not,
    bit_not,
    add,
    subtract,
    multiply,
    divide,
    remainder,
    equal,
    not_equal,
    less,
    less_equal,
    greater,
    greater_equal,
    bit_and,
    bit_or,
    bit_xor,
    shift_left,
    shift_right,
    cast,
    dot,
    cross,
    normalize,
    minimum,
    maximum,
    clamp,
    mix,
    square_root,
    sine,
    cosine,
    absolute,
    floor,
    fract,
    exponential,
    power,
    all,
    any,
    select,
    stage_output,
    return_,
    if_region,
    loop_region,
    yield,
};

enum class Effect : std::uint16_t {
    pure = 0,
    input_read = 1U << 0U,
    storage_read = 1U << 1U,
    storage_write = 1U << 2U,
    atomic = 1U << 3U,
    texture_read = 1U << 4U,
    image_write = 1U << 5U,
    barrier = 1U << 6U,
    termination = 1U << 7U,
    parameter_read = 1U << 8U,
};

[[nodiscard]] constexpr Effect operator|(Effect left, Effect right) noexcept
{
    return static_cast<Effect>(static_cast<std::uint16_t>(left) | static_cast<std::uint16_t>(right));
}

struct ConstantPayload {
    std::variant<bool, i32, u32, f32> value;
};

struct InterfacePayload {
    u32 field{};
};

enum class ParameterKind : std::uint8_t {
    camera_view_projection,
    argument,
};

struct ParameterPayload {
    u32 parameter{};
};

// Samples a floating-point/normalized 2D texture. The opcode determines
// whether the LOD is implicit (fragment only) or an explicit second operand.
// The binding identifies a portable texture resource slot, not a native handle.
struct TextureSamplePayload {
    u32 binding{};
};

// A read-only, runtime-sized array of column-major Mat4 values. The portable
// resource slot is independent of texture bindings and native buffer handles.
struct MatrixBufferPayload {
    u32 binding{};
};

struct FieldPayload {
    u32 field{};
};

struct SwizzlePayload {
    std::vector<u32> components;
};

using OperationPayload = std::variant<
    std::monostate,
    ConstantPayload,
    InterfacePayload,
    ParameterPayload,
    TextureSamplePayload,
    MatrixBufferPayload,
    FieldPayload,
    SwizzlePayload>;

struct ValueNode {
    TypeId type;
    OperationId producer;
};

struct Operation {
    OpCode opcode{OpCode::poison};
    std::vector<ValueId> operands;
    ValueId result;
    std::vector<RegionId> regions;
    OperationPayload payload;
    Effect effect{Effect::pure};
    SourceOrigin origin;
};

struct Region {
    std::vector<ValueId> arguments;
    std::vector<OperationId> operations;
};

struct InterfaceField {
    std::type_index semantic_type{typeid(void)};
    std::type_index value_type{typeid(void)};
    std::string semantic_name;
    TypeId type;
    Interpolation interpolation{Interpolation::none};
    Builtin builtin{Builtin::none};
    u32 builtin_index{};
    std::optional<u32> location;
};

struct ParameterField {
    ParameterKind kind{ParameterKind::camera_view_projection};
    TypeId type;
    std::optional<u32> location;
    u32 argument_index{};
    std::type_index argument_type{typeid(void)};

    friend bool operator==(const ParameterField&, const ParameterField&) = default;
};

// A semantic name attached to an existing immutable IR value. Observations
// are inert metadata: normal emission does not expose or evaluate a value only
// because it is observed. Diagnostic backends may selectively make one live.
struct Observation final {
    std::type_index semantic_type{typeid(void)};
    std::type_index value_type{typeid(void)};
    std::string semantic_name;
    TypeId type;
    ValueId value;
    SourceOrigin origin;
};

struct ModuleIR {
    StageKind stage{StageKind::vertex};
    std::string debug_name;
    TypeTable types;
    std::vector<ValueNode> values;
    std::vector<Operation> operations;
    std::vector<Region> regions{1};
    RegionId root_region{0};
    std::vector<InterfaceField> inputs;
    std::vector<InterfaceField> outputs;
    std::vector<ParameterField> parameters;
    std::vector<Observation> observations;

    [[nodiscard]] ValueId value_id(u32 index) const noexcept
    {
        return ValueId{index, value_owner_};
    }

    [[nodiscard]] bool owns(ValueId id) const noexcept
    {
        return id.valid() && id.owner == value_owner_;
    }

private:
    void rebind_values(u64 owner) noexcept;

    u64 value_owner_{};

    friend class FunctionBuilder;
};

class FunctionBuilder {
public:
    explicit FunctionBuilder(ModuleIR& module) noexcept;
    FunctionBuilder(const FunctionBuilder&) = delete;
    FunctionBuilder(FunctionBuilder&&) = delete;
    FunctionBuilder& operator=(const FunctionBuilder&) = delete;
    FunctionBuilder& operator=(FunctionBuilder&&) = delete;

    [[nodiscard]] ModuleIR& module() noexcept { return *module_; }
    [[nodiscard]] const ModuleIR& module() const noexcept { return *module_; }

    template<Value T>
    [[nodiscard]] TypeId type()
    {
        return detail::register_type<T>(module_->types);
    }

    [[nodiscard]] ValueId operation(
        OpCode opcode,
        TypeId result_type,
        std::span<const ValueId> operands = {},
        OperationPayload payload = {},
        Effect effect = Effect::pure,
        std::source_location location = std::source_location::current());

    void statement(
        OpCode opcode,
        std::span<const ValueId> operands = {},
        OperationPayload payload = {},
        Effect effect = Effect::pure,
        std::source_location location = std::source_location::current());

    void observe(
        std::type_index semantic_type,
        std::type_index value_type,
        std::string semantic_name,
        TypeId type,
        ValueId value,
        std::source_location location = std::source_location::current());

    [[nodiscard]] ValueId poison(TypeId result_type, std::source_location location = std::source_location::current());
    void fail(Diagnostic diagnostic);

    [[nodiscard]] bool failed() const noexcept { return diagnostic_.has_value(); }
    [[nodiscard]] const std::optional<Diagnostic>& diagnostic() const noexcept { return diagnostic_; }
    [[nodiscard]] bool owns(ValueId id) const noexcept
    {
        return module_->value_owner_ == owner_ && module_->owns(id);
    }

private:
    [[nodiscard]] static SourceOrigin origin(std::source_location location);
    void append(Operation operation);

    ModuleIR* module_;
    u64 owner_{};
    std::optional<Diagnostic> diagnostic_;
};

[[nodiscard]] Result<void> validate(const ModuleIR& module);
void optimize(ModuleIR& module);
[[nodiscard]] std::string dump_ir(const ModuleIR& module);
[[nodiscard]] std::string dump_interface(const ModuleIR& module);
[[nodiscard]] std::string_view opcode_name(OpCode opcode) noexcept;
[[nodiscard]] std::string_view parameter_kind_name(ParameterKind kind) noexcept;
[[nodiscard]] std::string_view type_name(const TypeDescription& type) noexcept;

} // namespace vng::shader
