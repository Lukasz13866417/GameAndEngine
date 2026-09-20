#pragma once
#include "surface_move_tool.hpp"
#include "rotation_tool.hpp"
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
        using namespace vng;
        SurfacePartAction action;handled_=false;
        if(modal_ && (target!=target_ ||
            viewport.x!=viewport_.x||viewport.y!=viewport_.y||viewport.width!=viewport_.width||viewport.height!=viewport_.height||!visible)) {
            cancel();action.cancelled=handled_=true;return action;
        }
        const bool reframe=modal_&&camera.view_projection!=camera_.view_projection;
        target_=target;camera_=camera;viewport_=viewport;
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
        action=move_.update(target,camera,viewport,rotation_.dragging()||modal_?std::span<const input::Event>{}:std::span<const input::Event>{move_input},
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
            !modal_&&can_begin&&!handled_?input:std::span<const input::Event>{},
            !modal_&&!handled_?raw:std::span<const input::Event>{},rotate_enabled,arrow_step);
        if(move_.handled())return action;
        if(rotation_.handledPointer()) {
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
    void cancel(){move_.cancel();rotation_.cancel();modal_=false;target_.reset();}
    bool dragging()const{return modal_||move_.dragging()||rotation_.dragging();}
    bool handled()const{return handled_;}
    bool visible()const{return move_.visible();}
    auto handle()const{return move_.handle();}
    auto position()const{return move_.position();}
    const RotationTool& rotation()const{return rotation_;}
    void append(vng::ui::DrawList& list,const vng::text::Font& font,bool pending,double seconds)const {
        move_.append(list,font,pending,seconds);rotation_.append(list,font,2);
    }
private:
    SurfaceMoveTool move_;
    RotationTool rotation_;
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
