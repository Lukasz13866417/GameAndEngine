#include "support/diagnostics.hpp"
#include "support/glfw_opengl_session.hpp"
#include "support/options.hpp"
#include "support/rigging_scene.hpp"
#include "support/window_loop.hpp"

#include <vng/gfx/camera.hpp>
#include <vng/opengl/frame.hpp>
#include <vng/rig_opengl/skinned_mesh_renderer.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <iostream>

int main(int argc, char** argv)
{
    namespace rig = vng::rig;
    namespace render = vng::render;
    auto options = example::parse_frame_options(argc, argv);
    if (!options) return example::fail(options.error());

    rig::ArmatureBuilder bones;
    const auto root = bones.add_bone("root");
    const auto middle = bones.add_bone("middle", root, {.translation = {0, 1, 0}});
    const auto tip = bones.add_bone("tip", middle, {.translation = {0, 1, 0}});
    auto armature = bones.build();
    if (!armature) return example::fail(armature.error());

    // Geometry and armature are independent. This binding owns their association.
    auto tube = example::rigging::make_tube();
    auto binding = rig::bind(std::move(tube.mesh), *armature);
    if (!binding) return example::fail(binding.error());
    for (std::size_t i = 0; i < tube.weights.size(); ++i) {
        const auto& weights = tube.weights[i];
        if (auto set = binding->set_weights(i, {
                {root, weights[0]}, {middle, weights[1]}, {tip, weights[2]}}); !set)
            return example::fail(set.error());
    }

    auto app = example::GlfwOpenGLSession::create(
        {.width = 1000, .height = 720, .title = "Vibe Engine - skeletal deformation"},
        {.debug = true, .samples = 0,
         .default_framebuffer_encoding = render::ColorEncoding::linear});
    if (!app) return example::fail(app.error());

    // Shaders, static geometry, packed influences and palette buffers are internal.
    auto renderer = render::make_skinned_mesh_renderer(app->device(), *binding);
    if (!renderer) return example::fail(renderer.error());
    auto left = armature->rest_pose();
    auto right = armature->rest_pose();

    vng::gfx::Camera camera;
    camera.set_position({3.0F, 2.0F, 6.5F}).look_at({0, 0, 0})
        .set_perspective({.vertical_fov = vng::degrees(45.0F),
                          .near_plane = 0.1F, .far_plane = 100.0F});
    std::cout << "One mesh and armature; two independent poses. Blue = root, orange = tip.\n";

    const auto start = std::chrono::steady_clock::now();
    example::WindowLoop loop{app->window(), options->frame_limit};
    bool reported = false;
    while (const auto extent = loop.next_extent()) {
        const auto time = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
        auto bend = rig::rotation({0, 0, 1}, vng::degrees(35.0F * std::sin(time + 0.8F)));
        auto curl = rig::rotation({1, 0, 0}, vng::degrees(45.0F * std::sin(time + 1.5F)));
        if (!bend) return example::fail(bend.error());
        if (!curl) return example::fail(curl.error());
        if (auto set = left.set_local_rotation(middle, *bend); !set) return example::fail(set.error());
        if (auto set = left.set_local_rotation(tip, *curl); !set) return example::fail(set.error());
        if (auto set = right.set_local_rotation(middle, *curl); !set) return example::fail(set.error());
        if (auto set = right.set_local_rotation(tip, *bend); !set) return example::fail(set.error());

        auto frame = render::begin_frame(app->device(), {
            .extent = *extent, .color_encoding = render::ColorEncoding::linear,
            .clear_color = std::array<vng::f32, 4>{0.018F, 0.024F, 0.045F, 1},
            .clear_depth = 1.0F});
        if (!frame) return example::fail(frame.error());
        auto view = render::RenderView::create(camera, *extent);
        if (!view) return example::fail(view.error());
        const std::array draws{
            render::SkinnedDraw{.pose = left, .transform = {.translation = {-0.95F, -1.5F, 0}}},
            render::SkinnedDraw{.pose = right, .transform = {.translation = {0.95F, -1.5F, 0}}},
        };
        if (auto drawn = renderer->render(*frame, *view, draws); !drawn)
            return example::fail(drawn.error());
        if (!reported) {
            const auto stats = renderer->stats();
            std::cout << stats.draw_calls << " draws, " << stats.triangles << " triangles, "
                      << stats.palette_uploads << " palette uploads; no vertex re-uploads.\n";
            reported = true;
        }
        if (auto ended = frame->end(); !ended) return example::fail(ended.error());
        if (auto presented = loop.present(); !presented) return example::fail(presented.error());
    }
}
