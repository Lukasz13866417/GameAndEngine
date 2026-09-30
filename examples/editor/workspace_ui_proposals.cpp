#include "workspace_ui.hpp"
#include "blueprint_mesh_panel.hpp"
#include "region_editor.hpp"

namespace editor_example {

MeshEditingReply EditingWorkspaceUI::poll_blueprint_controls(bool enabled) {
    auto& panel=blueprint_panel_child();
    const auto before=panel.selected_part();
    (void)panel.poll(enabled);
    const auto result=apply_pending(panel);
    MeshEditingReply reply;
    if(!result)reply.message=result.error().message;
    else if(*result){reply.authored=true;refresh_vertex();}
    if((panel.selected_part()!=before&&panel.selected_part())||panel.placing())
        merge(reply,dispatch(*this,workspace_situation(viewport()),WorkspaceContext{.mesh=MeshInput{.mode=MeshSelectMode::surface}}).mesh);
    if(!panel.status().empty()&&panel.status()!=last_blueprint_status_) {
        last_blueprint_status_=panel.status();reply.message=last_blueprint_status_;
    }
    return reply;
}
vng::content::Result<bool> EditingWorkspaceUI::apply_pending(BlueprintMeshPanel& panel) {
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
vng::content::Result<bool> EditingWorkspaceUI::apply_pending(RegionEditor& panel) {
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
