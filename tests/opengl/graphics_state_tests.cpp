#include <vng/gfx/gfx.hpp>
#include <vng/opengl/opengl.hpp>
#include <vng/render/render.hpp>
#include <vng/shader/shader.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <thread>

#include "../support/glfw_opengl.hpp"

TEST_CASE("Graphics state snapshots roundtrip all managed settings and reject invalid values atomically",
          "[opengl][graphics-state][snapshot]")
{
    auto window = vng::test::create_hidden_opengl_window(16, 16, "graphics snapshots");
    if (!window) {
        std::cerr << window.error().message << '\n';
        std::exit(77);
    }
    auto access = window->make_current();
    REQUIRE(access);
    auto device = vng::opengl::Device::create(*access);
    REQUIRE(device);
    auto frame = vng::render::begin_frame(*device, vng::render::FrameDesc{
        .extent = {16, 16}, .color_encoding = vng::render::ColorEncoding::linear,
        .clear_color = std::nullopt, .clear_depth = std::nullopt,
    });
    REQUIRE(frame);
    auto commands = frame->commands();
    auto graphics = commands.graphics_state();
    auto defaults = graphics.snapshot();
    REQUIRE(defaults);
    CHECK(*defaults == vng::opengl::GraphicsStateSnapshot{});

    const vng::opengl::GraphicsStateSnapshot custom{
        .depth = {.test = true, .write = true,
                  .compare = vng::render::DepthCompare::greater_equal},
        .cull = vng::render::CullMode::front,
        .front_face = vng::render::FrontFace::clockwise,
        .blend = vng::render::BlendMode::premultiplied_alpha,
        .polygon = vng::opengl::PolygonMode::line,
    };
    REQUIRE(graphics.set(custom));
    auto sibling_commands = frame->commands();
    auto sibling = sibling_commands.graphics_state();
    auto saved = sibling.snapshot();
    REQUIRE(saved);
    CHECK(*saved == custom);
    REQUIRE(sibling.set(*defaults));
    REQUIRE(graphics.set(*saved));
    auto restored = graphics.snapshot();
    REQUIRE(restored);
    CHECK(*restored == custom);

    using GetInteger = void (*)(unsigned, int*);
    using IsEnabled = unsigned char (*)(unsigned);
    auto get = reinterpret_cast<GetInteger>(access->resolve("glGetIntegerv"));
    auto enabled = reinterpret_cast<IsEnabled>(access->resolve("glIsEnabled"));
    REQUIRE(get); REQUIRE(enabled);
    const auto native_state = [&] {
        std::array<int, 8> values{};
        values[0] = enabled(0x0B71); // DEPTH_TEST
        get(0x0B72, &values[1]); // DEPTH_WRITEMASK
        get(0x0B74, &values[2]); // DEPTH_FUNC
        values[3] = enabled(0x0B44); // CULL_FACE
        get(0x0B45, &values[4]); // CULL_FACE_MODE
        get(0x0B46, &values[5]); // FRONT_FACE
        values[6] = enabled(0x0BE2); // BLEND, attachment zero
        // Core uses a shared front/back mode; allow a driver to return a pair.
        std::array<int, 2> polygon{};
        get(0x0B40, polygon.data());
        values[7] = polygon[0];
        return values;
    };
    const auto before = native_state();
    CHECK(before == std::array<int, 8>{
        1, 1, 0x0206, 1, 0x0404, 0x0900, 1, 0x1B01});

    // Each malformed snapshot also differs in valid fields; rejecting it
    // must not partially apply those fields before discovering the bad enum.
    const auto reject = [&](vng::opengl::GraphicsStateSnapshot value) {
        auto result = graphics.set(value);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == vng::opengl::ErrorCode::invalid_argument);
        auto unchanged = graphics.snapshot();
        REQUIRE(unchanged);
        CHECK(*unchanged == custom);
        CHECK(native_state() == before);
    };
    auto invalid = *defaults;
    invalid.depth.compare = static_cast<vng::render::DepthCompare>(255);
    reject(invalid);
    invalid = *defaults;
    invalid.cull = static_cast<vng::render::CullMode>(255);
    reject(invalid);
    invalid = *defaults;
    invalid.front_face = static_cast<vng::render::FrontFace>(255);
    reject(invalid);
    invalid = *defaults;
    invalid.blend = static_cast<vng::render::BlendMode>(255);
    reject(invalid);
    invalid = *defaults;
    invalid.polygon = static_cast<vng::opengl::PolygonMode>(255);
    reject(invalid);

    bool wrong_thread_rejected = false;
    std::thread worker([&] {
        wrong_thread_rejected = !graphics.snapshot() && !graphics.set(custom);
    });
    worker.join();
    CHECK(wrong_thread_rejected);

    window->release_current();
    auto detached = graphics.snapshot();
    REQUIRE_FALSE(detached);
    CHECK(detached.error().code == vng::opengl::ErrorCode::context_not_current);
    CHECK_FALSE(graphics.set(custom));
    REQUIRE(window->make_current());
    REQUIRE(frame->end());
    CHECK_FALSE(graphics.snapshot());
    CHECK_FALSE(graphics.set(custom));

    // A new generation starts with defaults, not the prior frame's snapshot.
    auto next = vng::render::begin_frame(*device, vng::render::FrameDesc{
        .extent = {16, 16}, .color_encoding = vng::render::ColorEncoding::linear,
        .clear_color = std::nullopt, .clear_depth = std::nullopt,
    });
    REQUIRE(next);
    auto next_commands = next->commands();
    auto next_graphics = next_commands.graphics_state();
    auto next_defaults = next_graphics.snapshot();
    REQUIRE(next_defaults);
    CHECK(*next_defaults == *defaults);
    CHECK_FALSE(graphics.snapshot());
}


