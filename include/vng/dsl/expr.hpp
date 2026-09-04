#pragma once

#include <vng/shader/ir.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <source_location>
#include <tuple>
#include <type_traits>
#include <utility>

namespace vng::dsl {

template<shader::Value T>
class Expr;

namespace detail {

template<class T>
struct expression_traits;

template<shader::Value T>
struct expression_traits<Expr<T>> {
    using value_type = T;
};

template<class T>
concept Expression = requires { typename expression_traits<std::remove_cvref_t<T>>::value_type; };

template<class T>
using expression_value_t = typename expression_traits<std::remove_cvref_t<T>>::value_type;

template<class T>
inline constexpr bool numeric_scalar_v =
    std::is_same_v<T, i32> || std::is_same_v<T, u32> || std::is_same_v<T, f32>;

template<class T, bool = is_vector_v<T>>
struct vector_info {
    static constexpr bool value = false;
};

template<class T>
struct vector_info<T, true> {
    using element = typename vector_traits<T>::value_type;
    static constexpr std::size_t size = vector_traits<T>::component_count;
    static constexpr bool value = true;
};

template<class T>
inline constexpr bool numeric_vector_v =
    vector_info<T>::value && [] {
        if constexpr (vector_info<T>::value) {
            return numeric_scalar_v<typename vector_info<T>::element>;
        }
        return false;
    }();

template<class T>
inline constexpr bool float_vector_v =
    vector_info<T>::value && [] {
        if constexpr (vector_info<T>::value) {
            return std::is_same_v<typename vector_info<T>::element, f32>;
        }
        return false;
    }();

template<class T>
inline constexpr bool arithmetic_value_v = numeric_scalar_v<T> || numeric_vector_v<T> || shader::detail::is_matrix_v<T>;

template<class T>
inline constexpr bool integral_value_v =
    std::is_same_v<T, i32> || std::is_same_v<T, u32> ||
    (vector_info<T>::value && [] {
        if constexpr (vector_info<T>::value) {
            using Element = typename vector_info<T>::element;
            return std::is_same_v<Element, i32> || std::is_same_v<Element, u32>;
        }
        return false;
    }());

template<class T>
inline constexpr bool unsigned_value_v =
    std::is_same_v<T, u32> ||
    (vector_info<T>::value && [] {
        if constexpr (vector_info<T>::value) {
            return std::is_same_v<typename vector_info<T>::element, u32>;
        }
        return false;
    }());

template<class T>
struct host_or_expression_value {
    using type = std::remove_cvref_t<T>;
};

template<Expression T>
struct host_or_expression_value<T> {
    using type = expression_value_t<T>;
};

template<class T>
using host_or_expression_value_t = typename host_or_expression_value<T>::type;

template<class T>
concept ShaderOperand = Expression<T> || shader::Value<std::remove_cvref_t<T>>;

template<class Left, class Right>
concept HasExpressionOperand = Expression<Left> || Expression<Right>;

template<class Left, class Right>
struct arithmetic_result {
    using type = void;
};

template<class T>
    requires arithmetic_value_v<T>
struct arithmetic_result<T, T> {
    using type = T;
};

template<class Element, std::size_t Size>
    requires numeric_scalar_v<Element>
struct arithmetic_result<Vector<Element, Size>, Element> {
    using type = Vector<Element, Size>;
};

template<class Element, std::size_t Size>
    requires numeric_scalar_v<Element>
struct arithmetic_result<Element, Vector<Element, Size>> {
    using type = Vector<Element, Size>;
};

template<std::size_t Size>
struct arithmetic_result<Matrix<Size>, f32> {
    using type = Matrix<Size>;
};

template<std::size_t Size>
struct arithmetic_result<f32, Matrix<Size>> {
    using type = Matrix<Size>;
};

template<class Left, class Right>
using arithmetic_result_t = typename arithmetic_result<Left, Right>::type;

template<class Left, class Right>
concept ArithmeticPair = !std::is_void_v<arithmetic_result_t<Left, Right>>;

template<class Left, class Right>
struct multiply_result : arithmetic_result<Left, Right> {};

template<std::size_t Size>
struct multiply_result<Matrix<Size>, Vector<f32, Size>> {
    using type = Vector<f32, Size>;
};

template<std::size_t Size>
struct multiply_result<Vector<f32, Size>, Matrix<Size>> {
    using type = Vector<f32, Size>;
};

template<class Left, class Right>
using multiply_result_t = typename multiply_result<Left, Right>::type;

template<class T>
struct comparison_result {
    using type = bool;
};

template<class Element, std::size_t Size>
struct comparison_result<Vector<Element, Size>> {
    using type = Vector<bool, Size>;
};

template<class T>
using comparison_result_t = typename comparison_result<T>::type;

template<class To, class From, bool = is_vector_v<To> && is_vector_v<From>>
struct castable_vector : std::false_type {};

template<class To, class From>
struct castable_vector<To, From, true>
    : std::bool_constant<vector_traits<To>::component_count == vector_traits<From>::component_count> {};

template<class To, class From>
inline constexpr bool castable_v =
    (shader::detail::is_scalar_value_v<To> && shader::detail::is_scalar_value_v<From>) ||
    castable_vector<To, From>::value;

template<class Condition, class Value, bool = is_vector_v<Condition> && is_vector_v<Value>>
struct vector_selectable : std::false_type {};

template<class Condition, class Value>
struct vector_selectable<Condition, Value, true>
    : std::bool_constant<
          std::is_same_v<typename vector_traits<Condition>::value_type, bool> &&
          vector_traits<Condition>::component_count == vector_traits<Value>::component_count> {};

[[nodiscard]] inline shader::Diagnostic mixed_builder_diagnostic(std::source_location location)
{
    return shader::Diagnostic{
        .code = shader::DiagnosticCode::mixed_builders,
        .message = "an expression cannot combine values from different shader builders",
        .notes = {},
        .origin = shader::SourceOrigin{
            .file = location.file_name(),
            .function = location.function_name(),
            .line = location.line(),
            .column = location.column(),
        },
        .generated_source = {},
    };
}

template<shader::Value T>
[[nodiscard]] Expr<T> literal(shader::FunctionBuilder& builder, const T& value);

template<shader::Value T>
[[nodiscard]] Expr<T> as_expression(shader::FunctionBuilder& builder, const Expr<T>& expression);

template<shader::Value T>
[[nodiscard]] Expr<T> as_expression(shader::FunctionBuilder& builder, const T& value)
{
    return literal(builder, value);
}

template<class First, class... Rest>
[[nodiscard]] shader::FunctionBuilder* common_builder(
    const First& first,
    const Rest&... rest)
{
    shader::FunctionBuilder* result = nullptr;
    u64 newest_owner = 0;
    auto inspect = [&](const auto& value) {
        if constexpr (Expression<decltype(value)>) {
            // Ownership generations are monotonic. Selecting the newest expression
            // lets a live later builder diagnose an older escaped expression without
            // dereferencing the escaped builder pointer.
            if (value.id().owner > newest_owner) {
                newest_owner = value.id().owner;
                result = value.builder();
            }
        }
    };
    inspect(first);
    (inspect(rest), ...);
    return result;
}

template<class... Arguments>
[[nodiscard]] bool has_foreign_builder(
    shader::FunctionBuilder* builder,
    const Arguments&... arguments)
{
    const bool foreign = ([&] {
        if constexpr (Expression<Arguments>) {
            return arguments.builder() != builder || !builder->owns(arguments.id());
        }
        return false;
    }() || ...);
    if (foreign) {
        builder->fail(mixed_builder_diagnostic(std::source_location::current()));
    }
    return foreign;
}

template<class Result, class Left, class Right>
[[nodiscard]] Expr<Result> binary(shader::OpCode opcode, const Left& left, const Right& right)
{
    auto* builder = common_builder(left, right);
    if (has_foreign_builder(builder, left, right)) {
        return Expr<Result>{*builder, builder->poison(builder->template type<Result>())};
    }
    auto l = as_expression(*builder, left);
    auto r = as_expression(*builder, right);
    const std::array operands{l.id(), r.id()};
    return Expr<Result>{*builder, builder->operation(opcode, builder->template type<Result>(), operands)};
}

template<class Result, class... Arguments>
[[nodiscard]] Expr<Result> intrinsic(shader::OpCode opcode, const Arguments&... arguments)
{
    auto* builder = common_builder(arguments...);
    if (has_foreign_builder(builder, arguments...)) {
        return Expr<Result>{*builder, builder->poison(builder->template type<Result>())};
    }
    std::array<shader::ValueId, sizeof...(Arguments)> operands{
        as_expression(*builder, arguments).id()...
    };
    return Expr<Result>{*builder, builder->operation(opcode, builder->template type<Result>(), operands)};
}

template<class Semantic, class Tuple, std::size_t Index = 0>
[[nodiscard]] auto field_expression(const Tuple& tuple)
{
    static_assert(Index < std::tuple_size_v<Tuple>, "missing field in dsl::make");
    using Field = std::tuple_element_t<Index, Tuple>;
    if constexpr (std::is_same_v<typename Field::semantic_type, Semantic>) {
        return std::get<Index>(tuple).expression;
    } else {
        return field_expression<Semantic, Tuple, Index + 1>(tuple);
    }
}

template<class Semantic, class... Fields>
inline constexpr std::size_t field_count_v =
    (std::size_t{0} + ... + (std::is_same_v<Semantic, typename Fields::semantic_type> ? 1U : 0U));

template<class Semantics, class... Fields>
struct complete_field_set;

template<class... Semantics, class... Fields>
struct complete_field_set<gfx::TypeList<Semantics...>, Fields...>
    : std::bool_constant<((field_count_v<Semantics, Fields...> == 1) && ...)> {};

template<class Semantics, class... Fields>
inline constexpr bool complete_field_set_v = complete_field_set<Semantics, Fields...>::value;

} // namespace detail

template<shader::Value T>
class Expr {
public:
    using value_type = T;

