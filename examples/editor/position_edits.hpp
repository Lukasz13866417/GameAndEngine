#pragma once

#include <vng/content/diagnostic.hpp>
#include <vng/timeline/timeline.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace editor_example {
struct State;

// Only the authored data owned by one position property. No geometry, other
// instance settings, or unrelated animation tracks are copied into a drag.
struct PositionSnapshot {
    vng::u32 object{};
    vng::Vec3 base_position{};
    std::optional<vng::timeline::Track> track;
    friend bool operator==(const PositionSnapshot&, const PositionSnapshot&) = default;
};
struct PositionEdit {
    vng::u64 base_revision{}, revision{};
    PositionSnapshot position;
    friend bool operator==(const PositionEdit&, const PositionEdit&) = default;
};

[[nodiscard]] vng::content::Result<PositionSnapshot> capture_position(const State&, vng::u32 object);
// These local mutations leave revision, undo and dirty tracking to the caller.
[[nodiscard]] vng::content::Result<void> restore_position(State&, const PositionSnapshot&);
// Returns false for a no-op. Existing animation edits a key at the playhead,
// preserving its incoming interpolation; an unkeyed property edits its base.
[[nodiscard]] vng::content::Result<bool> apply_position_value(State&, vng::u32 object, vng::Vec3);
// The same for a position where the instance should show at the playhead, as
// gizmos and drags give it: an orbiting camera stores the focus point its eye
// there looks at.
[[nodiscard]] vng::content::Result<bool> apply_placed_position(State&, vng::u32 object, vng::Vec3 placed);

[[nodiscard]] vng::content::Result<PositionEdit>
position_edit(vng::u64 base_revision, const State&, vng::u32 object);
[[nodiscard]] vng::content::Result<void> apply_position_edit(State&, const PositionEdit&);

// LE binary v1: 48-byte header (magic/version, base/target revisions, object,
// XYZ, key count, track-present flag). A track adds two string lengths, label,
// layer, and 20 bytes per {time, XYZ, incoming}. No textual scene serialization.
[[nodiscard]] vng::content::Result<std::string> encode_position_edit(const PositionEdit&);
[[nodiscard]] vng::content::Result<PositionEdit> decode_position_edit(std::string_view);
} // namespace editor_example
