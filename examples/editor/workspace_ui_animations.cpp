#include "workspace_ui.hpp"

namespace editor_example {
using namespace vng;
void EditingWorkspaceUI::create_animation_controls(BlueprintId id) {
    if(editing_.busy())return;
    viewport().mode=ViewMode::scene;viewport().paused=true;
    viewport_->animation_gizmo_->create(state(),id);show_tab(SidebarTab::properties);
}
WorkspaceFeedback EditingWorkspaceUI::poll_animation_controls(bool enabled,std::span<const input::Event> events) {
    WorkspaceFeedback feedback;auto& gizmo=*viewport_->animation_gizmo_;
    if(!enabled||!gizmo.visible())return feedback;
    auto action=gizmo.poll();
    for(const auto& event:events)if(event.kind==input::EventKind::key_down) {
        if(event.key==input::Key::escape&&(gizmo.creating()||editing_.active(EditGesture::animation_tree)))action.action=AnimationGizmo::Action::cancel;
        if(event.key==input::Key::enter&&editing_.creating_animation())action.action=AnimationGizmo::Action::finish;
    }
    const auto failed=[&](const content::Diagnostic& error){feedback.message=error.message;gizmo.error(error.message);};
    switch(action.action) {
    case AnimationGizmo::Action::none: break;
    case AnimationGizmo::Action::preview: {
        auto result=editing_.preview_animation(action.blueprint,action.targets,action.value.interval);
        if(!result){failed(result.error());break;}
        select_instance(*result);refresh_selection();feedback.authored=true;feedback.message="Animation preview / scrub the timeline, then Create or Cancel";break;
    }
    case AnimationGizmo::Action::edit: {
        if(!editing_.active(EditGesture::animation_tree)) {
            auto result=editing_.begin_animation(gizmo.target());if(!result){failed(result.error());break;}
        }
        auto result=editing_.animation(action.value);
        if(!result) {
            failed(result.error());if(!editing_.creating_animation())(void)editing_.cancel();break;
        }
        feedback.authored=*result;
        if(!action.dragging&&!editing_.creating_animation()) {
            auto result=editing_.commit();if(!result)failed(result.error());
        }
        break;
    }
    case AnimationGizmo::Action::finish: {
        auto result=editing_.commit();if(!result){failed(result.error());break;}
        gizmo.accepted(true);feedback.authored=*result;feedback.message="Animation created";refresh_selection();break;
    }
    case AnimationGizmo::Action::cancel: {
        if(editing_.active(EditGesture::animation_tree)) {
            auto result=editing_.cancel();if(!result){failed(result.error());break;}feedback.authored=*result;
        }
        gizmo.accepted(true);refresh_selection();feedback.message="Animation edit cancelled";break;
    }
    case AnimationGizmo::Action::bake:
    case AnimationGizmo::Action::erase: {
        auto id=gizmo.target();
        auto result=action.action==AnimationGizmo::Action::bake?editing_.bake_animation(id):editing_.erase_instances(std::span{&id,1});
        if(!result){failed(result.error());break;}
        feedback.authored=*result;refresh_selection();feedback.message=action.action==AnimationGizmo::Action::bake?
            "Animation baked to keyframes / root detached":"Animation removed / underlying keys restored";break;
    }
    }
    gizmo.present(state(),enabled,editing_.creating_animation());sync_sidebar();
    return feedback;
}
}
