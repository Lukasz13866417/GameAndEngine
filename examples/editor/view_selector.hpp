#pragma once

#include "project.hpp"
#include <vng/ui/ui.hpp>
#include <algorithm>
#include <map>
#include <set>

namespace editor_example {
// A destination carries the asset identity, not just the kind of preview.
// Selecting a mesh must never silently reuse the last selected scene instance.
struct ViewSelection {
    ViewMode mode{ViewMode::scene};
    BlueprintId blueprint{BlueprintId::mesh};
    friend bool operator==(const ViewSelection&, const ViewSelection&) = default;
};

[[nodiscard]] inline std::vector<vng::ui::Choice<ViewSelection>> view_choices(const State& state) {
    std::vector<vng::ui::Choice<ViewSelection>> choices{{{ViewMode::scene}, "Scene"}};
    for (const auto& blueprint : blueprint_catalog(state)) {
        if (blueprint.kind != BlueprintKind::mesh) continue;
        std::string name{blueprint.name};
        if (blueprint.id == BlueprintId::mesh) {
            const auto& metadata = state.document.mesh.document().metadata;
            if (const auto found = metadata.find("name"); found != metadata.end() &&
                !found->second.empty() && found->second.size() <= 256)
                name = found->second;
        }
        // Validated mesh names are UTF-8, but metadata may contain line breaks
        // or tabs. A menu option is always one line; leave the asset unchanged.
        for (auto& c : name)
            if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
        choices.push_back({{ViewMode::mesh, blueprint.id}, "Mesh: " + name});
    }
    // Separate assets can legitimately have the same user-facing name.
    std::map<std::string, std::size_t> counts;
    for (const auto& choice : choices) ++counts[choice.label];
    std::set<std::string> used;
    for (auto& choice : choices) {
        if (counts.at(choice.label) > 1) {
            const auto suffix = " #" + std::to_string(static_cast<vng::u32>(choice.value.blueprint));
            // Reserve the original names too: an asset can itself be named
            // "Cube #1", and must remain distinct from a disambiguated Cube.
            do { choice.label += suffix; }
            while (counts.contains(choice.label) || used.contains(choice.label));
        }
        used.insert(choice.label);
    }
    choices.push_back({{ViewMode::sun}, "Sun effect"});
    return choices;
}

// Keeps its toolbar slot stable and only replaces the dropdown when the asset
// catalog changes. Camera/vertex edits do not recreate widgets or close menus.
class ViewSelector {
public:
    ViewSelector(vng::ui::Container host, const State& state) : host_(host) { show(state); }

    void show(const State& state) {
        auto choices = view_choices(state);
        if (!dropdown_ || !std::ranges::equal(choices, choices_, [](const auto& a, const auto& b) {
                return a.value == b.value && a.label == b.label;
            })) {
            if (dropdown_) dropdown_->remove();
            choices_ = std::move(choices);
            dropdown_.emplace(host_.dropdown<ViewSelection>("View", choices_));
        }
        const ViewSelection selected{state.viewport.mode, state.viewport.mode == ViewMode::mesh
            ? state.viewport.inspected_mesh : BlueprintId::mesh};
        if (dropdown_->value() != selected) dropdown_->value(selected);
    }
    void enabled(bool enabled) { host_.enabled(enabled); }
    [[nodiscard]] std::optional<ViewSelection> changedValue() const { return dropdown_->changedValue(); }

private:
    vng::ui::Container host_;
    std::optional<vng::ui::Dropdown<ViewSelection>> dropdown_;
    std::vector<vng::ui::Choice<ViewSelection>> choices_;
};
} // namespace editor_example
