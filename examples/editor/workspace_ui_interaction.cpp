#include "workspace_ui.hpp"
#include "viewport_tools_ui.hpp"
#include "instance_controls.hpp"

namespace editor_example {

void EditingViewportUI::attach_tools(vng::ui::Container blueprint,vng::ui::Container controls,vng::ui::Container creation,
                                  vng::ui::Container inspector,vng::ui::Container popup) {
    if(blueprint_panel_||interaction_)throw std::logic_error("Viewport tools are already attached");
    blueprint_panel_.emplace(blueprint,editing_);
    interaction_.emplace(editing_,controls,creation,inspector,popup);
}

void EditingWorkspaceUI::attach_viewport_tools(vng::ui::Container blueprint,vng::ui::Container controls,vng::ui::Container creation,
                                           vng::ui::Container inspector,vng::ui::Container popup) {
    if(!viewport_)throw std::logic_error("Workspace UI has not been initialized");
    viewport_->attach_tools(blueprint,controls,creation,inspector,popup);
    synchronize_selection_gizmos(false);
}

BlueprintMeshPanel& EditingWorkspaceUI::blueprint_panel() {
    if(!viewport_||!viewport_->blueprint_panel_)throw std::logic_error("Viewport tools have not been attached");
    return *viewport_->blueprint_panel_;
}

ViewportToolsUI& EditingWorkspaceUI::interaction() {
    if(!viewport_||!viewport_->interaction_)throw std::logic_error("Viewport tools have not been attached");
    return *viewport_->interaction_;
}

void EditingWorkspaceUI::attach_manipulation(vng::ui::Container gizmo,vng::ui::Container pivot) {
    if(!viewport_)throw std::logic_error("Workspace UI has not been initialized");
    if(viewport_->gizmo_selector_||viewport_->rotation_pivot_)throw std::logic_error("Viewport manipulation controls are already attached");
    viewport_->gizmo_selector_.emplace(gizmo);
    viewport_->rotation_pivot_.emplace(pivot);
    synchronize_selection_gizmos(false);
}

GizmoSelector& EditingWorkspaceUI::gizmo_selector() {
    if(!viewport_||!viewport_->gizmo_selector_)throw std::logic_error("Viewport manipulation controls have not been attached");
    return *viewport_->gizmo_selector_;
}

RotationPivotControls& EditingWorkspaceUI::rotation_pivot() {
    if(!viewport_||!viewport_->rotation_pivot_)throw std::logic_error("Viewport manipulation controls have not been attached");
    return *viewport_->rotation_pivot_;
}

void EditingWorkspaceUI::begin_viewport_frame() {interaction().begin_frame();}

vng::content::Result<bool> EditingWorkspaceUI::finish(
    ViewportToolsUI& interaction,ViewportTool tool,bool cancelled) {
    if(interaction.active()!=tool||!editing_.busy()||editing_.awaiting_remote())return false;
    auto result=cancelled?editing_.cancel():editing_.commit();
    if(result)interaction.finished(tool);
    return result;
}

vng::content::Result<bool> EditingWorkspaceUI::cancel(ViewportToolsUI& interaction) {
    bool changed{};
    if(interaction.busy()&&editing_.busy()&&!editing_.awaiting_remote()) {
        auto restored=editing_.cancel();
        if(!restored)return std::unexpected(restored.error());
        changed=*restored;
    }
    interaction.reset();
    return changed;
}

vng::content::Result<InstanceTransformChange> EditingWorkspaceUI::execute(
    InstanceTransformGizmo& tool,vng::content::Result<InstanceTransformProposal> proposal) {
    return editor_example::execute(editing_,tool,std::move(proposal));
}

vng::content::Result<bool> EditingWorkspaceUI::execute(
    MeshTransformGizmo& tool,vng::content::Result<MeshTransformProposal> proposal) {
    return editor_example::execute(editing_,tool,std::move(proposal));
}

vng::content::Result<RotationChange> EditingWorkspaceUI::execute(
    InstanceRotationGizmo& tool,vng::content::Result<RotationProposal> proposal) {
    return editor_example::execute(editing_,tool,std::move(proposal));
}

vng::content::Result<std::optional<bool>> EditingWorkspaceUI::apply_instance_control(
    const vng::editor::Event& event,vng::u64 generation,vng::u64 minimum_context) {
    return editor_example::apply_instance_control(editing_,event,generation,minimum_context);
}

} // namespace editor_example
