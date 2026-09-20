#pragma once
#include "editing_session.hpp"
#include "tool_options.hpp"

namespace editor_example {
// Editable last-operation parameters, not a second undo stack. The session owns
// the original mesh, revision guard, re-evaluation and single history entry.
class MeshOperationTool final : public ToolOptions {
public:
    explicit MeshOperationTool(EditingSession& editing) : editing_(editing) {}
    void observe(BlueprintId blueprint, MeshOperation operation) {
        blueprint_ = blueprint;
        operation_ = operation;
        settings_ = {};
        revision_ = editing_.state().document.revision;
    }
    std::string_view title() const override {
        return operation_ == MeshOperation::subdivide ? "Subdivide" :
               operation_ == MeshOperation::align ? "Align to line" : "Make edge / face";
    }
    bool options_available() const override {
        const auto& state = editing_.state();
        return state.viewport.mode == ViewMode::mesh && state.viewport.inspected_mesh == blueprint_ &&
            editing_.can_adjust_mesh_operation(revision_);
    }
    void describe_options(vng::editor::Inspector& ui) override {
        if (operation_ != MeshOperation::fill) {
            auto edit = ui.edit("operation", settings_, "Operation settings");
            if (operation_ == MeshOperation::subdivide)
                edit.field("levels", &MeshOperationSettings::levels, "Subdivision levels");
            if (operation_ == MeshOperation::align)
                edit.slider("strength", &MeshOperationSettings::strength, 0, 1, "Strength");
            edit.apply("Update operation", [this](const MeshOperationSettings& next) -> vng::editor::Result<void> {
                auto changed = editing_.adjust_mesh_operation(revision_, next);
                if (!changed) return std::unexpected(vng::editor::Diagnostic{changed.error().message});
                settings_ = next;
                revision_ = editing_.state().document.revision;
                return {};
            });
        }
        ui.action("undo", [this]() -> vng::editor::Result<void> {
            if (!options_available()) return std::unexpected(vng::editor::Diagnostic{"The operation is no longer current"});
            auto undone = editing_.undo();
            if (!undone) return std::unexpected(vng::editor::Diagnostic{undone.error().message});
            return {};
        }, "Undo operation");
    }
private:
    EditingSession& editing_;
    BlueprintId blueprint_{};
    MeshOperation operation_{MeshOperation::subdivide};
    MeshOperationSettings settings_;
    vng::u64 revision_{};
};
}
