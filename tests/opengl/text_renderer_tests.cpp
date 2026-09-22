#include <vng/opengl/buffer.hpp>
#include <vng/opengl/device.hpp>
#include <vng/opengl/frame.hpp>
#include <vng/text_opengl/text_renderer.hpp>
#include <vng/render/frame.hpp>
#include <vng/text/text_renderer.hpp>
#include <vng/text/font.hpp>
#include <vng/text/font_provider.hpp>
#include <vng/opengl/gpu_mesh.hpp>
#include <vng/render/program.hpp>
#include <vng/shader/shader.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "../support/glfw_opengl.hpp"

namespace {

constexpr vng::Extent2D extent{256, 128};
using Pixel = std::array<std::uint8_t, 4>;

struct Harness final {
    vng::glfw_opengl::Window window;
    vng::opengl::CurrentContextAccess access;
    vng::opengl::Device device;
};

Harness create_harness()
{
    auto window = vng::test::create_hidden_opengl_window(
        extent.width, extent.height, "vng text renderer test");
    if (!window) {
        std::cerr << "OpenGL text test skipped: " << window.error().message << '\n';
        std::exit(77);
    }
    auto access = window->make_current();
    if (!access) {
        std::cerr << "OpenGL text test skipped: " << access.error().message << '\n';
        std::exit(77);
    }
    auto device = vng::opengl::Device::create(*access);
    if (!device) INFO(device.error().message);
    REQUIRE(device);
    return {std::move(*window), *access, std::move(*device)};
}

vng::text::Font load_font()
{
    auto font = vng::text::Font::load(VNG_TEST_FONT_PATH);
    if (!font) INFO(font.error().message);
    REQUIRE(font);
    return std::move(*font);
}

vng::opengl::Frame begin_clear(Harness& harness)
{
    auto frame = vng::render::begin_frame(
        harness.device,
        vng::render::FrameDesc{
            .extent = extent,
            .color_encoding = vng::render::ColorEncoding::linear,
            .clear_color = std::array<float, 4>{0, 0, 0, 1},
            .clear_depth = 1.0F,
        });
    if (!frame) INFO(frame.error().message);
    REQUIRE(frame);
    return std::move(*frame);
}

std::vector<Pixel> read_pixels(const Harness& harness)
{
    using ReadPixels = void (*)(std::int32_t, std::int32_t,
        std::int32_t, std::int32_t, std::uint32_t, std::uint32_t, void*);
    const auto read = reinterpret_cast<ReadPixels>(
        harness.access.resolve("glReadPixels"));
    REQUIRE(read != nullptr);
    std::vector<Pixel> pixels(extent.width * extent.height);
    read(0, 0, static_cast<std::int32_t>(extent.width),
         static_cast<std::int32_t>(extent.height),
         0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, pixels.data());
    return pixels;
}

bool has_ink(const Pixel& pixel)
{
    return pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0;
}

void require_rendered(const std::expected<void, vng::opengl::Diagnostic>& result)
{
    if (!result) INFO(result.error().message);
    REQUIRE(result);
}

} // namespace

TEST_CASE("text renderer batches font and size variations and retains warm caches",
          "[opengl][text]")
{
    auto harness = create_harness();
    auto font = load_font();
    auto alternate_font = load_font();
    auto renderer = vng::render::make_text_renderer(harness.device, font);
    if (!renderer) INFO(renderer.error().message);
    REQUIRE(renderer);
    STATIC_CHECK(vng::render::RendererFor<
        vng::opengl::TextRenderer, vng::opengl::Frame>);

    const std::array tickets{
        vng::render::TextDraw{
            .text = "Hello!", .position = {12, 8}, .size = 22,
            .color = {1, 0.5F, 0.25F, 1},
        },
        vng::render::TextDraw{
            .text = "Large", .position = {12, 40}, .size = 32,
        },
        vng::render::TextDraw{
            .text = "Other font", .position = {12, 90}, .size = 18,
            .font = alternate_font,
        },
    };

    auto frame = begin_clear(harness);
    require_rendered(renderer->render(frame, tickets));
    const auto first = renderer->stats();
    CHECK(first.glyphs > 0);
    CHECK(first.glyph_uploads > 0);
    CHECK(first.atlas_pages == 1);
    CHECK(first.draw_calls == 1);
    const auto pixels = read_pixels(harness);
    CHECK(std::count_if(pixels.begin(), pixels.end(), has_ink) > 100);
    REQUIRE(frame.end());

    auto second_frame = begin_clear(harness);
    const auto view = vng::render::RenderView::without_camera(extent);
    require_rendered(renderer->render(second_frame, view, tickets));
    const auto second = renderer->stats();
    CHECK(second.glyphs == first.glyphs);
    CHECK(second.glyph_uploads == 0);
    CHECK(second.atlas_pages == first.atlas_pages);
    CHECK(second.draw_calls == first.draw_calls);
    CHECK(second.layout_hits >= tickets.size());
    CHECK(read_pixels(harness) == pixels);
    auto measured = renderer->measure(tickets.front());
    REQUIRE(measured);
    CHECK(measured->width > 0.0F);
    CHECK(measured->height > 0.0F);
}

