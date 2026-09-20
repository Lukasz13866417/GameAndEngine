#include "../../examples/support/sun_softening.hpp"
#include "../support/glfw_opengl.hpp"

#include <vng/opengl/image.hpp>
#include <vng/opengl/render_target.hpp>
#include <vng/providers/target.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glad/gl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <utility>
#include <vector>

namespace {
using namespace vng;
namespace sun = example::sun;

glfw_opengl::Window hidden_window()
{
    auto window = test::create_hidden_opengl_window(64, 64, "Sun softening regression");
    if (!window) {
        std::cerr << "Sun softening test skipped: " << window.error().message << '\n';
        std::exit(77);
    }
    return std::move(*window);
}

render::FrameDesc frame_description(Extent2D extent)
{
    return {.extent = extent, .color_encoding = render::ColorEncoding::linear,
        .clear_color = std::array<f32, 4>{0, 0, 0, 0}, .clear_depth = {}};
}

std::vector<f32> constant_pixels(Extent2D extent, std::array<f32, 4> value)
{
    std::vector<f32> pixels(static_cast<std::size_t>(extent.width) * extent.height * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4)
        std::copy(value.begin(), value.end(), pixels.begin() + static_cast<std::ptrdiff_t>(i));
    return pixels;
}

struct NativeState final {
    GLint program{}, vao{}, draw_target{}, read_target{}, depth_compare{}, cull_face{};
    GLint blend_source{}, blend_destination{}, blend_equation{};
    GLint active_texture{}, texture_zero{}, sampler_zero{};
    GLboolean depth_test{}, depth_write{}, cull{}, blend{}, scissor{};
    std::array<GLint, 4> viewport{}, scissor_box{};
    std::array<GLboolean, 4> color_mask{};
    friend bool operator==(const NativeState&, const NativeState&) = default;
};

NativeState native_state()
{
    NativeState state;
    glGetIntegerv(GL_CURRENT_PROGRAM, &state.program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &state.vao);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &state.draw_target);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &state.read_target);
    glGetIntegerv(GL_DEPTH_FUNC, &state.depth_compare);
    glGetIntegerv(GL_CULL_FACE_MODE, &state.cull_face);
    glGetIntegeri_v(GL_BLEND_SRC_RGB, 0, &state.blend_source);
    glGetIntegeri_v(GL_BLEND_DST_RGB, 0, &state.blend_destination);
    glGetIntegeri_v(GL_BLEND_EQUATION_RGB, 0, &state.blend_equation);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &state.active_texture);
    glGetIntegeri_v(GL_TEXTURE_BINDING_2D, 0, &state.texture_zero);
    glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &state.sampler_zero);
    state.depth_test = glIsEnabled(GL_DEPTH_TEST);
    state.cull = glIsEnabled(GL_CULL_FACE);
    state.blend = glIsEnabledi(GL_BLEND, 0);
    state.scissor = glIsEnabledi(GL_SCISSOR_TEST, 0);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &state.depth_write);
    glGetIntegerv(GL_VIEWPORT, state.viewport.data());
    glGetIntegerv(GL_SCISSOR_BOX, state.scissor_box.data());
    glGetBooleani_v(GL_COLOR_WRITEMASK, 0, state.color_mask.data());
    return state;
}

std::array<GLint, 6> texture_sampling(const opengl::Image2D& image)
{
    std::array<GLint, 6> result{};
    constexpr std::array<GLenum, 6> fields{
        GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER, GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T,
        GL_TEXTURE_BASE_LEVEL, GL_TEXTURE_MAX_LEVEL};
    for (std::size_t i = 0; i < fields.size(); ++i)
        glGetTextureParameteriv(image.native_handle(), fields[i], &result[i]);
    return result;
}

struct ExternalSampler final {
    GLuint handle{};
    ExternalSampler() { glCreateSamplers(1, &handle); }
    ExternalSampler(const ExternalSampler&) = delete;
    ExternalSampler& operator=(const ExternalSampler&) = delete;
    ~ExternalSampler() { glDeleteSamplers(1, &handle); }
};

void check_no_debug_errors(const opengl::Device& device)
{
    for (const auto& message : device.take_debug_messages()) {
        INFO(message.message);
        CHECK(message.type != "error");
    }
    CHECK(device.take_lifecycle_diagnostics().empty());
    CHECK(glGetError() == GL_NO_ERROR);
}

