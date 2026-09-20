#pragma once

#include "keyframes.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>

namespace editor_example::detail {
using namespace vng;

// Shared implementation of bounded, timeline-aware scalar/Vec3 properties.
// Concrete position, rotation and scale snapshots retain distinct wire identities.
template<class Snapshot, class Edit, auto Base,
         Snapshot Edit::*Property, auto Member>
class InstancePropertyEdits {
public:
    using Value = std::remove_cvref_t<decltype(std::declval<Snapshot>().*Base)>;
    static_assert(std::same_as<Value, Vec3> || std::same_as<Value, f32>);
    std::array<char, 8> magic;
    std::string_view name;
    std::string_view label;
    f32 limit;
    f32 minimum{-limit};
    static constexpr std::size_t value_bytes = std::same_as<Value, Vec3> ? 12 : 4;
    static constexpr std::size_t header_bytes = 36 + value_bytes, key_bytes = 8 + value_bytes;
    static constexpr std::size_t maximum_bytes = header_bytes + 8 + timeline::max_label_bytes +
                                           timeline::max_layer_bytes + timeline::max_keys_per_track * key_bytes;
    static constexpr u64 max_revision = (u64{1} << 53) - 1;
    static_assert(sizeof(f32) == sizeof(u32) && std::numeric_limits<f32>::is_iec559);

    auto invalid(std::string message) {
        const auto replace = [&](std::string_view token, std::string_view value) {
            for (auto offset = message.find(token); offset != std::string::npos;
                 offset = message.find(token, offset + value.size()))
                message.replace(offset, token.size(), value);
        };
        replace("{Property}", label);
        replace("{property}", name);
        replace("{limit}", std::to_string(static_cast<int>(limit)));
        content::Diagnostic diagnostic;
        diagnostic.code = content::ErrorCode::invalid_document;
        diagnostic.message = std::move(message);
        return std::unexpected(std::move(diagnostic));
    }
    bool valid_value(Value value) {
        const auto scalar = [&](f32 v) { return std::isfinite(v) && v >= minimum && v <= limit; };
        if constexpr (std::same_as<Value, f32>) return scalar(value);
        else return scalar(value.x) && scalar(value.y) && scalar(value.z);
    }
    bool valid_text(std::string_view value, std::size_t limit) {
        return value.size() <= limit && value.find('\0') == std::string_view::npos;
    }
    content::Result<void> validate(const Snapshot& snapshot, f32 duration = timeline::max_time) {
        if (!snapshot.object || !valid_value((snapshot.*Base)))
            return invalid("{Property} edit needs an object and finite values inside its allowed range");
        if (!snapshot.track) return {};
        const auto& track = *snapshot.track;
        if (track.target != timeline::Target{snapshot.object, std::string(name)})
            return invalid("{Property} edit track must address the same object's {property} property");
        if (!valid_text(track.label, timeline::max_label_bytes) ||
            !valid_text(track.layer, timeline::max_layer_bytes))
            return invalid("{Property} track label/layer must be bounded strings without NUL");
        if (track.keys.empty() || track.keys.size() > timeline::max_keys_per_track)
            return invalid("{Property} track must have 1..4096 keys");
        if (!std::isfinite(duration) || duration < .1F || duration > timeline::max_time)
            return invalid("{Property} edit requires a valid timeline duration");
        f32 previous = -1;
        for (const auto& key : track.keys) {
            const auto* value = std::get_if<Value>(&key.value);
            if (!value || !valid_value(*value))
                return invalid("{Property} track keys must have the correct type and allowed range");
            if (!std::isfinite(key.time) || key.time < 0 || key.time > duration || key.time <= previous)
                return invalid("{Property} track keys must be ordered unique times inside the timeline");
            if (key.incoming != timeline::Interpolation::hold && key.incoming != timeline::Interpolation::linear)
                return invalid("Unknown {property} key interpolation");
            previous = key.time;
        }
        return {};
    }
    content::Result<void> validate(const Edit& edit) {
        if (!edit.base_revision || edit.revision <= edit.base_revision || edit.revision > max_revision)
            return invalid("{Property} edit requires 1 <= base < target <= 2^53-1");
        return validate((edit.*Property));
    }
    Value& value_of(SceneInstance& instance) {
        return (instance.transform.*Member);
    }
    Value value_of(const SceneInstance& instance) {
        return (instance.transform.*Member);
    }
    void integer(std::string& bytes, u64 value, unsigned width) {
        for (unsigned i = 0; i < width; ++i)
            bytes.push_back(static_cast<char>((value >> (i * 8U)) & 255U));
    }
    u64 integer(std::string_view bytes, std::size_t& offset, unsigned width) {
        u64 result{};
        for (unsigned i = 0; i < width; ++i)
            result |= u64(static_cast<unsigned char>(bytes[offset++])) << (i * 8U);
        return result;
    }
    void vector(std::string& bytes, Value value) {
        if constexpr (std::same_as<Value, f32>) integer(bytes, std::bit_cast<u32>(value), 4);
        else for (const auto scalar : {value.x, value.y, value.z})
                integer(bytes, std::bit_cast<u32>(scalar), 4);
    }
    Value vector(std::string_view bytes, std::size_t& offset) {
        if constexpr (std::same_as<Value, f32>)
            return std::bit_cast<f32>(static_cast<u32>(integer(bytes, offset, 4)));
        else {
            Vec3 value;
            for (auto* scalar : {&value.x, &value.y, &value.z})
                *scalar = std::bit_cast<f32>(static_cast<u32>(integer(bytes, offset, 4)));
            return value;
        }
    }

