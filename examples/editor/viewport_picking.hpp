#pragma once

#include "viewport_context.hpp"

namespace editor_example {
// Same host evidence as interaction routing, plus the pre-tool handle focus.
// A selected handle delays scene selection until a completed outside click.
struct ViewportSelectionContext {
    const ViewportInputContext& frame;
    bool selected_gizmo_handle{};
};
struct ViewportSelectionReply {
    std::optional<WorkspaceSelection::Change> selection;
    bool vertex_changed{}, viewport_changed{}, inspector_changed{}, blueprint_changed{};
    MeshEditingReply mesh;
    std::string status;
};

// A box gesture freezes its coordinate system and its initial selection.
// It belongs to the logical viewport, not to the application/window layout.
struct ViewportPickCapture {
    vng::editor::Selection<vng::u32> instances;
    std::vector<vng::u32> elements;
    vng::gfx::Camera camera;
    vng::Extent2D extent;
    vng::ui::Rect bounds;
    vng::u64 revision{};
    vng::f32 time{};
    ViewMode mode{};
    BlueprintId blueprint{};
    MeshSelectMode components{};
};
} // namespace editor_example
