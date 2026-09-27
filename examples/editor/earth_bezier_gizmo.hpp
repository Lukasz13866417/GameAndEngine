#pragma once
#include "blueprint_mesh_controls.hpp"
#include "../support/earth_connections.hpp"

namespace editor_example {
void append_bezier_gizmo(BlueprintMeshDescription&,const example::earth::InfrastructurePart&,
    const example::earth::TunnelCurve&,vng::Mat4);
}
