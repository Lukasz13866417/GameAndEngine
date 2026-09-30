#include "workspace_ui.hpp"
#include "animation.hpp"
#include "mesh_import_view.hpp"
#include <numbers>

namespace editor_example {
using namespace vng;
WorkspaceFeedback EditingWorkspaceUI::poll_world_bounds(OrbitDistanceRange range) {
    WorkspaceFeedback reply;
    if(bounds_panel_->apply_requested()) {
        auto value=bounds_panel_->read();
        if(!value)reply.message=value.error().message;
        else if(auto begun=editing_.begin_world_bounds();!begun)reply.message=begun.error().message;
        else if(auto changed=editing_.world_bounds(*value);!changed) {
            reply=finish_interaction(ViewportTool::bounds,true);reply.message=changed.error().message;
        } else reply=finish_interaction(ViewportTool::bounds,false);
    }
    if(bounds_panel_->frame_requested()) {
        (void)end_camera_visit(false);
        const auto& bounds=state().document.world_bounds;
        auto& pose=viewport().editor_camera;
        float radius{};
        for(unsigned c=0;c<3;++c) {
            pose.target[c]=(bounds.minimum[c]+bounds.maximum[c])*.5F;
            radius=std::hypot(radius,(bounds.maximum[c]-bounds.minimum[c])*.5F);
        }
        pose.distance=std::clamp(radius/std::sin(camera_vertical_fov*std::numbers::pi_v<float>/360)*1.15F,range.minimum,range.maximum);
        pose.yaw=30;pose.pitch=20;pose.zoom=1;viewport().smooth_zoom=false;
        show_bounds();sync_camera_controls();reply.camera_changed=true;
        reply.message="World bounds / drag a face handle to resize / Escape cancels";
    }
    return reply;
}
std::optional<EditNotice> EditingWorkspaceUI::take_changes() {
    auto notice=editing_.take_changes();
    if(!notice||!viewport_||!viewport_->interaction_)return notice;
    interaction_child().regions.changed(notice->changes);
    if(notice->changes.full)refresh_selection();
    else if(!notice->changes.regions.empty()&&lists_) {
        bool renamed{};
        for(const auto& [id,changes]:notice->changes.regions)
            if(const auto* instance=find_instance(state(),id))
                renamed|=lists_->handle(SceneLists::Browsing{},SceneListsContext{
                    .input={.rename=SceneListInput::Rename{id,instance->name}}}).renamed;
        if(renamed)refresh_selection(false);
    }
    return notice;
}
void EditingWorkspaceUI::view_changed() {
    viewport().smooth_zoom=false;++viewport().sequence;invalidate_inspector();clear_inspector();reset_view_tools();
    refresh_selection();refresh_vertex();sync_camera_controls(true);
}
std::string EditingWorkspaceUI::select_scene_instance(u32 object,editor::SelectionMode mode) {
    if(!find_instance(state(),object))return {};
    const bool isolated=viewport().mode!=ViewMode::scene;
    viewport().mode=ViewMode::scene;select_instance(object,mode);
    if(panels_)interaction_mode(InteractionMode::objects);
    if(isolated)view_changed();
    if(is_animation_blueprint(find_instance(state(),object)->blueprint))show_tab(SidebarTab::properties);
    return selection_message_;
}
std::string EditingWorkspaceUI::open_mesh(BlueprintId blueprint) {
    if(auto result=inspect_mesh(state(),viewport(),blueprint);!result)return result.error().message;
    if(panels_){interaction_mode(InteractionMode::vertices);show_tab(SidebarTab::properties);}
    view_changed();
    return "Editing mesh draft / Apply mesh to scene publishes changes; Save on disk preserves the project";
}
std::string EditingWorkspaceUI::poll_interaction_mode() {
    const auto mode=changed_interaction_mode();
    return mode?change_interaction_mode(*mode):std::string{};
}
std::string EditingWorkspaceUI::change_interaction_mode(InteractionMode mode) {
    interaction_mode(mode);
    std::string message;
    if(interaction_mode()==InteractionMode::vertices) {
        const auto target=mesh_target(state());
        if(viewport().mode!=ViewMode::mesh)message=open_mesh(target?target->blueprint:viewport().inspected_mesh);
        show_tab(SidebarTab::properties);refresh_vertex();
    } else if(viewport().mode==ViewMode::mesh) {
        viewport().mode=ViewMode::scene;viewport().selected_vertex=0;view_changed();
        message="Scene instance tools";
    }
    reset_move_tool();return message;
}
WorkspaceFeedback EditingWorkspaceUI::poll_scene_lists(std::span<const input::Event> events) {
    const auto actions=lists_->handle(SceneLists::Browsing{},SceneListsContext{.input={.events=events,.poll=true}});
    WorkspaceFeedback reply;
    reply.mode_changed=actions.instance.has_value()||actions.blueprint.has_value();
    if(actions.instance)reply.message=select_scene_instance(actions.instance->id,actions.instance->mode);
    if(actions.blueprint) {
        const auto id=actions.blueprint->id;
        if(actions.blueprint->create_instance) {
            if(is_animation_blueprint(id)) {create_animation_controls(id);return reply;}
            auto result=editing_.instantiate(id);
            if(!result)reply.message=result.error().message;
            else {
                viewport().mode=ViewMode::scene;clear_inspector();reset_move_tool();select_instance(*result);
                refresh_selection();refresh_vertex();interaction_mode(InteractionMode::objects);
                reply.message="Added "+object_name(state(),*result)+" / blueprint retained for reuse";reply.authored=true;
            }
        } else if(mesh_geometry(state(),id))reply.message=open_mesh(id);
        else {
            const auto found=std::ranges::find(state().document.instances,id,&SceneInstance::blueprint);
            if(found==state().document.instances.end())reply.message="Add an instance (+) to inspect this effect's controls";
            else {
                viewport().mode=(id==BlueprintId::region||id==BlueprintId::camera||is_animation_blueprint(id))?ViewMode::scene:ViewMode::sun;
                if(id!=BlueprintId::camera)(void)end_camera_visit(false);
                select_instance(found->id);viewport().selected_vertex=0;interaction_mode(InteractionMode::objects);
                view_changed();reply.message=selection_message_;
                if(is_animation_blueprint(id))show_tab(SidebarTab::properties);
            }
        }
    }
    return reply;
}
WorkspaceCommandResult EditingWorkspaceUI::shortcut(EditShortcut shortcut) {
    WorkspaceCommandResult reply;
    if(shortcut==EditShortcut::undo||shortcut==EditShortcut::redo) {
        auto result=shortcut==EditShortcut::undo?editing_.undo():editing_.redo();
        if(!result)reply.message=result.error().message;
        else if(*result) {
            reply.changed=reply.discontinuous=true;
            refresh_selection();refresh_vertex();sync_camera_controls(true);refresh_after_pose();
        }
    } else if(shortcut==EditShortcut::copy) {
        const bool keys=keyboard_target_==SelectionTarget::keyframes;
        const auto count=keys?timeline_view().selected_keyframes().size():selected_instances().size();
        const bool instances=viewport().mode!=ViewMode::mesh&&interaction_mode()==InteractionMode::objects;
        auto result=keys?editing_.copy_keyframes(timeline_view().selected_keyframes()):
            editing_.copy_instances(instances?selected_instances().items():std::span<const u32>{});
        if(!result)reply.message="Copy failed: "+result.error().message;
        else reply.message="Copied "+std::to_string(count)+(keys?(count==1?" keyframe":" keyframes"):(count==1?" instance":" instances"))+
            (keys?" / Ctrl+V preserves relative timing":" / Ctrl+V pastes in place");
    } else if(shortcut==EditShortcut::paste) {
        auto result=editing_.paste();
        if(!result)reply.message="Paste failed: "+result.error().message;
        else {
            reply.changed=true;clear_inspector();reset_view_tools();refresh_selection();refresh_vertex();sync_camera_controls();
            if(result->object) {
                select_instances(result->objects);refresh_selection();
                keyboard_target_=SelectionTarget::instances;
                if(sidebar_tab()==SidebarTab::keyframe)show_tab(SidebarTab::properties);
                if(panels_)interaction_mode(InteractionMode::objects);
                choose_gizmo(GizmoMode::move);
            } else {
                (void)dispatch(*this,workspace_situation(viewport()),WorkspaceContext{
                    .timeline=TimelineContext{.input={.synchronize=true,.select=std::span<const f32>{result->keyframes}}}});
                keyboard_target_=SelectionTarget::keyframes;show_tab(SidebarTab::keyframe);
            }
            const auto count=result->object?result->objects.size():result->keyframes.size();
            reply.message="Pasted "+std::to_string(count)+(result->object?(count==1?" instance":" instances"):(count==1?" keyframe":" keyframes"))+
                (result->object?" in place / copies overlap originals / Move to separate them":" at playhead / Ctrl+Z undoes paste");
        }
    }
    return reply;
}
WorkspaceCommandResult EditingWorkspaceUI::delete_selection() {
    WorkspaceCommandResult reply;
    if(viewport().mode==ViewMode::mesh&&mesh_components().mode()!=MeshSelectMode::whole&&blueprint_panel().has_gizmo()) {
        if(erase_blueprint_handle())reply.message="Removing selected handle...";
        if(auto result=apply_pending(blueprint_panel_child());!result)reply.message=result.error().message;
        else reply.changed=*result;
        return reply;
    }
    content::Result<bool> result{false};std::string removed;
    if(keyboard_target_==SelectionTarget::keyframes) {
        const auto& keys=timeline_view().selected_keyframes();
        if(keys.empty())return reply;
        const auto count=std::ranges::count_if(keys,[](f32 time){return time!=0;});
        if(!count){reply.message="Time zero is the permanent initial keyframe";return reply;}
        result=editing_.erase_keyframes(keys);removed=std::to_string(count)+" keyframe(s)";
    } else if(interaction_mode()==InteractionMode::vertices) {
        reply.message="Switch to Objects mode to delete an instance; vertex deletion is not supported.";return reply;
    } else if(viewport().mode!=ViewMode::mesh&&viewport().selected_object) {
        removed=std::to_string(selected_instances().size())+" instance(s) (blueprints kept)";
        result=editing_.erase_instances(selected_instances().items());
    } else return reply; // Nothing selected: nothing to delete, nothing to explain.
    if(!result){reply.message=result.error().message;return reply;}
    if(!*result)return reply;
    if(keyboard_target_==SelectionTarget::keyframes)
        (void)dispatch(*this,workspace_situation(viewport()),WorkspaceContext{.timeline=TimelineContext{.input={.clear_selection=true}}});
    editing_.select_keyframe(timeline_view().selected_keyframe());
    clear_inspector();reset_move_tool();refresh_selection();refresh_vertex();sync_camera_controls();
    reply.changed=true;reply.message="Deleted "+removed+" / Undo restores it";
    return reply;
}
} // namespace editor_example