TEST_CASE("text keeps painter order across atlas pages and blends opacity correctly",
          "[opengl][text]")
{
    auto harness = create_harness();
    auto first_font = load_font();
    auto second_font = load_font();
    auto renderer = vng::render::make_text_renderer(
        harness.device, first_font,
        vng::render::TextRendererOptions{.atlas_size = 64});
    if (!renderer) INFO(renderer.error().message);
    REQUIRE(renderer);

    // Identical glyphs from independently loaded font handles have identical
    // coverage but separate cache entries. Their large bitmaps force separate
    // pages. The A/B/A submission order must not become A/A/B during batching.
    const std::array tickets{
        vng::render::TextDraw{
            .text = "M", .position = {24, 20}, .size = 60,
            .color = {1, 0, 0, 0.5F}, .font = first_font,
        },
        vng::render::TextDraw{
            .text = "M", .position = {24, 20}, .size = 60,
            .color = {0, 1, 0, 0.5F}, .font = second_font,
        },
        vng::render::TextDraw{
            .text = "M", .position = {24, 20}, .size = 60,
            .color = {0, 0, 1, 0.5F}, .font = first_font,
        },
    };
    auto frame = begin_clear(harness);
    require_rendered(renderer->render(frame, tickets));
    CHECK(renderer->stats().atlas_pages == 2);
    CHECK(renderer->stats().draw_calls == 3);
    CHECK(renderer->stats().glyphs == 3);

    const auto pixels = read_pixels(harness);
    const auto strongest = std::max_element(
        pixels.begin(), pixels.end(),
        [](const Pixel& lhs, const Pixel& rhs) { return lhs[2] < rhs[2]; });
    REQUIRE(strongest != pixels.end());
    CHECK(std::abs(static_cast<int>((*strongest)[0]) - 32) <= 2);
    CHECK(std::abs(static_cast<int>((*strongest)[1]) - 64) <= 2);
    CHECK(std::abs(static_cast<int>((*strongest)[2]) - 128) <= 2);
    CHECK((*strongest)[3] == 255);
}

TEST_CASE("text clipping preserves glyph sampling inside the clip rectangle",
          "[opengl][text]")
{
    auto harness = create_harness();
    auto renderer = vng::render::make_text_renderer(harness.device, load_font());
    REQUIRE(renderer);
    vng::render::TextDraw ticket{
        .text = "MMMM", .position = {12, 8}, .size = 48,
    };
    auto full_frame = begin_clear(harness);
    require_rendered(renderer->render(full_frame, ticket));
    const auto full = read_pixels(harness);
    REQUIRE(full_frame.end());

    ticket.clip = vng::render::TextClip{30, 25, 48, 18};
    auto clipped_frame = begin_clear(harness);
    require_rendered(renderer->render(clipped_frame, ticket));
    const auto clipped = read_pixels(harness);
    std::size_t visible_inside = 0;
    std::size_t visible_outside = 0;
    int largest_inside_difference = 0;
    for (vng::u32 y = 0; y < extent.height; ++y) {
        const auto top_y = extent.height - 1 - y;
        for (vng::u32 x = 0; x < extent.width; ++x) {
            const auto index = y * extent.width + x;
            const bool inside = x >= 30 && x < 78 && top_y >= 25 && top_y < 43;
            if (inside) {
                visible_inside += has_ink(clipped[index]);
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    largest_inside_difference = std::max(
                        largest_inside_difference,
                        std::abs(static_cast<int>(clipped[index][channel])
                                 - static_cast<int>(full[index][channel])));
                }
            } else {
                visible_outside += has_ink(clipped[index]);
            }
        }
    }
    CHECK(visible_inside > 30);
    CHECK(visible_outside == 0);
    CHECK(largest_inside_difference <= 1);
}

