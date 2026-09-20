#pragma once
#include "surface_move.hpp"
#include <vng/editor/inspector.hpp>
#include <vng/editor/mesh.hpp>
#include <functional>
#include <optional>

namespace editor_example {
// Blueprint declarations provide controls and a CPU-only draft edit.
// The host supplies an owned draft when running it; recipes retain no session,
// UI, worker, GPU, or file references.
struct MeshDraftEdit {
    std::string label;
    std::function<vng::content::Result<vng::editor::EditableMesh>(const vng::editor::EditableMesh&)> apply;
    bool select_new_part{};
};
using SubmitMeshDraftEdit=std::function<void(MeshDraftEdit)>;
// Blueprint-local editable parts are not scene instances. The generic host owns
// only their selection; the blueprint declares their vocabulary and edits.
struct MeshPart {vng::u32 id;std::string label;};
struct MeshPartGizmo {
    SurfaceMove surface;
    std::function<MeshDraftEdit(vng::Vec3)> move;
    std::function<MeshDraftEdit(vng::f32)> rotate;
};
struct BlueprintMeshDescription {
    std::vector<MeshPart> parts;
    std::string hint;
    std::optional<MeshPartGizmo> gizmo{};
    // UInt32 per-vertex ownership. Zero means the whole blueprint; a triangle
    // belongs to a part only when all three of its vertices agree.
    std::string part_field;
};
[[nodiscard]] vng::editor::Result<BlueprintMeshDescription> describe_blueprint_mesh(
    vng::editor::Inspector&, const vng::editor::EditableMesh&, SubmitMeshDraftEdit,
    vng::u32 selected_part = 0);
}
