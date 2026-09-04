#include <vng/opengl/device.hpp>
#include <vng/window/glfw_opengl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace {

static_assert(vng::window::Window<vng::glfw_opengl::Window>);
static_assert(
    vng::opengl::ContextDesc{}.default_framebuffer_encoding
    == vng::render::ColorEncoding::srgb);
static_assert(
    !vng::opengl::DefaultFramebufferCapabilities{}.color_encoding);

[[noreturn]] void skip_ctest(const std::string& reason)
{
    std::cerr << "GLFW + OpenGL integration test skipped: " << reason << '\n';
    std::exit(77);
}

} // namespace

TEST_CASE("GLFW and OpenGL descriptions remain separate",
          "[window][glfw][opengl]")
{
    const vng::window::WindowDesc window_description{
        .width = 16,
        .height = 16,
        .title = "vng hidden GLFW OpenGL integration test",
        .visible = false,
        .resizable = false,
    };
    const vng::opengl::ContextDesc context_description{
        .version = {4, 6},
        .debug = false,
        .forward_compatible = true,
        .samples = 0,
        .default_framebuffer_encoding = vng::render::ColorEncoding::linear,
        .swap_interval = 0,
    };

    auto created = vng::glfw_opengl::create_window(
        window_description,
        context_description);
    if (!created) {
        skip_ctest(created.error().message);
    }

    CHECK(created->valid());
    auto access = created->make_current();
    REQUIRE(access.has_value());
    CHECK(access->valid());
    REQUIRE(access->required_default_framebuffer_encoding);
    CHECK(*access->required_default_framebuffer_encoding
          == vng::render::ColorEncoding::linear);
    auto device = vng::opengl::Device::create(*access);
    REQUIRE(device.has_value());
    const auto framebuffer = device->default_framebuffer_capabilities();
    REQUIRE(framebuffer.color_encoding.has_value());
    CHECK(*framebuffer.color_encoding == vng::render::ColorEncoding::linear);
    CHECK(framebuffer.srgb_conversion_supported);
    CHECK(created->current_context_access().has_value());
    CHECK(created->present().has_value());

    auto moved = std::move(*created);
    CHECK_FALSE(created->valid());
    CHECK(moved.valid());
    CHECK_FALSE(created->make_current().has_value());
    CHECK(moved.make_current().has_value());
    moved.poll_events();
    moved.release_current();
    CHECK_FALSE(moved.current_context_access().has_value());
}

TEST_CASE("GLFW OpenGL integration verifies requested sRGB physically",
          "[window][glfw][opengl][srgb]")
{
    auto created = vng::glfw_opengl::create_window(
        vng::window::WindowDesc{
            .width = 16,
            .height = 16,
            .title = "vng hidden GLFW sRGB verification test",
            .visible = false,
            .resizable = false,
        },
        vng::opengl::ContextDesc{
            .debug = false,
            .samples = 0,
            .default_framebuffer_encoding =
                vng::render::ColorEncoding::srgb,
            .swap_interval = 0,
        });
    if (!created) {
        skip_ctest(created.error().message);
    }
    auto access = created->make_current();
    REQUIRE(access);
    REQUIRE(access->required_default_framebuffer_encoding);
    CHECK(*access->required_default_framebuffer_encoding
          == vng::render::ColorEncoding::srgb);

    auto provider_without_requirement = *access;
    provider_without_requirement.required_default_framebuffer_encoding.reset();
    auto queried_only =
        vng::opengl::Device::create(provider_without_requirement);
    REQUIRE(queried_only);
    REQUIRE(queried_only->default_framebuffer_capabilities().color_encoding);

    auto device = vng::opengl::Device::create(*access);
    if (!device) {
        // Headless/Xvfb configurations often accept GLFW's hint but expose a
        // linear visual. The important contract is that this cannot silently
        // produce a Device whose default Frame has the wrong encoding.
        CHECK(device.error().code
              == vng::opengl::ErrorCode::unsupported_feature);
        CHECK(device.error().message.find("required a sRGB")
              != std::string::npos);
        return;
    }

    const auto framebuffer = device->default_framebuffer_capabilities();
    REQUIRE(framebuffer.color_encoding);
    CHECK(*framebuffer.color_encoding == vng::render::ColorEncoding::srgb);
    CHECK(framebuffer.srgb_conversion_supported);
}

TEST_CASE("GLFW OpenGL integration rejects contexts below backend minimum",
          "[window][glfw][opengl]")
{
    auto created = vng::glfw_opengl::create_window(
        vng::window::WindowDesc{},
        vng::opengl::ContextDesc{.version = {4, 5}});

    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().code == vng::window::ErrorCode::window_creation_failed);
}

TEST_CASE("OpenGL context operations diagnose a foreign thread",
          "[window][glfw][opengl][thread]")
{
    auto created = vng::glfw_opengl::create_window(
        vng::window::WindowDesc{
            .width = 16,
            .height = 16,
            .title = "vng GLFW OpenGL thread test",
            .visible = false,
            .resizable = false,
        },
        vng::opengl::ContextDesc{
            .debug = false,
            .swap_interval = 0,
        });
    if (!created) {
        skip_ctest(created.error().message);
    }

    std::optional<vng::window::Diagnostic> diagnostic;
    bool unexpectedly_succeeded = false;
    std::thread worker([&] {
        auto access = created->make_current();
        unexpectedly_succeeded = access.has_value();
        if (!access) {
            diagnostic.emplace(std::move(access.error()));
        }
    });
    worker.join();

    CHECK_FALSE(unexpectedly_succeeded);
    REQUIRE(diagnostic.has_value());
    CHECK(diagnostic->code == vng::window::ErrorCode::wrong_thread);
    CHECK(created->make_current().has_value());
}
