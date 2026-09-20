#include <vng/glsl/glsl.hpp>
#include <vng/shader/shader.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct Position : vng::gfx::Semantic<vng::Vec2> {};
struct WorldPosition : vng::gfx::Semantic<vng::Vec3> {};
struct VertexColor : vng::gfx::Semantic<vng::Vec4> {};
struct Offset : vng::gfx::Semantic<vng::Vec2> {};
struct DiagnosticOffset : vng::gfx::Semantic<vng::Vec2> {};
struct DiagnosticColor : vng::gfx::Semantic<vng::Vec3> {};
struct DiagnosticCounter : vng::gfx::Semantic<vng::u32> {};
struct DiagnosticMask : vng::gfx::Semantic<bool> {};
struct MissingDiagnostic : vng::gfx::Semantic<vng::Vec3> {};

using SimpleVertexInputs = vng::shader::VertexInputs<Position, VertexColor, Offset>;
using SimpleVertexOutputs = vng::shader::VertexOutputs<
    vng::shader::ClipPosition,
    vng::shader::smooth<VertexColor>>;
using SimpleFragmentInputs = vng::shader::FragmentInputs<
    vng::shader::smooth<VertexColor>>;
using SimpleFragmentOutputs = vng::shader::FragmentOutputs<
    vng::shader::Color<0>>;

[[nodiscard]] vng::shader::Result<vng::shader::GraphicsProgram> make_simple_program()
{
    auto vertex = vng::shader::vertex<SimpleVertexInputs, SimpleVertexOutputs>(
        "test_vertex",
        [](auto& stage) {
            const auto position = stage.input(Position{});
            const auto color = stage.input(VertexColor{});
            const auto offset = stage.input(Offset{});
            const auto moved = position + offset;
            const auto clip = vng::dsl::vec4(moved, 0.0F, 1.0F);
            return stage.output(
                vng::dsl::field<vng::shader::ClipPosition>(clip),
                vng::dsl::field<VertexColor>(color));
        });
    if (!vertex) {
        return std::unexpected(std::move(vertex.error()));
    }

    auto fragment = vng::shader::fragment<SimpleFragmentInputs, SimpleFragmentOutputs>(
        "test_fragment",
        [](auto& stage) {
            const auto color = stage.input(VertexColor{});
            return stage.output(
                vng::dsl::field<vng::shader::Color<0>>(color));
        });
    if (!fragment) {
        return std::unexpected(std::move(fragment.error()));
    }
    return vng::shader::link(std::move(*vertex), std::move(*fragment));
}

[[nodiscard]] vng::shader::Result<vng::shader::GraphicsProgram>
make_observed_simple_program()
{
    auto vertex = vng::shader::vertex<SimpleVertexInputs, SimpleVertexOutputs>(
        "test_vertex",
        [](auto& stage) {
            const auto position = stage.input(Position{});
            const auto color = stage.input(VertexColor{});
            const auto offset = stage.input(Offset{});
            const auto moved = position + offset;
            const auto clip = vng::dsl::vec4(moved, 0.0F, 1.0F);
            const auto result = stage.output(
                vng::dsl::field<vng::shader::ClipPosition>(clip),
                vng::dsl::field<VertexColor>(color));

            // Deliberately inserted before the stage-result extraction done
            // by the factory. Normal emission must omit this entire slice and
            // compact its generated temporary names.
            const auto diagnostic = moved + vng::Vec2{0.125F, 0.25F};
            stage.observe(DiagnosticOffset{}, diagnostic);
            return result;
        });
    if (!vertex) {
        return std::unexpected(std::move(vertex.error()));
    }

    auto fragment = vng::shader::fragment<
        SimpleFragmentInputs,
        SimpleFragmentOutputs>(
        "test_fragment",
        [](auto& stage) {
            return stage.output(
                vng::dsl::field<vng::shader::Color<0>>(
                    stage.input(VertexColor{})));
        });
    if (!fragment) {
        return std::unexpected(std::move(fragment.error()));
    }
    return vng::shader::link(std::move(*vertex), std::move(*fragment));
}

[[nodiscard]] vng::shader::Result<vng::shader::GraphicsProgram>
make_fragment_observed_program()
{
    auto vertex = vng::shader::vertex<SimpleVertexInputs, SimpleVertexOutputs>(
        "test_vertex",
        [](auto& stage) {
            const auto position = stage.input(Position{});
            const auto color = stage.input(VertexColor{});
            const auto offset = stage.input(Offset{});
            return stage.output(
                vng::dsl::field<vng::shader::ClipPosition>(
                    vng::dsl::vec4(position + offset, 0.0F, 1.0F)),
                vng::dsl::field<VertexColor>(color));
        });
    if (!vertex) {
        return std::unexpected(std::move(vertex.error()));
    }

    auto fragment = vng::shader::fragment<
        SimpleFragmentInputs,
        SimpleFragmentOutputs>(
        "test_fragment",
        [](auto& stage) {
            const auto color = stage.input(VertexColor{});
            const auto diagnostic_color = color.xyz()
                + vng::Vec3{0.125F, 0.25F, 0.375F};
            stage.observe(DiagnosticColor{}, diagnostic_color);

            // These observations prove that diagnostic emission makes only
            // the requested slice executable.
            stage.observe(
                DiagnosticCounter{}, stage.constant(vng::u32{37}));
            stage.observe(DiagnosticMask{}, stage.front_facing());

            return stage.output(
                vng::dsl::field<vng::shader::Color<0>>(color));
        });
    if (!fragment) {
        return std::unexpected(std::move(fragment.error()));
    }
    return vng::shader::link(std::move(*vertex), std::move(*fragment));
}

using CameraVertexInputs = vng::shader::VertexInputs<WorldPosition>;
using CameraVertexOutputs = vng::shader::VertexOutputs<
    vng::shader::ClipPosition>;
using CameraFragmentInputs = vng::shader::FragmentInputs<>;
using CameraFragmentOutputs = vng::shader::FragmentOutputs<
    vng::shader::Color<0>>;

