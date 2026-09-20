#pragma once
#include "transform_pivot.hpp"
#include "translation_tool.hpp"
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
    void update(vng::Vec3 center,bool visible,bool busy) {
        host_.visible(visible);host_.enabled(!busy);
        if(!visible)move_.value(false);
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
    void update_tool(TranslationTool& tool,const vng::gfx::CameraSnapshot& camera,vng::ui::Rect viewport,
        std::span<const vng::input::Event> input,std::span<const vng::input::Event> raw,bool enabled,float arrow_step=1.F) {
        const bool was=tool.dragging();
        if(!was)start_=pivot_.point;
        vng::editor::Schema schema;schema.stamp={1,1,1};
        schema.controls.push_back({"origin","Rotation origin",vng::editor::Kind::translation_gizmo,
            {{"position","Position",start_,{},{}}},true,{},{}});
        auto result=tool.update(schema,camera,viewport,input,raw,enabled&&moving(),true,arrow_step);
        if(auto p=tool.preview_position())pivot_.point=*p;
        if(result)pivot_.point=std::get<vng::Vec3>(result->values.front().value);
        else if(was&&!tool.dragging())pivot_.point=start_;
    }
    void cancel() {pivot_.point=start_;}
private:
    vng::ui::Container host_;
    vng::ui::Dropdown<PivotMode> modes_;
    vng::ui::Checkbox move_;
    vng::ui::Button reset_;
    TransformPivot pivot_{};
    vng::Vec3 start_{};
    bool initialized_{};
};
}
