#include "support/glfw_opengl_session.hpp"
#include "support/glow_options.hpp"
#include "support/glow_scene.hpp"
#include "support/window_loop.hpp"

#include <vng/opengl/bloom.hpp>
#include <vng/opengl/mesh_renderer.hpp>
#include <vng/opengl/render_target.hpp>
#include <vng/providers/image.hpp>
#include <vng/providers/mesh.hpp>
#include <vng/providers/target.hpp>
#ifdef VNG_GLOW_TEXT
#include <vng/opengl/text_renderer.hpp>
#include <vng/providers/font.hpp>
#endif

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
    namespace render = vng::render;
    namespace providers = vng::providers;
    namespace scene = example::glow;

    auto options = example::parse_glow_options(argc, argv);
    if (!options) return example::fail(options.error());
    auto app = example::GlfwOpenGLSession::create(
        {.width = 1200, .height = 780, .title = "Vibe Engine - HDR glow / resource providers"},
        {.debug = true, .samples = 0,
            .default_framebuffer_encoding = render::ColorEncoding::linear});
    if (!app) return example::fail(app.error());
    auto& device = app->device();

    // Providers retain reconstruction inputs; these builders may be discarded.
    auto ring_builder = render::mesh_renderer_builder(device);
    ring_builder.mesh(providers::mesh(scene::ring()));
    auto rings = ring_builder.build();
    if (!rings) return fail(rings.error());

    auto box_builder = render::mesh_renderer_builder(device);
    box_builder.mesh(providers::mesh(scene::box()));
    auto boxes = box_builder.build();
    if (!boxes) return fail(boxes.error());

    auto floor_builder = render::mesh_renderer_builder(device);
    floor_builder.mesh(providers::mesh(scene::floor()));
    floor_builder.albedo(providers::texture(scene::grid_texture(), {
        .sampler = {
            .min_filter = vng::gfx::ImageFilter::linear,
            .mag_filter = vng::gfx::ImageFilter::linear,
            .mip_filter = vng::gfx::MipmapFilter::linear,
            .wrap_u = vng::gfx::ImageWrap::repeat,
            .wrap_v = vng::gfx::ImageWrap::repeat,
        },
    }));
    auto floor = floor_builder.build();
    if (!floor) return fail(floor.error());

    const auto initial = app->window().framebuffer_extent();
    auto target = providers::render_target().provide(device, initial);
    if (!target) return fail(target.error());
    auto bloom = render::bloom_builder(device).levels(6).build(initial);
    if (!bloom) return fail(bloom.error());
#ifdef VNG_GLOW_TEXT
    auto text_builder = render::text_renderer_builder(device);
    text_builder.font(providers::font_file(VNG_EXAMPLE_FONT_PATH));
    auto text = text_builder.build();
    if (!text) return fail(text.error());
#endif

    vng::gfx::Camera camera;
    camera.set_position({4.5F, 3.3F, 9.5F}).look_at({0, -.15F, 0})
        .set_perspective({.vertical_fov = vng::degrees(44), .near_plane = .1F, .far_plane = 100});
    bool enabled = options->bloom;
    bool paused = false;
    bool previous_b = false, previous_r = false, previous_space = false;
    float time = 0;
    auto previous = std::chrono::steady_clock::now();
    std::cout << "B: bloom | Space: pause | R: reload providers | Esc: close\n"
              << "Linear RGBA16F scene -> bloom pyramid -> tone mapping. No render graph.\n";

    example::WindowLoop loop{app->window(), options->frame_limit};
    while (const auto extent = loop.next_extent()) {
        using vng::window::Key;
        if (app->window().key_down(Key::escape)) break;
        const auto b = app->window().key_down(Key::b);
        const auto r = app->window().key_down(Key::r);
        const auto space = app->window().key_down(Key::space);
        if (b && !previous_b) enabled = !enabled;
        if (space && !previous_space) paused = !paused;
        previous_b = b;
        previous_space = space;

        // Resource updates happen between frames, before anything borrows them.
        if ((r && !previous_r) || options->reload) {
            if (auto done = rings->reload(device); !done) return fail(done.error());
            if (auto done = boxes->reload(device); !done) return fail(done.error());
            if (auto done = floor->reload(device); !done) return fail(done.error());
            if (auto done = target->reload(device); !done) return fail(done.error());
            if (auto done = bloom->reload(device); !done) return fail(done.error());
            std::cout << "Reloaded mesh, texture, shader and target providers.\n";
            options->reload = false;
        }
        previous_r = r;
        if (auto resized = target->resize(device, *extent); !resized) return fail(resized.error());
        if (auto resized = bloom->resize(device, *extent); !resized) return fail(resized.error());
        const auto now = std::chrono::steady_clock::now();
        if (!paused) time += std::chrono::duration<float>(now - previous).count();
        previous = now;
        auto view = render::RenderView::create(camera, *extent);
        if (!view) { std::cerr << view.error().message << '\n'; return 1; }

        // First draw ordinary geometry into a sampleable HDR target.
        auto scene_frame = render::begin_frame(device, *target, {
            .extent = *extent,
            .color_encoding = render::ColorEncoding::linear,
            .clear_color = std::array<float, 4>{.006F, .009F, .019F, 1},
            .clear_depth = 1,
        });
        if (!scene_frame) return example::fail(scene_frame.error());
        if (auto draw = floor->render(*scene_frame, *view, render::SurfaceDraw{}); !draw)
            return fail(draw.error());
        if (auto draw = boxes->render(*scene_frame, *view, scene::plinths()); !draw)
            return fail(draw.error());
        if (auto draw = rings->render(*scene_frame, *view, scene::rings(time)); !draw)
            return fail(draw.error());
        if (auto ended = scene_frame->end(); !ended) return example::fail(ended.error());

        // Bloom is an explicit image operation, not a special scene renderer.
        auto output = render::begin_frame(device, {
            .extent = *extent,
            .color_encoding = render::ColorEncoding::linear,
            .clear_color = {}, .clear_depth = {},
        });
        if (!output) return example::fail(output.error());
        if (auto applied = bloom->apply(*output, target->color(), {
                .threshold = 1, .strength = enabled ? .45F : 0, .exposure = 1.15F,
            }); !applied)
            return fail(applied.error());
#ifdef VNG_GLOW_TEXT
        const std::array labels{
            render::TextDraw{.text = "GLOW LAB", .position = {30, 23},
                .size = 32, .color = {.75F, .88F, 1, 1}},
            render::TextDraw{.text = "HDR / EMISSION / BLOOM", .position = {32, 66},
                .size = 16, .color = {.36F, .53F, .7F, 1}},
            render::TextDraw{.text = enabled ? "BLOOM ON" : "BLOOM OFF",
                .position = {32, static_cast<float>(extent->height) - 66}, .size = 20,
                .color = enabled ? vng::Vec4{.15F, .85F, 1, 1} : vng::Vec4{.8F, .45F, .2F, 1}},
            render::TextDraw{.text = "B  bloom    SPACE  pause    R  reload resources    ESC  close",
                .position = {32, static_cast<float>(extent->height) - 34},
                .size = 16, .color = {.45F, .55F, .7F, 1}},
        };
        if (auto drawn = text->render(*output, labels); !drawn) return example::fail(drawn.error());
#endif
        if (auto ended = output->end(); !ended) return example::fail(ended.error());
        if (auto presented = loop.present(); !presented) return example::fail(presented.error());
    }
}