[[nodiscard]] vng::shader::Result<vng::shader::GraphicsProgram>
make_camera_program()
{
    auto vertex = vng::shader::vertex<CameraVertexInputs, CameraVertexOutputs>(
        "camera_vertex",
        [](auto& stage) {
            return stage.output(
                vng::dsl::field<vng::shader::ClipPosition>(
                    stage.camera().project(stage.input(WorldPosition{}))));
        });
    if (!vertex) {
        return std::unexpected(std::move(vertex.error()));
    }

    auto fragment = vng::shader::fragment<
        CameraFragmentInputs,
        CameraFragmentOutputs>(
        "camera_fragment",
        [](auto& stage) {
            return stage.output(
                vng::dsl::field<vng::shader::Color<0>>(
                    stage.constant(vng::Vec4{1.0F, 1.0F, 1.0F, 1.0F})));
        });
    if (!fragment) {
        return std::unexpected(std::move(fragment.error()));
    }
    return vng::shader::link(std::move(*vertex), std::move(*fragment));
}

constexpr std::string_view expected_vertex = R"(#version 460 core

struct vng_record_4
{
    vec4 m0;
    vec4 m1;
};

layout(location = 0) in vec2 vng_in_0;
layout(location = 1) in vec4 vng_in_1;
layout(location = 2) in vec2 vng_in_2;
layout(location = 0) smooth out vec4 vng_out_1;

void main()
{
    vec2 v0 = vng_in_0;
    vec4 v1 = vng_in_1;
    vec2 v2 = vng_in_2;
    vec2 v3 = (v0 + v2);
    float v4 = 0.0;
    float v5 = 1.0;
    vec4 v6 = vec4(v3, v4, v5);
    vng_record_4 v7 = vng_record_4(v6, v1);
    vec4 v8 = v7.m0;
    gl_Position = v8;
    vec4 v9 = v7.m1;
    vng_out_1 = v9;
    return;
}
)";

constexpr std::string_view expected_fragment = R"(#version 460 core

struct vng_record_3
{
    vec4 m0;
};

layout(location = 0) smooth in vec4 vng_in_0;
layout(location = 0) out vec4 vng_out_0;

void main()
{
    vec4 v0 = vng_in_0;
    vng_record_3 v1 = vng_record_3(v0);
    vec4 v2 = v1.m0;
    vng_out_0 = v2;
    return;
}
)";

constexpr std::string_view expected_camera_vertex = R"(#version 460 core

struct vng_record_5
{
    vec4 m0;
};

layout(location = 0) in vec3 vng_in_0;
layout(location = 0) uniform mat4 vng_camera_view_projection;

void main()
{
    vec3 v0 = vng_in_0;
    mat4 v1 = vng_camera_view_projection;
    float v2 = 1.0;
    vec4 v3 = vec4(v0, v2);
    vec4 v4 = (v1 * v3);
    vng_record_5 v5 = vng_record_5(v4);
    vec4 v6 = v5.m0;
    gl_Position = v6;
    return;
}
)";

constexpr std::string_view expected_analysis_vertex = R"(#version 460 core

struct vng_record_4
{
    vec4 m0;
    vec4 m1;
};

layout(location = 0) in vec2 vng_in_0;
layout(location = 1) in vec4 vng_in_1;
layout(location = 2) in vec2 vng_in_2;
layout(location = 0) smooth out vec4 vng_out_1;
layout(location = 0) uniform uint vng_analysis_first_item_id;
layout(location = 1) flat out uint vng_analysis_item_id;

void main()
{
    vec2 v0 = vng_in_0;
    vec4 v1 = vng_in_1;
    vec2 v2 = vng_in_2;
    vec2 v3 = (v0 + v2);
    float v4 = 0.0;
    float v5 = 1.0;
    vec4 v6 = vec4(v3, v4, v5);
    vng_record_4 v7 = vng_record_4(v6, v1);
    vec4 v8 = v7.m0;
    gl_Position = v8;
    vec4 v9 = v7.m1;
    vng_out_1 = v9;
    vng_analysis_item_id = vng_analysis_first_item_id + uint(gl_InstanceID);
    return;
}
)";

constexpr std::string_view expected_analysis_fragment = R"(#version 460 core

struct vng_record_3
{
    vec4 m0;
};

layout(location = 0) smooth in vec4 vng_in_0;
layout(location = 0) out vec4 vng_out_0;
layout(location = 1) flat in uint vng_analysis_item_id;
layout(location = 1) out uvec2 vng_analysis_surface_key;

void main()
{
    vec4 v0 = vng_in_0;
    vng_record_3 v1 = vng_record_3(v0);
    vec4 v2 = v1.m0;
    vng_out_0 = v2;
    vng_analysis_surface_key = uvec2(vng_analysis_item_id, uint(gl_PrimitiveID));
    return;
}
)";

struct A : vng::gfx::Semantic<vng::Vec3> {};
struct B : vng::gfx::Semantic<vng::Vec3> {};
struct Counter : vng::gfx::Semantic<vng::u32> {};

using OperationsVertexInputs = vng::shader::VertexInputs<A, B, Counter>;
using OperationsVertexOutputs = vng::shader::VertexOutputs<
    vng::shader::ClipPosition,
    vng::shader::smooth<A>,
    vng::shader::smooth<B>,
    vng::shader::flat<Counter>>;
using OperationsFragmentInputs = vng::shader::FragmentInputs<
    vng::shader::smooth<A>,
    vng::shader::smooth<B>,
    vng::shader::flat<Counter>>;
using OperationsFragmentOutputs = vng::shader::FragmentOutputs<
    vng::shader::Color<0>>;

using BuiltinVertexInputs = vng::shader::VertexInputs<>;
using BuiltinVertexOutputs = vng::shader::VertexOutputs<
    vng::shader::ClipPosition>;
using BuiltinFragmentInputs = vng::shader::FragmentInputs<>;
using BuiltinFragmentOutputs = vng::shader::FragmentOutputs<
    vng::shader::Color<0>,
    vng::shader::FragmentDepth>;

