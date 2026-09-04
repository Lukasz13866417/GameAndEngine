#include <vng/gfx/gfx.hpp>
#include <vng/shader/shader.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

struct Position : vng::gfx::Semantic<vng::Vec2> {};
struct Tint : vng::gfx::Semantic<vng::Vec4> {};
struct InstanceOffset : vng::gfx::Semantic<vng::Vec2> {};
struct MissingVarying : vng::gfx::Semantic<vng::Vec3> {};
struct SecondaryTint : vng::gfx::Semantic<vng::Vec4> {};
struct UserFlag : vng::gfx::Semantic<bool> {};
struct UserMask : vng::gfx::Semantic<vng::Vector<bool, 3>> {};
struct MatrixInput : vng::gfx::Semantic<vng::Mat4> {};
struct ObservedOffset : vng::gfx::Semantic<vng::Vec2> {};

using VertexIn = vng::shader::VertexInputs<Position, Tint, InstanceOffset>;
using VertexOut = vng::shader::VertexOutputs<
    vng::shader::ClipPosition,
    vng::shader::smooth<Tint>>;
using FragmentIn = vng::shader::FragmentInputs<vng::shader::smooth<Tint>>;
using FragmentOut = vng::shader::FragmentOutputs<vng::shader::Color<0>>;

using CpuVertex = vng::gfx::Record<Position, Tint>;
using VertexStageContext = vng::shader::StageContext<
    vng::shader::StageKind::vertex,
    VertexIn,
    VertexOut>;

template<class Left, class Right>
concept Addable = requires(Left left, Right right) { left + right; };

template<class Left, class Right>
concept Andable = requires(Left left, Right right) { left && right; };

template<class Value>
concept HostBoolean = requires(Value value) { static_cast<bool>(value); };

template<class Value>
concept Negatable = requires(Value value) { -value; };

template<class Value, class Low, class High>
concept Clampable = requires(Value value, Low low, High high) {
    vng::dsl::clamp(value, low, high);
};

static_assert(std::same_as<typename VertexIn::semantics,
                           vng::gfx::TypeList<Position, Tint, InstanceOffset>>);
static_assert(vng::shader::Value<CpuVertex>);
static_assert(std::same_as<vng::dsl::Float, vng::dsl::Expr<vng::f32>>);
static_assert(std::same_as<vng::dsl::Float4, vng::dsl::Expr<vng::Vec4>>);
static_assert(!std::default_initializable<vng::dsl::Float>);
static_assert(std::is_trivially_copy_constructible_v<vng::dsl::Float>);
static_assert(Addable<vng::dsl::Float, vng::f32>);
static_assert(!Addable<vng::dsl::Float, vng::f64>);
static_assert(!Addable<vng::dsl::Float, vng::u32>);
static_assert(!Andable<vng::dsl::Bool, vng::dsl::Bool>);
static_assert(!HostBoolean<vng::dsl::Bool>);
static_assert(!Negatable<vng::dsl::UInt>);
static_assert(!Negatable<vng::dsl::UInt2>);
static_assert(Negatable<vng::dsl::Int2>);
static_assert(!std::constructible_from<vng::shader::ShaderStage, vng::shader::ModuleIR>);
static_assert(Clampable<vng::dsl::Float, vng::f32, vng::f32>);
static_assert(Clampable<vng::dsl::Float3, vng::f32, vng::f32>);
static_assert(Clampable<vng::dsl::Float3, vng::dsl::Float3, vng::dsl::Float3>);
static_assert(!Clampable<vng::dsl::Float3, vng::f32, vng::dsl::Float3>);
static_assert(!Clampable<vng::dsl::Float3, vng::dsl::Float3, vng::f32>);
static_assert(std::same_as<
              decltype(std::declval<VertexStageContext&>().input(Position{})),
              vng::dsl::Float2>);
static_assert(std::same_as<
              decltype(std::declval<VertexStageContext&>().template input<Position>()),
              vng::dsl::Float2>);
static_assert(std::same_as<
              decltype(std::declval<VertexStageContext&>().camera().view_projection()),
              vng::dsl::Float4x4>);
static_assert(std::same_as<
              decltype(std::declval<VertexStageContext&>().camera().project(
                  std::declval<vng::dsl::Float3>())),
              vng::dsl::Float4>);
static_assert(vng::dsl::NamedFieldExpression<decltype(
              vng::dsl::field<Position>(std::declval<vng::dsl::Float2>()))>);

[[nodiscard]] vng::shader::Result<vng::shader::ShaderStage> make_vertex(int& calls)
{
    return vng::shader::vertex<VertexIn, VertexOut>("test_vertex", [&](auto& stage) {
        ++calls;
        auto position = stage.input(Position{});
        auto offset = stage.input(InstanceOffset{});
        auto instance = vng::dsl::cast<vng::f32>(stage.instance_index());
        auto zero_instance_shift = vng::dsl::vec2(instance * 0.0F, 0.0F);
        auto sum = position + offset + zero_instance_shift;
        auto copied = sum; // A handle copy must not add an IR operation.

        auto unused = copied + vng::Vec2{1.0F, 1.0F};
        (void)unused; // Pure DCE should remove this operation from the root region.

        // Named outputs deliberately arrive in the opposite order from VertexOut.
        // Record construction canonicalizes them by semantic, never by position.
        return stage.output(
            vng::dsl::field<Tint>(stage.input(Tint{})),
            vng::dsl::field<vng::shader::ClipPosition>(
                vng::dsl::vec4(copied, 0.0F, 1.0F)));
    });
}

[[nodiscard]] vng::shader::Result<vng::shader::ShaderStage> make_fragment(int& calls)
{
    return vng::shader::fragment<FragmentIn, FragmentOut>("test_fragment", [&](auto& stage) {
        ++calls;
        return stage.output(
            vng::dsl::field<vng::shader::Color<0>>(
                stage.input(Tint{})));
    });
}

