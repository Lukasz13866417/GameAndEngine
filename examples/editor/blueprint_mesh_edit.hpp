#pragma once
#include "editing_session.hpp"

namespace editor_example {
// An owned result/proposal. The panel may compute it, but only its coordinating
// parent executes it against the authoring authority.
struct BlueprintMeshEdit {
    enum class Kind { begin, preview, replace, commit, cancel };
    Kind kind{};
    BlueprintId blueprint{};
    vng::u64 source_revision{};
    std::optional<vng::editor::EditableMesh> mesh;
    std::optional<vng::u64> transaction{};
};

[[nodiscard]] inline vng::content::Result<bool> apply_blueprint_mesh_edit(
    EditingSession& editing, BlueprintMeshEdit& edit) {
    using Kind = BlueprintMeshEdit::Kind;
    const auto invalid = [](std::string message) -> vng::content::Result<bool> {
        vng::content::Diagnostic error;error.message=std::move(message);return std::unexpected(std::move(error));
    };
    if (edit.kind==Kind::begin || edit.kind==Kind::preview || edit.kind==Kind::replace) {
        const auto& state=editing.state();
        if (state.viewport.mode!=ViewMode::mesh || state.viewport.inspected_mesh!=edit.blueprint ||
            state.document.revision!=edit.source_revision)
            return invalid("Stale blueprint edit: target or source revision changed");
    }
    if (edit.kind==Kind::begin) {
        auto begun=editing.begin_mesh_draft_edit(edit.blueprint);
        if (!begun) return std::unexpected(begun.error());
        return false;
    }
    if (edit.kind==Kind::replace) {
        if (!edit.mesh) return invalid("Blueprint replacement has no mesh");
        return editing.replace_mesh_draft(edit.blueprint,edit.source_revision,std::move(*edit.mesh));
    }
    if (!editing.active(EditGesture::mesh_draft) || editing.active_blueprint()!=edit.blueprint ||
        !edit.transaction || editing.active_transaction()!=edit.transaction) {
        return invalid("Blueprint edit no longer owns the active transaction");
    }
    if (edit.kind==Kind::preview) {
        if (!edit.mesh) return invalid("Blueprint preview has no mesh");
        return editing.preview_mesh_draft(edit.source_revision,std::move(*edit.mesh));
    }
    return edit.kind==Kind::cancel ? editing.cancel() : editing.commit();
}
}