TEST_CASE("text rejects invalid fonts sizes and coordinates without touching a draw",
          "[opengl][text]")
{
    auto harness = create_harness();
    auto renderer = vng::render::make_text_renderer(harness.device);
    REQUIRE(renderer);
    auto frame = begin_clear(harness);
    vng::render::TextDraw ticket{.text = "invalid", .position = {12, 8}};

    auto no_font = renderer->render(frame, ticket);
    REQUIRE_FALSE(no_font);
    CHECK_FALSE(no_font.error().message.empty());

    ticket.font = load_font();
    ticket.size = 0;
    REQUIRE_FALSE(renderer->render(frame, ticket));
    ticket.size = std::numeric_limits<vng::u32>::max();
    REQUIRE_FALSE(renderer->render(frame, ticket));
    ticket.size = 24;
    ticket.position.x = std::numeric_limits<float>::infinity();
    REQUIRE_FALSE(renderer->render(frame, ticket));
    ticket.position.x = 12;
    ticket.position.y = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(renderer->render(frame, ticket));

    const auto empty = read_pixels(harness);
    CHECK(std::none_of(empty.begin(), empty.end(), has_ink));
    ticket.position.y = 8;
    require_rendered(renderer->render(frame, ticket));
    const auto recovered = read_pixels(harness);
    CHECK(std::any_of(recovered.begin(), recovered.end(), has_ink));
}