[[nodiscard]] vng::shader::Result<vng::shader::GraphicsProgram>
make_camera_program()
{
    auto vertex = vng::shader::vertex<VertexIn, VertexOut>(
        "camera_vertex",
        [](auto& stage) {
            const auto world_position = vng::dsl::vec3(
                stage.input(Position{}), 0.0F);
            const auto clip_position = stage.camera().project(world_position);
            // Repeated reads share one parameter-table entry even though each
            // expression remains an ordinary immutable IR value.
            const auto second_read = stage.camera().view_projection();
            const auto unchanged = second_read * vng::Vec4{};
            return stage.output(
                vng::dsl::field<vng::shader::ClipPosition>(
                    clip_position + unchanged),
                vng::dsl::field<Tint>(stage.input(Tint{})));
        });
    if (!vertex) {
        return std::unexpected(std::move(vertex.error()));
    }

    auto fragment = vng::shader::fragment<FragmentIn, FragmentOut>(
        "camera_fragment",
        [](auto& stage) {
            const auto transformed = stage.camera().view_projection()
                * stage.input(Tint{});
            return stage.output(
                vng::dsl::field<vng::shader::Color<0>>(transformed));
        });
    if (!fragment) {
        return std::unexpected(std::move(fragment.error()));
    }
    return vng::shader::link(std::move(*vertex), std::move(*fragment));
}

TEST_CASE("shader lambdas build deterministic IR and linked interfaces", "[shader][stage][link]")
{
    int vertex_calls = 0;
    int fragment_calls = 0;
    auto vertex = make_vertex(vertex_calls);
    auto fragment = make_fragment(fragment_calls);
    REQUIRE(vertex);
    REQUIRE(fragment);
    REQUIRE(vertex_calls == 1);
    REQUIRE(fragment_calls == 1);

    const auto first_dump = vertex->dump_ir();
    int second_calls = 0;
    auto second = make_vertex(second_calls);
    REQUIRE(second);
    REQUIRE(second_calls == 1);
    REQUIRE(first_dump == second->dump_ir());
    REQUIRE(first_dump.find("add") != std::string::npos);
    REQUIRE(first_dump.find("stage_output") != std::string::npos);

    auto program = vng::shader::link(std::move(*vertex), std::move(*fragment));
    REQUIRE(program);

    const auto& vertex_ir = program->vertex().ir();
    const auto& fragment_ir = program->fragment().ir();
    REQUIRE(vertex_ir.inputs.size() == 4);
    REQUIRE(vertex_ir.inputs[0].location == 0);
    REQUIRE(vertex_ir.inputs[1].location == 1);
    REQUIRE(vertex_ir.inputs[2].location == 2);
    REQUIRE(vertex_ir.inputs[3].builtin == vng::shader::Builtin::instance_index);
    REQUIRE(!vertex_ir.inputs[3].location);
    REQUIRE(vertex_ir.outputs[0].builtin == vng::shader::Builtin::clip_position);
    REQUIRE(!vertex_ir.outputs[0].location);
    REQUIRE(vertex_ir.outputs[1].location == 0);
    REQUIRE(fragment_ir.inputs[0].location == 0);
    REQUIRE(fragment_ir.outputs[0].location == 0);

    const auto interface_dump = program->dump_interface();
    REQUIRE(interface_dump.find("location=2") != std::string::npos);
    REQUIRE(interface_dump.find("smooth") != std::string::npos);
}

TEST_CASE("semantic observations retain selectable IR without becoming outputs",
          "[shader][ir][observation]")
{
    auto observed = vng::shader::vertex<VertexIn, VertexOut>([](auto& stage) {
        const auto position = stage.input(Position{});
        const auto result = stage.output(
            vng::dsl::field<vng::shader::ClipPosition>(
                vng::dsl::vec4(position, 0.0F, 1.0F)),
            vng::dsl::field<Tint>(stage.input(Tint{})));

        // This calculation is not part of the ordinary stage result. It is
        // retained solely because a diagnostic emission may select it.
        const auto diagnostic_value = position + vng::Vec2{0.25F, 0.5F};
        stage.observe(ObservedOffset{}, diagnostic_value);
        return result;
    });
    REQUIRE(observed);
    REQUIRE(observed->ir().observations.size() == 1);
    CHECK(observed->ir().observations[0].semantic_type == typeid(ObservedOffset));
    CHECK(observed->ir().observations[0].value_type == typeid(vng::Vec2));
    CHECK(observed->dump_ir().find("observe") != std::string::npos);
    CHECK(observed->dump_ir().find("ObservedOffset") != std::string::npos);
    CHECK(std::ranges::any_of(observed->ir().operations, [](const auto& operation) {
        return operation.opcode == vng::shader::OpCode::add;
    }));

    auto duplicate = vng::shader::vertex<VertexIn, VertexOut>([](auto& stage) {
        const auto position = stage.input(Position{});
        stage.observe(ObservedOffset{}, position);
        stage.observe(ObservedOffset{}, position);
        return stage.output(
            vng::dsl::field<vng::shader::ClipPosition>(
                vng::dsl::vec4(position, 0.0F, 1.0F)),
            vng::dsl::field<Tint>(stage.input(Tint{})));
    });
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error().code == vng::shader::DiagnosticCode::duplicate_semantic);

    vng::shader::ModuleIR foreign_module;
    vng::shader::FunctionBuilder foreign_builder{foreign_module};
    const auto foreign = vng::dsl::detail::literal(
        foreign_builder, vng::Vec2{1.0F, 2.0F});
    auto mixed = vng::shader::vertex<VertexIn, VertexOut>([&](auto& stage) {
        const auto position = stage.input(Position{});
        stage.observe(ObservedOffset{}, foreign);
        return stage.output(
            vng::dsl::field<vng::shader::ClipPosition>(
                vng::dsl::vec4(position, 0.0F, 1.0F)),
            vng::dsl::field<Tint>(stage.input(Tint{})));
    });
    REQUIRE_FALSE(mixed);
    CHECK(mixed.error().code == vng::shader::DiagnosticCode::mixed_builders);
}

