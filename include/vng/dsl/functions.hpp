#pragma once

#include <vng/dsl/expr.hpp>

#include <array>
#include <type_traits>

namespace vng::dsl {
namespace detail {

template<class... Arguments>
concept HasAnyExpression = (Expression<Arguments> || ...);

template<class T>
concept FloatValue = std::is_same_v<T, f32> || float_vector_v<T>;

template<class T>
concept NumericValue = numeric_scalar_v<T> || numeric_vector_v<T>;

template<class Result, class... Arguments>
    requires HasAnyExpression<Arguments...>
[[nodiscard]] Expr<Result> construct(const Arguments&... arguments)
{
    auto* builder = common_builder(arguments...);
    if (has_foreign_builder(builder, arguments...)) {
        return Expr<Result>{*builder, builder->poison(builder->template type<Result>())};
    }
    std::array<shader::ValueId, sizeof...(Arguments)> operands{
        as_expression(*builder, arguments).id()...
    };
    return Expr<Result>{
        *builder,
        builder->operation(shader::OpCode::construct, builder->template type<Result>(), operands)
    };
}

template<class T>
struct composite_components;

template<class E, std::size_t N>
struct composite_components<Vector<E, N>> {
    using type = E;
    static constexpr std::size_t count = N;
};

template<std::size_t N>
struct composite_components<Matrix<N>> {
    using type = Vector<f32, N>;
    static constexpr std::size_t count = N;
};

template<class T>
using component_t = typename composite_components<T>::type;

// Non-deduced wrappers ensure conversion happens at the original call site.
template<class T>
using operand_t = Operand<std::type_identity_t<T>>;
template<class T>
using constant_t = Constant<std::type_identity_t<T>>;

} // namespace detail

#define VNG_DSL_BINARY_INTRINSIC(NAME, OPCODE, REQUIREMENT, RESULT)                 \
    template<shader::Value T> requires (REQUIREMENT)                               \
    [[nodiscard]] Expr<RESULT> NAME(Expr<T> left, detail::operand_t<T> right)        \
    { return detail::binary<RESULT>(shader::OpCode::OPCODE, left, right); }         \
    template<shader::Value T> requires (REQUIREMENT)                               \
    [[nodiscard]] Expr<RESULT> NAME(detail::constant_t<T> left, Expr<T> right)      \
    { return detail::binary<RESULT>(shader::OpCode::OPCODE, left, right); }

VNG_DSL_BINARY_INTRINSIC(min, minimum, detail::NumericValue<T>, T)
VNG_DSL_BINARY_INTRINSIC(max, maximum, detail::NumericValue<T>, T)
VNG_DSL_BINARY_INTRINSIC(dot, dot, detail::float_vector_v<T>, f32)
VNG_DSL_BINARY_INTRINSIC(pow, power, detail::FloatValue<T>, T)
#undef VNG_DSL_BINARY_INTRINSIC

// Choose the first actual expression as the type/builder anchor. Constants
// before it and expression-or-constant operands after it make these overloads
// disjoint; host values never enter a generic forwarding function unchecked.
#define VNG_DSL_TERNARY_SAME(NAME, OPCODE, REQUIREMENT)                             \
    template<shader::Value T> requires (REQUIREMENT)                               \
    [[nodiscard]] Expr<T> NAME(Expr<T> a, detail::operand_t<T> b,                   \
        detail::operand_t<T> c)                                                   \
    { return detail::intrinsic<T>(shader::OpCode::OPCODE, a, b, c); }              \
    template<shader::Value T> requires (REQUIREMENT)                               \
    [[nodiscard]] Expr<T> NAME(detail::constant_t<T> a, Expr<T> b,                  \
        detail::operand_t<T> c)                                                   \
    { return detail::intrinsic<T>(shader::OpCode::OPCODE, a, b, c); }              \
    template<shader::Value T> requires (REQUIREMENT)                               \
    [[nodiscard]] Expr<T> NAME(detail::constant_t<T> a,                            \
        detail::constant_t<T> b, Expr<T> c)                                        \
    { return detail::intrinsic<T>(shader::OpCode::OPCODE, a, b, c); }

VNG_DSL_TERNARY_SAME(clamp, clamp, detail::NumericValue<T>)
VNG_DSL_TERNARY_SAME(mix, mix, detail::FloatValue<T>)
#undef VNG_DSL_TERNARY_SAME

template<class E, std::size_t N> requires detail::numeric_scalar_v<E>
[[nodiscard]] Expr<Vector<E, N>> clamp(Expr<Vector<E, N>> value,
    detail::operand_t<E> low, detail::operand_t<E> high)
{
    return detail::intrinsic<Vector<E, N>>(shader::OpCode::clamp, value, low, high);
}

// A vector-valued constant does not provide a deducible template argument.
// Spell out the supported widths while still deducing the scalar logical type.
#define VNG_DSL_CONSTANT_VECTOR_CLAMP(N)                                           \
    template<class E> requires detail::numeric_scalar_v<E>                        \
    [[nodiscard]] Expr<Vector<E, N>> clamp(detail::constant_t<Vector<E, N>> value, \
        Expr<E> low, detail::operand_t<E> high)                                   \
    { return detail::intrinsic<Vector<E, N>>(shader::OpCode::clamp, value, low, high); } \
    template<class E> requires detail::numeric_scalar_v<E>                        \
    [[nodiscard]] Expr<Vector<E, N>> clamp(detail::constant_t<Vector<E, N>> value, \
        detail::constant_t<E> low, Expr<E> high)                                  \
    { return detail::intrinsic<Vector<E, N>>(shader::OpCode::clamp, value, low, high); }

VNG_DSL_CONSTANT_VECTOR_CLAMP(2)
VNG_DSL_CONSTANT_VECTOR_CLAMP(3)
VNG_DSL_CONSTANT_VECTOR_CLAMP(4)
#undef VNG_DSL_CONSTANT_VECTOR_CLAMP

template<std::size_t N>
[[nodiscard]] Expr<Vector<f32, N>> mix(Expr<Vector<f32, N>> a,
    detail::operand_t<Vector<f32, N>> b, detail::Operand<f32> factor)
{
    return detail::intrinsic<Vector<f32, N>>(shader::OpCode::mix, a, b, factor);
}

template<std::size_t N>
[[nodiscard]] Expr<Vector<f32, N>> mix(detail::constant_t<Vector<f32, N>> a,
    Expr<Vector<f32, N>> b, detail::Operand<f32> factor)
{
    return detail::intrinsic<Vector<f32, N>>(shader::OpCode::mix, a, b, factor);
}

#define VNG_DSL_CONSTANT_VECTOR_MIX(T)                                             \
    [[nodiscard]] inline Expr<T> mix(detail::Constant<T> a, detail::Constant<T> b, \
        Float factor)                                                            \
    { return detail::intrinsic<T>(shader::OpCode::mix, a, b, factor); }

VNG_DSL_CONSTANT_VECTOR_MIX(Vec2)
VNG_DSL_CONSTANT_VECTOR_MIX(Vec3)
VNG_DSL_CONSTANT_VECTOR_MIX(Vec4)
#undef VNG_DSL_CONSTANT_VECTOR_MIX

[[nodiscard]] inline Float3 cross(Float3 left, Float3 right)
{
    return detail::binary<Vec3>(shader::OpCode::cross, left, right);
}

template<shader::Value T> requires detail::float_vector_v<T>
[[nodiscard]] Expr<T> normalize(Expr<T> value)
{
    return detail::intrinsic<T>(shader::OpCode::normalize, value);
}

template<shader::Value T> requires detail::FloatValue<T>
[[nodiscard]] Expr<T> sqrt(Expr<T> value)
{
    return detail::intrinsic<T>(shader::OpCode::square_root, value);
}

#define VNG_DSL_FLOAT_UNARY(NAME, OPCODE)                                        \
    template<shader::Value T> requires detail::FloatValue<T>                     \
    [[nodiscard]] Expr<T> NAME(Expr<T> value)                                    \
    { return detail::intrinsic<T>(shader::OpCode::OPCODE, value); }

VNG_DSL_FLOAT_UNARY(sin, sine)
VNG_DSL_FLOAT_UNARY(cos, cosine)
VNG_DSL_FLOAT_UNARY(floor, floor)
VNG_DSL_FLOAT_UNARY(fract, fract)
VNG_DSL_FLOAT_UNARY(exp, exponential)
#undef VNG_DSL_FLOAT_UNARY

template<shader::Value T>
    requires (detail::NumericValue<T> && !detail::unsigned_value_v<T>)
[[nodiscard]] Expr<T> abs(Expr<T> value)
{
    return detail::intrinsic<T>(shader::OpCode::absolute, value);
}

template<shader::Value Condition, shader::Value T>
    requires (std::same_as<Condition, bool> ||
              detail::vector_selectable<Condition, T>::value)
[[nodiscard]] Expr<T> select(Expr<Condition> condition, Expr<T> when_true,
    detail::operand_t<T> when_false)
{
    return detail::intrinsic<T>(shader::OpCode::select, condition, when_true, when_false);
}

template<shader::Value Condition, shader::Value T>
    requires (std::same_as<Condition, bool> ||
              detail::vector_selectable<Condition, T>::value)
[[nodiscard]] Expr<T> select(Expr<Condition> condition,
    detail::constant_t<T> when_true, Expr<T> when_false)
{
    return detail::intrinsic<T>(shader::OpCode::select, condition, when_true, when_false);
}

// When both branches are host constants, neither can deduce a logical type
// through its conversion. These overloads cover all primitive DSL value types.
#define VNG_DSL_CONSTANT_SELECT(...)                                                 \
    template<shader::Value Condition>                                             \
        requires (std::same_as<Condition, bool> ||                                \
                  detail::vector_selectable<Condition, __VA_ARGS__>::value)                 \
    [[nodiscard]] Expr<__VA_ARGS__> select(Expr<Condition> condition,                        \
        detail::Constant<__VA_ARGS__> when_true, detail::Constant<__VA_ARGS__> when_false)             \
    { return detail::intrinsic<__VA_ARGS__>(shader::OpCode::select, condition, when_true, when_false); }

VNG_DSL_CONSTANT_SELECT(bool)
VNG_DSL_CONSTANT_SELECT(i32)
VNG_DSL_CONSTANT_SELECT(u32)
VNG_DSL_CONSTANT_SELECT(f32)
VNG_DSL_CONSTANT_SELECT(Vec2)
VNG_DSL_CONSTANT_SELECT(Vec3)
VNG_DSL_CONSTANT_SELECT(Vec4)
VNG_DSL_CONSTANT_SELECT(IVec2)
VNG_DSL_CONSTANT_SELECT(IVec3)
VNG_DSL_CONSTANT_SELECT(IVec4)
VNG_DSL_CONSTANT_SELECT(UVec2)
VNG_DSL_CONSTANT_SELECT(UVec3)
VNG_DSL_CONSTANT_SELECT(UVec4)
VNG_DSL_CONSTANT_SELECT(Mat3)
VNG_DSL_CONSTANT_SELECT(Mat4)
VNG_DSL_CONSTANT_SELECT(Vector<bool, 2>)
VNG_DSL_CONSTANT_SELECT(Vector<bool, 3>)
VNG_DSL_CONSTANT_SELECT(Vector<bool, 4>)
#undef VNG_DSL_CONSTANT_SELECT

template<std::size_t N>
[[nodiscard]] Bool all(Expr<Vector<bool, N>> value)
{
    return detail::intrinsic<bool>(shader::OpCode::all, value);
}

template<std::size_t N>
[[nodiscard]] Bool any(Expr<Vector<bool, N>> value)
{
    return detail::intrinsic<bool>(shader::OpCode::any, value);
}

template<shader::Value Composite>
    requires requires { typename detail::composite_components<Composite>::type; } &&
             (detail::composite_components<Composite>::count == 2)
[[nodiscard]] Expr<Composite> make(Expr<detail::component_t<Composite>> a,
    detail::operand_t<detail::component_t<Composite>> b)
{
    return detail::construct<Composite>(a, b);
}

template<shader::Value Composite>
    requires requires { typename detail::composite_components<Composite>::type; } &&
             (detail::composite_components<Composite>::count == 2)
[[nodiscard]] Expr<Composite> make(detail::constant_t<detail::component_t<Composite>> a,
    Expr<detail::component_t<Composite>> b)
{
    return detail::construct<Composite>(a, b);
}

template<shader::Value Composite>
    requires requires { typename detail::composite_components<Composite>::type; } &&
             (detail::composite_components<Composite>::count == 3)
[[nodiscard]] Expr<Composite> make(Expr<detail::component_t<Composite>> a,
    detail::operand_t<detail::component_t<Composite>> b,
    detail::operand_t<detail::component_t<Composite>> c)
{
    return detail::construct<Composite>(a, b, c);
}

template<shader::Value Composite>
    requires requires { typename detail::composite_components<Composite>::type; } &&
             (detail::composite_components<Composite>::count == 3)
[[nodiscard]] Expr<Composite> make(detail::constant_t<detail::component_t<Composite>> a,
    Expr<detail::component_t<Composite>> b,
    detail::operand_t<detail::component_t<Composite>> c)
{
    return detail::construct<Composite>(a, b, c);
}

template<shader::Value Composite>
    requires requires { typename detail::composite_components<Composite>::type; } &&
             (detail::composite_components<Composite>::count == 3)
[[nodiscard]] Expr<Composite> make(detail::constant_t<detail::component_t<Composite>> a,
    detail::constant_t<detail::component_t<Composite>> b,
    Expr<detail::component_t<Composite>> c)
{
    return detail::construct<Composite>(a, b, c);
}

template<shader::Value Composite>
    requires requires { typename detail::composite_components<Composite>::type; } &&
             (detail::composite_components<Composite>::count == 4)
[[nodiscard]] Expr<Composite> make(Expr<detail::component_t<Composite>> a,
    detail::operand_t<detail::component_t<Composite>> b,
    detail::operand_t<detail::component_t<Composite>> c,
    detail::operand_t<detail::component_t<Composite>> d)
{
    return detail::construct<Composite>(a, b, c, d);
}

template<shader::Value Composite>
    requires requires { typename detail::composite_components<Composite>::type; } &&
             (detail::composite_components<Composite>::count == 4)
[[nodiscard]] Expr<Composite> make(detail::constant_t<detail::component_t<Composite>> a,
    Expr<detail::component_t<Composite>> b,
    detail::operand_t<detail::component_t<Composite>> c,
    detail::operand_t<detail::component_t<Composite>> d)
{
    return detail::construct<Composite>(a, b, c, d);
}

template<shader::Value Composite>
    requires requires { typename detail::composite_components<Composite>::type; } &&
             (detail::composite_components<Composite>::count == 4)
[[nodiscard]] Expr<Composite> make(detail::constant_t<detail::component_t<Composite>> a,
    detail::constant_t<detail::component_t<Composite>> b,
    Expr<detail::component_t<Composite>> c,
    detail::operand_t<detail::component_t<Composite>> d)
{
    return detail::construct<Composite>(a, b, c, d);
}

template<shader::Value Composite>
    requires requires { typename detail::composite_components<Composite>::type; } &&
             (detail::composite_components<Composite>::count == 4)
[[nodiscard]] Expr<Composite> make(detail::constant_t<detail::component_t<Composite>> a,
    detail::constant_t<detail::component_t<Composite>> b,
    detail::constant_t<detail::component_t<Composite>> c,
    Expr<detail::component_t<Composite>> d)
{
    return detail::construct<Composite>(a, b, c, d);
}

[[nodiscard]] inline Expr<Vec2> vec2(Expr<f32> a,
    detail::Operand<f32> b)
{
    return detail::construct<Vec2>(a, b);
}

[[nodiscard]] inline Expr<Vec2> vec2(detail::Constant<f32> a,
    Expr<f32> b)
{
    return detail::construct<Vec2>(a, b);
}

[[nodiscard]] inline Expr<Vec3> vec3(Expr<f32> a,
    detail::Operand<f32> b,
    detail::Operand<f32> c)
{
    return detail::construct<Vec3>(a, b, c);
}

[[nodiscard]] inline Expr<Vec3> vec3(detail::Constant<f32> a,
    Expr<f32> b,
    detail::Operand<f32> c)
{
    return detail::construct<Vec3>(a, b, c);
}

[[nodiscard]] inline Expr<Vec3> vec3(detail::Constant<f32> a,
    detail::Constant<f32> b,
    Expr<f32> c)
{
    return detail::construct<Vec3>(a, b, c);
}

[[nodiscard]] inline Expr<Vec3> vec3(Expr<Vec2> a,
    detail::Operand<f32> b)
{
    return detail::construct<Vec3>(a, b);
}

[[nodiscard]] inline Expr<Vec3> vec3(detail::Constant<Vec2> a,
    Expr<f32> b)
{
    return detail::construct<Vec3>(a, b);
}

[[nodiscard]] inline Expr<Vec4> vec4(Expr<f32> a,
    detail::Operand<f32> b,
    detail::Operand<f32> c,
    detail::Operand<f32> d)
{
    return detail::construct<Vec4>(a, b, c, d);
}

[[nodiscard]] inline Expr<Vec4> vec4(detail::Constant<f32> a,
    Expr<f32> b,
    detail::Operand<f32> c,
    detail::Operand<f32> d)
{
    return detail::construct<Vec4>(a, b, c, d);
}

[[nodiscard]] inline Expr<Vec4> vec4(detail::Constant<f32> a,
    detail::Constant<f32> b,
    Expr<f32> c,
    detail::Operand<f32> d)
{
    return detail::construct<Vec4>(a, b, c, d);
}

[[nodiscard]] inline Expr<Vec4> vec4(detail::Constant<f32> a,
    detail::Constant<f32> b,
    detail::Constant<f32> c,
    Expr<f32> d)
{
    return detail::construct<Vec4>(a, b, c, d);
}

[[nodiscard]] inline Expr<Vec4> vec4(Expr<Vec3> a,
    detail::Operand<f32> b)
{
    return detail::construct<Vec4>(a, b);
}

[[nodiscard]] inline Expr<Vec4> vec4(detail::Constant<Vec3> a,
    Expr<f32> b)
{
    return detail::construct<Vec4>(a, b);
}

[[nodiscard]] inline Expr<Vec4> vec4(Expr<Vec2> a,
    detail::Operand<f32> b,
    detail::Operand<f32> c)
{
    return detail::construct<Vec4>(a, b, c);
}

[[nodiscard]] inline Expr<Vec4> vec4(detail::Constant<Vec2> a,
    Expr<f32> b,
    detail::Operand<f32> c)
{
    return detail::construct<Vec4>(a, b, c);
}

[[nodiscard]] inline Expr<Vec4> vec4(detail::Constant<Vec2> a,
    detail::Constant<f32> b,
    Expr<f32> c)
{
    return detail::construct<Vec4>(a, b, c);
}

[[nodiscard]] inline Expr<Vec4> vec4(Expr<Vec2> a,
    detail::Operand<Vec2> b)
{
    return detail::construct<Vec4>(a, b);
}

[[nodiscard]] inline Expr<Vec4> vec4(detail::Constant<Vec2> a,
    Expr<Vec2> b)
{
    return detail::construct<Vec4>(a, b);
}

} // namespace vng::dsl
