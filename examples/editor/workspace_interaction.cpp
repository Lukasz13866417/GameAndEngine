#include "workspace.hpp"
#include "viewport_interaction.hpp"
#include "instance_controls.hpp"

namespace editor_example {

void EditingViewport::attach_tools(vng::ui::Container blueprint,vng::ui::Container controls,vng::ui::Container creation,
                                  vng::ui::Container inspector,vng::ui::Container popup) {
    if(blueprint_panel_||interaction_)throw std::logic_error("Viewport tools are already attached");
    blueprint_panel_.emplace(blueprint,editing_);
    interaction_.emplace(editing_,controls,creation,inspector,popup);
}

void EditingWorkspace::attach_viewport_tools(vng::ui::Container blueprint,vng::ui::Container controls,vng::ui::Container creation,
                                           vng::ui::Container inspector,vng::ui::Container popup) {
    if(!viewport_)throw std::logic_error("Workspace UI has not been initialized");
    viewport_->attach_tools(blueprint,controls,creation,inspector,popup);
    synchronize_selection_gizmos(false);
}

BlueprintMeshPanel& EditingWorkspace::blueprint_panel() {
    if(!viewport_||!viewport_->blueprint_panel_)throw std::logic_error("Viewport tools have not been attached");
    return *viewport_->blueprint_panel_;
}

ViewportInteraction& EditingWorkspace::interaction() {
    if(!viewport_||!viewport_->interaction_)throw std::logic_error("Viewport tools have not been attached");
    return *viewport_->interaction_;
}

void EditingWorkspace::attach_manipulation(vng::ui::Container gizmo,vng::ui::Container pivot) {
    if(!viewport_)throw std::logic_error("Workspace UI has not been initialized");
    if(viewport_->gizmo_selector_||viewport_->rotation_pivot_)throw std::logic_error("Viewport manipulation controls are already attached");
    viewport_->gizmo_selector_.emplace(gizmo);
    viewport_->rotation_pivot_.emplace(pivot);
    synchronize_selection_gizmos(false);
}

GizmoSelector& EditingWorkspace::gizmo_selector() {
    if(!viewport_||!viewport_->gizmo_selector_)throw std::logic_error("Viewport manipulation controls have not been attached");
    return *viewport_->gizmo_selector_;
}

RotationPivotControls& EditingWorkspace::rotation_pivot() {
    if(!viewport_||!viewport_->rotation_pivot_)throw std::logic_error("Viewport manipulation controls have not been attached");
    return *viewport_->rotation_pivot_;
}

void EditingWorkspace::begin_viewport_frame() {interaction().begin_frame();}

vng::content::Result<bool> EditingWorkspace::finish(
    ViewportInteraction& interaction,ViewportTool tool,bool cancelled) {
    if(interaction.active()!=tool||!editing_.busy()||editing_.awaiting_remote())return false;
    auto result=cancelled?editing_.cancel():editing_.commit();
    if(result)interaction.finished(tool);
    return result;
}

vng::content::Result<bool> EditingWorkspace::cancel(ViewportInteraction& interaction) {
    bool changed{};
    if(interaction.busy()&&editing_.busy()&&!editing_.awaiting_remote()) {
        auto restored=editing_.cancel();
        if(!restored)return std::unexpected(restored.error());
        changed=*restored;
    }
    interaction.reset();
    return changed;
}

vng::content::Result<InstanceTransformChange> EditingWorkspace::execute(
    InstanceTransformInteraction& tool,vng::content::Result<InstanceTransformProposal> proposal) {
    return editor_example::execute(editing_,tool,std::move(proposal));
}

vng::content::Result<bool> EditingWorkspace::execute(
    MeshTransform& tool,vng::content::Result<MeshTransformProposal> proposal) {
    return editor_example::execute(editing_,tool,std::move(proposal));
}

vng::content::Result<RotationChange> EditingWorkspace::execute(
    RotationInteraction& tool,vng::content::Result<RotationProposal> proposal) {
    return editor_example::execute(editing_,tool,std::move(proposal));
}

vng::content::Result<std::optional<bool>> EditingWorkspace::apply_instance_control(
    const vng::editor::Event& event,vng::u64 generation,vng::u64 minimum_context) {
    return editor_example::apply_instance_control(editing_,event,generation,minimum_context);
}

} // namespace editor_example
