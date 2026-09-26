#include "key_batch.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace example {
namespace {
using namespace vng;
namespace project = editor_example;
constexpr double degrees = 180 / std::numbers::pi;
auto invalid(std::string message) {
    content::Diagnostic error;
    error.code = content::ErrorCode::invalid_document;
    error.message = std::move(message);
    return std::unexpected(std::move(error));
}
// Wrap each component of `value` by whole turns to lie within half a turn of `previous`.
Vec3 nearest_turn(Vec3 value, Vec3 previous) {
    for (unsigned i = 0; i < 3; ++i)
        value[i] -= 360.F * std::round((value[i] - previous[i]) / 360.F);
    return value;
}
} // namespace

void KeyBatch::key(timeline::Target target, f32 time, timeline::Value value, timeline::Interpolation incoming) {
    auto& keys = tracks_[{target.object, std::move(target.property)}];
    count_ += !keys.contains(time);
    keys[time] = {time, std::move(value), incoming};
}

void KeyBatch::simplify(const timeline::Target& target, f32 tolerance) {
    const auto found = tracks_.find({target.object, target.property});
    if (found == tracks_.end() || found->second.size() < 3) return;
    std::vector<timeline::Keyframe> keys;
    for (const auto& [time, key] : found->second) keys.push_back(key);
    const auto error = [](const timeline::Value& a, const timeline::Value& b, f32 u, const timeline::Value& actual) {
        if (const auto* x = std::get_if<f32>(&a))
            return std::abs(std::lerp(*x, std::get<f32>(b), u) - std::get<f32>(actual));
        if (const auto* x = std::get_if<Vec3>(&a)) {
            const auto& y = std::get<Vec3>(b);
            const auto& z = std::get<Vec3>(actual);
            return std::hypot(std::lerp(x->x, y.x, u) - z.x, std::lerp(x->y, y.y, u) - z.y, std::lerp(x->z, y.z, u) - z.z);
        }
        return std::numeric_limits<f32>::infinity();
    };
    std::map<f32, timeline::Keyframe> kept{{keys.front().time, keys.front()}};
    std::size_t anchor = 0;
    for (std::size_t i = 1; i + 1 < keys.size(); ++i) {
        const auto& next = keys[i + 1];
        bool fits = keys[i].incoming == timeline::Interpolation::linear &&
                    next.incoming == timeline::Interpolation::linear;
        for (auto k = anchor + 1; fits && k <= i; ++k) {
            const auto u = (keys[k].time - keys[anchor].time) / (next.time - keys[anchor].time);
            fits = error(keys[anchor].value, next.value, u, keys[k].value) <= tolerance;
        }
        if (!fits) { kept[keys[i].time] = keys[i]; anchor = i; }
    }
    kept[keys.back().time] = keys.back();
    count_ -= found->second.size() - kept.size();
    found->second = std::move(kept);
}

content::Result<void> KeyBatch::commit(project::State& state) {
    const auto properties = project::animation_properties(state);
    std::vector<timeline::Track> tracks(state.document.timeline.tracks().begin(), state.document.timeline.tracks().end());
    for (auto& [identity, batch] : tracks_) {
        const auto& [object, property] = identity;
        const timeline::Target target{object, property};
        const auto described = std::ranges::find(properties, target, &project::AnimationProperty::target);
        if (described == properties.end()) return invalid("Unknown animated scene property '" + property + "'");
        auto existing = std::ranges::find(tracks, target, &timeline::Track::target);
        if (existing == tracks.end())
            existing = tracks.insert(tracks.end(), {target, described->label, described->layer, {}});
        std::map<f32, timeline::Keyframe> keys;
        for (const auto& key : existing->keys) keys[key.time] = key;
        for (auto& [time, key] : batch) {
            if (std::holds_alternative<bool>(key.value)) key.incoming = timeline::Interpolation::hold;
            const auto finite = std::visit([](const auto& value) {
                if constexpr (std::same_as<std::decay_t<decltype(value)>, Vec3>)
                    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
                else if constexpr (std::same_as<std::decay_t<decltype(value)>, f32>) return std::isfinite(value);
                else return true;
            }, key.value);
            if (!finite || !std::isfinite(time))
                return invalid("Non-finite '" + property + "' key for object " + std::to_string(object) +
                               " at " + std::to_string(time) + " s");
            if (property != "rotation")
                if (auto valid = project::validate_property_value(*described, key.value); !valid)
                    return invalid(valid.error().message + " (object " + std::to_string(object) + " at " +
                                   std::to_string(time) + " s)");
            keys[time] = std::move(key);
        }
        if (keys.begin()->first > 0) keys[0] = {0, described->base_value, timeline::Interpolation::hold};
        existing->keys.clear();
        for (auto& [time, key] : keys) existing->keys.push_back(std::move(key));
        if (property == "rotation") {
            for (std::size_t i = 1; i < existing->keys.size(); ++i) {
                auto& key = existing->keys[i];
                if (key.incoming != timeline::Interpolation::linear) continue;
                key.value = nearest_turn(std::get<Vec3>(key.value), std::get<Vec3>(existing->keys[i - 1].value));
            }
            // Whole turns do not change an orientation: center each component
            // in the editor's +/-360 degree range.
            for (unsigned c = 0; c < 3; ++c) {
                f32 low = std::numeric_limits<f32>::max(), high = std::numeric_limits<f32>::lowest();
                for (const auto& key : existing->keys) {
                    low = std::min(low, std::get<Vec3>(key.value)[c]);
                    high = std::max(high, std::get<Vec3>(key.value)[c]);
                }
                if (high - low > 720) {
                    std::string trace;
                    for (std::size_t i = 1; i < existing->keys.size(); ++i) {
                        const auto a = std::get<Vec3>(existing->keys[i-1].value)[c], b = std::get<Vec3>(existing->keys[i].value)[c];
                        if (std::abs(b - a) > 20) trace += " t=" + std::to_string(existing->keys[i].time) + ":" + std::to_string(a) + "->" + std::to_string(b);
                        if (trace.size() > 900) break;
                    }
                    return invalid("Rotation of object " + std::to_string(object) + " component " + std::to_string(c) +
                                   " turns more than twice around;" + trace);
                }
                // The whole turns that fit [low, high] inside [-360, 360].
                const auto shift = 360.F * std::ceil((high - 360.F) / 360.F);
                if (low - shift < -360.F) {
                    std::string trace;
                    for (std::size_t i = 1; i < existing->keys.size() && trace.size() < 700; ++i) {
                        const auto a = std::get<Vec3>(existing->keys[i-1].value)[c], b = std::get<Vec3>(existing->keys[i].value)[c];
                        if (std::abs(b - a) > 25) trace += " t=" + std::to_string(existing->keys[i].time) + ":" + std::to_string(a) + "->" + std::to_string(b);
                    }
                    return invalid("Rotation of object " + std::to_string(object) + " axis " + std::to_string(c) +
                                   " spans " + std::to_string(low) + ".." + std::to_string(high) + ";" + trace);
                }
                for (auto& key : existing->keys) std::get<Vec3>(key.value)[c] -= shift;
            }
        }
    }
    std::size_t total{};
    for (const auto& track : tracks) total += track.keys.size();
    if (total > timeline::max_total_keys) {
        std::ranges::sort(tracks, std::greater{}, [](const auto& track) { return track.keys.size(); });
        auto message = std::to_string(total) + " keys exceed the timeline limit of " +
                       std::to_string(timeline::max_total_keys) + "; largest:";
        for (std::size_t i = 0; i < std::min<std::size_t>(tracks.size(), 6); ++i)
            message += " " + std::to_string(tracks[i].target.object) + "/" + tracks[i].target.property + " " +
                       std::to_string(tracks[i].keys.size());
        return invalid(message);
    }
    auto previous = state.document.timeline;
    if (auto replaced = state.document.timeline.replace(std::move(tracks)); !replaced)
        return invalid(replaced.error().message);
    if (auto valid = project::validate_animation(state); !valid) {
        state.document.timeline = std::move(previous);
        return valid;
    }
    tracks_.clear();
    count_ = 0;
    return {};
}

