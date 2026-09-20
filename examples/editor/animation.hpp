#pragma once

#include "project.hpp"

namespace editor_example {
// Scene instances use u32 identities. Keep the camera in a separate, nonzero
// timeline identity domain (zero is the UI's "all objects" selection).
inline constexpr vng::u64 camera_animation_object = vng::u64{1} << 32;
inline constexpr std::array<std::string_view, 5> camera_track_properties{"yaw", "pitch", "distance", "target", "zoom"};
// The example's bridge maps stable scene properties onto the neutral timeline.
// base_value is authored state, not the current animation sample. UI layers are
// organizational labels; they do not change evaluation order or dependencies.
struct AnimationProperty {
    vng::timeline::Target target;
    std::string label, layer;
    vng::timeline::Value base_value;
    std::optional<vng::f32> minimum, maximum;
};
struct PropertyKey {
    vng::timeline::Target target;
    vng::timeline::Value value;
    vng::timeline::Interpolation incoming{vng::timeline::Interpolation::hold};
    bool keyed{};
};
// One validated transaction for any number of property keys. Unkeyed entries
// remove the key at this timestamp. No geometry copies or per-property rebuilds.
[[nodiscard]] vng::content::Result<void> edit_property_keys(
    State&, vng::f32 time, std::span<const PropertyKey>);
struct SceneValues {
    MeshSettings model;
    SunSettings sun;
    std::vector<SceneInstance> instances;
    InstanceTransform model_transform, sun_transform;
    friend bool operator==(const SceneValues&, const SceneValues&) = default;
};

[[nodiscard]] std::vector<AnimationProperty> animation_properties(const State&);
// Sampling does not copy the mesh, mutate authored values, or change revision.
[[nodiscard]] SceneValues evaluate_scene(const State&, vng::f32 time);
[[nodiscard]] SceneInstance evaluate_instance(const State&, const SceneInstance&, vng::f32 time);
// Placement queries must not copy an instance's editable boundary or settings.
[[nodiscard]] InstanceTransform evaluate_transform(const State&, const SceneInstance&, vng::f32 time);
[[nodiscard]] bool evaluate_visibility(const State&, const SceneInstance&, vng::f32 time);
[[nodiscard]] CameraPose evaluate_camera(const State&, vng::f32 time);
[[nodiscard]] bool has_camera_animation(const State&);
// Private editor navigation is never sampled from authored camera tracks.
[[nodiscard]] CameraPose preview_camera_pose(const State&, vng::f32 time);
// Updates the entire shot at one timestamp, transactionally, without copying meshes.
[[nodiscard]] vng::content::Result<void> key_camera(State&, vng::f32 time, const CameraPose&);
[[nodiscard]] vng::content::Result<void> validate_animation(const State&);
[[nodiscard]] vng::content::Result<void> validate_animation(
    std::span<const AnimationProperty>, const vng::timeline::Timeline&, vng::f32 duration,
    const std::map<vng::f32, std::string>& names);
[[nodiscard]] vng::content::Result<void> validate_property_value(
    const AnimationProperty&, const vng::timeline::Value&);
// Adds/replaces one key atomically; the first key after zero seeds a baseline
// key at zero so an edit at t=5 does not affect the whole earlier scene.
// History, dirty state and revision are deliberately left to the caller.
[[nodiscard]] vng::content::Result<void>
key_property(State&, const vng::timeline::Target&, vng::f32 time, vng::timeline::Value,
             vng::timeline::Interpolation = vng::timeline::Interpolation::linear);
} // namespace editor_example
