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

template<class Composite, class... Arguments>
[[nodiscard]] consteval bool directly_constructible()
{
    if constexpr (is_vector_v<Composite>) {
        using Element = typename vector_traits<Composite>::value_type;
        return sizeof...(Arguments) == vector_traits<Composite>::component_count &&
               (std::is_same_v<host_or_expression_value_t<Arguments>, Element> && ...);
    } else if constexpr (shader::detail::is_matrix_v<Composite>) {
        using Column = Vector<f32, shader::detail::matrix_traits<Composite>::rows>;
        return sizeof...(Arguments) == shader::detail::matrix_traits<Composite>::columns &&
               (std::is_same_v<host_or_expression_value_t<Arguments>, Column> && ...);
    }
    return false;
}

template<class Composite, class Low, class High>
[[nodiscard]] consteval bool valid_clamp_bounds()
{
    if constexpr (std::is_same_v<Composite, Low> && std::is_same_v<Composite, High>) {
        return true;
    } else if constexpr (is_vector_v<Composite>) {
        using Element = typename vector_traits<Composite>::value_type;
        return std::is_same_v<Element, Low> && std::is_same_v<Element, High>;
    }
    return false;
}

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

template<class Left, class Right>
concept SameNumericPair = ShaderOperand<Left> && ShaderOperand<Right> &&
    HasExpressionOperand<Left, Right> &&
    std::is_same_v<host_or_expression_value_t<Left>, host_or_expression_value_t<Right>> &&
    NumericValue<host_or_expression_value_t<Left>>;

} // namespace detail

template<class Left, class Right> requires detail::SameNumericPair<Left, Right>
[[nodiscard]] auto min(const Left& left, const Right& right)
{
    using Result = detail::host_or_expression_value_t<Left>;
    return detail::binary<Result>(shader::OpCode::minimum, left, right);
}

template<class Left, class Right> requires detail::SameNumericPair<Left, Right>
[[nodiscard]] auto max(const Left& left, const Right& right)
{
    using Result = detail::host_or_expression_value_t<Left>;
    return detail::binary<Result>(shader::OpCode::maximum, left, right);
}

template<class ValueArg, class Low, class High>
    requires detail::ShaderOperand<ValueArg> && detail::ShaderOperand<Low> && detail::ShaderOperand<High> &&
             detail::HasAnyExpression<ValueArg, Low, High> &&
             (detail::valid_clamp_bounds<
                 detail::host_or_expression_value_t<ValueArg>,
                 detail::host_or_expression_value_t<Low>,
                 detail::host_or_expression_value_t<High>>()) &&
             detail::NumericValue<detail::host_or_expression_value_t<ValueArg>>
[[nodiscard]] auto clamp(const ValueArg& value, const Low& low, const High& high)
{
    using Result = detail::host_or_expression_value_t<ValueArg>;
    return detail::intrinsic<Result>(shader::OpCode::clamp, value, low, high);
}

template<class Left, class Right>
    requires detail::ShaderOperand<Left> && detail::ShaderOperand<Right> &&
             detail::HasExpressionOperand<Left, Right> &&
             std::is_same_v<detail::host_or_expression_value_t<Left>, detail::host_or_expression_value_t<Right>> &&
             detail::float_vector_v<detail::host_or_expression_value_t<Left>>
[[nodiscard]] auto dot(const Left& left, const Right& right)
{
    return detail::binary<f32>(shader::OpCode::dot, left, right);
}

[[nodiscard]] inline Float3 cross(Float3 left, Float3 right)
{
    return detail::binary<Vec3>(shader::OpCode::cross, left, right);
}

template<shader::Value T>
    requires detail::float_vector_v<T>
[[nodiscard]] Expr<T> normalize(Expr<T> value)
{
    return detail::intrinsic<T>(shader::OpCode::normalize, value);
}

template<shader::Value T>
    requires detail::FloatValue<T>
[[nodiscard]] Expr<T> sqrt(Expr<T> value)
{
    return detail::intrinsic<T>(shader::OpCode::square_root, value);
}

template<class A, class B, class Factor>
    requires detail::ShaderOperand<A> && detail::ShaderOperand<B> && detail::ShaderOperand<Factor> &&
             detail::HasAnyExpression<A, B, Factor> &&
             std::is_same_v<detail::host_or_expression_value_t<A>, detail::host_or_expression_value_t<B>> &&
             detail::FloatValue<detail::host_or_expression_value_t<A>> &&
             (std::is_same_v<detail::host_or_expression_value_t<Factor>, f32> ||
              std::is_same_v<detail::host_or_expression_value_t<Factor>, detail::host_or_expression_value_t<A>>)
