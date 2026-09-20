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
static_assert(vng::window::Presentable<vng::glfw_opengl::Window>);
static_assert(!vng::window::Presentable<vng::window::GlfwWindow>);
static_assert(vng::window::PresentationDesc{}.vsync == vng::window::VSync::on);
// A presentation backend doesn't need GLFW, OpenGL, or a native Window API.
struct OtherPresentation {
    std::expected<void, vng::window::Diagnostic> present();
    std::expected<void, vng::window::Diagnostic> set_vsync(vng::window::VSync);
    vng::window::VSync vsync() const noexcept;
};
static_assert(vng::window::Presentable<OtherPresentation>);
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

TEST_CASE("VSync is owned by the presenting integration and survives context changes", "[window][opengl][vsync]") {
    using vng::window::VSync;
    using vng::window::ErrorCode;
    auto first=vng::glfw_opengl::create_window(
        {.width=16,.height=16,.visible=false}, {}, {.vsync=VSync::off});
    if(!first) skip_ctest(first.error().message);
    CHECK(first->vsync()==VSync::off);
    auto absent=first->set_vsync(VSync::on); REQUIRE_FALSE(absent);
    CHECK(absent.error().code==ErrorCode::context_not_current);
    CHECK(first->vsync()==VSync::off);
    REQUIRE(first->make_current());
    REQUIRE(first->set_vsync(VSync::on)); CHECK(first->vsync()==VSync::on);
    auto invalid=first->set_vsync(static_cast<VSync>(123)); REQUIRE_FALSE(invalid);
    CHECK(first->vsync()==VSync::on);
    std::optional<ErrorCode> thread_error;
    std::thread foreign([&] {
        auto result=first->set_vsync(VSync::off);
        if(!result) thread_error=result.error().code;
    });
    foreign.join();
    CHECK(thread_error==ErrorCode::wrong_thread);
    CHECK(first->vsync()==VSync::on);
    auto second=vng::glfw_opengl::create_window(
        {.width=16,.height=16,.visible=false}, {}, {.vsync=VSync::off});
    REQUIRE(second); REQUIRE(second->make_current());
    auto wrong=first->set_vsync(VSync::off); REQUIRE_FALSE(wrong);
    CHECK(wrong.error().code==ErrorCode::context_not_current);
    REQUIRE(second->current_context_access()); // Failure must not switch contexts.
    CHECK(first->vsync()==VSync::on); CHECK(second->vsync()==VSync::off);
    REQUIRE(first->make_current()); CHECK(first->vsync()==VSync::on);
    auto moved=std::move(*first);
    auto empty=first->set_vsync(VSync::off); REQUIRE_FALSE(empty);
    CHECK(empty.error().code==ErrorCode::invalid_window);
    CHECK(moved.vsync()==VSync::on);
    moved.release_current();
    REQUIRE_FALSE(moved.set_vsync(VSync::off));
    REQUIRE(moved.make_current()); CHECK(moved.vsync()==VSync::on);
    REQUIRE(moved.set_vsync(VSync::off));
    REQUIRE(moved.present());
    *second=std::move(moved);
    CHECK(second->vsync()==VSync::off); REQUIRE(second->make_current());
    REQUIRE(second->set_vsync(VSync::on));
    CHECK(second->vsync()==VSync::on);
    // No wall-clock/FPS assertions: drivers and compositors may override requests.
}

TEST_CASE("Invalid presentation requests fail before creating a native window", "[window][vsync]") {
    auto invalid=vng::glfw_opengl::create_window({}, {}, {.vsync=static_cast<vng::window::VSync>(-1)});
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code==vng::window::ErrorCode::window_creation_failed);
    CHECK(invalid.error().native_code==0);
}

TEST_CASE("OpenGL context and device survive native fullscreen switches", "[window][opengl][modes]") {
    auto window=vng::glfw_opengl::create_window(
        {.width=640,.height=480,.title="vng fullscreen lifetime test",.visible=false},
        {.debug=false,.default_framebuffer_encoding=vng::render::ColorEncoding::linear},
        {.vsync=vng::window::VSync::off});
    if(!window) skip_ctest(window.error().message);
    auto access=window->make_current(); REQUIRE(access);
    auto device=vng::opengl::Device::create(*access); REQUIRE(device);
    REQUIRE(window->set_fullscreen(true)); CHECK(window->fullscreen());
    REQUIRE(window->current_context_access());
    REQUIRE(device->clear_default_color({.1F,.2F,.3F,1}));
    REQUIRE(window->set_fullscreen(false)); CHECK_FALSE(window->fullscreen());
    REQUIRE(window->current_context_access());
    REQUIRE(device->clear_default_color({0,0,0,1}));
    REQUIRE(window->present()); window->hide();
}


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
    };

    auto created = vng::glfw_opengl::create_window(
        window_description,
        context_description, {.vsync = vng::window::VSync::off});
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
    CHECK_FALSE(created->key_down(vng::window::Key::space));
    CHECK_FALSE(created->key_down(vng::window::Key::w));
    CHECK_FALSE(moved.key_down(vng::window::Key::w));
    CHECK_FALSE(moved.key_down(static_cast<vng::window::Key>(-1)));
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
        }, {.vsync = vng::window::VSync::off});
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
        }, {.vsync = vng::window::VSync::off});
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