TEST_CASE("shader constants, records, and pure folding produce typed IR", "[shader][ir][dsl]")
{
    vng::shader::ModuleIR module;
    vng::shader::FunctionBuilder builder{module};

    auto two = vng::dsl::detail::literal(builder, 2.0F);
    auto folded = two + 3.0F;
    const auto operands = std::array{folded.id()};
    builder.statement(vng::shader::OpCode::stage_output,
                      operands,
                      vng::shader::InterfacePayload{0},
                      vng::shader::Effect::storage_write);

    // Give the deliberately hand-built IR a matching output contract.
    module.outputs.push_back(vng::shader::InterfaceField{
        .semantic_type = typeid(Tint),
        .value_type = typeid(vng::f32),
        .semantic_name = "scalar",
        .type = builder.type<vng::f32>(),
        .interpolation = vng::shader::Interpolation::none,
        .builtin = vng::shader::Builtin::none,
        .builtin_index = 0,
        .location = 0,
    });
    vng::shader::optimize(module);
    REQUIRE(vng::shader::dump_ir(module).find("add") == std::string::npos);

    CpuVertex cpu;
    cpu.set(Position{}, {0.25F, -0.5F});
    cpu.set(Tint{}, {1.0F, 0.5F, 0.25F, 1.0F});
    auto record = vng::dsl::detail::literal(builder, cpu);
    auto position = record.get(Position{});
    static_assert(std::same_as<decltype(position), vng::dsl::Float2>);
    REQUIRE(position.builder() == &builder);
}

TEST_CASE("matrix construction and vector multiplication survive stage optimization", "[shader][ir][dsl][matrix]")
{
    auto stage = vng::shader::vertex<VertexIn, VertexOut>([](auto& context) {
        const auto position = context.inputs().get(Position{});
        const auto position3 = vng::dsl::vec3(position, 1.0F);
        const auto transformed3 = context.constant(vng::Mat3::identity()) * position3;
        const auto position4 = vng::dsl::vec4(transformed3, 1.0F);
        const auto transformed4 = context.constant(vng::Mat4::identity()) * position4;
        return vng::dsl::make<VertexOut>(
            vng::dsl::field(vng::shader::ClipPosition{}, transformed4),
            vng::dsl::field(Tint{}, context.inputs().get(Tint{})));
    });
    REQUIRE(stage);

    const auto multiply_count = std::count_if(
        stage->ir().operations.begin(),
        stage->ir().operations.end(),
        [](const auto& operation) { return operation.opcode == vng::shader::OpCode::multiply; });
    CHECK(multiply_count == 2);
    bool has_mat3_multiply = false;
    bool has_mat4_multiply = false;
    for (const auto& operation : stage->ir().operations) {
        if (operation.opcode != vng::shader::OpCode::multiply) {
            continue;
        }
        const auto& left = stage->ir().types[
            stage->ir().values[operation.operands[0].value].type];
        has_mat3_multiply = has_mat3_multiply ||
            (left.kind == vng::shader::TypeKind::matrix && left.columns == 3);
        has_mat4_multiply = has_mat4_multiply ||
            (left.kind == vng::shader::TypeKind::matrix && left.columns == 4);
    }
    CHECK(has_mat3_multiply);
    CHECK(has_mat4_multiply);
}

TEST_CASE("camera access builds typed parameters shared across linked stages",
          "[shader][ir][dsl][camera][link]")
{
    auto program = make_camera_program();
    INFO((program ? std::string{} : program.error().message));
    REQUIRE(program);

    const auto& vertex = program->vertex().ir();
    const auto& fragment = program->fragment().ir();
    REQUIRE(vertex.parameters.size() == 1);
    REQUIRE(fragment.parameters.size() == 1);
    CHECK(vertex.parameters[0].kind ==
          vng::shader::ParameterKind::camera_view_projection);
    CHECK(fragment.parameters[0].kind == vertex.parameters[0].kind);
    REQUIRE(vertex.parameters[0].location);
    REQUIRE(fragment.parameters[0].location);
    CHECK(*vertex.parameters[0].location == 0);
    CHECK(fragment.parameters[0].location == vertex.parameters[0].location);
    CHECK(vertex.types[vertex.parameters[0].type].canonical_key == "mat4x4");
    CHECK(fragment.types[fragment.parameters[0].type].canonical_key == "mat4x4");

    const auto parameter_reads = [](const vng::shader::ModuleIR& module) {
        return std::count_if(
            module.operations.begin(),
            module.operations.end(),
            [](const auto& operation) {
                return operation.opcode == vng::shader::OpCode::parameter;
            });
    };
    CHECK(parameter_reads(vertex) == 2);
    CHECK(parameter_reads(fragment) == 1);
    for (const auto& operation : vertex.operations) {
        if (operation.opcode == vng::shader::OpCode::parameter) {
            CHECK(operation.effect == vng::shader::Effect::parameter_read);
        }
    }
    CHECK(program->vertex().dump_ir().find(
              "parameter [0:camera_view_projection]") != std::string::npos);
    CHECK(program->dump_interface().find(
              "camera_view_projection : vng::Matrix<4> location=0") !=
          std::string::npos);

    auto malformed = vertex;
    const auto read = std::ranges::find_if(
        malformed.operations,
        [](const auto& operation) {
            return operation.opcode == vng::shader::OpCode::parameter;
        });
    REQUIRE(read != malformed.operations.end());
    read->effect = vng::shader::Effect::pure;
    auto invalid_effect = vng::shader::validate(malformed);
    REQUIRE_FALSE(invalid_effect);
    CHECK(invalid_effect.error().message.find("effect classification") !=
          std::string::npos);

    malformed = vertex;
    const auto bad_payload = std::ranges::find_if(
        malformed.operations,
        [](const auto& operation) {
            return operation.opcode == vng::shader::OpCode::parameter;
        });
    REQUIRE(bad_payload != malformed.operations.end());
    bad_payload->payload = vng::shader::ParameterPayload{77};
    auto invalid_payload = vng::shader::validate(malformed);
    REQUIRE_FALSE(invalid_payload);
    CHECK(invalid_payload.error().message.find("parameter read") !=
          std::string::npos);

    malformed = vertex;
    malformed.parameters.push_back(malformed.parameters.front());
    auto duplicate_parameter = vng::shader::validate(malformed);
    REQUIRE_FALSE(duplicate_parameter);
    CHECK(duplicate_parameter.error().message.find("duplicates parameter kind") !=
          std::string::npos);

    malformed = vertex;
    malformed.parameters[0].type = malformed.inputs[0].type;
    auto wrong_parameter_type = vng::shader::validate(malformed);
    REQUIRE_FALSE(wrong_parameter_type);
    CHECK(wrong_parameter_type.error().message.find("must have type Mat4") !=
          std::string::npos);
}