namespace {
struct Position : vng::gfx::Semantic<vng::Vec3> {};
struct Color : vng::gfx::Semantic<vng::Vec4> {};
using Vertex = vng::gfx::Record<Position, Color>;
using Input = vng::shader::VertexInputs<Position, Color>;
using Varyings = vng::shader::VertexOutputs<
    vng::shader::ClipPosition, vng::shader::smooth<Color>>;
using FragmentInput = vng::shader::FragmentInputs<vng::shader::smooth<Color>>;
using Output = vng::shader::FragmentOutputs<vng::shader::Color<0>>;

auto make_program(bool camera = false, bool green = false)
{
    auto vertex = vng::shader::vertex<Input, Varyings>([camera](auto& stage) {
        auto position = stage.input(Position{});
        return stage.output(
            vng::dsl::field<vng::shader::ClipPosition>(camera
                ? stage.camera().project(position)
                : vng::dsl::vec4(position, 1.0F)),
            vng::dsl::field<Color>(stage.input(Color{})));
    });
    REQUIRE(vertex);
    auto fragment = vng::shader::fragment<FragmentInput, Output>([green](auto& stage) {
        return stage.output(vng::dsl::field<vng::shader::Color<0>>(
            green ? stage.constant(vng::Vec4{0, 1, 0, 1}) : stage.input(Color{})));
    });
    REQUIRE(fragment);
    return vng::shader::link(std::move(*vertex), std::move(*fragment));
}

auto triangle(float depth, vng::Vec4 color)
{
    vng::gfx::Mesh<Vertex> mesh(3);
    mesh.vertices()[0].set(Position{}, {-0.8F, -0.8F, depth});
    mesh.vertices()[1].set(Position{}, {0.8F, -0.8F, depth});
    mesh.vertices()[2].set(Position{}, {0.0F, 0.8F, depth});
    for (auto& vertex : mesh.vertices()) vertex.set(Color{}, color);
    mesh.add_face(0, 1, 2);
    return mesh;
}

template<class Context>
concept RunsTemporary = requires(Context& context, vng::opengl::Program&& program) {
    context.run(std::move(program));
};
template<class Context>
concept StateFromTemporary = requires(Context&& context) {
    std::move(context).graphics_state();
};
struct ForeignSetting {};
static_assert(vng::render::SupportsGraphicsSetting<
    vng::opengl::GraphicsState, vng::render::DepthTest>);
static_assert(vng::render::SupportsGraphicsSetting<
    vng::opengl::GraphicsState, vng::render::BlendMode>);
static_assert(vng::render::SupportsGraphicsSetting<
    vng::opengl::GraphicsState, vng::opengl::PolygonMode>);
static_assert(!vng::render::SupportsGraphicsSetting<
    vng::opengl::GraphicsState, ForeignSetting>);
static_assert(!vng::render::SupportsGraphicsSetting<
    vng::opengl::GraphicsState, vng::opengl::DepthCompare>);
static_assert(!RunsTemporary<vng::opengl::Commands>);
static_assert(!StateFromTemporary<vng::opengl::Commands>);
} // namespace

