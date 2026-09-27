#pragma once
#include "surface_move.hpp"
#include <vng/editor/inspector.hpp>
#include <vng/editor/mesh.hpp>
#include <functional>
#include <memory>
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
struct MeshPartId {
    std::string field;
    vng::u32 value{};
    explicit operator bool() const {return !field.empty()&&value!=0;}
    friend bool operator==(const MeshPartId&,const MeshPartId&)=default;
};
struct MeshPart {MeshPartId id;std::string label;};
struct MeshPartHandle {
    SurfaceMove surface;
    std::function<MeshDraftEdit(vng::Vec3)> move;
    std::function<MeshDraftEdit(vng::f32)> rotate;
    std::function<MeshDraftEdit()> erase{};
    std::function<MeshDraftEdit(vng::f32)> scale{};
};
// A blueprint-owned setup operation has local handles/options, but does not
// author a document until it submits a command. The host only routes gestures.
class MeshPartGizmoSetup {
public:
    virtual ~MeshPartGizmoSetup()=default;
    virtual std::vector<MeshPartHandle> handles() const=0;
    virtual void begin()=0;
    virtual void move(std::size_t,vng::Vec3)=0;
    virtual void finish(bool cancel)=0;
    virtual void describe(vng::editor::Inspector&,SubmitMeshDraftEdit)=0;
};
struct MeshPartGizmo {
    std::string label;
    std::vector<MeshPartHandle> handles;
    std::function<void(vng::editor::Inspector&,SubmitMeshDraftEdit)> options{};
    std::shared_ptr<MeshPartGizmoSetup> setup{};
};
struct MeshPartPlacement {
    std::string label;
    vng::Mat4 frame{vng::Mat4::identity()};
    std::function<MeshDraftEdit(vng::Vec3)> place;
};
// Blueprint-defined discrete edits. The host knows labels and a draft command,
// not the blueprint's connection model or other property semantics.
struct MeshPartChoice {
    std::string label;
    std::vector<std::string> options;
    std::size_t selected{};
    std::function<MeshDraftEdit(std::size_t)> choose;
};
struct MeshPartSlot {
    SurfaceMove marker;
    std::function<MeshDraftEdit()> attach;
};
struct MeshPartSocketTarget {
    MeshPartId part;
    std::vector<MeshPartSlot> slots;
};
struct MeshPartConnection {
    std::string label;
    std::vector<MeshPartSocketTarget> targets;
};
struct MeshEditMenu {
    std::string label;
    std::function<void(vng::editor::Inspector&,SubmitMeshDraftEdit)> describe;
};
struct BlueprintMeshDescription {
    std::vector<MeshEditMenu> menus;
    std::vector<MeshPart> parts;
    std::string hint;
    // First gizmo is the default. Each mode may expose several pickable
    // handles; the host owns selection, never the recipe's meaning.
    std::vector<MeshPartGizmo> gizmos;
    std::vector<MeshPartPlacement> placements;
    std::vector<MeshPartChoice> choices;
    std::vector<MeshPartConnection> connections;
};
[[nodiscard]] vng::editor::Result<BlueprintMeshDescription> describe_blueprint_mesh(
    vng::editor::Inspector&, const vng::editor::EditableMesh&, SubmitMeshDraftEdit,
    MeshPartId selected_part = {});
}