TEST_CASE("mixing expressions from different builders records a diagnostic", "[shader][dsl][diagnostic]")
{
    vng::shader::ModuleIR left_module;
    vng::shader::ModuleIR right_module;
    vng::shader::FunctionBuilder left_builder{left_module};
    vng::shader::FunctionBuilder right_builder{right_module};
    auto left = vng::dsl::detail::literal(left_builder, 1.0F);
    auto right = vng::dsl::detail::literal(right_builder, 2.0F);
    auto invalid = left + right;
    REQUIRE(right_builder.failed());
    CHECK(right_builder.diagnostic()->code == vng::shader::DiagnosticCode::mixed_builders);
    auto producing_opcode = [&](const auto& expression) {
        const auto& module = expression.builder() == &left_builder ? left_module : right_module;
        return module.operations[module.values[expression.id().value].producer.value].opcode;
    };
    CHECK(producing_opcode(invalid) == vng::shader::OpCode::poison);

    auto invalid_intrinsic = vng::dsl::mix(left, right, 0.5F);
    CHECK(producing_opcode(invalid_intrinsic) == vng::shader::OpCode::poison);

    auto invalid_construct = vng::dsl::vec2(left, right);
    CHECK(producing_opcode(invalid_construct) == vng::shader::OpCode::poison);

    auto left_vector = vng::dsl::detail::literal(
        left_builder,
        vng::Vec4{0.0F, 0.0F, 0.0F, 1.0F});
    auto right_vector = vng::dsl::detail::literal(
        right_builder,
        vng::Vec4{1.0F, 1.0F, 1.0F, 1.0F});
    auto invalid_record = vng::dsl::make<VertexOut>(
        vng::dsl::field(vng::shader::ClipPosition{}, left_vector),
        vng::dsl::field(Tint{}, right_vector));
    CHECK(producing_opcode(invalid_record) == vng::shader::OpCode::poison);
}

TEST_CASE("ValueId ownership defeats stale expression pointer ABA", "[shader][dsl][regression]")
{
    using Builder = vng::shader::FunctionBuilder;
    alignas(Builder) std::array<std::byte, sizeof(Builder)> storage{};
    auto* const address = reinterpret_cast<Builder*>(storage.data());

    vng::shader::ModuleIR old_module;
    auto* old_builder = std::construct_at(address, old_module);
    const auto stale = vng::dsl::detail::literal(*old_builder, 1.0F);
    std::destroy_at(old_builder);

    vng::shader::ModuleIR later_module;
    auto* later_builder = std::construct_at(address, later_module);
    const auto current = vng::dsl::detail::literal(*later_builder, 2.0F);

    REQUIRE(stale.builder() == current.builder());
    REQUIRE(stale.id().value == current.id().value);
    CHECK(stale.id().owner != current.id().owner);
    CHECK_FALSE(later_builder->owns(stale.id()));

    const auto direct_use = -stale;
    REQUIRE(later_module.owns(direct_use.id()));
    CHECK(later_module.operations[later_module.values[direct_use.id().value].producer.value].opcode ==
          vng::shader::OpCode::poison);

    const auto mixed = stale + current;
    REQUIRE(later_builder->failed());
    CHECK(later_builder->diagnostic()->code == vng::shader::DiagnosticCode::mixed_builders);
    REQUIRE(later_module.owns(mixed.id()));
    CHECK(later_module.operations[later_module.values[mixed.id().value].producer.value].opcode ==
          vng::shader::OpCode::poison);

    std::destroy_at(later_builder);
}

TEST_CASE("a later builder safely diagnoses stale operands in either order", "[shader][dsl][regression]")
{
    using Builder = vng::shader::FunctionBuilder;
    alignas(Builder) std::array<std::byte, sizeof(Builder)> old_storage{};
    alignas(Builder) std::array<std::byte, sizeof(Builder)> current_storage{};

    vng::shader::ModuleIR old_module;
    auto* old_builder = std::construct_at(
        reinterpret_cast<Builder*>(old_storage.data()),
        old_module);
    const auto stale = vng::dsl::detail::literal(*old_builder, 1.0F);
    std::destroy_at(old_builder);

    vng::shader::ModuleIR current_module;
    auto* current_builder = std::construct_at(
        reinterpret_cast<Builder*>(current_storage.data()),
        current_module);
    const auto current = vng::dsl::detail::literal(*current_builder, 2.0F);
    REQUIRE(stale.builder() != current.builder());

    const auto stale_first = stale + current;
    const auto stale_second = current + stale;
    REQUIRE(current_builder->failed());
    CHECK(current_builder->diagnostic()->code == vng::shader::DiagnosticCode::mixed_builders);
    CHECK(current_module.operations[current_module.values[stale_first.id().value].producer.value].opcode ==
          vng::shader::OpCode::poison);
    CHECK(current_module.operations[current_module.values[stale_second.id().value].producer.value].opcode ==
          vng::shader::OpCode::poison);

    std::destroy_at(current_builder);
}

