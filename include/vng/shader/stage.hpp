#pragma once

#include <vng/dsl/dsl.hpp>
#include <vng/shader/diagnostic.hpp>
#include <vng/shader/interface.hpp>
#include <vng/shader/ir.hpp>

#include <array>
#include <algorithm>
#include <functional>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace vng::shader {

namespace detail { class ShaderStageAccess; }

class ShaderStage {
public:
    ShaderStage() = delete;
    ShaderStage(const ShaderStage&) = default;
    ShaderStage(ShaderStage&&) noexcept = default;
    ShaderStage& operator=(const ShaderStage&) = default;
    ShaderStage& operator=(ShaderStage&&) noexcept = default;

    [[nodiscard]] StageKind kind() const noexcept { return ir_.stage; }
    [[nodiscard]] const ModuleIR& ir() const noexcept { return ir_; }
    [[nodiscard]] std::string dump_ir() const { return shader::dump_ir(ir_); }
    [[nodiscard]] std::string dump_interface() const { return shader::dump_interface(ir_); }

private:
    explicit ShaderStage(ModuleIR ir) : ir_(std::move(ir)) {}

    ModuleIR ir_;

    friend class detail::ShaderStageAccess;
};

namespace detail {

class ShaderStageAccess final {
public:
    [[nodiscard]] static ShaderStage from_ir(ModuleIR ir)
    {
        return ShaderStage{std::move(ir)};
    }

