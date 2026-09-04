#include <vng/gfx/gfx.hpp>
#include <vng/glsl/glsl.hpp>
#include <vng/opengl/glsl_source.hpp>
#include <vng/opengl/opengl.hpp>
#include <vng/shader/shader.hpp>
#include "../support/glfw_opengl.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

struct Position : vng::gfx::Semantic<vng::Vec2> {};
struct Color : vng::gfx::Semantic<vng::Vec4> {};
struct InstanceOffset : vng::gfx::Semantic<vng::Vec2> {};

using Vertex = vng::gfx::Record<Position, vng::gfx::as<Color, vng::gfx::unorm8x4>>;
using Instance = vng::gfx::Record<InstanceOffset>;
using Layout = vng::gfx::VertexLayout<
    vng::gfx::Stream<Vertex>,
    vng::gfx::Stream<Instance, vng::gfx::PerInstance<1>>>;
using Positions = vng::gfx::Record<Position>;
using Colors = vng::gfx::Record<Color>;
using SplitLayout = vng::gfx::VertexLayout<
    vng::gfx::Stream<Colors>,
    vng::gfx::Stream<Positions>,
    vng::gfx::Stream<Instance, vng::gfx::PerInstance<1>>>;

using VertexIn = vng::shader::VertexInputs<Position, Color, InstanceOffset>;
using VertexOut = vng::shader::VertexOutputs<
    vng::shader::ClipPosition,
    vng::shader::smooth<Color>>;
using FragmentIn = vng::shader::FragmentInputs<vng::shader::smooth<Color>>;
using FragmentOut = vng::shader::FragmentOutputs<vng::shader::Color<0>>;

static_assert(Vertex::offset(Color{}) == 8);
static_assert(Vertex::stride == 12);

[[nodiscard]] std::string describe(const vng::opengl::Diagnostic& diagnostic)
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

[[nodiscard]] std::string describe(const vng::shader::Diagnostic& diagnostic)
{
    std::string result = diagnostic.message;
    for (const auto& note : diagnostic.notes) {
        result += "\n  " + note;
    }
    if (!diagnostic.generated_source.empty()) {
        result += "\ngenerated source:\n" + diagnostic.generated_source;
    }
    return result;
}

[[noreturn]] void skip_ctest(const std::string& reason)
{
    std::cerr << "OpenGL test skipped: " << reason << '\n';
    std::exit(77);
}

[[nodiscard]] bool approximately_red(
    const std::vector<std::byte>& pixels,
    std::uint32_t width,
    std::uint32_t x,
    std::uint32_t y)
{
    const auto offset = (static_cast<std::size_t>(y) * width + x) * 4;
    return std::to_integer<unsigned>(pixels[offset]) > 245
        && std::to_integer<unsigned>(pixels[offset + 1]) < 10
        && std::to_integer<unsigned>(pixels[offset + 2]) < 10
        && std::to_integer<unsigned>(pixels[offset + 3]) > 245;
}

[[nodiscard]] bool approximately_black(
    const std::vector<std::byte>& pixels,
    std::uint32_t width,
    std::uint32_t x,
    std::uint32_t y)
{
    const auto offset = (static_cast<std::size_t>(y) * width + x) * 4;
    return std::to_integer<unsigned>(pixels[offset]) < 10
        && std::to_integer<unsigned>(pixels[offset + 1]) < 10
        && std::to_integer<unsigned>(pixels[offset + 2]) < 10;
}

[[nodiscard]] bool equivalent_pixels(
    const std::vector<std::byte>& left,
    const std::vector<std::byte>& right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto a = static_cast<int>(std::to_integer<unsigned>(left[index]));
        const auto b = static_cast<int>(std::to_integer<unsigned>(right[index]));
        if (a - b < -1 || a - b > 1) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("OpenGL rejects malformed erased vertex format descriptors",
          "[opengl][vertex-input]")
{
    auto bad_size = vng::gfx::f32x2::format;
    --bad_size.byte_size;
    auto translated = vng::opengl::translate_vertex_format(bad_size);
    REQUIRE_FALSE(translated.has_value());
    CHECK(translated.error().code == vng::opengl::ErrorCode::invalid_argument);

    auto bad_packed_count = vng::gfx::snorm10x3::format;
    bad_packed_count.storage_component_count = 3;
    translated = vng::opengl::translate_vertex_format(bad_packed_count);
    REQUIRE_FALSE(translated.has_value());
    CHECK(translated.error().code == vng::opengl::ErrorCode::invalid_argument);

    auto bad_float_delivery = vng::gfx::f32x4::format;
    bad_float_delivery.interpretation = vng::gfx::AttributeInterpretation::Integer;
    translated = vng::opengl::translate_vertex_format(bad_float_delivery);
    REQUIRE_FALSE(translated.has_value());
    CHECK(translated.error().code == vng::opengl::ErrorCode::invalid_argument);
}

TEST_CASE("OpenGL clears default framebuffer color and depth together",
          "[opengl][integration][clear]")
{
    auto window = vng::test::create_hidden_opengl_window(
        8, 8, "vng hidden default clear test");
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

    auto invalid_depth = device->clear_default(
        {0.0F, 0.0F, 0.0F, 1.0F},
        std::numeric_limits<float>::quiet_NaN());
    REQUIRE_FALSE(invalid_depth.has_value());
    CHECK(invalid_depth.error().code
          == vng::opengl::ErrorCode::invalid_argument);

    auto cleared = device->clear_default(
        {0.25F, 0.5F, 0.75F, 1.0F}, 0.375F);
    INFO((cleared ? std::string{} : describe(cleared.error())));
    REQUIRE(cleared.has_value());

    using GetInteger = void (*)(std::uint32_t, std::int32_t*);
    using GetDouble = void (*)(std::uint32_t, double*);
    using ReadPixels = void (*)(
        std::int32_t,
        std::int32_t,
        std::int32_t,
        std::int32_t,
        std::uint32_t,
        std::uint32_t,
        void*);
    const auto get_integer = reinterpret_cast<GetInteger>(
        access->resolve("glGetIntegerv"));
    const auto get_double = reinterpret_cast<GetDouble>(
        access->resolve("glGetDoublev"));
    const auto read_pixels = reinterpret_cast<ReadPixels>(
        access->resolve("glReadPixels"));
    REQUIRE(get_integer != nullptr);
    REQUIRE(get_double != nullptr);
    REQUIRE(read_pixels != nullptr);

    constexpr std::uint32_t gl_depth_bits = 0x0D56;
    constexpr std::uint32_t gl_depth_clear_value = 0x0B73;
    constexpr std::uint32_t gl_rgba = 0x1908;
    constexpr std::uint32_t gl_depth_component = 0x1902;
    constexpr std::uint32_t gl_unsigned_byte = 0x1401;
    constexpr std::uint32_t gl_float = 0x1406;

    std::int32_t depth_bits = 0;
    double depth_clear_value = 0.0;
    get_integer(gl_depth_bits, &depth_bits);
    get_double(gl_depth_clear_value, &depth_clear_value);
    // Device clears use glClearBuffer*, so the requested attachment value does
    // not leak into OpenGL's ambient clear-value state (whose default is 1).
    CHECK(std::abs(depth_clear_value - 1.0) < 0.00001);

    std::array<std::uint8_t, 4> color{};
    float depth = 0.0F;
    read_pixels(0, 0, 1, 1, gl_rgba, gl_unsigned_byte, color.data());
    if (depth_bits > 0) {
        read_pixels(
            0,
            0,
            1,
            1,
            gl_depth_component,
            gl_float,
            &depth);
    }

    CHECK(std::abs(static_cast<int>(color[0]) - 64) <= 1);
    CHECK(std::abs(static_cast<int>(color[1]) - 128) <= 1);
    CHECK(std::abs(static_cast<int>(color[2]) - 191) <= 1);
    CHECK(color[3] == 255);
    if (depth_bits > 0) {
        CHECK(std::abs(depth - 0.375F) < 0.00001F);
    }

    window->release_current();
    auto without_current_context = device->clear_default(
        {0.0F, 0.0F, 0.0F, 1.0F});
    REQUIRE_FALSE(without_current_context.has_value());
    CHECK(without_current_context.error().code
          == vng::opengl::ErrorCode::context_not_current);
}

TEST_CASE("OpenGL uploads column-major mat4 program uniforms without transposing",
          "[opengl][integration][program][uniform]")
{
    auto window = vng::test::create_hidden_opengl_window(
        8, 8, "vng hidden mat4 uniform test");
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

    constexpr std::uint32_t matrix_location = 5;
    vng::opengl::ShaderSource vertex_source;
    vertex_source.stage = vng::opengl::ShaderStage::vertex;
    vertex_source.text = R"glsl(#version 460 core
layout(location = 5) uniform mat4 transform;
void main()
{
    gl_Position = transform * vec4(0.25, -0.5, 0.75, 1.0);
}
)glsl";
    auto vertex = vng::opengl::Shader::compile(
        *device, std::move(vertex_source));
    INFO((vertex ? std::string{} : describe(vertex.error())));
    REQUIRE(vertex.has_value());

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
        *device, std::move(fragment_source));
    INFO((fragment ? std::string{} : describe(fragment.error())));
    REQUIRE(fragment.has_value());

    auto program = vng::opengl::Program::link_graphics(
        *device, *vertex, *fragment);
    INFO((program ? std::string{} : describe(program.error())));
    REQUIRE(program.has_value());

    const vng::Mat4 matrix{
        .columns = {
            vng::Vec4{1.0F, 2.0F, 3.0F, 4.0F},
            vng::Vec4{5.0F, 6.0F, 7.0F, 8.0F},
            vng::Vec4{9.0F, 10.0F, 11.0F, 12.0F},
            vng::Vec4{13.0F, 14.0F, 15.0F, 16.0F},
        },
    };
    auto uploaded = program->set_uniform_mat4(matrix_location, matrix);
    INFO((uploaded ? std::string{} : describe(uploaded.error())));
    REQUIRE(uploaded.has_value());

    using GetUniformFloat = void (*)(
        std::uint32_t, std::int32_t, float*);
    const auto get_uniform_float = reinterpret_cast<GetUniformFloat>(
        access->resolve("glGetUniformfv"));
    REQUIRE(get_uniform_float != nullptr);

    std::array<float, 16> actual{};
    get_uniform_float(
        program->native_handle(),
        static_cast<std::int32_t>(matrix_location),
        actual.data());
    const std::array<float, 16> expected{
        1.0F, 2.0F, 3.0F, 4.0F,
        5.0F, 6.0F, 7.0F, 8.0F,
        9.0F, 10.0F, 11.0F, 12.0F,
        13.0F, 14.0F, 15.0F, 16.0F,
    };
    CHECK(actual == expected);

    auto oversized = program->set_uniform_mat4(
        std::numeric_limits<std::uint32_t>::max(), matrix);
    REQUIRE_FALSE(oversized.has_value());
    CHECK(oversized.error().code
          == vng::opengl::ErrorCode::invalid_argument);

    window->release_current();
    auto without_current_context = program->set_uniform_mat4(
        matrix_location, matrix);
    REQUIRE_FALSE(without_current_context.has_value());
    CHECK(without_current_context.error().code
          == vng::opengl::ErrorCode::context_not_current);
    auto restored = window->make_current();
    INFO((restored ? std::string{} : restored.error().message));
    REQUIRE(restored.has_value());

    auto live_program = std::move(*program);
    CHECK(live_program.native_handle() != 0);
    auto empty = program->set_uniform_mat4(matrix_location, matrix);
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().code == vng::opengl::ErrorCode::invalid_argument);
}

