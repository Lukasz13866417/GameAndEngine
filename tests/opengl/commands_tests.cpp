#include <vng/gfx/gfx.hpp>
#include <vng/opengl/opengl.hpp>
#include <vng/render/render.hpp>
#include <vng/shader/shader.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include "../support/glfw_opengl.hpp"

namespace {

struct CommandPosition : vng::gfx::Semantic<vng::Vec3> {};
struct CommandColor : vng::gfx::Semantic<vng::Vec4> {};

using CommandVertex = vng::gfx::Record<CommandPosition, CommandColor>;
using VertexIn = vng::shader::VertexInputs<CommandPosition, CommandColor>;
using VertexOut = vng::shader::VertexOutputs<
    vng::shader::ClipPosition,
    vng::shader::smooth<CommandColor>>;
using FragmentIn = vng::shader::FragmentInputs<
    vng::shader::smooth<CommandColor>>;
using FragmentOut = vng::shader::FragmentOutputs<
    vng::shader::Color<0>>;

template<class Commands, class Program>
concept BindsProgramRvalue = requires(Commands& commands, Program&& program) {
    commands.bind(static_cast<Program&&>(program));
};

[[noreturn]] void skip_commands_test(const std::string& reason)
{
    std::cerr << "OpenGL commands test skipped: " << reason << '\n';
    std::exit(77);
}

[[nodiscard]] auto camera_program()
{
    auto vertex = vng::shader::vertex<VertexIn, VertexOut>(
        "commands_vertex",
        [](auto& stage) {
            return stage.output(
                vng::dsl::field<vng::shader::ClipPosition>(
                    stage.camera().project(
                        stage.input(CommandPosition{}))),
                vng::dsl::field<CommandColor>(
                    stage.input(CommandColor{})));
        });
    if (!vertex) {
        return vng::shader::Result<vng::shader::GraphicsProgram>{
            std::unexpected(std::move(vertex.error()))};
    }
    auto fragment = vng::shader::fragment<FragmentIn, FragmentOut>(
        "commands_fragment",
        [](auto& stage) {
            return stage.output(
                vng::dsl::field<vng::shader::Color<0>>(
                    stage.input(CommandColor{})));
        });
    if (!fragment) {
        return vng::shader::Result<vng::shader::GraphicsProgram>{
            std::unexpected(std::move(fragment.error()))};
    }
    return vng::shader::link(
        std::move(*vertex), std::move(*fragment));
}

[[nodiscard]] vng::opengl::GraphicsStateSnapshot draw_settings()
{
    return {
        .depth = {
            .test = true,
            .write = true,
            .compare = vng::render::DepthCompare::less,
        },
        .cull = vng::render::CullMode::back,
        .front_face = vng::render::FrontFace::counter_clockwise,
    };
}

} // namespace

