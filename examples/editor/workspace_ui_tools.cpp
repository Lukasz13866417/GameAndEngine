#include "workspace_ui.hpp"
#include "animation.hpp"

namespace editor_example {
using namespace vng;
// Keyframe rows follow the finished pose here. The inspector is rebuilt by the
// host from the worker's latest schema (pose_finished); schemas that arrived
// during the capture were intentionally not shown, so no cached copy is current.
void EditingWorkspaceUI::refresh_after_pose() {
    if(timeline_)(void)dispatch(*this,workspace_situation(viewport()),WorkspaceContext{.timeline=TimelineContext{.input={.synchronize=true}}});
}
WorkspaceFeedback EditingWorkspaceUI::finish_interaction(ViewportTool tool,bool cancelled) {
    WorkspaceFeedback reply;auto& tools=interaction_child();
    if(tool==ViewportTool::rotation&&cancelled) {
        auto result=execute(tools.rotation,tools.rotation.cancel());
        if(!result)reply.message=result.error().message;
        else if(result->finished) {
            refresh_after_pose();reply.pose_finished=true;
            reply.message="Rotation cancelled / original transform restored";
        }
        return reply;
    }
    const auto gesture=tool==ViewportTool::translation?EditGesture::move:tool==ViewportTool::scale?EditGesture::scale:
        tool==ViewportTool::bounds?EditGesture::world_bounds:tool==ViewportTool::rotation?EditGesture::rotation:EditGesture::camera;
    if(!editing_.active(gesture))return reply;
    const auto object=editing_.active_object();
    auto result=finish(tools,tool,cancelled);
    if(!result){reply.message=result.error().message;return reply;}
    if(cancelled) {
        if(tool==ViewportTool::translation)tools.translation.cancel();
        if(tool==ViewportTool::scale) {
            tools.scale.cancel();
            if(const auto* instance=find_instance(state(),object))
                reset_inspector_scale(evaluate_instance(state(),*instance,viewport().time).transform.scale);
        }
        if(tool==ViewportTool::bounds)tools.bounds.cancel();
        if(tool==ViewportTool::navigation) {
            (void)dispatch(tools,ViewportToolsUI::Navigate{},NavigationContext{.cancel=true});
            sync_camera_controls(true);reply.camera_changed=true;
        }
    }
    if(tool==ViewportTool::bounds)sync_bounds();
    if(tool==ViewportTool::rotation)tools.rotation.reset();
    refresh_after_pose();reply.pose_finished=true;
    const std::string name=tool==ViewportTool::translation?"Move":tool==ViewportTool::scale?"Scale":tool==ViewportTool::bounds?"World bounds":tool==ViewportTool::rotation?"Rotation":"Camera";
    reply.message=name+(cancelled?" cancelled / original values restored":*result?" updated / Undo restores the whole edit":" unchanged");
    return reply;
}
WorkspaceFeedback EditingWorkspaceUI::cancel_interaction() {
    auto& tools=interaction_child();
    if(tools.pivot.dragging()&&viewport_->rotation_pivot_)rotation_pivot_child().cancel();
    const bool camera=editing_.active(EditGesture::camera);
    const auto object=editing_.active_object();
    WorkspaceFeedback reply;auto result=cancel(tools);
    if(!result){reply.message=result.error().message;return reply;}
    sync_bounds();
    if(const auto* instance=find_instance(state(),object))
        reset_inspector_scale(evaluate_instance(state(),*instance,viewport().time).transform.scale);
    if(camera){sync_camera_controls(true);reply.camera_changed=true;}
    if(*result){refresh_after_pose();refresh_vertex();reply.pose_finished=true;}
    return reply;
}
WorkspaceFeedback EditingWorkspaceUI::edit_instance_scale(f32 value,bool finish_edit) {
    WorkspaceFeedback reply;
    if(!editing_.active(EditGesture::scale)) {
        auto begun=editing_.begin_scale(viewport().selected_object);
        if(!begun){reply.message=begun.error().message;return reply;}
    }
    if(auto result=editing_.scale(value);!result) {
        reply=finish_interaction(ViewportTool::scale,true);reply.message=result.error().message;
    } else if(finish_edit)reply=finish_interaction(ViewportTool::scale,false);
    return reply;
}
void EditingWorkspaceUI::clear_mesh_part() {
    (void)blueprint_panel_child().select_part({});interaction_child().mesh_part.cancel();
}
void EditingWorkspaceUI::reset_move_tool(){interaction_child().translation.cancel();}
void EditingWorkspaceUI::reset_view_tools() {
    interaction_child().translation.cancel();
    (void)dispatch(interaction_child(),ViewportToolsUI::Navigate{},NavigationContext{.cancel=true});
}
void EditingWorkspaceUI::focus_keyframes(bool show_inspector) {
    if(viewport_&&viewport_->interaction_)interaction_child().regions.deselect();
    keyboard_target_=SelectionTarget::keyframes;
    if(show_inspector)show_tab(SidebarTab::keyframe);
}
content::Result<bool> EditingWorkspaceUI::poll_region_menu(std::span<const input::Event> events) {
    auto& regions=interaction_child().regions;regions.poll_menu(editing_,events);return apply_pending(regions);
}
bool EditingWorkspaceUI::erase_blueprint_handle(){return blueprint_panel_child().erase_handle();}
void EditingWorkspaceUI::choose_gizmo(GizmoMode mode){gizmo_selector_child().value(mode);invalidate_gizmos();}
void EditingWorkspaceUI::choose_camera_gizmo(CameraGizmoMode mode){interaction_child().select_camera(mode);}
WorkspaceFeedback EditingWorkspaceUI::poll_gizmos(int cycle) {
    auto& panel=blueprint_panel_child();auto& gizmo=gizmo_selector_child();
    WorkspaceFeedback reply;bool cycled{};
    for(int i=0;i<std::abs(cycle);++i) {
        if(viewport().mode==ViewMode::mesh&&!panel.connecting()) {
            const bool part=mesh_components().mode()!=MeshSelectMode::whole&&panel.has_gizmo();
            const bool changed=part?panel.cycle_gizmo(cycle>0?1:-1):
                dispatch(*this,workspace_situation(viewport()),WorkspaceContext{.mesh=MeshInput{.cycle_gizmo=cycle>0?1:-1}}).mesh.gizmo_changed;
            if(changed)invalidate_gizmos();
        } else cycled|=gizmo.cycle(cycle>0?1:-1);
    }
    if(gizmo.changedValue()||cycled) {
        reply=cancel_interaction();invalidate_gizmos();
        if(reply.message.empty())reply.message=gizmo.value()==GizmoMode::scale?
            "Scale: drag the gold square / Escape cancels":gizmo.value()==GizmoMode::attitude?
            "Yaw / pitch / roll: turn around the ship axes / Escape cancels":gizmo.value()==GizmoMode::forward?
            "Forward / back: move along the active ship's forward axis / Escape cancels":rotation_mode(gizmo.value())?
            "Rotate: use the rings / Escape cancels":"Move: drag a colored axis / Escape cancels";
    }
    return reply;
}
CameraPreferenceEdit EditingWorkspaceUI::poll_camera_gizmo(const Settings& settings){return interaction_child().poll_camera(settings);}
void EditingWorkspaceUI::present_camera_gizmo(CameraGizmo::Presentation p,const Settings& settings) {
    // The viewport adds its own object gizmos; the camera-action overlay is a
    // sibling target only this workspace sees.
    p.other_gizmo=camera_ui().overlay_shown()&&!camera_ui().visiting();
    const auto target=camera_target_label();p.target=target;
    interaction_child().present_camera(p,settings);
}
void EditingWorkspaceUI::present_tool_options(ui::Rect bounds,bool enabled,bool diagnostic) {
    auto& tools=interaction_child();auto& panel=blueprint_panel_child();
    const auto route=[&](ToolPanelInput input){return dispatch(*this,workspace_situation(viewport()),WorkspaceContext{.tools=input});};
    route({.context={.validate=true}});
    auto* options=tools.instances.tool_options();
    const bool mesh=viewport().mode==ViewMode::mesh;
    const bool part=mesh&&mesh_components().mode()!=MeshSelectMode::whole&&panel.options_available();
    if(!options&&part)options=&panel;
    if(!options)options=tools.mesh.tool_options();
    if(!options)options=tools.regions.tool_options();
    const bool suspended=tools.object_tools_suspended();
    if(suspended){options=nullptr;route({.context={.close=true}});}
    const bool captured=tools.transforming();
    const bool visible=enabled&&!suspended&&!diagnostic&&
        (mesh?tools.mesh.visible()||tools.mesh_part.visible()||part:
            tools.translation.visible()||tools.rotation.visible()||tools.scale.visible()||tools.regions.tool().gizmo_visible()||
            tools.pivot.visible()||tools.bounds.handle(0)||tools.bounds.handle(1));
    const bool scale=tools.instances.active()?tools.instances.gizmo()==GizmoMode::scale:
        mesh?mesh_components().transform_mode()==GizmoMode::scale:tools.regions.selected()?
        tools.regions.tool().transform_mode()==GizmoMode::scale:gizmo_selector_child().value()==GizmoMode::scale;
    const bool changed=tools.gizmo_input.show(captured||visible,options,captured,scale);
    if(tools.gizmo_input.options_available()&&(captured||!this->tools().opened()||this->tools().showing(tools.gizmo_input)))
        route({.show=ToolPanel::Show{tools.gizmo_input,changed}});
    else if(options)route({.show=ToolPanel::Show{*options}});
    route({.context={.viewport=bounds}});
}
// Object overlays are hidden while an explicitly chosen camera child owns LMB.
void EditingWorkspaceUI::append_region_overlays(ui::DrawList& list,const text::Font& font) const {
    const auto& tools=*viewport_->interaction_;
    if(!tools.object_tools_suspended())tools.regions.append(list,font);
}
void EditingWorkspaceUI::append_tool_overlays(ui::DrawList& list,const text::Font& font,const gfx::Camera& camera,
    Extent2D extent,ui::Rect bounds,bool enabled,u64 revision,double time) {
    auto& tools=interaction_child();
    if(tools.object_tools_suspended())return;
    if(viewport().mode==ViewMode::scene&&selected_instances().size()>1&&enabled)
        for(const auto& p:tools.instance_projection.get(state(),extent,camera)) {
            if(!selected_instances().contains(p.object)||p.object==viewport().selected_object)continue;
            const Vec2 point{bounds.x+p.point.x*bounds.width,bounds.y+p.point.y*bounds.height};
            list.commands.emplace_back(ui::BoxDraw{{point.x-5,point.y-5,10,10},bounds,{1,.7F,.12F,1},{.04F,.04F,.04F,1},4,1});
        }
    if(!tools.instances.active()) {
        tools.translation.append(list,font);tools.bounds.append(list,font,false);tools.rotation.append(list,font);
        tools.pivot.append(list,font);tools.scale.append(list);
    }
    tools.instances.append(list,font);tools.mesh.append(list,font);tools.mesh_sockets.append(list,font);
    tools.mesh_part.append(list,font,viewport().mode==ViewMode::mesh&&blueprint_panel_child().pending(revision),time);
    tools.selection_box.append(list);
}
} // namespace editor_example