TEST_CASE("text atlas uploads and sampling tolerate unrelated OpenGL pixel state",
          "[opengl][text]")
{
    auto harness = create_harness();
    auto renderer = vng::render::make_text_renderer(harness.device, load_font());
    REQUIRE(renderer);
    const vng::render::TextDraw ticket{
        .text = "Pixel state", .position = {12.3F, 15.4F}, .size = 30,
    };
    auto first_frame = begin_clear(harness);
    require_rendered(renderer->render(first_frame, ticket));
    const auto reference = read_pixels(harness);
    REQUIRE(first_frame.end());
    REQUIRE(renderer->clear_cache(harness.device));

    using BindBuffer = void (*)(std::uint32_t, std::uint32_t);
    using PixelStore = void (*)(std::uint32_t, std::int32_t);
    using CreateSamplers = void (*)(std::int32_t, std::uint32_t*);
    using DeleteSamplers = void (*)(std::int32_t, const std::uint32_t*);
    using SamplerParameter = void (*)(std::uint32_t, std::uint32_t, std::int32_t);
    using BindSampler = void (*)(std::uint32_t, std::uint32_t);
    using GetInteger = void (*)(std::uint32_t, std::int32_t*);
    const auto bind_buffer = reinterpret_cast<BindBuffer>(
        harness.access.resolve("glBindBuffer"));
    const auto pixel_store = reinterpret_cast<PixelStore>(
        harness.access.resolve("glPixelStorei"));
    const auto create_samplers = reinterpret_cast<CreateSamplers>(
        harness.access.resolve("glCreateSamplers"));
    const auto delete_samplers = reinterpret_cast<DeleteSamplers>(
        harness.access.resolve("glDeleteSamplers"));
    const auto sampler_parameter = reinterpret_cast<SamplerParameter>(
        harness.access.resolve("glSamplerParameteri"));
    const auto bind_sampler = reinterpret_cast<BindSampler>(
        harness.access.resolve("glBindSampler"));
    const auto get_integer = reinterpret_cast<GetInteger>(
        harness.access.resolve("glGetIntegerv"));
    REQUIRE(bind_buffer != nullptr);
    REQUIRE(pixel_store != nullptr);
    REQUIRE(create_samplers != nullptr);
    REQUIRE(delete_samplers != nullptr);
    REQUIRE(sampler_parameter != nullptr);
    REQUIRE(bind_sampler != nullptr);
    REQUIRE(get_integer != nullptr);

    auto unpack = vng::opengl::Buffer::create(
        harness.device, vng::opengl::BufferCreateInfo{.size = 1024});
    REQUIRE(unpack);
    constexpr std::uint32_t unpack_buffer = 0x88EC;
    constexpr std::uint32_t unpack_binding = 0x88EF;
    constexpr std::uint32_t unpack_alignment = 0x0CF5;
    constexpr std::uint32_t unpack_row_length = 0x0CF2;
    constexpr std::uint32_t unpack_skip_rows = 0x0CF3;
    constexpr std::uint32_t unpack_skip_pixels = 0x0CF4;
    std::uint32_t sampler = 0;
    create_samplers(1, &sampler);
    REQUIRE(sampler != 0);
    sampler_parameter(sampler, 0x2801 /* GL_TEXTURE_MIN_FILTER */, 0x2600 /* GL_NEAREST */);
    sampler_parameter(sampler, 0x2800 /* GL_TEXTURE_MAG_FILTER */, 0x2600 /* GL_NEAREST */);
    bind_sampler(0, sampler);
    bind_buffer(unpack_buffer, unpack->native_handle());
    pixel_store(unpack_alignment, 8);
    pixel_store(unpack_row_length, 512);
    pixel_store(unpack_skip_rows, 7);
    pixel_store(unpack_skip_pixels, 11);

    auto second_frame = begin_clear(harness);
    {
        auto previous = second_frame.render_context();
        auto graphics = previous.graphics_state();
        REQUIRE(graphics.set(vng::render::DepthTest{true}));
        REQUIRE(graphics.set(vng::render::DepthCompare::never));
        REQUIRE(graphics.set(vng::render::CullMode::front));
        REQUIRE(graphics.set(vng::opengl::PolygonMode::line));
        REQUIRE(graphics.set(vng::render::BlendMode::disabled));
    }
    auto rendered = renderer->render(second_frame, ticket);
    std::array<std::int32_t, 5> retained{};
    get_integer(unpack_binding, &retained[0]);
    get_integer(unpack_alignment, &retained[1]);
    get_integer(unpack_row_length, &retained[2]);
    get_integer(unpack_skip_rows, &retained[3]);
    get_integer(unpack_skip_pixels, &retained[4]);

    // Restore test-owned native state even if render returned a diagnostic.
    bind_buffer(unpack_buffer, 0);
    pixel_store(unpack_alignment, 4);
    pixel_store(unpack_row_length, 0);
    pixel_store(unpack_skip_rows, 0);
    pixel_store(unpack_skip_pixels, 0);
    bind_sampler(0, 0);
    delete_samplers(1, &sampler);

    require_rendered(rendered);
    CHECK(retained == std::array<std::int32_t, 5>{
        static_cast<std::int32_t>(unpack->native_handle()), 8, 512, 7, 11});
    CHECK(renderer->stats().glyph_uploads > 0);
    CHECK(read_pixels(harness) == reference);

    // Another handle borrows the same state. A subsequent renderer explicitly
    // chooses opaque drawing rather than relying on a fresh-stream reset.
    auto following = second_frame.render_context();
    auto graphics = following.graphics_state();
    REQUIRE(graphics.set(vng::render::DepthTest{true}));
    using IsEnabledIndexed = std::uint8_t (*)(std::uint32_t, std::uint32_t);
    const auto is_enabled = reinterpret_cast<IsEnabledIndexed>(
        harness.access.resolve("glIsEnabledi"));
    REQUIRE(is_enabled != nullptr);
    CHECK(is_enabled(0x0BE2 /* GL_BLEND */, 0) == 1);
    REQUIRE(graphics.set(vng::render::BlendMode::disabled));
    CHECK(is_enabled(0x0BE2 /* GL_BLEND */, 0) == 0);
}

