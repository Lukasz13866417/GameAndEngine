#include "transform_gesture.hpp"
#include "rotation_math.hpp"
#include "transform_keys.hpp"
#include <algorithm>

namespace editor_example {
using namespace vng;
namespace {
std::optional<GizmoMode> requested(const input::Event& e, ui::Rect viewport,std::span<const GizmoMode> available) {
    const auto mode=gizmo_shortcut(e,available);
    return viewport.contains(e.position) && mode && gizmo_description(*mode).gesture ? mode : std::nullopt;
}
}
std::optional<GizmoMode> TransformGesture::requested(std::span<const input::Event> events, ui::Rect viewport,
    std::span<const GizmoMode> available) {
    for(const auto& e:events) if(auto mode=editor_example::requested(e,viewport,available)) return mode;
    return {};
}
void TransformGesture::motion(Vec2 p) {
    pointer_=p;
    if(kind_==TransformKind::move) {p.x+=keyboard_pixels_.x;p.y+=keyboard_pixels_.y;}
    if(kind_==TransformKind::move) {
        if(gizmo_==GizmoMode::forward) {
            const auto units=static_cast<float>(std::sqrt(gfx::camera_detail::dot(horizontal_,horizontal_)));
            const auto x=static_cast<float>(gfx::camera_detail::dot(horizontal_,forward_))/units;
            const auto y=static_cast<float>(gfx::camera_detail::dot(vertical_,forward_))/units;
            const float length=x*x+y*y;
            const float amount=length>.01F ? units*((p.x-start_pointer_.x)*x+(p.y-start_pointer_.y)*y)/length
                                          : -units*(p.y-start_pointer_.y);
            for(unsigned c=0;c<3;++c)delta_[c]=base_delta_[c]+forward_[c]*amount;
            return;
        }
        for(unsigned c=0;c<3;++c) delta_[c]=(axis_<0 || axis_==static_cast<int>(c))
            ? horizontal_[c]*(p.x-start_pointer_.x)+vertical_[c]*(p.y-start_pointer_.y) : 0;
        if(axis_>=0) {
            const auto units=static_cast<float>(std::sqrt(gfx::camera_detail::dot(horizontal_,horizontal_)));
            const auto c=static_cast<unsigned>(axis_);
            const auto x=horizontal_[c]/units, y=vertical_[c]/units, length=x*x+y*y;
            delta_[c]=length>.01F ? units*((p.x-start_pointer_.x)*x+(p.y-start_pointer_.y)*y)/length
                                 : -units*(p.y-start_pointer_.y);
        }
        for(unsigned c=0;c<3;++c)delta_[c]+=base_delta_[c];
    } else if(kind_==TransformKind::scale) {
        const auto radius=std::hypot(start_pointer_.x-center_.x,start_pointer_.y-center_.y);
        factor_=std::clamp(base_factor_*(radius>12 ? std::hypot(p.x-center_.x,p.y-center_.y)/radius
            : std::exp((p.x-start_pointer_.x)*.01F)),.001F,std::max(base_factor_,maximum_factor_));
    } else {
        const auto radius=std::hypot(start_pointer_.x-center_.x,start_pointer_.y-center_.y);
        const auto angle=radius>12 ? (std::atan2(start_pointer_.y-center_.y,start_pointer_.x-center_.x)
            -std::atan2(p.y-center_.y,p.x-center_.x))/rotation_math::radians : (p.x-start_pointer_.x)*.5;
        Vec3 axis{-camera_.forward.x,-camera_.forward.y,-camera_.forward.z};
        if(axis_>=0) { axis={}; axis[static_cast<unsigned>(axis_)]=1; }
        const auto delta=rotation_math::matrix(rotation_math::turn({},axis,angle+keyboard_angle_));
        angles_=rotation_math::euler(rotation_math::multiply(delta,rotation_math::matrix(base_angles_)),angles_);
    }
}
void TransformGesture::reframe(const gfx::CameraSnapshot& camera) {
    camera_=camera;base_delta_=delta_;base_angles_=angles_;base_factor_=factor_;
    start_pointer_=pointer_;keyboard_pixels_={};keyboard_angle_=0;
    Vec4 clip{};
    for(unsigned r=0;r<4;++r) {
        clip[r]=camera.view_projection[3][r];
        for(unsigned c=0;c<3;++c)clip[r]+=camera.view_projection[c][r]*(pivot_[c]+delta_[c]);
    }
    const float w=std::max(.0001F,clip.w);
    center_={viewport_.x+(clip.x/w+1)*viewport_.width*.5F,viewport_.y+(1-clip.y/w)*viewport_.height*.5F};
    const float units=2*w/std::max(.0001F,viewport_.height*camera.projection[1][1]);
    for(unsigned c=0;c<3;++c){horizontal_[c]=camera.right[c]*units;vertical_[c]=-camera.up[c]*units;}
    update_visual();
}
TransformAction TransformGesture::update(editor::Stamp stamp,Vec3 pivot,const gfx::CameraSnapshot& camera,
    ui::Rect viewport,std::span<const input::Event> input,std::span<const input::Event> raw,bool enabled,TransformGizmos gizmos,float arrow_step) {
    TransformAction result; handled_=false;
    if(!enabled || (active_ && (stamp.object!=stamp_.object || stamp.generation!=stamp_.generation ||
        viewport.x!=viewport_.x || viewport.y!=viewport_.y ||
        viewport.width!=viewport_.width || viewport.height!=viewport_.height))) {
        result.finished=result.cancelled=handled_=active_; cancel(); return result;
    }
    if(active_ && camera.view_projection!=camera_.view_projection)reframe(camera);
    const auto events=active_ ? raw : input;
    if(active_ && requested_axis_) {
        axis_=*requested_axis_;requested_axis_.reset();motion(pointer_);result.changed=true;
    }
    if(active_ && requested_finish_) {
        result.cancelled=*requested_finish_;result.finished=handled_=true;
        result.changed=!result.cancelled;cancel();return result;
    }
    for(const auto& e:events) {
        if(!active_) {
            const auto mode=editor_example::requested(e,viewport,gizmos.available);
            if(!mode) continue;
            const auto forward=gizmos.forward;
            if(*mode==GizmoMode::forward && (!forward || gfx::camera_detail::dot(*forward,*forward)<.0001F)) continue;
            gizmo_=*mode;kind_=*gizmo_description(*mode).gesture;
            stamp_=stamp; camera_=camera; viewport_=viewport;pivot_=pivot;
            scale_axes_rotation_=gizmos.local_scale_rotation.value_or(Vec3{});
            if(forward) {
                const auto length=static_cast<float>(std::sqrt(gfx::camera_detail::dot(*forward,*forward)));
                for(unsigned c=0;c<3;++c)forward_[c]=(*forward)[c]/length;
            }
            if(kind_==TransformKind::move)visual_.emplace<TranslationTool>();
            else if(kind_==TransformKind::rotate)visual_.emplace<RotationTool>();
            else visual_.emplace<ScaleTool>();
            delta_=base_delta_={}; angles_=base_angles_={}; factor_=base_factor_=1; axis_=-1; local_scale_=gizmos.local_scale_rotation.has_value();
            keyboard_pixels_={};keyboard_angle_=0;
            pointer_=start_pointer_=e.position;
            Vec4 clip{};
            for(unsigned r=0;r<4;++r) {clip[r]=camera.view_projection[3][r];for(unsigned c=0;c<3;++c)clip[r]+=camera.view_projection[c][r]*pivot[c];}
            const float w=std::max(.0001F,clip.w);
            center_={viewport.x+(clip.x/w+1)*viewport.width*.5F,viewport.y+(1-clip.y/w)*viewport.height*.5F};
            const float units=2*w/std::max(.0001F,viewport.height*camera.projection[1][1]);
            for(unsigned c=0;c<3;++c) {horizontal_[c]=camera.right[c]*units;vertical_[c]=-camera.up[c]*units;}
            active_=result.began=handled_=true;
            continue;
        }
        handled_=true;
        if(auto arrow=transform_arrow(e,arrow_step);arrow && kind_!=TransformKind::scale) {
            if(kind_==TransformKind::rotate)keyboard_angle_+=rotation_arrow(*arrow);
            else {keyboard_pixels_.x+=arrow->x*5;keyboard_pixels_.y+=arrow->y*5;}
            motion(pointer_);result.changed=true;
        }
        if(e.kind==input::EventKind::focus_lost || (e.kind==input::EventKind::key_down && e.key==input::Key::escape) ||
           (e.kind==input::EventKind::pointer_down && e.button==1)) {
            result.cancelled=result.finished=true; cancel(); break;
        }
        if(e.kind==input::EventKind::key_down && !e.repeat && !e.modifiers.control && !e.modifiers.alt &&
           !e.modifiers.super && gizmo_!=GizmoMode::forward) {
            const int next=e.key==input::Key::x ? 0 : e.key==input::Key::y ? 1 : e.key==input::Key::z ? 2 : -1;
            if(next>=0) {axis_=axis_==next ? -1 : next;motion(pointer_);result.changed=true;}
        }
        if(e.kind==input::EventKind::pointer_move) {motion(e.position);result.changed=true;}
        if((e.kind==input::EventKind::pointer_down && e.button==0) ||
           (e.kind==input::EventKind::key_down && e.key==input::Key::enter)) {
            if(e.kind==input::EventKind::pointer_down) motion(e.position);
            result.changed=result.finished=true; active_=false; break;
        }
    }
    handled_ |= active_;
    if(active_ && (result.began || result.changed)) update_visual();
    return result;
}
void TransformGesture::update_visual() {
    if(auto* move=std::get_if<TranslationTool>(&visual_)) {
        auto position=pivot_;
        for(unsigned c=0;c<3;++c)position[c]+=delta_[c];
        editor::Schema schema;schema.stamp=stamp_;
        schema.controls.push_back({"position","Selection",editor::Kind::translation_gizmo,
            {{"position","Position",position,{},{}}},true,{},{}});
        if(gizmo_==GizmoMode::forward) schema.controls.back().translation_axes.push_back({"Forward / back",forward_});
        (void)move->update(schema,camera_,viewport_,{},{},true,gizmo_!=GizmoMode::forward);
    } else if(auto* rotate=std::get_if<RotationTool>(&visual_)) {
        (void)rotate->update({stamp_,pivot_,{}},camera_,viewport_,{},{},true);
    } else {
        (void)std::get<ScaleTool>(visual_).update(stamp_,pivot_,1,camera_,viewport_,{},{},true,scale_axes_rotation_);
    }
}
bool TransformGesture::visible() const {
    return active_ && std::visit([](const auto& tool){return tool.visible();},visual_);
}
void TransformGesture::describe_options(editor::Inspector& ui) {
    if(gizmo_!=GizmoMode::forward) {
        ui.action("free",[this]{requested_axis_=-1;},"Free transform");
        for(int axis=0;axis<3;++axis)ui.action(std::string(1,"xyz"[axis]),[this,axis]{requested_axis_=axis;},
            std::string(1,"XYZ"[axis])+" axis");
    }
    ui.action("confirm",[this]{requested_finish_=false;},"Confirm transform");
    ui.action("cancel",[this]{requested_finish_=true;},"Cancel transform");
}
void TransformGesture::append(ui::DrawList& list,const text::Font& font) const {
    if(!active_) return;
    if(const auto* move=std::get_if<TranslationTool>(&visual_))move->append(list,font,axis_);
    else if(const auto* rotate=std::get_if<RotationTool>(&visual_))rotate->append(list,font,axis_);
    else std::get<ScaleTool>(visual_).append(list,axis_);
    if(!font.valid())return;
    std::string label(gizmo_label(gizmo_));
    if(axis_>=0) label+=(kind_==TransformKind::scale && local_scale_ ? " / local " : " / world ")+std::string(1,"XYZ"[axis_]);
    if(gizmo_!=GizmoMode::forward)label+=" | X Y Z constrain";
    label+=" | Arrows nudge / Shift fine | Click/Enter confirm | Esc/RMB cancel";
    list.commands.emplace_back(ui::TextDraw{label,{viewport_.x+12,viewport_.y+viewport_.height-28},viewport_,font,14,{1,.8F,.3F,1}});
}
}
