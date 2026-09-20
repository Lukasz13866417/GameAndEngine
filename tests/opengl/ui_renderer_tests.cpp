#include <vng/opengl/ui_renderer.hpp>
#include <vng/render/frame.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include "../support/glfw_opengl.hpp"

namespace {
using namespace vng;
constexpr Extent2D extent{256, 192};
using Pixel = std::array<unsigned char, 4>;
struct Harness {
    glfw_opengl::Window window;
    opengl::CurrentContextAccess access;
    opengl::Device device;
};
Harness create() {
    auto window =
        test::create_hidden_opengl_window(extent.width, extent.height, "UI renderer test");
    if (!window) {
        std::cerr << window.error().message << '\n';
        std::exit(77);
    }
    auto access = window->make_current();
    if (!access) {
        std::cerr << access.error().message << '\n';
        std::exit(77);
    }
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    return {std::move(*window), *access, std::move(*device)};
}
text::Font font() {
    auto result = text::Font::load(VNG_TEST_FONT_PATH);
    REQUIRE(result);
    return *result;
}
opengl::Frame begin(Harness& h) {
    auto result = render::begin_frame(h.device, {.extent = extent,
                                                 .color_encoding = render::ColorEncoding::linear,
                                                 .clear_color = std::array<float, 4>{0, 0, 0, 1},
                                                 .clear_depth = 1.0F});
    REQUIRE(result);
    return std::move(*result);
}
std::vector<Pixel> read(const Harness& h) {
    using Read = void (*)(i32, i32, i32, i32, u32, u32, void*);
    const auto function = reinterpret_cast<Read>(h.access.resolve("glReadPixels"));
    REQUIRE(function);
    std::vector<Pixel> pixels(extent.width * extent.height);
    function(0, 0, static_cast<i32>(extent.width), static_cast<i32>(extent.height), 0x1908, 0x1401,
             pixels.data());
    return pixels;
}
Pixel pixel(const std::vector<Pixel>& pixels, u32 x, u32 y) {
    return pixels[(extent.height - 1 - y) * extent.width + x];
}
void rendered(const std::expected<void, opengl::Diagnostic>& result) {
    if (!result)
        INFO(result.error().message);
    REQUIRE(result);
}
ui::DrawList list() {
    return {{256, 192}, extent, {}};
}
constexpr ui::Rect full{0, 0, 256, 192};
} // namespace

TEST_CASE("UI GPU label-heavy panels retain painter order with one geometry upload",
          "[ui][opengl][batching]") {
    auto h = create();
    auto renderer = render::make_ui_renderer(h.device);
    REQUIRE(renderer);
    auto draws = list();
    const auto face = font();
    for (u32 y = 0; y < 12; ++y)
        for (u32 x = 0; x < 16; ++x) {
            const auto left = static_cast<f32>(x * 16), top = static_cast<f32>(y * 16);
            draws.commands.emplace_back(
                ui::BoxDraw{{left, top, 16, 16}, full, {.05F, .1F, .15F, 1}, {}, 0, 0});
            draws.commands.emplace_back(
                ui::TextDraw{"UI", {left, top}, full, face, 10, {1, 1, 1, 1}});
        }
    auto frame = begin(h);
    rendered(renderer->render(frame, draws));
    const auto pixels = read(h);
    CHECK(renderer->stats().boxes == 192);
    CHECK(renderer->stats().glyphs == 384);
    CHECK(renderer->stats().draw_calls == 384);
    CHECK(renderer->stats().text_vertex_uploads == 1);
    REQUIRE(frame.end());

    // A representative panel alternates each button background with its text.
    // Report timings for local regression investigation without flaky limits.
    const auto start = std::chrono::steady_clock::now();
    for (u32 i = 0; i < 20; ++i) {
        auto next = begin(h);
        rendered(renderer->render(next, draws));
        CHECK(renderer->stats().glyph_uploads == 0);
        CHECK(renderer->stats().text_vertex_uploads == 1);
        REQUIRE(next.end());
    }
    CHECK(read(h) == pixels);
    const auto milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count() /
        20;
    std::cout << "UI 192-label panel: " << milliseconds << " ms/frame\n";
}

