#pragma once
#include "gizmo_mode.hpp"
#include "tool_options.hpp"
#include "translation_tool.hpp"
#include "rotation_tool.hpp"
#include "scale_tool.hpp"
#include <variant>
#include <vng/editor/inspector.hpp>
#include <vng/gfx/camera.hpp>
#include <vng/input/input.hpp>
#include <vng/ui/draw_list.hpp>

namespace editor_example {
struct TransformAction {
    bool began{}, changed{}, finished{}, cancelled{};
};
struct TransformGizmos {
    std::span<const GizmoMode> available{basic_transform_gizmos};
    std::optional<vng::Vec3> forward; // Evaluated world direction, captured on start.
    std::optional<vng::Vec3> local_scale_rotation; // Absent for world-space components.
};
// Mouse-following gizmo interaction, math and visuals. The host supplies a
// pivot/capabilities and owns the selected data, transaction and undo policy.
class TransformGesture : public ToolOptions {
public:
    void maximum_scale_factor(vng::f32 value) { maximum_factor_=value; }
    std::string_view title() const override { return gizmo_label(gizmo_); }
    bool options_available() const override { return active_; }
    void describe_options(vng::editor::Inspector&) override;
    static std::optional<GizmoMode> requested(std::span<const vng::input::Event>, vng::ui::Rect,
        std::span<const GizmoMode> available = basic_transform_gizmos);
    TransformAction update(vng::editor::Stamp, vng::Vec3 pivot, const vng::gfx::CameraSnapshot&,
        vng::ui::Rect, std::span<const vng::input::Event> unhandled,
        std::span<const vng::input::Event> raw, bool enabled, TransformGizmos = {}, float arrow_step = 1.F);
    void cancel() { active_ = false; requested_axis_.reset(); requested_finish_.reset(); }
    bool active() const { return active_; }
    bool handled() const { return handled_; }
    TransformKind kind() const { return kind_; }
    GizmoMode gizmo() const { return gizmo_; }
    bool visible() const;
    // Read-only inspection of the geometry actually being displayed.
    const TranslationTool* translation_gizmo() const { return active_ ? std::get_if<TranslationTool>(&visual_) : nullptr; }
    const RotationTool* rotation_gizmo() const { return active_ ? std::get_if<RotationTool>(&visual_) : nullptr; }
    const ScaleTool* scale_gizmo() const { return active_ ? std::get_if<ScaleTool>(&visual_) : nullptr; }
    vng::Vec3 translation() const { return delta_; }
    vng::Vec3 rotation() const { return angles_; } // World-space rotation delta, not an object's Euler angles.
    vng::f32 factor() const { return factor_; }
    int axis() const { return axis_; }
    void append(vng::ui::DrawList&, const vng::text::Font&) const;
private:
    TransformKind kind_{TransformKind::move};
    GizmoMode gizmo_{GizmoMode::move};
    // These are the same visuals used for pointer-drag gizmos, driven without
    // input here. The modal gesture remains the sole interaction owner.
    std::variant<TranslationTool,RotationTool,ScaleTool> visual_;
    vng::editor::Stamp stamp_{};
    vng::gfx::CameraSnapshot camera_{};
    vng::ui::Rect viewport_{};
    vng::Vec3 delta_{}, angles_{}, horizontal_{}, vertical_{};
    vng::Vec3 base_delta_{},base_angles_{};
    vng::f32 base_factor_{1};
    vng::Vec3 pivot_{}, forward_{}, scale_axes_rotation_{};
    vng::Vec2 pointer_{}, start_pointer_{}, center_{};
    vng::Vec2 keyboard_pixels_{};
    double keyboard_angle_{};
    vng::f32 factor_{1};
    vng::f32 maximum_factor_{1000};
    int axis_{-1};
    bool active_{}, handled_{}, local_scale_{};
    std::optional<int> requested_axis_;
    std::optional<bool> requested_finish_; // true cancels; false confirms.
    void motion(vng::Vec2);
    void reframe(const vng::gfx::CameraSnapshot&);
    void update_visual();
};
}
