#pragma once

#include "project.hpp"
#include <vng/editor/inspector.hpp>

namespace editor_example {
// CPU picking in normalized top-left viewport coordinates. Returns the nearest
// visible scene entity: 1 = mesh, 2 = sun; background returns nullopt. Meshes
// use their actual triangles, with the same two-sided policy as the editor
// renderer. The sun uses its displaced body's bounding sphere: its transparent
// corona, bloom and prominence strands deliberately do not intercept clicks.
// Call against the state/extent that produced the displayed image.
struct PickStats {
    std::size_t mesh_instances{};
    vng::spatial::QueryStats geometry;
};
[[nodiscard]] std::optional<vng::u32> pick_object(const State&, vng::Vec2, vng::Extent2D,
    const vng::gfx::Camera* = nullptr, PickStats* = nullptr);
// Visible instance origins, normalized top-left coordinates. This remains O(n)
// in instances, independent of triangle count, for box selection/selection marks.
struct InstancePoint {
    vng::u32 object; vng::Vec2 point;
    friend bool operator==(const InstancePoint&, const InstancePoint&) = default;
};
[[nodiscard]] std::vector<InstancePoint> instance_points(const State&, vng::Extent2D, const vng::gfx::Camera&);

// Viewport-owned projected origins, not another copy of the scene. Selection
// changes in Scene view can reuse this; authored changes, playback, camera/lens
// and view changes cannot. Document revisions are supplied by EditingSession.
class InstanceProjection {
public:
    [[nodiscard]] std::span<const InstancePoint> get(const State&, vng::Extent2D, const vng::gfx::Camera&);
    [[nodiscard]] vng::u64 rebuilds() const { return rebuilds_; }
private:
    struct Key {
        const State* state{};
        vng::u64 revision{};
        vng::f32 time{};
        ViewMode mode{};
        vng::u32 isolated_object{};
        vng::Extent2D extent{};
        vng::Mat4 view_projection{};
        friend bool operator==(const Key&, const Key&) = default;
    };
    std::optional<Key> key_;
    std::vector<InstancePoint> points_;
    vng::u64 rebuilds_{};
};

// Worker controls describe base settings. The viewport gizmo instead sits at
// the evaluated position, including animation. Other controls stay untouched.
[[nodiscard]] vng::editor::Schema selection_gizmo_schema(const State&,
                                                        const vng::editor::Schema&);
// The built-in instance position is already known locally. Its viewport
// affordance does not need a worker inspector round trip after selection.
// This description feeds local editing-session gestures, never native callbacks.
[[nodiscard]] vng::editor::Schema local_position_gizmo(const State&, vng::u64 generation);
// A translated animated object edits/adds its position key at the playhead,
// never its ineffective base position. True means handled locally; false means
// the ordinary worker callback should handle this event. Existing incoming
// interpolation is retained. The caller owns undo, revision and dirty state.
[[nodiscard]] vng::content::Result<bool>
apply_animated_translation(State&, const vng::editor::Event&);
} // namespace editor_example
