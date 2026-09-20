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

// Joins a purely visual tool to the selected instances' rotation properties. No renderer, GPU object,
// mesh copy, or worker callback is needed to preview a drag locally.
class RotationInteraction {
public:
    explicit RotationInteraction(EditingSession& editing) : editing_(editing) {}
    [[nodiscard]] bool active() const { return frozen_.has_value() && editing_.active(EditGesture::rotation); }
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

    [[nodiscard]] vng::content::Result<RotationChange> cancel() {
        RotationChange change{.object = editing_.active_object(), .finished = active(), .cancelled = true};
        auto restored = active() ? editing_.cancel() : vng::content::Result<bool>{false};
        if (!restored) return std::unexpected(restored.error());
        change.changed = *restored;
        tool_.cancel();
        frozen_.reset();
        selection_.clear();
        return change;
    }

    [[nodiscard]] vng::content::Result<RotationChange>
    update(vng::u64 generation,
           const vng::gfx::CameraSnapshot& camera, vng::ui::Rect viewport,
           std::span<const vng::input::Event> unhandled,
           std::span<const vng::input::Event> raw, bool enabled,
           std::span<const vng::u32> selection = {}, bool attitude = false, TransformPivot pivot = {}, bool free_rotation = false, float arrow_step = 1.F) {
        const auto& state = editing_.state();
        RotationGizmo description{};
        const auto* instance = find_instance(state, state.viewport.selected_object);
        if (instance && state.viewport.mode == ViewMode::scene && (enabled || active())) {
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
        RotationChange change{.object = state.viewport.selected_object};
        if (!active() && (tool_.dragging() || result)) {
            auto begun=attitude ? editing_.begin_attitude(state.viewport.selected_object,selection,pivot)
                                : editing_.begin_rotation(state.viewport.selected_object,selection,pivot);
            if (!begun) {
                tool_.cancel();
                return std::unexpected(begun.error());
            }
            frozen_ = description;
            selection_.assign(selection.begin(),selection.end());
            change.began = true;
        }
        auto value = result ? result : tool_.preview_rotation();
        if (active() && value) {
            const auto turn=tool_.turn();
            if (auto moved = free_rotation ? editing_.rotate_by(tool_.rotation_delta()) :
                attitude ? editing_.attitude(turn.axis,turn.degrees) : editing_.rotate(*value); moved) change.changed = *moved;
            else return std::unexpected(moved.error());
        }
        if (active() && !tool_.dragging()) {
            change.object = editing_.active_object();
            change.finished = true;
            change.cancelled = !result.has_value();
            const auto finished = result ? editing_.commit() : editing_.cancel();
            if (!finished) return std::unexpected(finished.error());
            if (result) change.committed = *finished;
            else change.changed = change.changed || *finished;
            frozen_.reset();
            selection_.clear();
        }
        return change;
    }

private:
    RotationTool tool_;
    EditingSession& editing_;
    std::optional<RotationGizmo> frozen_;
    std::vector<vng::u32> selection_;
    std::vector<InstanceCenter> centers_;
    std::vector<vng::u32> center_selection_;
    vng::u64 center_revision_{UINT64_MAX};
    vng::f32 center_time_{};
    vng::u32 center_object_{};
    vng::Vec3 center_{};
};
} // namespace editor_example