TEST_CASE("OpenGL frame commands bind view, dynamic state, and mesh draws",
          "[opengl][commands][integration]")
{
    STATIC_CHECK_FALSE(std::is_copy_constructible_v<vng::opengl::Commands>);
    STATIC_CHECK(std::is_move_constructible_v<vng::opengl::Commands>);
    STATIC_CHECK_FALSE(BindsProgramRvalue<
        vng::opengl::Commands,
        vng::opengl::Program>);

    auto window = vng::test::create_hidden_opengl_window(
        32, 32, "vng frame commands test");
    if (!window) {
        skip_commands_test(window.error().message);
    }
    auto access = window->make_current();
    if (!access) {
        skip_commands_test(access.error().message);
    }
    auto device = vng::opengl::Device::create(*access);
    REQUIRE(device);

    auto ir = camera_program();
    REQUIRE(ir);
    auto program = vng::render::compile_program(*device, *ir);
    REQUIRE(program);

    vng::gfx::Mesh<CommandVertex> source(3);
    source.vertices()[0].set(
        CommandPosition{}, {-0.6F, -0.6F, -2.0F});
    source.vertices()[1].set(
        CommandPosition{}, {0.6F, -0.6F, -2.0F});
    source.vertices()[2].set(
        CommandPosition{}, {0.0F, 0.6F, -2.0F});
    for (auto& vertex : source.vertices()) {
        vertex.set(CommandColor{}, {1.0F, 0.0F, 0.0F, 1.0F});
    }
    source.add_face(0, 1, 2);
    auto mesh = vng::opengl::upload_mesh(*device, source);
    REQUIRE(mesh);

    auto frame = vng::render::begin_frame(
        *device,
        vng::render::FrameDesc{
            .extent = {32, 32},
            .color_encoding = vng::render::ColorEncoding::linear,
            .clear_color = std::array{0.0F, 0.0F, 0.0F, 1.0F},
            .clear_depth = 1.0F,
        });
    REQUIRE(frame);
    auto commands = frame->commands();
    CHECK(commands.active());

    auto missing_program = commands.draw(*mesh);
    REQUIRE_FALSE(missing_program);
    CHECK(missing_program.error().message.find("program")
          != std::string::npos);

    // A valid program from another current context is rejected before any
    // state from it can be installed in this frame's context.
    {
        auto other_window = vng::test::create_hidden_opengl_window(
            8, 8, "vng foreign frame commands context");
        REQUIRE(other_window);
        auto other_access = other_window->make_current();
        REQUIRE(other_access);
        auto other_device = vng::opengl::Device::create(*other_access);
        REQUIRE(other_device);
        auto foreign_program = vng::render::compile_program(
            *other_device, *ir);
        REQUIRE(foreign_program);

        REQUIRE(window->make_current());
        auto foreign = commands.bind(*foreign_program);
        REQUIRE_FALSE(foreign);
        CHECK(foreign.error().code
              == vng::opengl::ErrorCode::incompatible_device);

        // Destroy the foreign resources with their owning context current.
        REQUIRE(other_window->make_current());
    }
    REQUIRE(window->make_current());

    auto graphics = commands.graphics_state();
    REQUIRE(graphics.set(draw_settings()));
    REQUIRE(commands.bind(*program));

    // The command stream uses generated parameter metadata to prevent a
    // camera-reading shader from accidentally drawing its default matrix.
    auto missing_view = commands.draw(*mesh);
    REQUIRE_FALSE(missing_view);
    CHECK(missing_view.error().message.find("requires view")
          != std::string::npos);
    auto absent_camera = commands.view(
        vng::render::RenderView::without_camera({32, 32}));
    REQUIRE_FALSE(absent_camera);
    CHECK(absent_camera.error().message.find("requires a camera")
          != std::string::npos);
    auto wrong_extent = commands.view(
        vng::render::RenderView::without_camera({31, 32}));
    REQUIRE_FALSE(wrong_extent);
    CHECK(wrong_extent.error().message.find("matching frame and view")
          != std::string::npos);

    vng::gfx::Camera camera;
    auto view = vng::render::RenderView::create(camera, {32, 32});
    REQUIRE(view);
    REQUIRE(commands.view(*view));

    // A failed replacement view poisons readiness instead of silently
    // retaining the previous camera snapshot.
    auto stale_extent = commands.view(
        vng::render::RenderView::without_camera({31, 32}));
    REQUIRE_FALSE(stale_extent);
    auto stale_view_draw = commands.draw(*mesh);
    REQUIRE_FALSE(stale_view_draw);
    CHECK(stale_view_draw.error().message.find("requires view")
          != std::string::npos);
    REQUIRE(commands.view(*view));

    using IsEnabled = std::uint8_t (*)(std::uint32_t);
    using GetInteger = void (*)(std::uint32_t, std::int32_t*);
    using ReadPixels = void (*)(
        std::int32_t,
        std::int32_t,
        std::int32_t,
        std::int32_t,
        std::uint32_t,
        std::uint32_t,
        void*);
    const auto is_enabled = reinterpret_cast<IsEnabled>(
        access->resolve("glIsEnabled"));
    const auto get_integer = reinterpret_cast<GetInteger>(
        access->resolve("glGetIntegerv"));
    const auto read_pixels = reinterpret_cast<ReadPixels>(
        access->resolve("glReadPixels"));
    REQUIRE(is_enabled != nullptr);
    REQUIRE(get_integer != nullptr);
    REQUIRE(read_pixels != nullptr);

    constexpr std::uint32_t gl_depth_test = 0x0B71;
    constexpr std::uint32_t gl_depth_func = 0x0B74;
    constexpr std::uint32_t gl_cull_face = 0x0B44;
    constexpr std::uint32_t gl_cull_face_mode = 0x0B45;
    constexpr std::uint32_t gl_front_face = 0x0B46;
    constexpr std::int32_t gl_gequal = 0x0206;
    constexpr std::int32_t gl_front = 0x0404;
    constexpr std::int32_t gl_cw = 0x0900;

    REQUIRE(graphics.set(vng::render::DepthState{
        .test = true,
        .write = false,
        .compare = vng::render::DepthCompare::greater_equal,
    }));
    REQUIRE(graphics.set(vng::render::CullMode::front));
    REQUIRE(graphics.set(vng::render::FrontFace::clockwise));
    CHECK(is_enabled(gl_depth_test) != 0);
    CHECK(is_enabled(gl_cull_face) != 0);
    std::int32_t integer_state{};
    get_integer(gl_depth_func, &integer_state);
    CHECK(integer_state == gl_gequal);
    get_integer(gl_cull_face_mode, &integer_state);
    CHECK(integer_state == gl_front);
    get_integer(gl_front_face, &integer_state);
    CHECK(integer_state == gl_cw);

    // Applying a snapshot updates draw state independently of the selected
    // program and does not invalidate its already uploaded view.
    auto draw_state = draw_settings();
    draw_state.cull = vng::render::CullMode::none;
    REQUIRE(graphics.set(draw_state));
    CHECK(is_enabled(gl_depth_test) != 0);
    CHECK(is_enabled(gl_cull_face) == 0);
    REQUIRE(commands.draw(*mesh));
    REQUIRE(device->finish());

    constexpr std::uint32_t gl_rgba = 0x1908;
    constexpr std::uint32_t gl_unsigned_byte = 0x1401;
    std::array<std::uint8_t, 4> center{};
    read_pixels(
        16, 16, 1, 1,
        gl_rgba, gl_unsigned_byte, center.data());
    CHECK(center[0] > 245U);
    CHECK(center[1] < 10U);
    CHECK(center[2] < 10U);
    CHECK(center[3] > 245U);

    REQUIRE(frame->end());
    CHECK_FALSE(commands.active());
    auto after_end = graphics.set(vng::render::DepthState{});
    REQUIRE_FALSE(after_end);
    CHECK(after_end.error().message.find("stale or ended")
          != std::string::npos);
}

