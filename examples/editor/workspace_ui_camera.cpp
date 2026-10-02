#include "workspace_ui.hpp"
#include "animation.hpp"
#include "camera_glyph.hpp"

namespace editor_example {
using namespace vng;
void EditingWorkspaceUI::initialize_camera(ui::Container menu,ui::Container overlay,const Settings& settings) {
    if(viewport_->camera_)throw std::logic_error("Camera UI is already initialized");
    viewport_->camera_.emplace(menu,overlay,preview_camera_pose(state(),viewport().time),settings);
}
void EditingWorkspaceUI::sync_camera_controls(bool reset,bool idle_only) {
    if(viewport_&&viewport_->camera_)viewport_->camera_->sync(preview_camera_pose(state(),viewport().time),reset,idle_only);
}
void EditingWorkspaceUI::camera_settings_changed(const Settings& settings) {
    viewport_->camera_->distance_.slider_range(settings.orbit_distance.minimum,settings.orbit_distance.maximum);
}
void EditingWorkspaceUI::camera_menu_layout(ui::Rect button,Vec2 screen) {viewport_->camera_->layout(button,screen);}
void EditingWorkspaceUI::close_camera_menu(){viewport_->camera_->close();}
void EditingWorkspaceUI::toggle_camera_menu(const Settings& settings){viewport_->camera_->toggle(settings);}
void EditingWorkspaceUI::enable_camera_menu(bool enabled){viewport_->camera_->enabled(enabled,!camera_ui().inspecting());}
std::string EditingWorkspaceUI::poll_camera_menu(std::span<const input::Event> events,bool modal) {
    return viewport_->camera_->poll(events,modal);
}
std::optional<content::Result<Settings>> EditingWorkspaceUI::camera_preferences(const Settings& settings) {
    return viewport_->camera_->preferences(settings);
}
WorkspaceFeedback EditingWorkspaceUI::poll_camera_pose(const Settings& settings) {
    WorkspaceFeedback reply;
    const auto before=preview_camera_pose(state(),viewport().time);
    if(auto after=viewport_->camera_->pose_edit(before,settings,reply.message);after&&*after!=before) {
        viewport().smooth_zoom=false;viewport().editor_camera=*after;reply.camera_changed=true;
    }
    return reply;
}
bool EditingWorkspaceUI::end_camera_visit(bool restore) {
    if(!viewport_||!viewport_->camera_)return false;
    auto& visit=viewport_->camera_->visit_;
    if(!visit)return false;
    if(restore)viewport().editor_camera=visit->previous;
    visit.reset();interaction_child().camera_gizmo().object_tools();
    sync_camera_controls();
    return true;
}
std::string EditingWorkspaceUI::visit_camera(u32 id,bool editable) {
    const auto* instance=find_instance(state(),id);
    if(!instance||!is_camera_instance(state(),id))return {};
    auto& visit=viewport_->camera_->visit_;
    const auto previous=visit?visit->previous:viewport().editor_camera;
    visit=CameraVisit{id,editable,previous};
    viewport().editor_camera=camera_pose(evaluate_instance(state(),*instance,viewport().time));
    auto& gizmo=interaction_child().camera_gizmo();
    gizmo.object_tools();if(editable)gizmo.select(CameraGizmoMode::look);
    sync_camera_controls();
    return editable?"Looking through "+instance->name+" / navigate freely, then Save this camera to author the view":
        "Inspecting "+instance->name+" / read-only view; Back returns to the editor view";
}
bool EditingWorkspaceUI::reconcile_camera_visit() {
    const auto& visit=viewport_->camera_->visit_;
    if(!visit)return false;
    const auto* instance=find_instance(state(),visit->camera);
    if(!instance||!is_camera_instance(state(),visit->camera)||viewport().mode!=ViewMode::scene)return end_camera_visit(true);
    if(!visit->editable) {
        const auto pose=camera_pose(evaluate_instance(state(),*instance,viewport().time));
        if(pose!=viewport().editor_camera){viewport().editor_camera=pose;sync_camera_controls();return true;}
    }
    return false;
}
std::string EditingWorkspaceUI::camera_label(bool playing) const {
    const auto& visit=viewport_->camera_->visit_;
    if(playing)return "SIM CAMERA / independent Play";
    if(!visit)return state().viewport.mode==ViewMode::scene?"EDITOR CAMERA / private view":"EDITOR CAMERA / blueprint view";
    return (visit->editable?"SIM CAMERA (entered) / ":"SIM CAMERA (inspecting) / ")+object_name(state(),visit->camera);
}
std::string EditingWorkspaceUI::camera_target_label() const {
    const auto& visit=viewport_->camera_->visit_;
    return visit?std::string(visit->editable?"Entered camera: ":"Inspecting: ")+object_name(state(),visit->camera):"Editor camera";
}
WorkspaceFeedback EditingWorkspaceUI::poll_camera_actions() {
    WorkspaceFeedback reply;auto& ui=*viewport_->camera_;
    const auto* selected=is_camera_instance(state(),viewport().selected_object)?find_instance(state(),viewport().selected_object):nullptr;
    if(selected&&(ui.overlay_.enter_clicked()||ui.overlay_.inspect_clicked())) {
        reply.message=visit_camera(selected->id,ui.overlay_.enter_clicked());reply.camera_changed=true;
    }
    if(ui.overlay_.back_clicked()){reply.camera_changed=end_camera_visit(true);reply.message="Editor view restored";}
    if(ui.overlay_.save_clicked()&&ui.visit_&&ui.visit_->editable) {
        const auto name=object_name(state(),ui.visit_->camera);
        auto saved=editing_.set_camera(ui.visit_->camera,viewport().editor_camera);
        if(!saved)reply.message=saved.error().message;
        else {reply.authored=*saved;reply.message=*saved?"Saved the editor view into "+name+" at this keyframe":name+" already matches the editor view";}
    }
    if(ui.overlay_.activate_clicked()&&selected) {
        const auto name=selected->name;
        auto result=editing_.set_active_camera(selected->id);
        if(!result)reply.message=result.error().message;
        else {reply.authored|=*result;reply.message=*result?name+" is the active camera from this keyframe on":name+" is already the active camera here";}
    }
    if(reply.authored){
        (void)dispatch(*this,workspace_situation(viewport()),WorkspaceContext{.timeline=TimelineContext{.input={.synchronize=true}}});
        refresh_selection();
    }
    return reply;
}
void EditingWorkspaceUI::present_camera_actions(ui::Rect bounds,const gfx::Camera& camera,Extent2D extent,f32 time,
                                               bool playing,bool pending,bool visible,bool modal) {
    auto& ui=*viewport_->camera_;
    const auto* selected=is_camera_instance(state(),viewport().selected_object)?find_instance(state(),viewport().selected_object):nullptr;
    const bool scene=viewport().mode==ViewMode::scene;
    ui.overlay_.sync(scene?selected:nullptr,ui.visit_,selected&&active_camera(state(),viewport().time)==selected,
        editing_.can_edit_scene_pose()&&!playing&&!pending&&!editing_.awaiting_remote(),scene&&!playing&&visible);
    ui.overlay_.enabled(!modal&&!pending&&!editing_.busy());
    if(ui.overlay_.shown()) {
        std::optional<Vec3> anchor;
        if(selected)anchor=camera_glyph(evaluate_instance(state(),*selected,time)).body_center();
        ui.overlay_.layout(bounds,camera,extent,anchor);
    }
}
} // namespace editor_example
