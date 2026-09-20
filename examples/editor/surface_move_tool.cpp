#include "surface_move_tool.hpp"
#include "transform_keys.hpp"
#include "../support/mesh_frame.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace editor_example {
namespace {
using namespace vng;
Vec3 add(Vec3 a, Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec3 mul(Vec3 a, double n) { return {f32(a.x*n),f32(a.y*n),f32(a.z*n)}; }
double dot(Vec3 a, Vec3 b) { return gfx::camera_detail::dot(a,b); }
Vec3 unit(Vec3 v) { return gfx::camera_detail::normalize(v,dot(v,v)); }
void line(ui::DrawList& list, ui::Rect clip, Vec2 a, Vec2 b, Vec4 color, f32 width=2) {
    const auto length=std::hypot(b.x-a.x,b.y-a.y);
    if(length<.001F)return;
    const Vec2 n{-(b.y-a.y)*width*.5F/length,(b.x-a.x)*width*.5F/length};
    const Vec2 p{a.x+n.x,a.y+n.y},q{a.x-n.x,a.y-n.y},r{b.x+n.x,b.y+n.y},s{b.x-n.x,b.y-n.y};
    list.commands.emplace_back(ui::TriangleDraw{{p,q,r},clip,color});
    list.commands.emplace_back(ui::TriangleDraw{{q,s,r},clip,color});
}
}
std::optional<Vec2> SurfaceMoveTool::project(Vec3 p) const {
    if(target_)p=example::mesh_frame::point(target_->frame,p);
    Vec4 clip{};
    for(unsigned r=0;r<4;++r)
        clip[r]=camera_.view_projection[3][r]+camera_.view_projection[0][r]*p.x+
                camera_.view_projection[1][r]*p.y+camera_.view_projection[2][r]*p.z;
    if(!std::isfinite(clip.w)||clip.w<=1e-6F||clip.z < -clip.w||clip.z>clip.w)return {};
    Vec2 result{viewport_.x+(clip.x/clip.w+1)*viewport_.width*.5F,
                viewport_.y+(1-clip.y/clip.w)*viewport_.height*.5F};
    if(!std::isfinite(result.x)||!std::isfinite(result.y))return {};
    return result;
}
std::optional<Vec3> SurfaceMoveTool::on_surface(Vec2 p) const {
    if(!target_||!std::isfinite(p.x)||!std::isfinite(p.y))return {};
    const auto x=(2*(p.x-viewport_.x)/viewport_.width-1)/camera_.projection[0][0];
    const auto y=(1-2*(p.y-viewport_.y)/viewport_.height)/camera_.projection[1][1];
    const auto offset=add(mul(camera_.right,x),mul(camera_.up,y));
    const bool perspective=camera_.projection[3][3]==0;
    const auto origin=example::mesh_frame::point(inverse_frame_,perspective?camera_.position:add(camera_.position,offset));
    const auto direction=unit(example::mesh_frame::vector(inverse_frame_,perspective?add(camera_.forward,offset):camera_.forward));
    const auto delta=sub(origin,target_->center);
    const auto b=dot(delta,direction),c=dot(delta,delta)-double(target_->radius)*target_->radius;
    const auto discriminant=b*b-c;
    double distance{};
    if(discriminant>=0) {
        distance=-b-std::sqrt(discriminant);
        if(distance<0)distance=-b+std::sqrt(discriminant);
        if(distance<0)return {};
    } else distance=std::max(0.,-b); // Outside silhouette: stop at its nearest surface direction.
    const auto radial=sub(add(origin,mul(direction,distance)),target_->center);
    if(dot(radial,radial)<1e-12)return {};
    return add(target_->center,mul(unit(radial),target_->radius));
}
void SurfaceMoveTool::geometry() {
    handle_.reset();
    if(!target_)return;
    // A formation on the far side is selected, but not pickable through Earth.
    const auto toward_eye=camera_.projection[3][3]==0
        ? sub(example::mesh_frame::point(inverse_frame_,camera_.position),ghost_)
        : mul(example::mesh_frame::vector(inverse_frame_,camera_.forward),-1);
    if(dot(sub(ghost_,target_->center),toward_eye) < -1e-6)return;
    if(auto point=project(ghost_);point&&viewport_.contains(*point))handle_=point;
}
void SurfaceMoveTool::cancel() { dragging_=keyboard_=false;handle_.reset();target_.reset(); }
SurfacePartAction SurfaceMoveTool::update(const std::optional<SurfaceMove>& target,
    const gfx::CameraSnapshot& camera,ui::Rect viewport,std::span<const input::Event> unhandled,
    std::span<const input::Event> raw,bool visible,bool can_begin,float arrow_step) {
    SurfacePartAction action;handled_=false;
    const auto inverse=example::mesh_frame::inverse(target?target->frame:Mat4::identity());
    const bool valid=visible&&target&&target->radius>0&&std::isfinite(target->radius)&&
        inverse.has_value()&&
        gfx::camera_detail::finite(target->center)&&gfx::camera_detail::finite(target->position)&&
        dot(sub(target->position,target->center),sub(target->position,target->center))>1e-12&&
        viewport.width>0&&viewport.height>0;
    const bool reframe=camera_.view_projection!=camera.view_projection;
    const bool moved_view=viewport_.x!=viewport.x ||
        viewport_.y!=viewport.y || viewport_.width!=viewport.width || viewport_.height!=viewport.height;
    if(!valid || (dragging_&&(target_!=target||moved_view))) {
        action.cancelled=dragging_;handled_=dragging_;cancel();
        if(!valid){camera_=camera;viewport_=viewport;return action;}
    }
    const bool same_target=target_==target;
    target_=target;camera_=camera;viewport_=viewport;inverse_frame_=*inverse;
    if(!dragging_ && (can_begin || !same_target))ghost_=target_->position;
    geometry();
    if(dragging_&&reframe&&handle_)offset_={pointer_.x-handle_->x,pointer_.y-handle_->y};
    for(const auto& event:raw.empty()?unhandled:raw) {
        const auto arrow=transform_arrow(event,arrow_step);
        const bool available=std::ranges::any_of(unhandled,[&](const auto& e){return e.kind==event.kind&&e.key==event.key;});
        const bool grab=event.kind==input::EventKind::key_down&&event.key==input::Key::g&&!event.repeat&&
            !event.modifiers.control&&!event.modifiers.alt&&!event.modifiers.super;
        if(!dragging_ && can_begin && handle_ && available && viewport.contains(event.position) && (grab||arrow)) {
            dragging_=keyboard_=action.began=handled_=true;
            pointer_=event.position;
            offset_={event.position.x-handle_->x,event.position.y-handle_->y};
        }
        if(dragging_ && arrow && handle_) {
            if(auto p=on_surface({handle_->x+arrow->x*5,handle_->y+arrow->y*5})) {
                ghost_=*p;action.changed=true;geometry();
                if(handle_)offset_={pointer_.x-handle_->x,pointer_.y-handle_->y};
            }
            handled_=true;continue;
        }
        if(dragging_) {
            if(event.kind==input::EventKind::focus_lost ||
               (event.kind==input::EventKind::key_down&&event.key==input::Key::escape) ||
               (event.kind==input::EventKind::pointer_down&&event.button==1)) {
                action.cancelled=true;handled_=true;dragging_=false;ghost_=target_->position;
                geometry();break;
            }
            if(keyboard_ && ((event.kind==input::EventKind::key_down&&event.key==input::Key::enter)||
                (event.kind==input::EventKind::pointer_down&&event.button==0))) {
                action.finished=handled_=true;dragging_=keyboard_=false;break;
            }
            const bool release=!keyboard_&&event.kind==input::EventKind::pointer_up&&event.button==0;
            if(event.kind==input::EventKind::pointer_move||release) {
                pointer_=event.position;
                handled_=true;
                if(auto position=on_surface({event.position.x-offset_.x,event.position.y-offset_.y});position&&*position!=ghost_) {
                    ghost_=*position;action.changed=true;
                }
                if(release){action.finished=true;dragging_=false;break;}
            }
        } else if(can_begin&&handle_&&event.kind==input::EventKind::pointer_down&&event.button==0&&
                  !event.modifiers.alt&&!event.modifiers.control&&
                  std::hypot(event.position.x-handle_->x,event.position.y-handle_->y)<=14 &&
                  std::ranges::any_of(unhandled,[&](const auto& available) {
                      return available.kind==event.kind&&available.button==event.button&&available.position==event.position;
                  })) {
            offset_={event.position.x-handle_->x,event.position.y-handle_->y};
            pointer_=event.position;
            keyboard_=false;dragging_=true;action.began=true;handled_=true;
        }
    }
    geometry();action.position=ghost_;return action;
}
void SurfaceMoveTool::append(ui::DrawList& list,const text::Font& font,bool pending,double seconds) const {
    constexpr Vec4 color{.2F,.85F,1,1};
    if(handle_&&target_) {
        const auto p=*handle_;
        const auto radial=unit(sub(ghost_,target_->center));
        const auto reference=std::abs(radial.y)<.95F?Vec3{0,1,0}:Vec3{0,0,1};
        const auto east=unit(gfx::camera_detail::cross(reference,radial));
        const auto north=gfx::camera_detail::cross(radial,east);
        for(auto tangent:{east,north}) {
            std::optional<Vec2> previous;
            for(int i=-8;i<=8;++i) {
                const auto a=double(i)*.018;
                auto point=project(add(target_->center,mul(add(mul(radial,std::cos(a)),mul(tangent,std::sin(a))),target_->radius)));
                if(point&&previous)line(list,viewport_,*previous,*point,color);
                previous=point;
            }
        }
        list.commands.emplace_back(ui::BoxDraw{{p.x-7,p.y-7,14,14},viewport_,{.035F,.12F,.16F,1},color,7,2});
        list.commands.emplace_back(ui::TextDraw{target_->label+" / move: G or drag",{p.x+15,p.y-18},viewport_,font,13,color});
    }
    if(pending&&viewport_.width>0) {
        const Vec2 p{viewport_.x+20,viewport_.y+23};
        list.commands.emplace_back(ui::BoxDraw{{viewport_.x+6,viewport_.y+6,225,34},viewport_,{.03F,.045F,.065F,.95F},{},4,0});
        for(int i=0;i<10;++i) {
            const auto a=seconds*5+double(i)*.45,b=a+.3;
            line(list,viewport_,{p.x+f32(9*std::cos(a)),p.y+f32(9*std::sin(a))},
                {p.x+f32(9*std::cos(b)),p.y+f32(9*std::sin(b))},{.2F,.85F,1,.15F+.085F*f32(i)},2.5F);
        }
        list.commands.emplace_back(ui::TextDraw{"Updating blueprint...",{p.x+18,p.y-8},viewport_,font,13,color});
    }
}
}
