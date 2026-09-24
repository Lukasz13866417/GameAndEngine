#include "effects.hpp"
#include "scale_limits.hpp"
#include "keyframes.hpp"
#include "position_edits.hpp"
#include "blueprint_gizmos.hpp"

#include <algorithm>
#include <cmath>

namespace editor_example {
namespace {
using namespace vng;
auto invalid(std::string message) { return std::unexpected(editor::Diagnostic{std::move(message)}); }

// One small instance and its timeline are staged, never a mesh/scene copy.
// Compare against the values the user actually saw, not raw authored defaults.
class PropertyTransaction {
public:
    PropertyTransaction(State& state, const SceneInstance& source)
        : instance(source), timeline(state.document.timeline), state_(state), properties_(animation_properties(state)) {}

    template<class T>
    editor::Result<void> change(std::string_view property, const T& before, const T& after, T& base) {
        if (before == after) return {};
        if (!at_paused_keyframe(state_)) return invalid("Insert a keyframe here to edit scene properties");
        const timeline::Target target{instance.id, std::string(property)};
        const auto description = std::ranges::find(properties_, target, &AnimationProperty::target);
        if (description == properties_.end()) return invalid("Unknown instance property: " + std::string(property));
        const auto low = description->minimum.value_or(-1000000), high = description->maximum.value_or(1000000);
        const auto scalar = [&](f32 value) { return std::isfinite(value) && value >= low && value <= high; };
        bool valid = true;
        if constexpr (std::same_as<T, f32>) valid = scalar(after);
        else if constexpr (std::same_as<T, Vec3>) valid = scalar(after.x) && scalar(after.y) && scalar(after.z);
        if (!valid) return invalid("Instance property exceeds editor limits: " + std::string(property));
        if (const auto* track = timeline.find(target); track || state_.viewport.time > 0) {
            if (state_.viewport.mode != ViewMode::scene)
                return invalid("Switch to Scene view before editing an animated instance property");
            if (!state_.viewport.paused)
                return invalid("Pause scene playback before editing an animated instance property");
            if (!std::isfinite(state_.viewport.time) || state_.viewport.time < 0 || state_.viewport.time > state_.document.timeline_duration)
                return invalid("Animated instance edit requires a playhead inside the timeline");
            auto incoming = std::same_as<T, bool> ? timeline::Interpolation::hold : timeline::Interpolation::linear;
            if (track) {
                const auto key = std::ranges::lower_bound(track->keys, state_.viewport.time, {}, &timeline::Keyframe::time);
                if (key != track->keys.end() && key->time == state_.viewport.time) incoming = key->incoming;
                if (state_.viewport.time == 0 && track->keys.front().time > 0) {
                    auto first = track->keys.front(); first.incoming = timeline::Interpolation::hold;
                    if (auto held = timeline.set(target, first); !held) return invalid(held.error().message);
                }
            } else if (auto seeded = timeline.set(target, {0, base, timeline::Interpolation::hold},
                                                 description->label, description->layer); !seeded)
                return invalid(seeded.error().message);
            if (auto keyed = timeline.set(target, {state_.viewport.time, after, incoming}); !keyed)
                return invalid(keyed.error().message);
        } else base = after;
        changes.properties.insert(target);
        return {};
    }