    Expr() = delete;
    constexpr Expr(const Expr&) noexcept = default;
    constexpr Expr(Expr&&) noexcept = default;
    constexpr Expr& operator=(const Expr&) noexcept = default;
    constexpr Expr& operator=(Expr&&) noexcept = default;

    constexpr Expr(shader::FunctionBuilder& builder, shader::ValueId id) noexcept
        : builder_(&builder), id_(id) {}

    [[nodiscard]] constexpr shader::FunctionBuilder* builder() const noexcept { return builder_; }
    [[nodiscard]] constexpr shader::ValueId id() const noexcept { return id_; }

    explicit operator bool() const = delete;

    template<class Semantic>
        requires shader::RecordContains<T, Semantic>
    [[nodiscard]] auto get(
        Semantic = {},
        std::source_location location = std::source_location::current()) const
    {
        using Result = shader::record_value_t<T, Semantic>;
        constexpr auto field_index = shader::record_field_index_v<T, Semantic>;
        const std::array operands{id_};
        return Expr<Result>{
            *builder_,
            builder_->operation(
                shader::OpCode::extract_field,
                builder_->template type<Result>(),
                operands,
                shader::FieldPayload{static_cast<u32>(field_index)},
                shader::Effect::pure,
                location)
        };
    }

    template<std::size_t... Components>
        requires (is_vector_v<T> && sizeof...(Components) >= 1 && sizeof...(Components) <= 4 &&
                  ((Components < vector_traits<T>::component_count) && ...))
    [[nodiscard]] auto swizzle(std::source_location location = std::source_location::current()) const
    {
        using Element = typename vector_traits<T>::value_type;
        using Result = std::conditional_t<sizeof...(Components) == 1,
                                          Element,
                                          Vector<Element, sizeof...(Components)>>;
        const std::array operands{id_};
        return Expr<Result>{
            *builder_,
            builder_->operation(
                shader::OpCode::swizzle,
                builder_->template type<Result>(),
                operands,
                shader::SwizzlePayload{{static_cast<u32>(Components)...}},
                shader::Effect::pure,
                location)
        };
    }

