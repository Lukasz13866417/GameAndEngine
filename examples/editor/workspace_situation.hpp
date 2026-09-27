#pragma once
#include "project.hpp"

namespace editor_example {
struct InspectScene {};
struct InspectMesh { BlueprintId blueprint; };
struct InspectEffect {};
using WorkspaceSituation = std::variant<InspectScene, InspectMesh, InspectEffect>;

// The persisted view enum is a file/protocol value. Convert once at the owning
// boundary; descendants receive a concrete situation, not another mode switch.
inline WorkspaceSituation workspace_situation(const ViewportState& view) {
    switch (view.mode) {
    case ViewMode::scene: return InspectScene{};
    case ViewMode::mesh: return InspectMesh{view.inspected_mesh};
    case ViewMode::sun: return InspectEffect{};
    }
    return InspectScene{};
}
} // namespace editor_example
