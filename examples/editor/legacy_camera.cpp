#include "legacy_camera.hpp"
#include "authoring_limits.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>

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

// One component of the old shot with its own keys and interpolation, sampled
// exactly as Timeline::sample did for it: the base pose where the component is
// untracked or before its first key, then its keys (the first key is a cut).
template<class T>
class Component {
public:
    Component(const timeline::Track* track, T base) : track_(track), base_(base) {}
    [[nodiscard]] T at(f32 time) const { return value(time, false); }
    // The value just before `time`: differs from at() only at a cut.
    [[nodiscard]] T before(f32 time) const { return value(time, true); }
    void times(std::set<f32>& out) const {
        if (track_) for (const auto& key : track_->keys) out.insert(key.time);
    }
private:
    [[nodiscard]] T value(f32 time, bool left) const {
        if (!track_) return base_;
        const auto& keys = track_->keys;
        const auto next = std::ranges::lower_bound(keys, time, {}, &Keyframe::time);
        if (!left && next != keys.end() && next->time == time) return std::get<T>(next->value);
        if (next == keys.begin()) return base_;
        const auto& previous = *(next - 1);
        if (next == keys.end() || next->incoming == Interpolation::hold) return std::get<T>(previous.value);
        const double ratio = (static_cast<double>(time) - previous.time) /
                             (static_cast<double>(next->time) - previous.time);
        return lerp(std::get<T>(previous.value), std::get<T>(next->value), ratio);
    }
    const timeline::Track* track_;
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

// A Vec3 camera property as a function of time, with breakpoints where any of
// its source components has a key. Between breakpoints it is continuous; at a
// breakpoint it may cut. `moves(a, b)` says whether any source component
// changes between two neighbouring breakpoints (the derived value can return
// to where it started, as a full orbit does). `tolerance` is set for
// properties that curve between breakpoints (the eye on an orbit); others are
// linear there.
struct Signal {
    std::vector<f32> breakpoints;
    std::function<Vec3(f32)> at, before;
    std::function<bool(f32, f32)> moves;
    std::function<f32(f32)> tolerance;
};

// Up to 2^10 pieces between two old keys; beyond that the curve is left as is.
constexpr int max_depth = 10;

// Adds keys strictly inside (x, y) until straight segments stay within
// tolerance, giving up once `out` holds more than `limit` keys.
void refine(const Signal& signal, f32 scale, f32 x, Vec3 vx, f32 y, Vec3 vy, int depth, std::size_t limit,
            std::vector<Keyframe>& out) {
    const f32 mid = x + (y - x) * .5F;
    if (depth >= max_depth || out.size() > limit || !(x < mid && mid < y)) return;
    f32 error{};
    for (const auto fraction : {.25F, .5F, .75F}) {
        const f32 t = x + (y - x) * fraction;
        if (!(x < t && t < y)) continue;
        const double ratio = (static_cast<double>(t) - x) / (static_cast<double>(y) - x);
        error = std::max(error, distance(signal.at(t), lerp(vx, vy, ratio)));
    }
    if (error <= signal.tolerance(mid) * scale) return;
    const auto vm = signal.at(mid);
    refine(signal, scale, x, vx, mid, vm, depth + 1, limit, out);
    out.push_back({mid, vm, Interpolation::linear});
    refine(signal, scale, mid, vm, y, vy, depth + 1, limit, out);
}

// The latest representable time strictly between `after` and `cut`, close to the cut.
std::optional<f32> just_before(f32 cut, f32 after) {
    f32 time = cut - std::min(1e-3F, (cut - after) * .5F);
    if (!(after < time && time < cut)) time = std::nextafter(cut, after);
    if (!(after < time && time < cut)) return {};
    return time;
}

// Keys that reproduce the signal: exact at every breakpoint and cut, and within
// tolerance*scale in between (scale 0 disables refinement). A cut in one
// component while another still moves is kept as a key just before the cut
// followed by a held key at it, so neither the cut nor the motion is lost.
// Returns nothing when refinement at this scale would exceed `limit` keys.
std::optional<std::vector<Keyframe>> keys_for(const Signal& signal, f32 scale, std::size_t limit) {
    std::vector<Keyframe> keys;
    const auto& times = signal.breakpoints;
    if (times.empty()) return keys;
    const bool refinable = signal.tolerance && scale > 0;
    keys.push_back({times.front(), signal.at(times.front()), Interpolation::hold});
    for (std::size_t i = 1; i < times.size(); ++i) {
        const f32 a = times[i - 1], b = times[i];
        const auto from = signal.at(a), left = signal.before(b), to = signal.at(b);
        if (left == to) {
            if (refinable) refine(signal, scale, a, from, b, to, 0, limit, keys);
            keys.push_back({b, to, Interpolation::linear});
        } else if (const auto freeze = just_before(b, a); !signal.moves(a, b) || !freeze) {
            // Nothing moved before the cut, or there is no room to keep the motion.
            keys.push_back({b, to, Interpolation::hold});
        } else {
            const auto held = signal.at(*freeze);
            if (refinable) refine(signal, scale, a, from, *freeze, held, 0, limit, keys);
            keys.push_back({*freeze, held, Interpolation::linear});
            keys.push_back({b, to, Interpolation::hold});
        }
        if (refinable && keys.size() > limit) return {};
    }
    return keys;
}

// Last resort when the exact keys exceed a timeline limit: repeatedly drop the
// interior key its neighbours reproduce best, so the scene still loads. A
// target of zero drops the track, leaving the camera's base value.
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
    std::get<CameraSettings>(camera->settings).active = true;
    place_camera(*camera, shot.pose);
    // An old eye could lie beyond the scene's coordinate range (target plus
    // distance); keep the scene loadable rather than reject it.
    camera->transform.position = clamp_position(camera->transform.position);
    if (shot.tracks.empty()) return {};