    [[nodiscard]] static ModuleIR& ir(ShaderStage& stage) noexcept
    {
        return stage.ir_;
    }
};

template<Interface Schema>
void describe_interface(FunctionBuilder& builder, std::vector<InterfaceField>& destination)
{
    shader::detail::for_each_type<typename Schema::declared_fields>([&]<class DeclaredField> {
        using Traits = interface_field_traits<DeclaredField>;
        using Semantic = typename Traits::semantic_type;
        using Value = typename Semantic::value_type;

        destination.push_back(InterfaceField{
            .semantic_type = typeid(Semantic),
            .value_type = typeid(Value),
            .semantic_name = shader::detail::type_name<Semantic>(),
            .type = builder.template type<Value>(),
            .interpolation = Traits::interpolation,
            .builtin = builtin_traits<Semantic>::value,
            .builtin_index = builtin_traits<Semantic>::index,
            .location = std::nullopt,
        });
    });
}

template<StageKind Stage, Interface Schema>
[[nodiscard]] Result<void> validate_declared_interface()
{
    if constexpr (Schema::stage != Stage) {
        return std::unexpected(Diagnostic{
            .code = DiagnosticCode::invalid_stage_result,
            .message = "shader interface belongs to the wrong stage",
            .notes = {},
            .origin = {},
            .generated_source = {},
        });
    }
    return {};
}

template<class Semantic>
[[nodiscard]] constexpr bool legal_builtin(StageKind stage, InterfaceDirection direction)
{
    constexpr auto builtin = builtin_traits<Semantic>::value;
    if constexpr (builtin == Builtin::none) {
        return true;
    } else if constexpr (builtin == Builtin::clip_position) {
        return stage == StageKind::vertex && direction == InterfaceDirection::output;
    } else if constexpr (builtin == Builtin::fragment_depth || builtin == Builtin::color) {
        return stage == StageKind::fragment && direction == InterfaceDirection::output;
    } else if constexpr (builtin == Builtin::fragment_coordinate || builtin == Builtin::front_facing) {
        return stage == StageKind::fragment && direction == InterfaceDirection::input;
    } else if constexpr (builtin == Builtin::vertex_index || builtin == Builtin::instance_index) {
        return stage == StageKind::vertex && direction == InterfaceDirection::input;
    }
    return false;
}

template<Interface Schema>
[[nodiscard]] Result<void> validate_builtins()
{
    Result<void> result{};
    u32 clip_positions = 0;
    u32 color_outputs = 0;
    shader::detail::for_each_type<typename Schema::semantics>([&]<class Semantic> {
        constexpr auto builtin = builtin_traits<Semantic>::value;
        if constexpr (builtin == Builtin::clip_position) {
            ++clip_positions;
        } else if constexpr (builtin == Builtin::color) {
            ++color_outputs;
        }

        bool legal = legal_builtin<Semantic>(Schema::stage, Schema::direction);
        if constexpr (Schema::stage == StageKind::vertex &&
                      Schema::direction == InterfaceDirection::input &&
                      builtin != Builtin::none) {
            legal = false; // VertexInputs::semantics is intentionally buffer-only.
        }
        if constexpr (Schema::stage == StageKind::fragment &&
                      Schema::direction == InterfaceDirection::output &&
                      builtin == Builtin::none) {
            legal = false;
        }

        if (!result || legal) {
            return;
        }
        result = std::unexpected(Diagnostic{
            .code = DiagnosticCode::invalid_builtin,
            .message = "semantic " + shader::detail::type_name<Semantic>() + " is illegal in this shader interface",
            .notes = {},
            .origin = {},
            .generated_source = {},
        });
    });

    if (!result) {
        return result;
    }
    if constexpr (Schema::stage == StageKind::vertex &&
                  Schema::direction == InterfaceDirection::output) {
        if (clip_positions != 1) {
            return std::unexpected(Diagnostic{
                .code = DiagnosticCode::invalid_builtin,
                .message = "a vertex stage must produce exactly one shader::ClipPosition",
                .notes = {},
                .origin = {},
                .generated_source = {},
            });
        }
    }
    if constexpr (Schema::stage == StageKind::fragment &&
                  Schema::direction == InterfaceDirection::output) {
        if (color_outputs == 0) {
            return std::unexpected(Diagnostic{
                .code = DiagnosticCode::invalid_builtin,
                .message = "a fragment stage must produce at least one shader::Color<N>",
                .notes = {},
                .origin = {},
                .generated_source = {},
            });
        }
    }
    return result;
}

[[nodiscard]] inline bool integral_interface_type(const TypeDescription& type)
{
    return (type.kind == TypeKind::scalar || type.kind == TypeKind::vector) &&
           type.scalar != ScalarKind::f32;
}

[[nodiscard]] inline Result<void> validate_interface_types(
    const ModuleIR& module,
    const std::vector<InterfaceField>& fields,
    InterfaceDirection direction)
{
    for (const auto& field : fields) {
        const auto& type = module.types[field.type];
        if (type.kind == TypeKind::record) {
            return std::unexpected(Diagnostic{
                .code = DiagnosticCode::unsupported_operation,
                .message = "record-valued stage interface fields are not supported in the first milestone",
                .notes = {"semantic: " + field.semantic_name},
                .origin = {},
                .generated_source = {},
            });
        }
        if (field.builtin == Builtin::none && type.kind == TypeKind::matrix) {
            return std::unexpected(Diagnostic{
                .code = DiagnosticCode::unsupported_operation,
                .message = "matrix-valued stage interface fields are not supported in the first milestone",
                .notes = {"semantic: " + field.semantic_name},
                .origin = {},
                .generated_source = {},
            });
        }
        if (field.builtin != Builtin::none && field.interpolation != Interpolation::none) {
            return std::unexpected(Diagnostic{
                .code = DiagnosticCode::invalid_builtin,
                .message = "shader builtins cannot have interpolation qualifiers",
                .notes = {"semantic: " + field.semantic_name},
                .origin = {},
                .generated_source = {},
            });
        }
        if (field.builtin == Builtin::none &&
            (type.kind == TypeKind::scalar || type.kind == TypeKind::vector) &&
            type.scalar == ScalarKind::boolean) {
            return std::unexpected(Diagnostic{
                .code = DiagnosticCode::unsupported_operation,
                .message = "boolean user stage interface fields are not supported by GLSL 4.60",
                .notes = {"semantic: " + field.semantic_name},
                .origin = {},
                .generated_source = {},
            });
        }
        if (module.stage == StageKind::vertex &&
            direction == InterfaceDirection::input &&
            field.interpolation != Interpolation::none) {
            return std::unexpected(Diagnostic{
                .code = DiagnosticCode::interface_mismatch,
                .message = "VertexInputs cannot have interpolation qualifiers",
                .notes = {"semantic: " + field.semantic_name},
                .origin = {},
                .generated_source = {},
            });
        }

        const bool varying = field.builtin == Builtin::none &&
            ((module.stage == StageKind::vertex && direction == InterfaceDirection::output) ||
             (module.stage == StageKind::fragment && direction == InterfaceDirection::input));
        if (varying && integral_interface_type(type) && field.interpolation != Interpolation::flat) {
            return std::unexpected(Diagnostic{
                .code = DiagnosticCode::interface_mismatch,
                .message = "integer and boolean varyings must use shader::flat",
                .notes = {"semantic: " + field.semantic_name},
                .origin = {},
                .generated_source = {},
            });
        }
    }
    return {};
}

} // namespace detail

