#include <vng/gfx/gfx.hpp>
#include <vng/opengl/opengl.hpp>
#include <vng/render/render.hpp>
#include <vng/render_opengl/program_runtime.hpp>
#include <vng/shader/shader.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>

#include "../support/glfw_opengl.hpp"

namespace {
using namespace vng;
struct Position : gfx::Semantic<Vec2> {};
struct ObservedColor : gfx::Semantic<Vec4> {};
using Vertex = gfx::Record<Position>;
using VI = shader::VertexInputs<Position>;
using VO = shader::VertexOutputs<shader::ClipPosition>;
using FI = shader::FragmentInputs<>;
using FO = shader::FragmentOutputs<shader::Color<0>>;
}

TEST_CASE("vertex texture displacement and procedural math match enhanced rendering",
          "[opengl][texture][math][integration]")
{
    auto window = test::create_hidden_opengl_window(32, 32, "vertex displacement");
    if (!window) { std::cerr << window.error().message << '\n'; std::exit(77); }
    auto access = window->make_current();
    REQUIRE(access);
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    auto cursor = device->diagnostic_cursor();
    REQUIRE(cursor);

    auto vertex = shader::vertex<VI, VO>([](auto& s, dsl::Float strength) {
        auto position = s.input(Position{});
        auto height = s.template sample_2d_lod<2>(s.constant(Vec2{0.5F, 0.5F}), 0.0F).x();
        auto displaced = position + dsl::vec2(height * strength, 0.0F);
        return s.output(dsl::field<shader::ClipPosition>(dsl::vec4(displaced, 0.0F, 1.0F)));
    });
    auto fragment = shader::fragment<FI, FO>([](auto& s, dsl::Float time) {
        auto r = dsl::pow(dsl::abs(dsl::sin(time) + dsl::cos(time)), 2.0F);
        auto g = dsl::fract(dsl::floor(r) + 0.25F);
        auto b = dsl::exp(-r);
        auto color = dsl::vec4(r, g, b, 1.0F);
        s.observe(ObservedColor{}, color);
        return s.output(dsl::field<shader::Color<0>>(color));
    });
    REQUIRE(vertex);
    REQUIRE(fragment);
    auto neutral = shader::link(*vertex, *fragment);
    REQUIRE(neutral);
    auto runtime = render::OpenGLProgramRuntime::create(*device, std::move(*neutral));
    INFO((runtime ? "" : runtime.error().message));
    REQUIRE(runtime);

    auto height = gfx::make_image(*device, {
        .extent = {1, 1}, .format = gfx::ImageFormat::rgba32f,
    });
    REQUIRE(height);
    REQUIRE(height->clear_rgba32f({0.5F, 0.5F, 0.5F, 1.0F}));
    REQUIRE(height->bind_to_unit(2));
    gfx::Mesh<Vertex> cpu(3);
    cpu.vertices()[0].set(Position{}, {-0.8F, -0.5F});
    cpu.vertices()[1].set(Position{}, {-0.2F, -0.5F});
    cpu.vertices()[2].set(Position{}, {-0.5F, 0.5F});
    cpu.add_face(0, 1, 2);
    auto mesh = opengl::upload_mesh(*device, cpu);
    REQUIRE(mesh);
    REQUIRE(mesh->prepare_vertex_input(*device, runtime->production()));
    auto target = opengl::RenderTarget::create(*device,
        render::TargetDesc{.color = gfx::ImageFormat::rgba32f, .depth = true}, {32, 32});
    REQUIRE(target);
    for (const auto strength : {0.0F, 1.0F}) {
        auto frame = render::begin_frame(*device, *target, render::FrameDesc{
            .extent = {32, 32}, .color_encoding = render::ColorEncoding::linear,
            .clear_color = std::array{0.0F, 0.0F, 0.0F, 0.0F}, .clear_depth = 1.0F,
        });
        REQUIRE(frame);
        auto commands = frame->commands();
        auto program = runtime->production();
        REQUIRE(commands.run(program, strength, 0.0F));
        REQUIRE(commands.depth({.test = false, .write = false}));
        REQUIRE(commands.cull(render::CullMode::none));
        REQUIRE(commands.draw(*mesh));
        auto pixels = target->framebuffer().read_rgba32f(0, 0, 0, 32, 32);
        REQUIRE(pixels);
        const auto active_x = strength == 0.0F ? 8U : 16U;
        const auto inactive_x = strength == 0.0F ? 16U : 8U;
        const auto pixel = pixels->at(16U * 32U + active_x);
        CHECK(std::abs(pixel.r - 1.0F) < 0.0001F);
        CHECK(std::abs(pixel.g - 0.25F) < 0.0001F);
        CHECK(std::abs(pixel.b - std::exp(-1.0F)) < 0.0001F);
        CHECK(pixels->at(16U * 32U + inactive_x).a == 0.0F);
        REQUIRE(frame->end());
        auto evidence = runtime->capture(*device, cpu, *mesh,
            render::RenderView::without_camera({32, 32}),
            opengl::CaptureState{opengl::GraphicsStateSnapshot{}, render::ColorEncoding::linear},
            analysis::CaptureRequest::standard().observe(ObservedColor{}));
        INFO((evidence ? "" : evidence.error().message));
        REQUIRE(evidence);
        CHECK(evidence->capture().surface_at({active_x, 16}));
        CHECK_FALSE(evidence->capture().surface_at({inactive_x, 16}));
        const auto* observed = evidence->observation(ObservedColor{});
        REQUIRE(observed);
        const auto value = observed->at({active_x, 16});
        CHECK(std::abs(value.x - pixel.r) < 0.0001F);
        CHECK(std::abs(value.y - pixel.g) < 0.0001F);
        CHECK(std::abs(value.z - pixel.b) < 0.0001F);
    }
    auto diagnostics = device->diagnostics_since(*cursor);
    REQUIRE(diagnostics);
    // Some drivers report successful buffer placement as GL "other"
    // notifications. Keep warnings/errors actionable without treating these
    // informational messages as rendering failures.
    for (const auto& diagnostic : diagnostics->debug_messages) {
        INFO("driver message " << diagnostic.id << ", source=" << diagnostic.source
            << ", type=" << diagnostic.type << ", severity=" << static_cast<int>(diagnostic.severity)
            << ": " << diagnostic.message);
        CHECK(diagnostic.severity == opengl::DebugSeverity::notification);
        CHECK(diagnostic.type != "error");
        CHECK(diagnostic.type != "undefined-behavior");
    }
    for (const auto& diagnostic : diagnostics->lifecycle_diagnostics) {
        INFO(diagnostic.message);
        CHECK(diagnostics->lifecycle_diagnostics.empty());
    }
}