template<vng::shader::Value T>
void retain(vng::shader::FunctionBuilder& builder, vng::dsl::Expr<T> expression)
{
    const std::array operands{expression.id()};
    builder.statement(
        vng::shader::OpCode::stage_output,
        operands,
        vng::shader::InterfacePayload{0},
        vng::shader::Effect::storage_write);
}

TEST_CASE("integer constant folding avoids signed overflow and wraps unsigned values", "[shader][ir][optimizer]")
{
    auto resulting_opcode = []<class MakeExpression>(MakeExpression make_expression) {
        vng::shader::ModuleIR module;
        vng::shader::FunctionBuilder builder{module};
        auto expression = make_expression(builder);
        retain(builder, expression);
        vng::shader::optimize(module);
        const auto output = std::find_if(
            module.operations.begin(),
            module.operations.end(),
            [](const auto& operation) { return operation.opcode == vng::shader::OpCode::stage_output; });
        REQUIRE(output != module.operations.end());
        const auto result = output->operands.front();
        return module.operations[module.values[result.value].producer.value].opcode;
    };

    const auto maximum = std::numeric_limits<vng::i32>::max();
    const auto minimum = std::numeric_limits<vng::i32>::min();

    REQUIRE(resulting_opcode([&](auto& builder) {
        return vng::dsl::detail::literal(builder, maximum) + vng::i32{1};
    }) == vng::shader::OpCode::add);
    REQUIRE(resulting_opcode([&](auto& builder) {
        return vng::dsl::detail::literal(builder, minimum) - vng::i32{1};
    }) == vng::shader::OpCode::subtract);
    REQUIRE(resulting_opcode([&](auto& builder) {
        return vng::dsl::detail::literal(builder, maximum) * vng::i32{2};
    }) == vng::shader::OpCode::multiply);
    REQUIRE(resulting_opcode([&](auto& builder) {
        return vng::dsl::detail::literal(builder, minimum) / vng::i32{-1};
    }) == vng::shader::OpCode::divide);
    REQUIRE(resulting_opcode([&](auto& builder) {
        return -vng::dsl::detail::literal(builder, minimum);
    }) == vng::shader::OpCode::negate);
    REQUIRE(resulting_opcode([](auto& builder) {
        return vng::dsl::detail::literal(builder, vng::i32{2}) + vng::i32{3};
    }) == vng::shader::OpCode::constant);

    vng::shader::ModuleIR unsigned_module;
    vng::shader::FunctionBuilder unsigned_builder{unsigned_module};
    auto wrapped = vng::dsl::detail::literal(
        unsigned_builder,
        std::numeric_limits<vng::u32>::max()) + vng::u32{1};
    retain(unsigned_builder, wrapped);
    vng::shader::optimize(unsigned_module);
    const auto wrapped_output = std::find_if(
        unsigned_module.operations.begin(),
        unsigned_module.operations.end(),
        [](const auto& operation) { return operation.opcode == vng::shader::OpCode::stage_output; });
    REQUIRE(wrapped_output != unsigned_module.operations.end());
    const auto wrapped_result = wrapped_output->operands.front();
    const auto& wrapped_operation = unsigned_module.operations[
        unsigned_module.values[wrapped_result.value].producer.value];
    REQUIRE(wrapped_operation.opcode == vng::shader::OpCode::constant);
    const auto* payload = std::get_if<vng::shader::ConstantPayload>(&wrapped_operation.payload);
    REQUIRE(payload != nullptr);
    REQUIRE(std::get<vng::u32>(payload->value) == 0);
}

[[nodiscard]] vng::dsl::Expr<VertexOut> dangling_stage_result()
{
    vng::shader::ModuleIR module;
    vng::shader::FunctionBuilder builder{module};
    return vng::dsl::Expr<VertexOut>{
        builder,
        builder.poison(builder.type<VertexOut>())
    };
}

TEST_CASE("a foreign stage result is diagnosed without dereferencing its builder", "[shader][stage][diagnostic]")
{
    const auto foreign = dangling_stage_result();
    int calls = 0;
    auto stage = vng::shader::vertex<VertexIn, VertexOut>([&](auto&) {
        ++calls;
        return foreign;
    });
    REQUIRE(calls == 1);
    REQUIRE(!stage);
    REQUIRE(stage.error().code == vng::shader::DiagnosticCode::mixed_builders);
}

TEST_CASE("an escaped stage result remains rejected across builder lifetimes", "[shader][stage][regression]")
{
    std::optional<vng::dsl::Expr<VertexOut>> escaped;
    bool observed_reused_address = false;
    auto body = [&](auto& stage) -> vng::dsl::Expr<VertexOut> {
        if (escaped) {
            const auto current = stage.constant(0.0F);
            observed_reused_address = escaped->builder() == current.builder();
            return *escaped;
        }

        const auto position = stage.inputs().get(Position{});
        auto result = vng::dsl::make<VertexOut>(
            vng::dsl::field(
                vng::shader::ClipPosition{},
                vng::dsl::vec4(position, 0.0F, 1.0F)),
            vng::dsl::field(Tint{}, stage.inputs().get(Tint{})));
        escaped.emplace(result);
        return result;
    };

    auto first = vng::shader::vertex<VertexIn, VertexOut>(body);
    REQUIRE(first);
    auto second = vng::shader::vertex<VertexIn, VertexOut>(body);
    INFO("builder address reused: " << observed_reused_address);
    REQUIRE_FALSE(second);
    CHECK(second.error().code == vng::shader::DiagnosticCode::mixed_builders);
}

