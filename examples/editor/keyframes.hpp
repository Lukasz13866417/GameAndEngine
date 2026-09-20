#pragma once
#include "animation.hpp"
#include <array>

namespace editor_example {
// A scene keyframe is a view of property keys at one timestamp. The existing
// sparse track representation/file format remains authoritative.
using KeyframeValue = PropertyKey;
// A field-level inspector edit, not a snapshot of the source pose. Vector
// components and interpolation are independent so range edits preserve the rest.
struct KeyframeChange {
    vng::timeline::Target target;
    std::optional<vng::timeline::Value> value{};
    std::array<bool, 3> components{true, true, true};
    std::optional<bool> keyed{};
    std::optional<vng::timeline::Interpolation> incoming{};
};
[[nodiscard]] std::optional<KeyframeChange> keyframe_change(const KeyframeValue& before,
                                                           const KeyframeValue& after);
// Inclusive range of existing scene keyframes; atomic, without changing names
// or timestamps. Unchanged components are sampled from the original animation.
[[nodiscard]] vng::content::Result<void> apply_keyframe_range(State&, vng::f32 first, vng::f32 last,
                                                            std::span<const KeyframeChange>);
// Zero is an implicit, permanent initial pose. Sparse tracks need no migration
// or extra zero keys; their existing evaluation remains authoritative.
[[nodiscard]] bool is_keyframe(const State&, vng::f32);
// Data-level validity for low-level authoring/worker operations. Interactive
// permission additionally requires EditingSession's explicit keyframe selection.
[[nodiscard]] bool at_paused_keyframe(const State&);
[[nodiscard]] std::vector<vng::f32> keyframe_times(const vng::timeline::Timeline&);
[[nodiscard]] std::vector<vng::f32> keyframe_times(const State&);
[[nodiscard]] std::string keyframe_name(const State&, vng::f32 time);
[[nodiscard]] std::vector<KeyframeValue> keyframe_values(const State&, vng::f32 time);
// Inspection needs property/animation data only, not a copy of scene geometry.
[[nodiscard]] std::vector<KeyframeValue> keyframe_values(
    std::span<const AnimationProperty>, const vng::timeline::Timeline&, vng::f32 time);
// Capture the evaluated state at the insertion time, preserving the current pose.
// An existing timestamp is selected without changing its values.
[[nodiscard]] vng::content::Result<void> add_keyframe(State&, vng::f32 time);
[[nodiscard]] vng::content::Result<void> edit_keyframe(State&, vng::f32 time,
                                                       std::span<const KeyframeValue>);
[[nodiscard]] vng::content::Result<void> erase_keyframe(State&, vng::f32 time);
[[nodiscard]] vng::content::Result<void> move_keyframe(State&, vng::f32 from, vng::f32 to);
// One transaction for the inspector's timestamp, custom name and property edits.
[[nodiscard]] vng::content::Result<void> update_keyframe(State&, vng::f32 from, vng::f32 to,
                                                         std::string name,
                                                         std::span<const KeyframeValue>);
} // namespace editor_example
