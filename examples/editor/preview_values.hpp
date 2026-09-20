#pragma once

#include "animation.hpp"

namespace editor_example {
struct MeshPreview {
    MeshTarget target;
    MeshSettings settings;
    InstanceTransform transform;
};
// Geometry is resolved by blueprint identity; only small appearance/placement
// metadata is sampled. Inspecting an asset does not require a fake instance.
[[nodiscard]] inline std::optional<MeshPreview> preview_mesh(const State& state, vng::f32 time) {
    const auto target = mesh_target(state);
    if (!target) return {};
    if (target->instance) {
        const auto* source = find_instance(state, *target->instance);
        if (!source) return {};
        const auto values = evaluate_instance(state, *source, time);
        return MeshPreview{*target, std::get<MeshSettings>(values.settings), values.transform};
    }
    const auto* defaults = mesh_blueprint_settings(state, target->blueprint);
    if (!defaults) return {};
    auto settings = *defaults;
    settings.visible = true;
    return MeshPreview{*target, settings, {}};
}

// Blueprint editing is independent of scene placement, visibility and keys.
// This copies only small instance metadata, never geometry or the timeline.
[[nodiscard]] inline SceneInstance preview_instance(const State& state,
                                                     const SceneInstance& source,
                                                     vng::f32 time) {
    if (state.viewport.mode != ViewMode::mesh || !std::holds_alternative<MeshSettings>(source.settings))
        return evaluate_instance(state, source, time);
    auto result = source;
    result.transform = {};
    const auto* defaults = mesh_blueprint_settings(state, source.blueprint);
    auto appearance = defaults ? *defaults : MeshSettings{};
    // A hidden instance (or hidden-by-default asset) must remain editable.
    appearance.visible = true;
    result.settings = appearance;
    return result;
}
} // namespace editor_example
