#include "legacy_camera.hpp"
#include "authoring_limits.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <span>

namespace editor_example {
using namespace vng;
using timeline::Interpolation;
using timeline::Keyframe;
namespace {
auto invalid(std::string message) {
    content::Diagnostic error;
    error.code = content::ErrorCode::invalid_document;
    error.message = std::move(message);
    return std::unexpected(std::move(error));
}

f32 lerp(f32 from, f32 to, double ratio) {
    return static_cast<f32>(std::lerp(static_cast<double>(from), static_cast<double>(to), ratio));
}
Vec3 lerp(Vec3 from, Vec3 to, double ratio) {
    return {lerp(from.x, to.x, ratio), lerp(from.y, to.y, ratio), lerp(from.z, to.z, ratio)};
}
f32 distance(Vec3 a, Vec3 b) {
    return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z);
}

// One track with its own keys and interpolation, sampled exactly as
// Timeline::sample does: the base value where it is untracked or before its
// first key, then its keys (so the first key is a cut from the base).
template<class T>
class Component {
public:
    Component(const std::vector<Keyframe>* keys, T base) : keys_(keys), base_(base) {}
    [[nodiscard]] T at(f32 time) const { return value(time, false); }
    // The value just before `time`: differs from at() only at a cut.
    [[nodiscard]] T before(f32 time) const { return value(time, true); }
    [[nodiscard]] std::span<const Keyframe> keys() const { return keys_ ? *keys_ : std::span<const Keyframe>{}; }
private:
    [[nodiscard]] T value(f32 time, bool left) const {
        if (!keys_) return base_;
        const auto& keys = *keys_;
        const auto next = std::ranges::lower_bound(keys, time, {}, &Keyframe::time);
        if (!left && next != keys.end() && next->time == time) return std::get<T>(next->value);
        if (next == keys.begin()) return base_;
        const auto& previous = *(next - 1);
        if (next == keys.end() || next->incoming == Interpolation::hold) return std::get<T>(previous.value);
        const double ratio = (static_cast<double>(time) - previous.time) /
                             (static_cast<double>(next->time) - previous.time);
        return lerp(std::get<T>(previous.value), std::get<T>(next->value), ratio);
    }
    const std::vector<Keyframe>* keys_;
    T base_;
};

struct Shot {
    Component<f32> yaw, pitch, distance, zoom;
    Component<Vec3> target;
    [[nodiscard]] CameraPose at(f32 t) const { return {yaw.at(t), pitch.at(t), distance.at(t), target.at(t), zoom.at(t)}; }
    [[nodiscard]] CameraPose before(f32 t) const {
        return {yaw.before(t), pitch.before(t), distance.before(t), target.before(t), zoom.before(t)};
    }
};

// The latest representable time strictly between `after` and `cut`, close to the cut.
std::optional<f32> just_before(f32 cut, f32 after) {
    f32 time = cut - std::min(1e-3F, (cut - after) * .5F);
    if (!(after < time && time < cut)) time = std::nextafter(cut, after);
    if (!(after < time && time < cut)) return {};
    return time;
}

// Rotation is {-pitch, yaw, 0}, linear wherever yaw and pitch both are, so a
// key at each of their key times reproduces both. A cut in one while the
// other still moves becomes a key just before the cut followed by a held key
// at it, so neither the cut nor the motion is lost.
std::vector<Keyframe> rotation_keys(const Component<f32>& yaw, const Component<f32>& pitch) {
    std::set<f32> times;
    for (const auto* component : {&yaw, &pitch})
        for (const auto& key : component->keys()) times.insert(key.time);
    const auto at = [&](f32 t) { return Vec3{-pitch.at(t), yaw.at(t), 0}; };
    const auto before = [&](f32 t) { return Vec3{-pitch.before(t), yaw.before(t), 0}; };
    std::vector<Keyframe> keys;
    for (auto time = times.begin(); time != times.end(); ++time) {
        if (time == times.begin()) { keys.push_back({*time, at(*time), Interpolation::hold}); continue; }
        const f32 a = *std::prev(time), b = *time;
        if (const auto left = before(b), to = at(b); left == to) {
            keys.push_back({b, to, Interpolation::linear});
        } else if (const auto freeze = just_before(b, a); at(a) == left || !freeze) {
            // Nothing moved before the cut, or there is no room to keep the motion.
            keys.push_back({b, to, Interpolation::hold});
        } else {
            keys.push_back({*freeze, at(*freeze), Interpolation::linear});
            keys.push_back({b, to, Interpolation::hold});
        }
    }
    return keys;
}

// Last resort when the rotation keys exceed a timeline limit: repeatedly drop
// the interior key its neighbours reproduce best, so the scene still loads. A
// target of zero drops the track, leaving the camera's base rotation.
void thin(std::vector<Keyframe>& keys, std::size_t target) {
    if (target == 0) { keys.clear(); return; }
    if (keys.size() <= target) return;
    const auto n = keys.size();
    std::vector<std::size_t> previous(n), next(n);
    for (std::size_t i = 0; i < n; ++i) { previous[i] = i ? i - 1 : n; next[i] = i + 1; }
    const auto value = [&](std::size_t i) { return std::get<Vec3>(keys[i].value); };
    const auto cost = [&](std::size_t i) -> f32 {
        const auto p = previous[i], q = next[i];
        const double ratio = (static_cast<double>(keys[i].time) - keys[p].time) /
                             (static_cast<double>(keys[q].time) - keys[p].time);
        const auto model = keys[q].incoming == Interpolation::hold ? value(p) : lerp(value(p), value(q), ratio);
        const auto actual_before = keys[i].incoming == Interpolation::hold ? value(p) : value(i);
        return std::max(distance(model, value(i)), distance(model, actual_before));
    };
    std::set<std::pair<f32, std::size_t>> queue;
    std::vector<f32> costs(n, 0);
    for (std::size_t i = 1; i + 1 < n; ++i) queue.insert({costs[i] = cost(i), i});
    std::vector<bool> alive(n, true);
    auto count = n;
    while (count > target && count > 2 && !queue.empty()) {
        const auto [ignored, i] = *queue.begin();
        queue.erase(queue.begin());
        alive[i] = false;
        --count;
        const auto p = previous[i], q = next[i];
        next[p] = q;
        previous[q] = p;
        for (const auto j : {p, q}) {
            if (j == 0 || j + 1 >= n) continue;
            queue.erase({costs[j], j});
            queue.insert({costs[j] = cost(j), j});
        }
    }
    if (count > target) alive[n - 1] = false; // A single key: keep the first.
    std::vector<Keyframe> kept;
    for (std::size_t i = 0; i < n; ++i) if (alive[i]) kept.push_back(std::move(keys[i]));
    keys = std::move(kept);
}

Vec3 clamp_position(Vec3 p) {
    for (unsigned c = 0; c < 3; ++c) p[c] = std::clamp(p[c], -scene_coordinate_limit, scene_coordinate_limit);
    return p;
}
} // namespace