template<Interface Inputs>
class InputView {
public:
    explicit InputView(FunctionBuilder& builder) noexcept : builder_(&builder) {}

    template<class Semantic>
        requires RecordContains<Inputs, Semantic>
    [[nodiscard]] dsl::Expr<record_value_t<Inputs, Semantic>> get(
        Semantic = {},
        std::source_location location = std::source_location::current()) const
    {
        using Result = record_value_t<Inputs, Semantic>;
        constexpr auto index = record_field_index_v<Inputs, Semantic>;
        return dsl::Expr<Result>{
            *builder_,
            builder_->operation(
                OpCode::input,
                builder_->template type<Result>(),
                {},
                InterfacePayload{static_cast<u32>(index)},
                Effect::input_read,
                location)
        };
    }

private:
    FunctionBuilder* builder_;
};

// A lightweight handle to renderer-supplied camera parameters. Like Expr, it
// only records typed reads in the current shader builder; it contains no
// backend object and performs no upload itself.
class CameraParameters final {
public:
    [[nodiscard]] dsl::Float4x4 view_projection(
        std::source_location location = std::source_location::current()) const
    {
        constexpr auto kind = ParameterKind::camera_view_projection;
        auto& parameters = builder_->module().parameters;
        auto found = std::ranges::find(parameters, kind, &ParameterField::kind);
        u32 index{};
        const auto type = builder_->type<Mat4>();
        if (found == parameters.end()) {
            index = static_cast<u32>(parameters.size());
            parameters.push_back(ParameterField{
                .kind = kind,
                .type = type,
                .location = std::nullopt,
            });
        } else {
            index = static_cast<u32>(std::distance(parameters.begin(), found));
        }

        return dsl::Float4x4{
            *builder_,
            builder_->operation(
                OpCode::parameter,
                type,
                {},
                ParameterPayload{index},
                Effect::parameter_read,
                location)
        };
    }

    [[nodiscard]] dsl::Float4 project(
        dsl::Float3 world_position,
        std::source_location location = std::source_location::current()) const
    {
        // Sequence construction explicitly: C++ does not otherwise prescribe
        // whether the two operands of operator* are evaluated left-to-right.
        const auto matrix = view_projection(location);
        const auto homogeneous_position = dsl::vec4(world_position, 1.0F);
        return matrix * homogeneous_position;
    }

private:
    explicit CameraParameters(FunctionBuilder& builder) noexcept
        : builder_(&builder)
    {}

    FunctionBuilder* builder_;

    template<StageKind, Interface, Interface>
    friend class StageContext;
};

template<StageKind Stage, Interface Inputs, Interface Outputs>
class StageContext {
public:
    explicit StageContext(FunctionBuilder& builder) noexcept : builder_(&builder) {}