TEST_CASE("OpenGL renders equivalent instanced geometry from packed and split layouts",
          "[opengl][integration]")
{
    auto window = vng::test::create_hidden_opengl_window(
        64, 32, "vng hidden OpenGL test");
    if (!window) {
        skip_ctest("OpenGL context unavailable: " + window.error().message);
    }
    auto access = window->make_current();
    if (!access) {
        skip_ctest("OpenGL context could not be made current: " + access.error().message);
    }
    auto device = vng::opengl::Device::create(*access);
    INFO((device ? std::string{} : describe(device.error())));
    REQUIRE(device.has_value());

    (void)device->take_debug_messages();
    auto marker = device->debug_marker("vng debug callback probe");
    INFO((marker ? std::string{} : describe(marker.error())));
    REQUIRE(marker.has_value());
    const auto debug_messages = device->take_debug_messages();
    CHECK(std::ranges::any_of(debug_messages, [](const auto& message) {
        return message.message.find("vng debug callback probe") != std::string::npos;
    }));

    auto vertex_stage = vng::shader::vertex<VertexIn, VertexOut>(
        "integration_vertex",
        [](auto& stage) {
            auto position = stage.input(Position{});
            auto offset = stage.input(InstanceOffset{});
            return stage.output(
                vng::dsl::field<vng::shader::ClipPosition>(
                    vng::dsl::vec4(position + offset, 0.0F, 1.0F)),
                vng::dsl::field<Color>(stage.input(Color{})));
        });
    auto fragment_stage = vng::shader::fragment<FragmentIn, FragmentOut>(
        "integration_fragment",
        [](auto& stage) {
            return stage.output(
                vng::dsl::field<vng::shader::Color<0>>(
                    stage.input(Color{})));
        });
    INFO((vertex_stage ? std::string{} : describe(vertex_stage.error())));
    REQUIRE(vertex_stage.has_value());
    INFO((fragment_stage ? std::string{} : describe(fragment_stage.error())));
    REQUIRE(fragment_stage.has_value());
    auto shader_program = vng::shader::link(
        std::move(*vertex_stage), std::move(*fragment_stage));
    INFO((shader_program ? std::string{} : describe(shader_program.error())));
    REQUIRE(shader_program.has_value());
    const auto original_ir = shader_program->vertex().dump_ir();
    auto generated = vng::glsl::emit(*shader_program);
    INFO((generated ? std::string{} : describe(generated.error())));
    REQUIRE(generated.has_value());
    CHECK(generated->vertex.source.find("layout(location = 2) in vec2") != std::string::npos);
    CHECK(generated->vertex.source.find("gl_Position") != std::string::npos);
    CHECK(generated->fragment.source.find("layout(location = 0) out vec4") != std::string::npos);

    auto deliberately_bad_source = vng::opengl::from_glsl(generated->vertex);
    REQUIRE_FALSE(deliberately_bad_source.source_map.empty());
    const auto mapped_line = deliberately_bad_source.source_map.front();
    std::size_t line_begin = 0;
    for (std::uint32_t line = 1; line < mapped_line.generated_line; ++line) {
        line_begin = deliberately_bad_source.text.find('\n', line_begin);
        REQUIRE(line_begin != std::string::npos);
        ++line_begin;
    }
    auto line_end = deliberately_bad_source.text.find('\n', line_begin);
    REQUIRE(line_end != std::string::npos);
    deliberately_bad_source.text.replace(
        line_begin,
        line_end - line_begin,
        "    this is deliberately invalid GLSL;");
    auto deliberately_bad = vng::opengl::Shader::compile(
        *device, deliberately_bad_source);
    REQUIRE_FALSE(deliberately_bad.has_value());
    CHECK(deliberately_bad.error().code ==
          vng::opengl::ErrorCode::shader_compilation_failed);
    CHECK_FALSE(deliberately_bad.error().driver_log.empty());
    CHECK(deliberately_bad.error().generated_source == deliberately_bad_source.text);
    REQUIRE(deliberately_bad.error().source_map.size() ==
            deliberately_bad_source.source_map.size());
    CHECK(deliberately_bad.error().source_map.front().generated_line ==
          mapped_line.generated_line);
    CHECK(deliberately_bad.error().source_map.front().ir_node == mapped_line.ir_node);

    auto vertex_shader = vng::opengl::Shader::compile(
        *device, vng::opengl::from_glsl(generated->vertex));
    INFO((vertex_shader ? std::string{} : describe(vertex_shader.error())));
    REQUIRE(vertex_shader.has_value());
    auto fragment_shader = vng::opengl::Shader::compile(
        *device, vng::opengl::from_glsl(generated->fragment));
    INFO((fragment_shader ? std::string{} : describe(fragment_shader.error())));
    REQUIRE(fragment_shader.has_value());
    auto program = vng::opengl::Program::link_graphics(
        *device, *vertex_shader, *fragment_shader);
    INFO((program ? std::string{} : describe(program.error())));
    REQUIRE(program.has_value());

    Vertex red_vertex;
    red_vertex.set(Color{}, {1.0F, 0.0F, 0.0F, 1.0F});
    vng::gfx::VertexStream<Vertex> vertices{red_vertex, red_vertex, red_vertex};
    vertices[0].set(Position{}, {-0.25F, -0.5F});
    vertices[1].set(Position{}, {0.25F, -0.5F});
    vertices[2].set(Position{}, {0.0F, 0.5F});

    vng::gfx::VertexStream<Instance> instances(2);
    instances[0].set(InstanceOffset{}, {-0.5F, 0.0F});
    instances[1].set(InstanceOffset{}, {0.5F, 0.0F});

    auto vertex_buffer = vng::opengl::Buffer::from_bytes(*device, vertices.bytes());
    auto instance_buffer = vng::opengl::Buffer::from_bytes(*device, instances.bytes());
    INFO((vertex_buffer ? std::string{} : describe(vertex_buffer.error())));
    REQUIRE(vertex_buffer.has_value());
    INFO((instance_buffer ? std::string{} : describe(instance_buffer.error())));
    REQUIRE(instance_buffer.has_value());
    auto vertex_array = vng::opengl::VertexArray::create(*device);
    INFO((vertex_array ? std::string{} : describe(vertex_array.error())));
    REQUIRE(vertex_array.has_value());
    const auto resolved = vng::gfx::resolve_vertex_input<VertexIn, Layout>();
    const std::array streams{
        vng::opengl::ResolvedStreamBuffer{0, &*vertex_buffer, 0},
        vng::opengl::ResolvedStreamBuffer{1, &*instance_buffer, 0},
    };
    auto invalid_extent = resolved;
    invalid_extent.attributes[0].offset = invalid_extent.attributes[0].stride - 1;
    auto rejected_extent = vng::opengl::configure_vertex_input(
        *vertex_array, invalid_extent, streams);
    REQUIRE_FALSE(rejected_extent.has_value());
    CHECK(rejected_extent.error().code == vng::opengl::ErrorCode::invalid_argument);

    auto invalid_alignment = resolved;
    invalid_alignment.attributes[0].offset = 1;
    auto rejected_alignment = vng::opengl::configure_vertex_input(
        *vertex_array, invalid_alignment, streams);
    REQUIRE_FALSE(rejected_alignment.has_value());
    CHECK(rejected_alignment.error().code == vng::opengl::ErrorCode::invalid_argument);

    auto misaligned_streams = streams;
    misaligned_streams[0].base_offset = 1;
    auto rejected_base_alignment = vng::opengl::configure_vertex_input(
        *vertex_array, resolved, misaligned_streams);
    REQUIRE_FALSE(rejected_base_alignment.has_value());
    CHECK(rejected_base_alignment.error().code == vng::opengl::ErrorCode::invalid_argument);

    auto configured = vng::opengl::configure_vertex_input(
        *vertex_array, resolved, streams);
    INFO((configured ? std::string{} : describe(configured.error())));
    REQUIRE(configured.has_value());

    auto color = vng::opengl::Renderbuffer::create(*device, 64, 32);
    auto framebuffer = vng::opengl::Framebuffer::create(*device);
    INFO((color ? std::string{} : describe(color.error())));
    REQUIRE(color.has_value());
    INFO((framebuffer ? std::string{} : describe(framebuffer.error())));
    REQUIRE(framebuffer.has_value());

    auto attached = framebuffer->attach_color(0, *color);
    INFO((attached ? std::string{} : describe(attached.error())));
    REQUIRE(attached.has_value());
    auto complete = framebuffer->check_complete();
    INFO((complete ? std::string{} : describe(complete.error())));
    REQUIRE(complete.has_value());
    auto bound_framebuffer = framebuffer->bind();
    INFO((bound_framebuffer ? std::string{} : describe(bound_framebuffer.error())));
    REQUIRE(bound_framebuffer.has_value());
    auto cleared = framebuffer->clear_color(0, {0.0F, 0.0F, 0.0F, 1.0F});
    INFO((cleared ? std::string{} : describe(cleared.error())));
    REQUIRE(cleared.has_value());
    auto viewport = device->viewport(0, 0, 64, 32);
    INFO((viewport ? std::string{} : describe(viewport.error())));
    REQUIRE(viewport.has_value());
    auto bound_program = program->bind();
    INFO((bound_program ? std::string{} : describe(bound_program.error())));
    REQUIRE(bound_program.has_value());
    auto bound_vertex_array = vertex_array->bind();
    INFO((bound_vertex_array ? std::string{} : describe(bound_vertex_array.error())));
    REQUIRE(bound_vertex_array.has_value());
    auto drawn = device->draw_arrays_instanced(
        vng::opengl::Primitive::triangles, 0, 3, 2);
    INFO((drawn ? std::string{} : describe(drawn.error())));
    REQUIRE(drawn.has_value());
    auto finished = device->finish();
    INFO((finished ? std::string{} : describe(finished.error())));
    REQUIRE(finished.has_value());
    auto pixels = framebuffer->read_rgba8(0, 0, 0, 64, 32);
    INFO((pixels ? std::string{} : describe(pixels.error())));
    REQUIRE(pixels.has_value());
    CHECK(approximately_red(*pixels, 64, 16, 16));
    CHECK(approximately_red(*pixels, 64, 48, 16));
    CHECK(approximately_black(*pixels, 64, 32, 16));

    // Drive the exact same linked shader with a differently ordered, split,
    // f32 color layout. The resulting image must be equivalent to the packed,
    // interleaved layout above.
    vng::gfx::VertexStream<Positions> positions(3);
    Colors red;
    red.set(Color{}, {1.0F, 0.0F, 0.0F, 1.0F});
    vng::gfx::VertexStream<Colors> colors{red, red, red};
    positions[0].set(Position{}, {-0.25F, -0.5F});
    positions[1].set(Position{}, {0.25F, -0.5F});
    positions[2].set(Position{}, {0.0F, 0.5F});

    auto positions_buffer = vng::opengl::Buffer::from_bytes(*device, positions.bytes());
    auto colors_buffer = vng::opengl::Buffer::from_bytes(*device, colors.bytes());
    auto split_vertex_array = vng::opengl::VertexArray::create(*device);
    INFO((positions_buffer ? std::string{} : describe(positions_buffer.error())));
    REQUIRE(positions_buffer.has_value());
    INFO((colors_buffer ? std::string{} : describe(colors_buffer.error())));
    REQUIRE(colors_buffer.has_value());
    INFO((split_vertex_array ? std::string{} : describe(split_vertex_array.error())));
    REQUIRE(split_vertex_array.has_value());
    const auto split_resolved = vng::gfx::resolve_vertex_input<VertexIn, SplitLayout>();
    const std::array split_streams{
        vng::opengl::ResolvedStreamBuffer{0, &*colors_buffer, 0},
        vng::opengl::ResolvedStreamBuffer{1, &*positions_buffer, 0},
        vng::opengl::ResolvedStreamBuffer{2, &*instance_buffer, 0},
    };
    auto split_configured = vng::opengl::configure_vertex_input(
        *split_vertex_array, split_resolved, split_streams);
    INFO((split_configured ? std::string{} : describe(split_configured.error())));
    REQUIRE(split_configured.has_value());
    auto split_cleared = framebuffer->clear_color(0, {0.0F, 0.0F, 0.0F, 1.0F});
    INFO((split_cleared ? std::string{} : describe(split_cleared.error())));
    REQUIRE(split_cleared.has_value());
    auto split_bound = split_vertex_array->bind();
    INFO((split_bound ? std::string{} : describe(split_bound.error())));
    REQUIRE(split_bound.has_value());
    auto split_drawn = device->draw_arrays_instanced(
        vng::opengl::Primitive::triangles, 0, 3, 2);
    INFO((split_drawn ? std::string{} : describe(split_drawn.error())));
    REQUIRE(split_drawn.has_value());
    auto split_pixels = framebuffer->read_rgba8(0, 0, 0, 64, 32);
    INFO((split_pixels ? std::string{} : describe(split_pixels.error())));
    REQUIRE(split_pixels.has_value());
    CHECK(equivalent_pixels(*pixels, *split_pixels));
    CHECK(shader_program->vertex().dump_ir() == original_ir);
}