[[nodiscard]] auto mix(const A& a, const B& b, const Factor& factor)
{
    using Result = detail::host_or_expression_value_t<A>;
    return detail::intrinsic<Result>(shader::OpCode::mix, a, b, factor);
}

template<shader::Value Condition, class True, class False>
    requires detail::ShaderOperand<True> && detail::ShaderOperand<False> &&
             std::is_same_v<detail::host_or_expression_value_t<True>, detail::host_or_expression_value_t<False>> &&
             (std::is_same_v<Condition, bool> ||
              detail::vector_selectable<Condition, detail::host_or_expression_value_t<True>>::value)
[[nodiscard]] auto select(Expr<Condition> condition, const True& when_true, const False& when_false)
{
    using T = detail::host_or_expression_value_t<True>;
    return detail::intrinsic<T>(shader::OpCode::select, condition, when_true, when_false);
}

template<std::size_t N>
[[nodiscard]] Bool all(Expr<Vector<bool, N>> value)
{
    return detail::intrinsic<bool>(shader::OpCode::all, value);
}

template<shader::Value Composite, class... Arguments>
    requires detail::HasAnyExpression<Arguments...> &&
             (detail::directly_constructible<Composite, Arguments...>())
[[nodiscard]] Expr<Composite> make(const Arguments&... arguments)
{
    return detail::construct<Composite>(arguments...);
}

template<std::size_t N>
[[nodiscard]] Bool any(Expr<Vector<bool, N>> value)
{
    return detail::intrinsic<bool>(shader::OpCode::any, value);
}

template<class X, class Y>
    requires detail::HasAnyExpression<X, Y> &&
             std::is_same_v<detail::host_or_expression_value_t<X>, f32> &&
             std::is_same_v<detail::host_or_expression_value_t<Y>, f32>
[[nodiscard]] Float2 vec2(const X& x, const Y& y)
{
    return detail::construct<Vec2>(x, y);
}

template<class X, class Y, class Z>
    requires detail::HasAnyExpression<X, Y, Z> &&
             std::is_same_v<detail::host_or_expression_value_t<X>, f32> &&
             std::is_same_v<detail::host_or_expression_value_t<Y>, f32> &&
             std::is_same_v<detail::host_or_expression_value_t<Z>, f32>
[[nodiscard]] Float3 vec3(const X& x, const Y& y, const Z& z)
{
    return detail::construct<Vec3>(x, y, z);
}

template<class XY, class Z>
    requires detail::HasAnyExpression<XY, Z> &&
             std::is_same_v<detail::host_or_expression_value_t<XY>, Vec2> &&
             std::is_same_v<detail::host_or_expression_value_t<Z>, f32>
[[nodiscard]] Float3 vec3(const XY& xy, const Z& z)
{
    return detail::construct<Vec3>(xy, z);
}

template<class X, class Y, class Z, class W>
    requires detail::HasAnyExpression<X, Y, Z, W> &&
             std::is_same_v<detail::host_or_expression_value_t<X>, f32> &&
             std::is_same_v<detail::host_or_expression_value_t<Y>, f32> &&
             std::is_same_v<detail::host_or_expression_value_t<Z>, f32> &&
             std::is_same_v<detail::host_or_expression_value_t<W>, f32>
[[nodiscard]] Float4 vec4(const X& x, const Y& y, const Z& z, const W& w)
{
    return detail::construct<Vec4>(x, y, z, w);
}

template<class XYZ, class W>
    requires detail::HasAnyExpression<XYZ, W> &&
             std::is_same_v<detail::host_or_expression_value_t<XYZ>, Vec3> &&
             std::is_same_v<detail::host_or_expression_value_t<W>, f32>
[[nodiscard]] Float4 vec4(const XYZ& xyz, const W& w)
{
    return detail::construct<Vec4>(xyz, w);
}

template<class XY, class Z, class W>
    requires detail::HasAnyExpression<XY, Z, W> &&
             std::is_same_v<detail::host_or_expression_value_t<XY>, Vec2> &&
             std::is_same_v<detail::host_or_expression_value_t<Z>, f32> &&
             std::is_same_v<detail::host_or_expression_value_t<W>, f32>
[[nodiscard]] Float4 vec4(const XY& xy, const Z& z, const W& w)
{
    return detail::construct<Vec4>(xy, z, w);
}

template<class XY, class ZW>
    requires detail::HasAnyExpression<XY, ZW> &&
             std::is_same_v<detail::host_or_expression_value_t<XY>, Vec2> &&
             std::is_same_v<detail::host_or_expression_value_t<ZW>, Vec2>
[[nodiscard]] Float4 vec4(const XY& xy, const ZW& zw)
{
    return detail::construct<Vec4>(xy, zw);
}

} // namespace vng::dsl
