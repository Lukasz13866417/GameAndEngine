#pragma once
#include "translation_tool.hpp"
#include "rotation_tool.hpp"
#include "scale_tool.hpp"
#include "transform_gesture.hpp"
#include <vng/editor/scene_cage.hpp>

namespace editor_example {
struct ComponentChange {
    std::vector<vng::editor::ScenePoint> points;
    bool began{}, changed{}, finished{}, cancelled{};
};
// Shared interaction math for mesh and boundary components. Captures only the
// selected points, not a document. The host owns commit, rollback and history.
class ComponentTransform {
public:
    void scale_limits(const ScaleLimits& limits) { modal_.maximum_scale_factor(limits.factor);scale_.maximum(limits.factor); }
    ComponentChange update(std::span<const vng::editor::ScenePoint>, vng::editor::Stamp,
        const vng::gfx::CameraSnapshot&, vng::ui::Rect,
        std::span<const vng::input::Event> unhandled, std::span<const vng::input::Event> raw,
        bool enabled, std::optional<vng::Vec3> pivot = {}, float arrow_step = 1.F);
    void kind(TransformKind value) { gizmo(transform_gizmo(value)); }
    TransformKind kind() const {return kind_;}
    void gizmo(GizmoMode value) { if(!active()) { kind_=gizmo_transform_kind(value);free_rotation_=value==GizmoMode::free_rotate; } }
    GizmoMode gizmo() const { return free_rotation_ ? GizmoMode::free_rotate : transform_gizmo(kind_); }
    bool active() const {return active_ || modal_.active();}
    bool visible() const {return modal_.active()?modal_.visible():kind_==TransformKind::move?move_.visible():
        kind_==TransformKind::rotate?rotate_.visible():scale_.visible();}
    bool handled() const {return handled_;}
    ToolOptions* tool_options() {return modal_.active()?&modal_:nullptr;}
    void cancel();
    void capabilities(std::span<const GizmoMode> value) { available_=value; }
    void append(vng::ui::DrawList&,const vng::text::Font& = {}) const;
    std::optional<vng::Vec2> handle(std::string_view axis) const {return move_.handle(axis);}
    [[nodiscard]] vng::Mat4 matrix() const;
private:
    TransformKind kind_{TransformKind::move};
    std::span<const GizmoMode> available_{basic_transform_gizmos};
    TranslationTool move_;
    RotationTool rotate_;
    ScaleTool scale_;
    TransformGesture modal_;
    std::vector<vng::editor::ScenePoint> start_;
    vng::editor::Stamp stamp_{};
    vng::gfx::CameraSnapshot camera_{};
    vng::ui::Rect viewport_{};
    vng::Vec3 pivot_{},delta_{},angles_{};
    float factor_{1};
    int axis_{-1};
    bool active_{},handled_{},free_rotation_{};
    void capture(std::span<const vng::editor::ScenePoint>,vng::editor::Stamp,
        const vng::gfx::CameraSnapshot&,vng::ui::Rect,vng::Vec3);
    std::vector<vng::editor::ScenePoint> transformed() const;
};
}