TEST_CASE("OpenGL rejects direct readback from multisample attachments",
          "[opengl][integration]")
{
    auto window = vng::test::create_hidden_opengl_window(
        8, 8, "vng hidden multisample test");
    if (!window) {
        skip_ctest("OpenGL context unavailable: " + window.error().message);
    }
    auto access = window->make_current();
    if (!access) {
        skip_ctest("OpenGL context could not be made current: " + access.error().message);
    }
    auto device = vng::opengl::Device::create(*access);
    INFO((device ? std::string{} : describe(device.error())));
    REQUIRE(device.has_value());

    auto color = vng::opengl::Renderbuffer::create(
        *device, 8, 8, vng::opengl::RenderbufferFormat::rgba8, 2);
    if (!color && color.error().code == vng::opengl::ErrorCode::invalid_argument) {
        SKIP("Multisample renderbuffers unavailable: " << color.error().message);
    }
    INFO((color ? std::string{} : describe(color.error())));
    REQUIRE(color.has_value());
    CHECK(color->samples() == 2);

    auto framebuffer = vng::opengl::Framebuffer::create(*device);
    INFO((framebuffer ? std::string{} : describe(framebuffer.error())));
    REQUIRE(framebuffer.has_value());
    auto attached = framebuffer->attach_color(0, *color);
    INFO((attached ? std::string{} : describe(attached.error())));
    REQUIRE(attached.has_value());
    auto complete = framebuffer->check_complete();
    INFO((complete ? std::string{} : describe(complete.error())));
    REQUIRE(complete.has_value());

    auto pixels = framebuffer->read_rgba8(0, 0, 0, 8, 8);
    REQUIRE_FALSE(pixels.has_value());
    CHECK(pixels.error().code == vng::opengl::ErrorCode::invalid_argument);
    CHECK(pixels.error().message.find("resolve") != std::string::npos);
}

