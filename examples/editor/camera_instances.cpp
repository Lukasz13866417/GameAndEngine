#include "project.hpp"
#include "animation.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>

namespace editor_example {
using namespace vng;
namespace {
constexpr f32 radians = std::numbers::pi_v<f32> / 180;
// eye = target + distance * orbit(yaw, pitch); the same convention as camera().
Vec3 orbit(f32 yaw, f32 pitch) {
    return {std::sin(yaw * radians) * std::cos(pitch * radians), std::sin(pitch * radians),
            std::cos(yaw * radians) * std::cos(pitch * radians)};
}
// Instance pitch looks up when positive; an orbit pitch is the eye's
// elevation above its pivot, so the two are opposite in sign.
f32 orbit_yaw(Vec3 rotation) { return std::remainder(rotation.y, 360.F); }
f32 orbit_pitch(Vec3 rotation) { return std::clamp(-rotation.x, -camera_max_pitch, camera_max_pitch); }
f32 orbit_distance(f32 focus) { return std::clamp(focus, camera_min_distance, camera_max_distance); }
auto invalid(std::string message) {
    content::Diagnostic error;
    error.code = content::ErrorCode::invalid_document;
    error.message = std::move(message);
    return std::unexpected(std::move(error));
}
} // namespace

CameraSettings* camera_settings(State& state, u32 id) {
    auto* instance = find_instance(state, id);
    return instance ? std::get_if<CameraSettings>(&instance->settings) : nullptr;
}
const CameraSettings* camera_settings(const State& state, u32 id) {
    const auto* instance = find_instance(state, id);
    return instance ? std::get_if<CameraSettings>(&instance->settings) : nullptr;
}
bool is_camera_instance(const State& state, u32 id) {
    return camera_settings(state, id) != nullptr;
}
bool has_camera(const State& state) {
    return std::ranges::any_of(state.document.instances, [](const SceneInstance& instance) {
        return std::holds_alternative<CameraSettings>(instance.settings);
    });
}

CameraPose camera_pose(const SceneInstance& evaluated) {
    CameraPose pose;
    const auto* lens = std::get_if<CameraSettings>(&evaluated.settings);
    if (!lens) return pose;
    const auto& transform = evaluated.transform;
    pose.yaw = orbit_yaw(transform.rotation);
    pose.pitch = orbit_pitch(transform.rotation);
    pose.distance = orbit_distance(lens->focus);
    pose.zoom = std::clamp(lens->zoom, camera_min_zoom, camera_max_zoom);
    pose.target = focus_point(transform.position, transform.rotation, lens->focus);
    return pose;
}

void place_camera(SceneInstance& instance, const CameraPose& pose) {
    auto* lens = std::get_if<CameraSettings>(&instance.settings);
    if (!lens) return;
    instance.transform.rotation = {-pose.pitch, pose.yaw, 0};
    instance.transform.position = lens->orbit ? pose.target : orbit_eye(pose.target, instance.transform.rotation, pose.distance);
    lens->focus = pose.distance;
    lens->zoom = pose.zoom;
}

Vec3 focus_point(Vec3 position, Vec3 rotation, f32 focus) {
    const auto direction = orbit(orbit_yaw(rotation), orbit_pitch(rotation));
    Vec3 point;
    for (unsigned c = 0; c < 3; ++c) point[c] = position[c] - direction[c] * orbit_distance(focus);
    return point;
}
Vec3 orbit_eye(Vec3 point, Vec3 rotation, f32 focus) {
    const auto direction = orbit(orbit_yaw(rotation), orbit_pitch(rotation));
    Vec3 eye;
    for (unsigned c = 0; c < 3; ++c) eye[c] = point[c] + direction[c] * orbit_distance(focus);
    return eye;
}

content::Result<void> validate_active_cameras(const State& state) {
    std::set<f32> times{0.F};
    std::vector<const SceneInstance*> cameras;
    for (const auto& instance : state.document.instances) {
        if (!std::holds_alternative<CameraSettings>(instance.settings)) continue;
        cameras.push_back(&instance);
        if (const auto* track = state.document.timeline.find({instance.id, "active"}))
            for (const auto& key : track->keys) times.insert(key.time);
    }
    if (cameras.size() < 2) return {};
    for (const auto time : times) {
        unsigned active{};
        for (const auto* camera : cameras)
            active += std::get<CameraSettings>(evaluate_instance(state, *camera, time).settings).active;
        if (active > 1) return invalid("Only one camera can be active at a time");
    }
    return {};
}
} // namespace editor_example
