#pragma once
#include "blueprint_gizmos.hpp"
#include "component_debug.hpp"
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
    void enabled(bool value) { enabled_=value;host_.enabled(value); }
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
    [[nodiscard]] DebugReport debug_report() const {
        std::string choices;
        for(auto mode:common_) {
            if(!choices.empty())choices+=", ";
            choices+=gizmo_choice_label(mode);
        }
        return {.name="gizmo_selector",.role="selection-compatible manipulation choices",
            .situation=common_.empty()?"No choices":"Available",
            .received={{"enabled",debug_bool(enabled_)}},
            .owned={{"visible",debug_bool(!common_.empty())},
                {"selected",common_.empty()?"none":gizmo_choice_label(value())},
                {"available choices",std::move(choices)}}};
    }
    [[nodiscard]] std::string debug_string() const { return debug_report().string(); }
private:
    vng::ui::Container host_;
    std::optional<vng::ui::Dropdown<GizmoMode>> dropdown_;
    std::vector<GizmoMode> common_;
    bool enabled_{true};
};
} // namespace editor_example
