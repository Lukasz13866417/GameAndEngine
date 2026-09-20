#include <vng/opengl/opengl.hpp>
#include <vng/glsl/emitter.hpp>
#include <vng/render/frame.hpp>
#include <vng/render/program.hpp>
#include <vng/shader/shader.hpp>
#include "../support/glfw_opengl.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>

namespace {

struct Position : vng::gfx::Semantic<vng::Vec2> {};

using VertexInputs = vng::shader::VertexInputs<Position>;
using VertexOutputs = vng::shader::VertexOutputs<
    vng::shader::ClipPosition>;
using FragmentInputs = vng::shader::FragmentInputs<>;
using FragmentOutputs = vng::shader::FragmentOutputs<
    vng::shader::Color<0>>;
using SparseFragmentOutputs = vng::shader::FragmentOutputs<
    vng::shader::Color<2>,
    vng::shader::Color<7>>;

[[noreturn]] void skip_ctest(const std::string& reason)
{
    std::cerr << "OpenGL test skipped: " << reason << '\n';
    std::exit(77);
}

[[nodiscard]] std::string describe(
    const vng::opengl::Diagnostic& diagnostic)
{
    std::string result = diagnostic.message;
    if (!diagnostic.driver_log.empty()) {
        result += "\ndriver log:\n" + diagnostic.driver_log;
    }
    if (!diagnostic.generated_source.empty()) {
        result += "\ngenerated source:\n" + diagnostic.generated_source;
    }
    return result;
}

[[nodiscard]] std::expected<vng::opengl::Program, vng::opengl::Diagnostic>
make_program(const vng::opengl::Device& device)
{
    vng::opengl::ShaderSource vertex_source;
    vertex_source.stage = vng::opengl::ShaderStage::vertex;
    vertex_source.text = R"glsl(#version 460 core
void main()
{
    const vec2 positions[3] = vec2[3](
        vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(0.0, 0.5));
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
}
)glsl";
    auto vertex = vng::opengl::Shader::compile(
        device, std::move(vertex_source));
    if (!vertex) {
        return std::unexpected(std::move(vertex.error()));
    }

    vng::opengl::ShaderSource fragment_source;
    fragment_source.stage = vng::opengl::ShaderStage::fragment;
    fragment_source.text = R"glsl(#version 460 core
layout(location = 0) out vec4 color;
void main()
{
    color = vec4(1.0);
}
)glsl";
    auto fragment = vng::opengl::Shader::compile(
        device, std::move(fragment_source));
    if (!fragment) {
        return std::unexpected(std::move(fragment.error()));
    }

    return vng::opengl::Program::link_graphics(
        device, *vertex, *fragment);
}

