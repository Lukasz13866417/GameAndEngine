#include "../../examples/support/sun_renderer.hpp"
#include "../support/glfw_opengl.hpp"

#include <vng/bloom_opengl/bloom.hpp>
#include <vng/opengl/image.hpp>
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
namespace sun = example::sun;

glfw_opengl::Window hidden_window()
{
    auto window = test::create_hidden_opengl_window(480, 300, "Procedural sun regression");
    if (!window) {
        std::cerr << "Sun OpenGL test skipped: " << window.error().message << '\n';
        std::exit(77);
    }
    return std::move(*window);
}

render::FrameDesc frame_description(Extent2D extent)
{
    return {.extent = extent, .color_encoding = render::ColorEncoding::linear,
        .clear_color = std::array<f32, 4>{0, 0, 0, 1}, .clear_depth = 1};
}

struct NativeState final {
    GLint program{}, vao{}, draw_target{}, read_target{}, depth_compare{}, cull_face{};
    GLint blend_source{}, blend_destination{}, blend_equation{};
    GLint active_texture{}, texture_zero{}, sampler_zero{};
    GLboolean depth_test{}, depth_write{}, cull{}, blend{};
    std::array<GLint, 4> viewport{};
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
    glGetBooleanv(GL_DEPTH_WRITEMASK, &state.depth_write);
    glGetIntegerv(GL_VIEWPORT, state.viewport.data());
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

f32 length(Vec3 p)
{
    return std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
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

} // namespace

TEST_CASE("Sun shader arguments animate radial displacement and preserve diagnostic evidence",
    "[examples][sun][opengl][analysis][bloom]")
{
    auto window = hidden_window();
    auto access = window.make_current();
    REQUIRE(access);
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    auto renderer = sun::SunRenderer::create(*device);
    INFO((renderer ? "Sun renderer created" : renderer.error().message));
    REQUIRE(renderer);
    CHECK(renderer->source_mesh().face_count() > 1000);
    constexpr Extent2D extent{480, 300};
    auto target = providers::render_target().provide(*device, extent);
    REQUIRE(target);
    auto external_texture = opengl::Image2D::create(*device, 1, 1, gfx::ImageFormat::rgba8);
    REQUIRE(external_texture);
    REQUIRE(external_texture->bind_to_unit(0));
    ExternalSampler external_sampler;
    glBindSampler(0, external_sampler.handle);
    glActiveTexture(GL_TEXTURE3);
    gfx::Camera camera;
    camera.set_position({0, 0, 4.6F}).look_at({0, 0, 0})
        .set_perspective({.vertical_fov = degrees(36), .near_plane = 0.1F, .far_plane = 30});
    auto view = render::RenderView::create(camera, extent);
    REQUIRE(view);

    const auto render_at = [&](f32 seconds, bool white_spots = true, Vec3 axis_scale = {1,1,1}) {
        auto frame = render::begin_frame(*device, *target, frame_description(extent));
        REQUIRE(frame);
        auto drawn = renderer->render(*frame, *view, {.time = seconds, .white_spots = white_spots, .axis_scale = axis_scale});
        INFO((drawn ? "Sun drawn" : drawn.error().message));
        REQUIRE(drawn);
        const auto state = native_state();
        CHECK(state.texture_zero == static_cast<GLint>(external_texture->native_handle()));
        CHECK(state.sampler_zero == static_cast<GLint>(external_sampler.handle));
        CHECK(state.active_texture == GL_TEXTURE3);
        REQUIRE(frame->end());
        auto pixels = target->framebuffer().read_rgba32f(0, 0, 0, extent.width, extent.height);
        REQUIRE(pixels);
        return std::move(*pixels);
    };
    auto original = render_at(3);
    auto later = render_at(7);
    auto repeated = render_at(3);
    CHECK(same_pixels(original, repeated));
    const auto stretched=render_at(3,true,{1.4F,.7F,1});
    CHECK_FALSE(same_pixels(original,stretched));
    CHECK(same_pixels(stretched,render_at(3,true,{1.4F,.7F,1})));
    const auto without_spots = render_at(3, false);
    CHECK_FALSE(same_pixels(original, without_spots));
    CHECK(same_pixels(original, render_at(3, true)));
    CHECK(same_pixels(original, render_at(3603)));
    const auto extreme_time = std::numeric_limits<f32>::max();
    CHECK(same_pixels(render_at(extreme_time), render_at(std::fmod(extreme_time, 3600.0F))));
    std::size_t animated_pixels = 0;
    f32 maximum_radiance = 0;
    bool finite_hdr = true;
    for (std::size_t i = 0; i < original.size(); ++i) {
        const auto& a = original[i];
        const auto& b = later[i];
        if (std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)}) > 0.01F)
            ++animated_pixels;
        maximum_radiance = std::max({maximum_radiance, a.r, a.g, a.b});
        finite_hdr = finite_hdr && std::isfinite(a.r) && std::isfinite(a.g)
            && std::isfinite(a.b) && std::isfinite(a.a);
    }
    INFO("Animated pixels: " << animated_pixels << ", maximum HDR: " << maximum_radiance);
    CHECK(animated_pixels > 100);
    CHECK(maximum_radiance > 1.0F);
    CHECK(finite_hdr);

    auto frame = render::begin_frame(*device, *target, frame_description(extent));
    REQUIRE(frame);
    REQUIRE(renderer->render(*frame, *view, {.time = 3}));
    auto before = target->framebuffer().read_rgba32f(0, 0, 0, extent.width, extent.height);
    REQUIRE(before);
    const auto state = native_state();
    const auto request = analysis::CaptureRequest::diagnostic()
        .observe(sun::WorldPosition{}).observe(sun::SurfaceNormal{}).observe(sun::Radiance{});
    auto displaced = renderer->diagnose(*frame, *view, {.time = 3, .white_spots = true}, request);
    INFO((displaced ? "Sun evidence captured" : displaced.error().message));
    REQUIRE(displaced);
    CHECK(native_state() == state);
    auto after = target->framebuffer().read_rgba32f(0, 0, 0, extent.width, extent.height);
    REQUIRE(after);
    CHECK(same_pixels(*before, *after));
    const auto& evidence = displaced->production();
    CHECK(evidence.summary().covered_pixel_count > extent.width * extent.height / 20);
    CHECK(evidence.summary().covered_pixel_count < extent.width * extent.height);
    CHECK(evidence.summary().visible_primitive_count > 100);
    REQUIRE(evidence.metadata().invocation);
    CHECK(evidence.metadata().invocation->complete());
    CHECK_FALSE(evidence.backend_artifacts().empty());
    const auto* positions = evidence.observation(sun::WorldPosition{});
    const auto* normals = evidence.observation(sun::SurfaceNormal{});
    const auto* radiance = evidence.observation(sun::Radiance{});
    REQUIRE(positions);
    REQUIRE(normals);
    REQUIRE(radiance);
    const auto keys = evidence.capture().surface_keys().pixels();
    f32 minimum_radius = std::numeric_limits<f32>::max(), maximum_radius = 0;
    f32 normal_error = 0, observed_radiance = 0;
    std::size_t surface_pixels{}, orange_pixels{}, hot_pixels{};
    bool finite_observations = true;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (!keys[i].has_surface()) continue;
        const auto p = positions->pixels()[i];
        const auto n = normals->pixels()[i];
        const auto r = radiance->pixels()[i];
        ++surface_pixels;
        orange_pixels += r.x > r.y * 4.0F && r.x > r.z * 50.0F ? 1U : 0U;
        hot_pixels += r.y > 1.5F && r.z > 0.5F ? 1U : 0U;
        for (u32 component = 0; component < 3; ++component)
            finite_observations = finite_observations && std::isfinite(p[component])
                && std::isfinite(n[component]) && std::isfinite(r[component]);
        minimum_radius = std::min(minimum_radius, length(p));
        maximum_radius = std::max(maximum_radius, length(p));
        normal_error = std::max(normal_error, std::abs(length(n) - 1.0F));
        observed_radiance = std::max({observed_radiance, r.x, r.y, r.z});
    }
    INFO("Observed radius: [" << minimum_radius << ", " << maximum_radius << ']');
    CHECK(finite_observations);
    CHECK(maximum_radius - minimum_radius > 0.001F);
    CHECK(normal_error < 0.005F);
    CHECK(observed_radiance > 1.0F);
    // The orange body is emissive on its own, not brown geometry made bright
    // by bloom. White-hot emission must exist, but stay in localized patches.
    INFO("Surface pixels: " << surface_pixels << ", orange: " << orange_pixels
        << ", hot: " << hot_pixels);
    CHECK(orange_pixels > surface_pixels * 4U / 5U);
    CHECK(hot_pixels > surface_pixels / 1000U);
    CHECK(hot_pixels < surface_pixels / 10U);
    CHECK(observed_radiance > 10.0F);

    // The same per-draw switch reaches enhanced shaders too: turning off hot
    // spots changes radiance, not surface identities, geometry, or arc draws.
    auto unlit_spots = renderer->diagnose(*frame, *view, {.time = 3, .white_spots = false}, request);
    REQUIRE(unlit_spots);
    const auto& unlit = unlit_spots->production();
    const auto* unlit_positions = unlit.observation(sun::WorldPosition{});
    const auto* unlit_radiance = unlit.observation(sun::Radiance{});
    REQUIRE(unlit_positions);
    REQUIRE(unlit_radiance);
    const auto unlit_keys = unlit.capture().surface_keys().pixels();
    REQUIRE(unlit_keys.size() == keys.size());
    std::size_t removed_hot_pixels{};
    for (std::size_t i = 0; i < keys.size(); ++i) {
        REQUIRE(unlit_keys[i] == keys[i]);
        if (!keys[i].has_surface()) continue;
        const auto p = positions->pixels()[i];
        const auto q = unlit_positions->pixels()[i];
        CHECK(p.x == q.x);
        CHECK(p.y == q.y);
        CHECK(p.z == q.z);
        const auto light = unlit_radiance->pixels()[i];
        CHECK(light.z < 0.03F);
        removed_hot_pixels += radiance->pixels()[i].z > light.z + 0.1F ? 1U : 0U;
    }
    CHECK(removed_hot_pixels > 20);
    CHECK(native_state() == state);
    auto after_toggle_capture = target->framebuffer().read_rgba32f(0, 0, 0, extent.width, extent.height);
    REQUIRE(after_toggle_capture);
    CHECK(same_pixels(*before, *after_toggle_capture));

    auto smooth = renderer->diagnose(*frame, *view, {.time = 3, .displacement = 0}, request);
    REQUIRE(smooth);
    const auto* smooth_positions = smooth->production().observation(sun::WorldPosition{});
    REQUIRE(smooth_positions);
    const auto smooth_keys = smooth->production().capture().surface_keys().pixels();
    f32 maximum_geometry_difference = 0, smooth_radius_error = 0;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (!smooth_keys[i].has_surface()) continue;
        const auto smooth_radius = length(smooth_positions->pixels()[i]);
        // Perspective interpolation is planar within each triangle, so the
        // no-displacement surface is very nearly, not mathematically, unit radius.
        smooth_radius_error = std::max(smooth_radius_error, std::abs(smooth_radius - 1.0F));
        if (keys[i].has_surface()) maximum_geometry_difference = std::max(maximum_geometry_difference,
            std::abs(length(positions->pixels()[i]) - smooth_radius));
    }
    CHECK(smooth_radius_error < 0.01F);
    CHECK(maximum_geometry_difference > 0.001F);
    CHECK_FALSE(renderer->render(*frame, render::RenderView::without_camera(extent), {.time = 3}));
    for (const auto time : {-1.0F, std::numeric_limits<f32>::infinity(),
             std::numeric_limits<f32>::quiet_NaN()})
        CHECK_FALSE(renderer->render(*frame, *view, {.time = time}));
    for (const auto displacement : {-0.1F, 1.1F, std::numeric_limits<f32>::infinity(),
             std::numeric_limits<f32>::quiet_NaN()})
        CHECK_FALSE(renderer->render(*frame, *view, {.time = 3, .displacement = displacement}));
    for (const auto radius : {0.0F, -1.0F, 10001.0F, std::numeric_limits<f32>::infinity()})
        CHECK_FALSE(renderer->render(*frame, *view, {.time = 3, .radius = radius}));
    CHECK_FALSE(renderer->diagnose(*frame, *view,
        {.position = {std::numeric_limits<f32>::quiet_NaN(), 0, 0}}, request));
    auto wrong_extent = render::RenderView::create(camera, {320, 200});
    REQUIRE(wrong_extent);
    CHECK_FALSE(renderer->render(*frame, *wrong_extent, {.time = 3}));
    // Factory validation happens before generating assets or creating objects.
    CHECK_FALSE(sun::SunRenderer::create(*device));
    {
        auto moved = std::move(*renderer);
        CHECK_FALSE(renderer->render(*frame, *view, {.time = 3}));
        CHECK_FALSE(renderer->diagnose(*frame, *view, {.time = 3}, request));
        *renderer = std::move(moved);
    }
    const auto before_invalid_batch = native_state();
    const std::array invalid_batch{sun::SunDraw{3}, sun::SunDraw{-1}};
    CHECK_FALSE(renderer->render(*frame, *view, invalid_batch));
    CHECK(native_state() == before_invalid_batch);
    auto after_invalid_batch = target->framebuffer().read_rgba32f(0, 0, 0, extent.width, extent.height);
    REQUIRE(after_invalid_batch);
    CHECK(same_pixels(*after, *after_invalid_batch));
    REQUIRE(frame->end());
    CHECK_FALSE(renderer->render(*frame, *view, {.time = 3}));

    auto output = providers::render_target({.color = gfx::ImageFormat::rgba32f, .depth = false})
        .provide(*device, extent);
    REQUIRE(output);
    auto bloom = render::bloom_builder(*device).levels(5).build(extent);
    REQUIRE(bloom);
    auto output_description = frame_description(extent);
    output_description.clear_depth = std::nullopt;
    auto composite = render::begin_frame(*device, *output, output_description);
    REQUIRE(composite);
    render::BloomSettings settings{1.0F, 0.0F, 0.8F};
    REQUIRE(bloom->apply(*composite, target->color(), settings));
    auto no_bloom = output->framebuffer().read_rgba32f(0, 0, 0, extent.width, extent.height);
    REQUIRE(no_bloom);
    settings.strength = 0.25F;
    REQUIRE(bloom->apply(*composite, target->color(), settings));
    auto glowing = output->framebuffer().read_rgba32f(0, 0, 0, extent.width, extent.height);
    REQUIRE(glowing);
    f32 maximum_bloom_difference = 0;
    bool valid_composite = true;
    for (std::size_t i = 0; i < glowing->size(); ++i) {
        const auto& pixel = (*glowing)[i];
        const auto& unlit = (*no_bloom)[i];
        maximum_bloom_difference = std::max({maximum_bloom_difference,
            pixel.r - unlit.r, pixel.g - unlit.g, pixel.b - unlit.b});
        valid_composite = valid_composite && std::isfinite(pixel.r) && std::isfinite(pixel.g)
            && std::isfinite(pixel.b) && pixel.r >= 0 && pixel.r <= 1.001F
            && pixel.g >= 0 && pixel.g <= 1.001F && pixel.b >= 0 && pixel.b <= 1.001F;
    }
    CHECK(valid_composite);
    CHECK(maximum_bloom_difference > 0.001F);
    REQUIRE(composite->end());

    constexpr Extent2D resized_extent{320, 200};
    REQUIRE(target->resize(*device, resized_extent));
    auto resized_view = render::RenderView::create(camera, resized_extent);
    REQUIRE(resized_view);
    auto resized_frame = render::begin_frame(*device, *target, frame_description(resized_extent));
    REQUIRE(resized_frame);
    REQUIRE(renderer->render(*resized_frame, *resized_view, {.time = 7}));
    REQUIRE(resized_frame->end());
    {
        // Destroy resources while the owning context is still current, before
        // checking lifecycle diagnostics rather than after the test returns.
        auto retired = std::move(*renderer);
    }
    check_no_debug_errors(*device);
}
