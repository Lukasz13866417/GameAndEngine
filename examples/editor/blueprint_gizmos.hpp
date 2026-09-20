#pragma once

#include "project.hpp"
#include "gizmo_mode.hpp"
#include <vng/editor/inspector.hpp>

namespace editor_example {
// One authoring adapter describes the tools a blueprint supplies. Selection
// intersects these descriptions; the viewport routes boundary editing from the
// same description instead of testing concrete blueprint identities.
enum class ManipulationSurface { object, boundary };
struct BlueprintManipulation {
    ManipulationSurface surface{ManipulationSurface::object};
    std::vector<GizmoMode> gizmos{basic_transform_gizmos.begin(), basic_transform_gizmos.end()};
    std::optional<vng::Vec3> forward;
    std::optional<std::array<vng::Vec3, 3>> attitude;
};
[[nodiscard]] BlueprintManipulation blueprint_manipulation(const State&, BlueprintId);
// Stable capability identities, intersected over the complete selection. Axes
// may differ between blueprints; yaw/pitch/roll retain their semantic meaning.
[[nodiscard]] std::vector<GizmoMode> selection_gizmos(const State&, std::span<const vng::u32>);
// Local yaw(up), pitch(right), roll(forward) axes. Both metadata directions
// must exist and be perpendicular; absent/invalid metadata grants no capability.
[[nodiscard]] std::optional<std::array<vng::Vec3,3>> blueprint_attitude_axes(const State&, BlueprintId);
// Blueprint metadata -> world-space handles for an already evaluated instance.
// Shared by local selection feedback and worker-side inspector descriptions.
// Reads only the applied blueprint; no mesh copies, GPU resources or callbacks.
[[nodiscard]] std::vector<vng::editor::TranslationAxis>
blueprint_translation_axes(const State&, const SceneInstance& evaluated);
// Strip non-common blueprint handles, including when an old worker schema is
// still in flight. Move retains its compatible extras; Forward shows only them.
void filter_translation_gizmos(vng::editor::Schema&, std::span<const GizmoMode> common);
}