[[nodiscard]] vng::shader::Result<vng::shader::GraphicsProgram> make_operations_program()
{
    auto vertex = vng::shader::vertex<OperationsVertexInputs, OperationsVertexOutputs>(
        [](auto& stage) {
            const auto a = stage.in().get(A{});
            const auto b = stage.in().get(B{});
            const auto counter = stage.in().get(Counter{});
            const auto clip = vng::dsl::vec4(a, 1.0F);
            return vng::dsl::make<OperationsVertexOutputs>(
                vng::dsl::field(vng::shader::ClipPosition{}, clip),
                vng::dsl::field(A{}, a),
                vng::dsl::field(B{}, b),
                vng::dsl::field(Counter{}, counter));
        });
    if (!vertex) return std::unexpected(std::move(vertex.error()));

    auto fragment = vng::shader::fragment<OperationsFragmentInputs, OperationsFragmentOutputs>(
        [](auto& stage) {
            const auto a = stage.in().get(A{});
            const auto b = stage.in().get(B{});
            const auto counter = stage.in().get(Counter{});

            const auto remainder = counter % 7U;
            const auto inverted = ~remainder;
            const auto masked = inverted & 15U;
            const auto combined_bits = (masked | 2U) ^ 1U;
            const auto shifted = (combined_bits << 1U) >> 1U;
            const auto bit_value = vng::dsl::cast<vng::f32>(shifted);

            const auto vector_choice = vng::dsl::select(a < b, a, b);
            const auto equal_choice = vng::dsl::select(vng::dsl::all(a == b), vector_choice, a);
            const auto unequal_choice = vng::dsl::select(vng::dsl::any(a != b), equal_choice, b);
            const auto less_equal_choice = vng::dsl::select(vng::dsl::any(a <= b), unequal_choice, a);
            const auto greater_choice = vng::dsl::select(vng::dsl::any(a > b), less_equal_choice, b);
            const auto compared = vng::dsl::select(vng::dsl::any(a >= b), greater_choice, a);

            const auto normal = vng::dsl::normalize(compared + vng::dsl::cross(a, b));
            const auto product = vng::dsl::dot(normal, b) * 0.5F;
            const auto quotient = (product - (-bit_value)) / 10.0F;
            const auto bounded = vng::dsl::clamp(
                vng::dsl::max(vng::dsl::min(quotient, 1.0F), 0.0F),
                0.0F,
                1.0F);
            const auto factor = vng::dsl::sqrt(bounded);
            const auto selected = vng::dsl::select(!(factor == 0.0F), normal, b);
            const auto result = vng::dsl::mix(selected, vector_choice, factor);
            const auto color = vng::dsl::vec4(result.xyz(), 1.0F);
            return vng::dsl::make<OperationsFragmentOutputs>(
                vng::dsl::field(vng::shader::Color<0>{}, color));
        });
    if (!fragment) return std::unexpected(std::move(fragment.error()));
    return vng::shader::link(std::move(*vertex), std::move(*fragment));
}

[[nodiscard]] vng::shader::Result<vng::shader::GraphicsProgram> make_builtin_program()
{
    auto vertex = vng::shader::vertex<BuiltinVertexInputs, BuiltinVertexOutputs>(
        [](auto& stage) {
            const auto vertex_index = stage.vertex_index();
            const auto instance_index = stage.instance_index();
            const auto coordinate = vng::dsl::cast<vng::f32>(vertex_index + instance_index);
            const auto clip = vng::dsl::vec4(coordinate, 0.0F, 0.0F, 1.0F);
            return vng::dsl::make<BuiltinVertexOutputs>(
                vng::dsl::field(vng::shader::ClipPosition{}, clip));
        });
    if (!vertex) return std::unexpected(std::move(vertex.error()));

    auto fragment = vng::shader::fragment<BuiltinFragmentInputs, BuiltinFragmentOutputs>(
        [](auto& stage) {
            const auto coordinate = stage.fragment_coordinate();
            const auto color = vng::dsl::select(
                stage.front_facing(),
                coordinate,
                stage.constant(vng::Vec4{1.0F, 0.0F, 1.0F, 1.0F}));
            return vng::dsl::make<BuiltinFragmentOutputs>(
                vng::dsl::field(vng::shader::Color<0>{}, color),
                vng::dsl::field(vng::shader::FragmentDepth{}, coordinate.z()));
        });
    if (!fragment) return std::unexpected(std::move(fragment.error()));
    return vng::shader::link(std::move(*vertex), std::move(*fragment));
}

[[nodiscard]] vng::shader::Result<vng::shader::GraphicsProgram> make_matrix_program()
{
    auto vertex = vng::shader::vertex<SimpleVertexInputs, SimpleVertexOutputs>(
        "matrix_vertex",
        [](auto& stage) {
            const auto position = stage.in().get(Position{});
            const auto offset = stage.in().get(Offset{});
            const auto position3 = vng::dsl::vec3(position + offset, 1.0F);

            const auto transform3 = vng::dsl::make<vng::Mat3>(
                stage.constant(vng::Vec3{1.0F, 0.0F, 0.0F}),
                stage.constant(vng::Vec3{0.0F, 1.0F, 0.0F}),
                stage.constant(vng::Vec3{0.0F, 0.0F, 1.0F}));
            const auto transformed3 = transform3 * position3;
            const auto position4 = vng::dsl::vec4(transformed3, 1.0F);

            const auto transform4 = vng::dsl::make<vng::Mat4>(
                stage.constant(vng::Vec4{1.0F, 0.0F, 0.0F, 0.0F}),
                stage.constant(vng::Vec4{0.0F, 1.0F, 0.0F, 0.0F}),
                stage.constant(vng::Vec4{0.0F, 0.0F, 1.0F, 0.0F}),
                stage.constant(vng::Vec4{0.0F, 0.0F, 0.0F, 1.0F}));
            const auto clip = transform4 * position4;

            return vng::dsl::make<SimpleVertexOutputs>(
                vng::dsl::field(vng::shader::ClipPosition{}, clip),
                vng::dsl::field(VertexColor{}, stage.in().get(VertexColor{})));
        });
    if (!vertex) return std::unexpected(std::move(vertex.error()));

    auto fragment = vng::shader::fragment<SimpleFragmentInputs, SimpleFragmentOutputs>(
        "matrix_fragment",
        [](auto& stage) {
            return vng::dsl::make<SimpleFragmentOutputs>(
                vng::dsl::field(
                    vng::shader::Color<0>{},
                    stage.in().get(VertexColor{})));
        });
    if (!fragment) return std::unexpected(std::move(fragment.error()));
    return vng::shader::link(std::move(*vertex), std::move(*fragment));
}

