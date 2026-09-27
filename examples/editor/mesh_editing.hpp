#pragma once
#include "mesh_tools.hpp"
#include "mesh_operation_tool.hpp"
#include "workspace_situation.hpp"

namespace editor_example {
// Routed input, not component state or a new set of situations. Spans are
// consumed synchronously and never retained. Multiple selection operations in
// one call are ordered (e.g. restore box origin, then add the picked set).
struct MeshSelectionInput {
    std::span<const vng::u32> elements{};
    vng::editor::SelectionMode mode{vng::editor::SelectionMode::replace};
};
struct MeshMenuPlacement {
    vng::Vec2 at, screen;
    std::optional<vng::ui::Rect> viewport;
};
struct MeshInput {
    std::optional<MeshSelectMode> mode{};
    std::optional<GizmoMode> gizmo{};
    int cycle_gizmo{};
    std::span<const MeshSelectionInput> selection{};
    std::optional<bool> select_all{}; // false: all, true: clear.
    std::optional<MeshMenuPlacement> open_menu{};
    std::optional<MeshAction> operation{};
    std::span<const vng::input::Event> menu_events{};
    std::span<const vng::input::Event> shortcuts{};
    bool poll_menu{}, close_menu{}, reset{};
};
struct MeshEditingContext {
    MeshInput input{};
    bool accept_input{true};
};
// Owned proposal: the workspace may execute it before delivering the next
// input event. No borrowed selection survives a topology change.
struct MeshEditProposal {
    BlueprintId blueprint{};
    MeshOperation operation{};
    std::vector<vng::u32> vertices;
    std::vector<vng::gfx::Edge> edges;
};
struct MeshEditingReply {
    bool authored{}, visibility_changed{}, selection_changed{}, mode_changed{}, gizmo_changed{}, clear_part_selection{}, camera_changed{};
    ToolOptions* operation_options{}; // Narrow declaration capability, not a child component.
    std::string message{};
    std::optional<MeshEditProposal> proposal{};
    std::optional<MeshOperationAdjustment> adjustment{};
};

class MeshEditing final {
public:
    MeshEditing(const EditingSession& editing, vng::ui::Container controls, vng::ui::Container popup)
        : editing_(editing), components_(controls, popup), operation_(editing) {}
    // Borrowed read-only geometry/selection evidence for overlays and picking.
    // Mutation is routed by the owning viewport, never through this view.
    [[nodiscard]] const MeshTools& components() const { return components_; }
    [[nodiscard]] DebugReport debug_report() const;
    [[nodiscard]] std::string debug_string() const { return debug_report().string(); }
private:
    friend class EditingViewport;
    MeshEditingReply handle(const InspectMesh&, const MeshEditingContext&);
    MeshEditingReply handle(const InspectScene&, const MeshEditingContext&);
    MeshEditingReply handle(const InspectEffect&, const MeshEditingContext&);
    MeshEditingReply suspend(const MeshEditingContext&);
    void operation(MeshAction, MeshEditingReply&);
    MeshEditingReply accept_operation(const MeshEditProposal&, const vng::content::Result<bool>&);
    MeshEditingReply accept_adjustment(const MeshOperationAdjustment&, const vng::content::Result<bool>&);
    [[nodiscard]] std::optional<MeshOperationAdjustment> take_adjustment() { return operation_.take_adjustment(); }
    const EditingSession& editing_;
    MeshTools components_;
    MeshOperationTool operation_;
    std::string_view situation_{"Not dispatched"};
    bool input_allowed_{};
    std::optional<BlueprintId> target_{};
    std::string last_result_;
};
} // namespace editor_example
