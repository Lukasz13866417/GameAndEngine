#include "character_view.hpp"
#include <vng/opengl/frame.hpp>
#include <vng/render/view.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace review {
using namespace vng;
namespace {
resources::Diagnostic failure(std::string message) {
    resources::Diagnostic error;
    error.message = std::move(message);
    return error;
}
f32 radians(f32 degrees) { return degrees * std::numbers::pi_v<f32> / 180; }
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, f32 s) { return {a.x * s, a.y * s, a.z * s}; }
f32 dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3 unit(Vec3 v) { return v * (1 / std::sqrt(dot(v, v))); }
Vec3 transform(const Mat4& m, Vec3 p) {
    return {m[0].x * p.x + m[1].x * p.y + m[2].x * p.z + m[3].x, m[0].y * p.x + m[1].y * p.y + m[2].y * p.z + m[3].y,
            m[0].z * p.x + m[1].z * p.y + m[2].z * p.z + m[3].z};
}

constexpr f32 vertical_fov = 38;
Vec3 target_of(const Orbit& o) { return {0, o.look, 0}; }
Vec3 eye_of(const Orbit& o) {
    const f32 yaw = radians(o.yaw), pitch = radians(o.pitch);
    return target_of(o) + Vec3{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)} * o.distance;
}
gfx::Camera camera_of(const Orbit& o) {
    gfx::Camera camera;
    camera.set_position(eye_of(o)).look_at(target_of(o))
        .set_perspective({.vertical_fov = degrees(vertical_fov), .near_plane = .05F, .far_plane = 120.F});
    return camera;
}
// The view ray through a normalized top-left point of an image of this orbit.
Vec3 ray_of(const Orbit& o, Vec2 normalized, Extent2D extent) {
    const auto forward = unit(target_of(o) - eye_of(o));
    const auto right = unit(cross(forward, {0, 1, 0}));
    const auto up = cross(right, forward);
    const f32 half = std::tan(radians(vertical_fov) / 2), aspect = static_cast<f32>(extent.width) / static_cast<f32>(extent.height);
    return unit(forward + right * ((2 * normalized.x - 1) * half * aspect) + up * ((1 - 2 * normalized.y) * half));
}
Vec3 position(const character::Mesh& mesh, std::size_t v) { return mesh.vertices()[v].get(gfx::Position{}); }
} // namespace

content::Result<character::Character> CharacterView::load(const std::filesystem::path& path) {
    return character::Character::load(path);
}

resources::Result<std::unique_ptr<CharacterView>> CharacterView::create(opengl::Device& device, character::Character body) {
    if (body.rig().clips.empty()) return std::unexpected(failure("The character has no clips to play"));
    auto stage = character::Stage::create();
    if (!stage) return std::unexpected(failure(stage.error().message));
    auto display = example::DisplaySurface::create(device, {16, 10}, true);
    if (!display) return std::unexpected(display.error());
    auto readback = opengl::Rgba8ReadbackQueue::create(device);
    if (!readback) return std::unexpected(resources::to_diagnostic(readback.error()));
    std::unique_ptr<CharacterView> view{new CharacterView(std::move(body), std::move(*stage), std::move(*display), std::move(*readback))};
    // The renderers read the bindings where the view keeps them.
    auto body_renderer = render::make_skinned_mesh_renderer(device, view->character_.binding(), render::SkinnedRendererOptions{.max_influences = 8});
    if (!body_renderer) return std::unexpected(resources::to_diagnostic(body_renderer.error()));
    view->body_.emplace(std::move(*body_renderer));
    auto floor_renderer = render::make_skinned_mesh_renderer(device, view->stage_.floor);
    if (!floor_renderer) return std::unexpected(resources::to_diagnostic(floor_renderer.error()));
    view->floor_.emplace(std::move(*floor_renderer));
    return view;
}

CharacterView::CharacterView(character::Character body, character::Stage stage, example::DisplaySurface display,
                             opengl::Rgba8ReadbackQueue readback)
    : character_(std::move(body)), stage_(std::move(stage)), display_(std::move(display)), readback_(std::move(readback)),
      pose_(character_.armature().rest_pose()), floor_pose_(stage_.armature.rest_pose()),
      clip_(&character_.rig().clips.front()) {}