TEST_CASE("parent rendering continues through its original context after a text child",
          "[opengl][text][composition]")
{
    using namespace vng;
    auto harness = create_harness();
    auto text_renderer = render::make_text_renderer(harness.device, load_font());
    REQUIRE(text_renderer);
    struct Position : gfx::Semantic<Vec2> {};
    using Vertex = gfx::Record<Position>;
    using VertexOut = shader::VertexOutputs<shader::ClipPosition>;
    auto vertex = shader::vertex<shader::VertexInputs<Position>, VertexOut>([](auto& s) {
        return s.output(dsl::field<shader::ClipPosition>(dsl::vec4(s.input(Position{}), 0.0F, 1.0F)));
    });
    auto fragment = shader::fragment<shader::FragmentInputs<>, shader::FragmentOutputs<shader::Color<0>>>([](auto& s) {
        return s.output(dsl::field<shader::Color<0>>(s.constant(Vec4{0, 1, 0, 1})));
    });
    REQUIRE(vertex); REQUIRE(fragment);
    auto ir = shader::link(std::move(*vertex), std::move(*fragment));
    REQUIRE(ir);
    auto program = render::compile_program(harness.device, *ir);
    REQUIRE(program);
    gfx::Mesh<Vertex> mesh(3);
    mesh.vertices()[0].set(Position{}, {0.1F, -0.8F});
    mesh.vertices()[1].set(Position{}, {0.9F, -0.8F});
    mesh.vertices()[2].set(Position{}, {0.5F, 0.8F});
    mesh.add_face(0, 1, 2);
    auto gpu = opengl::upload_mesh(harness.device, mesh);
    REQUIRE(gpu);
    auto frame = begin_clear(harness);
    auto parent = frame.render_context();
    auto graphics = parent.graphics_state();
    REQUIRE(parent.run(*program));
    REQUIRE(graphics.set(opengl::PolygonMode::line));
    REQUIRE(graphics.set(render::CullMode::front));
    REQUIRE(graphics.set(render::DepthState{true, true, render::DepthCompare::never}));
    require_rendered(text_renderer->render(frame, render::TextDraw{
        .text="Child", .position={8, 8}, .size=24}));
    CHECK(parent.active());
    // Child destruction drops only its borrowed handle. The parent's original
    // handles can choose their next program/state and issue a real draw.
    REQUIRE(parent.run(*program));
    REQUIRE(graphics.set(render::DepthState{false, false}));
    REQUIRE(graphics.set(render::CullMode::none));
    REQUIRE(graphics.set(render::BlendMode::disabled));
    REQUIRE(graphics.set(opengl::PolygonMode::fill));
    REQUIRE(parent.draw(*gpu));
    const auto pixels = read_pixels(harness);
    CHECK(pixels[64 * extent.width + 192] == Pixel{0, 255, 0, 255});
    CHECK(std::count_if(pixels.begin(), pixels.end(), [](const Pixel& pixel) {
        return pixel[0] > 100 && pixel[1] > 100 && pixel[2] > 100;
    }) > 50);
    REQUIRE(frame.end());
    CHECK_FALSE(parent.active());
    CHECK_FALSE(graphics.set(render::DepthTest{true}));
}