    SceneInstance instance;
    timeline::Timeline timeline;
    DocumentChanges changes;
private:
    State& state_;
    std::vector<AnimationProperty> properties_;
};

template<class Function>
editor::Result<void> apply(State& state, DocumentChanges& changes, u32 id, Function function) {
    auto* instance = find_instance(state, id);
    if (!instance) return invalid("Edited scene instance no longer exists");
    PropertyTransaction candidate{state, *instance};
    if (auto changed = function(candidate); !changed) return changed;
    changes.merge(candidate.changes);
    *instance = std::move(candidate.instance);
    state.document.timeline = std::move(candidate.timeline);
    return {};
}

editor::Result<void> apply_transform(State& state, DocumentChanges& changes, u32 id, const InstanceTransform& before, const InstanceTransform& next) {
    return apply(state, changes, id, [&](auto& candidate) -> editor::Result<void> {
        if (auto changed = candidate.change("scale", before.scale, next.scale, candidate.instance.transform.scale); !changed) return changed;
        if (auto changed = candidate.change("axis_scale", before.axis_scale, next.axis_scale, candidate.instance.transform.axis_scale); !changed) return changed;
        return candidate.change("rotation", before.rotation, next.rotation, candidate.instance.transform.rotation);
    });
}
editor::Result<void> apply_appearance(State& state, DocumentChanges& changes, u32 id, const MeshSettings& before, const MeshSettings& next) {
    return apply(state, changes, id, [&](auto& candidate) -> editor::Result<void> {
        auto& value = std::get<MeshSettings>(candidate.instance.settings);
        if (auto changed = candidate.change("brightness", before.brightness, next.brightness, value.brightness); !changed) return changed;
        if (auto changed = candidate.change("visible", before.visible, next.visible, value.visible); !changed) return changed;
        return candidate.change("wireframe", before.wireframe, next.wireframe, value.wireframe);
    });
}
editor::Result<void> apply_appearance(State& state, DocumentChanges& changes, u32 id, const SunSettings& before, const SunSettings& next) {
    return apply(state, changes, id, [&](auto& candidate) -> editor::Result<void> {
        auto& value = std::get<SunSettings>(candidate.instance.settings);
        if (auto changed = candidate.change("radius", before.radius, next.radius, value.radius); !changed) return changed;
        if (auto changed = candidate.change("displacement", before.displacement, next.displacement, value.displacement); !changed) return changed;
        if (auto changed = candidate.change("bloom", before.bloom, next.bloom, value.bloom); !changed) return changed;
        if (auto changed = candidate.change("white_spots", before.white_spots, next.white_spots, value.white_spots); !changed) return changed;
        return candidate.change("visible", before.visible, next.visible, value.visible);
    });
}
} // namespace

editor::Result<void> apply_lens(State& state, DocumentChanges& changes, u32 id, const CameraSettings& before, const CameraSettings& next) {
    const auto* source = find_instance(state, id);
    if (!source) return invalid("Edited scene instance no longer exists");
    // Kept to undo the lens edits if the placement cannot switch.
    const auto instance = *source;
    const auto timeline = state.document.timeline;
    const auto published = changes;
    if (auto lens = apply(state, changes, id, [&](auto& candidate) -> editor::Result<void> {
            auto* value = std::get_if<CameraSettings>(&candidate.instance.settings);
            if (!value) return invalid("Edited scene instance is not a camera");
            if (auto changed = candidate.change("zoom", before.zoom, next.zoom, value->zoom); !changed) return changed;
            if (auto changed = candidate.change("focus", before.focus, next.focus, value->focus); !changed) return changed;
            return candidate.change("visible", before.visible, next.visible, value->visible);
        }); !lens) return lens;
    // Placement is not animated: switching it keeps the camera where it is at its keys.
    if (before.orbit == next.orbit) return {};
    if (auto placed = set_camera_orbit(state, id, next.orbit); !placed) {
        *find_instance(state, id) = instance;
        state.document.timeline = timeline;
        changes = published;
        return invalid(placed.error().message);
    }
    changes.properties.insert({id, "position"});
    changes.properties.insert({id, "orbit"});
    return {};
}
editor::Result<void> apply_active_camera(State& state, DocumentChanges& changes, u32 id) {
    if (!is_camera_instance(state, id)) return invalid("Only a scene camera can be made active");
    std::vector<u32> cameras;
    for (const auto& instance : state.document.instances)
        if (std::holds_alternative<CameraSettings>(instance.settings)) cameras.push_back(instance.id);
    for (const auto camera : cameras) {
        const auto* instance = find_instance(state, camera);
        const bool active = std::get<CameraSettings>(evaluate_instance(state, *instance, state.viewport.time).settings).active;
        const bool wanted = camera == id;
        if (active == wanted) continue;
        if (auto changed = apply(state, changes, camera, [&](auto& candidate) -> editor::Result<void> {
                auto& value = std::get<CameraSettings>(candidate.instance.settings);
                return candidate.change("active", active, wanted, value.active);
            }); !changed) return changed;
    }
    return {};
}

void ProjectControls::describe_editor(vng::editor::Inspector& ui) {
    const auto id = state_.viewport.selected_object;
    const auto* source = find_instance(state_, id);
    // A blueprint inspection does not edit an instance's placement or timeline.
    if (!source || state_.viewport.mode == ViewMode::mesh) return;
    const auto initial = evaluate_instance(state_, *source, state_.viewport.time);
    if (state_.viewport.mode == ViewMode::scene) {
    auto transform = ui.edit("transform", initial.transform, "Instance transform");
    // Keep the quick slider useful at ordinary sizes, but never clamp an
    // authored transform simply because it exceeds that convenient range.
    transform.slider("scale", &InstanceTransform::scale, min_instance_scale,
        std::max(3.F,initial.transform.scale));
    transform.field("axis_scale", &InstanceTransform::axis_scale, "Axis scale (local)");
    transform.field("rotation", &InstanceTransform::rotation, "Rotation (degrees)");
    transform.apply("Apply instance transform", [this, id, before = initial.transform](const InstanceTransform& next) mutable {
        auto result = apply_transform(state_, changes_, id, before, next);
        if (result) before = next;
        return result;
    });
    auto movement = ui.translation_gizmo("position", initial.transform.position,
        [this, id, original = std::optional<PositionSnapshot>{}](vng::Vec3 next, vng::editor::Phase phase) mutable -> vng::editor::Result<void> {
            if (phase == vng::editor::Phase::begin) {
                auto captured = capture_position(state_, id);
                if (!captured) return invalid(captured.error().message);
                original = std::move(*captured);
                return {};
            }
            if (phase == vng::editor::Phase::cancel) {
                if (!original) return invalid("Position gesture has no captured starting value");
                auto restored = restore_position(state_, *original);
                if (!restored) return invalid(restored.error().message);
                changes_.properties.insert({id, "position"});
                original.reset();
                return {};
            }
            auto moved = apply_placed_position(state_, id, next);
            if (!moved) return invalid(moved.error().message);
            if (*moved) changes_.properties.insert({id, "position"});
            if (phase == vng::editor::Phase::commit) original.reset();
            return {};
        });
    for (const auto& axis : blueprint_translation_axes(state_, initial))
        movement.axis(axis.label, axis.direction);
    }
    if (const auto* sun = std::get_if<SunSettings>(&initial.settings)) {
        auto edit = ui.edit("surface", *sun);
        edit.slider("radius", &SunSettings::radius, .1F, 4);
        edit.slider("displacement", &SunSettings::displacement, 0, 1);
        edit.slider("bloom", &SunSettings::bloom, 0, 1);
        edit.toggle("white_spots", &SunSettings::white_spots);
        edit.toggle("visible", &SunSettings::visible);
        edit.apply("Apply surface", [this, id, before = *sun](const SunSettings& next) mutable {
            auto result = apply_appearance(state_, changes_, id, before, next);
            if (result) before = next;
            return result;
        });
        ui.action("reset_surface", [this, id] {
            const auto* instance = find_instance(state_, id);
            if (!instance) return vng::editor::Result<void>{invalid("Edited scene instance no longer exists")};
            const auto current = std::get<SunSettings>(evaluate_instance(state_, *instance, state_.viewport.time).settings);
            return apply_appearance(state_, changes_, id, current, *effect_blueprint_settings(state_,instance->blueprint));
        });
    } else if (const auto* lens = std::get_if<CameraSettings>(&initial.settings)) {
        auto edit = ui.edit("lens", *lens, "Camera lens");
        edit.slider("zoom", &CameraSettings::zoom, camera_min_zoom, std::max(10.F, lens->zoom), "Optical zoom");
        edit.slider("focus", &CameraSettings::focus, camera_min_distance, std::max(100.F, lens->focus), "Focus distance");
        edit.toggle("orbit", &CameraSettings::orbit, "Orbit focus point between keys");
        edit.toggle("visible", &CameraSettings::visible, "Show frustum in preview");
        edit.apply("Apply lens", [this, id, before = *lens](const CameraSettings& next) mutable {
            auto result = apply_lens(state_, changes_, id, before, next);
            if (result) before = next;
            return result;
        });
    } else if (const auto* settings = std::get_if<MeshSettings>(&initial.settings)) {
        const auto model = *settings;
        auto edit = ui.edit("model", model, "Appearance");
        edit.slider("brightness", &MeshSettings::brightness, 0, 5);
        edit.toggle("wireframe", &MeshSettings::wireframe);
        edit.toggle("visible", &MeshSettings::visible);
        edit.apply("Apply appearance", [this, id, before = model](const MeshSettings& next) mutable {
            auto result = apply_appearance(state_, changes_, id, before, next);
            if (result) before = next;
            return result;
        });
    }
}
} // namespace editor_example
