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
// A response to the parent's input call, not a command sent to the session.
// Captured identities are owned; nothing borrows an input batch or selection.
struct InstanceTransformProposal {
    InstanceTransformChange change{};
    vng::u32 object{};
    vng::u64 revision{};
    vng::f32 time{};
    std::vector<vng::u32> selection{}; // Only needed when beginning a gesture.
    TransformPivot pivot{};
    bool owns_transaction{};
    std::optional<vng::u64> transaction{};
    std::optional<InstanceMovement::Edit> move{};
    std::optional<vng::Vec3> rotation{};
    struct Scale { vng::f32 factor{1}; int axis{-1}; ScaleLimits limits{}; };
    std::optional<Scale> scale{};
};
// Read-only session view plus local G/R/S interaction. The parent executes the
// owned proposal, then acknowledges the result; this child cannot author data.
class InstanceTransformInteraction {
public:
    explicit InstanceTransformInteraction(const EditingSession& editing) : editing_(editing) {}
    void scale_limits(const ScaleLimits& limits) { limits_=limits;tool_.maximum_scale_factor(limits.factor); }
    bool active() const { return owns_; }
    bool handled() const { return tool_.handled(); }
    bool visible() const { return tool_.visible(); }
    GizmoMode gizmo() const { return tool_.gizmo(); }
    const TransformGesture& gesture() const { return tool_; }
    ToolOptions* tool_options() { return tool_.active()?&tool_:nullptr; }
    void reset() { tool_.cancel(); owns_=false; selection_.clear();transaction_.reset(); }
    void accept_result(const vng::content::Result<InstanceTransformChange>& result) {
        if(!result || result->cancelled) reset();
        else if(result->finished) { owns_=false;selection_.clear();transaction_.reset(); }
        else if(result->began) transaction_=editing_.active_transaction();
    }
    void append(vng::ui::DrawList& list,const vng::text::Font& font) const { tool_.append(list,font); }
    vng::content::Result<InstanceTransformProposal> update(vng::u64 generation,
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
        InstanceTransformProposal result{.change={action,tool_.kind()},.object=primary_,
            .revision=state.document.revision,.time=state.viewport.time,.owns_transaction=owns_,.transaction=transaction_};
        if(action.began) {
            owns_=true;primary_=instance->id;time_=state.viewport.time;
            selection_.assign(selection.begin(),selection.end());
            result.object=primary_;result.selection=selection_;result.pivot=pivot;
        }
        if(!owns_) return result;
        if(action.cancelled) return result;
        if(action.changed) {
            const InstanceMovement::Target target{primary_,origin_.position};
            auto position=InstanceMovement::world_position(target);
            for(unsigned c=0;c<3;++c) position[c]+=tool_.translation()[c];
            if(tool_.kind()==TransformKind::move) result.move=InstanceMovement::move_to(target,position);
            else if(tool_.kind()==TransformKind::rotate) result.rotation=tool_.rotation();
            else result.scale=InstanceTransformProposal::Scale{tool_.factor(),tool_.axis(),limits_};
        }
        return result;
    }
private:
    const EditingSession& editing_;
    ScaleLimits limits_;
    TransformGesture tool_;
    InstanceTransform origin_{};
    vng::Vec3 center_{};
    std::vector<vng::u32> selection_;
    vng::u32 primary_{};
    vng::f32 time_{};
    bool owns_{};
    std::optional<vng::u64> transaction_;
};

// Parent-side execution. Interaction code has no mutable path to the session.
[[nodiscard]] inline vng::content::Result<InstanceTransformChange> execute(
    EditingSession& editing, InstanceTransformInteraction& tool,
    vng::content::Result<InstanceTransformProposal> proposal) {
    using namespace vng;
    if(!proposal) { auto result=content::Result<InstanceTransformChange>{std::unexpected(proposal.error())};tool.accept_result(result);return result; }
    auto& edit=*proposal;
    auto change=edit.change;
    const auto kind=change.kind==TransformKind::move?EditGesture::move:
        change.kind==TransformKind::rotate?EditGesture::rotation:EditGesture::scale;
    bool owns=edit.owns_transaction&&edit.transaction&&editing.active_transaction()==edit.transaction&&
        editing.active(kind)&&editing.active_object()==edit.object;
    const auto fail=[&](content::Diagnostic error)->content::Result<InstanceTransformChange> {
        if(owns) {auto restored=editing.cancel();if(!restored)error=restored.error();}
        auto result=content::Result<InstanceTransformChange>{std::unexpected(std::move(error))};
        tool.accept_result(result);return result;
    };
    const auto invalid=[&](std::string message) {
        content::Diagnostic error{};
        error.message=std::move(message);
        return fail(std::move(error));
    };
    if(edit.owns_transaction&&!owns)
        return invalid("Instance transform no longer owns its edit transaction");
    if(!change.cancelled&&(change.began||owns)&&
        (editing.state().document.revision!=edit.revision||editing.state().viewport.selected_object!=edit.object||
         editing.state().viewport.time!=edit.time||editing.state().viewport.mode!=ViewMode::scene))
        return invalid("Instance transform proposal belongs to a stale target or revision");
    if(change.began) {
        auto begun=change.kind==TransformKind::move ? editing.begin_move(edit.object,edit.selection)
            : change.kind==TransformKind::rotate ? editing.begin_rotation(edit.object,edit.selection,edit.pivot)
            : editing.begin_scale(edit.object,edit.selection,true);
        if(!begun)return fail(begun.error());
        owns=true;
    }
    if(owns) {
        if(change.cancelled) {
            auto restored=editing.cancel();if(!restored)return fail(restored.error());change.changed=*restored;
        } else {
            content::Result<bool> changed{false};
            if(edit.move)changed=editing.apply(*edit.move);
            else if(edit.rotation)changed=editing.rotate_by(*edit.rotation);
            else if(edit.scale)changed=editing.scale_factor(edit.scale->factor,edit.scale->axis,edit.scale->limits);
            if(!changed)return fail(changed.error());
            change.changed=*changed;
            if(change.finished) {auto committed=editing.commit();if(!committed)return fail(committed.error());change.committed=*committed;}
        }
    }
    tool.accept_result(change);return change;
}
}
