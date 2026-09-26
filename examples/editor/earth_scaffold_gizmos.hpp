#pragma once
#include "blueprint_mesh_controls.hpp"
#include "../support/earth_connections.hpp"

namespace editor_example {
vng::editor::Result<void> append_scaffold_gizmos(BlueprintMeshDescription&,
    const example::earth::InfrastructurePart&,const example::earth::TunnelCurve&,vng::Mat4);
// Reused by standalone structures' whole-part gizmos.
void describe_scaffold_visibility(vng::editor::Inspector&,SubmitMeshDraftEdit,
    const example::earth::InfrastructurePart&);
}
