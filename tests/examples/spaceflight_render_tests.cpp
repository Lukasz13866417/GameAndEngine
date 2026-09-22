#include "../../examples/support/spaceflight_assets.hpp"
#include "../../examples/support/spaceflight_scene.hpp"
#include "../../examples/support/spaceflight_display.hpp"
#include "../../examples/support/space_background.hpp"
#include "../support/glfw_opengl.hpp"

#include <vng/bloom_opengl/bloom.hpp>
#include <vng/opengl/buffer.hpp>
#include <vng/opengl/render_target.hpp>
#include <vng/providers/target.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glad/gl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

namespace {
using namespace vng;
namespace flight = example::spaceflight;

glfw_opengl::Window hidden_window()
{
    auto window = test::create_hidden_opengl_window(480, 300, "Spaceflight regression");
    if (!window) {
        std::cerr << "Spaceflight OpenGL test skipped: " << window.error().message << '\n';
        std::exit(77);
    }
    return std::move(*window);
}

gfx::Camera scene_camera(const flight::Scene& scene)
{
    gfx::Camera camera;
    camera.set_position(scene.camera.position).look_at(scene.camera.target)
        .set_perspective({.vertical_fov = degrees(scene.camera.vertical_fov_degrees),
            .near_plane = scene.camera.near_plane, .far_plane = scene.camera.far_plane});
    return camera;
}

render::FrameDesc frame_description(Extent2D extent)
{
    return {.extent = extent, .color_encoding = render::ColorEncoding::linear,
        .clear_color = std::array<f32, 4>{0, 0, 0, 1}, .clear_depth = 1};
}

struct NativeState final {
    GLint program{}, vao{}, draw_target{}, read_target{};
    GLint depth_compare{}, cull_face{}, winding{}, generic_storage{}, indexed_storage{};
    GLint64 storage_offset{}, storage_size{};
    GLboolean depth_test{}, depth_write{}, cull{}, blend{};
    std::array<GLint, 4> viewport{};
    std::array<GLint, 2> polygon{};
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
    glGetIntegerv(GL_FRONT_FACE, &state.winding);
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &state.generic_storage);
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 0, &state.indexed_storage);
    glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_START, 0, &state.storage_offset);
    glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_SIZE, 0, &state.storage_size);
    state.depth_test = glIsEnabled(GL_DEPTH_TEST);
    state.cull = glIsEnabled(GL_CULL_FACE);
    state.blend = glIsEnabledi(GL_BLEND, 0);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &state.depth_write);
    glGetIntegerv(GL_VIEWPORT, state.viewport.data());
    glGetIntegerv(GL_POLYGON_MODE, state.polygon.data());
    glGetBooleani_v(GL_COLOR_WRITEMASK, 0, state.color_mask.data());
    return state;
}

bool same_pixels(const std::vector<opengl::Rgba32fPixel>& a,
    const std::vector<opengl::Rgba32fPixel>& b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](const auto& left, const auto& right) {
            return left.r == right.r && left.g == right.g && left.b == right.b && left.a == right.a;
        });
}

void check_no_debug_errors(const opengl::Device& device)
{
    for (const auto& message : device.take_debug_messages()) {
        // Drivers can emit informational shader-recompile/performance notes.
        INFO(message.message);
        CHECK(message.type != "error");
    }
    CHECK(device.take_lifecycle_diagnostics().empty());
}
} // namespace

