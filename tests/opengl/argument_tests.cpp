#include <vng/gfx/gfx.hpp>
#include <vng/opengl/opengl.hpp>
#include <vng/render/render.hpp>
#include <vng/render_opengl/program_runtime.hpp>
#include <vng/shader/shader.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <bit>
#include <cstdlib>
#include <iostream>

#include "../support/glfw_opengl.hpp"

namespace {
using namespace vng;
struct Position : gfx::Semantic<Vec2> {};
struct Tint : gfx::Semantic<Vec4> {};
struct Gain : gfx::Semantic<f32> {};
struct Observed : gfx::Semantic<Vec4> {};
using Settings = gfx::Record<Tint, Gain>;
using Vertex = gfx::Record<Position>;
using VI = shader::VertexInputs<Position>;
using VO = shader::VertexOutputs<shader::ClipPosition>;
using FI = shader::FragmentInputs<>;
using FO = shader::FragmentOutputs<shader::Color<0>>;

[[noreturn]] void skip(const std::string& reason)
{
    std::cerr << "OpenGL arguments test skipped: " << reason << '\n';
    std::exit(77);
}

auto make_program()
{
    auto vertex = shader::vertex<VI, VO>([](auto& s, dsl::Float4x4 model) {
        return s.output(dsl::field<shader::ClipPosition>(
            model * dsl::vec4(s.input(Position{}), 0.0F, 1.0F)));
    });
    auto fragment = shader::fragment<FI, FO>(
        [](auto& s, dsl::Expr<Settings> settings, dsl::Float) {
            const auto color = settings.get(Tint{}) * settings.get(Gain{});
            s.observe(Observed{}, color);
            return s.output(dsl::field<shader::Color<0>>(color));
        });
    using Result = shader::Result<shader::TypedGraphicsProgram<Mat4, Settings, f32>>;
    if (!vertex) return Result{std::unexpected(std::move(vertex.error()))};
    if (!fragment) return Result{std::unexpected(std::move(fragment.error()))};
    return shader::link(std::move(*vertex), std::move(*fragment));
}

template<class Program, class... Args>
concept CanRun = requires(opengl::Commands& commands, Program& program, const Args&... values) {
    commands.run(program, values...);
};
}

