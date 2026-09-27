#include "workspace.hpp"
#include "blueprint_mesh_panel.hpp"
#include "region_editor.hpp"

namespace editor_example {
vng::content::Result<bool> EditingWorkspace::apply_pending(BlueprintMeshPanel& panel) {
    bool changed{};
    std::optional<vng::content::Diagnostic> error;
    while(auto proposal=panel.take_edit()) {
        auto result=apply_blueprint_mesh_edit(editing_,*proposal);
        panel.accept_edit(*proposal,result);
        if(result)changed|=*result;
        else if(!error)error=result.error();
        // An acknowledgement can enqueue commit/cancel. Drain it even after
        // failure so the gesture does not leave the workspace captured.
    }
    if(error)return std::unexpected(std::move(*error));
    return changed;
}
vng::content::Result<bool> EditingWorkspace::apply_pending(RegionEditor& panel) {
    bool changed{};
    std::optional<vng::content::Diagnostic> error;
    while(auto proposal=panel.take_edit()) {
        auto result=apply_region_edit(editing_,*proposal);
        panel.accept_edit(*proposal,result,editing_.state());
        if(result)changed|=result->changed;
        else if(!error)error=result.error();
    }
    if(error)return std::unexpected(std::move(*error));
    return changed;
}
}
