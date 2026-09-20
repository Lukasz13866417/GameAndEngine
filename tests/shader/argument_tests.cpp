#include <vng/shader/shader.hpp>
#include <vng/shader/arguments.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <tuple>
#include <type_traits>

namespace {
using namespace vng;
using dsl::field;
using VI = shader::VertexInputs<>;
using VO = shader::VertexOutputs<shader::ClipPosition>;
using FI = shader::FragmentInputs<>;
using FO = shader::FragmentOutputs<shader::Color<0>>;
struct Gain : gfx::Semantic<f32> {};
struct Tint : gfx::Semantic<Vec4> {};
using Settings = gfx::Record<Gain, gfx::as<Tint, gfx::unorm8x4>>;

template<class T>
concept TemporaryArgumentViews = requires(T&& pack) { std::move(pack).views(); };
static_assert(!TemporaryArgumentViews<shader::ArgumentPack<f32>>);

auto named_vertex(shader::StageContext<shader::StageKind::vertex, VI, VO>& s,
                  dsl::Float x)
{
    return s.output(field<shader::ClipPosition>(dsl::vec4(x, 0.0F, 0.0F, 1.0F)));
}

auto vertex_with_float()
{
    return shader::vertex<VI, VO>([](auto& s, dsl::Float x) {
        return s.output(field<shader::ClipPosition>(dsl::vec4(x, 0.0F, 0.0F, 1.0F)));
    });
}
auto fragment_with_float()
{
    return shader::fragment<FI, FO>([](auto& s, dsl::Float x) {
        return s.output(field<shader::Color<0>>(dsl::vec4(x, 0.0F, 0.0F, 1.0F)));
    });
}
} // namespace

TEST_CASE("Named shader functions preserve their argument signature", "[shader][arguments]")
{
    auto named = shader::vertex<VI, VO>(named_vertex);
    auto pointer = shader::vertex<VI, VO>(&named_vertex);
    REQUIRE(named); REQUIRE(pointer);
    STATIC_REQUIRE(std::same_as<decltype(named)::value_type::signature, shader::Arguments<f32>>);
    CHECK(named->dump_ir() == pointer->dump_ir());
}

TEST_CASE("Shader lambda arguments record symbolic reads and preserve a typed signature", "[shader][arguments]")
{
    int executions = 0;
    auto build = [&] {
        return shader::vertex<VI, VO>([&](auto& s, dsl::Float x, dsl::Float4x4 model) {
            ++executions;
            return s.output(field<shader::ClipPosition>(model * dsl::vec4(x * 2.0F, 0.0F, 0.0F, 1.0F)));
        });
    };
    auto stage = build();
    REQUIRE(stage);
    CHECK(executions == 1);
    using Stage = typename decltype(stage)::value_type;
    STATIC_REQUIRE(std::same_as<Stage::signature, shader::Arguments<f32, Mat4>>);
    STATIC_REQUIRE_FALSE(std::convertible_to<Stage, shader::ShaderStage>);
    const auto& parameters = stage->ir().parameters;
    REQUIRE(parameters.size() == 2);
    CHECK(parameters[0].kind == shader::ParameterKind::argument);
    CHECK(parameters[0].argument_index == 0);
    CHECK(parameters[0].argument_type == typeid(f32));
    CHECK(parameters[1].argument_type == typeid(Mat4));
    CHECK(stage->ir().types[parameters[1].type].kind == shader::TypeKind::matrix);
    CHECK_FALSE(parameters[0].location);
    auto second = build();
    REQUIRE(second);
    CHECK(executions == 2);
    CHECK(stage->dump_ir() == second->dump_ir());
    CHECK(shader::validate(stage->ir()));
}

TEST_CASE("Stage argument lists are distinct unless explicitly shared", "[shader][arguments]")
{
    auto v = vertex_with_float();
    auto f = fragment_with_float();
    REQUIRE(v); REQUIRE(f);
    auto separate = shader::link(*v, *f);
    REQUIRE(separate);
    STATIC_REQUIRE(std::same_as<decltype(separate)::value_type::signature, shader::Arguments<f32, f32>>);
    CHECK(separate->vertex().ir().parameters[0].argument_index == 0);
    CHECK(separate->fragment().ir().parameters[0].argument_index == 1);
    CHECK(separate->vertex().ir().parameters[0].location != separate->fragment().ir().parameters[0].location);
    auto shared = shader::link(*v, *f, shader::shared_arguments);
    REQUIRE(shared);
    STATIC_REQUIRE(std::same_as<decltype(shared)::value_type::signature, shader::Arguments<f32>>);
    CHECK(shared->vertex().ir().parameters[0].argument_index == 0);
    CHECK(shared->fragment().ir().parameters[0].argument_index == 0);
    CHECK(shared->vertex().ir().parameters[0].location == shared->fragment().ir().parameters[0].location);
    CHECK(v->ir().parameters[0].location == std::nullopt);
    auto relinked = shader::link(separate->vertex(), separate->fragment());
    REQUIRE(relinked);
    CHECK(relinked->dump_interface() == separate->dump_interface());
    // Explicitly erasing one parameter-bearing stage must not fabricate a
    // typed linked program whose signature omits those erased arguments.
    CHECK_FALSE(shader::link(*v, f->untyped()));
    CHECK_FALSE(shader::link(v->untyped(), *f));
}