TEST_CASE("typed arguments update live uniforms and survive enhanced emission",
          "[opengl][arguments][integration]")
{
    using Program = opengl::TypedProgram<Mat4, Settings, f32>;
    STATIC_CHECK(CanRun<Program, Mat4, Settings, f32>);
    STATIC_CHECK_FALSE(CanRun<Program, Mat4, Settings, double>);
    STATIC_CHECK_FALSE(CanRun<Program, Mat4, Settings>);
    STATIC_CHECK_FALSE(CanRun<Program, Vec4, Settings, f32>);
    auto window = test::create_hidden_opengl_window(32, 32, "typed arguments");
    if (!window) skip(window.error().message);
    auto access = window->make_current();
    if (!access) skip(access.error().message);
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    auto neutral = make_program();
    REQUIRE(neutral);
    auto compiled = render::compile_program(*device, *neutral);
    INFO((compiled ? "" : compiled.error().message));
    REQUIRE(compiled);
    auto runtime = render::OpenGLProgramRuntime::create(*device, std::move(*neutral));
    INFO((runtime ? "" : runtime.error().message));
    REQUIRE(runtime);
    gfx::Mesh<Vertex> cpu(3);
    cpu.vertices()[0].set(Position{}, {-0.8F, -0.8F});
    cpu.vertices()[1].set(Position{}, {0.8F, -0.8F});
    cpu.vertices()[2].set(Position{}, {0.0F, 0.8F});
    cpu.add_face(0, 1, 2);
    auto mesh = opengl::upload_mesh(*device, cpu);
    REQUIRE(mesh);
    REQUIRE(mesh->prepare_vertex_input(*device, *compiled));
    REQUIRE(mesh->prepare_vertex_input(*device, runtime->production()));
    auto target = opengl::RenderTarget::create(*device,
        render::TargetDesc{.color = gfx::ImageFormat::rgba8, .depth = true}, {32, 32});
    REQUIRE(target);
    auto frame = render::begin_frame(*device, *target, render::FrameDesc{
        .extent = {32, 32}, .color_encoding = render::ColorEncoding::linear,
        .clear_color = std::array{0.0F, 0.0F, 0.0F, 1.0F}, .clear_depth = 1.0F,
    });
    REQUIRE(frame);
    auto commands = frame->commands();
    REQUIRE(commands.run(compiled->untyped()));
    CHECK_FALSE(commands.draw(*mesh)); // Explicit erasure cannot skip the contract.
    CHECK_FALSE(mesh->draw(*device, compiled->untyped()));
    Settings settings;
    settings.set(Tint{}, {1.0F, 0.0F, 0.0F, 1.0F});
    settings.set(Gain{}, 1.0F);
    auto model = Mat4::identity();
    REQUIRE(commands.run(*compiled, model, settings, 7.0F));
    REQUIRE(commands.depth({.test = false, .write = false}));
    REQUIRE(commands.cull(render::CullMode::none));
    REQUIRE(commands.draw(*mesh));
    auto pixels = target->framebuffer().read_rgba8_pixels(0, 0, 0, 32, 32);
    REQUIRE(pixels);
    CHECK(pixels->at(16U * 32U + 16U).r == 255);
    settings.set(Tint{}, {0.0F, 1.0F, 0.0F, 1.0F});
    const auto handle = compiled->native_handle();
    REQUIRE(commands.run(*compiled, model, settings, 8.0F));
    REQUIRE(commands.draw(*mesh));
    CHECK(compiled->native_handle() == handle);
    pixels = target->framebuffer().read_rgba8_pixels(0, 0, 0, 32, 32);
    REQUIRE(pixels);
    CHECK(pixels->at(16U * 32U + 16U).g == 255);
    CHECK(pixels->at(16U * 32U + 16U).r == 0);

    const shader::ArgumentPack<Mat4, Vec4, f32> wrong{model, Vec4{}, 1.0F};
    const auto wrong_views = wrong.views();
    CHECK_FALSE(compiled->untyped().set_arguments(wrong_views));
    CHECK_FALSE(commands.draw(*mesh)); // Failed update never leaves stale readiness.
    REQUIRE(commands.run(*compiled, model, settings, 8.0F));
    REQUIRE(frame->end());

    const auto view = render::RenderView::without_camera({32, 32});
    auto request = analysis::CaptureRequest::standard().observe(Observed{});
    CHECK_FALSE(runtime->capture(*device, cpu, *mesh, view, opengl::CaptureState{opengl::GraphicsStateSnapshot{}, render::ColorEncoding::linear}, request));
    REQUIRE(runtime->set_arguments(model, settings, 8.0F));
    settings.set(Tint{}, {1.0F, 0.0F, 0.0F, 1.0F}); // Snapshot remains green.
    auto first = runtime->capture(*device, cpu, *mesh, view, opengl::CaptureState{opengl::GraphicsStateSnapshot{}, render::ColorEncoding::linear}, request);
    INFO((first ? "" : first.error().message));
    REQUIRE(first);
    const auto* observed = first->observation(Observed{});
    REQUIRE(observed);
    CHECK(observed->at({16, 16}).y == 1.0F);
    CHECK(observed->at({16, 16}).x == 0.0F);
    REQUIRE(runtime->set_arguments(model, settings, 8.0F));
    auto second = runtime->capture(*device, cpu, *mesh, view, opengl::CaptureState{opengl::GraphicsStateSnapshot{}, render::ColorEncoding::linear}, request);
    REQUIRE(second);
    REQUIRE(second->observation(Observed{}));
    CHECK(second->observation(Observed{})->at({16, 16}).x == 1.0F);
    CHECK(second->observation(Observed{})->at({16, 16}).y == 0.0F);
    REQUIRE(first->metadata().invocation);
    REQUIRE(second->metadata().invocation);
    CHECK(first->metadata().invocation->workload_fingerprint
          != second->metadata().invocation->workload_fingerprint);
}