f32 CharacterView::duration() const { return clip_->duration(); }

std::optional<f32> CharacterView::shown_time() const { return shown_ ? std::optional{shown_->time} : std::nullopt; }

std::vector<std::string> CharacterView::clips() const {
    std::vector<std::string> names;
    for (const auto& c : character_.rig().clips) names.push_back(c.name);
    return names;
}

bool CharacterView::clip(std::string_view name) {
    const auto* found = character_.clip(name);
    if (!found) return false;
    clip_ = found;
    return true;
}

void CharacterView::orbit(Orbit o) {
    o.pitch = std::clamp(o.pitch, -89.F, 89.F);
    o.distance = std::clamp(o.distance, .3F, 30.F);
    o.look = std::clamp(o.look, -1.F, 4.F);
    o.yaw = std::remainder(o.yaw, 360.F);
    orbit_ = o;
}

resources::Result<void> CharacterView::posed(const Frame& frame, rig::Pose& pose) const {
    if (auto done = character_.pose(*frame.clip, frame.time, pose); !done) return std::unexpected(failure(done.error().message));
    return {};
}

resources::Result<void> CharacterView::update(opengl::Device& device, f32 time, Extent2D extent) {
    if (auto ready = readback_.try_take(); !ready) return std::unexpected(resources::to_diagnostic(ready.error()));
    else if (*ready) {
        if (const auto frame = in_flight_.find((*ready)->id); frame != in_flight_.end()) {
            shown_ = frame->second;
            image_ = std::make_shared<const gfx::ImageData>(std::move((*ready)->image));
            ++revision_;
        }
        std::erase_if(in_flight_, [&](const auto& entry) { return entry.first <= (*ready)->id; });
    }
    if (extent.width == 0 || extent.height == 0) return {};
    time = std::clamp(time, 0.F, duration());
    const Frame frame{clip_, time, orbit_, extent};
    // Paused on a frame that is already shown or on its way: nothing to draw.
    if (requested_ && requested_->clip == frame.clip && requested_->time == time && requested_->orbit == orbit_ &&
        requested_->extent.width == extent.width && requested_->extent.height == extent.height)
        return {};
    if (!readback_.available()) return {};
    if (auto done = posed(frame, pose_); !done) return done;
    if (auto resized = display_.resize(device, extent); !resized) return resized;
    auto target = render::begin_frame(device, display_.target(), {.extent = extent, .color_encoding = render::ColorEncoding::srgb,
        .clear_color = std::array<f32, 4>{character::Stage::sky.x, character::Stage::sky.y, character::Stage::sky.z, 1}, .clear_depth = 1.F});
    if (!target) return std::unexpected(resources::to_diagnostic(target.error()));
    auto view = render::RenderView::create(camera_of(orbit_), extent);
    if (!view) return std::unexpected(failure(view.error().message));
    // Walking on the spot: the floor moves back as fast as the clip walks.
    const Vec3 treadmill{0, 0, character::Stage::scroll(clip_->speed, time)};
    if (auto drawn = floor_->render(*target, *view, render::SkinnedDraw{.pose = floor_pose_, .transform = {.translation = treadmill}}); !drawn)
        return std::unexpected(resources::to_diagnostic(drawn.error()));
    if (auto drawn = body_->render(*target, *view, render::SkinnedDraw{.pose = pose_}); !drawn)
        return std::unexpected(resources::to_diagnostic(drawn.error()));
    if (auto ended = target->end(); !ended) return std::unexpected(resources::to_diagnostic(ended.error()));
    const auto id = next_id_++;
    auto queued = readback_.try_submit(display_.target().framebuffer(), 0, extent, id);
    if (!queued) return std::unexpected(resources::to_diagnostic(queued.error()));
    if (*queued) {
        in_flight_.emplace(id, frame);
        requested_ = frame;
    }
    return {};
}

