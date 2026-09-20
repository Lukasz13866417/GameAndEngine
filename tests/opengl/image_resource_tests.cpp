#include <vng/gfx/image.hpp>
#include <vng/opengl/opengl.hpp>
#include <vng/opengl/render_target.hpp>
#include <vng/render/frame.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include "../support/glfw_opengl.hpp"

TEST_CASE("Image uploads preserve host unpack state and generate usable mips", "[opengl][image][resource]")
{
    auto window = vng::test::create_hidden_opengl_window(8, 8, "image upload test");
    if (!window) { std::cerr << window.error().message << '\n'; std::exit(77); }
    auto access = window->make_current();
    REQUIRE(access);
    auto device = vng::opengl::Device::create(*access);
    REQUIRE(device);
    auto cursor = device->diagnostic_cursor();
    REQUIRE(cursor);
    using PixelStore = void (*)(vng::u32, int);
    using BindBuffer = void (*)(vng::u32, vng::u32);
    using GetInteger = void (*)(vng::u32, int*);
    using GetTextureLevel = void (*)(vng::u32, int, vng::u32, int*);
    const auto pixel_store = reinterpret_cast<PixelStore>(access->resolve("glPixelStorei"));
    const auto bind_buffer = reinterpret_cast<BindBuffer>(access->resolve("glBindBuffer"));
    const auto get_integer = reinterpret_cast<GetInteger>(access->resolve("glGetIntegerv"));
    const auto get_level = reinterpret_cast<GetTextureLevel>(access->resolve("glGetTextureLevelParameteriv"));
    REQUIRE(pixel_store); REQUIRE(bind_buffer); REQUIRE(get_integer); REQUIRE(get_level);
    auto pbo = vng::opengl::Buffer::create(*device, {.size = 256});
    REQUIRE(pbo);
    constexpr vng::u32 unpack_buffer = 0x88EC;
    constexpr std::array<vng::u32, 8> names{0x0CF5, 0x0CF2, 0x0CF3, 0x0CF4, 0x806E, 0x806D, 0x0CF0, 0x0CF1};
    constexpr std::array<int, 8> values{8, 7, 2, 3, 6, 1, 1, 1};
    for (std::size_t i = 0; i < names.size(); ++i) pixel_store(names[i], values[i]);
    bind_buffer(unpack_buffer, pbo->native_handle());
    vng::gfx::ImageData data{.extent = {2, 2}, .pixels = {
        std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255},
        std::byte{0}, std::byte{255}, std::byte{0}, std::byte{255},
        std::byte{0}, std::byte{0}, std::byte{255}, std::byte{255},
        std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255},
    }};
    auto image = vng::gfx::upload_image(*device, data, {.format = vng::gfx::ImageFormat::rgba8});
    REQUIRE(image);
    CHECK(image->mip_levels() == 2);
    int width{};
    get_level(image->native_handle(), 1, 0x1000, &width);
    CHECK(width == 1);
    auto framebuffer = vng::opengl::Framebuffer::create(*device);
    REQUIRE(framebuffer);
    REQUIRE(framebuffer->attach_color(0, *image));
    auto uploaded = framebuffer->read_rgba8(0, 0, 0, 2, 2);
    REQUIRE(uploaded);
    CHECK(*uploaded == data.pixels);
    auto hdr = vng::gfx::make_image(*device, {.extent = {1, 1}, .format = vng::gfx::ImageFormat::rgba16f});
    REQUIRE(hdr);
    REQUIRE(hdr->write_rgba32f(0, 0, 1, 1, std::array{3.0F, 0.5F, 7.0F, 1.0F}));
    REQUIRE(framebuffer->attach_color(0, *hdr));
    auto floats = framebuffer->read_rgba32f(0, 0, 0, 1, 1);
    REQUIRE(floats);
    CHECK(floats->front().r == 3.0F);
    CHECK(floats->front().b == 7.0F);
    for (std::size_t i = 0; i < names.size(); ++i) {
        int actual{}; get_integer(names[i], &actual); CHECK(actual == values[i]);
        pixel_store(names[i], i == 0 ? 4 : 0);
    }
    int actual_buffer{};
    get_integer(0x88EF, &actual_buffer);
    CHECK(static_cast<vng::u32>(actual_buffer) == pbo->native_handle());
    bind_buffer(unpack_buffer, 0);
    CHECK_FALSE(vng::gfx::make_image(*device, {.extent = {2, 2}, .mip_levels = 3}));
    auto invalid_pixels = data;
    invalid_pixels.pixels.pop_back();
    CHECK_FALSE(vng::gfx::upload_image(*device, invalid_pixels));
    auto diagnostics = device->diagnostics_since(*cursor);
    REQUIRE(diagnostics);
    CHECK(diagnostics->empty());
}