TEST_CASE("Typed graphics settings affect each draw independently of program selection",
          "[opengl][graphics-state][integration]")
{
    auto window = vng::test::create_hidden_opengl_window(32, 32, "state tests");
    if (!window) {
        std::cerr << window.error().message << '\n';
        std::exit(77);
    }
    auto access = window->make_current();
    REQUIRE(access);
    auto device = vng::opengl::Device::create(*access);
    REQUIRE(device);
    auto ir = make_program();
    REQUIRE(ir);
    auto program = vng::render::compile_program(*device, *ir);
    auto other = vng::render::compile_program(*device, *ir);
    REQUIRE(program);
    REQUIRE(other);
    REQUIRE(program->generated_source());
    CHECK(program->generated_source()->vertex.source
          == other->generated_source()->vertex.source);
    auto near_mesh = vng::opengl::upload_mesh(
        *device, triangle(-0.5F, {1, 0, 0, 1}));
    auto far_mesh = vng::opengl::upload_mesh(
        *device, triangle(0.5F, {0, 1, 0, 1}));
    auto middle_mesh = vng::opengl::upload_mesh(
        *device, triangle(0.0F, {0, 0, 1, 1}));
    REQUIRE(near_mesh);
    REQUIRE(far_mesh);
    REQUIRE(middle_mesh);
    REQUIRE(near_mesh->prepare_vertex_input(*device, *program));

    using GetInteger = void (*)(unsigned, int*);
    using IsEnabled = unsigned char (*)(unsigned);
    using ReadPixels = void (*)(int, int, int, int, unsigned, unsigned, void*);
    using Disable = void (*)(unsigned);
    using UseProgram = void (*)(unsigned);
    auto get = reinterpret_cast<GetInteger>(access->resolve("glGetIntegerv"));
    auto enabled = reinterpret_cast<IsEnabled>(access->resolve("glIsEnabled"));
    auto read = reinterpret_cast<ReadPixels>(access->resolve("glReadPixels"));
    auto disable = reinterpret_cast<Disable>(access->resolve("glDisable"));
    auto use = reinterpret_cast<UseProgram>(access->resolve("glUseProgram"));
    REQUIRE(get); REQUIRE(enabled); REQUIRE(read); REQUIRE(disable); REQUIRE(use);
    auto pixel = [&] {
        std::array<unsigned char, 4> result{};
        read(16, 16, 1, 1, 0x1908, 0x1401, result.data()); // RGBA, UNSIGNED_BYTE
        return result;
    };
    const vng::render::FrameDesc frame_desc{
        .extent = {32, 32},
        .color_encoding = vng::render::ColorEncoding::linear,
        .clear_color = std::array{0.0F, 0.0F, 0.0F, 1.0F},
        .clear_depth = 1.0F,
    };
    auto frame = vng::render::begin_frame(*device, frame_desc);
    REQUIRE(frame);
    auto context = frame->render_context();
    auto graphics = context.graphics_state();
    auto second_handle = context.graphics_state();

    // Set state before selecting a program, with independently controlled test
    // and write flags. Both handles see the same live state.
    REQUIRE(graphics.set(vng::render::DepthTest{true}));
    REQUIRE(second_handle.set(vng::render::DepthWrite{true}));
    REQUIRE(context.run(*program));
    REQUIRE(context.draw(*near_mesh));
    REQUIRE(context.draw(*far_mesh));
    CHECK(pixel() == std::array<unsigned char, 4>{255, 0, 0, 255});

    REQUIRE(graphics.set(vng::render::DepthTest{false}));
    REQUIRE(context.run(*other));
    REQUIRE(context.bind(*program));
    REQUIRE(context.draw(*far_mesh));
    CHECK(pixel() == std::array<unsigned char, 4>{0, 255, 0, 255});

    // Re-enabling testing retains the near red triangle's depth. Disabling
    // testing did not erase or write depth, even with its write flag enabled.
    REQUIRE(second_handle.set(vng::render::DepthTest{true}));
    REQUIRE(context.draw(*middle_mesh));
    CHECK(pixel() == std::array<unsigned char, 4>{0, 255, 0, 255});
    int value = 0;
    get(0x0B72, &value); // DEPTH_WRITEMASK
    CHECK(value == 1);

    // Comparison and write changes preserve testing. Turning writes off lets
    // two progressively farther triangles pass against the unchanged clear.
    REQUIRE(frame->end());
    auto successor = vng::render::begin_frame(*device, frame_desc);
    REQUIRE(successor);
    CHECK_FALSE(graphics.set(vng::render::DepthTest{false}));
    context = successor->render_context();
    graphics = context.graphics_state();
    REQUIRE(graphics.set(vng::render::DepthTest{true}));
    REQUIRE(graphics.set(vng::render::DepthWrite{false}));
    REQUIRE(graphics.set(vng::render::DepthCompare::less));
    REQUIRE(context.run(*program));
    REQUIRE(context.draw(*near_mesh));
    REQUIRE(context.draw(*far_mesh));
    CHECK(pixel() == std::array<unsigned char, 4>{0, 255, 0, 255});
    CHECK(enabled(0x0B71) != 0); // DEPTH_TEST
    get(0x0B72, &value);
    CHECK(value == 0);

    REQUIRE(graphics.set(vng::render::FrontFace::clockwise));
    REQUIRE(graphics.set(vng::render::CullMode::back));
    REQUIRE(graphics.set(vng::opengl::PolygonMode::line));
    REQUIRE(context.run(*other));
    get(0x0B46, &value); // FRONT_FACE
    CHECK(value == 0x0900); // CW
    std::array<int, 2> modes{};
    get(0x0B40, modes.data()); // POLYGON_MODE returns two values
    CHECK(modes[0] == 0x1B01); // LINE
    CHECK(enabled(0x0B44) != 0); // CULL_FACE

    // Invalid values must leave the selected state intact.
    CHECK_FALSE(graphics.set(static_cast<vng::render::DepthCompare>(-1)));
    CHECK_FALSE(graphics.set(static_cast<vng::render::CullMode>(99)));
    CHECK_FALSE(graphics.set(static_cast<vng::render::FrontFace>(99)));
    CHECK_FALSE(graphics.set(static_cast<vng::opengl::PolygonMode>(99)));
    get(0x0B40, modes.data());
    CHECK(modes[0] == 0x1B01);

    // Backend compatibility is also checked against the concrete device. A
    // valid GL setting cannot run while another native context is current.
    {
        auto foreign_window = vng::test::create_hidden_opengl_window(8, 8, "foreign state");
        REQUIRE(foreign_window);
        auto foreign_access = foreign_window->make_current();
        REQUIRE(foreign_access);
        auto foreign_device = vng::opengl::Device::create(*foreign_access);
        REQUIRE(foreign_device);
        auto foreign_program = vng::render::compile_program(*foreign_device, *ir);
        REQUIRE(foreign_program);
        auto wrong_context = graphics.set(vng::render::DepthTest{false});
        REQUIRE_FALSE(wrong_context);
        CHECK(wrong_context.error().code == vng::opengl::ErrorCode::context_not_current);
        REQUIRE(window->make_current());
        auto wrong_program = context.run(*foreign_program);
        REQUIRE_FALSE(wrong_program);
        CHECK(wrong_program.error().code == vng::opengl::ErrorCode::incompatible_device);
        REQUIRE(foreign_window->make_current());
    }
    REQUIRE(window->make_current());

    // Reasserting raw-GL disturbances is explicit, including desired state.
    disable(0x0B71);
    use(0);
    REQUIRE(context.restore());
    CHECK(enabled(0x0B71) != 0);
    get(0x8B8D, &value); // CURRENT_PROGRAM
    CHECK(static_cast<unsigned>(value) == other->native_handle());
    get(0x0B40, modes.data());
    CHECK(modes[0] == 0x1B01);

    std::optional<vng::opengl::Diagnostic> thread_error;
    std::thread worker([&] {
        auto result = graphics.set(vng::render::DepthTest{false});
        if (!result) thread_error = result.error();
    });
    worker.join();
    REQUIRE(thread_error);
    CHECK(thread_error->code == vng::opengl::ErrorCode::wrong_thread);

    // Every borrow shares the frame's state. Moving or replacing a Commands
    // handle cannot revoke state handles or reset their desired settings.
    auto moved = std::move(context);
    REQUIRE(graphics.set(vng::render::CullMode::none));
    context = successor->render_context();
    CHECK(moved.active());
    REQUIRE(graphics.set(vng::render::DepthTest{false}));
    CHECK(enabled(0x0B71) == 0);
    REQUIRE(graphics.set(vng::render::DepthTest{true}));
    auto fresh = context.graphics_state();
    REQUIRE(fresh.set(vng::render::DepthWrite{true}));
    CHECK(enabled(0x0B71) != 0); // borrowing does not reset the prior choice
    REQUIRE(successor->end());
    CHECK_FALSE(fresh.set(vng::render::DepthTest{true}));

    // The next frame starts with fresh defaults; a state handle remains usable
    // after its Commands handle dies, until this new frame actually ends.
    auto last_frame = vng::render::begin_frame(*device, frame_desc);
    REQUIRE(last_frame);
    std::optional<vng::opengl::GraphicsState> escaped;
    {
        auto scoped = last_frame->render_context();
        escaped.emplace(scoped.graphics_state());
        CHECK_FALSE(scoped.draw(*near_mesh)); // no program selection from the old frame
        REQUIRE(escaped->set(vng::render::DepthWrite{true}));
        CHECK(enabled(0x0B71) == 0);
        REQUIRE(escaped->set(vng::render::DepthTest{true}));
    }
    REQUIRE(escaped->set(vng::render::DepthTest{false}));
    REQUIRE(last_frame->end());
    CHECK_FALSE(escaped->set(vng::render::DepthTest{true}));
}