TEST_CASE("OpenGL texture render targets keep normalized integer and depth data typed",
          "[opengl][integration][image]")
{
    auto window = vng::test::create_hidden_opengl_window(
        4, 3, "vng hidden typed render target test");
    if (!window) {
        skip_ctest("OpenGL context unavailable: " + window.error().message);
    }
    auto access = window->make_current();
    if (!access) {
        skip_ctest("OpenGL context could not be made current: " + access.error().message);
    }
    auto device = vng::opengl::Device::create(*access);
    INFO((device ? std::string{} : describe(device.error())));
    REQUIRE(device.has_value());

    auto color = vng::opengl::Image2D::create(
        *device, 4, 3, vng::opengl::ImageFormat::rgba8);
    auto surface_keys = vng::opengl::Image2D::create(
        *device, 4, 3, vng::opengl::ImageFormat::rg32ui);
    auto depth = vng::opengl::Image2D::create(
        *device, 4, 3, vng::opengl::ImageFormat::depth32f);
    INFO((color ? std::string{} : describe(color.error())));
    REQUIRE(color.has_value());
    INFO((surface_keys ? std::string{} : describe(surface_keys.error())));
    REQUIRE(surface_keys.has_value());
    INFO((depth ? std::string{} : describe(depth.error())));
    REQUIRE(depth.has_value());
    CHECK(color->width() == 4);
    CHECK(color->height() == 3);
    CHECK(color->format() == vng::opengl::ImageFormat::rgba8);
    REQUIRE(color->bind_to_unit(0).has_value());

    auto framebuffer = vng::opengl::Framebuffer::create(*device);
    INFO((framebuffer ? std::string{} : describe(framebuffer.error())));
    REQUIRE(framebuffer.has_value());
    REQUIRE(framebuffer->attach_color(0, *color).has_value());
    REQUIRE(framebuffer->attach_color(1, *surface_keys).has_value());
    REQUIRE(framebuffer->attach_depth(*depth).has_value());
    auto complete = framebuffer->check_complete();
    INFO((complete ? std::string{} : describe(complete.error())));
    REQUIRE(complete.has_value());

    REQUIRE(framebuffer->clear_color(0, {1.0F, 0.0F, 0.0F, 1.0F}).has_value());
    REQUIRE(framebuffer->clear_color(
        1, vng::opengl::Rg32uiPixel{37U, 91U}).has_value());
    REQUIRE(framebuffer->clear_depth(0.375F).has_value());

    CHECK_FALSE(framebuffer->clear_color(1, {0.0F, 0.0F, 0.0F, 0.0F}).has_value());
    CHECK_FALSE(framebuffer->clear_color(
        0, vng::opengl::Rg32uiPixel{0U, 0U}).has_value());
    CHECK_FALSE(framebuffer->attach_color(2, *depth).has_value());
    CHECK_FALSE(framebuffer->attach_depth(*color).has_value());

    auto colors = framebuffer->read_rgba8_pixels(0, 0, 0, 4, 3);
    INFO((colors ? std::string{} : describe(colors.error())));
    REQUIRE(colors.has_value());
    REQUIRE(colors->size() == 12);
    CHECK(std::ranges::all_of(*colors, [](vng::opengl::Rgba8Pixel pixel) {
        return pixel == vng::opengl::Rgba8Pixel{255, 0, 0, 255};
    }));

    auto keys = framebuffer->read_rg32ui(1, 0, 0, 4, 3);
    INFO((keys ? std::string{} : describe(keys.error())));
    REQUIRE(keys.has_value());
    REQUIRE(keys->size() == 12);
    CHECK(std::ranges::all_of(*keys, [](vng::opengl::Rg32uiPixel key) {
        return key == vng::opengl::Rg32uiPixel{37U, 91U};
    }));

    auto depths = framebuffer->read_depth32f(0, 0, 4, 3);
    INFO((depths ? std::string{} : describe(depths.error())));
    REQUIRE(depths.has_value());
    REQUIRE(depths->size() == 12);
    CHECK(std::ranges::all_of(*depths, [](float value) {
        return std::abs(value - 0.375F) < 0.00001F;
    }));
}