TEST_CASE("Offscreen frames validate physical targets and preserve HDR values", "[opengl][image][frame]")
{
    auto window = vng::test::create_hidden_opengl_window(8, 8, "offscreen HDR test");
    if (!window) { std::cerr << window.error().message << '\n'; std::exit(77); }
    auto access = window->make_current();
    REQUIRE(access);
    auto device = vng::opengl::Device::create(*access);
    REQUIRE(device);
    auto cursor = device->diagnostic_cursor();
    REQUIRE(cursor);
    auto hdr = vng::gfx::make_image(*device, {.extent = {8, 8}, .format = vng::gfx::ImageFormat::rgba16f});
    auto depth = vng::gfx::make_image(*device, {.extent = {8, 8}, .format = vng::gfx::ImageFormat::depth32f});
    auto target = vng::opengl::Framebuffer::create(*device);
    REQUIRE(hdr); REQUIRE(depth); REQUIRE(target);
    REQUIRE(target->attach_color(0, *hdr));
    REQUIRE(target->attach_depth(*depth));
    REQUIRE(device->set_color_write_mask(0, {false, false, false, false}));
    REQUIRE(device->set_depth_state({.test_enabled = false, .write_enabled = false}));
    REQUIRE(device->set_scissor_enabled(true));
    CHECK_FALSE(vng::render::begin_frame(*device, *target,
        {.extent = {8, 8}, .color_encoding = vng::render::ColorEncoding::srgb, .clear_color = {}, .clear_depth = {}}));
    CHECK_FALSE(vng::render::begin_frame(*device, *target,
        {.extent = {4, 4}, .color_encoding = vng::render::ColorEncoding::linear, .clear_color = {}, .clear_depth = {}}));
    auto frame = vng::render::begin_frame(*device, *target, {
        .extent = {8, 8}, .color_encoding = vng::render::ColorEncoding::linear,
        .clear_color = std::array{4.0F, 2.0F, 0.5F, 1.0F}, .clear_depth = 0.75F,
    });
    REQUIRE(frame);
    CHECK(frame->uses_image(*hdr));
    CHECK(frame->uses_image(*depth));
    auto pixels = target->read_rgba32f(0, 0, 0, 8, 8);
    REQUIRE(pixels);
    for (const auto& pixel : *pixels) { CHECK(pixel.r == 4.0F); CHECK(pixel.g == 2.0F); }
    auto depths = target->read_depth32f(4, 4, 1, 1);
    REQUIRE(depths);
    CHECK(depths->front() == 0.75F);
    vng::opengl::ShaderSource vertex_source;
    vertex_source.stage = vng::opengl::ShaderStage::vertex;
    vertex_source.text = R"(#version 460 core
void main() {
    vec2 positions[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
    gl_Position = vec4(positions[gl_VertexID], 0, 1);
})";
    vng::opengl::ShaderSource fragment_source;
    fragment_source.stage = vng::opengl::ShaderStage::fragment;
    fragment_source.text = R"(#version 460 core
layout(location = 0) out vec4 color;
void main() { color = vec4(2, 6, 10, 1); }
)";
    auto vertex = vng::opengl::Shader::compile(*device, std::move(vertex_source));
    auto fragment = vng::opengl::Shader::compile(*device, std::move(fragment_source));
    REQUIRE(vertex); REQUIRE(fragment);
    auto program = vng::opengl::Program::link_graphics(*device, *vertex, *fragment);
    auto vao = vng::opengl::VertexArray::create(*device);
    REQUIRE(program); REQUIRE(vao);
    REQUIRE(program->bind()); REQUIRE(vao->bind());
    REQUIRE(device->draw_arrays_instanced(vng::opengl::Primitive::triangles, 0, 3));
    auto drawn = target->read_rgba32f(0, 4, 4, 1, 1);
    REQUIRE(drawn);
    CHECK(drawn->front().r == 2.0F);
    CHECK(drawn->front().g == 6.0F);
    CHECK(drawn->front().b == 10.0F);
    CHECK_FALSE(vng::render::begin_frame(*device, *target,
        {.extent = {8, 8}, .color_encoding = vng::render::ColorEncoding::linear, .clear_color = {}, .clear_depth = {}}));
    auto moved = std::move(*frame);
    CHECK(moved.uses_image(*hdr));
    CHECK_FALSE(frame->uses_image(*hdr));
    REQUIRE(moved.end());
    CHECK_FALSE(moved.uses_image(*hdr));
    auto srgb = vng::gfx::make_image(*device, {.extent = {8, 8}, .format = vng::gfx::ImageFormat::srgb8_alpha8});
    REQUIRE(srgb);
    REQUIRE(target->attach_color(0, *srgb));
    auto encoded = vng::render::begin_frame(*device, *target, {
        .extent = {8, 8}, .color_encoding = vng::render::ColorEncoding::srgb,
        .clear_color = std::array{0.5F, 0.5F, 0.5F, 1.0F}, .clear_depth = {},
    });
    REQUIRE(encoded);
    auto encoded_pixels = target->read_rgba8_pixels(0, 0, 0, 1, 1);
    REQUIRE(encoded_pixels);
    CHECK(std::abs(static_cast<int>(encoded_pixels->front().r) - 188) <= 1);
    REQUIRE(encoded->end());
    auto diagnostics = device->diagnostics_since(*cursor);
    REQUIRE(diagnostics);
    CHECK(diagnostics->empty());
}