TEST_CASE("Program-only rendering retains camera metadata and managed view readiness",
          "[opengl][graphics-state][camera]")
{
    auto window = vng::test::create_hidden_opengl_window(16, 16, "state camera");
    if (!window) std::exit(77);
    auto access = window->make_current();
    REQUIRE(access);
    auto device = vng::opengl::Device::create(*access);
    REQUIRE(device);
    auto ir = make_program(true);
    REQUIRE(ir);
    auto program = vng::render::compile_program(*device, *ir);
    auto other = vng::render::compile_program(*device, *ir);
    REQUIRE(program); REQUIRE(other);
    auto mesh = vng::opengl::upload_mesh(*device, triangle(-2, {1, 0, 0, 1}));
    REQUIRE(mesh);
    auto frame = vng::render::begin_frame(*device, vng::render::FrameDesc{
        .extent = {16, 16}, .color_encoding = vng::render::ColorEncoding::linear,
        .clear_color = std::nullopt, .clear_depth = std::nullopt,
    });
    REQUIRE(frame);
    auto context = frame->render_context();
    REQUIRE(context.run(*program));
    CHECK_FALSE(context.draw(*mesh));
    auto child = frame->commands();
    CHECK(context.active());
    CHECK_FALSE(child.draw(*mesh));
    vng::gfx::Camera camera;
    auto view = vng::render::RenderView::create(camera, {16, 16});
    REQUIRE(view);
    REQUIRE(child.view(*view)); // readiness is shared, not stored per handle
    REQUIRE(context.run(*program)); // same program keeps its uploaded view
    REQUIRE(context.graphics_state().set(vng::render::DepthTest{true}));
    REQUIRE(context.draw(*mesh));
    REQUIRE(child.run(*other));
    CHECK_FALSE(context.draw(*mesh)); // a different program needs its own upload
    REQUIRE(context.view(*view));
    REQUIRE(child.draw(*mesh));
    REQUIRE(context.bind(*other)); // forced binding deliberately resets readiness
    CHECK_FALSE(context.draw(*mesh));
    REQUIRE(frame->end());
}