using SparseFragmentOutputs = vng::shader::FragmentOutputs<
    vng::shader::Color<0>,
    vng::shader::Color<7>>;

[[nodiscard]] vng::shader::Result<vng::shader::GraphicsProgram>
make_sparse_fragment_outputs_program()
{
    auto vertex = vng::shader::vertex<SimpleVertexInputs, SimpleVertexOutputs>(
        [](auto& stage) {
            const auto position = stage.in().get(Position{});
            const auto offset = stage.in().get(Offset{});
            return vng::dsl::make<SimpleVertexOutputs>(
                vng::dsl::field(
                    vng::shader::ClipPosition{},
                    vng::dsl::vec4(position + offset, 0.0F, 1.0F)),
                vng::dsl::field(VertexColor{}, stage.in().get(VertexColor{})));
        });
    if (!vertex) return std::unexpected(std::move(vertex.error()));

    auto fragment = vng::shader::fragment<SimpleFragmentInputs, SparseFragmentOutputs>(
        [](auto& stage) {
            const auto color = stage.in().get(VertexColor{});
            return vng::dsl::make<SparseFragmentOutputs>(
                vng::dsl::field(vng::shader::Color<0>{}, color),
                vng::dsl::field(vng::shader::Color<7>{}, color));
        });
    if (!fragment) return std::unexpected(std::move(fragment.error()));
    return vng::shader::link(std::move(*vertex), std::move(*fragment));
}

void require_contains(std::string_view text, std::string_view expected)
{
    INFO("expected generated source to contain: " << expected);
    CHECK(text.find(expected) != std::string_view::npos);
}

} // namespace

TEST_CASE("GLSL emission is deterministic and matches its golden source", "[glsl]")
{
    auto program = make_simple_program();
    INFO((program ? std::string{} : program.error().message));
    REQUIRE(program.has_value());
    auto generated = vng::glsl::emit(*program);
    INFO((generated ? std::string{} : generated.error().message));
    REQUIRE(generated.has_value());
    CHECK(generated->vertex.source == expected_vertex);
    CHECK(generated->fragment.source == expected_fragment);
    REQUIRE(generated->vertex.interface.inputs.size() == 3);
    CHECK(generated->vertex.interface.inputs[0].location == 0);
    CHECK(generated->vertex.interface.inputs[0].glsl_type == "vec2");
    REQUIRE(generated->vertex.interface.outputs.size() == 2);
    CHECK(generated->vertex.interface.outputs[0].builtin ==
          vng::shader::Builtin::clip_position);
    CHECK_FALSE(generated->vertex.interface.outputs[0].location.has_value());
    CHECK(generated->vertex.interface.outputs[1].location == 0);
    REQUIRE(generated->fragment.interface.outputs.size() == 1);
    CHECK(generated->fragment.interface.outputs[0].builtin ==
          vng::shader::Builtin::color);
    CHECK(generated->fragment.interface.outputs[0].builtin_index == 0);
    CHECK(generated->fragment.interface.outputs[0].location == 0);
    CHECK(generated->parameters.empty());

    auto generated_again = vng::glsl::emit(*program);
    REQUIRE(generated_again.has_value());
    CHECK(generated_again->vertex.source == generated->vertex.source);
    CHECK(generated_again->fragment.source == generated->fragment.source);
    CHECK(generated_again->vertex.interface == generated->vertex.interface);
    CHECK(generated_again->fragment.interface == generated->fragment.interface);
    CHECK(generated_again->dump() == generated->dump());

    const auto& vertex_root = program->vertex().ir().regions[
        program->vertex().ir().root_region.value];
    REQUIRE(generated->vertex.source_map.size() == vertex_root.operations.size());
    for (const auto& mapping : generated->vertex.source_map) {
        const auto* found = generated->vertex.mapping_for_line(mapping.generated_line);
        REQUIRE(found != nullptr);
        CHECK(found->operation == mapping.operation);
    }
    CHECK(generated->vertex.mapping_for_line(1) == nullptr);
}

TEST_CASE("ordinary GLSL emission pays no cost for diagnostic observations",
          "[glsl][observation]")
{
    auto ordinary = make_simple_program();
    auto observed = make_observed_simple_program();
    REQUIRE(ordinary);
    REQUIRE(observed);
    REQUIRE(observed->vertex().ir().observations.size() == 1);

    auto ordinary_source = vng::glsl::emit(*ordinary);
    auto observed_source = vng::glsl::emit(*observed);
    REQUIRE(ordinary_source);
    REQUIRE(observed_source);
    CHECK(observed_source->vertex.source == ordinary_source->vertex.source);
    CHECK(observed_source->fragment.source == ordinary_source->fragment.source);
    CHECK(observed_source->vertex.source.find("0.125") == std::string::npos);

    auto fragment_observed = make_fragment_observed_program();
    REQUIRE(fragment_observed);
    auto fragment_observed_source = vng::glsl::emit(*fragment_observed);
    REQUIRE(fragment_observed_source);
    CHECK(fragment_observed_source->vertex.source == ordinary_source->vertex.source);
    CHECK(fragment_observed_source->fragment.source == ordinary_source->fragment.source);
    CHECK_FALSE(fragment_observed_source->observation.has_value());
}

