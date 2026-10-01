#pragma once
#include "project.hpp"
#include "selection_input_logic.hpp"
#include <vng/editor/selection.hpp>
#include <vng/ui/ui.hpp>
#include <unordered_map>
#include <unordered_set>
#include <cctype>

namespace editor_example {
// Owns widgets and their ID lookup. Document data is borrowed at sync(), not
// retained through pointers. Selection-only updates touch the changed rows.
class InstanceList {
public:
    struct Entry {vng::u32 id;vng::ui::Container row;vng::ui::Button button;vng::ui::Label blueprint;std::string name,blueprint_name;};
    struct Stats {std::size_t syncs{},rows_created{},labels_updated{};};
    enum class Filter { all, scene, regions };
    explicit InstanceList(vng::ui::Container host, Filter filter = Filter::all):host_(host),filter_(filter) {
        search_bar_=host_.row().height(32).padding(0).gap(4);
        search_=search_bar_.text_input("Search instances").placeholder("Name, blueprint or #ID").height(32);
        clear_=search_bar_.button("Clear").width(76).height(32);
        empty_=host_.label("No matching instances").height(28).visible(false);
    }
    std::string_view query() const { return query_; }
    std::size_t matches() const { return matches_; }
    void layout() { search_.width(std::max(1.F,search_bar_.bounds().width-80)); }
    void search(std::string_view query) {
        if(query_==query)return;
        query_=query;search_.value(query_);normalized_=lower(query_);
        filter_rows();host_.scroll(0);
    }
    bool contains(vng::u32 id) const {return lookup_.contains(id);}
    std::span<const Entry> entries() const {return rows_;}
    Stats stats() const {return stats_;}
    struct Pick {vng::u32 id; vng::editor::SelectionMode mode;};
    std::optional<Pick> poll(std::span<const vng::input::Event> events) {
        if(auto changed=search_.changedText())search(*changed);
        if(clear_.clicked()||search_.editCancelled())search("");
        for (const auto& row : rows_) if (row.button.clicked() && matches(row))
            return Pick{row.id, click_selection(click_modifiers(events, row.button.bounds()), true)};
        return {};
    }
    void reveal(vng::u32 id) {
        if (const auto at = lookup_.find(id); at != lookup_.end() && matches(rows_[at->second])) rows_[at->second].row.reveal();
    }
    bool rename(vng::u32 id,std::string_view name) {
        const auto at=lookup_.find(id);if(at==lookup_.end())return false;
        auto& row=rows_[at->second];if(row.name==name)return false;
        row.name=name;update_label(row);filter_rows();return true;
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
        filter_rows();
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
    static std::string lower(std::string_view value) {
        std::string result(value);
        for(auto& c:result)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return result;
    }
    bool matches(const Entry& row) const {
        if(normalized_.empty())return true;
        const auto text=lower(row.name+" "+row.blueprint_name+" #"+std::to_string(row.id));
        // Space-separated words may match different fields; punctuation is literal.
        for(std::size_t start=0;start<normalized_.size();) {
            start=normalized_.find_first_not_of(" \t",start);if(start==std::string::npos)break;
            const auto end=normalized_.find_first_of(" \t",start);
            if(text.find(normalized_.substr(start,end==std::string::npos?end:end-start))==std::string::npos)return false;
            if(end==std::string::npos)break;
            start=end;
        }
        return true;
    }
    void filter_rows() {
        matches_=0;
        for(auto& row:rows_){const bool show=matches(row);row.row.visible(show);matches_+=show;}
        empty_.visible(matches_==0&&!query_.empty());
    }
    void update_label(Entry& row) {
        row.button.text((selection_.active()==row.id?"> #":selection_.contains(row.id)?"+ #":"#")+
            std::to_string(row.id)+" "+row.name);++stats_.labels_updated;
    }
    vng::ui::Container host_,search_bar_;
    Filter filter_;
    vng::ui::TextField search_;
    vng::ui::Button clear_;
    vng::ui::Label empty_;
    std::string query_,normalized_;
    std::size_t matches_{};
    std::vector<Entry> rows_;
    std::unordered_map<vng::u32,std::size_t> lookup_;
    vng::editor::Selection<vng::u32> selection_;
    Stats stats_;
};
} // namespace editor_example
