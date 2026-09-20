#include "scale_tool.hpp"
#include "scale_edits.hpp"
#include "rotation_math.hpp"
#include <algorithm>
#include <cmath>

namespace editor_example {
ScaleAction ScaleTool::update(vng::editor::Stamp stamp,vng::Vec3 position,vng::f32 scale,
    const vng::gfx::CameraSnapshot& camera,vng::ui::Rect viewport,
    std::span<const vng::input::Event> unhandled,std::span<const vng::input::Event> raw,bool enabled,vng::Vec3 axes_rotation) {
    using namespace vng;
    ScaleAction action{.value=scale};
    handled_=false;
    const bool resized=viewport.x!=viewport_.x || viewport.y!=viewport_.y ||
        viewport.width!=viewport_.width || viewport.height!=viewport_.height;
    if(!enabled || !std::isfinite(scale) || scale<min_instance_scale || scale>max_instance_scale ||
       (dragging_ && (stamp.object!=stamp_.object || stamp.generation!=stamp_.generation || resized))) {
        action.finished=action.cancelled=handled_=dragging_;
        cancel(); return action;
    }
    const bool reframe=dragging_&&projection_!=camera.view_projection;
    if(!dragging_ || reframe) {
        projection_=camera.view_projection;
        if(reframe){initial_=value_;start_=pointer_;}
        Vec4 clip{};
        for(u32 row=0;row<4;++row) {
            clip[row]=camera.view_projection[3][row];
            for(u32 col=0;col<3;++col) clip[row]+=camera.view_projection[col][row]*position[col];
        }
        visible_=false;
        if(!std::isfinite(clip.w) || clip.w<=0 || clip.z < -clip.w || clip.z>clip.w) return action;
        origin_={viewport.x+(clip.x/clip.w+1)*.5F*viewport.width,
                 viewport.y+(1-clip.y/clip.w)*.5F*viewport.height};
        handle_={origin_.x+55,origin_.y-55};
        const auto basis=rotation_math::matrix(axes_rotation);
        for(unsigned axis=0;axis<3;++axis) {
            Vec3 local{};local[axis]=1;
            const auto direction=rotation_math::apply(basis,local);
            Vec4 projected{};
            for(unsigned r=0;r<4;++r)for(unsigned col=0;col<3;++col)
                projected[r]+=camera.view_projection[col][r]*direction[col];
            const float x=(projected.x*clip.w-clip.x*projected.w)*viewport.width;
            const float y=-(projected.y*clip.w-clip.y*projected.w)*viewport.height;
            const float length=std::hypot(x,y);
            axis_tips_[axis]=length>.01F ? Vec2{origin_.x+48*x/length,origin_.y+48*y/length} : origin_;
        }
        visible_=viewport.contains(origin_) && viewport.contains(handle_);
        viewport_=viewport;
        stamp_=stamp;
    }
    if(!visible_) return action;
    for(const auto& e:raw) {
        using K=input::EventKind;
        if(e.kind==K::focus_lost || (e.kind==K::key_down && e.key==input::Key::escape)) {
            action.finished=action.cancelled=handled_=dragging_;
            cancel(); return action;
        }
        if(!dragging_ && e.kind==K::pointer_down && e.button==0 &&
            !e.modifiers.alt && !e.modifiers.control && !e.modifiers.super &&
            ui::Rect{handle_.x-10,handle_.y-10,20,20}.contains(e.position) &&
            std::ranges::any_of(unhandled,[&](const auto& p) {
                return p.kind==e.kind && p.button==e.button && p.position==e.position;
            })) {
            dragging_=action.began=handled_=true;
            initial_=value_=scale;
            pointer_=start_=e.position;
        }
        if(dragging_ && (e.kind==K::pointer_move || (e.kind==K::pointer_up && e.button==0))) {
            if(std::isfinite(e.position.x) && std::isfinite(e.position.y)) {
                pointer_=e.position;
                const auto distance=(e.position.x-start_.x)-(e.position.y-start_.y);
                // A narrower preference never snaps an already larger object.
                const auto next=std::clamp(initial_*std::exp(std::clamp(distance/160.F,-10.F,10.F)),min_instance_scale,std::max(initial_,maximum_));
                action.changed|=next!=value_;
                value_=next;
                const auto offset=std::clamp(55.F+distance*.25F,20.F,160.F);
                handle_={origin_.x+offset,origin_.y-offset};
            }
            handled_=true;
            if(e.kind==K::pointer_up) { dragging_=false; action.finished=true; }
        }
    }
    action.value=(dragging_ || action.finished || action.began) ? value_ : scale;
    return action;
}
void ScaleTool::append(vng::ui::DrawList& list,int only_axis) const {
    using namespace vng;
    if(!visible_) return;
    constexpr std::array colors{Vec4{1,.12F,.08F,1},Vec4{.1F,1,.2F,1},Vec4{.15F,.45F,1,1}};
    for(unsigned axis=0;only_axis>=0 && axis<3;++axis) {
        if(axis!=static_cast<unsigned>(only_axis)) continue;
        const auto tip=axis_tips_[axis];
        for(int i=4;i<32;++i) {
            const float t=static_cast<float>(i)/32;
            const Vec2 p{origin_.x+(tip.x-origin_.x)*t,origin_.y+(tip.y-origin_.y)*t};
            list.commands.emplace_back(ui::BoxDraw{{p.x-1,p.y-1,2,2},viewport_,colors[axis],{},0,0});
        }
        list.commands.emplace_back(ui::BoxDraw{{tip.x-4,tip.y-4,8,8},viewport_,colors[axis],{0,0,0,1},0,1});
    }
    if(only_axis>=0) return; // A constrained scale has no uniform-scale handle.
    const Vec4 color=dragging_ ? Vec4{1,.9F,.35F,1} : Vec4{1,.7F,.12F,1};
    for(int i=0;i<48;++i) {
        const auto t=static_cast<f32>(i)/48;
        const Vec2 p{origin_.x+(handle_.x-origin_.x)*t,origin_.y+(handle_.y-origin_.y)*t};
        list.commands.emplace_back(ui::BoxDraw{{p.x-1,p.y-1,2,2},viewport_,color,{},0,0});
    }
    list.commands.emplace_back(ui::BoxDraw{{origin_.x-4,origin_.y-4,8,8},viewport_,color,{0,0,0,1},1,1});
    list.commands.emplace_back(ui::BoxDraw{{handle_.x-7,handle_.y-7,14,14},viewport_,color,{0,0,0,1},1,1});
}
}
