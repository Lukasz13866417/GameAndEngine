#include "../../examples/support/sun_shaders.hpp"
#include "../support/glfw_opengl.hpp"

#include <vng/render/opengl_program_runtime.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
using namespace vng;
namespace sun = example::sun;
using Runtime = render::TypedOpenGLProgramRuntime<sun::shaders::Parameters, Mat3>;
}

TEST_CASE("Surface and arc shaders share a live sun-local rotation", "[examples][sun][opengl][rotation]")
{
    auto window = test::create_hidden_opengl_window(96, 96, "Sun attachment rotation");
    if (!window) { std::cerr << window.error().message << '\n'; std::exit(77); }
    auto access = window->make_current();
    REQUIRE(access);
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    auto surface_shader = sun::shaders::surface();
    auto arc_shader = sun::shaders::prominences();
    REQUIRE(surface_shader);
    REQUIRE(arc_shader);
    const opengl::CaptureState state{opengl::GraphicsStateSnapshot{}, render::ColorEncoding::linear};
    auto surface = Runtime::create(*device, std::move(*surface_shader));
    auto arcs = Runtime::create(*device, std::move(*arc_shader));
    REQUIRE(surface);
    REQUIRE(arcs);

    // An asymmetric planar patch stands in for identical surface/arc roots.
    // Extra record semantics are simply ignored by the surface shader.
    sun::ProminenceMesh cpu(3);
    constexpr std::array points{Vec3{-0.75F, -0.4F, 0.3F},
        Vec3{0.1F, -0.35F, 0.3F}, Vec3{-0.2F, 0.45F, 0.3F}};
    for (u32 i = 0; i < points.size(); ++i) {
        cpu.vertices()[i].set(gfx::Position{}, points[i]);
        cpu.vertices()[i].set(gfx::Normal{}, {0, 0, 1});
        cpu.vertices()[i].set(sun::SurfaceUV{}, {0.25F, 0.5F});
        cpu.vertices()[i].set(sun::TubeOffset{}, {0, 0, 0.002F});
    }
    cpu.add_face(0, 1, 2);
    auto mesh = opengl::upload_mesh(*device, cpu);
    REQUIRE(mesh);
    auto map = opengl::Image2D::create(*device, 1, 1, gfx::ImageFormat::rgba32f);
    REQUIRE(map);
    REQUIRE(map->clear_rgba32f({0.5F, 0.5F, 0, 0.5F}));
    REQUIRE(map->bind_to_unit(0));
    gfx::Camera camera;
    camera.set_position({0, 0, 4}).look_at({0, 0, 0})
        .set_orthographic({.vertical_height = 3, .near_plane = 0.1F, .far_plane = 10});
    const Extent2D extent{96, 96};
    auto view = render::RenderView::create(camera, extent);
    REQUIRE(view);
    sun::shaders::Parameters params;
    params.set(sun::shaders::Time{}, 3);
    params.set(sun::shaders::Displacement{}, 0);
    params.set(sun::shaders::Eye{}, {0, 0, 4});
    params.set(sun::shaders::StrandHalo{}, 0);
    params.set(sun::shaders::WhiteSpots{}, 1);
    params.set(sun::shaders::Radius{}, 1);
    const auto request = analysis::CaptureRequest::standard().observe(sun::WorldPosition{});
    const Mat3 turned{{Vec3{0.5F, 0, -0.8660254F}, Vec3{0, 1, 0}, Vec3{0.8660254F, 0, 0.5F}}};
    std::array<double, 2> centroid_x{};
    std::size_t turn{};
    const auto surface_handle = surface->production().untyped().native_handle();
    const auto arc_handle = arcs->production().untyped().native_handle();
    for (const auto rotation : {Mat3::identity(), turned}) {
        REQUIRE(surface->set_arguments(params, rotation));
        REQUIRE(arcs->set_arguments(params, rotation));
        auto surface_evidence = surface->capture(*device, cpu, *mesh, *view, state, request);
        auto arc_evidence = arcs->capture(*device, cpu, *mesh, *view, state, request);
        REQUIRE(surface_evidence);
        REQUIRE(arc_evidence);
        const auto* surface_positions = surface_evidence->observation(sun::WorldPosition{});
        const auto* arc_positions = arc_evidence->observation(sun::WorldPosition{});
        REQUIRE(surface_positions);
        REQUIRE(arc_positions);
        const auto surface_keys = surface_evidence->capture().surface_keys().pixels();
        const auto arc_keys = arc_evidence->capture().surface_keys().pixels();
        std::size_t covered{};
        for (std::size_t i = 0; i < surface_keys.size(); ++i) {
            REQUIRE(surface_keys[i].has_surface() == arc_keys[i].has_surface());
            if (!surface_keys[i].has_surface()) continue;
            ++covered;
            const auto p = surface_positions->pixels()[i];
            const auto a = arc_positions->pixels()[i];
            CHECK(std::abs(p.x - a.x) < 0.00001F);
            CHECK(std::abs(p.y - a.y) < 0.00001F);
            CHECK(std::abs(p.z - a.z) < 0.00001F);
            // The observed patch lies in the actually rotated plane, not just
            // a different UV pattern on the old, stationary geometry.
            CHECK(std::abs(p.x * rotation[2].x + p.y * rotation[2].y
                + p.z * rotation[2].z - 0.3F) < 0.00001F);
            centroid_x[turn] += p.x;
        }
        REQUIRE(covered > 100);
        centroid_x[turn] /= static_cast<double>(covered);
        // The soft envelope expands in sun-local space before the same body
        // transform. It must not leave an unrotated halo behind a turning arc.
        params.set(sun::shaders::StrandHalo{}, 1.0F);
        REQUIRE(arcs->set_arguments(params, rotation));
        auto halo_evidence = arcs->capture(*device, cpu, *mesh, *view, state, request);
        REQUIRE(halo_evidence);
        const auto* halo_positions = halo_evidence->observation(sun::WorldPosition{});
        REQUIRE(halo_positions);
        const auto halo_keys = halo_evidence->capture().surface_keys().pixels();
        std::size_t halo_covered{};
        for (std::size_t i = 0; i < halo_keys.size(); ++i) {
            if (!halo_keys[i].has_surface()) continue;
            ++halo_covered;
            const auto p = halo_positions->pixels()[i];
            const auto plane_offset = p.x * rotation[2].x + p.y * rotation[2].y
                + p.z * rotation[2].z - 0.3F;
            // Artistic tuning can vary the exact radius. A genuinely broader,
            // finite envelope should still be close to this tiny source patch.
            CHECK(plane_offset > 0.014F);
            CHECK(plane_offset < 0.03F);
        }
        CHECK(halo_covered > 100);
        params.set(sun::shaders::StrandHalo{}, 0.0F);
        ++turn;
        CHECK(surface->production().untyped().native_handle() == surface_handle);
        CHECK(arcs->production().untyped().native_handle() == arc_handle);
    }
    CHECK(centroid_x[1] - centroid_x[0] > 0.25);

    SECTION("Fine turbulence cannot emit white without an authored hot footprint")
    {
        REQUIRE(surface->set_arguments(params, Mat3::identity()));
        const auto color_request = analysis::CaptureRequest::standard().observe(sun::Radiance{});
        // Exercise the bright ceiling, authored threshold, and dark lane.
        // An unmasked spark or a high palette floor would lose this range.
        struct Sample { f32 granule; f32 activity; bool enabled{true}; };
        for (const auto [granule, activity, enabled] : {Sample{1, 0}, Sample{1, 0.60F},
                 Sample{1, 1}, Sample{0, 0}, Sample{1, 1, false}, Sample{1, 1}}) {
            params.set(sun::shaders::WhiteSpots{}, enabled ? 1.0F : 0.0F);
            REQUIRE(surface->set_arguments(params, Mat3::identity()));
            REQUIRE(map->clear_rgba32f({granule, granule, 0, activity}));
            auto evidence = surface->capture(*device, cpu, *mesh, *view, state, color_request);
            REQUIRE(evidence);
            const auto* colors = evidence->observation(sun::Radiance{});
            REQUIRE(colors);
            const auto keys = evidence->capture().surface_keys().pixels();
            std::size_t covered{};
            for (std::size_t i = 0; i < keys.size(); ++i) {
                if (!keys[i].has_surface()) continue;
                ++covered;
                const auto light = colors->pixels()[i];
                if (activity <= 0.60F || !enabled) {
                    CHECK(light.x < 18.0F);
                    // Warm yellow tips are allowed; unmasked white is not.
                    CHECK(light.y < 1.8F);
                    CHECK(light.z < 0.03F);
                    if (granule == 0) {
                        CHECK(light.x < 1.0F);
                        CHECK(light.y < 0.03F);
                    } else {
                        CHECK(light.x > 14.0F);
                    }
                } else {
                    CHECK(light.y > 10.0F);
                    CHECK(light.z > 10.0F);
                }
            }
            CHECK(covered > 100);
            CHECK(surface->production().untyped().native_handle() == surface_handle);
        }
    }
    for (const auto& message : device->take_debug_messages()) {
        INFO(message.message);
        CHECK(message.type != "error");
    }
    CHECK(device->take_lifecycle_diagnostics().empty());
}