    content::Result<Snapshot> capture(const State& state, u32 object) {
        const auto* instance = find_instance(state, object);
        if (!instance) return invalid("{Property} edit references a missing scene instance");
        Snapshot result{object, value_of(*instance), {}};
        if (const auto* track = state.document.timeline.find({object, std::string(name)})) result.track = *track;
        if (auto valid = validate(result, state.document.timeline_duration); !valid)
            return std::unexpected(valid.error());
        return result;
    }

    content::Result<void> restore(State& state, const Snapshot& snapshot) {
        auto* instance = find_instance(state, snapshot.object);
        if (!instance) return invalid("{Property} edit references a missing scene instance");
        if (auto valid = validate(snapshot, state.document.timeline_duration); !valid) return valid;
        if (snapshot.track) {
            if (auto replaced = state.document.timeline.replace_track(*snapshot.track); !replaced)
                return invalid(replaced.error().message);
        } else {
            (void)state.document.timeline.erase({snapshot.object, std::string(name)});
        }
        // The only remaining write is a nonthrowing value after the track's
        // allocation, validation and transaction commit have all succeeded.
        value_of(*instance) = (snapshot.*Base);
        return {};
    }

    content::Result<bool> apply_value(State& state, u32 object, Value value) {
        if (!at_paused_keyframe(state)) return invalid("Insert a keyframe here to edit scene properties");
        auto* instance = find_instance(state, object);
        if (!instance) return invalid("{Property} edit references a missing scene instance");
        if (!valid_value(value)) {
            if constexpr (std::same_as<Value, Vec3>)
                return invalid("{Property} values must be finite and within +/-{limit}");
            else return invalid("{Property} values must be finite and inside their allowed range");
        }
        const timeline::Target target{object, std::string(name)};
        const auto* track = state.document.timeline.find(target);
        if (!track && state.viewport.time == 0) {
            if (value_of(*instance) == value) return false;
            value_of(*instance) = value;
            return true;
        }
        if (!state.viewport.paused || state.viewport.mode != ViewMode::scene)
            return invalid("Pause scene playback before editing an animated object");
        if (!std::isfinite(state.viewport.time) || state.viewport.time < 0 || state.viewport.time > state.document.timeline_duration)
            return invalid("{Property} edit playhead must lie inside the timeline");
        auto current = value_of(*instance);
        if (const auto sampled = state.document.timeline.sample(target, state.viewport.time)) {
            const auto* typed = std::get_if<Value>(&*sampled);
            if (!typed) return invalid("{Property} animation has the wrong value type");
            current = *typed;
        }
        if (current == value) return false;
        if (!track || (state.viewport.time == 0 && track->keys.front().time > 0)) {
            if (auto keyed = key_property(state, target, state.viewport.time, value); !keyed)
                return std::unexpected(keyed.error());
            return true;
        }
        const auto key = std::ranges::lower_bound(track->keys, state.viewport.time, {}, &timeline::Keyframe::time);
        const auto incoming = key != track->keys.end() && key->time == state.viewport.time
                                  ? key->incoming : timeline::Interpolation::linear;
        // Timeline::set changes only this key. No whole-state or whole-timeline
        // staging is necessary, and an existing exact-time key needs no allocation.
        if (auto changed = state.document.timeline.set(target, {state.viewport.time, value, incoming}); !changed)
            return invalid(changed.error().message);
        return true;
    }