TEST_CASE("Nested render contexts share program selection and parent run rebinds after child draws",
          "[opengl][graphics-state][commands][nested]")
{
    auto window = vng::test::create_hidden_opengl_window(32, 32, "nested command borrows");
    if (!window) std::exit(77);
    auto access = window->make_current(); REQUIRE(access);
    auto device = vng::opengl::Device::create(*access); REQUIRE(device);
    auto red_ir = make_program(), green_ir = make_program(false, true);
    REQUIRE(red_ir); REQUIRE(green_ir);
    auto red = vng::render::compile_program(*device, *red_ir);
    auto green = vng::render::compile_program(*device, *green_ir);
    REQUIRE(red); REQUIRE(green);
    auto mesh = vng::opengl::upload_mesh(*device, triangle(0, {1, 0, 0, 1}));
    REQUIRE(mesh);
    auto frame = vng::render::begin_frame(*device, vng::render::FrameDesc{
        .extent = {32, 32}, .color_encoding = vng::render::ColorEncoding::linear,
        .clear_color = std::array{0.F, 0.F, 0.F, 1.F}, .clear_depth = 1.F,
    });
    REQUIRE(frame);
    using GetInteger = void (*)(unsigned, int*);
    using ReadPixels = void (*)(int, int, int, int, unsigned, unsigned, void*);
    using IsEnabled = unsigned char (*)(unsigned);
    const auto get = reinterpret_cast<GetInteger>(access->resolve("glGetIntegerv"));
    const auto read = reinterpret_cast<ReadPixels>(access->resolve("glReadPixels"));
    const auto enabled = reinterpret_cast<IsEnabled>(access->resolve("glIsEnabled"));
    REQUIRE(get); REQUIRE(read); REQUIRE(enabled);
    const auto selected = [&] {
        int native{}; get(0x8B8D /* CURRENT_PROGRAM */, &native); return static_cast<unsigned>(native);
    };
    const auto pixel = [&] {
        std::array<unsigned char, 4> result{};
        read(16, 16, 1, 1, 0x1908, 0x1401, result.data()); return result;
    };
    auto parent = frame->render_context();
    auto parent_graphics = parent.graphics_state();
    REQUIRE(parent_graphics.set(vng::render::DepthTest{false}));
    REQUIRE(parent.run(*red)); REQUIRE(parent.draw(*mesh));
    CHECK(pixel() == std::array<unsigned char, 4>{255, 0, 0, 255});
    std::optional<vng::opengl::GraphicsState> child_graphics;
    {
        auto child = frame->commands();
        CHECK(parent.active()); CHECK(child.active());
        CHECK(selected() == red->native_handle());
        REQUIRE(child.draw(*mesh)); // inherits the parent's selected program
        REQUIRE(child.run(*green)); REQUIRE(child.draw(*mesh));
        CHECK(pixel() == std::array<unsigned char, 4>{0, 255, 0, 255});
        child_graphics.emplace(child.graphics_state());
        REQUIRE(child_graphics->set(vng::render::DepthWrite{false}));
        REQUIRE(parent.draw(*mesh)); // current shared program is the child's
        CHECK(pixel() == std::array<unsigned char, 4>{0, 255, 0, 255});
    }
    CHECK(parent.active());
    REQUIRE(child_graphics->set(vng::render::DepthTest{false}));
    REQUIRE(parent.run(*red)); // must not short-circuit on a per-handle cache
    CHECK(selected() == red->native_handle());
    REQUIRE(parent.draw(*mesh));
    CHECK(pixel() == std::array<unsigned char, 4>{255, 0, 0, 255});
    int write{}; get(0x0B72 /* DEPTH_WRITEMASK */, &write); CHECK(write == 0);
    CHECK(enabled(0x0B71 /* DEPTH_TEST */) == 0);

    auto moved_frame = std::move(*frame);
    CHECK(parent.active());
    REQUIRE(child_graphics->set(vng::render::DepthWrite{true}));
    REQUIRE(parent.draw(*mesh));
    REQUIRE(moved_frame.end());
    CHECK_FALSE(parent.active());
    CHECK_FALSE(child_graphics->set(vng::render::DepthTest{true}));
}

