// vng_soldier_demo: the soldier modelled in Blender, imported with
// examples/tools/import_model.py, walking on a floor that scrolls under him
// at the clip's own speed, so his planted foot stays put. He and the floor
// turn slowly in front of a fixed camera: the skinned renderer's light is
// fixed in the world, so turning him keeps him lit from the key side. The
// rig's skinned renderer deforms the bind-pose mesh on the GPU from a pose.
#include "character/character.hpp"
#include "support/diagnostics.hpp"
#include "support/glfw_opengl_session.hpp"
#include "support/options.hpp"
#include "support/presentation.hpp"
#include "support/window_loop.hpp"

#include <vng/gfx/camera.hpp>
#include <vng/opengl/frame.hpp>
#include <vng/rig_opengl/skinned_mesh_renderer.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <numbers>
#include <cmath>
#include <iostream>
#include <string>

namespace {
using namespace vng;
int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}
int fail(const rig::Diagnostic& error) { return fail(error.message); }
constexpr std::string_view usage = R"(vng_soldier_demo [options]
  --mesh PATH          skinned .vmesh to show (default: the soldier)
  --clip NAME          clip to play (default: the first)
  --time S             start at S seconds into the clip
  --speed X            playback speed (default 1)
  --in-place           keep the floor still: walk on the spot
  --still              don't turn him
  --frames N           quit after N frames
  --angle DEG          camera direction around him, 0 = in front (default 65)
  --distance M         camera distance (default 3.7)
  --look Y             height the camera aims at (default 0.85)
  --screenshot NEW.png render one frame at --time in a hidden window, save it and exit
  --record NEW_DIR     render --seconds of frames at --fps (default 4 s at 60) into a new
                       directory in a hidden window, then exit
Keys: Space pause, Left/Right step one frame, Escape quit.
)";

struct Options {
    std::filesystem::path mesh{VNG_SOLDIER_MESH};
    std::string clip;
    f32 time{}, speed{1}, angle{65}, distance{3.7F}, look{.85F}, seconds{4}, fps{60};
    bool in_place{}, still{}, help{};
    std::optional<u64> frames;
    std::optional<std::filesystem::path> screenshot, record;
};
std::optional<Options> parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool more = i + 1 < argc;
        try {
            if (arg == "--help") o.help = true;
            else if (arg == "--mesh" && more) o.mesh = argv[++i];
            else if (arg == "--clip" && more) o.clip = argv[++i];
            else if (arg == "--time" && more) o.time = std::stof(argv[++i]);
            else if (arg == "--speed" && more) o.speed = std::stof(argv[++i]);
            else if (arg == "--in-place") o.in_place = true;
            else if (arg == "--still") o.still = true;
            else if (arg == "--frames" && more) o.frames = std::stoull(argv[++i]);
            else if (arg == "--screenshot" && more) o.screenshot = argv[++i];
            else if (arg == "--record" && more) o.record = argv[++i];
            else if (arg == "--angle" && more) o.angle = std::stof(argv[++i]);
            else if (arg == "--distance" && more) o.distance = std::stof(argv[++i]);
            else if (arg == "--look" && more) o.look = std::stof(argv[++i]);
            else if (arg == "--seconds" && more) o.seconds = std::stof(argv[++i]);
            else if (arg == "--fps" && more) o.fps = std::stof(argv[++i]);
            else return std::nullopt;
        } catch (const std::exception&) { return std::nullopt; }
    }
    return o;
}

