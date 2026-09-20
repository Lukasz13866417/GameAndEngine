#pragma once
#include <algorithm>
#include <optional>
#include <span>
#include <vector>
#include <unordered_set>

namespace vng::editor {
enum class SelectionMode { replace, add, toggle, remove, range };
// Ordered UI selection with one active item and a stable range anchor. No
// document, rendering or input dependency; identities belong to the caller.
template<class Id> class Selection {
public:
    struct Change {
        std::vector<Id> added,removed;
        std::optional<Id> previous_active,active;
        [[nodiscard]] bool empty() const {return added.empty()&&removed.empty()&&previous_active==active;}
    };
    [[nodiscard]] Change changes_from(const Selection& previous) const {
        Change result{{},{},previous.active_,active_};
        for(auto id:items_)if(!previous.contains(id))result.added.push_back(id);
        for(auto id:previous.items_)if(!contains(id))result.removed.push_back(id);
        return result;
    }
    [[nodiscard]] std::span<const Id> items() const { return items_; }
    [[nodiscard]] std::optional<Id> active() const { return active_; }
    [[nodiscard]] bool contains(Id id) const { return membership_.contains(id); }
    [[nodiscard]] std::size_t size() const { return items_.size(); }
    void clear() { items_.clear(); membership_.clear(); active_.reset(); anchor_.reset(); }
    void select(Id id, SelectionMode mode=SelectionMode::replace, std::span<const Id> order={}) {
        if(mode==SelectionMode::range && anchor_) {
            auto a=std::ranges::find(order,*anchor_), b=std::ranges::find(order,id);
            if(a!=order.end() && b!=order.end()) {
                const std::vector<Id> values(std::min(a,b),std::max(a,b)+1);
                const auto anchor=anchor_;select(values);anchor_=anchor;active_=id;return;
            }
        }
        if(mode==SelectionMode::replace || mode==SelectionMode::range) clear();
        if(mode==SelectionMode::remove || (mode==SelectionMode::toggle && contains(id))) {
            std::erase(items_,id);
            membership_.erase(id);
            if(active_==id) active_=items_.empty()?std::nullopt:std::optional{items_.back()};
        } else {
            if(membership_.insert(id).second) items_.push_back(id);
            active_=id;
        }
        anchor_=id;
    }
    void select(std::span<const Id> ids,SelectionMode mode=SelectionMode::replace) {
        // Linear membership work for large box selections. Calling the scalar
        // overload for every vertex would make a dense mesh quadratic. Own the
        // input too, since callers may pass items() from this very selection.
        const std::vector<Id> values(ids.begin(),ids.end());
        if(mode==SelectionMode::replace) clear();
        std::unordered_set<Id> visited;
        for(auto id:values) {
            if(!visited.insert(id).second) continue;
            if(mode==SelectionMode::remove || (mode==SelectionMode::toggle && membership_.contains(id))) {
                membership_.erase(id);
                if(active_==id) active_.reset();
            } else {
                if(membership_.insert(id).second) items_.push_back(id);
                active_=id;
            }
            anchor_=id;
        }
        std::erase_if(items_,[&](Id id){return !membership_.contains(id);});
        if(!active_ && !items_.empty()) active_=items_.back();
    }
    template<class Predicate> void retain(Predicate valid) {
        std::erase_if(items_,[&](Id id){if(valid(id))return false;membership_.erase(id);return true;});
        if(active_ && !contains(*active_)) active_=items_.empty()?std::nullopt:std::optional{items_.back()};
        if(anchor_ && !valid(*anchor_)) anchor_=active_;
    }
    friend bool operator==(const Selection&,const Selection&)=default;
private:
    std::vector<Id> items_;
    std::unordered_set<Id> membership_;
    std::optional<Id> active_,anchor_;
};
}