TEST_CASE("Selected program moves and destruction clear shared selection without revoking frame handles",
          "[opengl][graphics-state][commands][lifetime]")
{
    auto window = vng::test::create_hidden_opengl_window(16, 16, "selected program lifetime");
    if (!window) std::exit(77);
    auto access = window->make_current(); REQUIRE(access);
    auto device = vng::opengl::Device::create(*access); REQUIRE(device);
    auto ir = make_program(); REQUIRE(ir);
    auto original = vng::render::compile_program(*device, *ir); REQUIRE(original);
    auto fallback = vng::render::compile_program(*device, *ir); REQUIRE(fallback);
    auto mesh = vng::opengl::upload_mesh(*device, triangle(0, {1, 0, 0, 1})); REQUIRE(mesh);
    auto frame = vng::render::begin_frame(*device, vng::render::FrameDesc{
        .extent = {16, 16}, .color_encoding = vng::render::ColorEncoding::linear,
        .clear_color = std::nullopt, .clear_depth = std::nullopt,
    });
    REQUIRE(frame);
    auto parent = frame->commands(), child = frame->render_context();
    auto graphics = parent.graphics_state();
    REQUIRE(parent.run(*original)); REQUIRE(child.draw(*mesh));
    auto moved = std::move(*original);
    CHECK(parent.active()); CHECK(child.active());
    CHECK_FALSE(parent.draw(*mesh)); CHECK_FALSE(child.draw(*mesh));
    REQUIRE(child.run(moved)); REQUIRE(parent.draw(*mesh));
    {
        auto replacement = vng::render::compile_program(*device, *ir); REQUIRE(replacement);
        *replacement = std::move(moved);
        CHECK_FALSE(parent.draw(*mesh)); CHECK_FALSE(child.draw(*mesh));
        REQUIRE(parent.run(*replacement)); REQUIRE(child.draw(*mesh));
        auto unselected = vng::render::compile_program(*device, *ir); REQUIRE(unselected);
        *replacement = std::move(*unselected); // moving over the selected destination also clears it
        CHECK_FALSE(parent.draw(*mesh)); CHECK_FALSE(child.draw(*mesh));
        REQUIRE(child.run(*replacement)); REQUIRE(parent.draw(*mesh));
    }
    // The selected Program has died, but the frame and its state still exist.
    CHECK(parent.active()); CHECK(child.active());
    CHECK_FALSE(parent.draw(*mesh)); CHECK_FALSE(child.draw(*mesh));
    REQUIRE(graphics.set(vng::render::DepthTest{false}));
    REQUIRE(child.run(*fallback)); REQUIRE(parent.draw(*mesh));
    REQUIRE(frame->end());
    CHECK_FALSE(parent.active()); CHECK_FALSE(child.active());
}