// A checkerboard of one-metre tiles, bound to a one-bone armature so the
// skinned renderer can draw it too; it scrolls by its draw transform. Beyond
// 10 m the tiles fade into the sky: lit by the renderer's fixed light, the
// fade colour lands exactly on the clear colour, so the floor has no edge.
constexpr f32 tile = 1.F;
constexpr int tiles = 80;
constexpr Vec4 sky{.33F, .40F, .48F, 1};
character::Mesh floor_mesh() {
    // An up-facing surface receives 0.75 * dot(up, light) + 0.25 of its colour.
    const f32 lit = .75F * .6F / std::sqrt(.3F * .3F + .6F * .6F + 1.F) + .25F;
    character::Mesh mesh{static_cast<std::size_t>(tiles * tiles * 4)};
    std::vector<gfx::TriangleFace> faces;
    const f32 start = -tile * tiles / 2;
    for (int row = 0; row < tiles; ++row)
        for (int column = 0; column < tiles; ++column) {
            const auto base = static_cast<u32>((row * tiles + column) * 4);
            const f32 x = start + tile * static_cast<f32>(column), z = start + tile * static_cast<f32>(row);
            const Vec4 color = (row + column) % 2 ? Vec4{.40F, .41F, .38F, 1} : Vec4{.47F, .48F, .45F, 1};
            const std::array<Vec3, 4> corners{Vec3{x, 0, z}, Vec3{x + tile, 0, z}, Vec3{x + tile, 0, z + tile}, Vec3{x, 0, z + tile}};
            for (u32 k = 0; k < 4; ++k) {
                const auto distance = std::hypot(corners[k].x, corners[k].z);
                const auto t = std::clamp((distance - 10.F) / 28.F, 0.F, 1.F);
                const auto fade = t * t * (3 - 2 * t);
                const auto mix = [&](f32 a, f32 b) { return a + (b - a) * fade; };
                auto& vertex = mesh.vertices()[base + k];
                vertex.set(gfx::Position{}, corners[k]);
                vertex.set(gfx::Normal{}, Vec3{0, 1, 0});
                vertex.set(gfx::Color{}, Vec4{mix(color.x, sky.x / lit), mix(color.y, sky.y / lit), mix(color.z, sky.z / lit), 1});
            }
            faces.push_back({base, base + 2, base + 1});
            faces.push_back({base, base + 3, base + 2});
        }
    mesh.faces() = std::move(faces);
    return mesh;
}
} // namespace