    [[nodiscard]] auto x() const requires (is_vector_v<T>) { return swizzle<0>(); }
    [[nodiscard]] auto y() const requires (is_vector_v<T> && vector_traits<T>::component_count >= 2) { return swizzle<1>(); }
    [[nodiscard]] auto z() const requires (is_vector_v<T> && vector_traits<T>::component_count >= 3) { return swizzle<2>(); }
    [[nodiscard]] auto w() const requires (is_vector_v<T> && vector_traits<T>::component_count >= 4) { return swizzle<3>(); }
    [[nodiscard]] auto xy() const requires (is_vector_v<T> && vector_traits<T>::component_count >= 2) { return swizzle<0, 1>(); }
    [[nodiscard]] auto xyz() const requires (is_vector_v<T> && vector_traits<T>::component_count >= 3) { return swizzle<0, 1, 2>(); }

private:
    shader::FunctionBuilder* builder_;
    shader::ValueId id_;
};

using Bool = Expr<bool>;
using Bool2 = Expr<Vector<bool, 2>>;
using Bool3 = Expr<Vector<bool, 3>>;
using Bool4 = Expr<Vector<bool, 4>>;
using Int = Expr<i32>;
using UInt = Expr<u32>;
using Float = Expr<f32>;
using Float2 = Expr<Vec2>;
using Float3 = Expr<Vec3>;
using Float4 = Expr<Vec4>;
using Int2 = Expr<IVec2>;
using Int3 = Expr<IVec3>;
using Int4 = Expr<IVec4>;
using UInt2 = Expr<UVec2>;
using UInt3 = Expr<UVec3>;
using UInt4 = Expr<UVec4>;
using Float3x3 = Expr<Mat3>;
using Float4x4 = Expr<Mat4>;

namespace detail {

template<shader::Value T>
Expr<T> as_expression(shader::FunctionBuilder& builder, const Expr<T>& expression)
{
    if (expression.builder() != &builder || !builder.owns(expression.id())) {
        builder.fail(mixed_builder_diagnostic(std::source_location::current()));
        return Expr<T>{builder, builder.poison(builder.template type<T>())};
    }
    return expression;
}

template<shader::Value T>
Expr<T> literal(shader::FunctionBuilder& builder, const T& value)
{
    if constexpr (shader::detail::is_scalar_value_v<T>) {
        return Expr<T>{
            builder,
            builder.operation(
                shader::OpCode::constant,
                builder.template type<T>(),
                {},
                shader::ConstantPayload{value})
        };
    } else if constexpr (is_vector_v<T>) {
        using Element = typename vector_traits<T>::value_type;
        std::array<shader::ValueId, vector_traits<T>::component_count> components{};
        for (std::size_t index = 0; index < components.size(); ++index) {
            components[index] = literal<Element>(builder, value[index]).id();
        }
        return Expr<T>{builder, builder.operation(shader::OpCode::construct, builder.template type<T>(), components)};
    } else if constexpr (shader::detail::is_matrix_v<T>) {
        std::array<shader::ValueId, shader::detail::matrix_traits<T>::columns> columns{};
        for (std::size_t index = 0; index < columns.size(); ++index) {
            columns[index] = literal(builder, value[index]).id();
        }
        return Expr<T>{builder, builder.operation(shader::OpCode::construct, builder.template type<T>(), columns)};
    } else if constexpr (gfx::is_record_v<T>) {
        using Semantics = typename shader::detail::record_traits<T>::semantics;
        std::array<shader::ValueId, shader::detail::type_list_size<Semantics>::value> fields{};
        std::size_t index = 0;
        shader::detail::for_each_type<Semantics>([&]<class Semantic> {
            fields[index++] = literal(builder, value.get(Semantic{})).id();
        });
        return Expr<T>{builder, builder.operation(shader::OpCode::construct, builder.template type<T>(), fields)};
    } else {
        static_assert(!shader::Interface<T>, "interface values must be built with dsl::make");
    }
}

} // namespace detail

template<class T>
concept Expression = detail::Expression<T>;

template<class T>
using expression_value_t = detail::expression_value_t<T>;

template<class Left, class Right>
    requires detail::ShaderOperand<Left> && detail::ShaderOperand<Right> &&
             detail::HasExpressionOperand<Left, Right> &&
             detail::ArithmeticPair<detail::host_or_expression_value_t<Left>, detail::host_or_expression_value_t<Right>>
[[nodiscard]] auto operator+(const Left& left, const Right& right)
{
    using Result = detail::arithmetic_result_t<detail::host_or_expression_value_t<Left>, detail::host_or_expression_value_t<Right>>;
    return detail::binary<Result>(shader::OpCode::add, left, right);
}

template<class Left, class Right>
    requires detail::ShaderOperand<Left> && detail::ShaderOperand<Right> &&
             detail::HasExpressionOperand<Left, Right> &&
             detail::ArithmeticPair<detail::host_or_expression_value_t<Left>, detail::host_or_expression_value_t<Right>>
[[nodiscard]] auto operator-(const Left& left, const Right& right)
{
    using Result = detail::arithmetic_result_t<detail::host_or_expression_value_t<Left>, detail::host_or_expression_value_t<Right>>;
    return detail::binary<Result>(shader::OpCode::subtract, left, right);
}

template<class Left, class Right>
    requires detail::ShaderOperand<Left> && detail::ShaderOperand<Right> &&
             detail::HasExpressionOperand<Left, Right> &&
             (!std::is_void_v<detail::multiply_result_t<detail::host_or_expression_value_t<Left>, detail::host_or_expression_value_t<Right>>>)
[[nodiscard]] auto operator*(const Left& left, const Right& right)
{
    using Result = detail::multiply_result_t<detail::host_or_expression_value_t<Left>, detail::host_or_expression_value_t<Right>>;
    return detail::binary<Result>(shader::OpCode::multiply, left, right);
}

template<class Left, class Right>
    requires detail::ShaderOperand<Left> && detail::ShaderOperand<Right> &&
             detail::HasExpressionOperand<Left, Right> &&
             detail::ArithmeticPair<detail::host_or_expression_value_t<Left>, detail::host_or_expression_value_t<Right>>
[[nodiscard]] auto operator/(const Left& left, const Right& right)
{
    using Result = detail::arithmetic_result_t<detail::host_or_expression_value_t<Left>, detail::host_or_expression_value_t<Right>>;
    return detail::binary<Result>(shader::OpCode::divide, left, right);
}

template<class Left, class Right>
    requires detail::ShaderOperand<Left> && detail::ShaderOperand<Right> &&
             detail::HasExpressionOperand<Left, Right> &&
             std::is_same_v<detail::host_or_expression_value_t<Left>, detail::host_or_expression_value_t<Right>> &&
             detail::integral_value_v<detail::host_or_expression_value_t<Left>>
[[nodiscard]] auto operator%(const Left& left, const Right& right)
{
    using Result = detail::host_or_expression_value_t<Left>;
    return detail::binary<Result>(shader::OpCode::remainder, left, right);
}

template<class Left, class Right>
concept IntegralPair = detail::ShaderOperand<Left> && detail::ShaderOperand<Right> &&
    detail::HasExpressionOperand<Left, Right> &&
    std::is_same_v<detail::host_or_expression_value_t<Left>, detail::host_or_expression_value_t<Right>> &&
    detail::integral_value_v<detail::host_or_expression_value_t<Left>>;

template<class Left, class Right> requires IntegralPair<Left, Right>
[[nodiscard]] auto operator&(const Left& left, const Right& right)
{
    using Result = detail::host_or_expression_value_t<Left>;
    return detail::binary<Result>(shader::OpCode::bit_and, left, right);
}

template<class Left, class Right> requires IntegralPair<Left, Right>
[[nodiscard]] auto operator|(const Left& left, const Right& right)
{
    using Result = detail::host_or_expression_value_t<Left>;
    return detail::binary<Result>(shader::OpCode::bit_or, left, right);
}

template<class Left, class Right> requires IntegralPair<Left, Right>
[[nodiscard]] auto operator^(const Left& left, const Right& right)
{
    using Result = detail::host_or_expression_value_t<Left>;
    return detail::binary<Result>(shader::OpCode::bit_xor, left, right);
}

template<class Left, class Right> requires IntegralPair<Left, Right>
[[nodiscard]] auto operator<<(const Left& left, const Right& right)
{
    using Result = detail::host_or_expression_value_t<Left>;
    return detail::binary<Result>(shader::OpCode::shift_left, left, right);
}

template<class Left, class Right> requires IntegralPair<Left, Right>
[[nodiscard]] auto operator>>(const Left& left, const Right& right)
{
    using Result = detail::host_or_expression_value_t<Left>;
    return detail::binary<Result>(shader::OpCode::shift_right, left, right);
}

template<shader::Value T>
    requires detail::arithmetic_value_v<T> && (!detail::unsigned_value_v<T>)
[[nodiscard]] Expr<T> operator-(Expr<T> value)
{
    const std::array operands{value.id()};
    return Expr<T>{*value.builder(), value.builder()->operation(shader::OpCode::negate, value.builder()->template type<T>(), operands)};
}

[[nodiscard]] inline Bool operator!(Bool value)
{
    const std::array operands{value.id()};
    return Bool{*value.builder(), value.builder()->operation(shader::OpCode::logical_not, value.builder()->template type<bool>(), operands)};
}

template<shader::Value T>
    requires detail::integral_value_v<T>
[[nodiscard]] Expr<T> operator~(Expr<T> value)
{
    const std::array operands{value.id()};
    return Expr<T>{*value.builder(), value.builder()->operation(shader::OpCode::bit_not, value.builder()->template type<T>(), operands)};
}

template<class Left, class Right>
    requires detail::ShaderOperand<Left> && detail::ShaderOperand<Right> &&
             detail::HasExpressionOperand<Left, Right> &&
             std::is_same_v<detail::host_or_expression_value_t<Left>, detail::host_or_expression_value_t<Right>> &&
             (!shader::detail::RecordValue<detail::host_or_expression_value_t<Left>>)
[[nodiscard]] auto operator==(const Left& left, const Right& right)
{
    using T = detail::host_or_expression_value_t<Left>;
    return detail::binary<detail::comparison_result_t<T>>(shader::OpCode::equal, left, right);
}

template<class Left, class Right>
    requires detail::ShaderOperand<Left> && detail::ShaderOperand<Right> &&
             detail::HasExpressionOperand<Left, Right> &&
             std::is_same_v<detail::host_or_expression_value_t<Left>, detail::host_or_expression_value_t<Right>> &&
             (!shader::detail::RecordValue<detail::host_or_expression_value_t<Left>>)
[[nodiscard]] auto operator!=(const Left& left, const Right& right)
{
    using T = detail::host_or_expression_value_t<Left>;
    return detail::binary<detail::comparison_result_t<T>>(shader::OpCode::not_equal, left, right);
}

template<class Left, class Right>
concept OrderedPair = detail::ShaderOperand<Left> && detail::ShaderOperand<Right> &&
    detail::HasExpressionOperand<Left, Right> &&
    std::is_same_v<detail::host_or_expression_value_t<Left>, detail::host_or_expression_value_t<Right>> &&
    (detail::numeric_scalar_v<detail::host_or_expression_value_t<Left>> ||
     detail::numeric_vector_v<detail::host_or_expression_value_t<Left>>);

template<class Left, class Right> requires OrderedPair<Left, Right>
[[nodiscard]] auto operator<(const Left& left, const Right& right)
{
    using T = detail::host_or_expression_value_t<Left>;
    return detail::binary<detail::comparison_result_t<T>>(shader::OpCode::less, left, right);
}

template<class Left, class Right> requires OrderedPair<Left, Right>
[[nodiscard]] auto operator<=(const Left& left, const Right& right)
{
    using T = detail::host_or_expression_value_t<Left>;
    return detail::binary<detail::comparison_result_t<T>>(shader::OpCode::less_equal, left, right);
}

template<class Left, class Right> requires OrderedPair<Left, Right>
[[nodiscard]] auto operator>(const Left& left, const Right& right)
{
    using T = detail::host_or_expression_value_t<Left>;
    return detail::binary<detail::comparison_result_t<T>>(shader::OpCode::greater, left, right);
}

template<class Left, class Right> requires OrderedPair<Left, Right>
[[nodiscard]] auto operator>=(const Left& left, const Right& right)
{
    using T = detail::host_or_expression_value_t<Left>;
    return detail::binary<detail::comparison_result_t<T>>(shader::OpCode::greater_equal, left, right);
}

template<shader::Value To, shader::Value From>
    requires detail::castable_v<To, From>
[[nodiscard]] Expr<To> cast(Expr<From> value)
{
    const std::array operands{value.id()};
    return Expr<To>{*value.builder(), value.builder()->operation(shader::OpCode::cast, value.builder()->template type<To>(), operands)};
}

template<gfx::SemanticType Semantic, shader::Value T>
struct FieldExpression {
    using semantic_type = Semantic;
    using value_type = T;
    Expr<T> expression;
};

template<class T>
struct is_field_expression : std::false_type {};

template<gfx::SemanticType Semantic, shader::Value T>
struct is_field_expression<FieldExpression<Semantic, T>> : std::true_type {};

template<class T>
concept NamedFieldExpression =
    is_field_expression<std::remove_cvref_t<T>>::value;

template<gfx::SemanticType Semantic, shader::Value T>
[[nodiscard]] FieldExpression<Semantic, T> field(Expr<T> value)
{
    return {value};
}

template<gfx::SemanticType Semantic, shader::Value T>
[[nodiscard]] FieldExpression<Semantic, T> field(Semantic, Expr<T> value)
{
    return field<Semantic>(value);
}

template<class Record, NamedFieldExpression... Fields>
    requires shader::detail::RecordValue<Record> && (sizeof...(Fields) > 0)
[[nodiscard]] Expr<Record> make(Fields... fields)
{
    using Semantics = typename shader::detail::record_traits<Record>::semantics;
    constexpr bool fields_are_unique =
        ((detail::field_count_v<typename Fields::semantic_type, Fields...> == 1) && ...);
    constexpr bool field_count_matches =
        sizeof...(Fields) == shader::detail::type_list_size<Semantics>::value;
    constexpr bool field_set_is_complete =
        detail::complete_field_set_v<Semantics, Fields...>;

    if constexpr (!fields_are_unique) {
        static_assert(fields_are_unique, "dsl::make fields must have unique semantics");
        std::unreachable();
    } else if constexpr (!field_count_matches) {
        static_assert(field_count_matches,
                      "dsl::make must initialize every record field exactly once");
        std::unreachable();
    } else if constexpr (!field_set_is_complete) {
        static_assert(field_set_is_complete,
                      "dsl::make is missing a required semantic or contains an unknown semantic");
        std::unreachable();
    } else {
        // Type and operand extraction are only instantiated after the semantic set is
        // known to be complete. This keeps malformed calls from recursing through
        // field_expression while the compiler is already reporting the real error.
        const auto tuple = std::tuple{fields...};
        shader::detail::for_each_type<Semantics>([&]<class Semantic> {
            using Actual = typename decltype(detail::field_expression<Semantic>(tuple))::value_type;
            using Expected = shader::record_value_t<Record, Semantic>;
            static_assert(std::is_same_v<Actual, Expected>, "dsl::make field has the wrong logical type");
        });

        auto* builder = std::apply(
            [](const auto&... values) { return detail::common_builder(values.expression...); },
            tuple);
        const bool has_foreign = std::apply(
            [builder](const auto&... values) {
                return detail::has_foreign_builder(builder, values.expression...);
            },
            tuple);
        if (has_foreign) {
            return Expr<Record>{*builder, builder->poison(builder->template type<Record>())};
        }
        std::array<shader::ValueId, sizeof...(Fields)> operands{};
        std::size_t index = 0;
        shader::detail::for_each_type<Semantics>([&]<class Semantic> {
            operands[index++] = detail::field_expression<Semantic>(tuple).id();
        });
        return Expr<Record>{
            *builder,
            builder->operation(shader::OpCode::construct, builder->template type<Record>(), operands)};
    }
}

} // namespace vng::dsl
