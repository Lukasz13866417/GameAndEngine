#pragma once
#include "editing_session.hpp"
#include "animation.hpp"
#include "transform_gesture.hpp"
#include "blueprint_gizmos.hpp"
#include "rotation_math.hpp"

namespace editor_example {
struct InstanceTransformChange : TransformAction {
    TransformKind kind{TransformKind::move};
    bool committed{};
};
// Session-bound adapter: a G/R/S gesture updates instance properties, never
// blueprint geometry or a region's local boundary. Captured baselines are small;
// no mesh/document copy or preview-worker round trip is needed per mouse move.
class InstanceTransformInteraction {
public:
    explicit InstanceTransformInteraction(EditingSession& editing) : editing_(editing) {}
    void scale_limits(const ScaleLimits& limits) { limits_=limits;tool_.maximum_scale_factor(limits.factor); }
    bool active() const { return owns_; }
    bool handled() const { return tool_.handled(); }
    bool visible() const { return tool_.visible(); }
    GizmoMode gizmo() const { return tool_.gizmo(); }
    const TransformGesture& gesture() const { return tool_; }
    ToolOptions* tool_options() { return tool_.active()?&tool_:nullptr; }
    void reset() { tool_.cancel(); owns_=false; selection_.clear(); }
    void append(vng::ui::DrawList& list,const vng::text::Font& font) const { tool_.append(list,font); }
    vng::content::Result<InstanceTransformChange> update(vng::u64 generation,
        std::span<const vng::u32> selection, TransformPivot pivot,
        const vng::gfx::CameraSnapshot& camera,vng::ui::Rect viewport,
        std::span<const vng::input::Event> input,std::span<const vng::input::Event> raw,bool enabled,float arrow_step=1.F) {
        using namespace vng;
        const auto& state=editing_.state();
        const auto* instance=find_instance(state,state.viewport.selected_object);
        enabled &= instance && state.viewport.mode==ViewMode::scene && editing_.can_edit_scene_pose();
        if(instance) enabled &= evaluate_visibility(state,*instance,state.viewport.time);
        if(owns_) enabled &= primary_==state.viewport.selected_object && time_==state.viewport.time &&
            std::ranges::equal(selection_,selection);
        else enabled &= !editing_.busy();

        // Query blueprint capabilities only when a shortcut can start a gesture,
        // never on each pointer motion across a large scene.
        constexpr std::array transforms{GizmoMode::move,GizmoMode::rotate,GizmoMode::scale,GizmoMode::forward};
        std::vector<GizmoMode> common;
        std::optional<Vec3> forward;
        if(!owns_ && enabled && TransformGesture::requested(input,viewport,transforms)) {
            common=selection_gizmos(state,selection);
            const auto centers=instance_centers(state,instance->id,selection);
            origin_=evaluate_transform(state,*instance,state.viewport.time);
            center_=pivot.mode==PivotMode::custom ? pivot.point : pivot.mode==PivotMode::individual
                ? centers.front().world : selection_center(centers);
            if(auto axis=blueprint_manipulation(state,instance->blueprint).forward)
                forward=rotation_math::direction(origin_.rotation,*axis);
        }
        auto action=tool_.update({state.viewport.selected_object,generation,state.document.revision},
            center_,camera,viewport,input,raw,enabled,
            {.available=common,.forward=forward,.local_scale_rotation=origin_.rotation},arrow_step);
        InstanceTransformChange result{action,tool_.kind()};
        const auto fail=[&](content::Diagnostic error)->content::Result<InstanceTransformChange> {
            if(owns_) {auto restored=editing_.cancel();if(!restored)error=restored.error();}
            reset(); return std::unexpected(std::move(error));
        };
        if(action.began) {
            auto begun=tool_.kind()==TransformKind::move ? editing_.begin_move(instance->id,selection)
                : tool_.kind()==TransformKind::rotate ? editing_.begin_rotation(instance->id,selection,pivot)
                : editing_.begin_scale(instance->id,selection,true);
            if(!begun) return fail(begun.error());
            owns_=true;primary_=instance->id;time_=state.viewport.time;
            selection_.assign(selection.begin(),selection.end());
        }
        if(!owns_) return result;
        if(action.cancelled) {
            auto restored=editing_.cancel();reset();
            if(!restored) return std::unexpected(restored.error());
            result.changed=*restored; return result;
        }
        if(action.changed) {
            auto position=origin_.position;
            for(unsigned c=0;c<3;++c) position[c]+=tool_.translation()[c];
            auto changed=tool_.kind()==TransformKind::move ? editing_.move(position)
                : tool_.kind()==TransformKind::rotate ? editing_.rotate_by(tool_.rotation())
                : editing_.scale_factor(tool_.factor(),tool_.axis(),limits_);
            if(!changed) return fail(changed.error());
            result.changed=*changed;
        }
        if(action.finished) {
            auto committed=editing_.commit();
            if(!committed) return fail(committed.error());
            result.committed=*committed; owns_=false; selection_.clear();
        }
        return result;
    }
private:
    EditingSession& editing_;
    ScaleLimits limits_;
    TransformGesture tool_;
    InstanceTransform origin_{};
    vng::Vec3 center_{};
    std::vector<vng::u32> selection_;
    vng::u32 primary_{};
    vng::f32 time_{};
    bool owns_{};
};
}
