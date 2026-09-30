#include "camera_gizmo.hpp"
#include <vng/input/routing.hpp>
#include <utility>

namespace editor_example {
namespace {
using namespace vng;
float& speed(Settings& settings,CameraGizmoMode mode) {
    if(mode==CameraGizmoMode::pan)return settings.camera_drag.pan;
    if(mode==CameraGizmoMode::forward)return settings.camera_drag.forward;
    if(mode==CameraGizmoMode::zoom)return settings.camera_drag.zoom;
    return settings.camera_drag.rotation;
}
void read_control(NumberControl& field,float& value,CameraPreferenceEdit& reply) {
    field.poll();
    if(auto changed=field.changedValue()){value=*changed;reply.changed=true;}
    reply.committed|=field.editCommitted();
    if(!field.status().empty())reply.error=field.status();
}
}
void CameraPointerGizmo::attach(ui::Container host) {
    const auto label=mode_==CameraGizmoMode::pan?"Pan multiplier":mode_==CameraGizmoMode::forward?"Forward multiplier":
        mode_==CameraGizmoMode::zoom?"Zoom multiplier":mode_==CameraGizmoMode::look?"Look degrees/pixel":"Orbit degrees/pixel";
    Settings defaults;
    speed_.emplace(host,label,.001F,100.F,speed(defaults,mode_));
    speed_->slider_range(.001F,mode_==CameraGizmoMode::orbit||mode_==CameraGizmoMode::look?2.F:10.F).visible(false);
}
void CameraPointerGizmo::present(bool visible,const Settings& settings) {
    visible_=visible;
    if(speed_) {auto copy=settings;speed_->visible(visible).value(speed(copy,mode_));}
}
void CameraPointerGizmo::poll(CameraPreferenceEdit& reply) {
    if(visible_&&speed_)read_control(*speed_,speed(reply.value,mode_),reply);
}
bool CameraPointerGizmo::update(CameraPose& pose,bool& smooth,const NavigationFrame& frame,
    std::span<const input::Event> raw,std::span<const input::Event> available,bool primary,bool enabled) {
    using Mode=CameraPointerLogic::DragMode;
    constexpr std::array modes{Mode::orbit,Mode::look,Mode::pan,Mode::dolly,Mode::zoom};
    pointer_.gesture(modes[static_cast<std::size_t>(mode_)],primary);
    pointer_.scroll_mode(mode_==CameraGizmoMode::zoom?CameraPointerLogic::ScrollMode::zoom:CameraPointerLogic::ScrollMode::move_forward);
    auto speeds=frame.drag_speeds;
    if(mode_==CameraGizmoMode::zoom)speeds.forward=speeds.zoom;
    pointer_.speeds(speeds);
    pointer_.orbit_enabled(true); // Shortcut arbitration belongs to CameraGizmo.
    pointer_.drag_origin(mode_==CameraGizmoMode::look?std::nullopt:frame.origin,
        mode_==CameraGizmoMode::look?nullptr:frame.mesh,frame.mesh_to_world);
    return pointer_.update(pose,frame.mode,smooth,{frame.viewport.x,frame.viewport.y},
        {frame.viewport.width,frame.viewport.height},available,raw,enabled);
}
DebugReport CameraPointerGizmo::debug_report() const {
    return {.name=std::string(camera_gizmo_description(mode_).label),.role="camera pointer gizmo",
        .situation=dragging()?"Dragging":"Ready",.owned={{"options visible",debug_bool(visible_)}},
        .children={pointer_.debug_report()}};
}
void CameraWalkGizmo::attach(ui::Container parent) {
    controls_=parent.column().padding(0).gap(4).visible(false);
    constexpr std::array labels{"Walk forward speed (units/s)","Walk sideways speed (units/s)","Walk vertical speed (units/s)","Walk Shift multiplier"};
    constexpr std::array captions{"Walk forward","Walk sideways","Walk vertical","Walk Shift"};
    for(std::size_t i=0;i<labels.size();++i) {
        speeds_.emplace_back(*controls_,labels[i],i==3?1.F:.001F,i==3?100.F:1'000'000.F,i==3?4.F:10.F,captions[i]);
        speeds_.back().slider_range(i==3?1.F:.001F,i==3?10.F:100.F);
    }
}
void CameraWalkGizmo::present(bool visible,const Settings& settings) {
    visible_=visible&&active();
    if(!controls_)return;
    controls_->visible(visible_);
    const std::array values{settings.walk.forward,settings.walk.sideways,settings.walk.vertical,settings.walk.fast_multiplier};
    for(std::size_t i=0;i<values.size();++i)speeds_[i].value(values[i]);
}
void CameraWalkGizmo::poll(CameraPreferenceEdit& reply) {
    if(!visible_||!controls_)return;
    const std::array values{&reply.value.walk.forward,&reply.value.walk.sideways,&reply.value.walk.vertical,&reply.value.walk.fast_multiplier};
    for(std::size_t i=0;i<values.size();++i)read_control(speeds_[i],*values[i],reply);
}
bool CameraWalkGizmo::update(CameraPose& pose,const NavigationFrame& frame,bool enabled) {
    return motion_.update(pose,frame.seconds,frame.raw,frame.walk_speeds,enabled&&frame.keyboard_enabled,frame.unhandled);
}
DebugReport CameraWalkGizmo::debug_report() const {
    return {.name="Walk",.role="camera walk gizmo",.situation=active()?"Walking":"Ready",
        .owned={{"moving",debug_bool(moving())},{"options visible",debug_bool(visible_)}}};
}
void CameraGizmo::attach(ui::Container host) {
    if(host_)throw std::logic_error("Camera gizmo UI already attached");
    host_=host.padding(6).gap(4).visible(false);
    heading_=host_->button("Camera modes...").height(30);
    body_=host_->column().padding(0).gap(4).scrollbar(ui::ScrollBar::automatic);
    for(const auto& description:camera_gizmo_descriptions) {
        const auto i=static_cast<std::size_t>(description.mode);
        choices_[i]=body_->button(std::string(description.label)+" ("+std::string(description.shortcut)+")").height(28);
    }
    objects_=body_->button("Object tools / selection (Esc)").height(28);
    hint_=body_->label("").height(44);
    scroll_=body_->checkbox("Scroll moves camera").height(28);
    for(auto& child:pointer_)child.attach(*body_);
    walk_.attach(*body_);
}
void CameraGizmo::present(const Presentation& p,const Settings& settings) {
    const bool fallback=!p.other_gizmo;
    if(fallback!=fallback_&&!selected_)opened_=fallback;
    fallback_=fallback;
    enabled_=p.enabled;
    visible_=p.visible;
    target_=p.target;
    // A chosen child lives in this menu: whatever hides it (Camera settings,
    // Logs, a dialog, Play) hands LMB back rather than leaving a hidden mode.
    if(!visible_&&selected_)object_tools();
    if(!host_)return;
    const bool expanded=opened_;
    host_->visible(visible_);
    heading_.text(std::string(active()?"CAMERA / ":"Camera modes / ")+std::string(camera_gizmo_description(operating_mode()).label)+(expanded?" −":" +"));
    body_->visible(expanded).enabled(enabled_);
    hint_.text(target_+"\n"+std::string(camera_gizmo_description(operating_mode()).hint));
    objects_.visible(selected_);
    for(const auto& description:camera_gizmo_descriptions)
        choices_[static_cast<std::size_t>(description.mode)].enabled(mode_!=description.mode||!selected_);
    if(!scroll_.changedValue())scroll_.value(settings.scroll_moves_camera);
    for(std::size_t i=0;i<pointer_.size();++i)
        pointer_[i].present(expanded&&(i==static_cast<std::size_t>(operating_mode())||(walk_.active()&&i==static_cast<std::size_t>(CameraGizmoMode::look))),settings);
    walk_.present(expanded,settings);
    const auto width=std::min(310.F,std::max(1.F,p.viewport.width-16));
    const auto height=expanded?std::min(310.F,std::max(42.F,p.viewport.height*.48F)):42.F;
    host_->position({p.viewport.x+std::max(0.F,p.viewport.width-width-8),p.viewport.y+std::max(0.F,p.viewport.height-height-8)}).width(width).height(height);
    body_->height(std::max(0.F,height-46));
}
CameraPreferenceEdit CameraGizmo::poll(const Settings& settings) {
    CameraPreferenceEdit reply{.value=settings,.error={}};
    if(!host_||!visible_)return reply;
    // Unfolding the menu is not a mode choice; only a child borrows LMB.
    if(heading_.clicked())opened_=!opened_;
    if(!enabled_)return reply;
    // Poll old controls before changing modes, so releasing a speed slider or
    // committing text does not disappear merely because another mode was picked.
    for(auto& child:pointer_)child.poll(reply);
    walk_.poll(reply);
    if(auto value=scroll_.changedValue()){reply.value.scroll_moves_camera=*value;reply.changed=reply.committed=true;}
    for(const auto& description:camera_gizmo_descriptions)
        if(choices_[static_cast<std::size_t>(description.mode)].clicked())select(description.mode);
    if(objects_.clicked())object_tools();
    return reply;
}
void CameraGizmo::select(CameraGizmoMode mode) {
    if(mode_!=mode)cancel_pointer();
    mode_=mode;selected_=true;opened_=true;
    walk_.active(mode==CameraGizmoMode::walk);
}
void CameraGizmo::object_tools() {cancel_pointer();walk_.active(false);selected_=false;mode_=CameraGizmoMode::orbit;}
void CameraGizmo::cancel_pointer() {
    cancelled_|=dragging();
    for(auto& child:pointer_)child.cancel();
    captured_.reset();
}
void CameraGizmo::cancel() { object_tools(); }
bool CameraGizmo::dragging() const {return std::ranges::any_of(pointer_,[](const auto& child){return child.dragging();});}
NavigationReply CameraGizmo::update(const NavigationFrame& frame,bool enabled) {
    NavigationReply reply{.pose=frame.pose,.smooth_zoom=frame.smooth_zoom};
    handled_=cancelled_=false;
    input_enabled_=enabled;
    const bool pointer_enabled=enabled&&frame.raw.focused&&!frame.raw.overflow;
    if(!pointer_enabled) {handled_=dragging();cancel_pointer();}
    const input::AvailableEvents available{frame.unhandled};
    for(const auto& event:frame.raw.events) {
        if(pointer_enabled && frame.keyboard_enabled && active() && !dragging() &&
            event.kind==input::EventKind::key_down && !event.repeat && event.modifiers.control &&
            (event.key==input::Key::left || event.key==input::Key::right) && available.contains(event)) {
            const auto count=static_cast<int>(camera_gizmo_descriptions.size());
            select(static_cast<CameraGizmoMode>((static_cast<int>(mode_)+count+(event.key==input::Key::right?1:-1))%count));
            handled_=true;continue;
        }
        // Focus loss and Escape end captures and held keys, even if UI used
        // the key. Leaving the chosen mode is a command: only an Escape that
        // no field, popup or flyout claimed, in an enabled viewport, does it.
        const bool escape=event.kind==input::EventKind::key_down&&event.key==input::Key::escape;
        if(escape||event.kind==input::EventKind::focus_lost) {
            handled_|=dragging();cancel_pointer();walk_.stop();
            if(escape&&selected_&&pointer_enabled&&!frame.escape_claimed){handled_=true;object_tools();}
            continue;
        }
        std::optional<CameraGizmoMode> next=captured_;
        if(!next&&pointer_enabled&&frame.viewport.contains(event.position)&&!contains(event.position)&&available.contains(event)) {
            if(event.kind==input::EventKind::scroll)
                next=mode_==CameraGizmoMode::zoom||(!frame.move_forward&&mode_!=CameraGizmoMode::forward)?CameraGizmoMode::zoom:CameraGizmoMode::forward;
            else if(event.kind==input::EventKind::pointer_down&&event.button==2) {
                if(event.modifiers.shift)next=CameraGizmoMode::pan;
                else if(event.modifiers.control)next=frame.move_forward?CameraGizmoMode::forward:CameraGizmoMode::zoom;
                else if(frame.orbit_enabled||selected_||walk_.active())next=walk_.active()||mode_==CameraGizmoMode::look?CameraGizmoMode::look:CameraGizmoMode::orbit;
            } else if(event.kind==input::EventKind::pointer_down&&event.button==0&&selected_)
                next=mode_==CameraGizmoMode::walk?CameraGizmoMode::look:mode_;
        }
        if(!next)continue;
        const auto raw=std::span{&event,1};
        const auto unused=available.contains(event)?raw:std::span<const input::Event>{};
        auto& child=pointer_[static_cast<std::size_t>(*next)];
        reply.changed|=child.update(reply.pose,reply.smooth_zoom,frame,raw,unused,selected_,pointer_enabled);
        handled_|=child.handled();cancelled_|=child.cancelled();
        captured_=child.dragging()?next:std::nullopt;
    }
    // An empty tick must still validate capture (resize/invalid target). Do not
    // retain held motion after focus loss or a UI input field taking ownership.
    if(captured_&&frame.raw.events.empty()) {
        auto& child=pointer_[static_cast<std::size_t>(*captured_)];
        child.update(reply.pose,reply.smooth_zoom,frame,{}, {},selected_,pointer_enabled);
        handled_|=child.handled();cancelled_|=child.cancelled();
        if(!child.dragging())captured_.reset();
    }
    if(walk_.update(reply.pose,frame,enabled)) {reply.changed=true;reply.smooth_zoom=false;handled_=true;}
    reply.cancelled=cancelled_;
    return reply;
}
void CameraGizmo::append(ui::DrawList& list) const {
    if(!visible_||!active()||!host_)return;
    // Small camera silhouette beside the corner controls, not a world-space
    // object or a selectable instance. The menu identifies the actual target.
    const auto b=host_->bounds();const ui::Rect clip{b.x-28,b.y,b.width+28,b.height};
    const Vec4 color{.3F,.85F,1.F,1};
    list.commands.emplace_back(ui::BoxDraw{{b.x-25,b.y+8,17,12},clip,{.03F,.08F,.1F,1},color,2,2});
    list.commands.emplace_back(ui::BoxDraw{{b.x-9,b.y+11,6,6},clip,color,{},1,0});
}
DebugReport CameraGizmo::debug_report() const {
    DebugReport report{.name="camera_navigation",.role="camera gizmo owning navigation gizmos",
        .situation=input_enabled_?std::string(camera_gizmo_description(operating_mode()).label):"Unavailable",
        .received={{"target",target_},{"fallback",debug_bool(fallback_)}},
        .owned={{"explicitly selected",debug_bool(selected_)},{"menu open",debug_bool(opened_)},
            {"pointer captured",debug_bool(dragging())},{"input handled",debug_bool(handled_)}}};
    for(const auto& child:pointer_)report.children.push_back(child.debug_report());
    report.children.push_back(walk_.debug_report());
    return report;
}
} // namespace editor_example
