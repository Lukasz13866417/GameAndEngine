#pragma once
#include "component_transform.hpp"
#include "editing_session.hpp"
#include "mesh_tools.hpp"
#include "animation.hpp"
#include "rotation_math.hpp"
#include "../support/mesh_frame.hpp"

namespace editor_example {
// Joins the shared point tool to blueprint-local, sparse mesh transactions.
class MeshTransform {
public:
    explicit MeshTransform(EditingSession& editing):editing_(editing){}
    void scale_limits(const ScaleLimits& limits) { tool_.scale_limits(limits); }
    bool active() const {return tool_.active();}
    bool visible() const {return tool_.visible();}
    bool handled() const {return tool_.handled();}
    ToolOptions* tool_options() {return tool_.tool_options();}
    void cancel() {tool_.cancel();}
    void append(vng::ui::DrawList& list,const vng::text::Font& font) const {tool_.append(list,font);}
    [[nodiscard]] vng::content::Result<bool> update(MeshTools& selection,
        const vng::gfx::CameraSnapshot& camera,vng::ui::Rect viewport,
        std::span<const vng::input::Event> input,std::span<const vng::input::Event> raw,bool enabled,float arrow_step=1.F) {
        using namespace vng;
        const auto& state=editing_.state();
        const auto target=mesh_target(state);
        const auto* mesh=editable_mesh(state);
        enabled &= target.has_value()&&mesh;
        if(!active()&&enabled) {
            if(revision_!=state.document.revision||selection_!=selection.selection_revision()||
               blueprint_!=target->blueprint||weld_!=state.viewport.weld||mode_!=state.viewport.mode) {
                points_.clear();ids_=selection.vertices(*mesh,state.viewport.weld);
                whole_=selection.mode()==MeshSelectMode::whole;
                transform_=state.viewport.mode==ViewMode::mesh?InstanceTransform{}:
                    evaluate_transform(state,*find_instance(state,state.viewport.selected_object),state.viewport.time);
                model_=mesh_transform(state,target->blueprint,transform_);
                const auto inverse=example::mesh_frame::inverse(model_);
                if(!inverse)return std::unexpected(inverse.error());
                inverse_=*inverse;
                for(auto id:ids_) {
                    auto p=example::mesh_frame::point(model_,mesh->position(id));
                    points_.push_back({id,p,{}});
                }
                if(whole_ && mesh->size()) {
                    // One synthetic pivot, not a huge implicit selection. The
                    // affine placement moves the unchanged geometry on the GPU.
                    points_.push_back({0,example::mesh_frame::point(model_,mesh->center()),{}});
                }
                revision_=state.document.revision;selection_=selection.selection_revision();
                blueprint_=target->blueprint;weld_=state.viewport.weld;mode_=state.viewport.mode;
            }
        }
        tool_.gizmo(selection.transform_mode());
        tool_.capabilities(whole_?std::span<const GizmoMode>{whole_mesh_gizmos}:std::span<const GizmoMode>{basic_transform_gizmos});
        auto action=tool_.update(points_,{static_cast<u64>(blueprint_),selection_,revision_},camera,viewport,input,raw,enabled,{},arrow_step);
        selection.transform_mode(tool_.gizmo());
        if(action.began) {
            auto begun=whole_?editing_.begin_mesh_transform(blueprint_):editing_.begin_vertices(blueprint_,ids_);
            if(!begun){tool_.cancel();return std::unexpected(begun.error());}
            if(whole_)whole_origin_=mesh_placement(state,blueprint_,true);
        }
        bool changed{};
        if(whole_ && editing_.active(EditGesture::mesh_transform)) {
            if(action.cancelled) {
                auto result=editing_.cancel();if(!result)return result;return *result;
            }
            if(action.changed) {
                auto result=editing_.mesh_transform(example::mesh_frame::compose(tool_.matrix(),whole_origin_));
                if(!result){(void)editing_.cancel();cancel();return result;}changed=*result;
            }
            if(action.finished){auto result=editing_.commit();if(!result)return result;}
            return changed;
        }
        if(editing_.active(EditGesture::vertices)) {
            if(action.cancelled) {
                auto result=editing_.cancel();if(!result)return result;changed=*result;
            } else {
                if(action.changed) {
                    std::vector<VertexPosition> values;
                    for(auto point:action.points) {
                        values.push_back({point.id,example::mesh_frame::point(inverse_,point.position)});
                    }
                    auto result=editing_.vertices(values);
                    if(!result){(void)editing_.cancel();tool_.cancel();return result;}changed=*result;
                }
                if(action.finished){auto result=editing_.commit();if(!result)return result;}
            }
        }
        return changed;
    }
private:
    EditingSession& editing_;
    ComponentTransform tool_;
    std::vector<vng::editor::ScenePoint> points_;
    std::vector<vng::u32> ids_;
    vng::u64 revision_{UINT64_MAX},selection_{};
    BlueprintId blueprint_{};
    bool weld_{};
    bool whole_{};
    vng::Mat4 model_{vng::Mat4::identity()},inverse_{vng::Mat4::identity()},whole_origin_{vng::Mat4::identity()};
    ViewMode mode_{};
    InstanceTransform transform_{};
};
}