TEST_CASE("OpenGL frame owns shared command authority across nested borrows and moves",
          "[opengl][commands][lifetime]")
{
    auto window = vng::test::create_hidden_opengl_window(
        8, 8, "vng frame command authority test");
    if (!window) {
        skip_commands_test(window.error().message);
    }
    auto access = window->make_current();
    if (!access) {
        skip_commands_test(access.error().message);
    }
    auto device = vng::opengl::Device::create(*access);
    REQUIRE(device);
    auto frame = vng::render::begin_frame(
        *device,
        vng::render::FrameDesc{
            .extent = {8, 8},
            .color_encoding = vng::render::ColorEncoding::linear,
            .clear_color = std::nullopt,
            .clear_depth = std::nullopt,
        });
    REQUIRE(frame);

    auto first = frame->commands();
    CHECK(first.active());
    auto replacement = frame->commands();
    CHECK(first.active());
    CHECK(replacement.active());

    auto moved_frame = std::move(*frame);
    CHECK(first.active());
    CHECK(replacement.active());
    auto after_move = moved_frame.commands();
    CHECK(after_move.active());

    auto moved_commands = std::move(after_move);
    CHECK_FALSE(after_move.active());
    CHECK(moved_commands.active());
    CHECK(first.active());
    CHECK(replacement.active());
    {
        auto nested = moved_frame.commands();
        CHECK(nested.active());
    }
    CHECK(first.active());
    CHECK(replacement.active());
    CHECK(moved_commands.active());

    REQUIRE(moved_frame.end());
    CHECK_FALSE(first.active());
    CHECK_FALSE(replacement.active());
    CHECK_FALSE(moved_commands.active());

    // Move assignment transfers ownership without changing the frame identity.
    auto successor = vng::render::begin_frame(
        *device,
        vng::render::FrameDesc{
            .extent = {8, 8},
            .color_encoding = vng::render::ColorEncoding::linear,
            .clear_color = std::nullopt,
            .clear_depth = std::nullopt,
        });
    REQUIRE(successor);
    auto before_assignment = successor->commands();
    moved_frame = std::move(*successor);
    CHECK(before_assignment.active());
    CHECK(moved_frame.active());
    CHECK_FALSE(first.active()); // a new frame never resurrects old borrows
    CHECK_FALSE(replacement.active());
    CHECK_FALSE(moved_commands.active());
    REQUIRE(moved_frame.end());
    CHECK_FALSE(before_assignment.active());

    // Borrowed handles cannot keep the native frame open after its owner dies.
    std::optional<vng::opengl::Commands> after_destruction;
    {
        auto scoped = vng::render::begin_frame(
            *device,
            vng::render::FrameDesc{
                .extent = {8, 8},
                .color_encoding = vng::render::ColorEncoding::linear,
                .clear_color = std::nullopt,
                .clear_depth = std::nullopt,
            });
        REQUIRE(scoped);
        after_destruction.emplace(scoped->commands());
        CHECK(after_destruction->active());
    }
    CHECK_FALSE(after_destruction->active());
}
