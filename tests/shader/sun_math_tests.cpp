#include <vng/shader/shader.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <type_traits>

namespace {
using namespace vng;
using VI = shader::VertexInputs<>;
using VO = shader::VertexOutputs<shader::ClipPosition>;
using FI = shader::FragmentInputs<>;
using FO = shader::FragmentOutputs<shader::Color<0>>;
using VertexContext = shader::StageContext<shader::StageKind::vertex, VI, VO>;
using FragmentContext = shader::StageContext<shader::StageKind::fragment, FI, FO>;

template<class T> concept Trigonometric = requires(T x) { dsl::sin(x); dsl::cos(x); };
template<class T> concept Absolute = requires(T x) { dsl::abs(x); };
template<class A, class B> concept Power = requires(A a, B b) { dsl::pow(a, b); };
template<class Context, class UV, class Level>
concept SampleLod = requires(Context& s, UV uv, Level level) {
    { s.template sample_2d_lod<0>(uv, level) } -> std::same_as<dsl::Float4>;
};

static_assert(Trigonometric<dsl::Float> && Trigonometric<dsl::Float4>);
static_assert(!Trigonometric<dsl::Int> && !Trigonometric<dsl::Float4x4>);
static_assert(Absolute<dsl::Int> && Absolute<dsl::Int3> && Absolute<dsl::Float>);
static_assert(!Absolute<dsl::UInt> && !Absolute<dsl::Bool>);
static_assert(Power<dsl::Float, dsl::Float> && Power<dsl::Float3, dsl::Float3>);
static_assert(!Power<dsl::Int, dsl::Int> && !Power<dsl::Float, dsl::Int>);
static_assert(SampleLod<VertexContext, dsl::Float2, dsl::Float>);
static_assert(SampleLod<FragmentContext, dsl::Float2, dsl::Float>);
static_assert(!SampleLod<VertexContext, dsl::Float3, dsl::Float>);
static_assert(!SampleLod<VertexContext, dsl::Float2, dsl::Int>);

template<class T>
auto animated_math(T value)
{
    return dsl::exp(dsl::fract(dsl::floor(dsl::abs(dsl::sin(value) + dsl::cos(value)))));
}
}

TEST_CASE("procedural shader math preserves scalar and vector types", "[shader][math]")
{
    auto vertex = shader::vertex<VI, VO>([](auto& s, dsl::Float x) {
        const auto value = dsl::pow(animated_math(x), 2.0F);
        return s.output(dsl::field<shader::ClipPosition>(dsl::vec4(value, 0.0F, 0.0F, 1.0F)));
    });
    auto fragment = shader::fragment<FI, FO>([](auto& s, dsl::Float4 x) {
        return s.output(dsl::field<shader::Color<0>>(
            dsl::pow(animated_math(x), Vec4{2, 2, 2, 2})));
    });
    REQUIRE(vertex);
    REQUIRE(fragment);
    CHECK(shader::validate(vertex->ir()));
    CHECK(shader::validate(fragment->ir()));
    for (auto opcode : {shader::OpCode::sine, shader::OpCode::cosine,
                        shader::OpCode::absolute, shader::OpCode::floor,
                        shader::OpCode::fract, shader::OpCode::exponential,
                        shader::OpCode::power}) {
        CHECK(std::ranges::count(vertex->ir().operations, opcode, &shader::Operation::opcode) == 1);
        CHECK(std::ranges::count(fragment->ir().operations, opcode, &shader::Operation::opcode) == 1);
    }
    auto invalid = vertex->ir();
    auto power = std::ranges::find(invalid.operations, shader::OpCode::power,
                                    &shader::Operation::opcode);
    REQUIRE(power != invalid.operations.end());
    power->operands.pop_back();
    CHECK_FALSE(shader::validate(invalid));
}

TEST_CASE("explicit LOD samples are shared and valid in vertex and fragment stages", "[shader][texture]")
{
    auto vertex = shader::vertex<VI, VO>([](auto& s, dsl::Float2 uv, dsl::Float level) {
        const auto sampled = s.template sample_2d_lod<2>(uv, level);
        const auto copy = sampled;
        (void)s.template sample_2d_lod<9>(uv, 0.0F);
        return s.output(dsl::field<shader::ClipPosition>(sampled + copy));
    });
    auto fragment = shader::fragment<FI, FO>([](auto& s, dsl::Float2 uv) {
        return s.output(dsl::field<shader::Color<0>>(s.template sample_2d_lod<2>(uv, 0.0F)));
    });
    REQUIRE(vertex);
    REQUIRE(fragment);
    CHECK(shader::validate(vertex->ir()));
    CHECK(shader::validate(fragment->ir()));
    CHECK(std::ranges::count(vertex->ir().operations, shader::OpCode::texture_sample_lod,
                             &shader::Operation::opcode) == 1);
    CHECK(vertex->dump_ir().find("[binding=9]") == std::string::npos);

    auto module = vertex->ir();
    auto sample = std::ranges::find(module.operations, shader::OpCode::texture_sample_lod,
                                    &shader::Operation::opcode);
    REQUIRE(sample != module.operations.end());
    SECTION("arity") { sample->operands.pop_back(); }
    SECTION("level type") { sample->operands[1] = sample->operands[0]; }
    SECTION("coordinates type") { sample->operands[0] = sample->operands[1]; }
    SECTION("payload") { sample->payload = std::monostate{}; }
    SECTION("effect") { sample->effect = shader::Effect::pure; }
    CHECK_FALSE(shader::validate(module));
}

TEST_CASE("explicit LOD sampling diagnoses either foreign operand", "[shader][texture][diagnostic]")
{
    shader::ModuleIR foreign_module;
    shader::FunctionBuilder foreign_builder{foreign_module};
    auto foreign_uv = dsl::detail::literal(foreign_builder, Vec2{0.5F, 0.5F});
    auto foreign_level = dsl::detail::literal(foreign_builder, 0.0F);
    auto vertex = shader::vertex<VI, VO>([&](auto& s, dsl::Float2 uv) {
        auto sampled = s.template sample_2d_lod<0>(uv, foreign_level);
        return s.output(dsl::field<shader::ClipPosition>(sampled));
    });
    REQUIRE_FALSE(vertex);
    CHECK(vertex.error().code == shader::DiagnosticCode::mixed_builders);
    auto fragment = shader::fragment<FI, FO>([&](auto& s) {
        return s.output(dsl::field<shader::Color<0>>(s.template sample_2d_lod<0>(foreign_uv, 0.0F)));
    });
    REQUIRE_FALSE(fragment);
    CHECK(fragment.error().code == shader::DiagnosticCode::mixed_builders);
}