TEST_CASE("Ready render targets retain providers and resize atomically", "[opengl][target][resource]")
{
    auto window = vng::test::create_hidden_opengl_window(8, 8, "provider target test");
    if (!window) { std::cerr << window.error().message << '\n'; std::exit(77); }
    auto access = window->make_current();
    REQUIRE(access);
    auto device = vng::opengl::Device::create(*access);
    REQUIRE(device);
    auto cursor = device->diagnostic_cursor();
    REQUIRE(cursor);
    // The creating provider disappears at the semicolon.
    auto target = vng::providers::render_target().provide(*device, {8, 8});
    REQUIRE(target);
    REQUIRE(target->depth());
    REQUIRE(target->framebuffer().check_complete());
    const auto original_color = target->color().native_handle();
    const auto original_framebuffer = target->framebuffer().native_handle();
    REQUIRE(target->resize(*device, {8, 8}));
    CHECK(target->color().native_handle() == original_color);
    CHECK(target->framebuffer().native_handle() == original_framebuffer);
    auto invalid = target->resize(*device, {0, 8});
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().cause_as<vng::opengl::Diagnostic>() != nullptr);
    CHECK(target->color().native_handle() == original_color);
    CHECK(target->framebuffer().native_handle() == original_framebuffer);
    CHECK(target->extent() == vng::Extent2D{8, 8});
    const auto& constant_target = *target;
    auto frame = vng::render::begin_frame(*device, constant_target, {
        .extent = {8, 8}, .color_encoding = vng::render::ColorEncoding::linear,
        .clear_color = std::array{5.0F, 1.0F, 0.0F, 1.0F}, .clear_depth = 1.0F,
    });
    REQUIRE(frame);
    CHECK(frame->uses_image(target->color()));
    CHECK_FALSE(target->reload(*device));
    CHECK_FALSE(target->resize(*device, {4, 4}));
    CHECK_FALSE(target->resize(*device, {8, 8}));
    CHECK(target->color().native_handle() == original_color);
    auto rendered = target->framebuffer().read_rgba32f(0, 0, 0, 1, 1);
    REQUIRE(rendered);
    CHECK(rendered->front().r == 5.0F);
    REQUIRE(frame->end());
    REQUIRE(target->resize(*device, {4, 2}));
    CHECK(target->extent() == vng::Extent2D{4, 2});
    CHECK(target->color().native_handle() != original_color);
    CHECK(target->color().format() == vng::gfx::ImageFormat::rgba16f);
    REQUIRE(target->depth());
    CHECK(target->depth()->extent() == target->extent());
    REQUIRE(target->framebuffer().check_complete());
    const auto resized_color = target->color().native_handle();
    REQUIRE(target->reload(*device));
    CHECK(target->color().native_handle() != resized_color);
    CHECK(target->extent() == vng::Extent2D{4, 2});
    auto provider = vng::providers::render_target({.color = vng::gfx::ImageFormat::srgb8_alpha8, .depth = false});
    auto first = provider.provide(*device, {2, 2});
    auto second = provider.provide(*device, {3, 3});
    REQUIRE(first); REQUIRE(second);
    CHECK(first->depth() == nullptr);
    REQUIRE(first->reload(*device));
    CHECK(first->color().format() == vng::gfx::ImageFormat::srgb8_alpha8);
    CHECK(first->depth() == nullptr);
    CHECK(second->extent() == vng::Extent2D{3, 3});
    CHECK(first->color().native_handle() != second->color().native_handle());
    auto diagnostics = device->diagnostics_since(*cursor);
    REQUIRE(diagnostics);
    CHECK(diagnostics->empty());
}
