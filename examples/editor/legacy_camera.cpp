#include "legacy_camera.hpp"
#include <map>

namespace editor_example {
using namespace vng;
namespace {
auto invalid(std::string message) {
    content::Diagnostic error;
    error.code = content::ErrorCode::invalid_document;
    error.message = std::move(message);
    return std::unexpected(std::move(error));
}
} // namespace

std::vector<AnimationProperty> legacy_camera_properties(const CameraPose& base) {
    return {{{legacy_camera_object, "yaw"}, "Orbit (deg)", "Camera", base.yaw, -180, 180},
            {{legacy_camera_object, "pitch"}, "Elevation (deg)", "Camera", base.pitch, -camera_max_pitch, camera_max_pitch},
            {{legacy_camera_object, "distance"}, "Distance", "Camera", base.distance, camera_min_distance, camera_max_distance},
            {{legacy_camera_object, "target"}, "Look-at target", "Camera", base.target, -camera_target_limit, camera_target_limit},
            {{legacy_camera_object, "zoom"}, "Optical zoom", "Camera", base.zoom, camera_min_zoom, camera_max_zoom}};
}

content::Result<void> migrate_legacy_camera(State& state, const LegacyCameraShot& shot) {
    if (has_camera(state)) return {};
    if (!valid_camera_pose(shot.pose)) return invalid("Invalid legacy animation camera");
    const auto selected = state.viewport.selected_object;
    const auto vertex = state.viewport.selected_vertex;
    auto created = instantiate(state, BlueprintId::camera);
    if (!created) return std::unexpected(created.error());
    state.viewport.selected_object = selected;
    state.viewport.selected_vertex = vertex;
    auto* camera = find_instance(state, *created);
    camera->name = "Animation camera";
    std::get<CameraSettings>(camera->settings).active = true;
    place_camera(*camera, shot.pose);
    if (shot.tracks.empty()) return {};

    timeline::Timeline legacy;
    if (auto replaced = legacy.replace(shot.tracks); !replaced) return invalid(replaced.error().message);
    std::map<f32, bool> times; // key time -> every legacy key there holds
    for (const auto& track : shot.tracks)
        for (const auto& key : track.keys) {
            const auto [entry, inserted] = times.try_emplace(key.time, true);
            entry->second = entry->second && key.incoming == timeline::Interpolation::hold;
        }
    std::array<timeline::Track, 4> tracks{{{{*created, "position"}, {}, {}, {}}, {{*created, "rotation"}, {}, {}, {}},
                                           {{*created, "zoom"}, {}, {}, {}}, {{*created, "focus"}, {}, {}, {}}}};
    for (const auto& [time, hold] : times) {
        auto pose = shot.pose;
        const auto take = [&](auto& field, std::string_view property) {
            if (const auto value = legacy.sample({legacy_camera_object, std::string(property)}, time))
                if (const auto* typed = std::get_if<std::remove_cvref_t<decltype(field)>>(&*value)) field = *typed;
        };
        take(pose.yaw, "yaw"); take(pose.pitch, "pitch"); take(pose.distance, "distance");
        take(pose.target, "target"); take(pose.zoom, "zoom");
        if (!valid_camera_pose(pose)) return invalid("Invalid legacy animation camera key");
        auto placed = *camera;
        place_camera(placed, pose);
        const auto& lens = std::get<CameraSettings>(placed.settings);
        const auto incoming = hold ? timeline::Interpolation::hold : timeline::Interpolation::linear;
        tracks[0].keys.push_back({time, placed.transform.position, incoming});
        tracks[1].keys.push_back({time, placed.transform.rotation, incoming});
        tracks[2].keys.push_back({time, lens.zoom, incoming});
        tracks[3].keys.push_back({time, lens.focus, incoming});
    }
    const auto properties = animation_properties(state);
    for (auto& track : tracks) {
        if (const auto property = std::ranges::find(properties, track.target, &AnimationProperty::target);
            property != properties.end()) {
            track.label = property->label;
            track.layer = property->layer;
        }
        if (auto replaced = state.document.timeline.replace_track(std::move(track)); !replaced)
            return invalid(replaced.error().message);
    }
    return {};
}
} // namespace editor_example
