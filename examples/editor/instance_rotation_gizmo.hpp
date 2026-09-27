#pragma once

#include "animation.hpp"
#include "editing_session.hpp"
#include "rotation_tool.hpp"
#include "blueprint_gizmos.hpp"

namespace editor_example {
struct RotationChange {
    vng::u32 object{};
    bool began{}, changed{}, finished{}, cancelled{}, committed{};
};
struct RotationProposal {
    RotationChange change{};
    vng::u64 revision{};
    vng::f32 time{};
    std::vector<vng::u32> selection{};
    TransformPivot pivot{};
    bool begin_attitude{}, owns_transaction{};
    std::optional<vng::u64> transaction{};
    std::optional<vng::Vec3> absolute{}, delta{};
    std::optional<std::array<vng::f64,3>> attitude{};
};

// Joins a purely visual tool to the selected instances' rotation properties. No renderer, GPU object,
// mesh copy, or worker callback is needed to preview a drag locally.
class InstanceRotationGizmo {
public:
    explicit InstanceRotationGizmo(const EditingSession& editing) : editing_(editing) {}
    [[nodiscard]] bool active() const {
        return frozen_&&transaction_&&editing_.active_transaction()==transaction_&&
            editing_.active(EditGesture::rotation)&&editing_.active_object()==frozen_->stamp.object;
    }
    [[nodiscard]] bool dragging() const { return tool_.dragging(); }
    [[nodiscard]] bool visible() const { return tool_.visible(); }
    [[nodiscard]] bool handledPointer() const { return tool_.handledPointer(); }
    [[nodiscard]] const RotationTool& tool() const { return tool_; }
    [[nodiscard]] vng::Vec3 center(std::span<const vng::u32> selection) {
        const auto& state=editing_.state();
        if(!active()&&(center_revision_!=state.document.revision||center_time_!=state.viewport.time||
            center_object_!=state.viewport.selected_object||!std::ranges::equal(center_selection_,selection))) {
            centers_=instance_centers(state,state.viewport.selected_object,selection);
            center_=selection_center(centers_);center_revision_=state.document.revision;
            center_time_=state.viewport.time;center_object_=state.viewport.selected_object;
            center_selection_.assign(selection.begin(),selection.end());
        }
        return center_;
    }
    void append(vng::ui::DrawList& list, const vng::text::Font& font = {}) const { tool_.append(list,font); }

    void accept_result(const vng::content::Result<RotationChange>& result) {
        if(!result||result->cancelled)reset();
        else if(result->finished){frozen_.reset();selection_.clear();transaction_.reset();}
        else if(result->began)transaction_=editing_.active_transaction();
    }
    [[nodiscard]] vng::content::Result<RotationProposal> cancel() {
        RotationProposal proposal{.change={.object=frozen_?static_cast<vng::u32>(frozen_->stamp.object):0,
                .finished=frozen_.has_value(),.cancelled=true},
            .revision=editing_.state().document.revision,.time=editing_.state().viewport.time,
            .owns_transaction=transaction_.has_value(),.transaction=transaction_};
        reset();
        return proposal;
    }
    void reset() {
        tool_.cancel();
        frozen_.reset();
        selection_.clear();
        transaction_.reset();
    }

