#pragma once
#include "surface_move.hpp"
#include "translation_tool.hpp"
#include "transform_keys.hpp"
#include "../support/mesh_frame.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace editor_example {
// Adapt the ordinary translation axis to a blueprint's radial constraint.
// Input capture, camera rebasing and arrow repeat stay in TranslationTool.
class SurfaceAltitudeTool {
public:
    SurfacePartAction update(const std::optional<SurfaceMove>& target,const vng::gfx::CameraSnapshot& camera,
        vng::ui::Rect viewport,std::span<const vng::input::Event> input,std::span<const vng::input::Event> raw,
        bool visible,bool can_begin,float arrow_step) {
        using namespace vng;SurfacePartAction action;
        if(!tool_.dragging()&&can_begin)ghost_.reset();
        if(!tool_.dragging())for(const auto& e:input)if(e.kind==input::EventKind::key_down&&
            !e.modifiers.control&&!e.modifiers.alt&&!e.modifiers.super&&
            (e.key==input::Key::g||e.key==input::Key::r))tool_.clear_selection();
        editor::Schema schema{{1,1,1},{}};
        const auto inverse=example::mesh_frame::inverse(target?target->frame:Mat4::identity());
        Vec3 radial{};std::optional<TranslationSegment> segment;
        bool enabled=visible&&target&&target->radial_range&&inverse;
        if(enabled) {
            for(unsigned c=0;c<3;++c)radial[c]=target->position[c]-target->center[c];
            const auto length=std::hypot(radial.x,radial.y,radial.z);
            enabled=length>1e-6F;
            if(enabled)for(unsigned c=0;c<3;++c)radial[c]/=length;
            const auto eye=example::mesh_frame::point(*inverse,camera.position);
            if(gfx::camera_detail::dot(radial,{eye.x-target->position.x,eye.y-target->position.y,eye.z-target->position.z})<0)enabled=false;
            schema.controls.push_back({"altitude","Endpoint altitude",editor::Kind::translation_gizmo,
                {{"position","Position",example::mesh_frame::point(target->frame,target->position),{}, {}}},true,{},
                {{"Up / down",example::mesh_frame::vector(target->frame,radial)}}});
            Vec3 first,last;
            for(unsigned c=0;c<3;++c) {
                first[c]=target->center[c]+radial[c]*target->radial_range->x;
                last[c]=target->center[c]+radial[c]*target->radial_range->y;
            }
            segment=TranslationSegment{example::mesh_frame::point(target->frame,first),example::mesh_frame::point(target->frame,last)};
        }
        std::vector<input::Event> keys(input.begin(),input.end()),events(raw.begin(),raw.end());
        if(!tool_.selected_axis()&&!tool_.dragging()) {
            std::erase_if(keys,[](const auto& e){return transform_arrow(e).has_value();});
            std::erase_if(events,[](const auto& e){return transform_arrow(e).has_value();});
        }
        const bool was=tool_.dragging();
        auto result=tool_.update(schema,camera,viewport,can_begin||was?std::span<const input::Event>{keys}:std::span<const input::Event>{},
            can_begin||was?std::span<const input::Event>{events}:std::span<const input::Event>{},enabled,false,arrow_step,segment);
        if(!tool_.handledPointer())return action;
        action.began=!was&&(tool_.dragging()||result.has_value());
        action.finished=(was&&!tool_.dragging())||result.has_value();
        action.cancelled=was&&!tool_.dragging()&&!result;
        if(action.cancelled){ghost_.reset();return action;}
        auto world=tool_.preview_position();
        if(result)world=std::get<Vec3>(result->values.front().value);
        if(world&&enabled) {
            const auto p=example::mesh_frame::point(*inverse,*world);
            const auto signed_radius=gfx::camera_detail::dot(radial,{p.x-target->center.x,p.y-target->center.y,p.z-target->center.z});
            const auto radius=std::clamp(f32(signed_radius),target->radial_range->x,target->radial_range->y);
            Vec3 point;for(unsigned c=0;c<3;++c)point[c]=target->center[c]+radial[c]*radius;
            action.changed=point!=ghost_.value_or(target->position);action.position=point;ghost_=point;
        }
        return action;
    }
    std::optional<SurfaceMove> presentation(std::optional<SurfaceMove> target) const {
        if(target&&ghost_) {target->position=*ghost_;target->radius=std::hypot(ghost_->x-target->center.x,ghost_->y-target->center.y,ghost_->z-target->center.z);}
        return target;
    }
    bool dragging() const{return tool_.dragging();}
    bool handled() const{return tool_.handledPointer();}
    bool selected() const{return tool_.selected_axis().has_value();}
    const TranslationTool& tool() const{return tool_;}
    void clear_selection(){tool_.clear_selection();}
    void cancel(){tool_.cancel();ghost_.reset();}
    void append(vng::ui::DrawList& list,const vng::text::Font& font) const{tool_.append(list,font);}
private:
    TranslationTool tool_;
    std::optional<vng::Vec3> ghost_;
};
}
