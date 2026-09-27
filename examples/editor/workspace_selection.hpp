#pragma once

#include "component_debug.hpp"
#include "project.hpp"
#include <vng/editor/selection.hpp>
#include <unordered_set>

namespace editor_example {
// Logical workspace selection, independent of list rows, gizmos and windows.
// Only its workspace owner changes it; children borrow the same observation.
class WorkspaceSelection final {
public:
    struct Change {
        bool changed{}, active_changed{};
        vng::u32 previous_active{}, active{};
    };
    [[nodiscard]] const vng::editor::Selection<vng::u32>& instances() const { return selected_; }
    [[nodiscard]] DebugReport debug_report() const {
        return {.name="selection", .role="authoritative scene-instance selection", .situation="Workspace-local",
            .owned={{"selected instances",std::to_string(selected_.size())},
                {"active instance",std::to_string(selected_.active().value_or(0))}},
            .observations={{"catalog revision",std::to_string(catalog_revision_)},
                {"catalog instances",std::to_string(order_.size())}}};
    }
private:
    friend class EditingWorkspaceUI;
    void observe(const State& state) {
        if(catalog_revision_==state.document.revision) return;
        catalog_revision_=state.document.revision;
        valid_.clear(); order_.clear();
        valid_.reserve(state.document.instances.size());
        order_.reserve(state.document.instances.size());
        for(const auto& instance:scene_instances(state)) {
            if(valid_.insert(instance.id).second) order_.push_back(instance.id);
        }
    }
    [[nodiscard]] Change changed(const vng::editor::Selection<vng::u32>& before) const {
        const auto previous=before.active().value_or(0), active=selected_.active().value_or(0);
        return {selected_!=before,previous!=active,previous,active};
    }
    Change select(const State& state,vng::u32 id,vng::editor::SelectionMode mode) {
        observe(state);
        const auto before=selected_;
        if(id && valid_.contains(id)) selected_.select(id,mode,order_);
        else if(!id && mode==vng::editor::SelectionMode::replace) selected_.clear();
        return changed(before);
    }
    Change select(const State& state,std::span<const vng::u32> ids,vng::editor::SelectionMode mode) {
        observe(state);
        const auto before=selected_;
        std::vector<vng::u32> accepted;
        accepted.reserve(ids.size());
        for(auto id:ids) if(valid_.contains(id)) accepted.push_back(id);
        selected_.select(accepted,mode);
        return changed(before);
    }
    Change restore(const State& state,const vng::editor::Selection<vng::u32>& origin) {
        observe(state);
        const auto before=selected_;
        selected_=origin;
        selected_.retain([&](auto id) { return valid_.contains(id); });
        return changed(before);
    }
    Change clear() {
        const auto before=selected_;
        selected_.clear();
        return changed(before);
    }
    Change reconcile(const State& state) {
        observe(state);
        const auto before=selected_;
        selected_.retain([&](auto id) { return valid_.contains(id); });
        // Domain history/load can restore its active-object bookmark. Honor it
        // without giving panels authority to overwrite another view's choice.
        const auto bookmark=valid_.contains(state.viewport.selected_object) ? state.viewport.selected_object : 0;
        if(selected_.active().value_or(0)!=bookmark) {
            selected_.clear();
            if(bookmark) selected_.select(bookmark);
        }
        return changed(before);
    }
    Change reset(const State& state) {
        const auto before=selected_;
        selected_.clear();
        reconcile(state);
        return changed(before);
    }
    vng::editor::Selection<vng::u32> selected_;
    std::unordered_set<vng::u32> valid_;
    std::vector<vng::u32> order_;
    vng::u64 catalog_revision_{};
};
} // namespace editor_example
