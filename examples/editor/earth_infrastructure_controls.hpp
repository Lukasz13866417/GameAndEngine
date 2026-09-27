#pragma once
#include "blueprint_mesh_controls.hpp"
namespace editor_example {
vng::editor::Result<void> append_infrastructure_menus(BlueprintMeshDescription&,const vng::content::vmesh::Document&);
vng::editor::Result<void> append_infrastructure_parts(BlueprintMeshDescription&,const vng::content::vmesh::Document&);
vng::editor::Result<bool> describe_infrastructure_parts(vng::editor::Inspector&,BlueprintMeshDescription&,
    const vng::content::vmesh::Document&,SubmitMeshDraftEdit,const MeshPartId&);
}