TEST_CASE("Blend settings establish color and alpha equations and survive invalid updates",
          "[opengl][graphics-state][blend]")
{
    auto window = vng::test::create_hidden_opengl_window(16, 16, "blend settings");
    if (!window) std::exit(77);
    auto access = window->make_current();
    REQUIRE(access);
    auto device = vng::opengl::Device::create(*access);
    REQUIRE(device);
    using GetIndexed = void (*)(unsigned, unsigned, int*);
    using IsEnabledIndexed = unsigned char (*)(unsigned, unsigned);
    const auto get = reinterpret_cast<GetIndexed>(access->resolve("glGetIntegeri_v"));
    const auto enabled = reinterpret_cast<IsEnabledIndexed>(access->resolve("glIsEnabledi"));
    REQUIRE(get);
    REQUIRE(enabled);
    const auto blend_state = [&] {
        // Enable, source RGB, destination RGB, source alpha, destination alpha,
        // RGB equation, alpha equation.
        std::array<int, 7> result{enabled(0x0BE2 /* GL_BLEND */, 0)};
        get(0x80C9 /* GL_BLEND_SRC_RGB */, 0, &result[1]);
        get(0x80C8 /* GL_BLEND_DST_RGB */, 0, &result[2]);
        get(0x80CB /* GL_BLEND_SRC_ALPHA */, 0, &result[3]);
        get(0x80CA /* GL_BLEND_DST_ALPHA */, 0, &result[4]);
        get(0x8009 /* GL_BLEND_EQUATION_RGB */, 0, &result[5]);
        get(0x883D /* GL_BLEND_EQUATION_ALPHA */, 0, &result[6]);
        return result;
    };
    constexpr std::array<int, 7> disabled{0, 1, 0, 1, 0, 0x8006, 0x8006};
    constexpr std::array<int, 7> straight{1, 0x0302, 0x0303, 1, 0x0303, 0x8006, 0x8006};
    constexpr std::array<int, 7> premultiplied{1, 1, 0x0303, 1, 0x0303, 0x8006, 0x8006};
    constexpr std::array<int, 7> additive{1, 1, 1, 0, 1, 0x8006, 0x8006};
    auto frame = vng::render::begin_frame(*device, vng::render::FrameDesc{
        .extent = {16, 16}, .color_encoding = vng::render::ColorEncoding::linear,
        .clear_color = std::nullopt, .clear_depth = std::nullopt,
    });
    REQUIRE(frame);
    auto context = frame->render_context();
    auto graphics = context.graphics_state();
    REQUIRE(graphics.set(vng::render::BlendMode::disabled));
    CHECK(blend_state() == disabled);
    REQUIRE(graphics.set(vng::render::BlendMode::additive));
    CHECK(blend_state() == additive);
    REQUIRE(graphics.set(vng::render::BlendMode::straight_alpha));
    CHECK(blend_state() == straight);
    // Alpha must use ONE even for straight RGB: squaring source alpha is wrong.
    REQUIRE(graphics.set(vng::render::BlendMode::premultiplied_alpha));
    CHECK(blend_state() == premultiplied);
    REQUIRE(graphics.set(vng::render::BlendMode::premultiplied_alpha));
    CHECK(blend_state() == premultiplied);
    auto invalid = graphics.set(static_cast<vng::render::BlendMode>(99));
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == vng::opengl::ErrorCode::invalid_argument);
    CHECK(blend_state() == premultiplied);

    auto ir = make_program();
    REQUIRE(ir);
    auto program = vng::render::compile_program(*device, *ir);
    REQUIRE(program);
    REQUIRE(context.run(*program));
    REQUIRE(context.bind(*program));
    CHECK(blend_state() == premultiplied);

    auto following = frame->render_context();
    CHECK(context.active());
    REQUIRE(graphics.set(vng::render::BlendMode::straight_alpha));
    REQUIRE(following.graphics_state().set(vng::render::DepthTest{false}));
    CHECK(blend_state() == straight);
}