TEST_CASE("text preserves other texture targets on its borrowed texture unit",
          "[opengl][text]")
{
    auto harness = create_harness();
    auto renderer = vng::render::make_text_renderer(harness.device, load_font());
    REQUIRE(renderer);

    using CreateTextures = void (*)(std::uint32_t, std::int32_t, std::uint32_t*);
    using DeleteTextures = void (*)(std::int32_t, const std::uint32_t*);
    using BindTextureUnit = void (*)(std::uint32_t, std::uint32_t);
    using ActiveTexture = void (*)(std::uint32_t);
    using GetInteger = void (*)(std::uint32_t, std::int32_t*);
    using GetIntegerIndexed = void (*)(std::uint32_t, std::uint32_t, std::int32_t*);
    const auto create_textures = reinterpret_cast<CreateTextures>(
        harness.access.resolve("glCreateTextures"));
    const auto delete_textures = reinterpret_cast<DeleteTextures>(
        harness.access.resolve("glDeleteTextures"));
    const auto bind_texture_unit = reinterpret_cast<BindTextureUnit>(
        harness.access.resolve("glBindTextureUnit"));
    const auto active_texture = reinterpret_cast<ActiveTexture>(
        harness.access.resolve("glActiveTexture"));
    const auto get_integer = reinterpret_cast<GetInteger>(
        harness.access.resolve("glGetIntegerv"));
    const auto get_indexed = reinterpret_cast<GetIntegerIndexed>(
        harness.access.resolve("glGetIntegeri_v"));
    REQUIRE(create_textures != nullptr);
    REQUIRE(delete_textures != nullptr);
    REQUIRE(bind_texture_unit != nullptr);
    REQUIRE(active_texture != nullptr);
    REQUIRE(get_integer != nullptr);
    REQUIRE(get_indexed != nullptr);

    constexpr std::uint32_t texture_3d = 0x806F;
    constexpr std::uint32_t texture_binding_3d = 0x806A;
    constexpr std::uint32_t texture_binding_2d = 0x8069;
    constexpr std::uint32_t texture0 = 0x84C0;
    constexpr std::uint32_t active_texture_binding = 0x84E0;
    std::uint32_t other_texture = 0;
    create_textures(texture_3d, 1, &other_texture);
    REQUIRE(other_texture != 0);
    bind_texture_unit(0, 0);
    bind_texture_unit(0, other_texture);
    active_texture(texture0 + 3);

    auto frame = begin_clear(harness);
    const auto rendered = renderer->render(frame, vng::render::TextDraw{
        .text = "Preserve textures", .position = {8, 8},
    });
    std::int32_t retained_3d{}, retained_2d{}, retained_active{};
    get_indexed(texture_binding_3d, 0, &retained_3d);
    get_indexed(texture_binding_2d, 0, &retained_2d);
    get_integer(active_texture_binding, &retained_active);

    // Deleting the test texture is safe even when an assertion below fails.
    bind_texture_unit(0, 0);
    active_texture(texture0);
    delete_textures(1, &other_texture);

    require_rendered(rendered);
    CHECK(retained_3d == static_cast<std::int32_t>(other_texture));
    CHECK(retained_2d == 0);
    CHECK(retained_active == static_cast<std::int32_t>(texture0 + 3));
    const auto pixels = read_pixels(harness);
    CHECK(std::any_of(pixels.begin(), pixels.end(), has_ink));
}

TEST_CASE("fully transparent text does not create glyphs or atlas pages",
          "[opengl][text]")
{
    auto harness = create_harness();
    auto renderer = vng::render::make_text_renderer(
        harness.device, load_font(),
        vng::render::TextRendererOptions{.atlas_size = 64});
    REQUIRE(renderer);
    auto frame = begin_clear(harness);
    // This is a valid font size, but its glyphs cannot fit the tiny atlas.
    // An invisible submission should never attempt to rasterize or pack them.
    const vng::render::TextDraw hidden{
        .text = "Invisible", .position = {8, 8}, .size = 4096,
        .color = {1, 1, 1, 0},
    };
    require_rendered(renderer->render(frame, hidden));
    const auto stats = renderer->stats();
    CHECK(stats.glyphs == 0);
    CHECK(stats.draw_calls == 0);
    CHECK(stats.glyph_uploads == 0);
    CHECK(stats.atlas_pages == 0);
    const auto pixels = read_pixels(harness);
    CHECK(std::none_of(pixels.begin(), pixels.end(), has_ink));
}

