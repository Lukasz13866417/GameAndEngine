#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <vng/editor/inspector.hpp>
#include <vng/gfx/camera.hpp>
#include <vng/input/input.hpp>
#include <vng/ui/draw_list.hpp>

namespace editor_example {
// UI-side world XYZ + explicitly described translation axes for the first gizmo.
// Dragging exposes a local preview position; release returns an atomic Apply.
// The host decides how to stream previews and commit/cancel one history gesture.
class TranslationTool final {
public:
    [[nodiscard]] std::optional<vng::editor::Event>
    update(const vng::editor::Schema&, const vng::gfx::CameraSnapshot&, vng::ui::Rect viewport,
           std::span<const vng::input::Event> unhandled, std::span<const vng::input::Event> raw,
           bool enabled, bool world_axes = true, float arrow_step = 1.F);
    // axis=-1 draws all handles; 0/1/2 draws only that world-axis feature.
    void append(vng::ui::DrawList&, const vng::text::Font& = {}, int axis = -1) const;
    // Read-only geometry for inspection/automation: X/Y/Z or a custom label.
    // A view-parallel axis has no handle.
    [[nodiscard]] std::optional<vng::Vec2> handle(std::string_view label, bool reverse = false) const;
    [[nodiscard]] bool dragging() const noexcept { return dragging_; }
    [[nodiscard]] bool visible() const noexcept { return visible_; }
    // Pure observation: reading/copying this value never emits an event. Null
    // after commit/cancel; update() still returns the exact final release value.
    [[nodiscard]] std::optional<vng::Vec3> preview_position() const noexcept {
        return dragging_ ? std::optional{ghost_} : std::nullopt;
    }
    // True for this update if a gizmo consumed a pointer gesture, including
    // a press+release or cancellation contained entirely in the same frame.
    [[nodiscard]] bool handledPointer() const noexcept { return handled_; }
    void cancel() noexcept;

private:
    struct Axis {
        vng::Vec3 world{};
        vng::Vec2 direction{};
        vng::f32 pixels_per_unit{}, perspective_ratio{};
        vng::f32 length{45};
        std::string label;
        bool custom{}, reverse{};
        bool visible{};
    };
    std::vector<Axis> axes_;
    std::vector<vng::editor::TranslationAxis> extra_axes_;
    vng::editor::Stamp stamp_{};
    std::string key_;
    vng::gfx::CameraSnapshot camera_{};
    vng::ui::Rect viewport_{};
    vng::Vec3 origin_{}, ghost_{}, segment_origin_{};
    vng::Vec3 keyboard_delta_{};
    vng::Vec2 screen_origin_{}, start_{}, pointer_{};
    Axis drag_axis_{};
    vng::u32 axis_{};
    bool visible_{}, dragging_{}, handled_{};
    bool world_axes_{true};
    bool keyboard_{};

    void geometry();
    void move(vng::Vec2);
};
} // namespace editor_example