TEST_CASE("UI batched text ranges match sequential draws across atlas pages and empty labels",
          "[ui][opengl][batching]") {
    auto h = create();
    auto renderer = render::make_ui_renderer(h.device);
    REQUIRE(renderer);
    auto draws = list();
    const auto face = font();
    draws.commands.emplace_back(ui::TextDraw{"", {}, full, face, 20, {1, 1, 1, 1}});
    for (const auto* text : {"A", "B", "A", "C"}) {
        // Each glyph is larger than half an atlas page. Repeated A also
        // exercises a page revisit, not just adjacent-page batching.
        draws.commands.emplace_back(
            ui::TextDraw{text, {-200, -220}, full, face, 900, {.2F, .4F, .8F, .5F}});
        draws.commands.emplace_back(
            ui::BoxDraw{{40, 40, 150, 100}, full, {.9F, .2F, .1F, .1F}, {}, 0, 0});
        draws.commands.emplace_back(ui::TextDraw{"invisible", {}, full, face, 20, {1, 1, 1, 0}});
        draws.commands.emplace_back(
            ui::TextDraw{"clipped", {900, 900}, full, face, 20, {1, 1, 1, 1}});
    }
    auto frame = begin(h);
    rendered(renderer->render(frame, draws));
    const auto together = read(h);
    CHECK(renderer->stats().text_vertex_uploads == 1);
    REQUIRE(frame.end());
    auto again = begin(h);
    for (const auto& command : draws.commands) {
        auto single = list();
        single.commands.push_back(command);
        rendered(renderer->render(again, single));
    }
    CHECK(read(h) == together);
    REQUIRE(again.end());
}

TEST_CASE("UI GPU shapes clip round blend and preserve text painter order", "[ui][opengl]") {
    auto h = create();
    auto renderer = render::make_ui_renderer(h.device);
    REQUIRE(renderer);
    auto draws = list();
    draws.commands.emplace_back(ui::BoxDraw{{8, 8, 48, 48}, full, {1, 0, 0, 1}, {}, 12, 0});
    draws.commands.emplace_back(
        ui::BoxDraw{{80, 8, 40, 48}, {90, 8, 20, 48}, {0, 1, 0, 1}, {}, 0, 0});
    draws.commands.emplace_back(ui::BoxDraw{{136, 8, 48, 48}, full, {1, 0, 0, .5F}, {}, 0, 0});
    draws.commands.emplace_back(ui::TextDraw{"MMMM", {8, 70}, full, font(), 28, {1, 0, 0, 1}});
    draws.commands.emplace_back(ui::BoxDraw{{4, 68, 120, 48}, full, {0, 0, 1, 1}, {}, 0, 0});
    draws.commands.emplace_back(ui::TextDraw{"MMMM", {136, 70}, full, font(), 28, {0, 1, 0, 1}});
    auto frame = begin(h);
    rendered(renderer->render(frame, draws));
    const auto pixels = read(h);
    CHECK(pixel(pixels, 32, 32) == Pixel{255, 0, 0, 255});
    CHECK(pixel(pixels, 8, 8) == Pixel{0, 0, 0, 255});
    CHECK(pixel(pixels, 95, 32) == Pixel{0, 255, 0, 255});
    CHECK(pixel(pixels, 85, 32) == Pixel{0, 0, 0, 255});
    CHECK(pixel(pixels, 115, 32) == Pixel{0, 0, 0, 255});
    CHECK(std::abs(static_cast<int>(pixel(pixels, 150, 32)[0]) - 128) <= 1);
    bool green{};
    for (u32 y = 70; y < 110; ++y)
        for (u32 x = 8; x < 116; ++x)
            CHECK(pixel(pixels, x, y) == Pixel{0, 0, 255, 255});
    for (u32 y = 70; y < 110; ++y)
        for (u32 x = 136; x < 250; ++x)
            green |= pixel(pixels, x, y)[1] > 128;
    CHECK(green);
    CHECK(renderer->stats().draw_calls == 4);
    CHECK(renderer->stats().glyph_uploads > 0);
    REQUIRE(frame.end());
    auto again = begin(h);
    rendered(renderer->render(again, draws));
    CHECK(renderer->stats().glyph_uploads == 0);
    CHECK(read(h) == pixels);
    REQUIRE(again.end());
    for (const auto& message : h.device.take_debug_messages()) {
        INFO(message.message);
        CHECK(message.severity == opengl::DebugSeverity::notification);
    }
}