[[nodiscard]] vng::shader::Result<vng::shader::GraphicsProgram>
make_neutral_program()
{
    auto vertex = vng::shader::vertex<VertexInputs, VertexOutputs>(
        "graphics_program_vertex",
        [](auto& stage) {
            return stage.output(
                vng::dsl::field<vng::shader::ClipPosition>(
                    vng::dsl::vec4(
                        stage.input(Position{}), 0.0F, 1.0F)));
        });
    if (!vertex) {
        return std::unexpected(std::move(vertex.error()));
    }
    auto fragment = vng::shader::fragment<FragmentInputs, FragmentOutputs>(
        "graphics_program_fragment",
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

[[nodiscard]] vng::shader::Result<vng::shader::GraphicsProgram>
make_sparse_neutral_program()
{
    auto vertex = vng::shader::vertex<VertexInputs, VertexOutputs>(
        "graphics_program_sparse_vertex",
        [](auto& stage) {
            return stage.output(
                vng::dsl::field<vng::shader::ClipPosition>(
                    vng::dsl::vec4(
                        stage.input(Position{}), 0.0F, 1.0F)));
        });
    if (!vertex) {
        return std::unexpected(std::move(vertex.error()));
    }
    auto fragment = vng::shader::fragment<
        FragmentInputs,
        SparseFragmentOutputs>(
        "graphics_program_sparse_fragment",
        [](auto& stage) {
            return stage.output(
                vng::dsl::field<vng::shader::Color<2>>(
                    stage.constant(vng::Vec4{
                        1.0F, 0.0F, 0.0F, 1.0F})),
                vng::dsl::field<vng::shader::Color<7>>(
                    stage.constant(vng::Vec4{
                        0.0F, 1.0F, 0.0F, 1.0F})));
        });
    if (!fragment) {
        return std::unexpected(std::move(fragment.error()));
    }
    return vng::shader::link(std::move(*vertex), std::move(*fragment));
}

} // namespace

static_assert(std::is_move_constructible_v<vng::opengl::Program>);
static_assert(std::is_move_assignable_v<vng::opengl::Program>);
static_assert(!std::is_copy_constructible_v<vng::opengl::Program>);
static_assert(!std::is_copy_assignable_v<vng::opengl::Program>);

TEST_CASE("OpenGL emitted program compilation retains interface and diagnostic evidence",
          "[opengl][integration][graphics-program]")
{
    auto window = vng::test::create_hidden_opengl_window(8, 8, "emitted program");
    if (!window) skip_ctest(window.error().message);
    auto access = window->make_current();
    REQUIRE(access);
    auto device = vng::opengl::Device::create(*access);
    REQUIRE(device);
    auto neutral = make_sparse_neutral_program();
    REQUIRE(neutral);
    auto emitted = vng::glsl::emit(*neutral);
    REQUIRE(emitted);
    auto compiled = vng::render::compile_program(*device, *emitted);
    INFO((compiled ? std::string{} : describe(compiled.error())));
    REQUIRE(compiled);
    REQUIRE(compiled->generated_source());
    CHECK(compiled->generated_source()->vertex.source == emitted->vertex.source);
    CHECK(compiled->generated_source()->fragment.source == emitted->fragment.source);
    REQUIRE(compiled->generated_source()->fragment.interface.outputs.size() == 2);
    CHECK(compiled->generated_source()->fragment.interface.outputs[0].location == 2);
    CHECK(compiled->generated_source()->fragment.interface.outputs[1].location == 7);

    auto malformed = *emitted;
    malformed.fragment.source += "\nthis is deliberately invalid GLSL;\n";
    auto failed = vng::render::compile_program(*device, malformed);
    REQUIRE_FALSE(failed);
    CHECK_FALSE(failed.error().driver_log.empty());
    CHECK(failed.error().generated_source == malformed.fragment.source);
    CHECK_FALSE(failed.error().source_map.empty());
}


TEST_CASE("OpenGL compiled program retains metadata independently of live draw state",
          "[opengl][integration][graphics-program]")
{
    auto window = vng::test::create_hidden_opengl_window(
        8, 8, "vng hidden graphics program test");
    if (!window) {
        skip_ctest("OpenGL context unavailable: " + window.error().message);
    }
    auto access = window->make_current();
    if (!access) {
        skip_ctest(
            "OpenGL context could not be made current: "
            + access.error().message);
    }
    auto device = vng::opengl::Device::create(*access);
    INFO((device ? std::string{} : describe(device.error())));
    REQUIRE(device.has_value());

    auto program = make_neutral_program();
    REQUIRE(program.has_value());

    const vng::opengl::GraphicsStateSnapshot description{
        .depth = {
            .test = true,
            .write = false,
            .compare = vng::render::DepthCompare::greater_equal,
        },
        .cull = vng::render::CullMode::front,
        .front_face = vng::render::FrontFace::clockwise,
    };
    auto compiled = vng::render::compile_program(*device, *program);
    INFO((compiled ? std::string{} : describe(compiled.error())));
    REQUIRE(compiled.has_value());
    CHECK(compiled->native_handle() != 0);
    CHECK(compiled->belongs_to(*device));
    REQUIRE(compiled->generated_source() != nullptr);
    CHECK_FALSE(compiled->generated_source()->vertex.source.empty());
    CHECK_FALSE(compiled->generated_source()->fragment.source.empty());
    const auto program_handle = compiled->native_handle();

    // Draw state belongs to the frame, never the compiled program. Poison
    // native state before the first frame-state synchronization.
    REQUIRE(device->set_depth_state({
        .test_enabled = false,
        .write_enabled = true,
        .compare = vng::opengl::DepthCompare::less,
    }).has_value());
    REQUIRE(device->set_blend_enabled(0, true).has_value());
    REQUIRE(device->set_color_write_mask(
        0, {false, false, false, false}).has_value());
    REQUIRE(device->set_scissor_enabled(true).has_value());
    REQUIRE(device->set_cull_state({
        .mode = vng::opengl::CullMode::none,
        .front_face = vng::opengl::FrontFaceWinding::counter_clockwise,
    }).has_value());
    REQUIRE(device->set_rasterizer_discard_enabled(true).has_value());
    REQUIRE(device->set_framebuffer_srgb_enabled(false).has_value());

    using PolygonMode = void (*)(std::uint32_t, std::uint32_t);
    using GetInteger = void (*)(std::uint32_t, std::int32_t*);
    using GetBoolean = void (*)(std::uint32_t, std::uint8_t*);
    using GetBooleanIndexed = void (*)(
        std::uint32_t, std::uint32_t, std::uint8_t*);
    using IsEnabled = std::uint8_t (*)(std::uint32_t);
    using IsEnabledIndexed = std::uint8_t (*)(std::uint32_t, std::uint32_t);

    const auto polygon_mode = reinterpret_cast<PolygonMode>(
        access->resolve("glPolygonMode"));
    const auto get_integer = reinterpret_cast<GetInteger>(
        access->resolve("glGetIntegerv"));
    const auto get_boolean = reinterpret_cast<GetBoolean>(
        access->resolve("glGetBooleanv"));
    const auto get_boolean_indexed = reinterpret_cast<GetBooleanIndexed>(
        access->resolve("glGetBooleani_v"));
    const auto is_enabled = reinterpret_cast<IsEnabled>(
        access->resolve("glIsEnabled"));
    const auto is_enabled_indexed = reinterpret_cast<IsEnabledIndexed>(
        access->resolve("glIsEnabledi"));
    REQUIRE(polygon_mode != nullptr);
    REQUIRE(get_integer != nullptr);
    REQUIRE(get_boolean != nullptr);
    REQUIRE(get_boolean_indexed != nullptr);
    REQUIRE(is_enabled != nullptr);
    REQUIRE(is_enabled_indexed != nullptr);

    constexpr std::uint32_t gl_front_and_back = 0x0408;
    constexpr std::uint32_t gl_line = 0x1B01;
    polygon_mode(gl_front_and_back, gl_line);

    auto frame = vng::render::begin_frame(*device, vng::render::FrameDesc{
        .extent = {8, 8},
        .color_encoding = vng::render::ColorEncoding::linear,
        .clear_color = std::nullopt, .clear_depth = std::nullopt,
    });
    REQUIRE(frame);
    auto commands = frame->commands();
    auto graphics = commands.graphics_state();
    REQUIRE(graphics.set(description));
    auto bound = commands.bind(*compiled);
    INFO((bound ? std::string{} : describe(bound.error())));
    REQUIRE(bound.has_value());

    constexpr std::uint32_t gl_current_program = 0x8B8D;
    constexpr std::uint32_t gl_depth_test = 0x0B71;
    constexpr std::uint32_t gl_depth_writemask = 0x0B72;
    constexpr std::uint32_t gl_depth_func = 0x0B74;
    constexpr std::uint32_t gl_gequal = 0x0206;
    constexpr std::uint32_t gl_blend = 0x0BE2;
    constexpr std::uint32_t gl_color_writemask = 0x0C23;
    constexpr std::uint32_t gl_scissor_test = 0x0C11;
    constexpr std::uint32_t gl_cull_face = 0x0B44;
    constexpr std::uint32_t gl_cull_face_mode = 0x0B45;
    constexpr std::uint32_t gl_front = 0x0404;
    constexpr std::uint32_t gl_front_face = 0x0B46;
    constexpr std::uint32_t gl_cw = 0x0900;
    constexpr std::uint32_t gl_rasterizer_discard = 0x8C89;
    constexpr std::uint32_t gl_framebuffer_srgb = 0x8DB9;
    constexpr std::uint32_t gl_polygon_mode = 0x0B40;
    constexpr std::uint32_t gl_fill = 0x1B02;

    std::int32_t scalar = 0;
    get_integer(gl_current_program, &scalar);
    CHECK(static_cast<std::uint32_t>(scalar) == program_handle);
    CHECK(is_enabled(gl_depth_test) != 0);
    std::uint8_t boolean = 1;
    get_boolean(gl_depth_writemask, &boolean);
    CHECK(boolean == 0);
    get_integer(gl_depth_func, &scalar);
    CHECK(static_cast<std::uint32_t>(scalar) == gl_gequal);
    CHECK(is_enabled_indexed(gl_blend, 0) == 0);
    std::array<std::uint8_t, 4> color_mask{};
    get_boolean_indexed(gl_color_writemask, 0, color_mask.data());
    CHECK(color_mask == std::array<std::uint8_t, 4>{1, 1, 1, 1});
    CHECK(is_enabled(gl_scissor_test) == 0);
    CHECK(is_enabled(gl_cull_face) != 0);
    get_integer(gl_cull_face_mode, &scalar);
    CHECK(static_cast<std::uint32_t>(scalar) == gl_front);
    get_integer(gl_front_face, &scalar);
    CHECK(static_cast<std::uint32_t>(scalar) == gl_cw);
    CHECK(is_enabled(gl_rasterizer_discard) == 0);
    CHECK(is_enabled(gl_framebuffer_srgb) == 0);
    std::array<std::int32_t, 2> polygon_state{};
    get_integer(gl_polygon_mode, polygon_state.data());
    // Core applies one shared front/back mode. Keep room for implementations
    // that return two entries, but only the first is needed for this check.
    CHECK(polygon_state[0] == static_cast<std::int32_t>(gl_fill));

    // Device identity is part of the realization. Even with another valid,
    // current OpenGL context, binding must fail before touching its state.
    {
        auto other_window = vng::test::create_hidden_opengl_window(
            4, 4, "vng second graphics program context");
        REQUIRE(other_window.has_value());
        auto other_access = other_window->make_current();
        REQUIRE(other_access.has_value());
        auto other_device = vng::opengl::Device::create(*other_access);
        INFO((other_device
            ? std::string{}
            : describe(other_device.error())));
        REQUIRE(other_device.has_value());
        CHECK_FALSE(compiled->belongs_to(*other_device));
        auto other_frame = vng::render::begin_frame(
            *other_device, vng::render::FrameDesc{
                .extent = {4, 4},
                .color_encoding = vng::render::ColorEncoding::linear,
                .clear_color = std::nullopt, .clear_depth = std::nullopt,
            });
        REQUIRE(other_frame);
        auto other_commands = other_frame->commands();
        auto wrong_device = other_commands.bind(*compiled);
        REQUIRE_FALSE(wrong_device.has_value());
        CHECK(wrong_device.error().code
              == vng::opengl::ErrorCode::incompatible_device);
    }

    auto restored_after_other = window->make_current();
    REQUIRE(restored_after_other.has_value());

    window->release_current();
    auto without_context = commands.bind(*compiled);
    REQUIRE_FALSE(without_context.has_value());
    CHECK(without_context.error().code
          == vng::opengl::ErrorCode::context_not_current);
    auto restored = window->make_current();
    REQUIRE(restored.has_value());

    // Full compilation does need a current context. Even failures before the
    // driver sees the shader retain the generated source and IR-node map.
    window->release_current();
    auto detached_compilation = vng::render::compile_program(
        *device, *program);
    REQUIRE_FALSE(detached_compilation.has_value());
    CHECK(detached_compilation.error().code
          == vng::opengl::ErrorCode::context_not_current);
    CHECK_FALSE(detached_compilation.error().generated_source.empty());
    CHECK_FALSE(detached_compilation.error().source_map.empty());

    auto restored_again = window->make_current();
    REQUIRE(restored_again.has_value());
}

TEST_CASE("OpenGL frame normalizes sparse output state independently of programs",
          "[opengl][integration][graphics-program][mrt]")
{
    auto window = vng::test::create_hidden_opengl_window(
        8, 8, "vng sparse MRT graphics program test");
    if (!window) {
        skip_ctest("OpenGL context unavailable: " + window.error().message);
    }
    auto access = window->make_current();
    if (!access) {
        skip_ctest(
            "OpenGL context could not be made current: "
            + access.error().message);
    }
    auto device = vng::opengl::Device::create(*access);
    INFO((device ? std::string{} : describe(device.error())));
    REQUIRE(device);

    using GetInteger = void (*)(std::uint32_t, std::int32_t*);
    using GetBooleanIndexed = void (*)(
        std::uint32_t, std::uint32_t, std::uint8_t*);
    using IsEnabledIndexed = std::uint8_t (*)(
        std::uint32_t, std::uint32_t);
    const auto get_integer = reinterpret_cast<GetInteger>(
        access->resolve("glGetIntegerv"));
    const auto get_boolean_indexed = reinterpret_cast<GetBooleanIndexed>(
        access->resolve("glGetBooleani_v"));
    const auto is_enabled_indexed = reinterpret_cast<IsEnabledIndexed>(
        access->resolve("glIsEnabledi"));
    REQUIRE(get_integer != nullptr);
    REQUIRE(get_boolean_indexed != nullptr);
    REQUIRE(is_enabled_indexed != nullptr);

    constexpr std::uint32_t gl_max_draw_buffers = 0x8824;
    constexpr std::uint32_t gl_blend = 0x0BE2;
    constexpr std::uint32_t gl_color_writemask = 0x0C23;
    std::int32_t maximum_draw_buffers = 0;
    get_integer(gl_max_draw_buffers, &maximum_draw_buffers);
    REQUIRE(maximum_draw_buffers >= 8);

    auto neutral = make_sparse_neutral_program();
    REQUIRE(neutral);
    auto compiled = vng::render::compile_program(*device, *neutral);
    INFO((compiled ? std::string{} : describe(compiled.error())));
    REQUIRE(compiled);
    REQUIRE(compiled->generated_source() != nullptr);
    REQUIRE(compiled->generated_source()->fragment.interface.outputs.size()
            == 2);
    CHECK(compiled->generated_source()->fragment.interface.outputs[0].location
          == 2);
    CHECK(compiled->generated_source()->fragment.interface.outputs[1].location
          == 7);
    for (const std::uint32_t location : {2U, 7U}) {
        REQUIRE(device->set_blend_enabled(location, true));
        REQUIRE(device->set_color_write_mask(
            location, {false, false, false, false}));
    }
    constexpr std::uint32_t unrelated_location = 1;
    REQUIRE(device->set_blend_enabled(unrelated_location, true));
    REQUIRE(device->set_color_write_mask(
        unrelated_location, {false, false, false, false}));
    auto frame = vng::render::begin_frame(*device, vng::render::FrameDesc{
        .extent = {8, 8}, .color_encoding = vng::render::ColorEncoding::linear,
        .clear_color = std::nullopt, .clear_depth = std::nullopt,
    });
    REQUIRE(frame);
    auto commands = frame->commands();
    REQUIRE(commands.run(*compiled));

    for (const std::uint32_t location : {2U, 7U}) {
        CHECK(is_enabled_indexed(gl_blend, location) == 0);
        std::array<std::uint8_t, 4> mask{};
        get_boolean_indexed(
            gl_color_writemask, location, mask.data());
        CHECK(mask == std::array<std::uint8_t, 4>{1, 1, 1, 1});
    }
    CHECK(is_enabled_indexed(gl_blend, unrelated_location) == 0);
    std::array<std::uint8_t, 4> unrelated_mask{};
    get_boolean_indexed(
        gl_color_writemask,
        unrelated_location,
        unrelated_mask.data());
    CHECK(unrelated_mask == std::array<std::uint8_t, 4>{1, 1, 1, 1});

    // Switching to a raw backend program does not install a hidden preset.
    auto raw = make_program(*device);
    REQUIRE(raw);
    REQUIRE(raw->generated_source() == nullptr);
    auto graphics = commands.graphics_state();
    REQUIRE(graphics.set(vng::render::BlendMode::additive));
    REQUIRE(commands.run(*raw));
    auto selected = graphics.snapshot();
    REQUIRE(selected);
    CHECK(selected->blend == vng::render::BlendMode::additive);
}
