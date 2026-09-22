#include "support/glfw_opengl_session.hpp"
#include "support/presentation.hpp"
#include "support/sun_options.hpp"
#include "support/sun_renderer.hpp"
#include "support/sun_inspection.hpp"
#include "support/sun_softening.hpp"
#include "support/window_loop.hpp"

#include <vng/bloom_opengl/bloom.hpp>
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
    namespace solar = example::sun;
    namespace render = vng::render;
    auto options = solar::parse_options(argc, argv);
    if (!options) return fail(options.error());
    if (options->help) { std::cout << solar::usage(); return 0; }
    auto app = example::GlfwOpenGLSession::create(
        {.width = options->extent.width, .height = options->extent.height,
            .title = "HELIOS / solar surface"},
        {.debug = true, .samples = 0,
            .default_framebuffer_encoding = render::ColorEncoding::linear});
    if (!app) return example::fail(app.error());
    auto& device = app->device();
    const auto initial = app->window().framebuffer_extent();
    std::cout << "HELIOS | generating solar surface and plasma strands...\n" << std::flush;

    // This renderer owns its meshes, procedural map, and DSL shaders.
    auto sun = solar::SunRenderer::create(device);
    if (!sun) return fail(sun.error());
    auto target = vng::providers::render_target().provide(device, initial);
    if (!target) return fail(target.error());
    auto softened = vng::providers::render_target({.depth = false}).provide(device, initial);
    if (!softened) return fail(softened.error());
    auto softening = solar::SunSoftening::create(device);
    if (!softening) return fail(softening.error());
    auto bloom = render::bloom_builder(device).levels(6).build(initial);
    if (!bloom) return fail(bloom.error());
    auto display = example::DisplaySurface::create(device, initial);
    if (!display) return fail(display.error());
    vng::gfx::Camera camera;
    camera.set_position({0, 0, 4.15F}).look_at({0, 0, 0}).set_perspective({
        .vertical_fov = vng::degrees(36.0F), .near_plane = 0.1F, .far_plane = 30.0F});
    bool paused = false, previous_space = false, previous_b = false, previous_d = false, previous_w = false;
    bool bloom_enabled = options->bloom, displaced = options->displacement, captured = false;
    bool white_spots = options->white_spots;
    float seconds = 0;
    auto previous = std::chrono::steady_clock::now();
    std::cout << "Space: pause | B: bloom | D: displacement | W: white spots | R: reset | Esc: close\n"
        << "A cinematic, false-color solar visualization; not a plasma simulation.\n";

    example::WindowLoop loop{app->window(), options->frame_limit};
    while (const auto extent = loop.next_extent()) {
        using vng::window::Key;
        if (app->window().key_down(Key::escape)) break;
        const bool space = app->window().key_down(Key::space);
        const bool b = app->window().key_down(Key::b);
        const bool d = app->window().key_down(Key::d);
        const bool w = app->window().key_down(Key::w);
        if (space && !previous_space) paused = !paused;
        if (b && !previous_b) bloom_enabled = !bloom_enabled;
        if (d && !previous_d) displaced = !displaced;
        if (w && !previous_w) {
            white_spots = !white_spots;
            std::cout << "White spots: " << (white_spots ? "on" : "off") << '\n';
        }
        const auto now = std::chrono::steady_clock::now();
        if (!paused) seconds += std::chrono::duration<float>(now - previous).count();
        if (app->window().key_down(Key::r)) seconds = 0;
        previous = now; previous_space = space; previous_b = b; previous_d = d;
        previous_w = w;

        if (auto done = target->resize(device, *extent); !done) return fail(done.error());
        if (auto done = softened->resize(device, *extent); !done) return fail(done.error());
        if (auto done = bloom->resize(device, *extent); !done) return fail(done.error());
        if (auto done = display->resize(device, *extent); !done) return fail(done.error());
        auto view = render::RenderView::create(camera, *extent);
        if (!view) return example::fail(view.error());
        const solar::SunDraw draw{.time = options->fixed_time.value_or(seconds),
            .displacement = displaced ? 1.0F : 0.0F, .white_spots = white_spots};
        auto frame = render::begin_frame(device, *target, {
            .extent = *extent, .color_encoding = render::ColorEncoding::linear,
            .clear_color = std::array<float, 4>{0, 0, 0, 1}, .clear_depth = 1});
        if (!frame) return example::fail(frame.error());
        if (auto done = sun->render(*frame, *view, draw); !done) return fail(done.error());
        if (options->analysis_directory && !captured) {
            auto evidence = sun->diagnose(*frame, *view, draw,
                vng::analysis::CaptureRequest::diagnostic().observe<solar::WorldPosition>()
                    .observe<solar::SurfaceNormal>().observe<solar::Radiance>());
            if (!evidence) return fail(evidence.error());
            if (auto done = solar::export_diagnostics(*evidence, *options->analysis_directory); !done)
                return fail(done.error());
        }
        if (auto done = frame->end(); !done) return example::fail(done.error());

        // A small screen-space filter softens fine granulation and strand edges.
        // Keep this in linear HDR; diagnostic surface data above stays untouched.
        auto filtered = render::begin_frame(device, *softened, {
            .extent = *extent, .color_encoding = render::ColorEncoding::linear,
            .clear_color = {}, .clear_depth = {}});
        if (!filtered) return example::fail(filtered.error());
        if (auto done = softening->apply(*filtered, target->color()); !done) return fail(done.error());
        if (auto done = filtered->end(); !done) return example::fail(done.error());

        // Keep physical window encoding separate from linear HDR rendering.
        auto output = render::begin_frame(device, display->target(), {
            .extent = *extent, .color_encoding = render::ColorEncoding::srgb,
            .clear_color = {}, .clear_depth = {}});
        if (!output) return example::fail(output.error());
        if (auto done = bloom->apply(*output, softened->color(), {
                .threshold = 6.5F, .strength = bloom_enabled ? 0.24F : 0.0F, .exposure = 0.9F}); !done)
            return fail(done.error());
        if (auto done = output->end(); !done) return example::fail(done.error());
        if (auto done = display->copy_to_window(device); !done) return fail(done.error());
        if (options->screenshot_path && !captured) {
            if (auto done = example::save_screenshot(device, *extent, *options->screenshot_path); !done)
                return fail(done.error());
            std::cout << "Screenshot: " << *options->screenshot_path << " (t=" << draw.time << ")\n";
        }
        captured = true;
        if (auto done = loop.present(); !done) return example::fail(done.error());
    }
}
