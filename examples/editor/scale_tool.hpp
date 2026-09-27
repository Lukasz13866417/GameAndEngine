#pragma once
#include "scale_limits.hpp"
#include <vng/editor/inspector.hpp>
#include <vng/gfx/camera.hpp>
#include <vng/input/input.hpp>
#include <vng/ui/draw_list.hpp>
#include <span>
#include <array>
#include "gizmo_handle_selection.hpp"

namespace editor_example {
struct ScaleAction {
    vng::f32 value{1};
    bool began{}, changed{}, finished{}, cancelled{};
};
// The square handle edits overall size, preserving local-axis proportions.
// Axis-specific scaling is exposed by S X/Y/Z and the instance inspector.
class ScaleTool {
public:
    void maximum(vng::f32 value) { maximum_=value; }
    [[nodiscard]] ScaleAction update(vng::editor::Stamp, vng::Vec3 position, vng::f32 scale,
        const vng::gfx::CameraSnapshot&, vng::ui::Rect,
        std::span<const vng::input::Event> unhandled,
        std::span<const vng::input::Event> raw, bool enabled, vng::Vec3 axes_rotation = {}, float arrow_step = 1.F,
        bool shortcut=false);
    void append(vng::ui::DrawList&, int axis = -1) const;
    void cancel(bool clear_selection=true) { dragging_=visible_=keyboard_=pointer_keyboard_=false; if(clear_selection)selection_.clear(); }
    bool selected() const { return selection_.axis().has_value(); }
    void clear_selection() { selection_.clear(); }
    [[nodiscard]] bool dragging() const { return dragging_; }
    [[nodiscard]] bool visible() const { return visible_; }
    [[nodiscard]] bool handledPointer() const { return handled_; }
    [[nodiscard]] vng::Vec2 handle() const { return handle_; }
private:
    vng::editor::Stamp stamp_{};
    vng::ui::Rect viewport_{};
    vng::Vec2 origin_{},handle_{},start_{},pointer_{};
    vng::Mat4 projection_{};
    std::array<vng::Vec2,3> axis_tips_{};
    vng::f32 initial_{1},value_{1};
    vng::f32 maximum_{3};
    bool dragging_{},visible_{},handled_{};
    bool keyboard_{};
    bool pointer_keyboard_{};
    GizmoHandleSelection selection_;
};
}