    // The old decoder put these tracks through the timeline, which sorts keys
    // and rejects duplicates or bad values; keep exactly those rules.
    timeline::Timeline old;
    if (auto normalized = old.replace(shot.tracks); !normalized) return invalid(normalized.error().message);
    const auto track = [&](std::string_view property) -> const timeline::Track* {
        return old.find({legacy_camera_object, std::string(property)});
    };
    const Shot legacy{{track("yaw"), shot.pose.yaw}, {track("pitch"), shot.pose.pitch},
                      {track("distance"), shot.pose.distance}, {track("zoom"), shot.pose.zoom},
                      {track("target"), shot.pose.target}};
    for (const auto& source : old.tracks())
        for (const auto& key : source.keys)
            if (!valid_camera_pose(legacy.at(key.time)) || !valid_camera_pose(legacy.before(key.time)))
                return invalid("Invalid legacy animation camera key");

    // The placement a pose gives this camera, using the same conversion as Save this camera.
    auto placed = [probe = *camera](const CameraPose& pose) mutable {
        place_camera(probe, pose);
        return probe;
    };
    const auto breakpoints = [](std::initializer_list<std::function<void(std::set<f32>&)>> sources) {
        std::set<f32> times;
        for (const auto& source : sources) source(times);
        return std::vector<f32>(times.begin(), times.end());
    };
    const auto times_of = [](const auto& component) {
        return std::function<void(std::set<f32>&)>([&component](std::set<f32>& out) { component.times(out); });
    };
    // Between breakpoints every component is linear, so it moves iff its
    // values at the two ends differ.
    const auto orbit_moves = [&](f32 a, f32 b) {
        const auto from = legacy.at(a), to = legacy.before(b);
        return from.yaw != to.yaw || from.pitch != to.pitch || from.distance != to.distance || from.target != to.target;
    };
    const auto turn_moves = [&](f32 a, f32 b) {
        const auto from = legacy.at(a), to = legacy.before(b);
        return from.yaw != to.yaw || from.pitch != to.pitch;
    };
    // The eye moves on an orbit, so it is refined to within about half a pixel.
    const Signal position{
        breakpoints({times_of(legacy.yaw), times_of(legacy.pitch), times_of(legacy.distance), times_of(legacy.target)}),
        [&](f32 t) { return clamp_position(placed(legacy.at(t)).transform.position); },
        [&](f32 t) { return clamp_position(placed(legacy.before(t)).transform.position); },
        orbit_moves,
        [&](f32 t) {
            const auto pose = legacy.at(t);
            return 5e-4F * pose.distance / std::max(1.F, pose.zoom);
        }};
    // Rotation is {-pitch, yaw, 0}: linear wherever yaw and pitch are.
    const Signal rotation{
        breakpoints({times_of(legacy.yaw), times_of(legacy.pitch)}),
        [&](f32 t) { return placed(legacy.at(t)).transform.rotation; },
        [&](f32 t) { return placed(legacy.before(t)).transform.rotation; },
        turn_moves,
        {}};
    // Focus and zoom are the old distance and zoom, key for key.
    const auto copy = [&](std::string_view property) {
        const auto* source = track(property);
        return source ? source->keys : std::vector<Keyframe>{};
    };
    const auto focus_keys = copy("distance"), zoom_keys = copy("zoom");

    // The old tracks fitted the scene's key budget; so must the new ones.
    std::size_t other_keys{};
    for (const auto& existing : state.document.timeline.tracks()) other_keys += existing.keys.size();
    const auto budget = timeline::max_total_keys - std::min(timeline::max_total_keys, other_keys + focus_keys.size() + zoom_keys.size());
    auto rotation_keys = *keys_for(rotation, 0, 0);
    // Refine the eye as finely as the limits allow; each attempt stops early.
    const auto room = std::min(timeline::max_keys_per_track, budget - std::min(budget, rotation_keys.size()));
    std::optional<std::vector<Keyframe>> refined;
    for (f32 scale = 1; !refined && scale <= 1 << 20; scale *= 4) refined = keys_for(position, scale, room);
    auto position_keys = refined ? std::move(*refined) : *keys_for(position, 0, 0);
    if (position_keys.size() > room || rotation_keys.size() > timeline::max_keys_per_track) {
        auto rotation_target = std::min(rotation_keys.size(), timeline::max_keys_per_track);
        auto position_target = std::min(position_keys.size(), timeline::max_keys_per_track);
        if (const auto wanted = rotation_target + position_target; wanted > budget) {
            // Share what is left in proportion to what each track wanted; with
            // no room at all a track is dropped rather than failing the load.
            rotation_target = std::min(rotation_target, std::max<std::size_t>(budget ? 1 : 0, rotation_target * budget / wanted));
            position_target = budget - rotation_target;
        }
        thin(rotation_keys, rotation_target);
        thin(position_keys, position_target);
    }

    const auto properties = animation_properties(state);
    for (auto [property, keys] : {std::pair{"position", std::move(position_keys)}, std::pair{"rotation", std::move(rotation_keys)},
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
