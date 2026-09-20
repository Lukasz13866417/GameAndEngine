#pragma once
#include "blueprint_mesh_controls.hpp"
#include "editing_session.hpp"
#include "inspector_panel.hpp"
#include <vng/gfx/camera.hpp>
#include <future>

namespace editor_example {
// Local blueprint authoring, independent of worker-side instance callbacks.
// Owns the declaration, UI adapter and one CPU job; EditingSession alone owns
// the document/history. Background jobs only see an owned mesh snapshot.
class BlueprintMeshPanel final {
public:
    BlueprintMeshPanel(vng::ui::Container, EditingSession&);
    BlueprintMeshPanel(const BlueprintMeshPanel&)=delete;
    BlueprintMeshPanel& operator=(const BlueprintMeshPanel&)=delete;
    BlueprintMeshPanel(BlueprintMeshPanel&&)=delete;
    BlueprintMeshPanel& operator=(BlueprintMeshPanel&&)=delete;
    void sync();
    void enabled(bool);
    // Poll after Screen::update(). Returns true only when a draft was committed.
    [[nodiscard]] bool poll(bool accept_input);
    [[nodiscard]] std::string_view status() const { return status_; }
    [[nodiscard]] bool busy() const { return job_.valid() || gesture_source_ != nullptr; }
    [[nodiscard]] std::optional<SurfaceMove> gizmo() const;
    [[nodiscard]] vng::content::Result<bool> edit_part(const SurfacePartAction&);
    [[nodiscard]] vng::u32 selected_part() const { return selected_part_; }
    [[nodiscard]] bool select_part(vng::u32);
    // Uses the mesh's shared BVH: ground and near-side clouds occlude far-side
    // formations. No projected marker list or per-cloud geometry copy.
    [[nodiscard]] vng::u32 pick_part(vng::Vec2 normalized, const vng::gfx::CameraSnapshot&) const;
    // Wait for the rendered image, not merely CPU completion or worker ACK.
    [[nodiscard]] bool pending(vng::u64 presented_revision) const {
        return job_.valid() || queued_.has_value() ||
            (shown_ == pending_blueprint_ && presented_revision < pending_revision_);
    }
private:
    void start(MeshDraftEdit);
    void launch(MeshDraftEdit);
    [[nodiscard]] vng::content::Result<bool> finish_gesture(bool cancel);
    vng::ui::Container host_;
    vng::ui::Label hint_;
    vng::ui::Container parts_host_;
    std::optional<vng::ui::Dropdown<vng::u32>> parts_;
    vng::u32 selected_part_{};
    std::vector<MeshPart> declared_parts_;
    std::string part_field_;
    EditingSession& editing_;
    InspectorPanel panel_;
    std::optional<vng::editor::Inspector> description_;
    std::optional<MeshPartGizmo> gizmo_;
    std::optional<BlueprintId> shown_;
    vng::u64 revision_{};
    std::future<vng::content::Result<vng::editor::EditableMesh>> job_;
    BlueprintId job_blueprint_{};
    vng::u64 job_revision_{};
    std::shared_ptr<const vng::editor::EditableMesh> gesture_source_;
    std::optional<MeshDraftEdit> queued_;
    bool finishing_{}, discard_job_{}, job_gesture_{};
    bool job_select_new_part_{};
    std::optional<BlueprintId> pending_blueprint_;
    vng::u64 pending_revision_{};
    std::string status_;
};
}
