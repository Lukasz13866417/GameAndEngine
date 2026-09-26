#pragma once
#include "blueprint_mesh_controls.hpp"
#include "editing_session.hpp"
#include "inspector_panel.hpp"
#include "tool_options.hpp"
#include <vng/gfx/camera.hpp>
#include <future>

namespace editor_example {
// Local blueprint authoring, independent of worker-side instance callbacks.
// Owns the declaration, UI adapter and one CPU job; EditingSession alone owns
// the document/history. Background jobs only see an owned mesh snapshot.
class BlueprintMeshPanel final : public ToolOptions {
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
    [[nodiscard]] bool busy() const { return job_.valid() || gesture_source_ != nullptr || setup_gesture_; }
    [[nodiscard]] DebugReport debug_report() const {
        return {.name="blueprint_recipe",.role="local blueprint declarations and asynchronous CPU edits",
            .situation=placing()?"Placing":connecting()?"Connecting":busy()?"Editing":"Inspecting",
            .owned={{"blueprint",shown_?std::to_string(static_cast<vng::u32>(*shown_)):"none"},
                {"source revision",std::to_string(revision_)},{"selected part field",selected_part_.field},
                {"selected part ID",std::to_string(selected_part_.value)},{"selected gizmo",std::to_string(selected_gizmo_)},
                {"selected handle",std::to_string(selected_handle_)},{"selected menu",selected_menu_},
                {"declared parts",std::to_string(declared_parts_.size())},{"declared gizmos",std::to_string(declared_gizmos_.size())},
                {"job result outstanding",debug_bool(job_.valid())},{"job source revision",std::to_string(job_revision_)},
                {"latest edit queued",debug_bool(queued_.has_value())},{"gesture source held",debug_bool(gesture_source_!=nullptr)},
                {"finishing",debug_bool(finishing_)},{"discard job result",debug_bool(discard_job_)},
                {"pending rendered revision",std::to_string(pending_revision_)}},.observations={{"status",status_}}};
    }
    [[nodiscard]] std::string debug_string() const { return debug_report().string(); }
    [[nodiscard]] bool has_gizmo() const;
    std::string_view title() const override;
    bool options_available() const override;
    vng::u64 options_revision() const override {return options_revision_;}
    void describe_options(vng::editor::Inspector&) override;
    [[nodiscard]] bool erase_handle();
    [[nodiscard]] std::optional<SurfaceMove> gizmo() const;
    [[nodiscard]] std::vector<SurfaceMove> gizmo_handles() const;
    [[nodiscard]] std::size_t selected_handle() const { return selected_handle_; }
    [[nodiscard]] vng::content::Result<bool> edit_part(const SurfacePartAction&);
    [[nodiscard]] const MeshPartId& selected_part() const { return selected_part_; }
    [[nodiscard]] bool select_part(MeshPartId);
    [[nodiscard]] bool select_gizmo(std::size_t);
    [[nodiscard]] bool cycle_gizmo(int direction);
    [[nodiscard]] bool select_handle(std::size_t);
    [[nodiscard]] bool begin_placement(std::size_t);
    [[nodiscard]] bool placing() const {return placement_.has_value();}
    void cancel_placement();
    [[nodiscard]] bool place_part(vng::Vec2 normalized,const vng::gfx::CameraSnapshot&);
    [[nodiscard]] bool begin_connection(std::size_t);
    [[nodiscard]] bool connecting() const {return connection_.has_value();}
    void cancel_connection();
    [[nodiscard]] bool pick_connection_target(vng::Vec2,const vng::gfx::CameraSnapshot&);
    [[nodiscard]] std::vector<SurfaceMove> connection_slots() const;
    [[nodiscard]] bool attach_slot(std::size_t);
    // Uses the mesh's shared BVH: terrain and nearer geometry occlude far-side
    // parts. Field-qualified identities keep different part kinds independent.
    [[nodiscard]] MeshPartId pick_part(vng::Vec2 normalized, const vng::gfx::CameraSnapshot&) const;
    // Wait for the rendered image, not merely CPU completion or worker ACK.
    [[nodiscard]] bool pending(vng::u64 presented_revision) const {
        return job_.valid() || queued_.has_value() ||
            (shown_ == pending_blueprint_ && presented_revision < pending_revision_);
    }
private:
    void show_menu();
    void show_handles();
    void start(MeshDraftEdit);
    void launch(MeshDraftEdit);
    struct Hit {vng::u32 face;vng::Vec3 position;};
    [[nodiscard]] std::optional<Hit> hit(vng::Vec2,const vng::gfx::CameraSnapshot&) const;
    [[nodiscard]] vng::content::Result<bool> finish_gesture(bool cancel);
    vng::ui::Container host_;
    vng::ui::Label hint_;
    vng::ui::Container menu_buttons_host_,menu_body_;
    InspectorPanel menu_panel_;
    std::vector<MeshEditMenu> declared_menus_;
    std::vector<vng::ui::Button> menu_buttons_;
    std::string selected_menu_;
    std::optional<vng::editor::Inspector> menu_description_;
    vng::u64 menu_revision_{};
    vng::ui::Container parts_host_;
    std::optional<vng::ui::Dropdown<MeshPartId>> parts_;
    MeshPartId selected_part_{};
    std::optional<vng::ui::Dropdown<std::size_t>> gizmos_;
    std::size_t selected_gizmo_{};
    std::optional<vng::ui::Container> handles_host_;
    std::optional<vng::ui::Dropdown<std::size_t>> handles_;
    std::size_t selected_handle_{};
    bool setup_gesture_{};
    vng::u64 options_revision_{};
    std::vector<MeshPart> declared_parts_;
    std::vector<MeshPartGizmo> declared_gizmos_;
    std::vector<MeshPartPlacement> declared_placements_;
    std::vector<MeshPartChoice> declared_choices_;
    std::vector<vng::ui::Dropdown<std::size_t>> choice_controls_;
    std::vector<MeshPartConnection> declared_connections_;
    std::vector<vng::ui::Button> connection_buttons_;
    std::optional<vng::ui::Button> cancel_connection_button_;
    std::optional<std::size_t> connection_,connection_target_;
    std::vector<vng::ui::Button> placement_buttons_;
    std::optional<MeshPartPlacement> placement_;
    EditingSession& editing_;
    InspectorPanel panel_;
    std::optional<vng::editor::Inspector> description_;
    std::optional<MeshPartHandle> gizmo_;
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