bool same_pixels(const std::vector<opengl::Rgba32fPixel>& a,
    const std::vector<opengl::Rgba32fPixel>& b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](const auto& l, const auto& r) {
            return l.r == r.r && l.g == r.g && l.b == r.b && l.a == r.a;
        });
}

} // namespace

TEST_CASE("Sun softening is symmetric normalized HDR convolution with scoped graphics state",
    "[examples][sun][opengl][softening]")
{
    auto window = hidden_window();
    auto access = window.make_current();
    REQUIRE(access);
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    auto softening = sun::SunSoftening::create(*device);
    REQUIRE(softening);
    constexpr Extent2D extent{9, 9};
    auto source = opengl::Image2D::create(*device, extent.width, extent.height, gfx::ImageFormat::rgba32f);
    REQUIRE(source);
    REQUIRE(source->set_sampler({.min_filter = gfx::ImageFilter::nearest,
        .mag_filter = gfx::ImageFilter::nearest, .wrap_u = gfx::ImageWrap::repeat,
        .wrap_v = gfx::ImageWrap::repeat}));
    auto values = constant_pixels(extent, {0, 0, 0, 0.375F});
    const auto center = static_cast<std::size_t>(extent.width * (extent.height / 2) + extent.width / 2);
    values[center * 4] = 8;
    values[center * 4 + 1] = 4;
    values[center * 4 + 2] = 2;
    REQUIRE(source->write_rgba32f(0, 0, extent.width, extent.height, values));
    auto output = providers::render_target({.color = gfx::ImageFormat::rgba32f, .depth = false})
        .provide(*device, extent);
    REQUIRE(output);
    auto external_texture = opengl::Image2D::create(*device, 1, 1, gfx::ImageFormat::rgba8);
    REQUIRE(external_texture);
    REQUIRE(external_texture->bind_to_unit(0));
    ExternalSampler external_sampler;
    glBindSampler(0, external_sampler.handle);
    glActiveTexture(GL_TEXTURE3);
    auto frame = render::begin_frame(*device, *output, frame_description(extent));
    REQUIRE(frame);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GREATER);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);
    glEnablei(GL_BLEND, 0);
    glBlendFunci(0, GL_ONE, GL_ONE);
    glEnablei(GL_SCISSOR_TEST, 0);
    glScissor(1, 1, 2, 2);
    glColorMaski(0, GL_FALSE, GL_TRUE, GL_FALSE, GL_FALSE);
    const auto before_state = native_state();
    const auto before_sampling = texture_sampling(*source);
    auto applied = softening->apply(*frame, *source);
    INFO((applied ? "Softening applied" : applied.error().message));
    REQUIRE(applied);
    CHECK(native_state() == before_state);
    CHECK(texture_sampling(*source) == before_sampling);
    auto pixels = output->framebuffer().read_rgba32f(0, 0, 0, extent.width, extent.height);
    REQUIRE(pixels);
    CHECK((*pixels)[center].r > 1.0F);
    CHECK((*pixels)[center].r < 8.0F);
    CHECK((*pixels)[center + 1].r > 0.1F);
    std::array<double, 3> sums{};
    for (u32 y = 0; y < extent.height; ++y) {
        for (u32 x = 0; x < extent.width; ++x) {
            const auto& pixel = (*pixels)[static_cast<std::size_t>(y) * extent.width + x];
            const auto& horizontal = (*pixels)[static_cast<std::size_t>(y) * extent.width + extent.width - 1 - x];
            const auto& vertical = (*pixels)[static_cast<std::size_t>(extent.height - 1 - y) * extent.width + x];
            CHECK(pixel.r == Catch::Approx(horizontal.r).margin(0.00002));
            CHECK(pixel.r == Catch::Approx(vertical.r).margin(0.00002));
            CHECK(pixel.a == Catch::Approx(0.375F).margin(0.00001));
            CHECK(pixel.r >= 0.0F);
            sums[0] += pixel.r; sums[1] += pixel.g; sums[2] += pixel.b;
        }
    }
    CHECK(sums[0] == Catch::Approx(8).margin(0.0002));
    CHECK(sums[1] == Catch::Approx(4).margin(0.0002));
    CHECK(sums[2] == Catch::Approx(2).margin(0.0002));
    CHECK((*pixels)[0].r == 0.0F);
    CHECK_FALSE(softening->apply(*frame, output->color()));
    CHECK(native_state() == before_state);
    auto after_alias = output->framebuffer().read_rgba32f(0, 0, 0, extent.width, extent.height);
    REQUIRE(after_alias);
    CHECK(same_pixels(*pixels, *after_alias));
    REQUIRE(frame->end());
    CHECK_FALSE(softening->apply(*frame, *source));
    check_no_debug_errors(*device);
}

