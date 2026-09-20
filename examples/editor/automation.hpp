#pragma once

#include "project.hpp"
#include <functional>
#include <vng/input/input.hpp>
#include <vng/ui/ui.hpp>
#include <vng/resources/diagnostic.hpp>
#include <vng/editor/preview.hpp>

namespace editor_example {
class RotationTool;
class TranslationTool;
class CageTool;
class ScaleTool;
class MeshTools;
class WorldBoundsTool;
class SurfacePartTool;
// Read-only evidence from the actual editor loop, after composing a frame.
// References and capture callbacks are valid only during observe(). Actions
// enter through input(), never through direct document/widget mutations.
struct EditorObservation {
    const State& state;
    const vng::ui::Screen& screen;
    const vng::ui::DrawList& draw_list;
    vng::u64 frame{}, worker_generation{}, image_revision{};
    vng::Extent2D extent{}, preview_extent{};
    vng::ui::Rect viewport{};
    bool preview_ready{}, dirty{}, gizmo_visible{}, dragging{}, inspector_ready{};
    bool mesh_overlay_visible{};
    const RotationTool* rotation_gizmo{}; // Read-only ring geometry, valid during observe().
    const ScaleTool* scale_gizmo{};
    const TranslationTool* translation_gizmo{};
    int gizmo_axis{-1}; // -1: all features; otherwise the displayed keyboard constraint.
    const WorldBoundsTool* bounds_gizmo{};
    const CageTool* region_gizmo{};
    const MeshTools* mesh_tools{};
    bool playing{}, debug_link{}, modal{};
    std::optional<vng::f32> selected_keyframe;
    std::span<const vng::u32> selected_instances;
    std::span<const vng::f32> selected_keyframes;
    bool box_selecting{};
    std::string_view status, worker_logs;
    const vng::gfx::ImageData* preview_pixels{};
    std::function<vng::resources::Result<vng::gfx::ImageData>()> capture;
    const vng::editor::preview::FrameInfo* presented{};
    const vng::editor::InteractionTimings* timings{};
    bool viewport_detached{};
    const vng::ui::Screen* viewport_screen{};
    const vng::ui::DrawList* viewport_draw_list{};
    const vng::ui::Screen* viewport_popups{};
    std::function<vng::resources::Result<vng::gfx::ImageData>()> capture_viewport;
    const TranslationTool* rotation_origin_gizmo{};
    vng::Vec3 rotation_origin{};
    const SurfacePartTool* mesh_part_gizmo{};
    bool blueprint_pending{};
};

// Optional example-level input driver. Production runs do not construct it,
// inspect UI trees, or read back screenshots. A driver may throw on failure;
// the normal RAII path still shuts down the editor and its worker.
class Automation {
public:
    virtual ~Automation() = default;
    virtual void input(vng::input::Frame&) = 0;
    virtual void viewport_input(vng::input::Frame&) {}
    virtual void observe(const EditorObservation&) = 0;
    [[nodiscard]] virtual bool finished() const = 0;
};
} // namespace editor_example
