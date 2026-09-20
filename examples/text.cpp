#include "support/diagnostics.hpp"
#include "support/glfw_opengl_session.hpp"
#include "support/options.hpp"
#include "support/window_loop.hpp"

#include <vng/opengl/frame.hpp>
#include <vng/opengl/text_renderer.hpp>

#include <array>
#include <iostream>

int main(int argc, char** argv)
{
    auto options = example::parse_frame_options(argc, argv);
    if (!options) return example::fail(options.error());

    auto font = vng::text::Font::load(VNG_EXAMPLE_FONT_PATH);
    if (!font) {
        std::cerr << font.error().message << '\n';
        return 1;
    }
    auto app = example::GlfwOpenGLSession::create(
        {.width = 960, .height = 620, .title = "Vibe Engine - text"},
        {.debug = true, .samples = 0,
         .default_framebuffer_encoding = vng::render::ColorEncoding::linear});
    if (!app) return example::fail(app.error());

    // The renderer owns its shaders, glyph atlases and GPU vertex buffer.
    // This factory selects the backend from the device type.
    auto text = vng::render::make_text_renderer(app->device(), *font);
    if (!text) return example::fail(text.error());

    using vng::render::TextDraw;
    const std::array labels{
        TextDraw{.text = "Vibe Engine", .position = {36, 24}, .size = 54,
                 .color = {0.5F, 0.85F, 1, 1}},
        TextDraw{.text = "Text rendering, without the plumbing.",
                 .position = {38, 101}, .size = 25},
        TextDraw{.text = "One renderer. Different sizes. Shared glyph atlases.",
                 .position = {38, 160}, .size = 19,
                 .color = {0.65F, 0.72F, 0.85F, 1}},
        TextDraw{.text = "Zażółć gęślą jaźń  ·  Ελληνικά  ·  Кириллица",
                 .position = {38, 210}, .size = 24},
        TextDraw{.text = "Kerning: AVATAR    Ligatures: office / affinity\n"
                        "Newlines and\ttabs work too.",
                 .position = {38, 263}, .size = 22},
        TextDraw{.text = "Clipped text continues beyond this boundary →",
                 .position = {38, 358}, .size = 28,
                 .color = {1, 0.65F, 0.3F, 1},
                 .clip = vng::render::TextClip{38, 358, 380, 45}},
        TextDraw{.text = "OVERLAP", .position = {38, 428}, .size = 48,
                 .color = {0.25F, 0.65F, 1, 0.7F}},
        TextDraw{.text = "OVERLAP", .position = {68, 446}, .size = 48,
                 .color = {1, 0.35F, 0.45F, 0.7F}},
        TextDraw{.text = "Positions and sizes are framebuffer pixels. Resize the window.",
                 .position = {38, 551}, .size = 17,
                 .color = {0.6F, 0.65F, 0.75F, 1}},
    };

    unsigned int frames = 0;
    example::WindowLoop loop{app->window(), options->frame_limit};
    while (const auto extent = loop.next_extent()) {
        auto frame = vng::render::begin_frame(app->device(), {
            .extent = *extent,
            .color_encoding = vng::render::ColorEncoding::linear,
            .clear_color = std::array<vng::f32, 4>{0.018F, 0.024F, 0.045F, 1},
            .clear_depth = std::nullopt,
        });
        if (!frame) return example::fail(frame.error());
        if (auto drawn = text->render(*frame, labels); !drawn)
            return example::fail(drawn.error());
        if (++frames <= 2) {
            const auto stats = text->stats();
            std::cout << "frame " << frames << ": " << stats.glyphs
                      << " glyphs, " << stats.draw_calls << " draw calls, "
                      << stats.glyph_uploads << " new glyph uploads, "
                      << stats.layout_hits << " cached layouts\n";
        }
        if (auto ended = frame->end(); !ended) return example::fail(ended.error());
        if (auto presented = loop.present(); !presented) return example::fail(presented.error());
    }
}
