#include <vng/opengl/opengl.hpp>
#include <vng/render/render.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <iostream>
#include <utility>

#include "../support/glfw_opengl.hpp"

TEST_CASE("OpenGL entry points declare one backend identity", "[opengl][frame][backend]") {
    STATIC_CHECK(vng::render::Frame<vng::opengl::Frame>);
    STATIC_CHECK(std::same_as<vng::render::backend_t<vng::opengl::Frame>,vng::opengl::Backend>);
    STATIC_CHECK(vng::render::SameBackend<vng::opengl::Device,vng::opengl::Frame>);
    STATIC_CHECK(vng::render::SameBackend<vng::opengl::Commands,vng::opengl::Frame>);
    STATIC_CHECK(std::same_as<vng::opengl::Backend::frame_type,vng::opengl::Frame>);
    STATIC_CHECK(std::same_as<vng::opengl::Backend::device_type,vng::opengl::Device>);
}

TEST_CASE("OpenGL frames establish and close a default-target scope",
          "[opengl][frame]")
{
    auto window = vng::test::create_hidden_opengl_window(
        16, 12, "vng hidden frame test");
    if (!window) {
        std::cerr << "OpenGL frame test skipped: "
                  << window.error().message << '\n';
        std::exit(77);
    }
    auto access = window->make_current();
    if (!access) {
        std::cerr << "OpenGL frame test skipped: "
                  << access.error().message << '\n';
        std::exit(77);
    }
    auto device = vng::opengl::Device::create(*access);
    REQUIRE(device);

    using ClearColor = void (*)(float, float, float, float);
    using ClearDepth = void (*)(double);
    using GetFloat = void (*)(std::uint32_t, float*);
    using GetBoolean = void (*)(std::uint32_t, std::uint8_t*);
    using GetBooleanIndexed = void (*)(
        std::uint32_t, std::uint32_t, std::uint8_t*);
    using IsEnabled = std::uint8_t (*)(std::uint32_t);
    using ReadPixels = void (*)(
        std::int32_t,
        std::int32_t,
        std::int32_t,
        std::int32_t,
        std::uint32_t,
        std::uint32_t,
        void*);

    const auto clear_color = reinterpret_cast<ClearColor>(
        access->resolve("glClearColor"));
    const auto clear_depth = reinterpret_cast<ClearDepth>(
        access->resolve("glClearDepth"));
    const auto get_float = reinterpret_cast<GetFloat>(
        access->resolve("glGetFloatv"));
    const auto get_boolean = reinterpret_cast<GetBoolean>(
        access->resolve("glGetBooleanv"));
    const auto get_boolean_indexed = reinterpret_cast<GetBooleanIndexed>(
        access->resolve("glGetBooleani_v"));
    const auto is_enabled = reinterpret_cast<IsEnabled>(
        access->resolve("glIsEnabled"));
    const auto read_pixels = reinterpret_cast<ReadPixels>(
        access->resolve("glReadPixels"));
    REQUIRE(clear_color != nullptr);
    REQUIRE(clear_depth != nullptr);
    REQUIRE(get_float != nullptr);
    REQUIRE(get_boolean != nullptr);
    REQUIRE(get_boolean_indexed != nullptr);
    REQUIRE(is_enabled != nullptr);
    REQUIRE(read_pixels != nullptr);

    constexpr std::uint32_t gl_color_clear_value = 0x0C22;
    constexpr std::uint32_t gl_depth_clear_value = 0x0B73;
    constexpr std::uint32_t gl_depth_writemask = 0x0B72;
    constexpr std::uint32_t gl_color_writemask = 0x0C23;
    constexpr std::uint32_t gl_framebuffer_srgb = 0x8DB9;
    constexpr std::uint32_t gl_rgba = 0x1908;
    constexpr std::uint32_t gl_depth_component = 0x1902;
    constexpr std::uint32_t gl_float = 0x1406;

    // Seed state that used to make begin_frame's clears silently ineffective.
    // The clear-value sentinels also prove that implementation details do not
    // leak through OpenGL's global GL_*_CLEAR_VALUE state.
    const std::array<float, 4> clear_color_sentinel{
        0.91F, 0.82F, 0.73F, 0.64F};
    constexpr float clear_depth_sentinel = 0.37F;
    clear_color(
        clear_color_sentinel[0],
        clear_color_sentinel[1],
        clear_color_sentinel[2],
        clear_color_sentinel[3]);
    clear_depth(clear_depth_sentinel);
    REQUIRE(device->set_color_write_mask(
        0, {false, false, false, false}));
    REQUIRE(device->set_depth_state({
        .test_enabled = true,
        .write_enabled = false,
        .compare = vng::opengl::DepthCompare::never,
    }));
    REQUIRE(device->set_framebuffer_srgb_enabled(true));

    constexpr std::array<float, 4> requested_color{
        0.1F, 0.2F, 0.3F, 1.0F};
    constexpr float requested_depth = 0.75F;

    auto frame = vng::render::begin_frame(
        *device,
        vng::render::FrameDesc{
            .extent = {16, 12},
            .color_encoding = vng::render::ColorEncoding::linear,
            .clear_color = requested_color,
            .clear_depth = requested_depth,
        });
    REQUIRE(frame);
    STATIC_CHECK(vng::render::Frame<vng::opengl::Frame>);
    CHECK(frame->active());
    CHECK(frame->extent() == vng::Extent2D{16, 12});
    CHECK(frame->color_encoding() == vng::render::ColorEncoding::linear);
    CHECK(frame->belongs_to(*device));

    std::array<std::uint8_t, 4> color_mask{};
    get_boolean_indexed(gl_color_writemask, 0, color_mask.data());
    CHECK(color_mask == std::array<std::uint8_t, 4>{1, 1, 1, 1});
    std::uint8_t depth_mask = 0;
    get_boolean(gl_depth_writemask, &depth_mask);
    CHECK(depth_mask == 1);
    CHECK(is_enabled(gl_framebuffer_srgb) == 0);

    std::array<float, 4> actual_color{};
    float actual_depth = 0.0F;
    read_pixels(8, 6, 1, 1, gl_rgba, gl_float, actual_color.data());
    read_pixels(
        8,
        6,
        1,
        1,
        gl_depth_component,
        gl_float,
        &actual_depth);
    for (std::size_t component = 0; component < actual_color.size();
         ++component) {
        CHECK(std::abs(actual_color[component] - requested_color[component])
              <= (2.0F / 255.0F));
    }
    CHECK(std::abs(actual_depth - requested_depth) <= 0.0001F);

    std::array<float, 4> retained_clear_color{};
    float retained_clear_depth = 0.0F;
    get_float(gl_color_clear_value, retained_clear_color.data());
    get_float(gl_depth_clear_value, &retained_clear_depth);
    CHECK(retained_clear_color == clear_color_sentinel);
    CHECK(std::abs(retained_clear_depth - clear_depth_sentinel) <= 0.0001F);

    REQUIRE(frame->end());
    CHECK_FALSE(frame->active());
    REQUIRE_FALSE(frame->end());

    const auto second_encoding = vng::render::ColorEncoding::linear;
    auto srgb_frame = vng::render::begin_frame(
        *device,
        vng::render::FrameDesc{
            .extent = {16, 12},
            .color_encoding = second_encoding,
            .clear_color = std::nullopt,
            .clear_depth = std::nullopt,
        });
    REQUIRE(srgb_frame);
    CHECK(srgb_frame->color_encoding() == second_encoding);
    CHECK((is_enabled(gl_framebuffer_srgb) != 0)
          == (second_encoding == vng::render::ColorEncoding::srgb));

    // Frame identity follows the shared context, not the address or lifetime
    // of the Device facade that began it.
    auto relocated_device = std::move(*device);
    CHECK(srgb_frame->belongs_to(relocated_device));
    REQUIRE(srgb_frame->device().require_current(
        "frame retained context test"));
    *device = std::move(relocated_device);

    auto moved_frame = std::move(*srgb_frame);
    CHECK_FALSE(srgb_frame->active());
    CHECK_FALSE(srgb_frame->belongs_to(*device));
    auto moved_from_device = srgb_frame->device().require_current(
        "moved-from frame device test");
    REQUIRE_FALSE(moved_from_device);
    CHECK(moved_from_device.error().code
          == vng::opengl::ErrorCode::invalid_context_access);
    CHECK(moved_frame.belongs_to(*device));
    REQUIRE(moved_frame.end());

    auto retained_frame = [&]()
        -> std::expected<vng::opengl::Frame, vng::opengl::Diagnostic> {
        auto temporary_device = vng::opengl::Device::create(*access);
        if (!temporary_device) {
            return std::unexpected(std::move(temporary_device.error()));
        }
        return vng::render::begin_frame(
            *temporary_device,
            vng::render::FrameDesc{
                .extent = {16, 12},
                .color_encoding = vng::render::ColorEncoding::linear,
                .clear_color = std::nullopt,
                .clear_depth = std::nullopt,
            });
    }();
    REQUIRE(retained_frame);
    CHECK(retained_frame->belongs_to(*device));
    REQUIRE(retained_frame->device().require_current(
        "frame outlives Device facade test"));
    REQUIRE(retained_frame->end());

    auto invalid = vng::render::begin_frame(
        *device, vng::render::FrameDesc{});
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == vng::opengl::ErrorCode::invalid_argument);

    auto invalid_encoding = vng::render::begin_frame(
        *device,
        vng::render::FrameDesc{
            .extent = {16, 12},
            .color_encoding =
                static_cast<vng::render::ColorEncoding>(255),
            .clear_color = std::nullopt,
            .clear_depth = std::nullopt,
        });
    REQUIRE_FALSE(invalid_encoding);
    CHECK(invalid_encoding.error().code
          == vng::opengl::ErrorCode::invalid_argument);
}

