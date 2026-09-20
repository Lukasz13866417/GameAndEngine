#pragma once
#include "blueprint_gizmos.hpp"
#include <vng/ui/ui.hpp>
#include <algorithm>

namespace editor_example {
// Selection capabilities own the menu. Rebuild only when its choices change,
// retaining a supported mode and falling back to Move otherwise.
class GizmoSelector {
public:
    explicit GizmoSelector(vng::ui::Container host) : host_(host) {}
    void show(const State& state, std::span<const vng::u32> selection) {
        auto common=selection_gizmos(state,selection);
        host_.visible(!common.empty());
        if(dropdown_ && common==common_)return;
        const auto previous=value();
        if(dropdown_) { dropdown_->remove(); dropdown_.reset(); }
        common_=std::move(common);
        if(common_.empty())return;
        std::vector<vng::ui::Choice<GizmoMode>> choices;
        for(auto mode:common_)choices.push_back({mode,gizmo_choice_label(mode)});
        dropdown_.emplace(host_.dropdown<GizmoMode>("Gizmo",std::move(choices)));
        value(previous);
    }
    [[nodiscard]] GizmoMode value() const { return dropdown_ ? dropdown_->value() : GizmoMode::move; }
    void value(GizmoMode mode) {
        if(dropdown_)dropdown_->value(std::ranges::find(common_,mode)!=common_.end() ? mode : GizmoMode::move);
    }
    void enabled(bool value) { host_.enabled(value); }
    bool cycle(int direction) {
        if (common_.empty() || direction == 0) return false;
        const auto previous = value();
        const auto at = std::ranges::find(common_, previous) - common_.begin();
        const auto count = static_cast<std::ptrdiff_t>(common_.size());
        value(common_[static_cast<std::size_t>((at + (direction > 0 ? 1 : -1) + count) % count)]);
        return value() != previous;
    }
    [[nodiscard]] std::optional<GizmoMode> changedValue() const { return dropdown_ ? dropdown_->changedValue() : std::nullopt; }
    [[nodiscard]] std::span<const GizmoMode> common() const { return common_; }
private:
    vng::ui::Container host_;
    std::optional<vng::ui::Dropdown<GizmoMode>> dropdown_;
    std::vector<GizmoMode> common_;
};
} // namespace editor_example