void key_look(KeyBatch& batch, u32 camera, f32 time, const Look& look, timeline::Interpolation incoming) {
    const double dx = double(look.eye.x) - look.target.x, dy = double(look.eye.y) - look.target.y,
                 dz = double(look.eye.z) - look.target.z;
    const auto distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    // Camera instances store the look direction as {-pitch, yaw, 0}.
    const Vec3 rotation{static_cast<f32>(-std::asin(dy / distance) * degrees),
                        static_cast<f32>(std::atan2(dx, dz) * degrees), 0};
    batch.key({camera, "position"}, time, look.eye, incoming);
    batch.key({camera, "rotation"}, time, rotation, incoming);
    batch.key({camera, "focus"}, time,
              std::clamp(static_cast<f32>(distance), project::camera_min_distance, project::camera_max_distance), incoming);
    batch.key({camera, "zoom"}, time, look.zoom, incoming);
}

project::CameraPose orbit_pose(const Look& look) {
    const double dx = double(look.eye.x) - look.target.x, dy = double(look.eye.y) - look.target.y,
                 dz = double(look.eye.z) - look.target.z;
    const auto distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    return {static_cast<f32>(std::atan2(dx, dz) * degrees), static_cast<f32>(std::asin(dy / distance) * degrees),
            static_cast<f32>(distance), look.target, look.zoom};
}

Vec3 orientation(Vec3 forward, Vec3 up) {
    const auto normalize = [](std::array<double, 3> v) {
        const auto l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        return std::array{v[0] / l, v[1] / l, v[2] / l};
    };
    const auto cross = [](std::array<double, 3> a, std::array<double, 3> b) {
        return std::array{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    // Columns of R = Rz*Ry*Rx are the local axes: +X right, +Y up, +Z back.
    const auto back = normalize({-double(forward.x), -double(forward.y), -double(forward.z)});
    const auto right = normalize(cross({up.x, up.y, up.z}, back));
    const auto top = cross(back, right);
    auto pitch = std::atan2(top[2], back[2]);
    auto yaw = std::asin(std::clamp(-right[2], -1.0, 1.0));
    auto roll = std::atan2(right[1], right[0]);
    // Nose along +/-X: gimbal lock, where pitch and roll above are both
    // atan2(0, 0). Only their sum or difference matters there; keep roll at
    // zero and take pitch from what remains of the up and back axes.
    if (std::hypot(right[0], right[1]) < 1e-9) {
        roll = 0;
        pitch = std::atan2(-back[1], top[1]);
        return {static_cast<f32>(pitch * degrees), static_cast<f32>(yaw * degrees), 0};
    }
    // (pitch, yaw, roll) and (pitch+180, 180-yaw, roll+180) are the same
    // rotation. Craft use the branch with pitch within +/-90 degrees, so yaw
    // turns freely and stays continuous through any heading.
    if (std::abs(pitch) > std::numbers::pi / 2) {
        const auto wrap = [](double a) { return std::remainder(a, 2 * std::numbers::pi); };
        pitch = wrap(pitch + std::numbers::pi);
        yaw = wrap(std::numbers::pi - yaw);
        roll = wrap(roll + std::numbers::pi);
    }
    return {static_cast<f32>(pitch * degrees), static_cast<f32>(yaw * degrees), static_cast<f32>(roll * degrees)};
}
} // namespace example