    [[nodiscard]] InputView<Inputs> inputs() noexcept { return InputView<Inputs>{*builder_}; }
    [[nodiscard]] InputView<Inputs> in() noexcept { return inputs(); }
    [[nodiscard]] CameraParameters camera() noexcept { return CameraParameters{*builder_}; }

    template<class Semantic>
        requires RecordContains<Inputs, Semantic>
    [[nodiscard]] dsl::Expr<record_value_t<Inputs, Semantic>> input(
        Semantic semantic,
        std::source_location location = std::source_location::current())
    {
        return inputs().get(semantic, location);
    }

    template<class Semantic>
        requires RecordContains<Inputs, Semantic>
    [[nodiscard]] dsl::Expr<record_value_t<Inputs, Semantic>> input(
        std::source_location location = std::source_location::current())
    {
        return input(Semantic{}, location);
    }

    template<dsl::NamedFieldExpression... Fields>
        requires (sizeof...(Fields) > 0)
    [[nodiscard]] dsl::Expr<Outputs> output(Fields... fields)
    {
        return dsl::make<Outputs>(std::move(fields)...);
    }

    template<Value T>
        requires (!Interface<T>)
    [[nodiscard]] dsl::Expr<T> constant(const T& value)
    {
        return dsl::detail::literal(*builder_, value);
    }

    // Gives a diagnostic tool a stable semantic handle for an otherwise
    // internal expression. This records metadata only; ordinary shader
    // emission and execution are unchanged.
    template<gfx::SemanticType Semantic, Value T>
        requires std::same_as<gfx::semantic_value_t<Semantic>, T>
    void observe(
        Semantic,
        dsl::Expr<T> value,
        std::source_location location = std::source_location::current())
    {
        builder_->observe(
            typeid(Semantic),
            typeid(T),
            shader::detail::type_name<Semantic>(),
            builder_->template type<T>(),
            value.id(),
            location);
    }

    template<gfx::SemanticType Semantic, Value T>
        requires std::same_as<gfx::semantic_value_t<Semantic>, T>
    void observe(
        dsl::Expr<T> value,
        std::source_location location = std::source_location::current())
    {
        observe(Semantic{}, value, location);
    }

    [[nodiscard]] dsl::Int vertex_index() requires (Stage == StageKind::vertex)
    {
        return builtin_input<VertexIndex>();
    }

    [[nodiscard]] dsl::Int instance_index() requires (Stage == StageKind::vertex)
    {
        return builtin_input<InstanceIndex>();
    }

    [[nodiscard]] dsl::Float4 fragment_coordinate() requires (Stage == StageKind::fragment)
    {
        return builtin_input<FragmentCoordinate>();
    }

    [[nodiscard]] dsl::Bool front_facing() requires (Stage == StageKind::fragment)
    {
        return builtin_input<FrontFacing>();
    }

private:
    template<class Semantic>
    [[nodiscard]] dsl::Expr<typename Semantic::value_type> builtin_input()
    {
        using Result = typename Semantic::value_type;
        constexpr auto builtin = detail::builtin_traits<Semantic>::value;
        auto& fields = builder_->module().inputs;
        u32 index = 0;
        for (; index < fields.size(); ++index) {
            if (fields[index].builtin == builtin) {
                break;
            }
        }
        if (index == fields.size()) {
            fields.push_back(InterfaceField{
                .semantic_type = typeid(Semantic),
                .value_type = typeid(Result),
                .semantic_name = shader::detail::type_name<Semantic>(),
                .type = builder_->template type<Result>(),
                .interpolation = Interpolation::none,
                .builtin = builtin,
                .builtin_index = 0,
                .location = std::nullopt,
            });
        }
        return dsl::Expr<Result>{
            *builder_,
            builder_->operation(
                OpCode::input,
                builder_->template type<Result>(),
                {},
                InterfacePayload{index},
                Effect::input_read)
        };
    }

    FunctionBuilder* builder_;
};