TEST_CASE("The actual spaceship enters view and enhanced evidence preserves the production draw",
    "[examples][spaceflight][opengl][analysis]")
{
    auto window = hidden_window();
    auto access = window.make_current();
    REQUIRE(access);
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    auto scene = flight::load_scene(VNG_TEST_SPACEFLIGHT_PATH);
    REQUIRE(scene);
    auto mesh = flight::load_ship(scene->mesh_path);
    REQUIRE(mesh);
    REQUIRE(mesh->face_count() > 1000);
    auto renderer = flight::ShipRenderer::create(*device, std::move(*mesh));
    REQUIRE(renderer);
    constexpr Extent2D extent{480, 300};
    auto target = providers::render_target().provide(*device, extent);
    REQUIRE(target);
    GLint alignment{};
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &alignment);
    REQUIRE(alignment > 0);
    auto external_storage = opengl::Buffer::create(*device,
        {.size = static_cast<std::size_t>(alignment) + 256});
    auto generic_storage = opengl::Buffer::create(*device, {.size = 64});
    REQUIRE(external_storage);
    REQUIRE(generic_storage);
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, external_storage->native_handle(), alignment, 128);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, generic_storage->native_handle());
    const auto camera = scene_camera(*scene);
    auto view = render::RenderView::create(camera, extent);
    REQUIRE(view);
    auto frame = render::begin_frame(*device, *target, frame_description(extent));
    REQUIRE(frame);

    auto opening = renderer->diagnose(*frame, *view, {scene->transform_at(0)});
    REQUIRE(opening);
    for (const auto& variant : opening->variants())
        CHECK(variant.evidence.summary().covered_pixel_count == 0);

    const flight::ShipDraw hero{scene->transform_at(scene->hero_time)};
    REQUIRE(renderer->render(*frame, *view, hero));
    auto before = target->framebuffer().read_rgba32f(0, 0, 0, extent.width, extent.height);
    REQUIRE(before);
    const auto before_state = native_state();
    CHECK(before_state.indexed_storage == static_cast<GLint>(external_storage->native_handle()));
    CHECK(before_state.generic_storage == static_cast<GLint>(generic_storage->native_handle()));
    CHECK(before_state.storage_offset == alignment);
    CHECK(before_state.storage_size == 128);
    REQUIRE(glGetError() == GL_NO_ERROR);

    const auto request = analysis::CaptureRequest::diagnostic()
        .observe(flight::WorldPosition{})
        .observe(flight::WorldNormal{})
        .observe(flight::Radiance{});
    auto sweep = renderer->diagnose(*frame, *view, hero, request);
    REQUIRE(sweep);
    CHECK(native_state() == before_state);
    REQUIRE(glGetError() == GL_NO_ERROR);
    auto after = target->framebuffer().read_rgba32f(0, 0, 0, extent.width, extent.height);
    REQUIRE(after);
    CHECK(same_pixels(*before, *after));

    const auto& evidence = sweep->production();
    CHECK(evidence.summary().covered_pixel_count > extent.width * extent.height / 20);
    CHECK(evidence.summary().covered_pixel_count < extent.width * extent.height);
    CHECK(evidence.summary().visible_primitive_count > 20);
    const auto* positions = evidence.observation(flight::WorldPosition{});
    const auto* normals = evidence.observation(flight::WorldNormal{});
    const auto* radiance = evidence.observation(flight::Radiance{});
    REQUIRE(positions);
    REQUIRE(normals);
    REQUIRE(radiance);
    bool all_finite = true;
    f32 maximum_normal_error = 0, maximum_radiance = 0;
    const auto keys = evidence.capture().surface_keys().pixels();
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (!keys[i].has_surface()) continue;
        const auto p = positions->pixels()[i];
        const auto n = normals->pixels()[i];
        const auto r = radiance->pixels()[i];
        for (u32 component = 0; component < 3; ++component)
            all_finite = all_finite && std::isfinite(p[component])
                && std::isfinite(n[component]) && std::isfinite(r[component]);
        all_finite = all_finite && std::isfinite(r.w);
        maximum_normal_error = std::max(maximum_normal_error,
            std::abs(std::sqrt(n.x*n.x + n.y*n.y + n.z*n.z) - 1.0F));
        maximum_radiance = std::max({maximum_radiance, r.x, r.y, r.z});
    }
    INFO("Maximum observed linear radiance: " << maximum_radiance);
    CHECK(all_finite);
    CHECK(maximum_normal_error < 0.002F);
    CHECK(maximum_radiance > 1.5F);
    REQUIRE(evidence.metadata().invocation);
    CHECK(evidence.metadata().invocation->complete());
    CHECK_FALSE(evidence.backend_artifacts().empty());

    flight::ShipDraw scaled = hero;
    scaled.transform[0].x *= 2;
    CHECK_FALSE(renderer->render(*frame, *view, scaled));
    CHECK_FALSE(renderer->render(*frame, render::RenderView::without_camera(extent), hero));
    REQUIRE(frame->end());
    CHECK_FALSE(renderer->render(*frame, *view, hero));
    check_no_debug_errors(*device);
}