TEST_CASE("OpenGL frames require the physical default-target encoding",
          "[opengl][frame][srgb]")
{
    auto window = vng::glfw_opengl::create_window(
        vng::window::WindowDesc{
            .width = 8,
            .height = 8,
            .title = "vng default-target encoding test",
            .visible = false,
            .resizable = false,
        },
        vng::opengl::ContextDesc{
            .debug = true,
            .samples = 0,
            .default_framebuffer_encoding =
                vng::render::ColorEncoding::linear,
        }, {.vsync = vng::window::VSync::off});
    if (!window) {
        std::cerr << "OpenGL frame test skipped: "
                  << window.error().message << '\n';
        std::exit(77);
    }
    auto access = window->make_current();
    if (!access) {
        std::cerr << "OpenGL frame test skipped: "
                  << access.error().message << '\n';
        std::exit(77);
    }
    auto device = vng::opengl::Device::create(*access);
    REQUIRE(device);

    const auto capabilities =
        device->default_framebuffer_capabilities();
    REQUIRE(capabilities.color_encoding.has_value());
    const auto matching = *capabilities.color_encoding;
    const auto mismatching = matching == vng::render::ColorEncoding::srgb
        ? vng::render::ColorEncoding::linear
        : vng::render::ColorEncoding::srgb;
    if (matching == vng::render::ColorEncoding::srgb) {
        REQUIRE(capabilities.srgb_conversion_supported);
    }

    auto unsupported = vng::render::begin_frame(
        *device,
        vng::render::FrameDesc{
            .extent = {8, 8},
            .color_encoding = mismatching,
            .clear_color = std::nullopt,
            .clear_depth = std::nullopt,
        });
    REQUIRE_FALSE(unsupported);
    CHECK(unsupported.error().code
          == vng::opengl::ErrorCode::unsupported_feature);

    auto supported = vng::render::begin_frame(
        *device,
        vng::render::FrameDesc{
            .extent = {8, 8},
            .color_encoding = matching,
            .clear_color = std::nullopt,
            .clear_depth = std::nullopt,
        });
    REQUIRE(supported);
    REQUIRE(supported->end());
}

