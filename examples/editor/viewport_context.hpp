#pragma once

#include "camera_gizmo.hpp"
#include "mesh_editing_ui.hpp"
#include "workspace_selection.hpp"

namespace editor_example {

// Host facts for one already-ordered input occurrence (or the final time tick).
// No callbacks, mutable sibling references, preview services or widget handles.
struct ViewportInputContext {
    struct Input {
        const vng::input::Frame& raw;
        std::span<const vng::input::Event> unhandled;
        double seconds{}, frame_seconds{};
        bool tick{}, keyboard_enabled{true}, shortcuts_enabled{true}, tool_menu{};
    } input;
    struct Presented {
        const vng::gfx::Camera& camera;
        vng::Extent2D extent;
        vng::ui::Rect bounds;
        vng::u64 generation{}, revision{}, minimum_overlay_revision{};
        bool time_current{}, delivery_ready{};
        const vng::editor::Schema* schema{};
    } presented;
    struct Navigation {
        CameraDragSpeeds drag_speeds;
        WalkSpeeds walk_speeds;
        // escape_claimed: a text edit, popup or open flyout had this frame's Escape.
        bool move_forward{}, enabled{true}, escape_claimed{}, numeric_active{};
    } navigation{};
    struct Tools {
        bool enabled{}, ready{}, components{}, diagnostic{}, worker_busy{}, toolbar_blocked{};
        bool scene_aids_visible{}, bounds_visible{};
    } tools{};
};

// Observed outcomes for host presentation/preview delivery. Authoring has
// already been executed by the workspace, never by a descendant callback.
struct ViewportInputReply {
    CameraPose previous_camera{};
    NavigationReply navigation{};
    bool navigation_was_dragging{}, selected_gizmo_handle{};
    bool pose_finished{}, geometry_changed{}, bounds_changed{}, overlay_changed{}, view_changed{}, gizmo_changed{};
    std::optional<WorkspaceSelection::Change> selection{};
    struct ScaleObservation { vng::u32 object{}; vng::f32 value{}; bool cancelled{}; };
    std::optional<ScaleObservation> scale{};
    MeshEditingReply mesh{};
    std::optional<vng::editor::Event> native{};
    std::string status{};
};

} // namespace editor_example