TEST_CASE("UI triangle overlays clip and batch with ordinary shapes", "[ui][opengl]") {
    auto h=create();auto renderer=render::make_ui_renderer(h.device);REQUIRE(renderer);
    auto draws=list();
    draws.commands.emplace_back(ui::TriangleDraw{{Vec2{0,0},Vec2{200,0},Vec2{0,180}},{20,20,100,100},{1,0,0,1}});
    draws.commands.emplace_back(ui::BoxDraw{{30,30,10,10},full,{0,1,0,1},{},0,0});
    auto frame=begin(h);rendered(renderer->render(frame,draws));
    const auto pixels=read(h);
    CHECK(pixel(pixels,25,25)==Pixel{255,0,0,255});
    CHECK(pixel(pixels,10,25)==Pixel{0,0,0,255});
    CHECK(pixel(pixels,25,130)==Pixel{0,0,0,255});
    CHECK(pixel(pixels,35,35)==Pixel{0,255,0,255});
    CHECK(renderer->stats().draw_calls==1);REQUIRE(frame.end());
}

TEST_CASE("UI GPU uses logical coordinates and clips scaled text and boxes", "[ui][opengl]") {
    auto h = create();
    auto renderer = render::make_ui_renderer(h.device);
    REQUIRE(renderer);
    auto draws = list();
    draws.logical_size = {128, 96};
    draws.commands.emplace_back(ui::BoxDraw{{10, 10, 20, 20}, full, {0, 0, 1, 1}, {}, 0, 0});
    draws.commands.emplace_back(
        ui::TextDraw{"MMMM", {40, 10}, {44, 14, 20, 10}, font(), 18, {1, 1, 1, 1}});
    auto frame = begin(h);
    rendered(renderer->render(frame, draws));
    const auto pixels = read(h);
    CHECK(pixel(pixels, 30, 30) == Pixel{0, 0, 255, 255});
    CHECK(pixel(pixels, 15, 15) == Pixel{0, 0, 0, 255});
    bool ink{};
    std::size_t outside{};
    for (u32 y = 0; y < extent.height; ++y)
        for (u32 x = 70; x < extent.width; ++x) {
            if (x >= 88 && x < 128 && y >= 28 && y < 48)
                ink |= pixel(pixels, x, y)[0] > 0;
            else
                outside += pixel(pixels, x, y)[0] > 0;
        }
    CHECK(ink);
    CHECK(outside == 0);
}