TEST_CASE("text providers rebuild fonts transactionally and keep external fonts independent",
          "[opengl][text][resources]")
{
    auto harness = create_harness();
    auto calls = std::make_shared<int>(0);
    auto recipe = vng::resources::share_provider(vng::resources::provider(
        [calls, file = vng::providers::font_file(VNG_TEST_FONT_PATH)] {
            ++*calls;
            return file.provide();
        }));
    auto build = [&] {
        auto builder = vng::render::text_renderer_builder(harness.device);
        builder.font(recipe);
        auto first = builder.build();
        REQUIRE(first);
        auto second = builder.build();
        REQUIRE(second);
        return first;
    };
    auto renderer = build();
    CHECK(*calls == 2);
    const auto external = load_font();
    const std::array tickets{
        vng::render::TextDraw{.text = "Reload", .position = {8, 8}},
        vng::render::TextDraw{.text = "External", .position = {8, 42}, .font = external},
    };
    auto frame = begin_clear(harness);
    require_rendered(renderer->render(frame, tickets));
    const auto pixels = read_pixels(harness);
    CHECK(renderer->stats().glyph_uploads > 0);
    auto busy = renderer->reload_font(harness.device);
    REQUIRE_FALSE(busy);
    CHECK(*calls == 2);
    REQUIRE(frame.end());

    auto bad = renderer->reload_font(harness.device,
        vng::providers::font_file("/vng-nonexistent-font-for-reload-test.ttf"));
    REQUIRE_FALSE(bad);
    REQUIRE(bad.error().cause_as<vng::text::Diagnostic>());
    auto warm_frame = begin_clear(harness);
    require_rendered(renderer->render(warm_frame, tickets));
    CHECK(renderer->stats().glyph_uploads == 0);
    CHECK(read_pixels(harness) == pixels);
    REQUIRE(warm_frame.end());

    REQUIRE(renderer->reload_font(harness.device));
    CHECK(*calls == 3);
    CHECK(renderer->stats().atlas_pages == 0);
    auto refreshed = begin_clear(harness);
    require_rendered(renderer->render(refreshed, tickets));
    CHECK(renderer->stats().glyph_uploads > 0);
    CHECK(read_pixels(harness) == pixels);
    REQUIRE(refreshed.end());

    REQUIRE(renderer->replace_font(harness.device, external));
    auto missing = renderer->reload_font(harness.device);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == vng::resources::ErrorCode::no_provider);
    auto all = renderer->reload(harness.device);
    REQUIRE(all);
    CHECK(all->retained == std::vector<std::string>{"font"});
    CHECK(*calls == 3);
    auto adopted = vng::resources::provided(external, recipe);
    REQUIRE(renderer->replace_font(harness.device, std::move(adopted)));
    CHECK(*calls == 3);
    REQUIRE(renderer->reload(harness.device));
    CHECK(*calls == 4);
    CHECK(external.measure("External", 24));
}

TEST_CASE("text reload rejects reentrant mutation and a provider leaving a frame active",
          "[opengl][text][resources]")
{
    auto harness = create_harness();
    auto font = load_font();
    vng::opengl::TextRenderer* owner{};
    bool open_frame{};
    std::optional<vng::opengl::Frame> borrowed;
    auto recipe = vng::resources::provider([&]() -> vng::resources::Result<vng::text::Font> {
        if (owner) {
            CHECK_FALSE(owner->reload_font(harness.device));
            CHECK_FALSE(owner->replace_font(harness.device, font));
            CHECK_FALSE(owner->clear_cache(harness.device));
        }
        if (open_frame) borrowed.emplace(begin_clear(harness));
        return font;
    });
    auto builder = vng::render::text_renderer_builder(harness.device);
    auto renderer = builder.font(recipe).build();
    REQUIRE(renderer);
    owner = &*renderer;
    REQUIRE(renderer->reload_font(harness.device));
    open_frame = true;
    auto blocked = renderer->reload(harness.device);
    REQUIRE_FALSE(blocked);
    REQUIRE(borrowed);
    REQUIRE(borrowed->active());
    const vng::render::TextDraw ticket{.text = "Still alive", .position = {8, 8}};
    require_rendered(renderer->render(*borrowed, ticket));
    const auto pixels = read_pixels(harness);
    CHECK(std::any_of(pixels.begin(), pixels.end(), has_ink));
    REQUIRE(borrowed->end());
    borrowed.reset();
    open_frame = false;
    REQUIRE(renderer->reload(harness.device));

    // A callback may change its builder. This build retains the source and
    // options it started with, including when the builder drops its last copy.
    owner = nullptr;
    builder.font(vng::resources::provider([&]() -> vng::resources::Result<vng::text::Font> {
        builder.font(font).options({.atlas_size = 1});
        return font;
    }));
    auto snapshotted = builder.build();
    REQUIRE(snapshotted);
    REQUIRE(snapshotted->reload_font(harness.device));
    REQUIRE_FALSE(builder.build());
}