TEST_CASE("Space background, ship and bloom compose in HDR and remain usable after resize",
    "[examples][spaceflight][opengl][bloom]")
{
    auto window = hidden_window();
    auto access = window.make_current();
    REQUIRE(access);
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    auto scene = flight::load_scene(VNG_TEST_SPACEFLIGHT_PATH);
    REQUIRE(scene);
    auto mesh = flight::load_ship(scene->mesh_path);
    REQUIRE(mesh);
    auto renderer = flight::ShipRenderer::create(*device, std::move(*mesh));
    REQUIRE(renderer);
    constexpr Extent2D initial{480, 300};
    auto target = providers::render_target().provide(*device, initial);
    REQUIRE(target);
    auto output = providers::render_target({.color = gfx::ImageFormat::rgba32f, .depth = false})
        .provide(*device, initial);
    REQUIRE(output);
    auto background = flight::SpaceBackground::create(*device, initial, scene->star_count, scene->star_seed);
    REQUIRE(background);
    auto bloom = render::bloom_builder(*device).levels(6).build(initial);
    REQUIRE(bloom);
    const auto camera = scene_camera(*scene);
    const flight::ShipDraw hero{scene->transform_at(scene->hero_time)};

    for (const Extent2D extent : {initial, Extent2D{320, 200}}) {
        REQUIRE(target->resize(*device, extent));
        REQUIRE(output->resize(*device, extent));
        REQUIRE(background->resize(*device, extent));
        REQUIRE(bloom->resize(*device, extent));
        auto view = render::RenderView::create(camera, extent);
        REQUIRE(view);
        auto frame = render::begin_frame(*device, *target, frame_description(extent));
        REQUIRE(frame);
        REQUIRE(background->render(*frame));
        // Background blending and disabled depth must not leak into ship draws.
        REQUIRE(renderer->render(*frame, *view, hero));
        CHECK(glIsEnabled(GL_DEPTH_TEST) == GL_TRUE);
        CHECK(glIsEnabledi(GL_BLEND, 0) == GL_FALSE);
        CHECK(glIsEnabled(GL_CULL_FACE) == GL_TRUE);
        REQUIRE(frame->end());
        auto hdr = target->framebuffer().read_rgba32f(0, 0, 0, extent.width, extent.height);
        REQUIRE(hdr);
        f32 maximum_hdr = 0;
        for (const auto& pixel : *hdr) maximum_hdr = std::max({maximum_hdr, pixel.r, pixel.g, pixel.b});
        CHECK(maximum_hdr > 1.5F);

        auto description = frame_description(extent);
        description.clear_depth = std::nullopt;
        auto composite = render::begin_frame(*device, *output, description);
        REQUIRE(composite);
        auto settings = scene->bloom;
        settings.strength = 0;
        REQUIRE(bloom->apply(*composite, target->color(), settings));
        auto unlit = output->framebuffer().read_rgba32f(0, 0, 0, extent.width, extent.height);
        REQUIRE(unlit);
        settings.strength = scene->bloom.strength;
        REQUIRE(settings.strength > 0.0F);
        REQUIRE(bloom->apply(*composite, target->color(), settings));
        auto glowing = output->framebuffer().read_rgba32f(0, 0, 0, extent.width, extent.height);
        REQUIRE(glowing);
        f32 maximum_delta = 0;
        double total_delta = 0;
        std::size_t changed_pixels = 0;
        bool all_finite = true, bounded = true, alpha_preserved = true;
        for (std::size_t i = 0; i < glowing->size(); ++i) {
            const auto& pixel = (*glowing)[i];
            const auto& original = (*unlit)[i];
            all_finite = all_finite && std::isfinite(pixel.r) && std::isfinite(pixel.g)
                && std::isfinite(pixel.b) && std::isfinite(pixel.a);
            bounded = bounded && pixel.r >= 0 && pixel.g >= 0 && pixel.b >= 0
                && pixel.r <= 1.001F && pixel.g <= 1.001F && pixel.b <= 1.001F;
            alpha_preserved = alpha_preserved && std::abs(pixel.a-original.a) < 1.0e-6F;
            const auto delta = std::max({pixel.r-original.r, pixel.g-original.g, pixel.b-original.b});
            maximum_delta = std::max(maximum_delta, delta);
            total_delta += pixel.r-original.r + pixel.g-original.g + pixel.b-original.b;
            if (delta > 1.0e-4F) ++changed_pixels;
        }
        INFO("Extent " << extent.width << 'x' << extent.height << ", HDR max " << maximum_hdr
             << ", bloom max delta " << maximum_delta << ", changed pixels " << changed_pixels);
        CHECK(all_finite);
        CHECK(bounded);
        CHECK(alpha_preserved);
        CHECK(maximum_delta > 0.001F);
        CHECK(total_delta > 0.01);
        CHECK(changed_pixels > 5);
        REQUIRE(composite->end());
    }
    REQUIRE(glGetError() == GL_NO_ERROR);
    check_no_debug_errors(*device);
}

