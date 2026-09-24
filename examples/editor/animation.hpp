#pragma once

#include "project.hpp"

namespace editor_example {
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
// The position is where the instance shows: for an orbiting camera, its eye,
// placed around the focus point it stores.
[[nodiscard]] InstanceTransform evaluate_transform(const State&, const SceneInstance&, vng::f32 time);
// The position value that shows an instance at `placed` at `time`: `placed`
// itself, or for an orbiting camera the focus point its eye there looks at.
[[nodiscard]] vng::Vec3 stored_position(const State&, const SceneInstance&, vng::Vec3 placed, vng::f32 time);
[[nodiscard]] bool evaluate_visibility(const State&, const SceneInstance&, vng::f32 time);
// The simulation camera: the pose of the active scene camera instance, or
// nothing when the scene has no camera. The editor's own view is never this.
[[nodiscard]] std::optional<CameraPose> evaluate_camera(const State&, vng::f32 time);
// The editor pose that looks through a camera at this time, or nothing when
// the camera's pivot (eye minus focus along its view) lies beyond the editor
// camera's range.
[[nodiscard]] std::optional<CameraPose> look_through(const State&, const SceneInstance& camera, vng::f32 time);
// The camera instance marked active at this time, else the first camera; null
// when the scene has none.
[[nodiscard]] const SceneInstance* active_camera(const State&, vng::f32 time);
// Scene authoring helpers: create the scene's first camera at a pose (returns
// the existing active camera untouched when one exists), and key one camera's
// placement and lens at a timestamp.
[[nodiscard]] vng::content::Result<vng::u32> ensure_camera(State&, const CameraPose&, std::string name = "Camera");
[[nodiscard]] vng::content::Result<void> key_camera(State&, vng::u32 camera, vng::f32 time, const CameraPose&,
    vng::timeline::Interpolation = vng::timeline::Interpolation::linear);
// Places a camera by its focus point (orbit) or by its eye. Its position
// values are rewritten, with position keys added where only its rotation or
// focus was keyed, so it stays where it is at time zero and at each of its
// keys; only the path between keys changes.
[[nodiscard]] vng::content::Result<void> set_camera_orbit(State&, vng::u32 camera, bool orbit);
// A camera position value at `time` re-expressed for the other placement: the
// focus point an eye there looks at (to orbit), or the eye around a focus point.
[[nodiscard]] vng::Vec3 switch_placement(const State&, const SceneInstance& camera, vng::Vec3 value,
                                         vng::f32 time, bool orbit);
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