TEST_CASE("UI GPU renders live screens and rejects incompatible submission", "[ui][opengl]") {
    auto h = create();
    auto renderer = render::make_ui_renderer(h.device);
    REQUIRE(renderer);
    ui::Screen screen{ui::dark_theme(font())};
    auto button = screen.column().position({8, 8}).width(180).button("Click me");
    input::Frame input{.logical_size = {256, 192}, .framebuffer = extent};
    REQUIRE(screen.update(input, 0));
    const auto b = button.bounds();
    input.pointer = {b.x + 8, b.y + 8};
    input.events = {{.kind = input::EventKind::pointer_down, .position = input.pointer},
                    {.kind = input::EventKind::pointer_up, .position = input.pointer}};
    REQUIRE(screen.update(input, 0));
    REQUIRE(button.clicked());
    auto frame = begin(h);
    rendered(renderer->render(frame, screen));
    CHECK(button.clicked());
    const auto pixels = read(h);
    auto invalid = list();
    invalid.framebuffer = {128, 96};
    CHECK_FALSE(renderer->render(frame, invalid));
    invalid = list();
    invalid.commands.emplace_back(ui::BoxDraw{{0, 0, 10, 10}, full, {1, 0, 0, 1}, {}, 0, 0});
    invalid.commands.emplace_back(ui::BoxDraw{
        {0, 0, std::numeric_limits<float>::infinity(), 10}, full, {0, 1, 0, 1}, {}, 0, 0});
    CHECK_FALSE(renderer->render(frame, invalid));
    CHECK(read(h) == pixels);
    auto moved = std::move(*renderer);
    CHECK_FALSE(renderer->render(frame, screen));
    rendered(moved.render(frame, screen));
    REQUIRE(frame.end());
    CHECK_FALSE(moved.render(frame, screen));
    {
        auto other = create();
        auto other_frame = begin(other);
        CHECK_FALSE(moved.render(other_frame, screen));
        REQUIRE(other_frame.end());
    }
    // Destroy the first device's objects while its own context is current.
    REQUIRE(h.window.make_current());
}
TEST_CASE("UI cached preview survives sidebar geometry changes across frames", "[ui][opengl][regression]") {
    auto h=create();
    auto renderer=render::make_ui_renderer(h.device); REQUIRE(renderer);
    const auto pixels=std::make_shared<gfx::ImageData>(gfx::ImageData{{1,1},
        {std::byte{255},std::byte{0},std::byte{0},std::byte{255}}});
    const auto face=font();
    for(const auto count : {2,60,3,80,2}) {
        auto draws=list();
        draws.commands.emplace_back(ui::ImageDraw{{0,0,128,192},full,pixels,{},1});
        for(int i=0;i<count;++i) {
            draws.commands.emplace_back(ui::BoxDraw{{130,0,126,192},full,{.1F,.2F,.3F,1},{},0,0});
            draws.commands.emplace_back(ui::TextDraw{"Panel " + std::to_string(i),{132,12},full,face,20,{1,1,1,1}});
        }
        auto frame=begin(h);
        rendered(renderer->render(frame,draws));
        CHECK(pixel(read(h),64,96)==Pixel{255,0,0,255});
        REQUIRE(frame.end());
    }
}