TEST_CASE("Spaceflight presentation encodes sRGB exactly once and restores copy state",
    "[examples][spaceflight][opengl][presentation]")
{
    auto window = hidden_window();
    auto access = window.make_current();
    REQUIRE(access);
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    constexpr Extent2D extent{16, 16};
    auto display = flight::DisplaySurface::create(*device, extent);
    REQUIRE(display);
    auto external = providers::render_target({.color = gfx::ImageFormat::rgba8, .depth = false})
        .provide(*device, extent);
    REQUIRE(external);
    CHECK(display->target().color().format() == gfx::ImageFormat::srgb8_alpha8);
    auto description = frame_description(extent);
    description.color_encoding = render::ColorEncoding::srgb;
    description.clear_color = std::array<f32, 4>{0.25F, 0.5F, 0.75F, 1};
    description.clear_depth = std::nullopt;
    auto frame = render::begin_frame(*device, display->target(), description);
    REQUIRE(frame);
    CHECK_FALSE(display->copy_to_window(*device));
    CHECK_FALSE(display->resize(*device, {32, 20}));
    REQUIRE(frame->end());
    auto encoded = display->target().framebuffer().read_rgba8(0, 8, 8, 1, 1);
    REQUIRE(encoded);
    REQUIRE(encoded->size() == 4);
    const opengl::Rgba8Pixel expected{
        std::to_integer<u8>((*encoded)[0]), std::to_integer<u8>((*encoded)[1]),
        std::to_integer<u8>((*encoded)[2]), std::to_integer<u8>((*encoded)[3])};
    CHECK(std::abs(static_cast<int>(expected.r) - 137) <= 1);
    CHECK(std::abs(static_cast<int>(expected.g) - 188) <= 1);
    CHECK(std::abs(static_cast<int>(expected.b) - 225) <= 1);

    // Hostile caller state: different current read/draw FBO, disabled source
    // read selection, front-buffer default draw selection, tiny scissor, and
    // sRGB conversion enabled. The byte copy must preserve every one of these.
    const auto source = display->target().framebuffer().native_handle();
    const auto other = external->framebuffer().native_handle();
    glNamedFramebufferReadBuffer(source, GL_NONE);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDrawBuffer(GL_FRONT);
    GLint default_draw{};
    glGetIntegerv(GL_DRAW_BUFFER0, &default_draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, other);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, other);
    glEnable(GL_FRAMEBUFFER_SRGB);
    glEnablei(GL_SCISSOR_TEST, 0);
    glScissor(0, 0, 1, 1);
    REQUIRE(glGetError() == GL_NO_ERROR);
    const auto state = native_state();
    REQUIRE(display->copy_to_window(*device));
    CHECK(native_state() == state);
    CHECK(glIsEnabled(GL_FRAMEBUFFER_SRGB) == GL_TRUE);
    CHECK(glIsEnabledi(GL_SCISSOR_TEST, 0) == GL_TRUE);
    std::array<GLint, 4> scissor{};
    glGetIntegerv(GL_SCISSOR_BOX, scissor.data());
    CHECK(scissor == std::array<GLint, 4>{0, 0, 1, 1});
    glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
    GLint source_read{};
    glGetIntegerv(GL_READ_BUFFER, &source_read);
    CHECK(source_read == GL_NONE);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    GLint restored_default_draw{};
    glGetIntegerv(GL_DRAW_BUFFER0, &restored_default_draw);
    CHECK(restored_default_draw == default_draw);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    opengl::Rgba8Pixel pixel{};
    glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &pixel);
    REQUIRE(glGetError() == GL_NO_ERROR);
    CHECK(pixel == expected);
    CHECK(pixel.a == 255);

    glDisable(GL_FRAMEBUFFER_SRGB);
    glDisablei(GL_SCISSOR_TEST, 0);
    glDrawBuffer(GL_BACK);
    glNamedFramebufferReadBuffer(source, GL_COLOR_ATTACHMENT0);
    REQUIRE(display->resize(*device, {32, 20}));
    CHECK(display->target().extent() == Extent2D{32, 20});
    REQUIRE(glGetError() == GL_NO_ERROR);
    check_no_debug_errors(*device);
}