TEST_CASE("one fragment observation lowers to one deterministic attachment",
          "[glsl][observation]")
{
    auto program = make_fragment_observed_program();
    REQUIRE(program);

    const auto fragment_ir_before = program->fragment().dump_ir();
    const auto request = vng::glsl::observation(DiagnosticColor{});
    CHECK(request.stage == vng::shader::StageKind::fragment);
    CHECK(request.semantic_type == typeid(DiagnosticColor));
    CHECK(request.value_type == typeid(vng::Vec3));

    auto generated = vng::glsl::emit(*program, request);
    INFO((generated ? std::string{} : generated.error().message));
    REQUIRE(generated);
    CHECK(program->fragment().dump_ir() == fragment_ir_before);
    CHECK_FALSE(generated->analysis.has_value());
    REQUIRE(generated->observation.has_value());

    const auto& metadata = *generated->observation;
    CHECK(metadata.source_stage == vng::shader::StageKind::fragment);
    CHECK(metadata.semantic_type == typeid(DiagnosticColor));
    CHECK(metadata.value_type == typeid(vng::Vec3));
    CHECK(metadata.semantic_name.find("DiagnosticColor") != std::string::npos);
    CHECK(metadata.glsl_type == "vec3");
    CHECK(metadata.attachment_glsl_type == "vec4");
    CHECK(metadata.scalar_kind == vng::shader::ScalarKind::f32);
    CHECK(metadata.component_count == 3);
    CHECK(metadata.attachment_location == 1);
    CHECK(metadata.output_name == "vng_observation_value");
    CHECK(metadata.origin.line != 0);

    require_contains(
        generated->fragment.source,
        "layout(location = 1) out vec4 vng_observation_value;");
    require_contains(
        generated->fragment.source,
        "vng_observation_value = v");
    require_contains(generated->fragment.source, ", 0.0);");
    require_contains(generated->fragment.source, "0.125");
    CHECK(generated->fragment.source.find("37u") == std::string::npos);
    CHECK(generated->fragment.source.find("gl_FrontFacing") == std::string::npos);

    const auto assignment = generated->fragment.source.find(
        "    vng_observation_value = ");
    REQUIRE(assignment != std::string::npos);
    const auto generated_line = static_cast<std::uint32_t>(
        1 + std::count(
                generated->fragment.source.begin(),
                generated->fragment.source.begin()
                    + static_cast<std::ptrdiff_t>(assignment),
                '\n'));
    const auto* mapping = generated->fragment.mapping_for_line(generated_line);
    REQUIRE(mapping != nullptr);
    const auto selected = std::ranges::find(
        program->fragment().ir().observations,
        typeid(DiagnosticColor),
        &vng::shader::Observation::semantic_type);
    REQUIRE(selected != program->fragment().ir().observations.end());
    CHECK(mapping->operation
          == program->fragment().ir().values[selected->value.value].producer);
    CHECK(mapping->origin.line == selected->origin.line);
    REQUIRE(generated->fragment.interface.outputs.size() == 2);
    CHECK(generated->fragment.interface.outputs.back().semantic_type
          == typeid(DiagnosticColor));
    CHECK(generated->fragment.interface.outputs.back().glsl_type == "vec4");
    CHECK(generated->fragment.interface.outputs.back().location == 1);

    auto generated_again = vng::glsl::emit(
        *program, vng::glsl::observation<DiagnosticColor>());
    REQUIRE(generated_again);
    CHECK(generated_again->vertex.source == generated->vertex.source);
    CHECK(generated_again->fragment.source == generated->fragment.source);
    REQUIRE(generated_again->observation);
    CHECK(*generated_again->observation == metadata);
}

TEST_CASE("observation emission validates selection stage and attachment type",
          "[glsl][observation][diagnostic]")
{
    auto program = make_fragment_observed_program();
    REQUIRE(program);

    auto missing = vng::glsl::emit(
        *program, vng::glsl::observation(MissingDiagnostic{}));
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code
          == vng::shader::DiagnosticCode::interface_mismatch);

    auto wrong_type_request = vng::glsl::observation(DiagnosticColor{});
    wrong_type_request.value_type = typeid(vng::Vec4);
    auto wrong_type = vng::glsl::emit(*program, wrong_type_request);
    REQUIRE_FALSE(wrong_type);
    CHECK(wrong_type.error().code
          == vng::shader::DiagnosticCode::interface_mismatch);

    auto unsupported_bool = vng::glsl::emit(
        *program, vng::glsl::observation(DiagnosticMask{}));
    REQUIRE_FALSE(unsupported_bool);
    CHECK(unsupported_bool.error().code
          == vng::shader::DiagnosticCode::unsupported_operation);

    auto vertex_program = make_observed_simple_program();
    REQUIRE(vertex_program);
    auto unsupported_vertex = vng::glsl::emit(
        *vertex_program,
        vng::glsl::observation(
            DiagnosticOffset{}, vng::shader::StageKind::vertex));
    REQUIRE_FALSE(unsupported_vertex);
    CHECK(unsupported_vertex.error().code
          == vng::shader::DiagnosticCode::unsupported_operation);
    CHECK(unsupported_vertex.error().message.find("interpolation")
          != std::string::npos);
}

TEST_CASE("integer observations retain their integer attachment contract",
          "[glsl][observation]")
{
    auto program = make_fragment_observed_program();
    REQUIRE(program);

    auto generated = vng::glsl::emit(
        *program, vng::glsl::observation(DiagnosticCounter{}));
    INFO((generated ? std::string{} : generated.error().message));
    REQUIRE(generated);
    REQUIRE(generated->observation);
    CHECK(generated->observation->glsl_type == "uint");
    CHECK(generated->observation->attachment_glsl_type == "uvec4");
    CHECK(generated->observation->scalar_kind == vng::shader::ScalarKind::u32);
    CHECK(generated->observation->component_count == 1);
    require_contains(
        generated->fragment.source,
        "layout(location = 1) out uvec4 vng_observation_value;");
    require_contains(generated->fragment.source, "37u");
    require_contains(generated->fragment.source, ", 0u, 0u, 0u);");
    CHECK(generated->fragment.source.find("0.125") == std::string::npos);
    CHECK(generated->fragment.source.find("gl_FrontFacing") == std::string::npos);
}

