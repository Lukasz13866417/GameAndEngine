#include "support/glfw_opengl_session.hpp"
#include "support/space_background.hpp"
#include "support/spaceflight_assets.hpp"
#include "support/spaceflight_display.hpp"
#include "support/spaceflight_inspection.hpp"
#include "support/spaceflight_scene.hpp"
#include "support/sun_renderer.hpp"
#include "support/sun_inspection.hpp"
#include "support/sun_softening.hpp"
#include "support/window_loop.hpp"

#include <vng/opengl/bloom.hpp>
#include <vng/opengl/render_target.hpp>
#include <vng/providers/target.hpp>

#include <chrono>
#include <iostream>

namespace {
int fail(const vng::resources::Diagnostic& error)
{
    std::cerr << error.message << '\n' << error.driver_log << '\n';
    for (const auto& context : error.context) std::cerr << "  " << context << '\n';
    return 1;
}
} // namespace

int main(int argc, char** argv)
{
    namespace flight = example::spaceflight;
    namespace render = vng::render;

    auto options = flight::parse_options(argc, argv, VNG_EXAMPLE_SPACEFLIGHT_PATH);
    if (!options) return example::fail(options.error());
    if (options->help) { std::cout << flight::usage(); return 0; }
    auto scene = flight::load_scene(options->scene_path);
    if (!scene) return example::fail(scene.error());
    auto mesh = flight::load_ship(scene->mesh_path);
    if (!mesh) return example::fail(mesh.error());

    auto app = example::GlfwOpenGLSession::create(
        {.width = scene->extent.width,
            .height = scene->extent.height, .title = scene->sun
                ? "KESTREL / HELIOS - solar flyby" : "KESTREL / deep-space flyby"},
        {.debug = true, .samples = 0,
            .default_framebuffer_encoding = render::ColorEncoding::linear});
    if (!app) return example::fail(app.error());
    auto& device = app->device();
    const auto initial = app->window().framebuffer_extent();

    // The renderer owns the mesh and authors both DSL shaders internally.
    auto ship = flight::ShipRenderer::create(device, std::move(*mesh));
    if (!ship) return fail(ship.error());
    auto background = flight::SpaceBackground::create(device, initial, scene->star_count, scene->star_seed);
    if (!background) return fail(background.error());
    auto target = vng::providers::render_target().provide(device, initial);
    if (!target) return fail(target.error());
    auto bloom = render::bloom_builder(device).levels(6).build(initial);
    if (!bloom) return fail(bloom.error());
    auto display = flight::DisplaySurface::create(device, initial);
    if (!display) return fail(display.error());
    std::optional<example::sun::SunRenderer> sun;
    std::optional<example::sun::SunSoftening> softening;
    std::optional<vng::opengl::RenderTarget> softened;
    if (scene->sun) {
        std::cout << "Generating solar surface and plasma strands...\n" << std::flush;
        auto renderer = example::sun::SunRenderer::create(device);
        if (!renderer) return fail(renderer.error());
        sun.emplace(std::move(*renderer));
        auto filter = example::sun::SunSoftening::create(device);
        if (!filter) return fail(filter.error());
        softening.emplace(std::move(*filter));
        auto filtered = vng::providers::render_target({.depth = false}).provide(device, initial);
        if (!filtered) return fail(filtered.error());
        softened.emplace(std::move(*filtered));
    }

    vng::gfx::Camera camera;
    camera.set_position(scene->camera.position).look_at(scene->camera.target)
        .set_perspective({.vertical_fov = vng::degrees(scene->camera.vertical_fov_degrees),
            .near_plane = scene->camera.near_plane, .far_plane = scene->camera.far_plane});
    const bool capture = options->analysis_directory || options->screenshot_path;
    if (capture && !options->fixed_time) options->fixed_time = scene->hero_time;
    if (capture && !options->frame_limit) options->frame_limit = 1;
    bool bloom_enabled = options->bloom, paused = false, captured = false;
    bool previous_b = false, previous_r = false, previous_space = false;
    bool previous_w = false, white_spots = scene->sun && scene->sun->white_spots;
    float seconds = 0;
    auto previous = std::chrono::steady_clock::now();
    std::cout << "KESTREL / K-07 | " << scene->mesh_path << '\n'
              << "Space: pause | R: restart flyby | B: bloom | W: sun white spots | Esc: close\n"
              << (scene->sun ? "The ship emerges from beneath the camera and banks around the sun.\n"
                  : "The shot starts with empty space; the ship enters and passes the camera.\n");

    example::WindowLoop loop{app->window(), options->frame_limit};
    while (const auto extent = loop.next_extent()) {
        using vng::window::Key;
        if (app->window().key_down(Key::escape)) break;
        const bool b = app->window().key_down(Key::b);
        const bool r = app->window().key_down(Key::r);
        const bool space = app->window().key_down(Key::space);
        const bool w = app->window().key_down(Key::w);
        if (w && !previous_w) white_spots = !white_spots;
        previous_w = w;
        if (b && !previous_b) bloom_enabled = !bloom_enabled;
        if (space && !previous_space) paused = !paused;
        const auto now = std::chrono::steady_clock::now();
        if (!paused) seconds += std::chrono::duration<float>(now - previous).count();
        if (r && !previous_r) seconds = 0;
        previous = now;
        previous_b = b; previous_r = r; previous_space = space;

        // Resize resources only between frames. Animation changes only the ticket.
        if (auto done = target->resize(device, *extent); !done) return fail(done.error());
        if (auto done = bloom->resize(device, *extent); !done) return fail(done.error());
        if (auto done = background->resize(device, *extent); !done) return fail(done.error());
        if (auto done = display->resize(device, *extent); !done) return fail(done.error());
        if (softened) {
            if (auto done = softened->resize(device, *extent); !done) return fail(done.error());
        }
        auto view = render::RenderView::create(camera, *extent);
        if (!view) return example::fail(view.error());
        const float sample_time = options->fixed_time.value_or(seconds);
        flight::ShipDraw draw{scene->transform_at(sample_time)};
        example::sun::SunDraw solar_draw{.time = sample_time, .white_spots = white_spots};
        if (scene->sun) {
            solar_draw.position = scene->sun->position;
            solar_draw.radius = scene->sun->radius;
            draw.light_position = scene->sun->position;
            draw.light_color = {3.4F, 1.4F, 0.55F};
        }

        // Ordinary rendering: all scene geometry shares linear HDR and depth.
        auto frame = render::begin_frame(device, *target, {
            .extent = *extent, .color_encoding = render::ColorEncoding::linear,
            .clear_color = std::array<float, 4>{0, 0, 0, 1}, .clear_depth = 1});
        if (!frame) return example::fail(frame.error());
        if (auto done = background->render(*frame); !done) return fail(done.error());
        if (sun) {
            if (auto done = sun->render(*frame, *view, solar_draw); !done) return fail(done.error());
        }
        if (auto done = ship->render(*frame, *view, draw); !done) return fail(done.error());

        // Optional enhanced shaders inspect the same mesh, camera and transform.
        // Each isolated capture excludes the background and later postprocessing.
        if (options->analysis_directory && !captured) {
            auto sweep = ship->diagnose(*frame, *view, draw,
                vng::analysis::CaptureRequest::diagnostic()
                    .observe<flight::WorldPosition>().observe<flight::WorldNormal>()
                    .observe<flight::Radiance>());
            if (!sweep) return fail(sweep.error());
            std::cout << "Diagnostic sample: " << sample_time << " seconds\n";
            const auto ship_directory = sun ? *options->analysis_directory / "ship" : *options->analysis_directory;
            if (auto done = flight::export_diagnostics(*sweep, ship_directory); !done)
                return fail(done.error());
            if (sun) {
                auto evidence = sun->diagnose(*frame, *view, solar_draw,
                    vng::analysis::CaptureRequest::diagnostic().observe<example::sun::WorldPosition>()
                        .observe<example::sun::SurfaceNormal>().observe<example::sun::Radiance>());
                if (!evidence) return fail(evidence.error());
                if (auto done = example::sun::export_diagnostics(*evidence,
                        *options->analysis_directory / "sun", solar_draw.position); !done)
                    return fail(done.error());
            }
        }
        if (auto done = frame->end(); !done) return example::fail(done.error());
        if (softened) {
            auto filtered = render::begin_frame(device, *softened, {
                .extent = *extent, .color_encoding = render::ColorEncoding::linear,
                .clear_color = {}, .clear_depth = {}});
            if (!filtered) return example::fail(filtered.error());
            if (auto done = softening->apply(*filtered, target->color()); !done) return fail(done.error());
            if (auto done = filtered->end(); !done) return example::fail(done.error());
        }

        // Bloom + tone mapping + hardware display encoding. This works even
        // when the native window cannot supply an sRGB framebuffer.
        auto output = render::begin_frame(device, display->target(), {
            .extent = *extent, .color_encoding = render::ColorEncoding::srgb,
            .clear_color = {}, .clear_depth = {}});
        if (!output) return example::fail(output.error());
        auto settings = scene->bloom;
        if (!bloom_enabled) settings.strength = 0;
        if (auto done = bloom->apply(*output, softened ? softened->color() : target->color(), settings); !done)
            return fail(done.error());
        if (auto done = output->end(); !done) return example::fail(done.error());
        if (auto done = display->copy_to_window(device); !done) return fail(done.error());
        if (options->screenshot_path && !captured) {
            if (auto done = flight::save_screenshot(device, *extent, *options->screenshot_path); !done)
                return fail(done.error());
            std::cout << "Screenshot: " << *options->screenshot_path << " (t=" << sample_time << ")\n";
        }
        captured = true;
        if (auto done = loop.present(); !done) return example::fail(done.error());
    }
}
