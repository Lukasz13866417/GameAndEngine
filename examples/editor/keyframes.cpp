#include "keyframes.hpp"
#include <algorithm>
#include <cmath>
#include <set>

namespace editor_example {
namespace {
using namespace vng;
auto invalid(std::string message) {
    content::Diagnostic diagnostic;
    diagnostic.message = std::move(message);
    return std::unexpected(std::move(diagnostic));
}
bool valid_time(const State& state, f32 time) {
    return std::isfinite(time) && time >= 0 && time <= state.document.timeline_duration;
}
bool exists(const timeline::Timeline& timeline, f32 time) {
    return std::ranges::any_of(timeline.tracks(), [&](const auto& track) {
        return std::ranges::find(track.keys, time, &timeline::Keyframe::time) != track.keys.end();
    });
}
bool exists(const State& state, f32 time) {
    return time == 0 || state.document.keyframe_names.contains(time) || exists(state.document.timeline, time);
}
} // namespace
bool is_keyframe(const State& state, f32 time) { return valid_time(state, time) && exists(state, time); }
bool at_paused_keyframe(const State& state) {
    return state.viewport.paused && is_keyframe(state, state.viewport.time);
}
std::optional<KeyframeChange> keyframe_change(const KeyframeValue& before, const KeyframeValue& after) {
    KeyframeChange change{.target = after.target};
    if (before.value != after.value) {
        change.value = after.value;
        if (const auto* value = std::get_if<Vec3>(&before.value))
            if (const auto* next = std::get_if<Vec3>(&after.value))
                for (std::size_t i = 0; i < 3; ++i) change.components[i] = (*value)[i] != (*next)[i];
    }
    if (before.keyed != after.keyed) change.keyed = after.keyed;
    if (before.incoming != after.incoming) change.incoming = after.incoming;
    if (!change.value && !change.keyed && !change.incoming) return {};
    return change;
}
content::Result<void> apply_keyframe_range(State& state, f32 first, f32 last,
                                          std::span<const KeyframeChange> changes) {
    if (!valid_time(state, first) || !valid_time(state, last) || first > last)
        return invalid("Range must be ordered and inside the timeline duration");
    auto times = keyframe_times(state);
    std::erase_if(times, [&](f32 time) { return time < first || time > last; });
    if (times.empty()) return invalid("There are no keyframes in this range");
    if (changes.empty()) return invalid("Edit a keyframe property before applying to a range");
    const auto properties = animation_properties(state);
    if (auto valid = validate_animation(properties, state.document.timeline,
            state.document.timeline_duration, state.document.keyframe_names); !valid) return valid;
    const auto& original = state.document.timeline;
    std::vector<timeline::Track> tracks(original.tracks().begin(), original.tracks().end());
    std::set<std::pair<u64, std::string>> seen;
    for (const auto& change : changes) {
        if (!change.value && !change.keyed && !change.incoming)
            return invalid("Empty keyframe property change");
        if (change.keyed == false && times.front() == 0)
            return invalid("The initial pose cannot be unkeyed; start the range after zero");
        if (!seen.emplace(change.target.object, change.target.property).second)
            return invalid("Duplicate keyframe property change");
        const auto property = std::ranges::find(properties, change.target, &AnimationProperty::target);
        if (property == properties.end()) return invalid("Unknown animated property '" + change.target.property + "'");
        if (change.value && change.value->index() != property->base_value.index())
            return invalid("Keyframe property type mismatch");
        auto track = std::ranges::find(tracks, change.target, &timeline::Track::target);
        if (track == tracks.end()) {
            if (change.keyed == false) continue;
            tracks.push_back({.target = change.target, .label = property->label, .layer = property->layer, .keys = {}});
            track = std::prev(tracks.end());
            if (first > 0) track->keys.push_back({0, property->base_value, timeline::Interpolation::hold});
        }
        for (auto time : times) {
            auto at = std::ranges::lower_bound(track->keys, time, {}, &timeline::Keyframe::time);
            const bool existing = at != track->keys.end() && at->time == time;
            if (change.keyed == false) {
                if (existing) track->keys.erase(at);
                continue;
            }
            auto value = original.sample(change.target, time).value_or(property->base_value);
            if (change.value) {
                if (auto* vector = std::get_if<Vec3>(&value)) {
                    const auto& next = std::get<Vec3>(*change.value);
                    for (std::size_t i = 0; i < 3; ++i) if (change.components[i]) (*vector)[i] = next[i];
                } else value = *change.value;
            }
            if (auto valid = validate_property_value(*property, value); !valid) return valid;
            auto incoming = change.incoming.value_or(existing ? at->incoming :
                std::holds_alternative<bool>(value) ? timeline::Interpolation::hold : timeline::Interpolation::linear);
            if (std::holds_alternative<bool>(value)) incoming = timeline::Interpolation::hold;
            if (time == 0 && !existing && at != track->keys.end()) at->incoming = timeline::Interpolation::hold;
            if (existing) *at = {time, value, incoming};
            else track->keys.insert(at, {time, value, incoming});
        }
    }
    std::erase_if(tracks, [](const auto& track) { return track.keys.empty(); });
    auto animation = original;
    if (auto replaced = animation.replace(std::move(tracks)); !replaced) return invalid(replaced.error().message);
    auto names = state.document.keyframe_names;
    for (auto time : times) names.try_emplace(time);
    if (auto valid = validate_animation(properties, animation, state.document.timeline_duration, names); !valid) return valid;
    state.document.timeline = std::move(animation);
    state.document.keyframe_names = std::move(names);
    return {};
}
std::vector<f32> keyframe_times(const timeline::Timeline& timeline) {
    std::vector<f32> times;
    for (const auto& track : timeline.tracks())
        for (const auto& key : track.keys)
            times.push_back(key.time);
    std::ranges::sort(times);
    times.erase(std::unique(times.begin(), times.end()), times.end());
    return times;
}
std::vector<f32> keyframe_times(const State& state) {
    auto times = keyframe_times(state.document.timeline);
    times.push_back(0);
    for (const auto& [time, name] : state.document.keyframe_names) {
        (void)name;
        times.push_back(time);
    }
    std::ranges::sort(times);
    times.erase(std::unique(times.begin(), times.end()), times.end());
    return times;
}
std::string keyframe_name(const State& state, f32 time) {
    const auto found = state.document.keyframe_names.find(time);
    return found == state.document.keyframe_names.end() ? std::string{} : found->second;
}
std::vector<KeyframeValue> keyframe_values(const State& state, f32 time) {
    return keyframe_values(animation_properties(state), state.document.timeline, time);
}
std::vector<KeyframeValue> keyframe_values(std::span<const AnimationProperty> properties,
                                          const timeline::Timeline& animation, f32 time) {
    std::vector<KeyframeValue> result;
    result.reserve(properties.size());
    for (const auto& property : properties) {
        auto value = animation.sample(property.target, time).value_or(property.base_value);
        const bool continuous =
            std::holds_alternative<f32>(value) || std::holds_alternative<Vec3>(value);
        KeyframeValue field{
            property.target, value,
            continuous ? timeline::Interpolation::linear : timeline::Interpolation::hold, false};
        if (const auto* track = animation.find(property.target)) {
            if (const auto it = std::ranges::find(track->keys, time, &timeline::Keyframe::time);
                it != track->keys.end()) {
                field.incoming = it->incoming;
                field.keyed = true;
            }
        }
        if (time == 0) field.keyed = true;
        result.push_back(std::move(field));
    }
    return result;
}
content::Result<void> add_keyframe(State& state, f32 time) {
    if (!valid_time(state, time))
        return invalid("Keyframe time must be inside the timeline");
    if (auto valid = validate_animation(state); !valid)
        return valid;
    if (exists(state, time))
        return {};
    // Sample before making any changes: insertion records the visible pose,
    // rather than snapping it back to the preceding timestamp.
    auto values = keyframe_values(state, time);
    for (auto& field : values) {
        field.keyed = true;
        if (const auto* track = state.document.timeline.find(field.target)) {
            const auto next = std::ranges::upper_bound(track->keys, time, {}, &timeline::Keyframe::time);
            if (next != track->keys.end()) field.incoming = next->incoming;
        }
    }
    if (state.document.keyframe_names.size() == timeline::max_total_keys)
        return invalid("Too many named keyframes");
    if (auto keyed = edit_property_keys(state, time, values); !keyed) return keyed;
    state.document.keyframe_names.try_emplace(time);
    return {};
}
content::Result<void> edit_keyframe(State& state, f32 time, std::span<const KeyframeValue> values) {
    if (!valid_time(state, time) || !exists(state, time))
        return invalid("The selected keyframe no longer exists");
    if (!state.document.keyframe_names.contains(time) &&
        state.document.keyframe_names.size() == timeline::max_total_keys)
        return invalid("Too many named keyframes");
    if (auto keyed = edit_property_keys(state, time, values); !keyed) return keyed;
    state.document.keyframe_names.try_emplace(time);
    return {};
}
content::Result<void> erase_keyframe(State& state, f32 time) {
    if (time == 0) return invalid("The initial keyframe at time zero cannot be deleted");
    if (!valid_time(state, time) || !exists(state, time))
        return invalid("The selected keyframe no longer exists");
    auto tracks = std::vector<timeline::Track>(state.document.timeline.tracks().begin(),
                                               state.document.timeline.tracks().end());
    for (auto& track : tracks)
        std::erase_if(track.keys, [&](const auto& key) { return key.time == time; });
    std::erase_if(tracks, [](const auto& track) { return track.keys.empty(); });
    if (auto result = state.document.timeline.replace(std::move(tracks)); !result)
        return invalid(result.error().message);
    state.document.keyframe_names.erase(time);
    return {};
}
content::Result<void> move_keyframe(State& state, f32 from, f32 to) {
    if (!valid_time(state, from) || !valid_time(state, to) || !exists(state, from))
        return invalid("Keyframe move requires an existing source and a time inside the timeline");
    if (from == to)
        return {};
    if (from == 0) return invalid("The initial keyframe at time zero cannot be moved");
    if (exists(state, to))
        return invalid("Another keyframe already exists at that time");
    auto tracks = std::vector<timeline::Track>(state.document.timeline.tracks().begin(),
                                               state.document.timeline.tracks().end());
    for (auto& track : tracks)
        for (auto& key : track.keys)
            if (key.time == from)
                key.time = to;
    if (auto result = state.document.timeline.replace(std::move(tracks)); !result)
        return invalid(result.error().message);
    if (auto name = state.document.keyframe_names.extract(from); !name.empty()) {
        name.key() = to;
        state.document.keyframe_names.insert(std::move(name));
    }
    return {};
}
content::Result<void> update_keyframe(State& state, f32 from, f32 to, std::string name,
                                      std::span<const KeyframeValue> values) {
    auto candidate = state;
    if (auto edited = edit_keyframe(candidate, from, values); !edited)
        return edited;
    candidate.document.keyframe_names[from] = std::move(name);
    if (auto moved = move_keyframe(candidate, from, to); !moved)
        return moved;
    if (auto valid = validate_animation(candidate); !valid)
        return valid;
    state.document.timeline = std::move(candidate.document.timeline);
    state.document.keyframe_names = std::move(candidate.document.keyframe_names);
    return {};
}
} // namespace editor_example
