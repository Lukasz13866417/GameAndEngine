#pragma once
#include "surface_move.hpp"
#include <vng/gfx/camera.hpp>
#include <vng/input/input.hpp>
#include <vng/ui/draw_list.hpp>
#include <optional>
#include <span>

namespace editor_example {
// Immediate local handle; the blueprint authoring host schedules geometry work.
class SurfaceMoveTool {
public:
    [[nodiscard]] SurfacePartAction update(const std::optional<SurfaceMove>&,
        const vng::gfx::CameraSnapshot&, vng::ui::Rect,
        std::span<const vng::input::Event> unhandled, std::span<const vng::input::Event> raw,
        bool visible, bool can_begin, float arrow_step = 1.F);
    void append(vng::ui::DrawList&, const vng::text::Font&, bool pending, double seconds) const;
    void cancel();
    [[nodiscard]] bool dragging() const { return dragging_; }
    [[nodiscard]] bool handled() const { return handled_; }
    [[nodiscard]] bool visible() const { return handle_.has_value(); }
    [[nodiscard]] std::optional<vng::Vec2> handle() const { return handle_; }
    [[nodiscard]] vng::Vec3 position() const { return ghost_; }
private:
    std::optional<SurfaceMove> target_;
    vng::gfx::CameraSnapshot camera_;
    vng::Mat4 inverse_frame_{vng::Mat4::identity()};
    vng::ui::Rect viewport_;
    vng::Vec3 ghost_{};
    vng::Vec2 offset_{},pointer_{};
    std::optional<vng::Vec2> handle_;
    bool dragging_{}, handled_{}, keyboard_{};
    [[nodiscard]] std::optional<vng::Vec2> project(vng::Vec3) const;
    [[nodiscard]] std::optional<vng::Vec3> on_surface(vng::Vec2) const;
    void geometry();
};
}
