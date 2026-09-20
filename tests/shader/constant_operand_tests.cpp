#include <vng/shader/shader.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <concepts>
#include <type_traits>
#include <variant>

namespace {

using namespace vng;
using namespace vng::dsl;

// A requires-expression checks viable types, not whether a particular value is
// a constant expression. Mutable-value rejection lives in compile_fail tests.
template<class T, class U>
concept Multiplies = requires(T a, U b) { a * b; };

static_assert(Multiplies<Float, f32>);
static_assert(Multiplies<Float3, f32>);
static_assert(Multiplies<Float4x4, Vec4>);
static_assert(Multiplies<Vec4, Float4x4>);
static_assert(!Multiplies<Int, u32>);
static_assert(!Multiplies<Int, f32>);
static_assert(!Multiplies<Float, double>);
static_assert(!Multiplies<Float, void*>);
static_assert(!Multiplies<Float, std::size_t>);

template<class T>
const shader::Operation& producer(const shader::ModuleIR& ir, Expr<T> expression)
{
    return ir.operations[ir.values[expression.id().value].producer.value];
}

TEST_CASE("implicit DSL operands preserve exact compile-time constants",
          "[shader][dsl][constants]")
{
    shader::ModuleIR ir;
    shader::FunctionBuilder builder{ir};
    const auto integer = detail::literal(builder, i32{2});
    constexpr i32 factor = 12;
    const auto scaled = integer * factor;
    REQUIRE(producer(ir, scaled).opcode == shader::OpCode::multiply);
    const auto constant_id = producer(ir, scaled).operands[1];
    const auto& constant = ir.operations[ir.values[constant_id.value].producer.value];
    REQUIRE(constant.opcode == shader::OpCode::constant);
    CHECK(std::get<i32>(std::get<shader::ConstantPayload>(constant.payload).value) == 12);

    const auto reversed = (3 + 4) * integer;
    CHECK(producer(ir, reversed).opcode == shader::OpCode::multiply);
    const auto masked = ((integer + 2) % 3) & 7;
    CHECK(producer(ir, masked).opcode == shader::OpCode::bit_and);
    CHECK(producer(ir, 12 == integer).opcode == shader::OpCode::equal);
    CHECK(producer(ir, integer != 12).opcode == shader::OpCode::not_equal);
    CHECK(producer(ir, integer <= 12).opcode == shader::OpCode::less_equal);

    const auto vector = detail::literal(builder, Vec3{1.0F, 2.0F, 3.0F});
    CHECK(producer(ir, vector * 2.0F).opcode == shader::OpCode::multiply);
    CHECK(producer(ir, 2.0F * vector).opcode == shader::OpCode::multiply);
    CHECK(producer(ir, vector + Vec3{1.0F, 0.0F, 0.0F}).opcode == shader::OpCode::add);
    CHECK(producer(ir, vector.x() * Vec3{1.0F, 2.0F, 3.0F}).opcode == shader::OpCode::multiply);
    const auto matrix = detail::literal(builder, Mat3::identity());
    CHECK(producer(ir, matrix * Vec3{1.0F, 2.0F, 3.0F}).opcode == shader::OpCode::multiply);
    CHECK(producer(ir, Vec3{1.0F, 2.0F, 3.0F} * matrix).opcode == shader::OpCode::multiply);
    CHECK(producer(ir, vector * Mat3::identity()).opcode == shader::OpCode::multiply);
    CHECK(producer(ir, Mat3::identity() * vector).opcode == shader::OpCode::multiply);
    CHECK_FALSE(builder.failed());
}

TEST_CASE("DSL construction and intrinsics infer builders around constant operands",
          "[shader][dsl][constants]")
{
    shader::ModuleIR ir;
    shader::FunctionBuilder builder{ir};
    const auto x = detail::literal(builder, 0.5F);
    const auto v = detail::literal(builder, Vec3{0.1F, 0.2F, 0.3F});
    const auto condition = x < 1.0F;

    const std::array results{
        vec4(x, 0.0F, 0.0F, 1.0F),
        vec4(0.0F, x, 0.0F, 1.0F),
        vec4(0.0F, 0.0F, x, 1.0F),
        vec4(0.0F, 0.0F, 0.0F, x),
        vec4(Vec3{1.0F, 0.0F, 0.0F}, x),
        vec4(Vec2{1.0F, 0.0F}, 0.0F, x),
        vec4(Vec2{1.0F, 0.0F}, vec2(x, 1.0F)),
        make<Vec4>(0.0F, 0.0F, x, 1.0F),
    };
    for (const auto result : results) {
        CHECK(result.builder() == &builder);
        CHECK(producer(ir, result).opcode == shader::OpCode::construct);
    }
    const auto m = make<Mat3>(
        Vec3{1.0F, 0.0F, 0.0F}, v, Vec3{0.0F, 0.0F, 1.0F});
    CHECK(producer(ir, m).opcode == shader::OpCode::construct);

    CHECK(producer(ir, min(x, 1.0F)).opcode == shader::OpCode::minimum);
    CHECK(producer(ir, max(0.0F, x)).opcode == shader::OpCode::maximum);
    CHECK(producer(ir, clamp(x, 0.0F, 1.0F)).opcode == shader::OpCode::clamp);
    CHECK(producer(ir, clamp(0.5F, x, 1.0F)).opcode == shader::OpCode::clamp);
    CHECK(producer(ir, clamp(0.5F, 0.0F, x)).opcode == shader::OpCode::clamp);
    CHECK(producer(ir, clamp(v, 0.0F, 1.0F)).opcode == shader::OpCode::clamp);
    CHECK(producer(ir, clamp(Vec3{}, x, 1.0F)).opcode == shader::OpCode::clamp);
    CHECK(producer(ir, mix(v, Vec3{1.0F, 1.0F, 1.0F}, 0.5F)).opcode == shader::OpCode::mix);
    CHECK(producer(ir, mix(Vec3{}, Vec3{1.0F, 1.0F, 1.0F}, x)).opcode == shader::OpCode::mix);
    CHECK(producer(ir, mix(0.0F, 1.0F, x)).opcode == shader::OpCode::mix);
    CHECK(producer(ir, select(condition, 1.0F, 0.0F)).opcode == shader::OpCode::select);
    CHECK(producer(ir, select(condition, x, 0.0F)).opcode == shader::OpCode::select);
    CHECK(producer(ir, select(condition, 1.0F, x)).opcode == shader::OpCode::select);
    CHECK(producer(ir, select(v < Vec3{}, Vec3{}, Vec3{1.0F, 1.0F, 1.0F})).opcode == shader::OpCode::select);
    CHECK(producer(ir, dot(v, Vec3{1.0F, 0.0F, 0.0F})).opcode == shader::OpCode::dot);
    CHECK_FALSE(builder.failed());
}

TEST_CASE("wrapping intrinsic operands does not conceal mixed builders",
          "[shader][dsl][constants][diagnostic]")
{
    shader::ModuleIR first_ir;
    shader::ModuleIR second_ir;
    shader::FunctionBuilder first{first_ir};
    shader::FunctionBuilder second{second_ir};
    const auto x = detail::literal(first, 0.5F);
    const auto y = detail::literal(second, 0.75F);
    const auto result = clamp(x, 0.0F, y);
    REQUIRE(second.failed());
    CHECK(second.diagnostic()->code == shader::DiagnosticCode::mixed_builders);
    CHECK(result.builder() == &second);
    CHECK(producer(second_ir, result).opcode == shader::OpCode::poison);
}

TEST_CASE("stage.constant explicitly bakes an ordinary CPU value",
          "[shader][dsl][constants]")
{
    using Inputs = shader::FragmentInputs<>;
    using Outputs = shader::FragmentOutputs<shader::Color<0>>;
    f32 mutable_value = 0.25F;
    auto fragment = shader::fragment<Inputs, Outputs>([&](auto& stage) {
        const auto baked = stage.constant(mutable_value);
        return stage.output(field<shader::Color<0>>(vec4(baked, baked, baked, 1.0F)));
    });
    REQUIRE(fragment);
    const auto before = fragment->dump_ir();
    mutable_value = 0.75F;
    CHECK(fragment->dump_ir() == before);
}

} // namespace
