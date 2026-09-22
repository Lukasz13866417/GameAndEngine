#include "support/glfw_opengl_session.hpp"
#include "support/presentation.hpp"
#include "support/ui_options.hpp"
#include "support/window_loop.hpp"
#include <vng/ui_opengl/ui_renderer.hpp>
#include <chrono>
#include <iostream>

namespace {
enum class Quality { low, medium, high };
int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}
} // namespace
int main(int argc, char** argv) {
    const auto options = example::parse_ui_options(argc, argv);
    if (!options)
        return fail(options.error());
    if (options->help) {
        std::cout << example::ui_help;
        return 0;
    }
    auto font = vng::text::Font::load(VNG_EXAMPLE_FONT_PATH);
    if (!font)
        return fail(font.error().message);
    auto app = example::GlfwOpenGLSession::create(
        {.width = 980, .height = 720, .title = "Vibe Engine / UI"},
        {.debug = true,
         .samples = 0,
         .default_framebuffer_encoding = vng::render::ColorEncoding::linear});
    if (!app)
        return example::fail(app.error());
    auto& device = app->device();
    auto renderer = vng::render::make_ui_renderer(device);
    if (!renderer)
        return fail(renderer.error().message);
    auto display = example::DisplaySurface::create(device, app->window().framebuffer_extent());
    if (!display)
        return fail(display.error().message);

    vng::ui::Screen screen{vng::ui::dark_theme(*font)};
    auto panel = screen.column().position({24, 24}).width(370).padding(16).gap(10);
    panel.label("VIBE / FLIGHT CONTROLS");
    auto actions = panel.row().padding(0).gap(10);
    auto pause = actions.button("Pause").width(158);
    auto restart = actions.button("Restart").width(170);
    auto bloom = panel.checkbox("Bloom").value(true);
    auto white = panel.checkbox("White spots");
    auto quality = panel
                       .dropdown<Quality>("Quality", {{Quality::low, "Low"},
                                                      {Quality::medium, "Medium"},
                                                      {Quality::high, "High"}})
                       .value(Quality::high);
    panel.label("Ship name");
    auto name = panel.text_input().value("Kestrel");
    panel.label("Notes  /  Ctrl+Enter submits");
    auto notes = panel.text_area().height(116).value(
        "Edit this text.\nSelection, clipboard and undo work.\nUnicode: Zażółć  é  Ελληνικά");
    auto status = panel.label("Ready");

    auto info = screen.column().position({420, 24}).width(536).padding(16).gap(8);
    info.label("POLLING, NOT CALLBACKS");
    auto clock = info.label("Flight time: 0 s");
    auto held = info.label("Hold a button to inspect isPressed()");
    auto settings = info.label("Bloom: on  |  White spots: off");
    info.label("Tab / Shift+Tab: move focus");
    info.label("Space / Enter: activate a button");
    info.label("Arrows / Home / End: move the caret");
    info.label("Ctrl+A / C / X / V: select and clipboard");
    info.label("Ctrl+Z / Y: undo and redo");
    info.label("Escape: dismiss popup or blur field");
    auto scroll = screen.column().position({420, 512}).width(536).height(184).padding(16).gap(6);
    scroll.label("SCROLL THIS PANEL");
    for (int i = 0; i < 12; ++i)
        scroll.label("Telemetry channel " + std::to_string(i + 1));

    bool paused{}, captured{};
    float time{};
    auto previous = std::chrono::steady_clock::now();
    example::WindowLoop loop{app->window(), options->frames};
    while (auto extent = loop.next_extent()) {
        auto now = std::chrono::steady_clock::now();
        const auto dt = std::chrono::duration<float>(now - previous).count();
        previous = now;
        auto input = vng::ui::update(screen, app->window(), dt);
        if (!input)
            return fail(input.error().message);
        if (input->keyDown(vng::input::Key::escape))
            break;
        if (pause.clicked()) {
            paused = !paused;
            pause.text(paused ? "Resume" : "Pause");
        }
        if (restart.clicked())
            time = 0;
        if (!paused)
            time += dt;
        if (auto text = name.changedText())
            status.text("Name changed: " + std::string(*text));
        if (auto text = notes.submittedText())
            status.text("Notes submitted: " + std::to_string(text->size()) + " bytes");
        if (auto selected = quality.changedValue())
            status.text("Quality: " + std::to_string(static_cast<int>(*selected)));
        clock.text("Flight time: " + std::to_string(static_cast<int>(time)) + " s");
        held.text(pause.isPressed() || restart.isPressed() ? "isPressed() = true"
                                                           : "isPressed() = false");
        settings.text(std::string("Bloom: ") + (bloom.value() ? "on" : "off") +
                      "  |  White spots: " + (white.value() ? "on" : "off"));

        if (auto resized = display->resize(device, *extent); !resized)
            return fail(resized.error().message);
        auto frame =
            vng::render::begin_frame(device, display->target(),
                                     {.extent = *extent,
                                      .color_encoding = vng::render::ColorEncoding::srgb,
                                      .clear_color = std::array<float, 4>{.008F, .012F, .024F, 1},
                                      .clear_depth = {}});
        if (!frame)
            return fail(frame.error().message);
        if (auto drawn = renderer->render(*frame, screen); !drawn)
            return fail(drawn.error().message);
        if (auto ended = frame->end(); !ended)
            return fail(ended.error().message);
        if (auto copied = display->copy_to_window(device); !copied)
            return fail(copied.error().message);
        if (options->screenshot && !captured) {
            if (auto saved = example::save_screenshot(device, *extent, *options->screenshot);
                !saved)
                return fail(saved.error().message);
            captured = true;
        }
        if (auto presented = loop.present(); !presented)
            return example::fail(presented.error());
    }
}
