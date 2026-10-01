#include <vng/timeline/timeline.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <concepts>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

namespace vng::timeline {
namespace {

bool target_before(const Target& left, const Target& right) noexcept
{
    return left.object < right.object ||
           (left.object == right.object && left.property < right.property);
}

template<class Tracks>
auto track_at(Tracks& tracks, const Target& target)
{
    return std::lower_bound(tracks.begin(), tracks.end(), target,
                           [](const Track& track, const Target& key) {
                               return target_before(track.target, key);
                           });
}

template<class Keys>
auto key_at(Keys& keys, f32 time)
{
    return std::lower_bound(keys.begin(), keys.end(), time,
                           [](const Keyframe& key, f32 at) { return key.time < at; });
}

auto fail(ErrorCode code, std::string message, const Target& target,
          std::optional<f32> time = {})
{
    return std::unexpected(Diagnostic{code, std::move(message), target, time});
}

bool valid_time(f32 time) noexcept
{
    return std::isfinite(time) && time >= 0 && time <= max_time;
}

bool valid_text(std::string_view text, std::size_t limit) noexcept
{
    return text.size() <= limit && text.find('\0') == std::string_view::npos;
}

Result<void> validate_target(const Target& target)
{
    if (!target.object || target.property.empty() ||
        !valid_text(target.property, max_property_bytes)) {
        return fail(ErrorCode::invalid_target,
                    "A timeline target needs a nonzero object and a nonempty property of at most 256 bytes without NUL", target);
    }
    return {};
}

Result<void> validate_metadata(const Target& target, std::string_view label, std::string_view layer)
{
    if (!valid_text(label, max_label_bytes) || !valid_text(layer, max_layer_bytes)) {
        return fail(ErrorCode::invalid_metadata,
                    "Timeline labels/layers must be bounded strings without NUL", target);
    }
    return {};
}

Result<void> validate_key(const Target& target, const Keyframe& key)
{
    if (!valid_time(key.time)) {
        return fail(ErrorCode::invalid_time,
                    "Keyframe time must be finite and between 0 and 86400 seconds", target, key.time);
    }
    if (key.value.valueless_by_exception()) {
        return fail(ErrorCode::invalid_value, "Keyframe has no value", target, key.time);
    }
    const bool valid = std::visit([](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::same_as<T, f32>) {
            return std::isfinite(value);
        } else if constexpr (std::same_as<T, Vec3>) {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        } else if constexpr (std::same_as<T, std::string>) {
            return value.size() <= max_string_value_bytes;
        } else {
            return true;
        }
    }, key.value);
    if (!valid) {
        return fail(ErrorCode::invalid_value,
                    "Keyframe values must be finite and strings cannot exceed 4096 bytes", target, key.time);
    }
    if (key.incoming != Interpolation::hold && key.incoming != Interpolation::linear && key.incoming != Interpolation::cubic) {
        return fail(ErrorCode::invalid_interpolation, "Unknown keyframe interpolation mode", target, key.time);
    }
    if (key.incoming != Interpolation::hold &&
        !std::holds_alternative<f32>(key.value) && !std::holds_alternative<Vec3>(key.value)) {
        return fail(ErrorCode::invalid_interpolation,
                    "Interpolation requires a float or Vec3 track; discrete values use hold", target, key.time);
    }
    return {};
}

std::size_t key_count(std::span<const Track> tracks) noexcept
{
    std::size_t total{};
    for (const auto& track : tracks) total += track.keys.size();
    return total;
}

Result<void> validate_track(Track& track)
{
    if (auto result = validate_target(track.target); !result) return result;
    if (auto result = validate_metadata(track.target, track.label, track.layer); !result) return result;
    if (track.keys.empty()) return fail(ErrorCode::empty_track, "Timeline tracks cannot be empty", track.target);
    if (track.keys.size() > max_keys_per_track)
        return fail(ErrorCode::limit_exceeded, "Timeline keyframe limit exceeded", track.target);
    for (auto& key : track.keys) {
        if (auto result = validate_key(track.target, key); !result) return result;
        if (key.value.index() != track.keys.front().value.index())
            return fail(ErrorCode::type_mismatch, "All keys in a track must have the same value type", track.target, key.time);
        if (key.time == 0) key.time = 0;
    }
    std::sort(track.keys.begin(), track.keys.end(),
              [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    for (std::size_t i = 1; i < track.keys.size(); ++i)
        if (track.keys[i - 1].time == track.keys[i].time)
            return fail(ErrorCode::duplicate_time, "Duplicate keyframe time in a track", track.target, track.keys[i].time);
    return {};
}

// Double precision avoids overflow for finite endpoints with opposite signs
// and preserves the ratio for very closely spaced floating-point key times.
f32 interpolate(f32 from, f32 to, double ratio)
{
    return static_cast<f32>(std::lerp(static_cast<double>(from), static_cast<double>(to), ratio));
}

double component(const Value& value, unsigned axis) {
    if (const auto* scalar=std::get_if<f32>(&value)) return *scalar;
    return std::get<Vec3>(value)[axis];
}
double slope(std::span<const Keyframe> keys, std::size_t i, unsigned axis) {
    const auto secant=[&](std::size_t left) {
        return (component(keys[left+1].value,axis)-component(keys[left].value,axis))/
               (double(keys[left+1].time)-keys[left].time);
    };
    const bool left=i>0 && keys[i].incoming!=Interpolation::hold;
    const bool right=i+1<keys.size() && keys[i+1].incoming!=Interpolation::hold;
    if (left && right) {
        const auto a=secant(i-1),b=secant(i);
        if (a*b<=0) return 0;
        const double h0=double(keys[i].time)-keys[i-1].time,h1=double(keys[i+1].time)-keys[i].time;
        const auto w0=2*h1+h0,w1=h1+2*h0;
        return (w0+w1)/(w0/a+w1/b);
    }
    // One-sided three-point endpoint slope, limited to retain monotonicity.
    const auto endpoint=[](double a,double b,double h0,double h1) {
        auto m=((2*h0+h1)*a-h0*b)/(h0+h1);
        if (m*a<=0) return 0.;
        if (a*b<=0 && std::abs(m)>3*std::abs(a)) m=3*a;
        return m;
    };
    if (right) {
        if (i+2<keys.size() && keys[i+2].incoming!=Interpolation::hold)
            return endpoint(secant(i),secant(i+1),double(keys[i+1].time)-keys[i].time,double(keys[i+2].time)-keys[i+1].time);
        return secant(i);
    }
    if (left) {
        if (i>1 && keys[i-1].incoming!=Interpolation::hold)
            return endpoint(secant(i-1),secant(i-2),double(keys[i].time)-keys[i-1].time,double(keys[i-1].time)-keys[i-2].time);
        return secant(i-1);
    }
    return 0;
}

} // namespace

void Timeline::changed() noexcept
{
    static std::atomic<u64> next{1};
    version_ = next.fetch_add(1, std::memory_order_relaxed);
}

const Track* Timeline::find(const Target& target) const noexcept
{
    const auto at = track_at(tracks_, target);
    return at != tracks_.end() && at->target == target ? &*at : nullptr;
}

Result<void> Timeline::set(Target target, Keyframe key, std::string label, std::string layer)
{
    if (auto result = validate_target(target); !result) return result;
    if (auto result = validate_metadata(target, label, layer); !result) return result;
    if (auto result = validate_key(target, key); !result) return result;
    if (key.time == 0) key.time = 0; // Canonicalize negative zero.
    auto at = track_at(tracks_, target);
    if (at != tracks_.end() && at->target == target) {
        if (at->keys.front().value.index() != key.value.index()) {
            return fail(ErrorCode::type_mismatch, "All keys in a track must have the same value type", target, key.time);
        }
        auto key_position = key_at(at->keys, key.time);
        if (key_position != at->keys.end() && key_position->time == key.time) {
            *key_position = std::move(key);
        } else {
            if (at->keys.size() == max_keys_per_track || key_count(tracks_) == max_total_keys) {
                return fail(ErrorCode::limit_exceeded, "Timeline keyframe limit reached", target, key.time);
            }
            at->keys.insert(key_position, std::move(key));
        }
        if (!label.empty()) at->label = std::move(label);
        if (!layer.empty()) at->layer = std::move(layer);
    } else {
        if (tracks_.size() == max_tracks || key_count(tracks_) == max_total_keys) {
            return fail(ErrorCode::limit_exceeded, "Timeline track or total keyframe limit reached", target, key.time);
        }
        Track added{std::move(target), std::move(label), std::move(layer), {}};
        added.keys.push_back(std::move(key));
        tracks_.insert(at, std::move(added));
    }
    changed();
    return {};
}

bool Timeline::erase(const Target& target, f32 time)
{
    if (!valid_time(time)) return false;
    auto track = track_at(tracks_, target);
    if (track == tracks_.end() || track->target != target) return false;
    const auto key = key_at(track->keys, time);
    if (key == track->keys.end() || key->time != time) return false;
    track->keys.erase(key);
    if (track->keys.empty()) tracks_.erase(track);
    changed();
    return true;
}

bool Timeline::erase(const Target& target)
{
    const auto track = track_at(tracks_, target);
    if (track == tracks_.end() || track->target != target) return false;
    tracks_.erase(track);
    changed();
    return true;
}

Result<void> Timeline::replace_track(Track replacement)
{
    if (auto valid = validate_track(replacement); !valid) return valid;
    const auto at = track_at(tracks_, replacement.target);
    const bool replacing = at != tracks_.end() && at->target == replacement.target;
    if (!replacing && tracks_.size() >= max_tracks)
        return fail(ErrorCode::limit_exceeded, "Timeline track limit exceeded", replacement.target);
    const auto old_keys = replacing ? at->keys.size() : 0;
    if (replacement.keys.size() > max_total_keys - (key_count(tracks_) - old_keys))
        return fail(ErrorCode::limit_exceeded, "Timeline keyframe limit exceeded", replacement.target);
    // Track moves are nonthrowing. Insertion allocates before mutating the
    // existing vector; replacement transfers only the incoming key storage.
    static_assert(std::is_nothrow_move_assignable_v<Track> && std::is_nothrow_move_constructible_v<Track>);
    if (replacing) *at = std::move(replacement);
    else tracks_.insert(at, std::move(replacement));
    changed();
    return {};
}

Result<void> Timeline::move(const Target& target, f32 from, f32 to)
{
    if (auto result = validate_target(target); !result) return result;
    if (!valid_time(from) || !valid_time(to)) {
        return fail(ErrorCode::invalid_time, "Keyframe move requires finite times between 0 and 86400 seconds", target,
                    !valid_time(from) ? from : to);
    }
    auto track = track_at(tracks_, target);
    if (track == tracks_.end() || track->target != target) {
        return fail(ErrorCode::not_found, "Cannot move a keyframe from a missing track", target, from);
    }
    const auto key = key_at(track->keys, from);
    if (key == track->keys.end() || key->time != from) {
        return fail(ErrorCode::not_found, "Cannot move a missing keyframe", target, from);
    }
    if (from == to) return {};
    const auto destination = key_at(track->keys, to);
    if (destination != track->keys.end() && destination->time == to) {
        return fail(ErrorCode::duplicate_time, "A keyframe already occupies the destination time", target, to);
    }
    key->time = to == 0 ? 0 : to;
    // Rotate rather than copying the track or allocating storage: a move changes
    // only key order/time and preserves the moved key's incoming interpolation.
    if (destination < key) {
        std::rotate(destination, key, key + 1);
    } else if (destination > key) {
        std::rotate(key, key + 1, destination);
    }
    changed();
    return {};
}

Result<void> Timeline::replace(std::vector<Track> tracks)
{
    if (tracks.size() > max_tracks) {
        return std::unexpected(Diagnostic{ErrorCode::limit_exceeded, "Timeline track limit exceeded", {}, {}});
    }
    std::size_t total{};
    for (auto& track : tracks) {
        if (auto result = validate_track(track); !result) return result;
        if (track.keys.size() > max_total_keys - total) {
            return fail(ErrorCode::limit_exceeded, "Timeline keyframe limit exceeded", track.target);
        }
        total += track.keys.size();
    }
    std::sort(tracks.begin(), tracks.end(),
              [](const Track& a, const Track& b) { return target_before(a.target, b.target); });
    for (std::size_t i = 1; i < tracks.size(); ++i) {
        if (tracks[i - 1].target == tracks[i].target) {
            return fail(ErrorCode::duplicate_target, "Only one track may address each object/property pair", tracks[i].target);
        }
    }
    tracks_ = std::move(tracks);
    changed();
    return {};
}

std::optional<Value> Timeline::sample(const Target& target, f32 time) const
{
    const auto* track = find(target);
    if (!track) return {};
    return sample_keys(track->keys,time);
}

std::optional<Value> sample_keys(std::span<const Keyframe> keys, f32 time) {
    if (!valid_time(time) || keys.empty()) return {};
    const auto next = key_at(keys, time);
    if (next != keys.end() && next->time == time) return next->value;
    if (next == keys.begin()) return {};
    const auto& previous = *(next - 1);
    if (next == keys.end() || next->incoming == Interpolation::hold) return previous.value;
    const double ratio = (static_cast<double>(time) - previous.time) /
                         (static_cast<double>(next->time) - previous.time);
    if (next->incoming==Interpolation::cubic) {
        const auto i=static_cast<std::size_t>(next-keys.begin());
        const auto dt=double(next->time)-previous.time,u=ratio,u2=u*u,u3=u2*u;
        const auto value=[&](unsigned axis) {
            const auto a=component(previous.value,axis),b=component(next->value,axis);
            const auto v=(2*u3-3*u2+1)*a+(u3-2*u2+u)*dt*slope(keys,i-1,axis)+
                         (-2*u3+3*u2)*b+(u3-u2)*dt*slope(keys,i,axis);
            return static_cast<f32>(std::clamp(v,std::min(a,b),std::max(a,b)));
        };
        if (std::holds_alternative<f32>(previous.value)) return value(0);
        return Vec3{value(0),value(1),value(2)};
    }
    if (const auto* from = std::get_if<f32>(&previous.value)) {
        return interpolate(*from, std::get<f32>(next->value), ratio);
    }
    const auto& from = std::get<Vec3>(previous.value);
    const auto& to = std::get<Vec3>(next->value);
    return Vec3{interpolate(from.x, to.x, ratio), interpolate(from.y, to.y, ratio),
                interpolate(from.z, to.z, ratio)};
}

Result<Track> simplify(Track track, f32 tolerance, Interpolation mode) {
    if (auto valid=validate_track(track);!valid) return std::unexpected(valid.error());
    if (!std::isfinite(tolerance) || tolerance<0 ||
        (mode!=Interpolation::linear && mode!=Interpolation::cubic))
        return fail(ErrorCode::invalid_value,"Reduction requires a finite nonnegative tolerance and linear/cubic interpolation",track.target);
    const auto original=track.keys;
    const bool continuous=std::holds_alternative<f32>(original.front().value)||std::holds_alternative<Vec3>(original.front().value);
    const auto error=[&](const Value& a,const Value& b) {
        if (!continuous) return a==b?0.:std::numeric_limits<double>::infinity();
        double sum{};
        for (unsigned axis=0;axis<(std::holds_alternative<Vec3>(a)?3U:1U);++axis) {
            const auto d=component(a,axis)-component(b,axis);sum+=d*d;
        }
        return std::sqrt(sum);
    };
    bool changed=true;
    while (changed) {
        changed=false;
        for (std::size_t i=1;i+1<track.keys.size();) {
            const auto& keys=track.keys;
            const bool redundant_hold=keys[i].incoming==Interpolation::hold &&
                keys[i+1].incoming==Interpolation::hold && keys[i].value==keys[i-1].value;
            if (!redundant_hold && (!continuous || keys[i].incoming==Interpolation::hold || keys[i+1].incoming==Interpolation::hold)) {++i;continue;}
            // Changing a Hermite neighbor can change the adjacent segments as
            // well. Verify that whole local dependency footprint, not just the
            // segment that loses a key. Always compare to the original track.
            const auto from=keys[i>3?i-3:0].time,to=keys[std::min(i+3,keys.size()-1)].time;
            auto candidate=keys;
            candidate.erase(candidate.begin()+static_cast<std::ptrdiff_t>(i));
            if (!redundant_hold) candidate[i].incoming=mode;
            bool fits=true;
            auto at=key_at(original,from);
            for (;fits && at!=original.end() && at->time<=to;++at) {
                const auto test=[&](f32 time) {
                    return error(*sample_keys(original,time),*sample_keys(candidate,time))<=tolerance;
                };
                fits=test(at->time);
                if (fits && at+1!=original.end() && (at+1)->time<=to)
                    for (unsigned part=1;fits && part<4;++part)
                        fits=test(std::lerp(at->time,(at+1)->time,static_cast<f32>(part)/4.F));
            }
            if (fits) {track.keys=std::move(candidate);changed=true;}
            else ++i;
        }
    }
    // An identical final hold adds no information, including on static tracks.
    while (track.keys.size()>1 && track.keys[track.keys.size()-2].incoming!=Interpolation::cubic &&
           track.keys.back().value==track.keys[track.keys.size()-2].value)
        track.keys.pop_back();
    return track;
}

} // namespace vng::timeline
