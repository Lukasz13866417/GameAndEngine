#include "animation.hpp"
#include "scale_limits.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace editor_example {
namespace {
using namespace vng;
auto invalid(std::string message) {
    content::Diagnostic error;
    error.code = content::ErrorCode::invalid_document;
    error.message = std::move(message);
    return std::unexpected(std::move(error));
}
bool in_range(f32 value, f32 minimum, f32 maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}
auto identity(const timeline::Target& target) {
    return std::pair{target.object, std::string_view{target.property}};
}
using PropertyLookup = std::map<std::pair<u64, std::string_view>, const AnimationProperty*>;
bool valid_keyframe_name(std::string_view name) {
    if (name.size() > 256)
        return false;
    for (std::size_t i = 0; i < name.size();) {
        const auto first = static_cast<unsigned char>(name[i++]);
        if (first < 0x80) {
            if (first < 32 || first == 127)
                return false;
            continue;
        }
        unsigned count{}, code{}, minimum{};
        if (first >= 0xc2 && first <= 0xdf) {
            count = 1;
            code = first & 0x1fU;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            count = 2;
            code = first & 0xfU;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            count = 3;
            code = first & 7U;
            minimum = 0x10000;
        } else
            return false;
        if (count > name.size() - i)
            return false;
        while (count--) {
            const auto byte = static_cast<unsigned char>(name[i++]);
            if ((byte & 0xc0U) != 0x80U)
                return false;
            code = (code << 6U) | (byte & 0x3fU);
        }
        if (code < minimum || code <= 0x9f || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
            return false;
    }
    return true;
}
content::Result<void> validate_value(const AnimationProperty& property,
                                     const timeline::Value& value) {
    if (property.base_value.index() != value.index())
        return invalid("Animation key type does not match property '" + property.target.property +
                       "'");
    const auto minimum = property.minimum.value_or(-1000000),
               maximum = property.maximum.value_or(1000000);
    const bool valid = std::visit(
        [&](const auto& entry) {
            using T = std::remove_cvref_t<decltype(entry)>;
            if constexpr (std::same_as<T, f32>)
                return in_range(entry, minimum, maximum);
            else if constexpr (std::same_as<T, Vec3>)
                return in_range(entry.x, minimum, maximum) && in_range(entry.y, minimum, maximum) &&
                       in_range(entry.z, minimum, maximum);
            else
                return true;
        },
        value);
    if (!valid)
        return invalid("Animation key exceeds editor limits for '" + property.target.property +
                       "'");
    return {};
}
template <class T>
void sample(T& value, const timeline::Timeline& timeline, u64 object, std::string property,
            f32 time) {
    if (const auto result = timeline.sample({object, std::move(property)}, time))
        if (const auto* typed = std::get_if<T>(&*result))
            value = *typed;
}
} // namespace

std::vector<AnimationProperty> animation_properties(const State& state) {
    std::vector<AnimationProperty> properties;
    for (const auto& instance : state.document.instances) {
        const auto id = instance.id;
        const auto& name = instance.name;
        properties.insert(properties.end(), {
            {{id, "position"}, "Position", name, instance.transform.position, -scene_coordinate_limit, scene_coordinate_limit},
            {{id, "rotation"}, "Rotation (deg)", name, instance.transform.rotation, -360, 360},
            {{id, "scale"}, "Scale", name, instance.transform.scale, min_instance_scale, max_instance_scale},
            {{id, "axis_scale"}, "Axis scale", name, instance.transform.axis_scale, min_axis_scale, max_axis_scale}});
        if (const auto* value = std::get_if<MeshSettings>(&instance.settings)) {
            properties.insert(properties.end(), {
                {{id, "brightness"}, "Brightness", name, value->brightness, 0, 5},
                {{id, "visible"}, "Visible", name, value->visible, {}, {}},
                {{id, "wireframe"}, "Wireframe", name, value->wireframe, {}, {}}});
        } else if (const auto* settings = std::get_if<SunSettings>(&instance.settings)) {
            const auto& sun = *settings;
            properties.insert(properties.end(), {
                {{id, "radius"}, "Radius", name, sun.radius, .1F, 4},
                {{id, "displacement"}, "Displacement", name, sun.displacement, 0, 1},
                {{id, "bloom"}, "Bloom", name, sun.bloom, 0, 1},
                {{id, "white_spots"}, "White spots", name, sun.white_spots, {}, {}},
                {{id, "visible"}, "Visible", name, sun.visible, {}, {}}});
        } else if (const auto* lens = std::get_if<CameraSettings>(&instance.settings)) {
            properties.insert(properties.end(), {
                {{id, "zoom"}, "Optical zoom", name, lens->zoom, camera_min_zoom, camera_max_zoom},
                {{id, "focus"}, "Focus distance", name, lens->focus, camera_min_distance, camera_max_distance},
                {{id, "active"}, "Active camera", name, lens->active, {}, {}},
                {{id, "visible"}, "Visible", name, lens->visible, {}, {}}});
        } else if (const auto* region = std::get_if<RegionSettings>(&instance.settings)) {
            properties.push_back({{id, "visible"}, "Visible", name, region->visible, {}, {}});
        }
    }
    for (auto& property : properties) {
        if (const auto* track = state.document.timeline.find(property.target)) {
            if (!track->label.empty())
                property.label = track->label;
            if (!track->layer.empty())
                property.layer = track->layer;
        }
    }
    return properties;
}

SceneValues evaluate_scene(const State& state, f32 time) {
    SceneValues result{};
    result.model.visible = result.sun.visible = false;
    result.instances.reserve(state.document.instances.size());
    for (const auto& instance : state.document.instances) {
        result.instances.push_back(evaluate_instance(state, instance, time));
        const auto& value = result.instances.back();
        if (value.id == 1 && std::holds_alternative<MeshSettings>(value.settings)) {
            result.model = std::get<MeshSettings>(value.settings);
            result.model_transform = value.transform;
        }
        if (value.id == 2 && std::holds_alternative<SunSettings>(value.settings)) {
            result.sun = std::get<SunSettings>(value.settings);
            result.sun_transform = value.transform;
        }
    }
    return result;
}

const SceneInstance* active_camera(const State& state, f32 time) {
    const SceneInstance* first{};
    for (const auto& instance : state.document.instances) {
        const auto* lens = std::get_if<CameraSettings>(&instance.settings);
        if (!lens) continue;
        if (!first) first = &instance;
        auto active = lens->active;
        sample(active, state.document.timeline, instance.id, "active", time);
        if (active) return &instance;
    }
    return first;
}

std::optional<CameraPose> look_through(const State& state, const SceneInstance& camera, f32 time) {
    const auto pose = camera_pose(evaluate_instance(state, camera, time));
    if (!valid_camera_pose(pose)) return {};
    return pose;
}

std::optional<CameraPose> evaluate_camera(const State& state, f32 time) {
    const auto* camera = active_camera(state, time);
    if (!camera) return {};
    return camera_pose(evaluate_instance(state, *camera, time));
}

content::Result<u32> ensure_camera(State& state, const CameraPose& pose, std::string name) {
    if (const auto* existing = active_camera(state, 0)) return existing->id;
    auto created = instantiate(state, BlueprintId::camera);
    if (!created) return created;
    auto* camera = find_instance(state, *created);
    camera->name = std::move(name);
    std::get<CameraSettings>(camera->settings).active = true;
    place_camera(*camera, pose);
    return *created;
}

content::Result<void> key_camera(State& state, u32 id, f32 time, const CameraPose& pose, timeline::Interpolation mode) {
    const auto* camera = find_instance(state, id);
    if (!camera || !std::holds_alternative<CameraSettings>(camera->settings)) {
        content::Diagnostic error;
        error.message = "Camera keys require a scene camera instance";
        return std::unexpected(std::move(error));
    }
    auto placed = *camera;
    place_camera(placed, pose);
    const auto& lens = std::get<CameraSettings>(placed.settings);
    std::vector<PropertyKey> keys;
    for (const auto& [property, value] : std::array<std::pair<std::string_view, timeline::Value>, 4>{
             {{"position", placed.transform.position}, {"rotation", placed.transform.rotation},
              {"zoom", lens.zoom}, {"focus", lens.focus}}}) {
        const timeline::Target target{id, std::string(property)};
        auto incoming = mode;
        if (const auto* track = state.document.timeline.find(target))
            if (const auto key = std::ranges::find(track->keys, time, &timeline::Keyframe::time);
                key != track->keys.end()) incoming = key->incoming;
        keys.push_back({target, value, incoming, true});
    }
    return edit_property_keys(state, time, keys);
}

InstanceTransform evaluate_transform(const State& state, const SceneInstance& source, f32 time) {
    auto result = source.transform;
    sample(result.position, state.document.timeline, source.id, "position", time);
    sample(result.rotation, state.document.timeline, source.id, "rotation", time);
    sample(result.scale, state.document.timeline, source.id, "scale", time);
    sample(result.axis_scale, state.document.timeline, source.id, "axis_scale", time);
    return result;
}
bool evaluate_visibility(const State& state, const SceneInstance& source, f32 time) {
    auto result = std::visit([](const auto& value) { return value.visible; }, source.settings);
    sample(result, state.document.timeline, source.id, "visible", time);
    return result;
}
SceneInstance evaluate_instance(const State& state, const SceneInstance& source, f32 time) {
    auto result = source;
    result.transform = evaluate_transform(state, source, time);
    std::visit([&](auto& value) {
        sample(value.visible, state.document.timeline, source.id, "visible", time);
        if constexpr (std::same_as<std::remove_cvref_t<decltype(value)>, MeshSettings>) {
            sample(value.brightness, state.document.timeline, source.id, "brightness", time);
            sample(value.wireframe, state.document.timeline, source.id, "wireframe", time);
        } else if constexpr (std::same_as<std::remove_cvref_t<decltype(value)>, SunSettings>) {
            sample(value.radius, state.document.timeline, source.id, "radius", time);
            sample(value.displacement, state.document.timeline, source.id, "displacement", time);
            sample(value.bloom, state.document.timeline, source.id, "bloom", time);
            sample(value.white_spots, state.document.timeline, source.id, "white_spots", time);
        } else if constexpr (std::same_as<std::remove_cvref_t<decltype(value)>, CameraSettings>) {
            sample(value.zoom, state.document.timeline, source.id, "zoom", time);
            sample(value.focus, state.document.timeline, source.id, "focus", time);
            sample(value.active, state.document.timeline, source.id, "active", time);
        }
    }, result.settings);
    return result;
}

content::Result<void> validate_animation(const State& state) {
    return validate_animation(animation_properties(state), state.document.timeline,
                              state.document.timeline_duration, state.document.keyframe_names);
}
content::Result<void> validate_property_value(const AnimationProperty& property, const timeline::Value& value) {
    return validate_value(property, value);
}
content::Result<void> validate_animation(std::span<const AnimationProperty> properties,
                                        const timeline::Timeline& animation, f32 duration,
                                        const std::map<f32, std::string>& names) {
    if (!in_range(duration, .1F, 86400))
        return invalid("Timeline duration must be in [0.1, 86400] seconds");
    if (names.size() > timeline::max_total_keys)
        return invalid("Too many named keyframes");
    for (const auto& [time, name] : names) {
        if (!in_range(time, 0, duration))
            return invalid("Named keyframe time must lie within the timeline duration");
        if (!valid_keyframe_name(name))
            return invalid(
                "Keyframe name must be valid UTF-8, at most 256 bytes, with no control characters");
    }
    PropertyLookup lookup;
    for (const auto& property : properties) lookup.emplace(identity(property.target), &property);
    for (const auto& track : animation.tracks()) {
        const auto property = lookup.find(identity(track.target));
        if (property == lookup.end())
            return invalid("Unknown animated scene property '" + track.target.property +
                           "' on object " + std::to_string(track.target.object));
        for (const auto& key : track.keys) {
            if (!in_range(key.time, 0, duration))
                return invalid("Animation key time must lie within the timeline duration");
            if (auto valid = validate_value(*property->second, key.value); !valid)
                return valid;
        }
    }
    return {};
}

content::Result<void> key_property(State& state, const timeline::Target& target, f32 time,
                                   timeline::Value value, timeline::Interpolation interpolation) {
    const PropertyKey key{target, std::move(value), interpolation, true};
    return edit_property_keys(state, time, std::span{&key, 1});
}
content::Result<void> edit_property_keys(State& state, f32 time, std::span<const PropertyKey> keys) {
    if (!in_range(time, 0, state.document.timeline_duration))
        return invalid("Animation key time must lie within the timeline duration");
    const auto properties = animation_properties(state);
    if (auto valid = validate_animation(properties, state.document.timeline,
            state.document.timeline_duration, state.document.keyframe_names); !valid)
        return valid;
    PropertyLookup lookup;
    for (const auto& property : properties) lookup.emplace(identity(property.target), &property);
    std::set<std::pair<u64, std::string_view>> seen;
    auto edited = state.document.timeline;
    for (const auto& key : keys) {
        const auto& target = key.target;
        if (!seen.insert(identity(target)).second) return invalid("Duplicate keyframe property");
        const auto found = lookup.find(identity(target));
        if (found == lookup.end()) return invalid("Unknown animated scene property '" + target.property + "'");
        const auto* property = found->second;
        if (!key.keyed) {
            if (time == 0) return invalid("The initial pose cannot be unkeyed");
            (void)edited.erase(target, time);
            continue;
        }
        if (auto valid = validate_value(*property, key.value); !valid) return valid;
        auto interpolation = key.incoming;
        if (interpolation != timeline::Interpolation::hold && interpolation != timeline::Interpolation::linear)
            return invalid("Unknown key interpolation");
        if (std::holds_alternative<bool>(key.value)) interpolation = timeline::Interpolation::hold;
        const auto* existing = edited.find(target);
        const auto label = existing ? existing->label : property->label;
        const auto layer = existing ? existing->layer : property->layer;
        if (time == 0 && existing && existing->keys.front().time > 0) {
            // Before its first explicit key a sparse track used the initial
            // pose, not interpolation. Materializing zero must retain that.
            auto first = existing->keys.front();
            first.incoming = timeline::Interpolation::hold;
            if (auto held = edited.set(target, first); !held) return invalid(held.error().message);
        }
        if (!existing && time > 0) {
            if (auto baseline = validate_value(*property, property->base_value); !baseline)
                return baseline;
            if (auto seeded = edited.set(
                    target, {0, property->base_value, timeline::Interpolation::hold}, label, layer);
                !seeded)
                return invalid(seeded.error().message);
        }
        if (auto keyed = edited.set(target, {time, key.value, interpolation}, label, layer); !keyed)
            return invalid(keyed.error().message);
    }
    state.document.timeline = std::move(edited);
    return {};
}
} // namespace editor_example