TEST_CASE("OpenGL render state scope restores capture state exactly",
          "[opengl][integration][state]")
{
    auto window = vng::test::create_hidden_opengl_window(
        16, 16, "vng hidden render state scope test");
    if (!window) {
        skip_ctest("OpenGL context unavailable: " + window.error().message);
    }
    auto access = window->make_current();
    if (!access) {
        skip_ctest("OpenGL context could not be made current: " + access.error().message);
    }
    auto device = vng::opengl::Device::create(*access);
    INFO((device ? std::string{} : describe(device.error())));
    REQUIRE(device.has_value());

    using Enable = void (*)(std::uint32_t);
    using Disable = void (*)(std::uint32_t);
    using ColorMaskIndexed = void (*)(
        std::uint32_t, std::uint8_t, std::uint8_t, std::uint8_t, std::uint8_t);
    using GetBoolean = void (*)(std::uint32_t, std::uint8_t*);
    using GetBooleanIndexed = void (*)(
        std::uint32_t, std::uint32_t, std::uint8_t*);
    using GetInteger = void (*)(std::uint32_t, std::int32_t*);
    using IsEnabled = std::uint8_t (*)(std::uint32_t);
    using IsEnabledIndexed = std::uint8_t (*)(std::uint32_t, std::uint32_t);
    using Scissor = void (*)(
        std::int32_t, std::int32_t, std::int32_t, std::int32_t);
    using CullFace = void (*)(std::uint32_t);
    using FrontFace = void (*)(std::uint32_t);
    using BindFramebuffer = void (*)(std::uint32_t, std::uint32_t);
    using UseProgram = void (*)(std::uint32_t);

    const auto enable = reinterpret_cast<Enable>(access->resolve("glEnable"));
    const auto disable = reinterpret_cast<Disable>(access->resolve("glDisable"));
    const auto color_mask_indexed = reinterpret_cast<ColorMaskIndexed>(
        access->resolve("glColorMaski"));
    const auto get_boolean = reinterpret_cast<GetBoolean>(
        access->resolve("glGetBooleanv"));
    const auto get_boolean_indexed = reinterpret_cast<GetBooleanIndexed>(
        access->resolve("glGetBooleani_v"));
    const auto get_integer = reinterpret_cast<GetInteger>(
        access->resolve("glGetIntegerv"));
    const auto is_enabled = reinterpret_cast<IsEnabled>(
        access->resolve("glIsEnabled"));
    const auto is_enabled_indexed = reinterpret_cast<IsEnabledIndexed>(
        access->resolve("glIsEnabledi"));
    const auto scissor = reinterpret_cast<Scissor>(access->resolve("glScissor"));
    const auto cull_face = reinterpret_cast<CullFace>(access->resolve("glCullFace"));
    const auto front_face = reinterpret_cast<FrontFace>(access->resolve("glFrontFace"));
    const auto bind_framebuffer = reinterpret_cast<BindFramebuffer>(
        access->resolve("glBindFramebuffer"));
    const auto use_program = reinterpret_cast<UseProgram>(
        access->resolve("glUseProgram"));
    REQUIRE(enable != nullptr);
    REQUIRE(disable != nullptr);
    REQUIRE(color_mask_indexed != nullptr);
    REQUIRE(get_boolean != nullptr);
    REQUIRE(get_boolean_indexed != nullptr);
    REQUIRE(get_integer != nullptr);
    REQUIRE(is_enabled != nullptr);
    REQUIRE(is_enabled_indexed != nullptr);
    REQUIRE(scissor != nullptr);
    REQUIRE(cull_face != nullptr);
    REQUIRE(front_face != nullptr);
    REQUIRE(bind_framebuffer != nullptr);
    REQUIRE(use_program != nullptr);

    constexpr std::uint32_t gl_draw_framebuffer = 0x8CA9;
    constexpr std::uint32_t gl_read_framebuffer = 0x8CA8;
    constexpr std::uint32_t gl_draw_framebuffer_binding = 0x8CA6;
    constexpr std::uint32_t gl_read_framebuffer_binding = 0x8CAA;
    constexpr std::uint32_t gl_viewport = 0x0BA2;
    constexpr std::uint32_t gl_current_program = 0x8B8D;
    constexpr std::uint32_t gl_vertex_array_binding = 0x85B5;
    constexpr std::uint32_t gl_depth_test = 0x0B71;
    constexpr std::uint32_t gl_depth_writemask = 0x0B72;
    constexpr std::uint32_t gl_depth_func = 0x0B74;
    constexpr std::uint32_t gl_greater = 0x0204;
    constexpr std::uint32_t gl_blend = 0x0BE2;
    constexpr std::uint32_t gl_color_writemask = 0x0C23;
    constexpr std::uint32_t gl_scissor_test = 0x0C11;
    constexpr std::uint32_t gl_scissor_box = 0x0C10;
    constexpr std::uint32_t gl_cull_face = 0x0B44;
    constexpr std::uint32_t gl_cull_face_mode = 0x0B45;
    constexpr std::uint32_t gl_front_face = 0x0B46;
    constexpr std::uint32_t gl_front = 0x0404;
    constexpr std::uint32_t gl_cw = 0x0900;
    constexpr std::uint32_t gl_rasterizer_discard = 0x8C89;
    constexpr std::uint32_t gl_framebuffer_srgb = 0x8DB9;

    auto original_draw = vng::opengl::Framebuffer::create(*device);
    auto original_read = vng::opengl::Framebuffer::create(*device);
    auto original_vao = vng::opengl::VertexArray::create(*device);
    auto changed_vao = vng::opengl::VertexArray::create(*device);
    REQUIRE(original_draw.has_value());
    REQUIRE(original_read.has_value());
    REQUIRE(original_vao.has_value());
    REQUIRE(changed_vao.has_value());

    vng::opengl::ShaderSource vertex_source;
    vertex_source.stage = vng::opengl::ShaderStage::vertex;
    vertex_source.text =
        "#version 460 core\nvoid main(){gl_Position=vec4(0.0);}\n";
    auto vertex = vng::opengl::Shader::compile(
        *device, std::move(vertex_source));
    vng::opengl::ShaderSource fragment_source;
    fragment_source.stage = vng::opengl::ShaderStage::fragment;
    fragment_source.text =
        "#version 460 core\nlayout(location=0) out vec4 c;void main(){c=vec4(1.0);}\n";
    auto fragment = vng::opengl::Shader::compile(
        *device, std::move(fragment_source));
    INFO((vertex ? std::string{} : describe(vertex.error())));
    REQUIRE(vertex.has_value());
    INFO((fragment ? std::string{} : describe(fragment.error())));
    REQUIRE(fragment.has_value());
    auto program = vng::opengl::Program::link_graphics(
        *device, *vertex, *fragment);
    INFO((program ? std::string{} : describe(program.error())));
    REQUIRE(program.has_value());

    bind_framebuffer(gl_draw_framebuffer, original_draw->native_handle());
    bind_framebuffer(gl_read_framebuffer, original_read->native_handle());
    REQUIRE(device->viewport(1, 2, 7, 8).has_value());
    REQUIRE(program->bind().has_value());
    REQUIRE(original_vao->bind().has_value());
    REQUIRE(device->set_depth_state({
        .test_enabled = true,
        .write_enabled = false,
        .compare = vng::opengl::DepthCompare::greater,
    }).has_value());
    REQUIRE(device->set_blend_enabled(0, true).has_value());
    REQUIRE(device->set_blend_enabled(1, false).has_value());
    color_mask_indexed(0, 1, 0, 1, 0);
    color_mask_indexed(1, 0, 1, 0, 1);
    enable(gl_scissor_test);
    scissor(2, 3, 4, 5);
    enable(gl_cull_face);
    cull_face(gl_front);
    front_face(gl_cw);
    enable(gl_rasterizer_discard);
    enable(gl_framebuffer_srgb);

    const std::array affected_draw_buffers{1U, 0U, 1U};
    auto saved = vng::opengl::RenderStateScope::capture(
        *device, affected_draw_buffers);
    INFO((saved ? std::string{} : describe(saved.error())));
    REQUIRE(saved.has_value());

    REQUIRE(device->bind_default_framebuffer().has_value());
    REQUIRE(device->viewport(0, 0, 1, 1).has_value());
    use_program(0);
    REQUIRE(changed_vao->bind().has_value());
    REQUIRE(device->set_depth_state({
        .test_enabled = false,
        .write_enabled = true,
        .compare = vng::opengl::DepthCompare::less,
    }).has_value());
    REQUIRE(device->set_blend_enabled(0, false).has_value());
    REQUIRE(device->set_blend_enabled(1, true).has_value());
    color_mask_indexed(0, 0, 1, 0, 1);
    color_mask_indexed(1, 1, 0, 1, 0);
    disable(gl_scissor_test);
    scissor(0, 0, 1, 1);
    disable(gl_cull_face);
    cull_face(0x0405); // GL_BACK
    front_face(0x0901); // GL_CCW
    disable(gl_rasterizer_discard);
    disable(gl_framebuffer_srgb);

    REQUIRE(saved->restore().has_value());
    CHECK_FALSE(saved->active());
    REQUIRE(saved->restore().has_value());

    std::int32_t scalar = 0;
    get_integer(gl_draw_framebuffer_binding, &scalar);
    CHECK(static_cast<std::uint32_t>(scalar) == original_draw->native_handle());
    get_integer(gl_read_framebuffer_binding, &scalar);
    CHECK(static_cast<std::uint32_t>(scalar) == original_read->native_handle());
    std::array<std::int32_t, 4> viewport{};
    get_integer(gl_viewport, viewport.data());
    CHECK(viewport == std::array<std::int32_t, 4>{1, 2, 7, 8});
    get_integer(gl_current_program, &scalar);
    CHECK(static_cast<std::uint32_t>(scalar) == program->native_handle());
    get_integer(gl_vertex_array_binding, &scalar);
    CHECK(static_cast<std::uint32_t>(scalar) == original_vao->native_handle());
    CHECK(is_enabled(gl_depth_test) != 0);
    std::uint8_t boolean = 0;
    get_boolean(gl_depth_writemask, &boolean);
    CHECK(boolean == 0);
    get_integer(gl_depth_func, &scalar);
    CHECK(static_cast<std::uint32_t>(scalar) == gl_greater);
    CHECK(is_enabled_indexed(gl_blend, 0) != 0);
    CHECK(is_enabled_indexed(gl_blend, 1) == 0);
    std::array<std::uint8_t, 4> mask{};
    get_boolean_indexed(gl_color_writemask, 0, mask.data());
    CHECK(mask == std::array<std::uint8_t, 4>{1, 0, 1, 0});
    get_boolean_indexed(gl_color_writemask, 1, mask.data());
    CHECK(mask == std::array<std::uint8_t, 4>{0, 1, 0, 1});
    CHECK(is_enabled(gl_scissor_test) != 0);
    std::array<std::int32_t, 4> scissor_box{};
    get_integer(gl_scissor_box, scissor_box.data());
    CHECK(scissor_box == std::array<std::int32_t, 4>{2, 3, 4, 5});
    CHECK(is_enabled(gl_cull_face) != 0);
    get_integer(gl_cull_face_mode, &scalar);
    CHECK(static_cast<std::uint32_t>(scalar) == gl_front);
    get_integer(gl_front_face, &scalar);
    CHECK(static_cast<std::uint32_t>(scalar) == gl_cw);
    CHECK(is_enabled(gl_rasterizer_discard) != 0);
    CHECK(is_enabled(gl_framebuffer_srgb) != 0);

    REQUIRE(device->viewport(3, 4, 5, 6).has_value());
    {
        auto automatic = vng::opengl::RenderStateScope::capture(*device);
        REQUIRE(automatic.has_value());
        REQUIRE(device->viewport(0, 0, 2, 2).has_value());
    }
    get_integer(gl_viewport, viewport.data());
    CHECK(viewport == std::array<std::int32_t, 4>{3, 4, 5, 6});

    const std::array invalid_draw_buffer{std::numeric_limits<std::uint32_t>::max()};
    auto invalid = vng::opengl::RenderStateScope::capture(
        *device, invalid_draw_buffer);
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error().code == vng::opengl::ErrorCode::invalid_argument);

    REQUIRE(device->bind_default_framebuffer().has_value());
    disable(gl_scissor_test);
    disable(gl_cull_face);
    disable(gl_rasterizer_discard);
    disable(gl_framebuffer_srgb);
}

