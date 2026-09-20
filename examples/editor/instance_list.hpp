#pragma once
#include "project.hpp"
#include "selection_input.hpp"
#include <vng/editor/selection.hpp>
#include <vng/ui/ui.hpp>
#include <unordered_map>
#include <unordered_set>

namespace editor_example {
// Owns widgets and their ID lookup. Document data is borrowed at sync(), not
// retained through pointers. Selection-only updates touch the changed rows.
class InstanceList {
public:
    struct Entry {vng::u32 id;vng::ui::Container row;vng::ui::Button button;vng::ui::Label blueprint;std::string name,blueprint_name;};
    struct Stats {std::size_t syncs{},rows_created{},labels_updated{};};
    enum class Filter { all, scene, regions };
    explicit InstanceList(vng::ui::Container host, Filter filter = Filter::all):host_(host),filter_(filter) {}
    bool contains(vng::u32 id) const {return lookup_.contains(id);}
    std::span<const Entry> entries() const {return rows_;}
    Stats stats() const {return stats_;}
    struct Pick {vng::u32 id; vng::editor::SelectionMode mode;};
    std::optional<Pick> poll(std::span<const vng::input::Event> events) const {
        for (const auto& row : rows_) if (row.button.clicked())
            return Pick{row.id, click_selection(click_modifiers(events, row.button.bounds()), true)};
        return {};
    }
    void reveal(vng::u32 id) {
        if (const auto at = lookup_.find(id); at != lookup_.end()) rows_[at->second].row.reveal();
    }
    bool rename(vng::u32 id,std::string_view name) {
        const auto at=lookup_.find(id);if(at==lookup_.end())return false;
        auto& row=rows_[at->second];if(row.name==name)return false;
        row.name=name;update_label(row);return true;
    }
    void sync(std::span<const SceneInstance> instances,std::span<const Blueprint> blueprints) {
        ++stats_.syncs;
        std::unordered_set<vng::u32> live;
        const auto accepts = [&](const SceneInstance& instance) {
            const bool region = std::holds_alternative<RegionSettings>(instance.settings);
            return filter_ == Filter::all || (filter_ == Filter::regions ? region : !region);
        };
        for(const auto& instance:instances)if(accepts(instance))live.insert(instance.id);
        std::erase_if(rows_,[&](auto& row){if(live.contains(row.id))return false;row.row.remove();return true;});
        lookup_.clear();
        for(std::size_t i=0;i<rows_.size();++i)lookup_.emplace(rows_[i].id,i);
        std::unordered_map<BlueprintId,std::string_view> names;
        for(const auto& blueprint:blueprints)names.emplace(blueprint.id,blueprint.name);
        for(const auto& instance:instances) {
            if (!accepts(instance)) continue;
            bool added{};
            auto at=lookup_.find(instance.id);
            if(at==lookup_.end()) {
                auto row=host_.column().height(56).padding(0).gap(0);
                at=lookup_.emplace(instance.id,rows_.size()).first;
                rows_.push_back({instance.id,row,row.button("").height(32),row.label("").height(24),{}, {}});
                added=true;++stats_.rows_created;
            }
            auto& row=rows_[at->second];
            if(added||row.name!=instance.name){row.name=instance.name;update_label(row);}
            const auto found=names.find(instance.blueprint);
            const auto name=found==names.end()?std::string_view{"Missing blueprint"}:found->second;
            if(added||row.blueprint_name!=name){row.blueprint_name=name;row.blueprint.text("Blueprint: "+row.blueprint_name);}
        }
    }
    void selection(const vng::editor::Selection<vng::u32>& next) {
        const auto change=next.changes_from(selection_);
        if(change.empty())return;
        std::unordered_set<vng::u32> changed(change.added.begin(),change.added.end());
        changed.insert(change.removed.begin(),change.removed.end());
        if(change.previous_active!=change.active) {
            if(change.previous_active)changed.insert(*change.previous_active);
            if(change.active)changed.insert(*change.active);
        }
        selection_=next;
        for(auto id:changed)if(auto at=lookup_.find(id);at!=lookup_.end())update_label(rows_[at->second]);
    }
private:
    void update_label(Entry& row) {
        row.button.text((selection_.active()==row.id?"> #":selection_.contains(row.id)?"+ #":"#")+
            std::to_string(row.id)+" "+row.name);++stats_.labels_updated;
    }
    vng::ui::Container host_;
    Filter filter_;
    std::vector<Entry> rows_;
    std::unordered_map<vng::u32,std::size_t> lookup_;
    vng::editor::Selection<vng::u32> selection_;
    Stats stats_;
};
} // namespace editor_example