TEST_CASE("Arguments coexist with camera declarations and unused argument contracts", "[shader][arguments]")
{
    auto v = shader::vertex<VI, VO>([](auto& s, dsl::Float3 position, dsl::Float /*unused*/) {
        return s.output(field<shader::ClipPosition>(s.camera().project(position)));
    });
    auto f = shader::fragment<FI, FO>([](auto& s) {
        return s.output(field<shader::Color<0>>(s.constant(Vec4{1, 1, 1, 1})));
    });
    REQUIRE(v); REQUIRE(f);
    auto program = shader::link(*v, *f);
    REQUIRE(program);
    STATIC_REQUIRE(std::same_as<decltype(program)::value_type::signature, shader::Arguments<Vec3, f32>>);
    const auto& parameters = program->vertex().ir().parameters;
    REQUIRE(parameters.size() == 3);
    CHECK(parameters[0].argument_index == 0);
    CHECK(parameters[1].argument_index == 1);
    CHECK(parameters[2].kind == shader::ParameterKind::camera_view_projection);
    CHECK(parameters[0].location != parameters[2].location);
    STATIC_REQUIRE(shader::arguments_match_v<shader::Arguments<Vec3, f32>, const Vec3&, f32>);
    STATIC_REQUIRE_FALSE(shader::arguments_match_v<shader::Arguments<Vec3, f32>, Vec3, double>);
    STATIC_REQUIRE_FALSE(shader::arguments_match_v<shader::Arguments<Vec3, f32>, Vec3>);
}

TEST_CASE("Record parameters are logical fields and consume distinct leaf locations", "[shader][arguments]")
{
    auto v = vertex_with_float();
    auto f = shader::fragment<FI, FO>([](auto& s, dsl::Expr<Settings> settings, dsl::Float offset) {
        return s.output(field<shader::Color<0>>(settings.get(Tint{}) * (settings.get(Gain{}) + offset)));
    });
    REQUIRE(v); REQUIRE(f);
    auto program = shader::link(*v, *f);
    REQUIRE(program);
    STATIC_REQUIRE(std::same_as<decltype(program)::value_type::signature, shader::Arguments<f32, Settings, f32>>);
    const auto& parameters = program->fragment().ir().parameters;
    CHECK(parameters[0].argument_index == 1);
    CHECK(parameters[1].argument_index == 2);
    REQUIRE(parameters[0].location); REQUIRE(parameters[1].location);
    CHECK(*parameters[1].location == *parameters[0].location + 2);
    auto invalid = f->ir();
    invalid.parameters[0].argument_type = typeid(void);
    CHECK_FALSE(shader::validate(invalid));
    invalid = f->ir();
    invalid.parameters[1].argument_index = invalid.parameters[0].argument_index;
    CHECK_FALSE(shader::validate(invalid));
}

TEST_CASE("Argument packs snapshot logical values without copying vertex encodings", "[shader][arguments]")
{
    Settings settings;
    settings.set(Gain{}, 1.25F);
    settings.set(Tint{}, {0.5F, 1.0F, 0.0F, 1.0F});
    const auto decoded = settings.get(Tint{});
    Mat3 matrix = Mat3::identity();
    matrix[2][0] = 7.0F;
    shader::ArgumentPack pack{settings, matrix, i32{-9}, true};
    settings.set(Gain{}, 9.0F);
    matrix[2][0] = 99.0F;
    const auto views = pack.views();
    REQUIRE(views.size() == 4);
    CHECK(views[0].type == typeid(Settings));
    REQUIRE(views[0].words.size() == 5);
    CHECK(std::bit_cast<float>(views[0].words[0]) == 1.25F);
    CHECK(std::bit_cast<float>(views[0].words[1]) == decoded.x);
    CHECK(std::bit_cast<float>(views[0].words[2]) == 1.0F);
    REQUIRE(views[1].words.size() == 9);
    CHECK(std::bit_cast<float>(views[1].words[6]) == 7.0F);
    CHECK(std::bit_cast<i32>(views[2].words[0]) == -9);
    CHECK(views[3].words[0] == 1);
}
