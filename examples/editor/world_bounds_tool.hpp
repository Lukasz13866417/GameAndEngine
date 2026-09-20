#pragma once
#include "world_bounds.hpp"
#include <vng/gfx/camera.hpp>
#include <vng/input/input.hpp>
#include <vng/ui/draw_list.hpp>
#include <optional>
#include <array>
#include <span>

namespace editor_example {
struct BoundsAction {
    WorldBounds value;
    bool began{}, changed{}, finished{}, cancelled{};
};
// UI-side geometry only. The host owns the authoring transaction and transport.
class WorldBoundsTool {
public:
    [[nodiscard]] BoundsAction update(const WorldBounds&, const vng::gfx::CameraSnapshot&,
        vng::ui::Rect, std::span<const vng::input::Event> unhandled,
        std::span<const vng::input::Event> raw, bool visible, bool editable);
    void append(vng::ui::DrawList&, const vng::text::Font& = {}, bool boundary = true) const;
    // -X,+X,-Y,+Y,-Z,+Z, in that order. View-parallel faces have no drag handle.
    [[nodiscard]] std::optional<vng::Vec2> handle(unsigned face) const;
    [[nodiscard]] bool dragging() const { return dragging_; }
    [[nodiscard]] bool handledPointer() const { return handled_; }
    void cancel() { dragging_ = visible_ = false; }
private:
    struct Handle { vng::Vec2 point{}, direction{}; vng::f32 pixels{}, ratio{}; bool visible{}; };
    std::array<Handle, 6> handles_{};
    WorldBounds value_{}, initial_{};
    vng::gfx::CameraSnapshot camera_{};
    vng::ui::Rect viewport_{};
    vng::Vec2 start_{},pointer_{};
    Handle axis_{};
    unsigned face_{};
    bool dragging_{}, visible_{}, editable_{}, handled_{};
    void geometry();
    void move(vng::Vec2);
};
}