TEST_CASE("IR validation rejects malformed ownership, order, payloads, and operations", "[shader][ir][validator]")
{
    int calls = 0;
    auto stage = make_vertex(calls);
    REQUIRE(stage);
    REQUIRE(vng::shader::validate(stage->ir()));

    auto invalid_interface = stage->ir();
    invalid_interface.outputs[0].type = {};
    auto interface_result = vng::shader::validate(invalid_interface);
    REQUIRE(!interface_result);
    REQUIRE(interface_result.error().code == vng::shader::DiagnosticCode::invalid_ir);

    auto invalid_arity = stage->ir();
    const auto add = std::find_if(
        invalid_arity.operations.begin(),
        invalid_arity.operations.end(),
        [](const auto& operation) { return operation.opcode == vng::shader::OpCode::add; });
    REQUIRE(add != invalid_arity.operations.end());
    add->operands.pop_back();
    auto arity_result = vng::shader::validate(invalid_arity);
    REQUIRE(!arity_result);
    REQUIRE(arity_result.error().message.find("expected 2 operands") != std::string::npos);

    auto invalid_effect = stage->ir();
    const auto input = std::find_if(
        invalid_effect.operations.begin(),
        invalid_effect.operations.end(),
        [](const auto& operation) { return operation.opcode == vng::shader::OpCode::input; });
    REQUIRE(input != invalid_effect.operations.end());
    input->effect = vng::shader::Effect::pure;
    auto effect_result = vng::shader::validate(invalid_effect);
    REQUIRE(!effect_result);
    REQUIRE(effect_result.error().message.find("effect classification") != std::string::npos);

    auto foreign_value = stage->ir();
    const auto foreign_operand_operation = std::find_if(
        foreign_value.operations.begin(),
        foreign_value.operations.end(),
        [](const auto& operation) { return !operation.operands.empty(); });
    REQUIRE(foreign_operand_operation != foreign_value.operations.end());
    ++foreign_operand_operation->operands[0].owner;
    auto foreign_value_result = vng::shader::validate(foreign_value);
    REQUIRE(!foreign_value_result);
    CHECK(foreign_value_result.error().message.find("invalid operand") != std::string::npos);

    auto omitted_operation = stage->ir();
    omitted_operation.regions[omitted_operation.root_region.value].operations.erase(
        omitted_operation.regions[omitted_operation.root_region.value].operations.begin());
    auto omission_result = vng::shader::validate(omitted_operation);
    REQUIRE(!omission_result);
    REQUIRE(omission_result.error().message.find("exactly one region") != std::string::npos);

    auto duplicated_operation = stage->ir();
    auto& duplicated_root = duplicated_operation.regions[duplicated_operation.root_region.value].operations;
    duplicated_root.insert(duplicated_root.begin(), duplicated_root.front());
    auto duplication_result = vng::shader::validate(duplicated_operation);
    REQUIRE(!duplication_result);
    REQUIRE(duplication_result.error().message.find("exactly one region") != std::string::npos);

    auto root_argument = stage->ir();
    root_argument.regions[root_argument.root_region.value].arguments.push_back(root_argument.value_id(0));
    auto root_argument_result = vng::shader::validate(root_argument);
    REQUIRE(!root_argument_result);
    CHECK(root_argument_result.error().message.find("root region") != std::string::npos);

    auto hidden_region_argument = stage->ir();
    auto& hidden_root = hidden_region_argument.regions[hidden_region_argument.root_region.value].operations;
    const auto hidden_add = std::find_if(
        hidden_root.begin(),
        hidden_root.end(),
        [&](vng::shader::OperationId id) {
            return hidden_region_argument.operations[id.value].opcode == vng::shader::OpCode::add;
        });
    REQUIRE(hidden_add != hidden_root.end());
    const auto hidden_add_id = *hidden_add;
    const auto hidden_type = hidden_region_argument.values[
        hidden_region_argument.operations[hidden_add_id.value].operands[0].value].type;
    const auto argument_id = hidden_region_argument.value_id(
        static_cast<vng::u32>(hidden_region_argument.values.size()));
    hidden_region_argument.values.push_back(vng::shader::ValueNode{
        .type = hidden_type,
        .producer = {},
    });
    const auto child_region_id = vng::shader::RegionId{
        static_cast<vng::u32>(hidden_region_argument.regions.size())};
    hidden_region_argument.regions.push_back(vng::shader::Region{
        .arguments = {argument_id},
        .operations = {},
    });
    const auto structural_id = vng::shader::OperationId{
        static_cast<vng::u32>(hidden_region_argument.operations.size())};
    hidden_region_argument.operations.push_back(vng::shader::Operation{
        .opcode = vng::shader::OpCode::if_region,
        .operands = {},
        .result = {},
        .regions = {child_region_id},
        .payload = {},
        .effect = vng::shader::Effect::pure,
        .origin = {},
    });
    auto& updated_hidden_root =
        hidden_region_argument.regions[hidden_region_argument.root_region.value].operations;
    updated_hidden_root.insert(updated_hidden_root.end() - 1, structural_id);
    hidden_region_argument.operations[hidden_add_id.value].operands[0] = argument_id;
    auto hidden_argument_result = vng::shader::validate(hidden_region_argument);
    REQUIRE(!hidden_argument_result);
    CHECK(hidden_argument_result.error().message.find("not produced earlier") != std::string::npos);

    auto use_before_definition = stage->ir();
    auto& reordered_root = use_before_definition.regions[use_before_definition.root_region.value].operations;
    const auto dependent = std::find_if(
        reordered_root.begin(),
        reordered_root.end(),
        [&](vng::shader::OperationId id) {
            return use_before_definition.operations[id.value].opcode == vng::shader::OpCode::add;
        });
    REQUIRE(dependent != reordered_root.end());
    const auto dependent_id = *dependent;
    reordered_root.erase(dependent);
    reordered_root.insert(reordered_root.begin(), dependent_id);
    auto ordering_result = vng::shader::validate(use_before_definition);
    REQUIRE(!ordering_result);
    REQUIRE(ordering_result.error().message.find("not produced earlier") != std::string::npos);

    auto unexpected_region = stage->ir();
    unexpected_region.regions.push_back({});
    const auto ordinary = std::find_if(
        unexpected_region.operations.begin(),
        unexpected_region.operations.end(),
        [](const auto& operation) { return operation.opcode == vng::shader::OpCode::add; });
    REQUIRE(ordinary != unexpected_region.operations.end());
    ordinary->regions.push_back(vng::shader::RegionId{
        static_cast<vng::u32>(unexpected_region.regions.size() - 1)});
    auto nested_result = vng::shader::validate(unexpected_region);
    REQUIRE(!nested_result);
    REQUIRE(nested_result.error().message.find("only if and loop") != std::string::npos);

    auto wrong_payload = stage->ir();
    const auto payload_operation = std::find_if(
        wrong_payload.operations.begin(),
        wrong_payload.operations.end(),
        [](const auto& operation) { return operation.opcode == vng::shader::OpCode::add; });
    REQUIRE(payload_operation != wrong_payload.operations.end());
    payload_operation->payload = vng::shader::InterfacePayload{0};
    auto payload_result = vng::shader::validate(wrong_payload);
    REQUIRE(!payload_result);
    REQUIRE(payload_result.error().message.find("payload variant") != std::string::npos);
}