TEST_CASE("Sun softening preserves constant HDR and reuses typed texel size across extents",
    "[examples][sun][opengl][softening]")
{
    auto window = hidden_window();
    auto access = window.make_current();
    REQUIRE(access);
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    auto softening = sun::SunSoftening::create(*device);
    REQUIRE(softening);
    constexpr Extent2D initial{8, 6};
    auto output = providers::render_target({.color = gfx::ImageFormat::rgba32f, .depth = false})
        .provide(*device, initial);
    REQUIRE(output);
    auto wrong_size = opengl::Image2D::create(*device, 2, 2, gfx::ImageFormat::rgba32f);
    REQUIRE(wrong_size);
    for (const auto extent : {initial, Extent2D{13, 9}}) {
        REQUIRE(output->resize(*device, extent));
        auto source = opengl::Image2D::create(*device, extent.width, extent.height, gfx::ImageFormat::rgba32f);
        REQUIRE(source);
        auto not_hdr = opengl::Image2D::create(*device, extent.width, extent.height, gfx::ImageFormat::rgba8);
        REQUIRE(not_hdr);
        auto ldr_output = providers::render_target({.color = gfx::ImageFormat::rgba8, .depth = false})
            .provide(*device, extent);
        REQUIRE(ldr_output);
        const auto values = constant_pixels(extent, {3.25F, 1.5F, 0.125F, 0.375F});
        REQUIRE(source->write_rgba32f(0, 0, extent.width, extent.height, values));
        auto frame = render::begin_frame(*device, *output, frame_description(extent));
        REQUIRE(frame);
        CHECK_FALSE(softening->apply(*frame, *wrong_size));
        CHECK_FALSE(softening->apply(*frame, *not_hdr));
        REQUIRE(softening->apply(*frame, *source));
        auto pixels = output->framebuffer().read_rgba32f(0, 0, 0, extent.width, extent.height);
        REQUIRE(pixels);
        for (const auto& pixel : *pixels) {
            CHECK(pixel.r == Catch::Approx(3.25F).margin(0.00002));
            CHECK(pixel.g == Catch::Approx(1.5F).margin(0.00002));
            CHECK(pixel.b == Catch::Approx(0.125F).margin(0.00002));
            CHECK(pixel.a == Catch::Approx(0.375F).margin(0.00002));
        }
        REQUIRE(frame->end());
        auto ldr_frame = render::begin_frame(*device, *ldr_output, frame_description(extent));
        REQUIRE(ldr_frame);
        CHECK_FALSE(softening->apply(*ldr_frame, *source));
        REQUIRE(ldr_frame->end());

        auto checker = constant_pixels(extent, {0, 0, 0, 1});
        for (u32 y = 0; y < extent.height; ++y) {
            for (u32 x = 0; x < extent.width; ++x) {
                const auto index = (static_cast<std::size_t>(y) * extent.width + x) * 4;
                checker[index] = checker[index + 1] = checker[index + 2] = static_cast<f32>((x + y) % 2);
            }
        }
        REQUIRE(source->write_rgba32f(0, 0, extent.width, extent.height, checker));
        auto checker_frame = render::begin_frame(*device, *output, frame_description(extent));
        REQUIRE(checker_frame);
        REQUIRE(softening->apply(*checker_frame, *source));
        auto softened = output->framebuffer().read_rgba32f(0, 0, 0, extent.width, extent.height);
        REQUIRE(softened);
        for (u32 y = 1; y + 1 < extent.height; ++y) {
            for (u32 x = 1; x + 1 < extent.width; ++x) {
                const auto& pixel = (*softened)[static_cast<std::size_t>(y) * extent.width + x];
                CHECK(pixel.r > 0.3F);
                CHECK(pixel.r < 0.7F);
                CHECK(pixel.a == Catch::Approx(1.0F).margin(0.00002));
            }
        }
        REQUIRE(checker_frame->end());
    }
    {
        auto retired = std::move(*softening);
    }
    check_no_debug_errors(*device);
}
