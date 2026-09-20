#include "../../examples/support/spaceflight_assets.hpp"
#include "../../examples/support/spaceflight_scene.hpp"
#include "../../examples/support/sun_renderer.hpp"
#include "../support/glfw_opengl.hpp"

#include <vng/providers/target.hpp>
#include <vng/opengl/render_target.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glad/gl.h>

#include <cmath>
#include <cstdlib>
#include <iostream>

TEST_CASE("Solar flyby shares world depth and diagnoses placed sun and moving ship",
    "[examples][solar][opengl]")
{
    using namespace vng;
    namespace flight = example::spaceflight;
    namespace solar = example::sun;
    auto window = test::create_hidden_opengl_window(480, 300, "Solar flyby regression");
    if (!window) { std::cerr << window.error().message << '\n'; std::exit(77); }
    auto access = window->make_current();
    REQUIRE(access);
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    auto scene = flight::load_scene(VNG_TEST_SOLAR_FLYBY_PATH);
    REQUIRE(scene);
    REQUIRE(scene->sun);
    auto mesh = flight::load_ship(scene->mesh_path);
    REQUIRE(mesh);
    auto ship = flight::ShipRenderer::create(*device, std::move(*mesh));
    REQUIRE(ship);
    auto sun = solar::SunRenderer::create(*device);
    REQUIRE(sun);
    const Extent2D extent{480,300};
    auto target = providers::render_target({.color = gfx::ImageFormat::rgba32f}).provide(*device, extent);
    REQUIRE(target);
    gfx::Camera camera;
    camera.set_position(scene->camera.position).look_at(scene->camera.target).set_perspective({
        .vertical_fov = degrees(scene->camera.vertical_fov_degrees),
        .near_plane = scene->camera.near_plane, .far_plane = scene->camera.far_plane});
    auto view = render::RenderView::create(camera, extent);
    REQUIRE(view);
    const solar::SunDraw solar_draw{.time = 7, .position = scene->sun->position, .radius = scene->sun->radius};
    CHECK_FALSE(solar_draw.white_spots);
    auto frame = render::begin_frame(*device, *target, {
        .extent = extent, .color_encoding = render::ColorEncoding::linear,
        .clear_color = std::array<f32,4>{0,0,0,1}, .clear_depth = 1});
    REQUIRE(frame);
    REQUIRE(sun->render(*frame, *view, solar_draw));
    auto solar_evidence = sun->diagnose(*frame, *view, solar_draw,
        analysis::CaptureRequest::standard().observe<solar::WorldPosition>()
            .observe<solar::SurfaceNormal>().observe<solar::Radiance>());
    REQUIRE(solar_evidence);
    const auto& e = solar_evidence->production();
    REQUIRE(e.summary().covered_pixel_count > 30000);
    const auto* positions = e.observation(solar::WorldPosition{});
    const auto* normals = e.observation(solar::SurfaceNormal{});
    const auto* radiance = e.observation(solar::Radiance{});
    REQUIRE(positions); REQUIRE(normals); REQUIRE(radiance);
    double solar_x{};
    for (std::size_t i = 0; i < e.capture().surface_keys().pixels().size(); ++i) {
        if (!e.capture().surface_keys().pixels()[i].has_surface()) continue;
        solar_x += static_cast<double>(i % extent.width);
        const auto p = positions->pixels()[i];
        const auto c = solar_draw.position;
        const Vec3 offset{p.x-c.x,p.y-c.y,p.z-c.z};
        const auto distance = std::sqrt(offset.x*offset.x+offset.y*offset.y+offset.z*offset.z);
        CHECK(distance > solar_draw.radius*.98F);
        CHECK(distance < solar_draw.radius*1.02F);
        const auto n = normals->pixels()[i];
        CHECK(std::abs(n.x-offset.x/distance) < 1e-4F);
        CHECK(std::abs(n.y-offset.y/distance) < 1e-4F);
        CHECK(std::abs(n.z-offset.z/distance) < 1e-4F);
        CHECK(radiance->pixels()[i].z < .03F);
    }
    CHECK(solar_x/static_cast<double>(e.summary().covered_pixel_count) < extent.width*.4);

    double previous_x = -1;
    std::size_t previous_coverage = extent.width * extent.height;
    for (f32 time : {0.0F, 7.0F, 11.0F}) {
        flight::ShipDraw draw{.transform = scene->transform_at(time),
            .light_position = scene->sun->position, .light_color = {3.4F,1.4F,.55F}};
        auto evidence = ship->diagnose(*frame, *view, draw,
            analysis::CaptureRequest::standard().observe<flight::Radiance>());
        REQUIRE(evidence);
        const auto& capture = evidence->production();
        if (time == 0) {
            CHECK(capture.summary().covered_pixel_count == 0);
            continue;
        }
        CHECK(capture.summary().covered_pixel_count > 300);
        CHECK(capture.summary().covered_pixel_count < previous_coverage);
        previous_coverage = capture.summary().covered_pixel_count;
        double centroid{};
        const auto keys = capture.capture().surface_keys().pixels();
        for (std::size_t i = 0; i < keys.size(); ++i)
            if (keys[i].has_surface()) centroid += static_cast<double>(i % extent.width);
        centroid /= static_cast<double>(capture.summary().covered_pixel_count);
        CHECK(centroid > previous_x);
        previous_x = centroid;
    }

    // Force a foreground crossing over the solar disk. Surface and additive
    // arcs must not bleed through the ship, regardless of submission order.
    flight::ShipDraw crossing{.light_position = scene->sun->position};
    // Put it on the camera-to-sun ray, not at a fixed world coordinate.
    const auto eye = scene->camera.position;
    const auto center = scene->sun->position;
    crossing.transform[3] = {eye.x+(center.x-eye.x)*.2F,
        eye.y+(center.y-eye.y)*.2F, eye.z+(center.z-eye.z)*.2F, 1};
    auto ship_evidence = ship->diagnose(*frame, *view, crossing,
        analysis::CaptureRequest::standard().observe<flight::Radiance>());
    REQUIRE(ship_evidence);
    REQUIRE(ship->render(*frame, *view, crossing));
    REQUIRE(sun->render(*frame, *view, solar_draw));
    auto pixels = target->framebuffer().read_rgba32f(0,0,0,extent.width,extent.height);
    REQUIRE(pixels);
    const auto& ship_capture = ship_evidence->production();
    const auto* ship_light = ship_capture.observation(flight::Radiance{});
    REQUIRE(ship_light);
    std::size_t overlap{};
    for (std::size_t i = 0; i < pixels->size(); ++i) {
        if (!ship_capture.capture().surface_keys().pixels()[i].has_surface()) continue;
        overlap += e.capture().surface_keys().pixels()[i].has_surface() ? 1U : 0U;
        const auto expected = ship_light->pixels()[i];
        // Evidence is top-left; raw OpenGL readback is bottom-left.
        const auto y = i / extent.width, x = i % extent.width;
        const auto actual = (*pixels)[(extent.height-1U-y)*extent.width+x];
        CHECK(std::abs(actual.r-expected.x) < 1e-4F);
        CHECK(std::abs(actual.g-expected.y) < 1e-4F);
        CHECK(std::abs(actual.b-expected.z) < 1e-4F);
    }
    CHECK(overlap > 500);
    REQUIRE(frame->end());
    for (const auto& message : device->take_debug_messages()) {
        INFO(message.message);
        CHECK(message.type != "error");
    }
    CHECK(glGetError() == GL_NO_ERROR);
    CHECK(device->take_lifecycle_diagnostics().empty());
}