[[nodiscard]] vng::shader::ModuleIR make_matrix_intrinsic_module(vng::shader::OpCode opcode)
{
    vng::shader::ModuleIR module;
    vng::shader::FunctionBuilder builder{module};
    const auto matrix = vng::dsl::detail::literal(builder, vng::Mat4::identity());
    const std::array all_operands{matrix.id(), matrix.id(), matrix.id()};
    const std::size_t operand_count =
        opcode == vng::shader::OpCode::minimum || opcode == vng::shader::OpCode::maximum ? 2U : 3U;
    const auto result = builder.operation(
        opcode,
        builder.type<vng::Mat4>(),
        std::span<const vng::shader::ValueId>{all_operands}.first(operand_count));

    module.outputs.push_back(vng::shader::InterfaceField{
        .semantic_type = typeid(Tint),
        .value_type = typeid(vng::Mat4),
        .semantic_name = "matrix",
        .type = builder.type<vng::Mat4>(),
        .interpolation = vng::shader::Interpolation::none,
        .builtin = vng::shader::Builtin::none,
        .builtin_index = 0,
        .location = 0,
    });
    const std::array output_operands{result};
    builder.statement(
        vng::shader::OpCode::stage_output,
        output_operands,
        vng::shader::InterfacePayload{0},
        vng::shader::Effect::storage_write);
    builder.statement(
        vng::shader::OpCode::return_,
        {},
        {},
        vng::shader::Effect::termination);
    return module;
}

TEST_CASE("GLSL scalar and vector intrinsics reject matrix operands", "[shader][ir][validator]")
{
    for (const auto opcode : {
             vng::shader::OpCode::minimum,
             vng::shader::OpCode::maximum,
             vng::shader::OpCode::clamp,
             vng::shader::OpCode::mix,
         }) {
        auto module = make_matrix_intrinsic_module(opcode);
        const auto result = vng::shader::validate(module);
        REQUIRE(!result);
        REQUIRE(result.error().code == vng::shader::DiagnosticCode::invalid_ir);
    }
}

TEST_CASE("stage factories reject illegal interfaces but permit boolean builtins", "[shader][stage][interface]")
{
    using InterpolatedVertexIn = vng::shader::VertexInputs<vng::shader::smooth<Position>>;
    int interpolated_calls = 0;
    auto interpolated = vng::shader::vertex<InterpolatedVertexIn, VertexOut>([&](auto& stage) {
        ++interpolated_calls;
        const auto position = stage.inputs().get(Position{});
        return vng::dsl::make<VertexOut>(
            vng::dsl::field(
                vng::shader::ClipPosition{},
                vng::dsl::vec4(position, 0.0F, 1.0F)),
            vng::dsl::field(Tint{}, stage.constant(vng::Vec4{1.0F, 1.0F, 1.0F, 1.0F})));
    });
    REQUIRE(!interpolated);
    REQUIRE(interpolated_calls == 0);
    REQUIRE(interpolated.error().code == vng::shader::DiagnosticCode::interface_mismatch);

    using MatrixVertexIn = vng::shader::VertexInputs<MatrixInput>;
    int matrix_vertex_calls = 0;
    auto matrix_vertex = vng::shader::vertex<MatrixVertexIn, VertexOut>([&](auto& stage) {
        ++matrix_vertex_calls;
        return vng::dsl::make<VertexOut>(
            vng::dsl::field(
                vng::shader::ClipPosition{},
                stage.constant(vng::Vec4{0.0F, 0.0F, 0.0F, 1.0F})),
            vng::dsl::field(Tint{}, stage.constant(vng::Vec4{1.0F, 1.0F, 1.0F, 1.0F})));
    });
    REQUIRE(!matrix_vertex);
    REQUIRE(matrix_vertex_calls == 0);
    REQUIRE(matrix_vertex.error().code == vng::shader::DiagnosticCode::unsupported_operation);

    using BoolVertexIn = vng::shader::VertexInputs<UserFlag>;
    int boolean_vertex_calls = 0;
    auto boolean_vertex = vng::shader::vertex<BoolVertexIn, VertexOut>([&](auto& stage) {
        ++boolean_vertex_calls;
        return vng::dsl::make<VertexOut>(
            vng::dsl::field(
                vng::shader::ClipPosition{},
                stage.constant(vng::Vec4{0.0F, 0.0F, 0.0F, 1.0F})),
            vng::dsl::field(Tint{}, stage.constant(vng::Vec4{1.0F, 1.0F, 1.0F, 1.0F})));
    });
    REQUIRE(!boolean_vertex);
    REQUIRE(boolean_vertex_calls == 0);
    REQUIRE(boolean_vertex.error().code == vng::shader::DiagnosticCode::unsupported_operation);

    using BoolFragmentIn = vng::shader::FragmentInputs<vng::shader::flat<UserMask>>;
    int boolean_fragment_calls = 0;
    auto boolean_fragment = vng::shader::fragment<BoolFragmentIn, FragmentOut>([&](auto& stage) {
        ++boolean_fragment_calls;
        return vng::dsl::make<FragmentOut>(
            vng::dsl::field(
                vng::shader::Color<0>{},
                stage.constant(vng::Vec4{1.0F, 1.0F, 1.0F, 1.0F})));
    });
    REQUIRE(!boolean_fragment);
    REQUIRE(boolean_fragment_calls == 0);
    REQUIRE(boolean_fragment.error().code == vng::shader::DiagnosticCode::unsupported_operation);

    using EmptyFragmentIn = vng::shader::FragmentInputs<>;
    auto front_facing = vng::shader::fragment<EmptyFragmentIn, FragmentOut>([](auto& stage) {
        const auto color = vng::dsl::select(
            stage.front_facing(),
            vng::Vec4{1.0F, 1.0F, 1.0F, 1.0F},
            vng::Vec4{0.0F, 0.0F, 0.0F, 1.0F});
        return vng::dsl::make<FragmentOut>(
            vng::dsl::field(vng::shader::Color<0>{}, color));
    });
    REQUIRE(front_facing);
    const auto builtin = std::find_if(
        front_facing->ir().inputs.begin(),
        front_facing->ir().inputs.end(),
        [](const auto& input) { return input.builtin == vng::shader::Builtin::front_facing; });
    REQUIRE(builtin != front_facing->ir().inputs.end());
    REQUIRE(builtin->value_type == typeid(bool));
}

