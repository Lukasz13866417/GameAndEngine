#include <vng/glsl/glsl.hpp>
#include <vng/shader/shader.hpp>

#include <catch2/catch_test_macros.hpp>

namespace {
using namespace vng;
struct Position : gfx::Semantic<Vec2> {};
struct Tint : gfx::Semantic<Vec4> {};
struct Gain : gfx::Semantic<f32> {};
using Settings = gfx::Record<Tint, Gain>;
using VI = shader::VertexInputs<Position>;
using VO = shader::VertexOutputs<shader::ClipPosition>;
using FI = shader::FragmentInputs<>;
using FO = shader::FragmentOutputs<shader::Color<0>>;
}

TEST_CASE("typed shader arguments lower to deterministic logical uniform leaves",
          "[glsl][arguments]")
{
    auto vertex = shader::vertex<VI, VO>([](auto& s, dsl::Float4x4 model) {
        return s.output(dsl::field<shader::ClipPosition>(
            model * dsl::vec4(s.input(Position{}), 0.0F, 1.0F)));
    });
    REQUIRE(vertex);
    auto fragment = shader::fragment<FI, FO>(
        [](auto& s, dsl::Expr<Settings> settings, dsl::Float) {
            return s.output(dsl::field<shader::Color<0>>(
                settings.get(Tint{}) * settings.get(Gain{})));
        });
    REQUIRE(fragment);
    auto program = shader::link(std::move(*vertex), std::move(*fragment));
    REQUIRE(program);
    auto source = glsl::emit(*program);
    REQUIRE(source);
    CHECK(source->vertex.source.find("layout(location = 0) uniform mat4 vng_argument_0;")
          != std::string::npos);
    CHECK(source->fragment.source.find("layout(location = 1) uniform vec4 vng_argument_1_m0;")
          != std::string::npos);
    CHECK(source->fragment.source.find("layout(location = 2) uniform float vng_argument_1_m1;")
          != std::string::npos);
    // An unused lambda argument stays in the call contract and source.
    CHECK(source->fragment.source.find("layout(location = 3) uniform float vng_argument_2;")
          != std::string::npos);
    REQUIRE(source->parameters.size() == 3);
    CHECK(source->parameters[0].word_count == 16);
    CHECK(source->parameters[1].word_count == 5);
    CHECK(source->parameters[1].argument_type == typeid(Settings));
    REQUIRE(source->parameters[1].leaves.size() == 2);
    CHECK(source->parameters[1].leaves[0].rows == 4);
    CHECK(source->parameters[1].leaves[1].word_offset == 4);
    auto again = glsl::emit(*program);
    REQUIRE(again);
    CHECK(source->dump() == again->dump());
    auto enhanced = glsl::emit(*program, glsl::AnalysisEmission{});
    REQUIRE(enhanced);
    CHECK(enhanced->parameters == source->parameters);
    CHECK(enhanced->analysis->first_item_uniform_location == 4);
}

TEST_CASE("shared record arguments flatten identically across shader stages",
          "[glsl][arguments]")
{
    auto vertex = shader::vertex<VI, VO>([](auto& s, dsl::Expr<Settings> settings) {
        return s.output(dsl::field<shader::ClipPosition>(dsl::vec4(
            s.input(Position{}) * settings.get(Gain{}), 0.0F, 1.0F)));
    });
    auto fragment = shader::fragment<FI, FO>([](auto& s, dsl::Expr<Settings> settings) {
        return s.output(dsl::field<shader::Color<0>>(settings.get(Tint{})));
    });
    REQUIRE(vertex);
    REQUIRE(fragment);
    auto program = shader::link(std::move(*vertex), std::move(*fragment), shader::shared_arguments);
    REQUIRE(program);
    auto source = glsl::emit(*program);
    REQUIRE(source);
    REQUIRE(source->parameters.size() == 1);
    CHECK(source->parameters[0].argument_index == 0);
    CHECK(source->parameters[0].leaves.size() == 2);
    for (const auto* text : {&source->vertex.source, &source->fragment.source}) {
        CHECK(text->find("layout(location = 0) uniform vec4 vng_argument_0_m0;")
              != std::string::npos);
        CHECK(text->find("layout(location = 1) uniform float vng_argument_0_m1;")
              != std::string::npos);
    }
}
