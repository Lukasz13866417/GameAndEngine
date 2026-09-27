#pragma once
#include "transform_pivot.hpp"
#include "move_gizmo.hpp"
#include "rotation_origin_movement.hpp"
#include "component_debug.hpp"
#include <vng/ui/ui.hpp>

namespace editor_example {
// Private viewport preference and cursor. Moving an origin is not a document
// edit, does not create a keyframe, and never enters undo/preview serialization.
class RotationPivotControls {
public:
    explicit RotationPivotControls(vng::ui::Container parent):host_(parent.column().padding(0).gap(4)),
        modes_(host_.dropdown<PivotMode>("Pivot",{{PivotMode::selection,"Selection center"},
            {PivotMode::individual,"Individual centers"},{PivotMode::custom,"Custom point"}})) {
        move_=host_.checkbox("Move rotation origin");
        reset_=host_.button("Reset origin to center");
    }
    void update(vng::Vec3 center,bool visible,bool busy,bool poll_controls=true) {
        visible_=visible;enabled_=!busy;
        host_.visible(visible);host_.enabled(!busy);
        if(!visible)move_.value(false);
        if(!poll_controls)return;
        if(auto value=modes_.changedValue()) {
            pivot_.mode=*value;
            if(*value==PivotMode::custom&&!initialized_){pivot_.point=center;initialized_=true;}
            if(*value!=PivotMode::custom)move_.value(false);
        }
        if(reset_.clicked())pivot_.point=center;
        move_.visible(pivot_.mode==PivotMode::custom);reset_.visible(pivot_.mode==PivotMode::custom);
    }
    TransformPivot value() const {return pivot_;}
    bool moving() const {return pivot_.mode==PivotMode::custom&&move_.value();}
    void update_tool(MoveGizmo<RotationOriginMovement>& tool,const vng::gfx::CameraSnapshot& camera,vng::ui::Rect viewport,
        std::span<const vng::input::Event> input,std::span<const vng::input::Event> raw,bool enabled,float arrow_step=1.F) {
        const bool was=tool.dragging();
        if(!was)start_=pivot_.point;
        const TransformPivot target{PivotMode::custom,start_};
        const MoveGizmoContext context{{1,1,1},camera,viewport,input,raw,true,true,arrow_step};
        const auto result=enabled&&moving()?tool.update(target,context):tool.cancel();
        if(result.edit)pivot_.point=result.edit->point;
        if(result.cancelled)pivot_.point=start_;
    }
    void cancel() {pivot_.point=start_;}
    [[nodiscard]] DebugReport debug_report() const {
        const auto point=[](vng::Vec3 value) {
            return std::to_string(value.x)+", "+std::to_string(value.y)+", "+std::to_string(value.z);
        };
        return {.name="rotation_origin",.role="private rotation pivot choice and movable custom point",
            .situation=pivot_.mode==PivotMode::selection?"Selection center":
                pivot_.mode==PivotMode::individual?"Individual centers":"Custom point",
            .received={{"visible",debug_bool(visible_)},{"enabled",debug_bool(enabled_)}},
            .owned={{"custom point",point(pivot_.point)},{"initialized",debug_bool(initialized_)},
                {"move origin enabled",debug_bool(moving())},{"gesture start",point(start_)}}};
    }
    [[nodiscard]] std::string debug_string() const { return debug_report().string(); }
private:
    vng::ui::Container host_;
    vng::ui::Dropdown<PivotMode> modes_;
    vng::ui::Checkbox move_;
    vng::ui::Button reset_;
    TransformPivot pivot_{};
    vng::Vec3 start_{};
    bool initialized_{};
    bool visible_{},enabled_{true};
};
}