TEST_CASE("camera parameters lower to deterministic GLSL and program metadata",
          "[glsl][camera][parameter]")
{
    auto program = make_camera_program();
    INFO((program ? std::string{} : program.error().message));
    REQUIRE(program);

    auto generated = vng::glsl::emit(*program);
    INFO((generated ? std::string{} : generated.error().message));
    REQUIRE(generated);
    CHECK(generated->vertex.source == expected_camera_vertex);
    REQUIRE(generated->parameters.size() == 1);
    CHECK(generated->parameters[0].kind ==
          vng::shader::ParameterKind::camera_view_projection);
    CHECK(generated->parameters[0].name == "vng_camera_view_projection");
    CHECK(generated->parameters[0].glsl_type == "mat4");
    CHECK(generated->parameters[0].location == 0);
    require_contains(
        generated->vertex.source,
        "mat4 v1 = vng_camera_view_projection;");

    auto generated_again = vng::glsl::emit(*program);
    REQUIRE(generated_again);
    CHECK(generated_again->vertex.source == generated->vertex.source);
    CHECK(generated_again->parameters == generated->parameters);

    auto analysis = vng::glsl::emit(
        *program, vng::glsl::AnalysisEmission{});
    INFO((analysis ? std::string{} : analysis.error().message));
    REQUIRE(analysis);
    REQUIRE(analysis->analysis);
    CHECK(analysis->analysis->first_item_uniform_location == 1);
    CHECK(analysis->parameters == generated->parameters);
    require_contains(
        analysis->vertex.source,
        "layout(location = 0) uniform mat4 vng_camera_view_projection;");
    require_contains(
        analysis->vertex.source,
        "layout(location = 1) uniform uint vng_analysis_first_item_id;");
}

TEST_CASE("analysis emission adds deterministic surface identity plumbing",
          "[glsl][analysis]")
{
    auto program = make_simple_program();
    INFO((program ? std::string{} : program.error().message));
    REQUIRE(program.has_value());

    const auto vertex_ir_before = program->vertex().dump_ir();
    const auto fragment_ir_before = program->fragment().dump_ir();
    auto generated = vng::glsl::emit(*program, vng::glsl::AnalysisEmission{});
    INFO((generated ? std::string{} : generated.error().message));
    REQUIRE(generated.has_value());

    CHECK(program->vertex().dump_ir() == vertex_ir_before);
    CHECK(program->fragment().dump_ir() == fragment_ir_before);
    CHECK(generated->vertex.source == expected_analysis_vertex);
    CHECK(generated->fragment.source == expected_analysis_fragment);

    REQUIRE(generated->analysis.has_value());
    CHECK(generated->analysis->item_id_varying_location == 1);
    CHECK(generated->analysis->surface_key_location == 1);
    CHECK(generated->analysis->first_item_uniform_name
          == "vng_analysis_first_item_id");
    CHECK(generated->analysis->first_item_uniform_location == 0);

    REQUIRE(generated->vertex.interface.inputs.size() == 3);
    CHECK(generated->vertex.interface.inputs[0].semantic_type == typeid(Position));
    REQUIRE(generated->vertex.interface.outputs.size() == 3);
    const auto& generated_vertex_output = generated->vertex.interface.outputs.back();
    CHECK(generated_vertex_output.semantic_name == "vng.analysis.item_id");
    CHECK(generated_vertex_output.glsl_type == "uint");
    CHECK(generated_vertex_output.location == 1);
    CHECK(generated_vertex_output.interpolation == vng::shader::Interpolation::flat);

    REQUIRE(generated->fragment.interface.inputs.size() == 2);
    CHECK(generated->fragment.interface.inputs.back() == generated_vertex_output);
    REQUIRE(generated->fragment.interface.outputs.size() == 2);
    const auto& generated_surface_key = generated->fragment.interface.outputs.back();
    CHECK(generated_surface_key.semantic_name == "vng.analysis.surface_key");
    CHECK(generated_surface_key.glsl_type == "uvec2");
    CHECK(generated_surface_key.location == 1);

    auto generated_again = vng::glsl::emit(
        *program, vng::glsl::AnalysisEmission{});
    REQUIRE(generated_again.has_value());
    CHECK(generated_again->vertex.source == generated->vertex.source);
    CHECK(generated_again->fragment.source == generated->fragment.source);
    CHECK(generated_again->analysis == generated->analysis);

    auto normal_after_analysis = vng::glsl::emit(*program);
    REQUIRE(normal_after_analysis.has_value());
    CHECK(normal_after_analysis->vertex.source == expected_vertex);
    CHECK(normal_after_analysis->fragment.source == expected_fragment);
    CHECK_FALSE(normal_after_analysis->analysis.has_value());
}

TEST_CASE("analysis output uses the lowest free sparse fragment target location",
          "[glsl][analysis]")
{
    auto program = make_sparse_fragment_outputs_program();
    INFO((program ? std::string{} : program.error().message));
    REQUIRE(program.has_value());

    auto generated = vng::glsl::emit(*program, vng::glsl::AnalysisEmission{});
    INFO((generated ? std::string{} : generated.error().message));
    REQUIRE(generated.has_value());
    REQUIRE(generated->analysis.has_value());
    CHECK(generated->analysis->surface_key_location == 1);
    require_contains(
        generated->fragment.source,
        "layout(location = 7) out vec4 vng_out_1;");
    require_contains(
        generated->fragment.source,
        "layout(location = 1) out uvec2 vng_analysis_surface_key;");
}

TEST_CASE("GLSL emission covers every current expression operation", "[glsl]")
{
    auto operations_program = make_operations_program();
    INFO((operations_program ? std::string{} : operations_program.error().message));
    REQUIRE(operations_program.has_value());
    auto operations = vng::glsl::emit(*operations_program);
    INFO((operations ? std::string{} : operations.error().message));
    REQUIRE(operations.has_value());
    const std::string_view source = operations->fragment.source;
    require_contains(source, " % ");
    require_contains(source, " = (~");
    require_contains(source, " & ");
    require_contains(source, " | ");
    require_contains(source, " ^ ");
    require_contains(source, " << ");
    require_contains(source, " >> ");
    require_contains(source, "float(");
    require_contains(source, "lessThan(");
    require_contains(source, "lessThanEqual(");
    require_contains(source, "greaterThan(");
    require_contains(source, "greaterThanEqual(");
    require_contains(source, "equal(");
    require_contains(source, "notEqual(");
    require_contains(source, "all(");
    require_contains(source, "any(");
    require_contains(source, "mix(");
    require_contains(source, "cross(");
    require_contains(source, "dot(");
    require_contains(source, "normalize(");
    require_contains(source, "min(");
    require_contains(source, "max(");
    require_contains(source, "clamp(");
    require_contains(source, "sqrt(");
    require_contains(source, " ? ");
    require_contains(source, " = (!");
}