TEST_CASE("OpenGL viewport validates limits without changing state",
          "[opengl][integration][state]")
{
    auto window = vng::test::create_hidden_opengl_window(
        8, 8, "vng hidden viewport validation test");
    if (!window) {
        skip_ctest("OpenGL context unavailable: " + window.error().message);
    }
    auto access = window->make_current();
    if (!access) {
        skip_ctest("OpenGL context could not be made current: " + access.error().message);
    }
    auto device = vng::opengl::Device::create(*access);
    INFO((device ? std::string{} : describe(device.error())));
    REQUIRE(device.has_value());

    using GetInteger = void (*)(std::uint32_t, std::int32_t*);
    const auto get_integer = reinterpret_cast<GetInteger>(
        access->resolve("glGetIntegerv"));
    REQUIRE(get_integer != nullptr);

    constexpr std::uint32_t gl_viewport = 0x0BA2;
    constexpr std::uint32_t gl_max_viewport_dims = 0x0D3A;
    std::array<std::int32_t, 2> maximum_dimensions{};
    get_integer(gl_max_viewport_dims, maximum_dimensions.data());
    REQUIRE(maximum_dimensions[0] > 0);
    REQUIRE(maximum_dimensions[1] > 0);

    REQUIRE(device->viewport(1, 2, 4, 5).has_value());
    const auto expect_original_viewport = [&] {
        std::array<std::int32_t, 4> viewport{};
        get_integer(gl_viewport, viewport.data());
        CHECK(viewport == std::array<std::int32_t, 4>{1, 2, 4, 5});
    };

    auto overflowing_edge = device->viewport(
        std::numeric_limits<std::int32_t>::max(), 0, 1, 1);
    REQUIRE_FALSE(overflowing_edge.has_value());
    CHECK(overflowing_edge.error().code
          == vng::opengl::ErrorCode::invalid_argument);
    CHECK(overflowing_edge.error().message.find("coordinate plus extent")
          != std::string::npos);
    expect_original_viewport();

    if (maximum_dimensions[0]
        < std::numeric_limits<std::int32_t>::max()) {
        auto oversized = device->viewport(
            0, 0, maximum_dimensions[0] + 1, 1);
        REQUIRE_FALSE(oversized.has_value());
        CHECK(oversized.error().code
              == vng::opengl::ErrorCode::invalid_argument);
        CHECK(oversized.error().message.find("GL_MAX_VIEWPORT_DIMS")
              != std::string::npos);
        expect_original_viewport();
    }

    auto negative = device->viewport(0, 0, -1, 1);
    REQUIRE_FALSE(negative.has_value());
    CHECK(negative.error().code == vng::opengl::ErrorCode::invalid_argument);
    expect_original_viewport();

    // A valid call after every rejected case proves that no rejected request
    // leaked an OpenGL error into the next checked boundary.
    REQUIRE(device->viewport(0, 0, 1, 1).has_value());
}