    [[nodiscard]] vng::content::Result<RotationProposal>
    update(vng::u64 generation,
           const vng::gfx::CameraSnapshot& camera, vng::ui::Rect viewport,
           std::span<const vng::input::Event> unhandled,
           std::span<const vng::input::Event> raw, bool enabled,
           std::span<const vng::u32> selection = {}, bool attitude = false, TransformPivot pivot = {}, bool free_rotation = false, float arrow_step = 1.F) {
        const auto& state = editing_.state();
        if(frozen_&&transaction_&&editing_.active_transaction()!=transaction_)return cancel();
        RotationGizmo description{};
        const auto* instance = find_instance(state, state.viewport.selected_object);
        if (instance && state.viewport.mode == ViewMode::scene) {
            const auto transform = evaluate_transform(state, *instance, state.viewport.time);
            enabled = enabled && evaluate_visibility(state, *instance, state.viewport.time);
            description = {{instance->id, generation, state.document.revision},
                           transform.position, transform.rotation};
            description.position=center(selection);
            description.free_rotation=free_rotation;
            if(pivot.mode==PivotMode::custom)description.position=pivot.point;
            else if(pivot.mode==PivotMode::individual&&!centers_.empty())description.position=centers_.front().world;
            if(attitude) {
                description.local_axes=blueprint_attitude_axes(state,instance->blueprint);
                enabled &= description.local_axes.has_value();
            }
        } else enabled = false;
        // Our own revision/rotation updates must not cancel the active gesture;
        // changes of object or worker generation still must.
        if (frozen_ && (description.stamp.object != frozen_->stamp.object ||
                        generation != frozen_->stamp.generation || description.local_axes!=frozen_->local_axes ||
                        description.free_rotation!=frozen_->free_rotation ||
                        !std::ranges::equal(selection_,selection))) enabled = false;
        const auto result = tool_.update(frozen_.value_or(description), camera, viewport,
                                         unhandled, raw, enabled, arrow_step);
        RotationProposal proposal{.change={.object=state.viewport.selected_object},
            .revision=state.document.revision,.time=state.viewport.time,.owns_transaction=active(),.transaction=transaction_};
        auto& change=proposal.change;
        if (!active() && (tool_.dragging() || result)) {
            frozen_ = description;
            selection_.assign(selection.begin(),selection.end());
            proposal.selection=selection_;proposal.pivot=pivot;proposal.begin_attitude=attitude;
            change.began = true;
        }
        auto value = result ? result : tool_.preview_rotation();
        if (frozen_ && value) {
            if(free_rotation)proposal.delta=tool_.rotation_delta();
            else if(attitude)proposal.attitude=tool_.local_turns();
            else proposal.absolute=*value;
        }
        if (frozen_ && !tool_.dragging()) {
            change.object = static_cast<vng::u32>(frozen_->stamp.object);
            change.finished = true;
            change.cancelled = !result.has_value();
        }
        return proposal;
    }

private:
    RotationTool tool_;
    const EditingSession& editing_;
    std::optional<RotationGizmo> frozen_;
    std::optional<vng::u64> transaction_;
    std::vector<vng::u32> selection_;
    std::vector<InstanceCenter> centers_;
    std::vector<vng::u32> center_selection_;
    vng::u64 center_revision_{UINT64_MAX};
    vng::f32 center_time_{};
    vng::u32 center_object_{};
    vng::Vec3 center_{};
};

[[nodiscard]] inline vng::content::Result<RotationChange> execute(
    EditingSession& editing, InstanceRotationGizmo& tool,
    vng::content::Result<RotationProposal> proposal) {
    using namespace vng;
    if(!proposal){auto result=content::Result<RotationChange>{std::unexpected(proposal.error())};tool.accept_result(result);return result;}
    const auto& edit=*proposal;
    auto change=edit.change;
    bool owns=edit.owns_transaction&&edit.transaction&&editing.active_transaction()==edit.transaction&&
        editing.active(EditGesture::rotation)&&editing.active_object()==change.object;
    const auto fail=[&](content::Diagnostic error)->content::Result<RotationChange> {
        if(owns){auto restored=editing.cancel();if(!restored)error=restored.error();}
        auto result=content::Result<RotationChange>{std::unexpected(std::move(error))};tool.accept_result(result);return result;
    };
    const auto invalid=[&](std::string message) {
        content::Diagnostic error{};
        error.message=std::move(message);
        return fail(std::move(error));
    };
    if(edit.owns_transaction&&!owns)return invalid("Rotation proposal no longer owns its edit transaction");
    if(!change.cancelled&&(change.began||owns)&&
        (editing.state().document.revision!=edit.revision||editing.state().viewport.selected_object!=change.object||
         editing.state().viewport.time!=edit.time||editing.state().viewport.mode!=ViewMode::scene))
        return invalid("Rotation proposal belongs to a stale target or revision");
    if(change.began) {
        auto begun=edit.begin_attitude?editing.begin_attitude(change.object,edit.selection,edit.pivot)
            :editing.begin_rotation(change.object,edit.selection,edit.pivot);
        if(!begun)return fail(begun.error());
        owns=true;
    }
    if(owns) {
        if(change.cancelled) {
            auto restored=editing.cancel();if(!restored)return fail(restored.error());change.changed=*restored;
        } else {
            content::Result<bool> changed{false};
            if(edit.delta)changed=editing.rotate_by(*edit.delta);
            else if(edit.attitude)changed=editing.attitude(*edit.attitude);
            else if(edit.absolute)changed=editing.rotate(*edit.absolute);
            if(!changed)return fail(changed.error());
            change.changed=*changed;
            if(change.finished){auto committed=editing.commit();if(!committed)return fail(committed.error());change.committed=*committed;}
        }
    }
    tool.accept_result(change);return change;
}
} // namespace editor_example