std::optional<Hit> CharacterView::pick(Vec2 normalized) {
    if (!shown_) return {};
    auto pose = character_.armature().rest_pose();
    if (!posed(*shown_, pose)) return {};
    const auto& binding = character_.binding();
    const auto& mesh = binding.mesh();
    std::vector<Vec3> bind(mesh.vertex_count());
    for (std::size_t v = 0; v < bind.size(); ++v) bind[v] = position(mesh, v);
    const auto moved = binding.deform_points(bind, pose);
    if (!moved) return {};
    // The nearest triangle the view ray meets (Moller-Trumbore).
    const auto eye = eye_of(shown_->orbit), ray = ray_of(shown_->orbit, normalized, shown_->extent);
    f32 nearest = std::numeric_limits<f32>::max();
    std::optional<std::tuple<std::size_t, f32, f32>> found;
    const auto& faces = mesh.faces();
    for (std::size_t f = 0; f < faces.size(); ++f) {
        const auto& p = *moved;
        const auto a = p[faces[f].vertices[0]], b = p[faces[f].vertices[1]], c = p[faces[f].vertices[2]];
        const auto ab = b - a, ac = c - a, h = cross(ray, ac);
        const f32 det = dot(ab, h);
        if (std::abs(det) < 1e-10F) continue;
        const auto s = eye - a;
        const f32 u = dot(s, h) / det;
        if (u < 0 || u > 1) continue;
        const auto q = cross(s, ab);
        const f32 v = dot(ray, q) / det;
        if (v < 0 || u + v > 1) continue;
        const f32 t = dot(ac, q) / det;
        if (t > 1e-4F && t < nearest) { nearest = t; found = std::tuple{f, u, v}; }
    }
    if (!found) return {};
    const auto [f, u, v] = *found;
    const std::array corners{faces[f].vertices[0], faces[f].vertices[1], faces[f].vertices[2]};
    const std::array share{1 - u - v, u, v};
    // The same place on the bind-pose triangle, and the bone that moves it most.
    Hit hit{1, {}, bind[corners[0]] * share[0] + bind[corners[1]] * share[1] + bind[corners[2]] * share[2]};
    std::map<u32, f32> pull;
    for (std::size_t k = 0; k < 3; ++k)
        if (const auto influences = binding.weights().influences(corners[k]))
            for (const auto& influence : *influences)
                if (const auto index = character_.armature().index(influence.bone)) pull[*index] += influence.weight * share[k];
    if (const auto most = std::ranges::max_element(pull, {}, [](const auto& entry) { return entry.second; }); most != pull.end())
        hit.name = character_.armature().bones()[most->first].name;
    return hit;
}

const std::vector<Mat4>* CharacterView::shown_palette() const {
    if (!shown_) return nullptr;
    if (!palette_ || palette_revision_ != revision_) {
        auto pose = character_.armature().rest_pose();
        if (!posed(*shown_, pose)) return nullptr;
        auto palette = character_.binding().palette(pose);
        if (!palette) return nullptr;
        palette_ = std::move(*palette);
        palette_revision_ = revision_;
    }
    return &*palette_;
}

std::optional<Vec2> CharacterView::project(Vec3 bind_point) const {
    const auto* palette = shown_palette();
    if (!palette) return {};
    // The point moves as the bind-pose vertex nearest it does.
    const auto& mesh = character_.binding().mesh();
    std::size_t nearest{};
    f32 best = std::numeric_limits<f32>::max();
    for (std::size_t v = 0; v < mesh.vertex_count(); ++v) {
        const auto d = position(mesh, v) - bind_point;
        if (const auto distance = dot(d, d); distance < best) { best = distance; nearest = v; }
    }
    const auto influences = character_.binding().weights().influences(nearest);
    if (!influences) return {};
    Vec3 moved{};
    for (const auto& influence : *influences)
        if (const auto index = character_.armature().index(influence.bone); index && *index < palette->size())
            moved = moved + transform((*palette)[*index], bind_point) * influence.weight;
    const auto snapshot = camera_of(shown_->orbit).snapshot(shown_->extent);
    if (!snapshot) return {};
    std::array<double, 4> clip{};
    const std::array<double, 4> world{moved.x, moved.y, moved.z, 1};
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned column = 0; column < 4; ++column) clip[row] += double(snapshot->view_projection[column][row]) * world[column];
    if (clip[3] <= 1e-9) return {};
    return Vec2{static_cast<f32>((clip[0] / clip[3] + 1) * .5), static_cast<f32>((1 - clip[1] / clip[3]) * .5)};
}
} // namespace review
