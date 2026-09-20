#pragma once

#include <vng/content/diagnostic.hpp>
#include <vng/timeline/timeline.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace editor_example {
struct State;

// Euler XYZ degrees bounded to +/-360, matching scene transforms.
// Only the authored data owned by one rotation property. No geometry, other
// instance settings, or unrelated animation tracks are copied into a drag.
struct RotationSnapshot {
    vng::u32 object{};
    vng::Vec3 base_rotation{};
    std::optional<vng::timeline::Track> track;
    friend bool operator==(const RotationSnapshot&, const RotationSnapshot&) = default;
};
struct RotationEdit {
    vng::u64 base_revision{}, revision{};
    RotationSnapshot rotation;
    friend bool operator==(const RotationEdit&, const RotationEdit&) = default;
};

[[nodiscard]] vng::content::Result<RotationSnapshot> capture_rotation(const State&, vng::u32 object);
// These local mutations leave revision, undo and dirty tracking to the caller.
[[nodiscard]] vng::content::Result<void> restore_rotation(State&, const RotationSnapshot&);
// Returns false for a no-op. Existing animation edits a key at the playhead,
// preserving its incoming interpolation; an unkeyed property edits its base.
[[nodiscard]] vng::content::Result<bool> apply_rotation_value(State&, vng::u32 object, vng::Vec3);

[[nodiscard]] vng::content::Result<RotationEdit>
rotation_edit(vng::u64 base_revision, const State&, vng::u32 object);
[[nodiscard]] vng::content::Result<void> apply_rotation_edit(State&, const RotationEdit&);

// LE binary v1: 48-byte header (magic/version, base/target revisions, object,
// XYZ, key count, track-present flag). A track adds two string lengths, label,
// layer, and 20 bytes per {time, XYZ, incoming}. No textual scene serialization.
[[nodiscard]] vng::content::Result<std::string> encode_rotation_edit(const RotationEdit&);
[[nodiscard]] vng::content::Result<RotationEdit> decode_rotation_edit(std::string_view);
} // namespace editor_example