namespace detail {

template<StageKind Stage, Interface Inputs, Interface Outputs, class Body>
[[nodiscard]] Result<ShaderStage> build_stage(std::string debug_name, Body&& body)
{
    static_assert(Inputs::stage == Stage && Inputs::direction == InterfaceDirection::input,
                  "the input schema does not belong to this shader stage");
    static_assert(Outputs::stage == Stage && Outputs::direction == InterfaceDirection::output,
                  "the output schema does not belong to this shader stage");

    if (auto result = validate_builtins<Inputs>(); !result) {
        return std::unexpected(std::move(result.error()));
    }
    if (auto result = validate_builtins<Outputs>(); !result) {
        return std::unexpected(std::move(result.error()));
    }

    ModuleIR module;
    module.stage = Stage;
    module.debug_name = std::move(debug_name);
    FunctionBuilder builder{module};
    describe_interface<Inputs>(builder, module.inputs);
    describe_interface<Outputs>(builder, module.outputs);
    if (auto validity = validate_interface_types(module, module.inputs, InterfaceDirection::input); !validity) {
        return std::unexpected(std::move(validity.error()));
    }
    if (auto validity = validate_interface_types(module, module.outputs, InterfaceDirection::output); !validity) {
        return std::unexpected(std::move(validity.error()));
    }

    StageContext<Stage, Inputs, Outputs> context{builder};
    using ResultExpression = std::invoke_result_t<
        Body,
        StageContext<Stage, Inputs, Outputs>&>;
    static_assert(std::same_as<std::remove_cvref_t<ResultExpression>, dsl::Expr<Outputs>>,
                  "a shader stage must return dsl::Expr<its complete Outputs schema>");

    auto result = std::invoke(std::forward<Body>(body), context);
    if (result.builder() != &builder || !builder.owns(result.id())) {
        return std::unexpected(Diagnostic{
            .code = DiagnosticCode::mixed_builders,
            .message = "shader stage returned an expression from another builder",
            .notes = {},
            .origin = {},
            .generated_source = {},
        });
    }

    shader::detail::for_each_type<typename Outputs::semantics>([&]<class Semantic> {
        constexpr auto index = record_field_index_v<Outputs, Semantic>;
        const auto value = result.get(Semantic{});
        const std::array operands{value.id()};
        builder.statement(
            OpCode::stage_output,
            operands,
            InterfacePayload{static_cast<u32>(index)},
            Effect::storage_write);
    });
    builder.statement(OpCode::return_, {}, {}, Effect::termination);

    if (builder.diagnostic()) {
        return std::unexpected(*builder.diagnostic());
    }
    if (auto validity = validate(module); !validity) {
        return std::unexpected(std::move(validity.error()));
    }
    optimize(module);
    if (auto validity = validate(module); !validity) {
        return std::unexpected(std::move(validity.error()));
    }
    return ShaderStageAccess::from_ir(std::move(module));
}

} // namespace detail

template<Interface Inputs, Interface Outputs, class Body>
[[nodiscard]] Result<ShaderStage> vertex(std::string debug_name, Body&& body)
{
    return detail::build_stage<StageKind::vertex, Inputs, Outputs>(
        std::move(debug_name), std::forward<Body>(body));
}

template<Interface Inputs, Interface Outputs, class Body>
[[nodiscard]] Result<ShaderStage> vertex(Body&& body)
{
    return vertex<Inputs, Outputs>("vertex_main", std::forward<Body>(body));
}

template<Interface Inputs, Interface Outputs, class Body>
[[nodiscard]] Result<ShaderStage> fragment(std::string debug_name, Body&& body)
{
    return detail::build_stage<StageKind::fragment, Inputs, Outputs>(
        std::move(debug_name), std::forward<Body>(body));
}

template<Interface Inputs, Interface Outputs, class Body>
[[nodiscard]] Result<ShaderStage> fragment(Body&& body)
{
    return fragment<Inputs, Outputs>("fragment_main", std::forward<Body>(body));
}

} // namespace vng::shader
