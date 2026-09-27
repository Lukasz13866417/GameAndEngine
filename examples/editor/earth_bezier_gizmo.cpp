#include "earth_bezier_gizmo.hpp"

namespace editor_example {
namespace {
using namespace vng;
namespace earth=example::earth;
using Part=earth::InfrastructurePart;
MeshDraftEdit edit(Part part) {
    return {"Tunnel Bezier curve",[part=std::move(part)](const editor::EditableMesh& source)->content::Result<editor::EditableMesh> {
        auto result=earth::edit_infrastructure(source.document(),part);
        if(!result)return std::unexpected(result.error());
        return editor::EditableMesh::create(std::move(*result));
    }};
}
}
void append_bezier_gizmo(BlueprintMeshDescription& description,const Part& part,const earth::TunnelCurve& curve,Mat4 frame) {
    MeshPartChoice mode{"Tunnel path",{"Automatic arch","Bezier control points"},part.bezier_controls?1U:0U,{}};
    mode.choose=[part,curve](std::size_t index) {
        auto next=part;
        if(index==0)next.bezier_controls.reset();
        else if(!next.bezier_controls)next.bezier_controls=curve.initial_bezier_controls();
        return edit(std::move(next));
    };
    description.choices.push_back(std::move(mode));
    if(!part.bezier_controls)return;
    MeshPartGizmo gizmo{"Bezier control points",{}};
    for(std::size_t i=0;i<part.bezier_controls->size();++i) {
        const auto point=(*part.bezier_controls)[i];
        gizmo.handles.push_back({{{},point,std::hypot(point.x,point.y,point.z),"Control "+std::to_string(i+1),frame,false,Vec2{.01F,513}},
            [part,i](Vec3 point){auto next=part;(*next.bezier_controls)[i]=point;return edit(std::move(next));},{},
            [part,i]{auto next=part;next.bezier_controls->erase(next.bezier_controls->begin()+std::ptrdiff_t(i));return edit(std::move(next));}});
    }
    gizmo.options=[part,curve](editor::Inspector& ui,SubmitMeshDraftEdit submit) {
        auto values=ui.edit("bezier_sampling",part,"Bezier path / Earth coordinates");
        values.field("segments",&Part::curve_segments,"Line segments (8–256)");
        values.apply("Apply subdivision",[submit](const Part& next){submit(edit(next));});
        // Degree elevation inserts one control without changing the curve.
        if(part.bezier_controls->size()<14)ui.action("add_bezier_control",[part,curve,submit] {
            auto points=*part.bezier_controls;points.insert(points.begin(),curve.sample(0).position);points.push_back(curve.sample(1).position);
            std::vector<Vec3> controls;
            const auto n=points.size();
            for(std::size_t i=1;i<n;++i) {
                const auto t=f32(i)/f32(n);
                controls.push_back(earth::placement::add(earth::placement::mul(points[i-1],t),earth::placement::mul(points[i],1-t)));
            }
            auto next=part;next.bezier_controls=std::move(controls);submit(edit(std::move(next)));
        },"Add control point (preserve shape)");
    };
    description.gizmos.push_back(std::move(gizmo));
}
}