TEST_CASE("GLSL emission maps all supported graphics builtins", "[glsl][builtin]")
{
    auto builtin_program = make_builtin_program();
    INFO((builtin_program ? std::string{} : builtin_program.error().message));
    REQUIRE(builtin_program.has_value());
    auto builtins = vng::glsl::emit(*builtin_program);
    INFO((builtins ? std::string{} : builtins.error().message));
    REQUIRE(builtins.has_value());
    require_contains(builtins->vertex.source, "gl_VertexID");
    require_contains(builtins->vertex.source, "gl_InstanceID");
    require_contains(builtins->vertex.source, "gl_Position =");
    require_contains(builtins->fragment.source, "gl_FragCoord");
    require_contains(builtins->fragment.source, "gl_FrontFacing");
    require_contains(builtins->fragment.source, "gl_FragDepth =");
}

TEST_CASE("GLSL emission preserves matrix construction and matrix-vector multiplication",
          "[glsl][matrix]")
{
    auto program = make_matrix_program();
    INFO((program ? std::string{} : program.error().message));
    REQUIRE(program.has_value());
    auto generated = vng::glsl::emit(*program);
    INFO((generated ? std::string{} : generated.error().message));
    REQUIRE(generated.has_value());

    const auto& module = program->vertex().ir();
    const auto& source = generated->vertex.source;
    bool saw_mat3_construct = false;
    bool saw_mat4_construct = false;
    bool saw_mat4_vector_multiply = false;

    for (const auto& operation : module.operations) {
        if (!operation.result.valid()) {
            continue;
        }
        const auto& result_type = module.types[
            module.values[operation.result.value].type];

        if (operation.opcode == vng::shader::OpCode::construct
            && result_type.kind == vng::shader::TypeKind::matrix) {
            const auto type_name = result_type.columns == 3 ? "mat3" : "mat4";
            const auto expected = std::string(type_name) + " v"
                + std::to_string(operation.result.value) + " = " + type_name + '(';
            require_contains(source, expected);
            saw_mat3_construct = saw_mat3_construct || result_type.columns == 3;
            saw_mat4_construct = saw_mat4_construct || result_type.columns == 4;
        }

        if (operation.opcode == vng::shader::OpCode::multiply
            && operation.operands.size() == 2) {
            const auto& left_type = module.types[
                module.values[operation.operands[0].value].type];
            const auto& right_type = module.types[
                module.values[operation.operands[1].value].type];
            if (left_type.kind == vng::shader::TypeKind::matrix
                && left_type.columns == 4
                && right_type.kind == vng::shader::TypeKind::vector
                && right_type.columns == 4) {
                const auto expected = "vec4 v" + std::to_string(operation.result.value)
                    + " = (v" + std::to_string(operation.operands[0].value)
                    + " * v" + std::to_string(operation.operands[1].value) + ");";
                require_contains(source, expected);
                saw_mat4_vector_multiply = true;
            }
        }
    }

    CHECK(saw_mat3_construct);
    CHECK(saw_mat4_construct);
    CHECK(saw_mat4_vector_multiply);

    auto generated_again = vng::glsl::emit(*program);
    REQUIRE(generated_again.has_value());
    CHECK(generated_again->vertex.source == source);
}

TEST_CASE("2D texture emission declares live bindings and preserves diagnostic variants",
          "[glsl][texture]")
{
    auto base = make_simple_program();
    REQUIRE(base);
    auto fragment = vng::shader::fragment<SimpleFragmentInputs, SimpleFragmentOutputs>(
        [](auto& stage) {
            const auto uv = stage.input(VertexColor{}).xy();
            const auto sampled = stage.template sample_2d<3>(uv);
            const auto other = stage.template sample_2d<1>(uv);
            const auto unused = stage.template sample_2d<9>(uv);
            (void)unused;
            stage.observe(DiagnosticColor{}, stage.template sample_2d<7>(uv).xyz());
            return stage.output(vng::dsl::field<vng::shader::Color<0>>(
                sampled + other));
        });
    REQUIRE(fragment);
    auto program = vng::shader::link(base->vertex(), std::move(*fragment));
    REQUIRE(program);
    auto normal = vng::glsl::emit(*program);
    auto enhanced = vng::glsl::emit(*program, vng::glsl::AnalysisEmission{});
    auto observed = vng::glsl::emit(*program, vng::glsl::observation(DiagnosticColor{}));
    REQUIRE(normal);
    REQUIRE(enhanced);
    REQUIRE(observed);
    CHECK(normal->vertex.texture_bindings.empty());
    CHECK(normal->fragment.texture_bindings == std::vector<vng::u32>{1, 3});
    CHECK(normal->texture_bindings == std::vector<vng::u32>{1, 3});
    CHECK(enhanced->texture_bindings == normal->texture_bindings);
    CHECK(observed->texture_bindings == std::vector<vng::u32>{1, 3, 7});
    require_contains(normal->fragment.source,
        "layout(binding = 1) uniform sampler2D vng_texture_1;\n"
        "layout(binding = 3) uniform sampler2D vng_texture_3;\n");
    require_contains(normal->fragment.source, "texture(vng_texture_3, ");
    require_contains(enhanced->fragment.source, "texture(vng_texture_3, ");
    require_contains(observed->fragment.source, "texture(vng_texture_7, ");
    CHECK(normal->fragment.source.find("vng_texture_7") == std::string::npos);
    CHECK(observed->fragment.source.find("vng_texture_9") == std::string::npos);

    const auto texture_position = normal->fragment.source.find("texture(vng_texture_3");
    REQUIRE(texture_position != std::string::npos);
    const auto generated_line = static_cast<vng::u32>(std::count(
        normal->fragment.source.begin(),
        normal->fragment.source.begin() + static_cast<std::ptrdiff_t>(texture_position), '\n') + 1);
    const auto* mapping = normal->fragment.mapping_for_line(generated_line);
    REQUIRE(mapping != nullptr);
    CHECK(program->fragment().ir().operations[mapping->operation.value].opcode
          == vng::shader::OpCode::texture_sample);
    auto repeated = vng::glsl::emit(*program);
    REQUIRE(repeated);
    CHECK(repeated->dump() == normal->dump());
}

