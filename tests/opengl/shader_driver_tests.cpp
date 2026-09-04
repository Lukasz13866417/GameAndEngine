#include <vng/glsl/glsl.hpp>
#include <vng/opengl/glsl_source.hpp>
#include <vng/opengl/opengl.hpp>
#include <vng/shader/shader.hpp>
#include "../support/glfw_opengl.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Position : vng::gfx::Semantic<vng::Vec2> {};
struct Color : vng::gfx::Semantic<vng::Vec4> {};

using VertexIn = vng::shader::VertexInputs<Position, Color>;
using VertexOut = vng::shader::VertexOutputs<
    vng::shader::ClipPosition,
    vng::shader::smooth<Color>>;
using FragmentIn = vng::shader::FragmentInputs<vng::shader::smooth<Color>>;
using FragmentOut = vng::shader::FragmentOutputs<vng::shader::Color<0>>;

[[noreturn]] void skip_ctest(const std::string& reason)
{
    std::cerr << "OpenGL test skipped: " << reason << '\n';
    std::exit(77);
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

[[nodiscard]] bool contains_message(
    const std::vector<vng::opengl::DebugMessage>& messages,
    const std::string& expected)
{
    return std::ranges::any_of(messages, [&](const auto& message) {
        return message.message.find(expected) != std::string::npos;
    });
}

[[nodiscard]] vng::shader::Result<vng::shader::GraphicsProgram>
make_driver_exercise_program()
{
    auto vertex = vng::shader::vertex<VertexIn, VertexOut>(
        "driver_exercise_vertex",
        [](auto& stage) {
            const auto position = stage.inputs().get(Position{});
            const auto position3 = vng::dsl::vec3(position, 1.0F);
            const auto transform3 = vng::dsl::make<vng::Mat3>(
                stage.constant(vng::Vec3{1.0F, 0.0F, 0.0F}),
                stage.constant(vng::Vec3{0.0F, 1.0F, 0.0F}),
                stage.constant(vng::Vec3{0.0F, 0.0F, 1.0F}));
            const auto transformed3 = transform3 * position3;
            const auto transform4 = vng::dsl::make<vng::Mat4>(
                stage.constant(vng::Vec4{1.0F, 0.0F, 0.0F, 0.0F}),
                stage.constant(vng::Vec4{0.0F, 1.0F, 0.0F, 0.0F}),
                stage.constant(vng::Vec4{0.0F, 0.0F, 1.0F, 0.0F}),
                stage.constant(vng::Vec4{0.0F, 0.0F, 0.0F, 1.0F}));
            const auto clip = transform4 * vng::dsl::vec4(transformed3, 1.0F);
            return vng::dsl::make<VertexOut>(
                vng::dsl::field(vng::shader::ClipPosition{}, clip),
                vng::dsl::field(Color{}, stage.inputs().get(Color{})));
        });
    if (!vertex) {
        return std::unexpected(std::move(vertex.error()));
    }

    auto fragment = vng::shader::fragment<FragmentIn, FragmentOut>(
        "driver_exercise_fragment",
        [](auto& stage) {
            const auto color = stage.inputs().get(Color{});
            const auto threshold = stage.constant(
                vng::Vec4{0.25F, 0.5F, 0.75F, 1.0F});
            const auto per_component = vng::dsl::select(
                color < threshold,
                color,
                threshold);
            const auto all_equal = vng::dsl::all(color == threshold);
            const auto any_different = vng::dsl::any(color != threshold);
            const auto scalar_choice = vng::dsl::select(
                all_equal,
                threshold,
                per_component);
            const auto result = vng::dsl::select(
                any_different,
                scalar_choice,
                color);
            return vng::dsl::make<FragmentOut>(
                vng::dsl::field(vng::shader::Color<0>{}, result));
        });
    if (!fragment) {
        return std::unexpected(std::move(fragment.error()));
    }
    return vng::shader::link(std::move(*vertex), std::move(*fragment));
}

} // namespace

TEST_CASE("OpenGL shares context state across Device facades and compiles advanced DSL output",
          "[opengl][integration][shader-driver]")
{
    auto window = vng::test::create_hidden_opengl_window(
        8, 8, "vng hidden shader driver test");
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
    {
        auto second = vng::opengl::Device::create(*access);
        INFO((second ? std::string{} : describe(second.error())));
        REQUIRE(second.has_value());
        (void)second->take_debug_messages();

        auto marker = device->debug_marker("vng shared Device callback probe");
        INFO((marker ? std::string{} : describe(marker.error())));
        REQUIRE(marker.has_value());
        CHECK(contains_message(
            second->take_debug_messages(),
            "vng shared Device callback probe"));
    }

    // Destroying one facade must not uninstall the context callback still
    // shared by the surviving facade.
    (void)device->take_debug_messages();
    auto marker = device->debug_marker("vng surviving Device callback probe");
    INFO((marker ? std::string{} : describe(marker.error())));
    REQUIRE(marker.has_value());
    CHECK(contains_message(
        device->take_debug_messages(),
        "vng surviving Device callback probe"));

    auto program = make_driver_exercise_program();
    INFO((program ? std::string{} : describe(program.error())));
    REQUIRE(program.has_value());
    auto generated = vng::glsl::emit(*program);
    INFO((generated ? std::string{} : describe(generated.error())));
    REQUIRE(generated.has_value());
    CHECK(generated->vertex.source.find("mat3(") != std::string::npos);
    CHECK(generated->vertex.source.find("mat4(") != std::string::npos);
    CHECK(generated->fragment.source.find("lessThan(") != std::string::npos);
    CHECK(generated->fragment.source.find("equal(") != std::string::npos);
    CHECK(generated->fragment.source.find("notEqual(") != std::string::npos);
    CHECK(generated->fragment.source.find("mix(") != std::string::npos);

    auto vertex_shader = vng::opengl::Shader::compile(
        *device,
        vng::opengl::from_glsl(generated->vertex));
    INFO((vertex_shader ? std::string{} : describe(vertex_shader.error())));
    REQUIRE(vertex_shader.has_value());
    auto fragment_shader = vng::opengl::Shader::compile(
        *device,
        vng::opengl::from_glsl(generated->fragment));
    INFO((fragment_shader ? std::string{} : describe(fragment_shader.error())));
    REQUIRE(fragment_shader.has_value());
    auto linked = vng::opengl::Program::link_graphics(
        *device,
        *vertex_shader,
        *fragment_shader);
    INFO((linked ? std::string{} : describe(linked.error())));
    REQUIRE(linked.has_value());
}