TEST_CASE("relinking a copied stage recomputes every interface location", "[shader][link][regression]")
{
    using RelinkVertexOut = vng::shader::VertexOutputs<
        vng::shader::ClipPosition,
        vng::shader::smooth<Tint>,
        vng::shader::smooth<SecondaryTint>>;
    using BothFragmentIn = vng::shader::FragmentInputs<
        vng::shader::smooth<Tint>,
        vng::shader::smooth<SecondaryTint>>;
    using SecondaryFragmentIn = vng::shader::FragmentInputs<vng::shader::smooth<SecondaryTint>>;

    auto vertex = vng::shader::vertex<VertexIn, RelinkVertexOut>([](auto& stage) {
        const auto position = stage.inputs().get(Position{});
        const auto tint = stage.inputs().get(Tint{});
        return vng::dsl::make<RelinkVertexOut>(
            vng::dsl::field(
                vng::shader::ClipPosition{},
                vng::dsl::vec4(position, 0.0F, 1.0F)),
            vng::dsl::field(Tint{}, tint),
            vng::dsl::field(SecondaryTint{}, tint));
    });
    auto both_fragment = vng::shader::fragment<BothFragmentIn, FragmentOut>([](auto& stage) {
        const auto color = stage.inputs().get(Tint{}) + stage.inputs().get(SecondaryTint{});
        return vng::dsl::make<FragmentOut>(
            vng::dsl::field(vng::shader::Color<0>{}, color));
    });
    REQUIRE(vertex);
    REQUIRE(both_fragment);

    auto first_link = vng::shader::link(std::move(*vertex), std::move(*both_fragment));
    REQUIRE(first_link);
    auto reused_vertex = first_link->vertex();

    auto secondary_fragment = vng::shader::fragment<SecondaryFragmentIn, FragmentOut>([](auto& stage) {
        return vng::dsl::make<FragmentOut>(
            vng::dsl::field(vng::shader::Color<0>{}, stage.inputs().get(SecondaryTint{})));
    });
    REQUIRE(secondary_fragment);
    auto second_link = vng::shader::link(std::move(reused_vertex), std::move(*secondary_fragment));
    REQUIRE(second_link);

    const auto& vertex_outputs = second_link->vertex().ir().outputs;
    const auto tint = std::find_if(vertex_outputs.begin(), vertex_outputs.end(), [](const auto& output) {
        return output.semantic_type == typeid(Tint);
    });
    const auto secondary = std::find_if(vertex_outputs.begin(), vertex_outputs.end(), [](const auto& output) {
        return output.semantic_type == typeid(SecondaryTint);
    });
    REQUIRE(tint != vertex_outputs.end());
    REQUIRE(secondary != vertex_outputs.end());
    REQUIRE(secondary->location == 0);
    REQUIRE(tint->location == 1);
    REQUIRE(second_link->fragment().ir().inputs[0].location == 0);
}

TEST_CASE("linking diagnoses missing and interpolation-mismatched varyings", "[shader][link][diagnostic]")
{
    using MissingInput = vng::shader::FragmentInputs<vng::shader::smooth<MissingVarying>>;
    int vertex_calls = 0;
    auto vertex = make_vertex(vertex_calls);
    auto fragment = vng::shader::fragment<MissingInput, FragmentOut>([](auto& stage) {
        auto missing = stage.inputs().get(MissingVarying{});
        return vng::dsl::make<FragmentOut>(
            vng::dsl::field(vng::shader::Color<0>{}, vng::dsl::vec4(missing, 1.0F)));
    });
    REQUIRE(vertex);
    REQUIRE(fragment);
    auto missing = vng::shader::link(std::move(*vertex), std::move(*fragment));
    REQUIRE(!missing);
    REQUIRE(missing.error().code == vng::shader::DiagnosticCode::interface_mismatch);

    using FlatVertexOut = vng::shader::VertexOutputs<
        vng::shader::ClipPosition,
        vng::shader::flat<Tint>>;
    auto flat_vertex = vng::shader::vertex<VertexIn, FlatVertexOut>([](auto& stage) {
        auto p = stage.inputs().get(Position{});
        return vng::dsl::make<FlatVertexOut>(
            vng::dsl::field(vng::shader::ClipPosition{}, vng::dsl::vec4(p, 0.0F, 1.0F)),
            vng::dsl::field(Tint{}, stage.inputs().get(Tint{})));
    });
    int fragment_calls = 0;
    auto smooth_fragment = make_fragment(fragment_calls);
    REQUIRE(flat_vertex);
    REQUIRE(smooth_fragment);
    auto interpolation = vng::shader::link(std::move(*flat_vertex), std::move(*smooth_fragment));
    REQUIRE(!interpolation);
    REQUIRE(interpolation.error().message.find("interpolation") != std::string::npos);
}

} // namespace
