#include "scene_demo.hpp"
#include "../editor/animation.hpp"
#include "../editor/scene_file.hpp"
#include "../support/glfw_opengl_session.hpp"
#include "../support/presentation.hpp"
#include "../support/spaceflight_scene.hpp"
#include "../support/window_loop.hpp"

#include <chrono>
#include <iostream>
#include <algorithm>

namespace {
template<class Error> int report_failure(const Error& error) {
    if constexpr (requires { example::fail(error); }) return example::fail(error);
    else {
        std::cerr << error.message << '\n';
        if constexpr (requires { error.driver_log; }) std::cerr << error.driver_log << '\n';
        if constexpr (requires { error.context; })
            for (const auto& context : error.context) std::cerr << "  " << context << '\n';
        if constexpr (requires { error.generated_source; }) std::cerr << error.generated_source << '\n';
        return 1;
    }
}
}

int example::run_scene_demo(int argc, char** argv,const SceneDemo& demo) {
    namespace project = editor_example;
    namespace fleet = example::fleet;
    using namespace vng;
    auto options = example::spaceflight::parse_options(argc,argv,demo.scene);
    if (!options) return report_failure(options.error());
    if (options->help) {
        std::cout << demo.name << " [editor_scene.vscene] [--time SECONDS] [--once | --frames N]\n"
            << "  [--screenshot NEW.png] [--analyze NEW_DIRECTORY] [--no-bloom]\n"
            << "Space pauses, R restarts, Esc closes. End pose holds instead of looping.\n"
            << "Edit the same file with vng_editor_demo --scene FILE.\n";
        return 0;
    }
    project::SceneFile source;
    auto scene = source.load(options->scene_path);
    if (!scene) return report_failure(scene.error());
    scene->viewport.mode = project::ViewMode::scene;
    if (!project::has_camera(*scene)) {
        std::cerr << options->scene_path.string() << " has no scene camera; add one in the editor (Blueprints > Camera)\n";
        return 1;
    }
    if (!options->bloom) {
        scene->document.environment.bloom_strength = 0;
        auto tracks = std::vector<timeline::Track>(scene->document.timeline.tracks().begin(),
                                                   scene->document.timeline.tracks().end());
        // A render-only override in this loaded copy; no scene file is changed.
        for (auto& instance : scene->document.instances)
            if (auto* sun = std::get_if<project::SunSettings>(&instance.settings)) {
                sun->bloom = 0;
                std::erase_if(tracks,[&](const auto& track) {
                    return track.target.object == instance.id && track.target.property == "bloom";
                });
            }
        if (auto replaced = scene->document.timeline.replace(std::move(tracks)); !replaced)
            return report_failure(replaced.error());
    }
    const bool capture = options->screenshot_path || options->analysis_directory;
    if (capture && !options->fixed_time) options->fixed_time = demo.capture_time;
    if (capture && !options->frame_limit) options->frame_limit = 1;
    auto app = example::GlfwOpenGLSession::create(
        {.width=1280,.height=800,.title=demo.title},
        {.debug=true,.samples=0,.default_framebuffer_encoding=render::ColorEncoding::linear});
    if (!app) return example::fail(app.error());
    auto& device = app->device();
    auto runtime = project::Runtime::create(device,*scene);
    if (!runtime) return report_failure(runtime.error());
    std::cout << "Shared editor scene: " << options->scene_path << '\n'
              << std::ranges::count_if(scene->document.instances, [](const auto& instance) {
                    return std::holds_alternative<project::MeshSettings>(instance.settings);
                 }) << " mesh instances / "
              << scene->document.mesh_assets.size()+1 << " mesh blueprints / "
              << scene->document.timeline_duration << " seconds\n"
              << "Space: pause | R: restart | Esc: close\n";
    bool paused{}, previous_space{}, previous_r{}, captured{};
    f32 elapsed{};
    auto previous = std::chrono::steady_clock::now();
    example::WindowLoop loop{app->window(),options->frame_limit};
    while (const auto extent = loop.next_extent()) {
        if (app->window().key_down(window::Key::escape)) break;
        const bool space = app->window().key_down(window::Key::space);
        const bool restart = app->window().key_down(window::Key::r);
        const auto now = std::chrono::steady_clock::now();
        if (!paused) elapsed = std::min(scene->document.timeline_duration,
            elapsed+std::chrono::duration<f32>(now-previous).count());
        if (space && !previous_space) paused = !paused;
        if (restart && !previous_r) elapsed = 0;
        previous = now; previous_space = space; previous_r = restart;
        const auto time = std::clamp(options->fixed_time.value_or(elapsed),0.F,scene->document.timeline_duration);
        const auto camera = project::camera(*project::evaluate_camera(*scene,time),project::ViewMode::scene);
        if (auto rendered = runtime->render_frame(device,{*scene,camera,*extent,time}); !rendered)
            return report_failure(rendered.error());
        if (auto presented = runtime->present(device); !presented) return report_failure(presented.error());
        if (!captured && capture) {
            if (options->screenshot_path)
                if (auto saved = example::save_screenshot(device,*extent,*options->screenshot_path); !saved)
                    return report_failure(saved.error());
            if (options->analysis_directory)
                if (auto inspected = fleet::inspect(device,*runtime,*scene,camera,*extent,time,
                                                    *options->analysis_directory,demo.evidence); !inspected)
                    return report_failure(inspected.error());
            captured = true;
        }
        if (auto presented = loop.present(); !presented) return report_failure(presented.error());
    }
    return 0;
}