TEST_CASE("every primitive argument family uploads logical values without native ABI assumptions",
          "[opengl][arguments][integration]")
{
    auto window = test::create_hidden_opengl_window(8, 8, "argument upload types");
    if (!window) skip(window.error().message);
    auto access = window->make_current();
    if (!access) skip(access.error().message);
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    using GetFloat = void (*)(u32, i32, f32*);
    using GetSigned = void (*)(u32, i32, i32*);
    using GetUnsigned = void (*)(u32, i32, u32*);
    auto get_float = reinterpret_cast<GetFloat>(access->resolve("glGetUniformfv"));
    auto get_signed = reinterpret_cast<GetSigned>(access->resolve("glGetUniformiv"));
    auto get_unsigned = reinterpret_cast<GetUnsigned>(access->resolve("glGetUniformuiv"));
    REQUIRE(get_float);
    REQUIRE(get_signed);
    REQUIRE(get_unsigned);

    auto check = [&]<shader::Argument T>(const T& input) {
        auto vertex = shader::vertex<VI, VO>([](auto& s) {
            return s.output(dsl::field<shader::ClipPosition>(
                dsl::vec4(s.input(Position{}), 0.0F, 1.0F)));
        });
        auto fragment = shader::fragment<FI, FO>([](auto& s, dsl::Expr<T> value) {
            auto scalar = [&] {
                if constexpr (std::same_as<T, bool>) {
                    return dsl::select(value, s.constant(1.0F), s.constant(0.0F));
                } else if constexpr (shader::detail::is_matrix_v<T>) {
                    return (value * s.constant(Vec3{0.2F, 0.4F, 0.6F})).x();
                } else if constexpr (is_vector_v<T>) {
                    return dsl::cast<f32>(value.x());
                } else {
                    return dsl::cast<f32>(value);
                }
            }();
            return s.output(dsl::field<shader::Color<0>>(
                dsl::vec4(scalar, 0.0F, 0.0F, 1.0F)));
        });
        REQUIRE(vertex);
        REQUIRE(fragment);
        auto neutral = shader::link(std::move(*vertex), std::move(*fragment));
        REQUIRE(neutral);
        auto program = render::compile_program(*device, *neutral);
        INFO((program ? "" : program.error().message));
        REQUIRE(program);
        REQUIRE(program->set_arguments(input));
        const auto* source = program->generated_source();
        REQUIRE(source);
        REQUIRE(source->parameters.size() == 1);
        REQUIRE(source->parameters[0].leaves.size() == 1);
        const auto& field = source->parameters[0];
        const auto& leaf = field.leaves[0];
        const shader::ArgumentPack<T> pack{input};
        const auto expected = pack.views()[0].words;
        if (leaf.scalar == shader::ScalarKind::f32) {
            std::array<f32, 16> actual{};
            get_float(program->native_handle(), static_cast<i32>(leaf.location), actual.data());
            for (std::size_t index = 0; index < expected.size(); ++index) {
                CHECK(actual[index] == std::bit_cast<f32>(expected[index]));
            }
        } else if (leaf.scalar == shader::ScalarKind::u32) {
            std::array<u32, 4> actual{};
            get_unsigned(program->native_handle(), static_cast<i32>(leaf.location), actual.data());
            for (std::size_t index = 0; index < expected.size(); ++index) CHECK(actual[index] == expected[index]);
        } else {
            std::array<i32, 4> actual{};
            get_signed(program->native_handle(), static_cast<i32>(leaf.location), actual.data());
            for (std::size_t index = 0; index < expected.size(); ++index) {
                CHECK(actual[index] == std::bit_cast<i32>(expected[index]));
            }
        }
    };
    check(true);
    check(false);
    check(i32{-2'000'000'000});
    check(u32{4'000'000'001});
    check(0.375F);
    check(Vec2{0.25F, -0.5F});
    check(Vec3{-0.25F, 0.5F, 0.75F});
    check(Vec4{0.125F, -0.25F, 0.375F, 0.5F});
    check(IVec2{-31, 47});
    check(IVec3{11, -17, 53});
    check(IVec4{-100, 200, -300, 400});
    check(UVec2{4'000'000'001U, 53});
    check(UVec3{13, 47, 4'000'000'000U});
    check(UVec4{4'000'000'001U, 17, 29, 4'000'000'003U});
    Mat3 matrix{};
    matrix[0] = {1.0F, 2.0F, 3.0F};
    matrix[1] = {4.0F, 5.0F, 6.0F};
    matrix[2] = {7.0F, 8.0F, 9.0F};
    check(matrix);
}

TEST_CASE("shared record uniforms link across stages and upload once",
          "[opengl][arguments][integration]")
{
    auto window = test::create_hidden_opengl_window(8, 8, "shared arguments");
    if (!window) skip(window.error().message);
    auto access = window->make_current();
    if (!access) skip(access.error().message);
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    auto vertex = shader::vertex<VI, VO>([](auto& s, dsl::Expr<Settings> settings) {
        return s.output(dsl::field<shader::ClipPosition>(dsl::vec4(
            s.input(Position{}) * settings.get(Gain{}), 0.0F, 1.0F)));
    });
    auto fragment = shader::fragment<FI, FO>([](auto& s, dsl::Expr<Settings> settings) {
        return s.output(dsl::field<shader::Color<0>>(settings.get(Tint{})));
    });
    REQUIRE(vertex);
    REQUIRE(fragment);
    auto neutral = shader::link(std::move(*vertex), std::move(*fragment), shader::shared_arguments);
    REQUIRE(neutral);
    auto program = render::compile_program(*device, *neutral);
    INFO((program ? "" : program.error().message));
    REQUIRE(program);
    Settings settings;
    settings.set(Tint{}, {0.25F, 0.5F, 0.75F, 1.0F});
    settings.set(Gain{}, 0.8F);
    REQUIRE(program->set_arguments(settings));
    REQUIRE(program->generated_source()->parameters.size() == 1);
    CHECK(program->untyped().argument_snapshots().size() == 1);
}