    content::Result<Edit> make_edit(u64 base_revision, const State& state, u32 object) {
        auto snapshot = capture(state, object);
        if (!snapshot) return std::unexpected(snapshot.error());
        Edit result{base_revision, state.document.revision, std::move(*snapshot)};
        if (auto valid = validate(result); !valid) return std::unexpected(valid.error());
        return result;
    }
    content::Result<void> apply_edit(State& state, const Edit& edit) {
        if (auto valid = validate(edit); !valid) return valid;
        if (state.document.revision != edit.base_revision)
            return invalid("Stale {property} edit base revision; resynchronize before applying");
        if (auto applied = restore(state, (edit.*Property)); !applied) return applied;
        state.document.revision = edit.revision;
        return {};
    }

    content::Result<std::string> encode(const Edit& edit) {
        if (auto valid = validate(edit); !valid) return std::unexpected(valid.error());
        const auto& track = (edit.*Property).track;
        const auto keys = track ? track->keys.size() : 0;
        std::string bytes;
        bytes.reserve(header_bytes + (track ? 8 + track->label.size() + track->layer.size() + keys * key_bytes : 0));
        bytes.append(magic.data(), magic.size());
        integer(bytes, edit.base_revision, 8);
        integer(bytes, edit.revision, 8);
        integer(bytes, (edit.*Property).object, 4);
        vector(bytes, (edit.*Property).*Base);
        integer(bytes, keys, 4);
        integer(bytes, track ? 1 : 0, 4);
        if (track) {
            integer(bytes, track->label.size(), 4);
            integer(bytes, track->layer.size(), 4);
            bytes += track->label;
            bytes += track->layer;
            for (const auto& key : track->keys) {
                integer(bytes, std::bit_cast<u32>(key.time), 4);
                vector(bytes, std::get<Value>(key.value));
                integer(bytes, key.incoming == timeline::Interpolation::hold ? 0 : 1, 4);
            }
        }
        return bytes;
    }

    content::Result<Edit> decode(std::string_view bytes) {
        if (bytes.size() < header_bytes || bytes.size() > maximum_bytes)
            return invalid("{Property} edit payload has an invalid length");
        if (bytes.substr(0, magic.size()) != std::string_view(magic.data(), magic.size()))
            return invalid("{Property} edit payload has an unknown magic or version");
        std::size_t offset = magic.size();
        Edit edit;
        edit.base_revision = integer(bytes, offset, 8);
        edit.revision = integer(bytes, offset, 8);
        (edit.*Property).object = static_cast<u32>(integer(bytes, offset, 4));
        (edit.*Property).*Base = vector(bytes, offset);
        const auto count = integer(bytes, offset, 4);
        const auto present = integer(bytes, offset, 4);
        if (present > 1 || count > timeline::max_keys_per_track || (present == 0) != (count == 0))
            return invalid("{Property} edit has an invalid track flag or key count");
        if (present) {
            if (bytes.size() < header_bytes + 8) return invalid("{Property} track metadata header is truncated");
            const auto label_bytes = integer(bytes, offset, 4), layer_bytes = integer(bytes, offset, 4);
            if (label_bytes > timeline::max_label_bytes || layer_bytes > timeline::max_layer_bytes ||
                bytes.size() != header_bytes + 8 + label_bytes + layer_bytes + count * key_bytes)
                return invalid("{Property} edit metadata/key sizes do not match its bounded payload");
            timeline::Track track{{(edit.*Property).object, std::string(name)},
                                   std::string(bytes.substr(offset, label_bytes)),
                                   std::string(bytes.substr(offset + label_bytes, layer_bytes)), {}};
            offset += static_cast<std::size_t>(label_bytes + layer_bytes);
            track.keys.reserve(static_cast<std::size_t>(count));
            for (u64 i = 0; i < count; ++i) {
                const auto time = std::bit_cast<f32>(static_cast<u32>(integer(bytes, offset, 4)));
                const auto value = vector(bytes, offset);
                const auto incoming = integer(bytes, offset, 4);
                if (incoming > 1) return invalid("Unknown {property} key interpolation");
                track.keys.push_back({time, value, incoming == 0 ? timeline::Interpolation::hold : timeline::Interpolation::linear});
            }
            (edit.*Property).track = std::move(track);
        } else if (bytes.size() != header_bytes) {
            return invalid("Unkeyed {property} edit has unexpected trailing bytes");
        }
        if (auto valid = validate(edit); !valid) return std::unexpected(valid.error());
        return edit;
    }

};
} // namespace editor_example::detail