TEST_CASE("OpenGL standard raster state is deterministic and scoped",
          "[opengl][integration][state]")
{
    auto window = vng::test::create_hidden_opengl_window(
        8, 8, "vng hidden hostile raster state test");
    if (!window) {
        skip_ctest("OpenGL context unavailable: " + window.error().message);
    }
    auto access = window->make_current();
    if (!access) {
        skip_ctest("OpenGL context could not be made current: " + access.error().message);
    }
    auto device = vng::opengl::Device::create(*access);
    INFO((device ? std::string{} : describe(device.error())));
    REQUIRE(device.has_value());

    using Capability = void (*)(std::uint32_t);
    using IndexedCapability = void (*)(std::uint32_t, std::uint32_t);
    using IsEnabled = std::uint8_t (*)(std::uint32_t);
    using IsEnabledIndexed = std::uint8_t (*)(
        std::uint32_t, std::uint32_t);
    using GetError = std::uint32_t (*)();
    using GetBoolean = void (*)(std::uint32_t, std::uint8_t*);
    using GetInteger = void (*)(std::uint32_t, std::int32_t*);
    using GetIntegerIndexed = void (*)(
        std::uint32_t, std::uint32_t, std::int32_t*);
    using GetFloat = void (*)(std::uint32_t, float*);
    using GetFloatIndexed = void (*)(
        std::uint32_t, std::uint32_t, float*);
    using GetDoubleIndexed = void (*)(
        std::uint32_t, std::uint32_t, double*);
    using PolygonMode = void (*)(std::uint32_t, std::uint32_t);
    using SampleMask = void (*)(std::uint32_t, std::uint32_t);
    using SampleCoverage = void (*)(float, std::uint8_t);
    using ClipControl = void (*)(std::uint32_t, std::uint32_t);
    using DepthRange = void (*)(double, double);
    using DepthRangeIndexed = void (*)(std::uint32_t, double, double);
    using LogicOp = void (*)(std::uint32_t);
    using ViewportIndexed = void (*)(
        std::uint32_t, float, float, float, float);
    using ScissorIndexed = void (*)(
        std::uint32_t,
        std::int32_t,
        std::int32_t,
        std::int32_t,
        std::int32_t);

    const auto enable = reinterpret_cast<Capability>(access->resolve("glEnable"));
    const auto disable = reinterpret_cast<Capability>(access->resolve("glDisable"));
    const auto enable_indexed = reinterpret_cast<IndexedCapability>(
        access->resolve("glEnablei"));
    const auto disable_indexed = reinterpret_cast<IndexedCapability>(
        access->resolve("glDisablei"));
    const auto is_enabled = reinterpret_cast<IsEnabled>(
        access->resolve("glIsEnabled"));
    const auto is_enabled_indexed = reinterpret_cast<IsEnabledIndexed>(
        access->resolve("glIsEnabledi"));
    const auto get_error = reinterpret_cast<GetError>(access->resolve("glGetError"));
    const auto get_boolean = reinterpret_cast<GetBoolean>(
        access->resolve("glGetBooleanv"));
    const auto get_integer = reinterpret_cast<GetInteger>(
        access->resolve("glGetIntegerv"));
    const auto get_integer_indexed = reinterpret_cast<GetIntegerIndexed>(
        access->resolve("glGetIntegeri_v"));
    const auto get_float = reinterpret_cast<GetFloat>(access->resolve("glGetFloatv"));
    const auto get_float_indexed = reinterpret_cast<GetFloatIndexed>(
        access->resolve("glGetFloati_v"));
    const auto get_double_indexed = reinterpret_cast<GetDoubleIndexed>(
        access->resolve("glGetDoublei_v"));
    const auto polygon_mode = reinterpret_cast<PolygonMode>(
        access->resolve("glPolygonMode"));
    const auto sample_mask = reinterpret_cast<SampleMask>(
        access->resolve("glSampleMaski"));
    const auto sample_coverage = reinterpret_cast<SampleCoverage>(
        access->resolve("glSampleCoverage"));
    const auto clip_control = reinterpret_cast<ClipControl>(
        access->resolve("glClipControl"));
    const auto depth_range = reinterpret_cast<DepthRange>(
        access->resolve("glDepthRange"));
    const auto depth_range_indexed = reinterpret_cast<DepthRangeIndexed>(
        access->resolve("glDepthRangeIndexed"));
    const auto logic_op = reinterpret_cast<LogicOp>(access->resolve("glLogicOp"));
    const auto viewport_indexed = reinterpret_cast<ViewportIndexed>(
        access->resolve("glViewportIndexedf"));
    const auto scissor_indexed = reinterpret_cast<ScissorIndexed>(
        access->resolve("glScissorIndexed"));
    REQUIRE(enable != nullptr);
    REQUIRE(disable != nullptr);
    REQUIRE(enable_indexed != nullptr);
    REQUIRE(disable_indexed != nullptr);
    REQUIRE(is_enabled != nullptr);
    REQUIRE(is_enabled_indexed != nullptr);
    REQUIRE(get_error != nullptr);
    REQUIRE(get_boolean != nullptr);
    REQUIRE(get_integer != nullptr);
    REQUIRE(get_integer_indexed != nullptr);
    REQUIRE(get_float != nullptr);
    REQUIRE(get_float_indexed != nullptr);
    REQUIRE(get_double_indexed != nullptr);
    REQUIRE(polygon_mode != nullptr);
    REQUIRE(sample_mask != nullptr);
    REQUIRE(sample_coverage != nullptr);
    REQUIRE(clip_control != nullptr);
    REQUIRE(depth_range != nullptr);
    REQUIRE(depth_range_indexed != nullptr);
    REQUIRE(logic_op != nullptr);
    REQUIRE(viewport_indexed != nullptr);
    REQUIRE(scissor_indexed != nullptr);

    constexpr std::uint32_t gl_no_error = 0;
    constexpr std::uint32_t gl_color_logic_op = 0x0BF2;
    constexpr std::uint32_t gl_logic_op_mode = 0x0BF0;
    constexpr std::uint32_t gl_xor = 0x1506;
    constexpr std::uint32_t gl_polygon_mode = 0x0B40;
    constexpr std::uint32_t gl_line = 0x1B01;
    constexpr std::uint32_t gl_point = 0x1B00;
    constexpr std::uint32_t gl_fill = 0x1B02;
    constexpr std::uint32_t gl_front_and_back = 0x0408;
    constexpr std::uint32_t gl_sample_mask = 0x8E51;
    constexpr std::uint32_t gl_sample_mask_value = 0x8E52;
    constexpr std::uint32_t gl_max_sample_mask_words = 0x8E59;
    constexpr std::uint32_t gl_sample_alpha_to_coverage = 0x809E;
    constexpr std::uint32_t gl_sample_alpha_to_one = 0x809F;
    constexpr std::uint32_t gl_sample_coverage = 0x80A0;
    constexpr std::uint32_t gl_sample_coverage_value = 0x80AA;
    constexpr std::uint32_t gl_sample_coverage_invert = 0x80AB;
    constexpr std::uint32_t gl_sample_shading = 0x8C36;
    constexpr std::uint32_t gl_polygon_offset_fill = 0x8037;
    constexpr std::uint32_t gl_polygon_offset_line = 0x2A02;
    constexpr std::uint32_t gl_polygon_offset_point = 0x2A01;
    constexpr std::uint32_t gl_depth_clamp = 0x864F;
    constexpr std::uint32_t gl_primitive_restart = 0x8F9D;
    constexpr std::uint32_t gl_primitive_restart_fixed_index = 0x8D69;
    constexpr std::uint32_t gl_dither = 0x0BD0;
    constexpr std::uint32_t gl_polygon_smooth = 0x0B41;
    constexpr std::uint32_t gl_stencil_test = 0x0B90;
    constexpr std::uint32_t gl_multisample = 0x809D;
    constexpr std::uint32_t gl_clip_origin = 0x935C;
    constexpr std::uint32_t gl_clip_depth_mode = 0x935D;
    constexpr std::uint32_t gl_upper_left = 0x8CA2;
    constexpr std::uint32_t gl_lower_left = 0x8CA1;
    constexpr std::uint32_t gl_zero_to_one = 0x935F;
    constexpr std::uint32_t gl_negative_one_to_one = 0x935E;
    constexpr std::uint32_t gl_depth_range = 0x0B70;
    constexpr std::uint32_t gl_max_clip_distances = 0x0D32;
    constexpr std::uint32_t gl_clip_distance0 = 0x3000;
    constexpr std::uint32_t gl_max_viewports = 0x825B;
    constexpr std::uint32_t gl_viewport = 0x0BA2;
    constexpr std::uint32_t gl_scissor_test = 0x0C11;
    constexpr std::uint32_t gl_scissor_box = 0x0C10;

    const std::array hostile_capabilities{
        gl_color_logic_op,
        gl_sample_mask,
        gl_sample_alpha_to_coverage,
        gl_sample_alpha_to_one,
        gl_sample_coverage,
        gl_sample_shading,
        gl_polygon_offset_fill,
        gl_polygon_offset_line,
        gl_polygon_offset_point,
        gl_depth_clamp,
        gl_primitive_restart,
        gl_primitive_restart_fixed_index,
        gl_dither,
        gl_polygon_smooth,
        gl_stencil_test,
    };

    std::int32_t maximum_sample_mask_words = 0;
    std::int32_t maximum_clip_distances = 0;
    std::int32_t maximum_viewports = 0;
    get_integer(gl_max_sample_mask_words, &maximum_sample_mask_words);
    get_integer(gl_max_clip_distances, &maximum_clip_distances);
    get_integer(gl_max_viewports, &maximum_viewports);
    REQUIRE(maximum_sample_mask_words > 0);
    REQUIRE(maximum_clip_distances > 0);
    REQUIRE(maximum_viewports > 1);

    while (get_error() != gl_no_error) {
    }
    for (const auto capability : hostile_capabilities) {
        enable(capability);
    }
    enable(gl_multisample);
    logic_op(gl_xor);
    polygon_mode(gl_front_and_back, gl_line);

    const std::array<float, 4> original_viewport0{0.25F, 0.5F, 6.5F, 7.25F};
    const std::array<float, 4> original_viewport1{1.5F, 1.25F, 5.75F, 4.5F};
    viewport_indexed(
        0,
        original_viewport0[0],
        original_viewport0[1],
        original_viewport0[2],
        original_viewport0[3]);
    viewport_indexed(
        1,
        original_viewport1[0],
        original_viewport1[1],
        original_viewport1[2],
        original_viewport1[3]);
    const std::array<std::int32_t, 4> original_scissor0{1, 2, 3, 4};
    const std::array<std::int32_t, 4> original_scissor1{4, 3, 2, 1};
    scissor_indexed(
        0,
        original_scissor0[0],
        original_scissor0[1],
        original_scissor0[2],
        original_scissor0[3]);
    scissor_indexed(
        1,
        original_scissor1[0],
        original_scissor1[1],
        original_scissor1[2],
        original_scissor1[3]);
    enable_indexed(gl_scissor_test, 0);
    disable_indexed(gl_scissor_test, 1);

    std::vector<std::uint32_t> original_mask_words(
        static_cast<std::size_t>(maximum_sample_mask_words));
    for (std::size_t index = 0; index < original_mask_words.size(); ++index) {
        original_mask_words[index] = 0xA5A50000U
            ^ static_cast<std::uint32_t>(index * 0x1111U);
        sample_mask(
            static_cast<std::uint32_t>(index),
            original_mask_words[index]);
    }
    sample_coverage(0.375F, 1);
    clip_control(gl_upper_left, gl_zero_to_one);
    std::vector<std::array<double, 2>> original_depth_ranges(
        static_cast<std::size_t>(maximum_viewports));
    for (std::size_t index = 0; index < original_depth_ranges.size(); ++index) {
        const auto near_value = static_cast<double>(index + 1)
            / (4.0 * static_cast<double>(original_depth_ranges.size() + 1));
        const auto far_value = 1.0 - near_value;
        depth_range_indexed(
            static_cast<std::uint32_t>(index),
            near_value,
            far_value);
        get_double_indexed(
            gl_depth_range,
            static_cast<std::uint32_t>(index),
            original_depth_ranges[index].data());
    }
    for (std::int32_t index = 0; index < maximum_clip_distances; ++index) {
        const auto capability = gl_clip_distance0
            + static_cast<std::uint32_t>(index);
        if (index % 2 == 0) {
            enable(capability);
        } else {
            disable(capability);
        }
    }
    REQUIRE(get_error() == gl_no_error);

    auto saved = vng::opengl::RenderStateScope::capture(*device);
    INFO((saved ? std::string{} : describe(saved.error())));
    REQUIRE(saved.has_value());

    auto standardized = device->set_standard_raster_state();
    INFO((standardized ? std::string{} : describe(standardized.error())));
    REQUIRE(standardized.has_value());

    for (const auto capability : hostile_capabilities) {
        CHECK(is_enabled(capability) == 0);
    }
    CHECK(is_enabled(gl_multisample) != 0);
    std::int32_t integer = 0;
    get_integer(gl_logic_op_mode, &integer);
    CHECK(static_cast<std::uint32_t>(integer) == gl_xor);
    std::array<std::int32_t, 2> polygon_modes{};
    get_integer(gl_polygon_mode, polygon_modes.data());
    CHECK(static_cast<std::uint32_t>(polygon_modes[0]) == gl_fill);
    get_integer(gl_clip_origin, &integer);
    CHECK(static_cast<std::uint32_t>(integer) == gl_lower_left);
    get_integer(gl_clip_depth_mode, &integer);
    CHECK(static_cast<std::uint32_t>(integer) == gl_negative_one_to_one);
    std::array<double, 2> depth_values{};
    for (std::int32_t index = 0; index < maximum_viewports; ++index) {
        get_double_indexed(
            gl_depth_range,
            static_cast<std::uint32_t>(index),
            depth_values.data());
        CHECK(depth_values == std::array<double, 2>{0.0, 1.0});
    }
    for (std::int32_t index = 0; index < maximum_clip_distances; ++index) {
        CHECK(is_enabled(
                  gl_clip_distance0 + static_cast<std::uint32_t>(index))
              == 0);
    }

    // Disabling these features must not needlessly replace their parameters.
    for (std::size_t index = 0; index < original_mask_words.size(); ++index) {
        get_integer_indexed(
            gl_sample_mask_value,
            static_cast<std::uint32_t>(index),
            &integer);
        CHECK(static_cast<std::uint32_t>(integer)
              == original_mask_words[index]);
    }
    float coverage_value = 0.0F;
    std::uint8_t coverage_inverted = 0;
    get_float(gl_sample_coverage_value, &coverage_value);
    get_boolean(gl_sample_coverage_invert, &coverage_inverted);
    CHECK(std::abs(coverage_value - 0.375F) < 0.00001F);
    CHECK(coverage_inverted != 0);

    // Change captured values as well as enables to exercise exact restoration.
    polygon_mode(gl_front_and_back, gl_point);
    for (std::size_t index = 0; index < original_mask_words.size(); ++index) {
        sample_mask(static_cast<std::uint32_t>(index), 0U);
    }
    sample_coverage(0.875F, 0);
    depth_range(0.5, 0.5);
    REQUIRE(device->viewport(0, 0, 1, 1).has_value());
    REQUIRE(device->set_scissor_enabled(false).has_value());
    scissor_indexed(0, 0, 0, 1, 1);
    scissor_indexed(1, 0, 0, 1, 1);
    REQUIRE(saved->restore().has_value());

    for (const auto capability : hostile_capabilities) {
        CHECK(is_enabled(capability) != 0);
    }
    CHECK(is_enabled(gl_multisample) != 0);
    get_integer(gl_logic_op_mode, &integer);
    CHECK(static_cast<std::uint32_t>(integer) == gl_xor);
    get_integer(gl_polygon_mode, polygon_modes.data());
    CHECK(static_cast<std::uint32_t>(polygon_modes[0]) == gl_line);
    for (std::size_t index = 0; index < original_mask_words.size(); ++index) {
        get_integer_indexed(
            gl_sample_mask_value,
            static_cast<std::uint32_t>(index),
            &integer);
        CHECK(static_cast<std::uint32_t>(integer)
              == original_mask_words[index]);
    }
    get_float(gl_sample_coverage_value, &coverage_value);
    get_boolean(gl_sample_coverage_invert, &coverage_inverted);
    CHECK(std::abs(coverage_value - 0.375F) < 0.00001F);
    CHECK(coverage_inverted != 0);
    get_integer(gl_clip_origin, &integer);
    CHECK(static_cast<std::uint32_t>(integer) == gl_upper_left);
    get_integer(gl_clip_depth_mode, &integer);
    CHECK(static_cast<std::uint32_t>(integer) == gl_zero_to_one);
    for (std::size_t index = 0; index < original_depth_ranges.size(); ++index) {
        get_double_indexed(
            gl_depth_range,
            static_cast<std::uint32_t>(index),
            depth_values.data());
        CHECK(std::abs(
                  depth_values[0] - original_depth_ranges[index][0])
              < 0.0000001);
        CHECK(std::abs(
                  depth_values[1] - original_depth_ranges[index][1])
              < 0.0000001);
    }
    std::array<float, 4> viewport_values{};
    get_float_indexed(gl_viewport, 0, viewport_values.data());
    CHECK(viewport_values == original_viewport0);
    get_float_indexed(gl_viewport, 1, viewport_values.data());
    CHECK(viewport_values == original_viewport1);
    std::array<std::int32_t, 4> scissor_values{};
    get_integer_indexed(gl_scissor_box, 0, scissor_values.data());
    CHECK(scissor_values == original_scissor0);
    get_integer_indexed(gl_scissor_box, 1, scissor_values.data());
    CHECK(scissor_values == original_scissor1);
    CHECK(is_enabled_indexed(gl_scissor_test, 0) != 0);
    CHECK(is_enabled_indexed(gl_scissor_test, 1) == 0);
    for (std::int32_t index = 0; index < maximum_clip_distances; ++index) {
        CHECK((is_enabled(
                   gl_clip_distance0 + static_cast<std::uint32_t>(index))
               != 0)
              == (index % 2 == 0));
    }
}