TEST_CASE("OpenGL frames have one generation-owned scope per context",
          "[opengl][frame][lifetime]")
{
    auto window = vng::test::create_hidden_opengl_window(
        8, 8, "vng frame ownership test");
    if (!window) {
        std::cerr << "OpenGL frame test skipped: "
                  << window.error().message << '\n';
        std::exit(77);
    }
    auto access = window->make_current();
    if (!access) {
        std::cerr << "OpenGL frame test skipped: "
                  << access.error().message << '\n';
        std::exit(77);
    }
    auto device = vng::opengl::Device::create(*access);
    REQUIRE(device);

    const auto description = vng::render::FrameDesc{
        .extent = {8, 8},
        .color_encoding = vng::render::ColorEncoding::linear,
        .clear_color = std::nullopt,
        .clear_depth = std::nullopt,
    };

    auto first = vng::render::begin_frame(*device, description);
    REQUIRE(first);
    CHECK(first->active());

    // A second begin cannot silently retarget work represented by `first`.
    auto overlapping = vng::render::begin_frame(*device, description);
    REQUIRE_FALSE(overlapping);
    CHECK(overlapping.error().code
          == vng::opengl::ErrorCode::invalid_argument);
    CHECK(overlapping.error().message.find("overlapping")
          != std::string::npos);
    CHECK(first->active());

    auto moved = std::move(*first);
    CHECK_FALSE(first->active());
    CHECK(moved.active());

    auto still_overlapping = vng::render::begin_frame(
        *device, description);
    REQUIRE_FALSE(still_overlapping);
    CHECK(moved.active());
    REQUIRE(moved.end());
    CHECK_FALSE(moved.active());

    auto successor = vng::render::begin_frame(*device, description);
    REQUIRE(successor);
    CHECK(successor->active());

    // An ended generation is not a capability for its successor.
    auto stale_end = moved.end();
    REQUIRE_FALSE(stale_end);
    CHECK(successor->active());

    // Move assignment transfers ownership into an inert Frame value.
    auto slot = std::move(*successor);
    CHECK_FALSE(successor->active());
    CHECK(slot.active());
    REQUIRE(slot.end());

    auto inert_slot = std::move(slot);
    CHECK_FALSE(slot.active());
    CHECK_FALSE(inert_slot.active());
    auto replacement = vng::render::begin_frame(*device, description);
    REQUIRE(replacement);
    inert_slot = std::move(*replacement);
    CHECK_FALSE(replacement->active());
    CHECK(inert_slot.active());
    REQUIRE(inert_slot.end());

    // Dropping the owner closes the logical scope without issuing GL work.
    {
        auto automatic = vng::render::begin_frame(*device, description);
        REQUIRE(automatic);
        CHECK(automatic->active());
    }
    auto after_destruction = vng::render::begin_frame(*device, description);
    REQUIRE(after_destruction);
    REQUIRE(after_destruction->end());
}

TEST_CASE("OpenGL frame activity reflects native context lifetime",
          "[opengl][frame][lifetime]")
{
    auto window = vng::test::create_hidden_opengl_window(
        8, 8, "vng expired frame ownership test");
    if (!window) {
        std::cerr << "OpenGL frame test skipped: "
                  << window.error().message << '\n';
        std::exit(77);
    }
    auto access = window->make_current();
    if (!access) {
        std::cerr << "OpenGL frame test skipped: "
                  << access.error().message << '\n';
        std::exit(77);
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
    CHECK(frame->active());

    // This models a context provider invalidating its native lifetime token.
    access->lifetime->invalidate();
    CHECK_FALSE(frame->active());
    auto ended = frame->end();
    REQUIRE_FALSE(ended);
    CHECK(ended.error().code == vng::opengl::ErrorCode::context_expired);
}
