#include "component_transform.hpp"
#include "rotation_math.hpp"
#include <algorithm>

namespace editor_example {
using namespace vng;
Mat4 ComponentTransform::matrix() const {
    auto m=Mat4::identity();
    if(kind_==TransformKind::rotate) {
        const auto r=rotation_math::matrix(angles_);
        for(unsigned row=0;row<3;++row)for(unsigned col=0;col<3;++col)m[col][row]=f32(r[row][col]);
    } else if(kind_==TransformKind::scale) {
        for(unsigned c=0;c<3;++c)if(axis_<0||axis_==static_cast<int>(c))m[c][c]=factor_;
    }
    for(unsigned r=0;r<3;++r) {
        m[3][r]=pivot_[r]+delta_[r];
        for(unsigned c=0;c<3;++c)m[3][r]-=m[c][r]*pivot_[c];
    }
    return m;
}
void ComponentTransform::cancel() {
    active_=false;modal_.cancel();move_.cancel();rotate_.cancel();scale_.cancel();start_.clear();
}
void ComponentTransform::capture(std::span<const editor::ScenePoint> points,editor::Stamp stamp,
    const gfx::CameraSnapshot& camera,ui::Rect viewport,Vec3 pivot) {
    start_.assign(points.begin(),points.end());stamp_=stamp;camera_=camera;viewport_=viewport;pivot_=pivot;
    delta_={};angles_={};factor_=1;axis_=-1;
}
std::vector<editor::ScenePoint> ComponentTransform::transformed() const {
    auto result=start_;
    if(kind_==TransformKind::move) {
        if(delta_!=Vec3{})for(auto& point:result)for(unsigned c=0;c<3;++c)point.position[c]+=delta_[c];
        return result;
    }
    if((kind_==TransformKind::rotate&&angles_==Vec3{})||
       (kind_==TransformKind::scale&&factor_==1))return result;
    const auto rotation=rotation_math::matrix(angles_);
    for(auto& point:result) {
        Vec3 relative{};for(unsigned c=0;c<3;++c)relative[c]=point.position[c]-pivot_[c];
        if(kind_==TransformKind::rotate)relative=rotation_math::apply(rotation,relative);
        if(kind_==TransformKind::scale)for(unsigned c=0;c<3;++c)if(axis_<0||axis_==static_cast<int>(c))relative[c]*=factor_;
        for(unsigned c=0;c<3;++c)point.position[c]=pivot_[c]+relative[c]+delta_[c];
    }
    return result;
}
ComponentChange ComponentTransform::update(std::span<const editor::ScenePoint> points,editor::Stamp stamp,
    const gfx::CameraSnapshot& camera,ui::Rect viewport,std::span<const input::Event> input,
    std::span<const input::Event> raw,bool enabled,std::optional<Vec3> custom_pivot,float arrow_step) {
    ComponentChange action;handled_=false;
    if(!enabled||points.empty()||(active()&&(stamp.object!=stamp_.object||stamp.generation!=stamp_.generation||
        viewport.x!=viewport_.x||viewport.y!=viewport_.y||
        viewport.width!=viewport_.width||viewport.height!=viewport_.height))) {
        action.finished=action.cancelled=active();handled_=active();cancel();return action;
    }
    Vec3 center{};
    for(const auto& point:points)for(unsigned c=0;c<3;++c)center[c]+=point.position[c]/static_cast<float>(points.size());
    if(custom_pivot)center=*custom_pivot;
    const bool was=active_;
    const auto modal=modal_.update(stamp,center,camera,viewport,input,raw,!active_,{.available=available_},arrow_step);
    if(modal.began) capture(points,stamp,camera,viewport,center);
    if(modal_.handled()) {
        handled_=true;kind_=modal_.kind();free_rotation_=false;
        action.began=modal.began;action.changed=modal.changed;action.finished=modal.finished;action.cancelled=modal.cancelled;
        delta_=modal_.translation();angles_=modal_.rotation();factor_=modal_.factor();axis_=modal_.axis();
        if(action.changed&&!action.cancelled)action.points=transformed();
        move_.cancel();rotate_.cancel();scale_.cancel();
        if(action.finished) {
            // Hand off to passive handles without a blank confirmation frame or
            // feeding the confirming click into another interaction.
            auto position=pivot_;
            if(!action.cancelled && kind_==TransformKind::move)
                for(unsigned c=0;c<3;++c)position[c]+=delta_[c];
            if(kind_==TransformKind::move) {
                editor::Schema schema;schema.stamp=stamp_;
                schema.controls.push_back({"point","Selection",editor::Kind::translation_gizmo,
                    {{"position","Position",position,{},{}}},true,{},{}});
                (void)move_.update(schema,camera,viewport,{},{},true);
            } else if(kind_==TransformKind::rotate) {
                (void)rotate_.update({stamp_,position,{}},camera,viewport,{},{},true);
            } else (void)scale_.update(stamp_,position,1,camera,viewport,{},{},true);
        }
        return action;
    }
    if(!active_)capture(points,stamp,camera,viewport,center);
    const bool extending=std::ranges::any_of(input,[](const auto& e){return e.kind==input::EventKind::pointer_down&&(e.modifiers.shift||e.modifiers.control);});
    if(extending)input={};
    bool dragging{},finished{};
    if(kind_==TransformKind::move) {
        editor::Schema schema;schema.stamp=stamp_;
        schema.controls.push_back({"point","Selection",editor::Kind::translation_gizmo,{{"position","Position",pivot_,{},{}}},true,{},{}});
        auto result=move_.update(schema,camera,viewport,input,raw,true,true,arrow_step);
        auto p=move_.preview_position();if(result)p=std::get<Vec3>(result->values.front().value);
        if(p){const auto before=delta_;for(unsigned c=0;c<3;++c)delta_[c]=(*p)[c]-pivot_[c];action.changed=delta_!=before;}
        dragging=move_.dragging();finished=result.has_value();handled_=move_.handledPointer();
    } else if(kind_==TransformKind::rotate) {
        auto result=rotate_.update({stamp_,pivot_,{}, {},free_rotation_},camera,viewport,input,raw,true,arrow_step);
        auto angle=result?result:rotate_.preview_rotation();if(angle){action.changed=angles_!=*angle;angles_=*angle;}
        dragging=rotate_.dragging();finished=result.has_value();handled_=rotate_.handledPointer();
    } else {
        auto result=scale_.update(stamp_,pivot_,1,camera,viewport,input,raw,true);
        factor_=result.value;action.changed=result.changed;
        dragging=scale_.dragging();finished=result.finished&&!result.cancelled;handled_=scale_.handledPointer();
    }
    action.began=!was&&(dragging||finished);action.finished=(was&&!dragging)||finished;
    action.cancelled=was&&!dragging&&!finished;active_=dragging;
    if(action.changed&&!action.cancelled)action.points=transformed();
    return action;
}
void ComponentTransform::append(ui::DrawList& list,const text::Font& font) const {
    if(modal_.active()) {modal_.append(list,font);return;}
    if(kind_==TransformKind::move)move_.append(list,font);
    else if(kind_==TransformKind::rotate)rotate_.append(list,font);
    else scale_.append(list);
}
}