TEST_CASE("OpenGL resource factories validate limits and failed attachments are transactional",
          "[opengl][integration][resource]")
{
    auto window = vng::test::create_hidden_opengl_window(
        8, 8, "vng hidden resource validation test");
    if (!window) {
        skip_ctest("OpenGL context unavailable: " + window.error().message);
    }
    auto access = window->make_current();
    if (!access) {
        skip_ctest("OpenGL context could not be made current: " + access.error().message);
    }
    auto device = vng::opengl::Device::create(*access);
    INFO((device ? std::string{} : describe(device.error())));
    REQUIRE(device.has_value());

    auto empty_buffer = vng::opengl::Buffer::create(*device, {.size = 0});
    REQUIRE_FALSE(empty_buffer.has_value());
    CHECK(empty_buffer.error().code == vng::opengl::ErrorCode::invalid_argument);

    auto oversized = vng::opengl::Renderbuffer::create(
        *device,
        static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()),
        1);
    REQUIRE_FALSE(oversized.has_value());
    CHECK(oversized.error().code == vng::opengl::ErrorCode::invalid_argument);
    CHECK(oversized.error().message.find("GL_MAX_RENDERBUFFER_SIZE") != std::string::npos);

    using GetInteger = void (*)(std::uint32_t, std::int32_t*);
    using GetInternalFormat = void (*)(
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::int32_t,
        std::int32_t*);
    using DeleteRenderbuffers = void (*)(std::int32_t, const std::uint32_t*);

    const auto get_integer = reinterpret_cast<GetInteger>(
        access->resolve("glGetIntegerv"));
    const auto get_internal_format = reinterpret_cast<GetInternalFormat>(
        access->resolve("glGetInternalformativ"));
    const auto delete_renderbuffers = reinterpret_cast<DeleteRenderbuffers>(
        access->resolve("glDeleteRenderbuffers"));
    REQUIRE(get_integer != nullptr);
    REQUIRE(get_internal_format != nullptr);
    REQUIRE(delete_renderbuffers != nullptr);

    constexpr std::uint32_t gl_max_samples = 0x8D57;
    constexpr std::uint32_t gl_renderbuffer = 0x8D41;
    constexpr std::uint32_t gl_rgba8 = 0x8058;
    constexpr std::uint32_t gl_samples = 0x80A9;
    constexpr std::uint32_t gl_num_sample_counts = 0x9380;
    std::int32_t max_samples = 0;
    std::int32_t sample_count = 0;
    get_integer(gl_max_samples, &max_samples);
    get_internal_format(
        gl_renderbuffer,
        gl_rgba8,
        gl_num_sample_counts,
        1,
        &sample_count);
    REQUIRE(max_samples >= 0);
    REQUIRE(sample_count >= 0);

    std::vector<std::int32_t> supported_samples(
        static_cast<std::size_t>(sample_count));
    if (!supported_samples.empty()) {
        get_internal_format(
            gl_renderbuffer,
            gl_rgba8,
            gl_samples,
            sample_count,
            supported_samples.data());
    }

    std::optional<std::uint32_t> unsupported_sample;
    for (std::int32_t candidate = 1; candidate <= max_samples; ++candidate) {
        if (std::ranges::find(supported_samples, candidate) == supported_samples.end()) {
            unsupported_sample = static_cast<std::uint32_t>(candidate);
            break;
        }
    }
    if (unsupported_sample) {
        auto rejected = vng::opengl::Renderbuffer::create(
            *device,
            8,
            8,
            vng::opengl::RenderbufferFormat::rgba8,
            *unsupported_sample);
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().code == vng::opengl::ErrorCode::invalid_argument);
        CHECK(rejected.error().message.find("not supported") != std::string::npos);
    }

    auto excessive_samples = vng::opengl::Renderbuffer::create(
        *device,
        8,
        8,
        vng::opengl::RenderbufferFormat::rgba8,
        std::numeric_limits<std::uint32_t>::max());
    REQUIRE_FALSE(excessive_samples.has_value());
    CHECK(excessive_samples.error().code == vng::opengl::ErrorCode::invalid_argument);
    CHECK(excessive_samples.error().message.find("GL_MAX_SAMPLES") != std::string::npos);

    auto single_sample = vng::opengl::Renderbuffer::create(*device, 8, 8);
    auto framebuffer = vng::opengl::Framebuffer::create(*device);
    INFO((single_sample ? std::string{} : describe(single_sample.error())));
    REQUIRE(single_sample.has_value());
    INFO((framebuffer ? std::string{} : describe(framebuffer.error())));
    REQUIRE(framebuffer.has_value());
    auto attached = framebuffer->attach_color(0, *single_sample);
    INFO((attached ? std::string{} : describe(attached.error())));
    REQUIRE(attached.has_value());
    REQUIRE(framebuffer->check_complete().has_value());

    const auto multisample = std::ranges::find_if(
        supported_samples,
        [](std::int32_t samples) { return samples > 0; });
    if (multisample != supported_samples.end()) {
        auto doomed = vng::opengl::Renderbuffer::create(
            *device,
            8,
            8,
            vng::opengl::RenderbufferFormat::rgba8,
            static_cast<std::uint32_t>(*multisample));
        INFO((doomed ? std::string{} : describe(doomed.error())));
        REQUIRE(doomed.has_value());
        const auto doomed_handle = doomed->native_handle();
        delete_renderbuffers(1, &doomed_handle);

        auto failed = framebuffer->attach_color(0, *doomed);
        REQUIRE_FALSE(failed.has_value());
        CHECK(failed.error().code == vng::opengl::ErrorCode::operation_failed);
        CHECK(failed.error().message.find("GL_INVALID_OPERATION") != std::string::npos);
        REQUIRE(framebuffer->check_complete().has_value());

        // A failed multisample replacement must not poison the cached sample
        // count for the still-attached single-sample renderbuffer.
        auto pixels = framebuffer->read_rgba8(0, 0, 0, 8, 8);
        INFO((pixels ? std::string{} : describe(pixels.error())));
        REQUIRE(pixels.has_value());
    }

    auto doomed_depth = vng::opengl::Renderbuffer::create(
        *device,
        8,
        8,
        vng::opengl::RenderbufferFormat::depth24_stencil8);
    INFO((doomed_depth ? std::string{} : describe(doomed_depth.error())));
    REQUIRE(doomed_depth.has_value());
    const auto doomed_depth_handle = doomed_depth->native_handle();
    delete_renderbuffers(1, &doomed_depth_handle);
    auto failed_depth = framebuffer->attach_depth_stencil(*doomed_depth);
    REQUIRE_FALSE(failed_depth.has_value());
    CHECK(failed_depth.error().code == vng::opengl::ErrorCode::operation_failed);
    CHECK(failed_depth.error().message.find("GL_INVALID_OPERATION") != std::string::npos);
}
