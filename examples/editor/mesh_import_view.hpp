#pragma once

#include "project.hpp"

namespace editor_example {
// Import focuses the new asset in an isolated inspection view. Its native
// coordinates, scene transform, and the scene's cameras stay untouched.
inline void inspect_imported_mesh(State& state) {
    const auto* instance = find_instance(state, state.viewport.selected_object);
    if (instance && is_mesh_instance(state, instance->id))
        (void)inspect_mesh(state, instance->blueprint);
}
} // namespace editor_example