LegacyCameraShot read_legacy_camera_shot(content::Reader file, const CameraPose& view_camera) {
    LegacyCameraShot shot{view_camera, {}};
    for (const auto member : file.members())
        if (member.name == "animation_camera")
            shot.pose = {member.value.get<f32>("yaw"), member.value.get<f32>("pitch"), member.value.get<f32>("distance"),
                         member.value.get_or<Vec3>("camera_target", {}), member.value.get_or<f32>("zoom", 1)};
    return shot;
}

std::vector<AnimationProperty> legacy_camera_properties(const CameraPose& base) {
    return {{{legacy_camera_object, "yaw"}, "Orbit (deg)", "Camera", base.yaw, -180, 180},
            {{legacy_camera_object, "pitch"}, "Elevation (deg)", "Camera", base.pitch, -camera_max_pitch, camera_max_pitch},
            {{legacy_camera_object, "distance"}, "Distance", "Camera", base.distance, camera_min_distance, camera_max_distance},
            {{legacy_camera_object, "target"}, "Look-at target", "Camera", base.target, -camera_target_limit, camera_target_limit},
            {{legacy_camera_object, "zoom"}, "Optical zoom", "Camera", base.zoom, camera_min_zoom, camera_max_zoom}};
}

content::Result<void> migrate_legacy_camera(State& state, const LegacyCameraShot& shot) {
    if (has_camera(state)) return {}; // The shot was never used; it is dropped unread.
    if (!valid_camera_pose(shot.pose)) return invalid("Invalid legacy animation camera");
    // A scene already at the instance limit keeps loading, without a camera.
    if (state.document.instances.size() >= max_scene_instances || !state.document.next_instance_id ||
        state.document.next_instance_id == std::numeric_limits<u32>::max())
        return {};
    const auto selected = state.viewport.selected_object;
    const auto vertex = state.viewport.selected_vertex;
    auto created = instantiate(state, BlueprintId::camera);
    if (!created) return std::unexpected(created.error());
    state.viewport.selected_object = selected;
    state.viewport.selected_vertex = vertex;
    auto* camera = find_instance(state, *created);
    camera->name = "Animation camera";
    auto& lens = std::get<CameraSettings>(camera->settings);
    lens.active = true;
    // The old camera orbited its target; an orbiting camera moves the same way.
    lens.orbit = true;
    place_camera(*camera, shot.pose);
    // An old eye could lie beyond the scene's coordinate range (target plus
    // distance); keep the scene loadable rather than reject it.
    camera->transform.position = clamp_position(camera->transform.position);
    if (shot.tracks.empty()) return {};

    // The old decoder put these tracks through the timeline, which sorts keys
    // and rejects duplicates or bad values; keep exactly those rules.
    timeline::Timeline old;
    if (auto normalized = old.replace(shot.tracks); !normalized) return invalid(normalized.error().message);
    const auto keys_of = [&](std::string_view property) -> const std::vector<Keyframe>* {
        const auto* track = old.find({legacy_camera_object, std::string(property)});
        return track ? &track->keys : nullptr;
    };
    const Shot legacy{{keys_of("yaw"), shot.pose.yaw}, {keys_of("pitch"), shot.pose.pitch},
                      {keys_of("distance"), shot.pose.distance}, {keys_of("zoom"), shot.pose.zoom},
                      {keys_of("target"), shot.pose.target}};
    for (const auto& source : old.tracks())
        for (const auto& key : source.keys)
            if (!valid_camera_pose(legacy.at(key.time)) || !valid_camera_pose(legacy.before(key.time)))
                return invalid("Invalid legacy animation camera key");

    // Each new track comes from the old tracks it depends on, key for key.
    // Focus and zoom are the old distance and zoom; rotation is yaw and pitch.
    const auto copy = [](const Component<f32>& component) {
        const auto keys = component.keys();
        return std::vector<Keyframe>(keys.begin(), keys.end());
    };
    const auto focus_keys = copy(legacy.distance), zoom_keys = copy(legacy.zoom);
    auto turn_keys = rotation_keys(legacy.yaw, legacy.pitch);
    // The old tracks fitted the scene's key limits; only the keys that keep a
    // cut in yaw or pitch apart from the other's motion can exceed them.
    std::size_t other_keys{};
    for (const auto& existing : state.document.timeline.tracks()) other_keys += existing.keys.size();
    const auto target_count = legacy.target.keys().size();
    const auto budget = timeline::max_total_keys -
        std::min(timeline::max_total_keys, other_keys + target_count + focus_keys.size() + zoom_keys.size());
    thin(turn_keys, std::min(timeline::max_keys_per_track, budget));

    // The eye has a key wherever the target has one, placed around the target
    // by the rotation and focus the camera has there. Between keys the camera
    // orbits, so its focus point follows the old target and the eye follows
    // yaw, pitch and distance as they did.
    const Component<Vec3> turn{&turn_keys, camera->transform.rotation};
    std::vector<Keyframe> position_keys;
    for (const auto& key : legacy.target.keys())
        position_keys.push_back({key.time, clamp_position(orbit_eye(std::get<Vec3>(key.value), turn.at(key.time),
                                                                    legacy.distance.at(key.time))),
                                 key.incoming});

    const auto properties = animation_properties(state);
    for (auto [property, keys] : {std::pair{"position", std::move(position_keys)}, std::pair{"rotation", std::move(turn_keys)},
                                  std::pair{"focus", focus_keys}, std::pair{"zoom", zoom_keys}}) {
        if (keys.empty()) continue;
        timeline::Track generated{{*created, property}, {}, {}, std::move(keys)};
        if (const auto found = std::ranges::find(properties, generated.target, &AnimationProperty::target);
            found != properties.end()) {
            generated.label = found->label;
            generated.layer = found->layer;
        }
        if (auto replaced = state.document.timeline.replace_track(std::move(generated)); !replaced)
            return invalid(replaced.error().message);
    }
    return {};
}
} // namespace editor_example
