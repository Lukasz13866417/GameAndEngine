#pragma once
#include "project.hpp"
#include <vng/ui/ui.hpp>
#include <algorithm>
#include <optional>

namespace editor_example {
// Both sidebar and flyout use this view. Creating/editing blueprints is an
// application action; this class owns only widgets and stable blueprint IDs.
class BlueprintList {
public:
    struct Entry {
        BlueprintId id;
        vng::ui::Container row;
        vng::ui::Button edit, add;
    };
    struct Action { BlueprintId id; bool create_instance; };
    explicit BlueprintList(vng::ui::Container host) : host_(host) {}
    void sync(std::span<const Blueprint> blueprints) {
        bool added{};
        std::erase_if(rows_, [&](auto& row) {
            if (std::ranges::find(blueprints, row.id, &Blueprint::id) != blueprints.end()) return false;
            row.row.remove(); return true;
        });
        for (const auto& blueprint : blueprints) {
            auto found = std::ranges::find(rows_, blueprint.id, &Entry::id);
            if (found == rows_.end()) {
                added = true;
                auto row = host_.row().height(34).padding(0).gap(4);
                rows_.push_back({blueprint.id, row, row.button("").height(34), row.button("+").width(38).height(34)});
                found = std::prev(rows_.end());
            }
            found->edit.text("Edit " + std::string(blueprint.name));
        }
        if (added) row_width_.reset();
        layout();
    }
    void layout() {
        if (rows_.empty()) return;
        const auto width = rows_.front().row.bounds().width;
        if (row_width_ == width) return;
        row_width_ = width;
        // Reserve the add button even for long blueprint names. Row bounds
        // already exclude the list's scrollbar and padding.
        for (auto& row : rows_) row.edit.width(std::max(1.F, width - 42));
    }
    std::optional<Action> poll() const {
        for (const auto& row : rows_) {
            if (row.add.clicked()) return Action{row.id, true};
            if (row.edit.clicked()) return Action{row.id, false};
        }
        return {};
    }
private:
    vng::ui::Container host_;
    std::vector<Entry> rows_;
    std::optional<vng::f32> row_width_;
};
}