TEST_CASE("Render state scopes restore indexed blend factors and equations",
          "[opengl][graphics-state][blend][state-scope]")
{
    auto window = vng::test::create_hidden_opengl_window(16, 16, "blend state scope");
    if (!window) std::exit(77);
    auto access = window->make_current();
    REQUIRE(access);
    auto device = vng::opengl::Device::create(*access);
    REQUIRE(device);
    using GetIndexed = void (*)(unsigned, unsigned, int*);
    using IsEnabledIndexed = unsigned char (*)(unsigned, unsigned);
    using EnableIndexed = void (*)(unsigned, unsigned);
    using BlendFunction = void (*)(unsigned, unsigned, unsigned, unsigned, unsigned);
    using BlendEquation = void (*)(unsigned, unsigned, unsigned);
    const auto get = reinterpret_cast<GetIndexed>(access->resolve("glGetIntegeri_v"));
    const auto enabled = reinterpret_cast<IsEnabledIndexed>(access->resolve("glIsEnabledi"));
    const auto enable = reinterpret_cast<EnableIndexed>(access->resolve("glEnablei"));
    const auto disable = reinterpret_cast<EnableIndexed>(access->resolve("glDisablei"));
    const auto function = reinterpret_cast<BlendFunction>(access->resolve("glBlendFuncSeparatei"));
    const auto equation = reinterpret_cast<BlendEquation>(access->resolve("glBlendEquationSeparatei"));
    REQUIRE(get);
    REQUIRE(enabled);
    REQUIRE(enable);
    REQUIRE(disable);
    REQUIRE(function);
    REQUIRE(equation);
    const auto blend_state = [&](unsigned index) {
        std::array<int, 7> result{enabled(0x0BE2, index)};
        get(0x80C9, index, &result[1]);
        get(0x80C8, index, &result[2]);
        get(0x80CB, index, &result[3]);
        get(0x80CA, index, &result[4]);
        get(0x8009, index, &result[5]);
        get(0x883D, index, &result[6]);
        return result;
    };
    auto frame = vng::render::begin_frame(*device, vng::render::FrameDesc{
        .extent = {16, 16}, .color_encoding = vng::render::ColorEncoding::linear,
        .clear_color = std::nullopt, .clear_depth = std::nullopt,
    });
    REQUIRE(frame);
    auto context = frame->render_context();
    auto graphics = context.graphics_state();
    REQUIRE(graphics.set(vng::render::DepthTest{false}));
    // Seed distinct state in each draw buffer. Separate RGB/alpha equations
    // and nonstandard factors ensure that restoring only enable is inadequate.
    enable(0x0BE2, 0);
    function(0, 0x0306 /* DST_COLOR */, 0x0307 /* ONE_MINUS_DST_COLOR */,
             0x0302 /* SRC_ALPHA */, 0x0305 /* ONE_MINUS_DST_ALPHA */);
    equation(0, 0x800A /* FUNC_SUBTRACT */, 0x800B /* FUNC_REVERSE_SUBTRACT */);
    disable(0x0BE2, 1);
    function(1, 0x0302, 0x0303, 0, 1);
    equation(1, 0x800B, 0x800A);
    const auto before_zero = blend_state(0);
    const auto before_one = blend_state(1);
    const auto exercise_scope = [&](bool explicit_restore) {
        {
            const std::array<vng::u32, 2> buffers{0, 1};
            auto scope = vng::opengl::RenderStateScope::capture(*device, buffers);
            REQUIRE(scope);
            REQUIRE(graphics.set(vng::render::BlendMode::premultiplied_alpha));
            enable(0x0BE2, 1);
            function(1, 1, 0, 1, 0);
            equation(1, 0x8006, 0x8006);
            REQUIRE(blend_state(0) != before_zero);
            REQUIRE(blend_state(1) != before_one);
            if (explicit_restore) {
                REQUIRE(scope->restore());
                CHECK_FALSE(scope->active());
                REQUIRE(scope->restore());
            }
        }
        CHECK(blend_state(0) == before_zero);
        CHECK(blend_state(1) == before_one);
        // The scoped native restore invalidates cached applied state. Even a
        // repeated setting must reapply the command stream's desired blend.
        REQUIRE(graphics.set(vng::render::BlendMode::premultiplied_alpha));
        CHECK(blend_state(0) == std::array<int, 7>{
            1, 1, 0x0303, 1, 0x0303, 0x8006, 0x8006});
    };
    SECTION("explicit restore") { exercise_scope(true); }
    SECTION("scope exit") { exercise_scope(false); }
}
