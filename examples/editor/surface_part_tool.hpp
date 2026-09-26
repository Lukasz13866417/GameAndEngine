#pragma once
#include "surface_move_tool.hpp"
#include "surface_altitude_tool.hpp"
#include "rotation_tool.hpp"
#include "scale_tool.hpp"
#include "rotation_math.hpp"
#include "transform_keys.hpp"
#include "../support/mesh_frame.hpp"
#include <numbers>

namespace editor_example {
// A blueprint-declared surface constraint plus an optional heading ring. The
// tool owns input/visuals only; the panel schedules immutable authoring jobs.
class SurfacePartTool {
public:
    SurfacePartAction update(const std::optional<SurfaceMove>& target,const vng::gfx::CameraSnapshot& camera,
        vng::ui::Rect viewport,std::span<const vng::input::Event> input,std::span<const vng::input::Event> raw,
        bool visible,bool can_begin,float arrow_step=1.F) {
        markers_.clear();
        return update_active(target,camera,viewport,input,raw,visible,can_begin,arrow_step);
    }
    // A blueprint can expose several handles in one mode. The nearest visible
    // handle wins a press; that same press begins its drag, without a second click.
    SurfacePartAction update(std::span<const SurfaceMove> handles,std::size_t selected,
        const vng::gfx::CameraSnapshot& camera,vng::ui::Rect viewport,
        std::span<const vng::input::Event> input,std::span<const vng::input::Event> raw,
        bool visible,bool can_begin,float arrow_step=1.F) {
        using namespace vng;
        markers_.resize(handles.size());
        for(std::size_t i=0;i<handles.size();++i)
            (void)markers_[i].update(handles[i],camera,viewport,{},{},visible,true);
        const auto original=selected;
        selected=handles.empty()?0:std::min(selected,handles.size()-1);
        if(!dragging()&&can_begin&&visible)for(const auto& event:input) {
            if(event.kind!=input::EventKind::pointer_down||event.button!=0||
               event.modifiers.control||event.modifiers.alt||event.modifiers.super)continue;
            float nearest=14;
            for(std::size_t i=0;i<markers_.size();++i)if(const auto p=markers_[i].handle()) {
                const auto distance=std::hypot(p->x-event.position.x,p->y-event.position.y);
                if(distance<nearest){nearest=distance;selected=i;}
            }
            break;
        }
        shown_handle_=selected;
        auto action=update_active(handles.empty()?std::nullopt:std::optional{handles[selected]},
            camera,viewport,input,raw,visible,can_begin,arrow_step);
        if(selected!=original)action.selected_handle=selected;
        return action;
    }
    std::vector<std::optional<vng::Vec2>> handle_positions() const {
        std::vector<std::optional<vng::Vec2>> points;
        for(std::size_t i=0;i<markers_.size();++i)
            points.push_back(i==shown_handle_?handle():markers_[i].handle());
        return points;
    }
private:
    SurfacePartAction update_active(const std::optional<SurfaceMove>& target,const vng::gfx::CameraSnapshot& camera,
        vng::ui::Rect viewport,std::span<const vng::input::Event> input,std::span<const vng::input::Event> raw,
        bool visible,bool can_begin,float arrow_step) {
        using namespace vng;
        SurfacePartAction action;handled_=false;
        if((target&&target->scale)||scale_.dragging()) {
            if(target!=target_&&scale_.dragging()) {
                cancel();action.cancelled=action.finished=handled_=true;return action;
            }
            if(target!=target_)scale_.cancel();
            move_.cancel();rotation_.cancel();altitude_.cancel();modal_=false;
            target_=target;
            if(!target||!target->scale)return action;
            scale_.maximum(target->scale->maximum);
            const auto result=scale_.update({1,1,1},example::mesh_frame::point(target->frame,target->position),target->scale->value,
                camera,viewport,can_begin||scale_.dragging()?input:std::span<const input::Event>{},raw,visible,{},arrow_step,true);
            handled_=scale_.handledPointer();
            action.began=result.began;action.changed=result.changed;action.finished=result.finished;action.cancelled=result.cancelled;
            action.scale_value=result.value;return action;
        }
        scale_.cancel();
        if(modal_ && (target!=target_ ||
            viewport.x!=viewport_.x||viewport.y!=viewport_.y||viewport.width!=viewport_.width||viewport.height!=viewport_.height||!visible)) {
            cancel();action.cancelled=handled_=true;return action;
        }
        const bool reframe=modal_&&camera.view_projection!=camera_.view_projection;
        target_=target;camera_=camera;viewport_=viewport;
        auto lift=altitude_.update(target,camera,viewport,input,raw,
            visible&&!move_.dragging()&&!rotation_.dragging()&&!modal_,can_begin,arrow_step);
        if(altitude_.handled()) {
            rotation_.clear_selection();prefer_rotation_=false;
            (void)move_.update(altitude_.presentation(target),camera,viewport,{},{},visible,false,arrow_step);
            handled_=true;return lift;
        }
        if(!dragging()&&can_begin)for(const auto& event:input)
            if(event.kind==input::EventKind::key_down && !event.modifiers.control && !event.modifiers.alt && !event.modifiers.super) {
                if(event.key==input::Key::g)prefer_rotation_=false;
                if(event.key==input::Key::r)prefer_rotation_=true;
            }
        std::vector<input::Event> move_input(input.begin(),input.end()),move_raw(raw.begin(),raw.end());
        if(prefer_rotation_&&target&&target->can_rotate) {
            std::erase_if(move_input,[](const auto& e){return transform_arrow(e).has_value();});
            std::erase_if(move_raw,[](const auto& e){return transform_arrow(e).has_value();});
        }
        // Update the center without giving it input while a ring owns capture.
        action=move_.update(altitude_.presentation(target),camera,viewport,rotation_.dragging()||modal_?std::span<const input::Event>{}:std::span<const input::Event>{move_input},
            rotation_.dragging()||modal_?std::span<const input::Event>{}:std::span<const input::Event>{move_raw},visible,can_begin&&!rotation_.dragging()&&!modal_,arrow_step);
        handled_=move_.handled();
        const bool rotate_enabled=visible && target && target->can_rotate && (move_.visible()||rotation_.dragging()||modal_) && !move_.dragging();
        RotationGizmo ring{};
        if(rotate_enabled) {
            const auto unit=[](Vec3 p){return gfx::camera_detail::normalize(p,gfx::camera_detail::dot(p,p));};
            const Vec3 radial{target->position.x-target->center.x,target->position.y-target->center.y,target->position.z-target->center.z};
            auto inv=example::mesh_frame::inverse(target->frame);
            const auto n=example::mesh_frame::normal(*inv,unit(radial));
            const auto x=unit(gfx::camera_detail::cross(std::abs(n.y)<.9F?Vec3{0,1,0}:Vec3{1,0,0},n));
            const auto y=gfx::camera_detail::cross(n,x);
            ring={{1,1,1},example::mesh_frame::point(target->frame,target->position),{},std::array{x,y,n},false,2};
        }
        const bool was=rotation_.dragging();
        auto result=rotation_.update(ring,camera,viewport,
            // can_begin gates new gestures, not input for the captured one.
            // Keyboard rotation needs available down/up events to recognize a
            // confirming viewport click (and distinguish it from a UI click).
            !modal_&&(can_begin||was)&&!handled_?input:std::span<const input::Event>{},
            !modal_&&!handled_?raw:std::span<const input::Event>{},rotate_enabled,arrow_step);
        if(move_.handled()) {
            altitude_.clear_selection();rotation_.clear_selection();prefer_rotation_=false;return action;
        }
        if(rotation_.handledPointer()) {
            altitude_.clear_selection();
            prefer_rotation_=true;
            handled_=true;action.began=!was&&(rotation_.dragging()||result.has_value());
            action.finished=(was&&!rotation_.dragging())||result.has_value();
            action.cancelled=was&&!rotation_.dragging()&&!result;
            const auto angle=f32(rotation_.turn().degrees);
            if(action.began)last_ring_angle_=0;
            action.changed=!action.cancelled&&angle!=last_ring_angle_;
            last_ring_angle_=angle;
            action.rotation_degrees=f32(rotation_.turn().degrees);return action;
        }
        if(!rotate_enabled)return action;
        if(move_.handle())center_=*move_.handle();
        const auto center=center_;
        if(reframe)pointer_angle_=std::atan2(pointer_.y-center.y,pointer_.x-center.x);
        for(const auto& event:modal_?raw:input) {
            if(!modal_) {
                if(!can_begin || !viewport.contains(event.position) || event.kind!=input::EventKind::key_down ||
                    event.key!=input::Key::r || event.repeat || event.modifiers.control || event.modifiers.alt || event.modifiers.super)continue;
                modal_=true;angle_=keyboard_angle_=0;pointer_=event.position;pointer_angle_=std::atan2(event.position.y-center.y,event.position.x-center.x);
                action.began=handled_=true;continue;
            }
            handled_=true;
            if(event.kind==input::EventKind::focus_lost || (event.kind==input::EventKind::key_down&&event.key==input::Key::escape) ||
                (event.kind==input::EventKind::pointer_down&&event.button==1)) {
                action.cancelled=action.finished=true;modal_=false;break;
            }
            if(auto arrow=transform_arrow(event,arrow_step)) {keyboard_angle_+=rotation_arrow(*arrow);action.changed=true;}
            if(event.kind==input::EventKind::pointer_move) {
                pointer_=event.position;
                const auto next=std::atan2(event.position.y-center.y,event.position.x-center.x);
                angle_-=f32(std::remainder(next-pointer_angle_,2*std::numbers::pi)/rotation_math::radians);
                pointer_angle_=next;action.changed=true;
            }
            if((event.kind==input::EventKind::key_down&&event.key==input::Key::enter)||
                (event.kind==input::EventKind::pointer_down&&event.button==0)) {
                action.changed=action.finished=true;modal_=false;break;
            }
        }
        if(modal_||action.finished)action.rotation_degrees=angle_+keyboard_angle_;
        return action;
    }
public:
    void cancel(){move_.cancel();rotation_.cancel();altitude_.cancel();scale_.cancel();markers_.clear();modal_=false;target_.reset();}
    bool dragging()const{return modal_||move_.dragging()||rotation_.dragging()||altitude_.dragging()||scale_.dragging();}
    bool handled()const{return handled_;}
    bool visible()const{return scale_.visible()||move_.visible();}
    std::optional<vng::Vec2> handle()const{return scale_.visible()?std::optional{scale_.handle()}:move_.handle();}
    auto position()const{return target_&&target_->scale?target_->position:move_.position();}
    const RotationTool& rotation()const{return rotation_;}
    const ScaleTool& scaling()const{return scale_;}
    const TranslationTool& altitude()const{return altitude_.tool();}
    void append(vng::ui::DrawList& list,const vng::text::Font& font,bool pending,double seconds)const {
        for(std::size_t i=0;i<markers_.size();++i)if(i!=shown_handle_)markers_[i].append_marker(list,font);
        move_.append(list,font,pending,seconds,markers_.size()>1);rotation_.append(list,font,2);altitude_.append(list,font);
        scale_.append(list);
    }
private:
    SurfaceMoveTool move_;
    SurfaceAltitudeTool altitude_;
    RotationTool rotation_;
    ScaleTool scale_;
    std::vector<SurfaceMoveTool> markers_;
    std::size_t shown_handle_{};
    std::optional<SurfaceMove> target_;
    vng::gfx::CameraSnapshot camera_{};
    vng::ui::Rect viewport_{};
    bool modal_{},handled_{},prefer_rotation_{};
    vng::f32 angle_{},keyboard_angle_{};
    vng::f32 last_ring_angle_{};
    double pointer_angle_{};
    vng::Vec2 pointer_{},center_{};
};
}