int main(int argc, char** argv) {
    namespace rig = vng::rig;
    namespace render = vng::render;
    const auto options = parse(argc, argv);
    if (!options || options->help) {
        std::cout << usage;
        return options ? 0 : 2;
    }
    auto soldier = character::Character::load(options->mesh);
    if (!soldier) return example::fail(soldier.error());
    const auto* clip = options->clip.empty() ? (soldier->rig().clips.empty() ? nullptr : &soldier->rig().clips.front())
                                             : soldier->clip(options->clip);
    if (!clip) return example::fail(example::UsageError{"no such clip in " + options->mesh.string()});

    rig::ArmatureBuilder floor_bones;
    (void)floor_bones.add_bone("floor");
    auto floor_armature = floor_bones.build();
    if (!floor_armature) return fail(floor_armature.error());
    auto floor = rig::bind(floor_mesh(), *floor_armature);
    if (!floor) return fail(floor.error());
    const auto floor_bone = floor_armature->bone("floor").value();
    for (std::size_t v = 0; v < floor->mesh().vertex_count(); ++v)
        if (auto weight = floor->set_weights(v, {{floor_bone, 1.F}}); !weight) return fail(weight.error());

    const bool recording = options->record.has_value();
    const bool hidden = options->screenshot.has_value() || recording;
    if (recording && !std::filesystem::create_directory(*options->record))
        return fail(options->record->string() + " exists or cannot be made; recordings go into a new directory");
    auto app = example::GlfwOpenGLSession::create(
        {.width = 1280, .height = 800, .title = "Vibe Engine - soldier", .visible = !hidden},
        {.debug = false, .samples = 0, .default_framebuffer_encoding = render::ColorEncoding::linear});
    if (!app) {
        (void)example::fail(app.error());
        return hidden ? 77 : 1; // scripted runs: 77 lets a harness skip without OpenGL
    }
    auto& device = app->device();
    auto renderer = render::make_skinned_mesh_renderer(device, soldier->binding(), render::SkinnedRendererOptions{.max_influences = 8});
    if (!renderer) return example::fail(renderer.error());
    auto floor_renderer = render::make_skinned_mesh_renderer(device, *floor);
    if (!floor_renderer) return example::fail(floor_renderer.error());
    auto display = example::DisplaySurface::create(device, app->window().framebuffer_extent(), true);
    if (!display) return fail(display.error().message);

    auto pose = soldier->armature().rest_pose();
    const auto floor_pose = floor_armature->rest_pose();
    std::cout << soldier->rig().name << ": " << soldier->binding().mesh().vertex_count() << " vertices, "
              << soldier->armature().bone_count() << " bones; playing \"" << clip->name << "\" (" << clip->duration()
              << " s, walking " << clip->speed << " m/s).\n";

    using Clock = std::chrono::steady_clock;
    example::WindowLoop loop{app->window(), options->frames};
    auto previous = Clock::now();
    f32 time = options->time, wall = 0;
    bool paused = options->screenshot.has_value(), space_held = false, left_held = false, right_held = false;
    const auto recorded_frames = static_cast<unsigned>(std::lround(options->seconds * options->fps));
    for (unsigned rendered = 0; const auto extent = loop.next_extent(); ++rendered) {
        using vng::window::Key;
        auto& window = app->window();
        if (window.key_down(Key::escape)) break;
        const auto edge = [&](Key key, bool& held) { const bool down = window.key_down(key); const bool pressed = down && !held; held = down; return pressed; };
        if (edge(Key::space, space_held)) paused = !paused;
        if (edge(Key::left, left_held)) time -= 1.F / clip->fps;
        if (edge(Key::right, right_held)) time += 1.F / clip->fps;
        const auto now = Clock::now();
        // Recordings advance by whole frames of --fps, however long rendering takes.
        const auto dt = recording ? (rendered ? 1.F / options->fps : 0.F)
                                  : std::min(std::chrono::duration<f32>(now - previous).count(), .1F);
        previous = now;
        if (!paused) time += dt * options->speed;
        if (!options->screenshot) wall += dt;
        if (auto posed = soldier->pose(*clip, time, pose); !posed) return example::fail(posed.error());

        // Treadmill: the floor moves back as fast as the clip walks forward.
        const f32 walked = options->in_place ? 0.F : clip->speed * time;
        const f32 scroll = -std::fmod(walked, 2 * tile);
        // A slow turntable: he and the floor turn together under a fixed camera.
        const f32 turn = options->still ? 0.F : .12F * wall;
        const rig::Quat spin{0, std::sin(turn / 2), 0, std::cos(turn / 2)};
        const Vec3 floor_offset{std::sin(turn) * scroll, 0, std::cos(turn) * scroll};
        gfx::Camera camera;
        const f32 yaw = options->angle * std::numbers::pi_v<f32> / 180;
        camera.set_position({options->distance * std::sin(yaw), 1.45F, options->distance * std::cos(yaw)})
            .look_at({0, options->look, 0})
            .set_perspective({.vertical_fov = degrees(38.F), .near_plane = .05F, .far_plane = 120.F});

        if (auto resized = display->resize(device, *extent); !resized) return fail(resized.error().message);
        auto frame = render::begin_frame(device, display->target(), {
            .extent = *extent, .color_encoding = render::ColorEncoding::srgb,
            .clear_color = std::array<f32, 4>{sky.x, sky.y, sky.z, 1}, .clear_depth = 1.F});
        if (!frame) return example::fail(frame.error());
        auto view = render::RenderView::create(camera, *extent);
        if (!view) return example::fail(view.error());
        // The light he carries (his flashlight), turned with him; it lights him and the floor.
        std::optional<render::SpotLight> spot;
        if (const auto spots = soldier->spots(pose); !spots.empty()) {
            const auto turn_with = rig::matrix({.rotation = spin});
            const auto turned = [&](Vec3 v, f32 w) {
                return Vec3{turn_with[0].x * v.x + turn_with[1].x * v.y + turn_with[2].x * v.z + turn_with[3].x * w,
                            turn_with[0].y * v.x + turn_with[1].y * v.y + turn_with[2].y * v.z + turn_with[3].y * w,
                            turn_with[0].z * v.x + turn_with[1].z * v.y + turn_with[2].z * v.z + turn_with[3].z * w};
            };
            spot = spots.front();
            spot->position = turned(spot->position, 1);
            spot->direction = turned(spot->direction, 0);
        }
        if (auto drawn = floor_renderer->render(*frame, *view, render::SkinnedDraw{.pose = floor_pose,
                .transform = {.translation = floor_offset, .rotation = spin}, .spot = spot}); !drawn)
            return example::fail(drawn.error());
        if (auto drawn = renderer->render(*frame, *view, render::SkinnedDraw{.pose = pose, .transform = {.rotation = spin}, .spot = spot}); !drawn)
            return example::fail(drawn.error());
        if (auto ended = frame->end(); !ended) return example::fail(ended.error());
        if (auto copied = display->copy_to_window(device); !copied) return fail(copied.error().message);
        if (recording) {
            const auto path = *options->record / std::format("frame_{:04}.png", rendered);
            if (auto saved = example::save_screenshot(device, *extent, path); !saved) return fail(saved.error().message);
            if (rendered + 1 >= recorded_frames) {
                std::cout << "Recorded " << recorded_frames << " frames into " << options->record->string() << '\n';
                return 0;
            }
            continue;
        }
        if (options->screenshot && rendered >= 1) {
            if (auto saved = example::save_screenshot(device, *extent, *options->screenshot); !saved) return fail(saved.error().message);
            std::cout << "Saved " << options->screenshot->string() << '\n';
            return 0;
        }
        if (auto presented = loop.present(); !presented) return example::fail(presented.error());
    }
    return 0;
}
