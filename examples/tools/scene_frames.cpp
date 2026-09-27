// Renders a saved scene offscreen through the active scene camera, loading it
// once: at listed times, over a range at a frame rate, or from explicit eyes.
// For reviewing shots frame by frame without opening the editor or playing
// the demo in real time. Frames are named f#####_tSSS.SSS.png.
#include "../editor/animation.hpp"
#include "../editor/runtime.hpp"
#include "../editor/scene_file.hpp"
#include "../support/presentation.hpp"
#include <vng/glfw_opengl/glfw_opengl.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <string_view>
#include <vector>

namespace {
using namespace vng;
namespace project = editor_example;
f32 number(const char* text) { return std::strtof(text, nullptr); }
// An explicit eye, used instead of the scene camera.
struct Look { Vec3 eye, target; f32 zoom, far; };
int usage() {
    std::fprintf(stderr,
        "Usage: vng_scene_frames SCENE.vscene OUT_DIR WIDTH HEIGHT TIME...\n"
        "       vng_scene_frames SCENE.vscene OUT_DIR WIDTH HEIGHT --range FROM TO FPS\n"
        "       vng_scene_frames SCENE.vscene OUT_DIR WIDTH HEIGHT --look TIME EX EY EZ TX TY TZ ZOOM FAR [TIME ...]\n");
    return 1;
}
}

int main(int argc, char** argv) {
    if (argc < 6) return usage();
    const std::filesystem::path out = argv[2];
    const Extent2D extent{static_cast<u32>(std::atoi(argv[3])), static_cast<u32>(std::atoi(argv[4]))};
    if (!extent.width || !extent.height) return usage();
    std::vector<f32> times;
    std::vector<Look> looks;
    const std::string_view mode = argv[5];
    if (mode == "--range") {
        if (argc != 9) return usage();
        const double from = number(argv[6]), to = number(argv[7]), fps = number(argv[8]);
        if (fps <= 0) return usage();
        for (double t = from; t <= to + 1e-6; t += 1 / fps) times.push_back(static_cast<f32>(t));
    } else if (mode == "--look") {
        if (argc < 15 || (argc - 6) % 9) return usage();
        for (int i = 6; i + 9 <= argc; i += 9) {
            times.push_back(number(argv[i]));
            looks.push_back({{number(argv[i+1]), number(argv[i+2]), number(argv[i+3])},
                {number(argv[i+4]), number(argv[i+5]), number(argv[i+6])}, number(argv[i+7]), number(argv[i+8])});
        }
    } else
        for (int i = 5; i < argc; ++i) times.push_back(number(argv[i]));

    auto window = glfw_opengl::create_window(
        window::WindowDesc{.width = extent.width, .height = extent.height, .title = "vng_scene_frames",
                           .visible = false, .resizable = false},
        opengl::ContextDesc{.debug = false, .samples = 0, .default_framebuffer_encoding = render::ColorEncoding::linear},
        {.vsync = window::VSync::off});
    // 77: no display or OpenGL 4.6 here, as in the GPU tests.
    if (!window) { std::fprintf(stderr, "%s\n", window.error().message.c_str()); return 77; }
    auto token = window->make_current();
    if (!token) { std::fprintf(stderr, "%s\n", token.error().message.c_str()); return 77; }
    auto device = opengl::Device::create(*token);
    if (!device) { std::fprintf(stderr, "%s\n", device.error().message.c_str()); return 77; }
    project::SceneFile file;
    auto scene = file.load(argv[1]);
    if (!scene) { std::fprintf(stderr, "%s\n", scene.error().message.c_str()); return 1; }
    scene->viewport.mode = project::ViewMode::scene;
    scene->viewport.pilot_camera = true;
    auto runtime = project::Runtime::create(*device, *scene);
    if (!runtime) { std::fprintf(stderr, "%s\n", runtime.error().message.c_str()); return 1; }
    std::filesystem::create_directories(out);
    for (std::size_t i = 0; i < times.size(); ++i) {
        const auto time = times[i];
        // The scene camera as the demo plays it, far plane and all.
        auto camera = project::render_camera(*scene, time);
        if (!looks.empty()) {
            const auto& look = looks[i];
            const auto half = std::atan(std::tan(project::camera_vertical_fov * std::numbers::pi_v<f32> / 360) / look.zoom);
            camera = gfx::Camera{};
            camera.set_position(look.eye).look_at(look.target).set_perspective({
                .vertical_fov = degrees(2 * half * 180 / std::numbers::pi_v<f32>), .near_plane = .05F, .far_plane = look.far});
        }
        auto image = runtime->render(*device, {*scene, camera, extent, time, false});
        if (!image) { std::fprintf(stderr, "%s\n", image.error().message.c_str()); return 1; }
        char name[48];
        std::snprintf(name, sizeof name, "f%05zu_t%07.3f.png", i, static_cast<double>(time));
        auto written = example::write_rgba8_png(out / name, extent,
            {reinterpret_cast<const u8*>(image->pixels.data()), image->pixels.size()});
        if (!written) { std::fprintf(stderr, "%s\n", written.error().message.c_str()); return 1; }
    }
    std::printf("Rendered %zu frames into %s\n", times.size(), out.c_str());
}