TEST_CASE("UI GPU image views decode sRGB preserve painter order and recycle revisions",
          "[ui][opengl]") {
    auto h = create();
    auto renderer = render::make_ui_renderer(h.device);
    REQUIRE(renderer);
    auto pixels = std::make_shared<gfx::ImageData>(
        gfx::ImageData{{1, 2},
                       {std::byte{128}, std::byte{128}, std::byte{128}, std::byte{255},
                        std::byte{255}, std::byte{0}, std::byte{0}, std::byte{128}}});
    const auto identity = std::make_shared<char>();
    auto draws = list();
    draws.logical_size = {128, 96};
    draws.commands.emplace_back(ui::BoxDraw{{0, 0, 128, 96}, full, {0, 0, 1, 1}, {}, 0, 0});
    draws.commands.emplace_back(
        ui::ImageDraw{{10, 10, 40, 60}, {20, 10, 20, 60}, pixels, identity, 1});
    draws.commands.emplace_back(ui::BoxDraw{{25, 25, 10, 10}, full, {0, 1, 0, 1}, {}, 0, 0});
    auto frame = begin(h);
    using CreateSamplers = void (*)(i32, u32*);
    using BindSampler = void (*)(u32, u32);
    using DeleteSamplers = void (*)(i32, const u32*);
    using ActiveTexture = void (*)(u32);
    using GetInteger = void (*)(u32, i32*);
    using GetIndexedInteger = void (*)(u32, u32, i32*);
    const auto create_sampler =
        reinterpret_cast<CreateSamplers>(h.access.resolve("glCreateSamplers"));
    const auto bind_sampler = reinterpret_cast<BindSampler>(h.access.resolve("glBindSampler"));
    const auto delete_sampler =
        reinterpret_cast<DeleteSamplers>(h.access.resolve("glDeleteSamplers"));
    const auto active_texture =
        reinterpret_cast<ActiveTexture>(h.access.resolve("glActiveTexture"));
    const auto get_integer = reinterpret_cast<GetInteger>(h.access.resolve("glGetIntegerv"));
    const auto get_indexed =
        reinterpret_cast<GetIndexedInteger>(h.access.resolve("glGetIntegeri_v"));
    REQUIRE(create_sampler);
    REQUIRE(bind_sampler);
    REQUIRE(delete_sampler);
    REQUIRE(active_texture);
    REQUIRE(get_integer);
    REQUIRE(get_indexed);
    struct SamplerScope {
        u32 value{};
        DeleteSamplers destroy;
        ~SamplerScope() { destroy(1, &value); }
    } external_sampler{0, delete_sampler};
    create_sampler(1, &external_sampler.value);
    bind_sampler(0, external_sampler.value);
    active_texture(0x84C0 + 5); // GL_TEXTURE5
    i32 external_texture{};
    get_indexed(0x8069, 0, &external_texture); // GL_TEXTURE_BINDING_2D
    rendered(renderer->render(frame, draws));
    i32 restored_sampler{}, restored_texture{}, restored_active{};
    get_indexed(0x8919, 0, &restored_sampler); // GL_SAMPLER_BINDING
    get_indexed(0x8069, 0, &restored_texture);
    get_integer(0x84E0, &restored_active); // GL_ACTIVE_TEXTURE
    CHECK(static_cast<u32>(restored_sampler) == external_sampler.value);
    CHECK(restored_texture == external_texture);
    CHECK(restored_active == 0x84C0 + 5);
    const auto first = read(h);
    // 128 sRGB decodes to ~55/255 linear; the frame is explicitly linear.
    auto gray = pixel(first, 45, 25);
    for (int i = 0; i < 3; ++i)
        CHECK(std::abs(static_cast<int>(gray[static_cast<std::size_t>(i)]) - 55) <= 1);
    CHECK(pixel(first, 30, 25) == Pixel{0, 0, 255, 255});
    CHECK(pixel(first, 90, 25) == Pixel{0, 0, 255, 255});
    CHECK(pixel(first, 60, 60) == Pixel{0, 255, 0, 255});
    const auto alpha = pixel(first, 45, 135);
    CHECK(std::abs(static_cast<int>(alpha[0]) - 128) <= 1);
    CHECK(alpha[1] == 0);
    CHECK(std::abs(static_cast<int>(alpha[2]) - 127) <= 1);
    CHECK(renderer->stats().images == 1);
    CHECK(renderer->stats().image_uploads == 1);
    CHECK(renderer->stats().cached_images == 1);
    CHECK(renderer->stats().draw_calls == 3);
    rendered(renderer->render(frame, draws));
    CHECK(renderer->stats().image_uploads == 0);
    CHECK(read(h) == first);
    auto replacement = std::make_shared<gfx::ImageData>(*pixels);
    replacement->pixels[0] = std::byte{255};
    auto& image = std::get<ui::ImageDraw>(draws.commands[1]);
    image.pixels = replacement;
    image.revision = 2;
    rendered(renderer->render(frame, draws));
    CHECK(renderer->stats().image_uploads == 1);
    CHECK(renderer->stats().cached_images == 1);
    CHECK(pixel(read(h), 45, 25)[0] == 255);
    image.revision = 3;
    rendered(renderer->render(frame, draws));
    CHECK(renderer->stats().image_uploads == 1);
    // Same extent reuses storage; a different extent replaces it transparently.
    image.pixels = std::make_shared<gfx::ImageData>(
        gfx::ImageData{{1, 1}, {std::byte{0}, std::byte{255}, std::byte{0}, std::byte{255}}});
    rendered(renderer->render(frame, draws));
    CHECK(renderer->stats().image_uploads == 1);
    CHECK(renderer->stats().cached_images == 1);
    CHECK(pixel(read(h), 45, 25) == Pixel{0, 255, 0, 255});
    auto bad = list();
    bad.commands.emplace_back(ui::ImageDraw{full, full, std::make_shared<gfx::ImageData>(), {}, 0});
    CHECK_FALSE(renderer->render(frame, bad));
    rendered(renderer->render(frame, list()));
    CHECK(renderer->stats().cached_images == 0);
    REQUIRE(frame.end());
    REQUIRE(renderer->clear_cache(h.device));
    for (const auto& message : h.device.take_debug_messages()) {
        INFO(message.message);
        CHECK(message.severity == opengl::DebugSeverity::notification);
    }
}