TEST_CASE("matrix buffer emission declares live bindings and preserves deformation in diagnostic variants",
          "[glsl][matrix_buffer]")
{
    auto vertex = vng::shader::vertex<SimpleVertexInputs, SimpleVertexOutputs>([](auto& s) {
        const auto index = s.constant(vng::u32{});
        const auto moved = s.template matrix_buffer<4>(index)
                         * s.template matrix_buffer<1>(index)
                         * vng::dsl::vec4(s.input(Position{}), 0.0F, 1.0F);
        return s.output(vng::dsl::field<vng::shader::ClipPosition>(moved),
                        vng::dsl::field<VertexColor>(s.input(VertexColor{})));
    });
    auto fragment = vng::shader::fragment<SimpleFragmentInputs, SimpleFragmentOutputs>([](auto& s) {
        const auto index = s.constant(vng::u32{2});
        const auto first = s.template matrix_buffer<3>(index);
        const auto second = s.template matrix_buffer<1>(index);
        const auto duplicate = s.template matrix_buffer<3>(index);
        const auto unused = s.template matrix_buffer<9>(index);
        (void)unused;
        s.observe(DiagnosticColor{}, (s.template matrix_buffer<7>(index) * s.input(VertexColor{})).xyz());
        return s.output(vng::dsl::field<vng::shader::Color<0>>(
            (first + second + duplicate) * s.input(VertexColor{})));
    });
    REQUIRE(vertex);
    REQUIRE(fragment);
    auto program = vng::shader::link(std::move(*vertex), std::move(*fragment));
    REQUIRE(program);
    const auto original_ir = program->vertex().dump_ir() + program->fragment().dump_ir();
    auto normal = vng::glsl::emit(*program);
    auto enhanced = vng::glsl::emit(*program, vng::glsl::AnalysisEmission{});
    auto observed = vng::glsl::emit(*program, vng::glsl::observation(DiagnosticColor{}));
    REQUIRE(normal);
    REQUIRE(enhanced);
    REQUIRE(observed);
    CHECK(normal->vertex.matrix_buffer_bindings == std::vector<vng::u32>{1, 4});
    CHECK(normal->fragment.matrix_buffer_bindings == std::vector<vng::u32>{1, 3});
    CHECK(normal->matrix_buffer_bindings == std::vector<vng::u32>{1, 3, 4});
    CHECK(enhanced->matrix_buffer_bindings == normal->matrix_buffer_bindings);
    CHECK(observed->matrix_buffer_bindings == std::vector<vng::u32>{1, 3, 4, 7});
    CHECK(normal->texture_bindings.empty());
    CHECK(program->vertex().dump_ir() + program->fragment().dump_ir() == original_ir);

    const std::string declarations =
        "layout(std430, binding = 1) readonly buffer vng_matrices_1\n"
        "{\n    mat4 vng_matrix_1[];\n};\n"
        "layout(std430, binding = 3) readonly buffer vng_matrices_3\n"
        "{\n    mat4 vng_matrix_3[];\n};\n";
    require_contains(normal->fragment.source, declarations);
    require_contains(enhanced->fragment.source, declarations);
    require_contains(normal->vertex.source, " = vng_matrix_4[");
    require_contains(enhanced->vertex.source, " = vng_matrix_4[");
    CHECK(observed->vertex.source == normal->vertex.source);
    require_contains(observed->fragment.source, " = vng_matrix_7[");
    CHECK(normal->fragment.source.find("vng_matrix_7") == std::string::npos);
    CHECK(observed->fragment.source.find("vng_matrix_9") == std::string::npos);

    const auto position = normal->vertex.source.find(" = vng_matrix_4[");
    REQUIRE(position != std::string::npos);
    const auto line = static_cast<vng::u32>(std::count(
        normal->vertex.source.begin(),
        normal->vertex.source.begin() + static_cast<std::ptrdiff_t>(position), '\n') + 1);
    const auto* mapping = normal->vertex.mapping_for_line(line);
    REQUIRE(mapping != nullptr);
    CHECK(program->vertex().ir().operations[mapping->operation.value].opcode
          == vng::shader::OpCode::matrix_buffer_read);
    auto repeated = vng::glsl::emit(*program);
    REQUIRE(repeated);
    CHECK(repeated->dump() == normal->dump());
}

TEST_CASE("unsupported IR returns a source-preserving diagnostic", "[glsl][diagnostic]")
{
    auto program = make_simple_program();
    REQUIRE(program.has_value());
    auto fragment_ir = program->fragment().ir();
    const auto unsupported_id = vng::shader::OperationId{
        static_cast<vng::u32>(fragment_ir.operations.size())};
    fragment_ir.operations.push_back(vng::shader::Operation{
        .opcode = vng::shader::OpCode::if_region,
        .operands = {},
        .result = {},
        .regions = {},
        .payload = {},
        .effect = vng::shader::Effect::pure,
        .origin = {},
    });
    auto& root_operations = fragment_ir.regions[fragment_ir.root_region.value].operations;
    root_operations.insert(root_operations.end() - 1, unsupported_id);
    auto unsupported_stage = vng::shader::detail::ShaderStageAccess::from_ir(
        std::move(fragment_ir));
    auto unsupported = vng::glsl::emit(unsupported_stage);
    REQUIRE_FALSE(unsupported.has_value());
    CHECK(unsupported.error().code == vng::shader::DiagnosticCode::unsupported_operation);
    CHECK_FALSE(unsupported.error().generated_source.empty());
}
