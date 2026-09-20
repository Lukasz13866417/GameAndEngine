#include <vng/glsl/glsl.hpp>
#include <vng/shader/shader.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace {
using namespace vng;
struct UV : gfx::Semantic<Vec2> {};
struct Height : gfx::Semantic<f32> {};
using VI = shader::VertexInputs<UV>;
using VO = shader::VertexOutputs<shader::ClipPosition, shader::smooth<UV>, shader::smooth<Height>>;
using FI = shader::FragmentInputs<shader::smooth<UV>, shader::smooth<Height>>;
using FO = shader::FragmentOutputs<shader::Color<0>>;
}

TEST_CASE("GLSL preserves explicit vertex LOD and procedural math in diagnostic variants", "[glsl][texture][math]")
{
    auto vertex = shader::vertex<VI, VO>([](auto& s, dsl::Float time) {
        auto uv = s.input(UV{});
        auto height = s.template sample_2d_lod<2>(uv, 0.0F).x();
        auto displacement = dsl::pow(dsl::abs(dsl::sin(time) + dsl::cos(time)), 2.0F) * height;
        s.observe(Height{}, displacement);
        return s.output(dsl::field<shader::ClipPosition>(dsl::vec4(uv, displacement, 1.0F)),
                        dsl::field(UV{}, uv), dsl::field(Height{}, displacement));
    });
    auto fragment = shader::fragment<FI, FO>([](auto& s) {
        auto uv = s.input(UV{});
        // A per-pixel observation of vertex displacement needs an explicit
        // interpolation contract, supplied by the smooth Height varying.
        s.observe(Height{}, s.input(Height{}));
        auto value = s.template sample_2d_lod<2>(uv, 0.0F)
                   + s.template sample_2d<3>(uv);
        (void)s.template sample_2d_lod<9>(uv, 0.0F);
        return s.output(dsl::field<shader::Color<0>>(
            dsl::exp(dsl::fract(dsl::floor(value)))));
    });
    REQUIRE(vertex);
    REQUIRE(fragment);
    auto program = shader::link(*vertex, *fragment);
    REQUIRE(program);
    auto normal = glsl::emit(*program);
    auto enhanced = glsl::emit(*program, glsl::AnalysisEmission{});
    auto observed = glsl::emit(*program, glsl::observation(Height{}));
    REQUIRE(normal);
    REQUIRE(enhanced);
    INFO((observed ? "" : observed.error().message));
    REQUIRE(observed);
    auto vertex_observation = glsl::emit(*program,
        glsl::observation(Height{}, shader::StageKind::vertex));
    REQUIRE_FALSE(vertex_observation);
    CHECK(vertex_observation.error().code == shader::DiagnosticCode::unsupported_operation);
    CHECK(vertex_observation.error().message.find("interpolation contract") != std::string::npos);
    CHECK(normal->vertex.texture_bindings == std::vector<u32>{2});
    CHECK(normal->fragment.texture_bindings == std::vector<u32>{2, 3});
    CHECK(normal->texture_bindings == std::vector<u32>{2, 3});
    for (const auto* source : {&*normal, &*enhanced, &*observed}) {
        CHECK(source->texture_bindings == normal->texture_bindings);
        CHECK(source->vertex.source.find("layout(binding = 2) uniform sampler2D vng_texture_2;") != std::string::npos);
        CHECK(source->vertex.source.find("textureLod(vng_texture_2, ") != std::string::npos);
        CHECK(source->fragment.source.find("textureLod(vng_texture_2, ") != std::string::npos);
        CHECK(source->fragment.source.find("texture(vng_texture_3, ") != std::string::npos);
        CHECK(source->fragment.source.find("vng_texture_9") == std::string::npos);
        for (const auto* name : {"sin(", "cos(", "abs(", "pow("})
            CHECK(source->vertex.source.find(name) != std::string::npos);
        for (const auto* name : {"floor(", "fract(", "exp("})
            CHECK(source->fragment.source.find(name) != std::string::npos);
    }
    const auto position = normal->vertex.source.find("textureLod(");
    REQUIRE(position != std::string::npos);
    const auto line = static_cast<u32>(std::count(normal->vertex.source.begin(),
        normal->vertex.source.begin() + static_cast<std::ptrdiff_t>(position), '\n') + 1);
    const auto* mapping = normal->vertex.mapping_for_line(line);
    REQUIRE(mapping);
    CHECK(program->vertex().ir().operations[mapping->operation.value].opcode == shader::OpCode::texture_sample_lod);
    auto repeated = glsl::emit(*program);
    REQUIRE(repeated);
    CHECK(normal->dump() == repeated->dump());
}
