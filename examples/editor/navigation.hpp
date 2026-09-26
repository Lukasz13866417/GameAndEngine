#pragma once

#include "project.hpp"
#include "component_debug.hpp"
#include <vng/input/input.hpp>
#include <span>

namespace editor_example {
// Turntable navigation in logical viewport pixels. Gesture capture starts only
// from unhandled input, then follows raw input across panels until release.
// Mutates the camera only: callers own revisioning and preview synchronization.
class NavigationTool {
public:
    enum class ScrollMode { zoom, move_forward };
    void scroll_mode(ScrollMode mode) { if (mode != scroll_mode_) cancel(); scroll_mode_ = mode; }
    void look_in_place(bool enabled) { look_in_place_ = enabled; }
    void speeds(CameraDragSpeeds speeds) { speeds_ = speeds; }
    void orbit_enabled(bool enabled) { orbit_enabled_=enabled; }
    // Optional displayed object center for MMB orbit, pan and dolly.
    // Supplying/changing it never reframes the camera. A gesture freezes it.
    // Optional borrowed mesh enables surface-aware approach speed. Refresh
    // before update; no geometry is copied or rebuilt by navigation.
    void drag_origin(std::optional<vng::Vec3> origin,
                     const vng::editor::EditableMesh* mesh=nullptr,
                     vng::Mat4 mesh_to_world=vng::Mat4::identity());
    [[nodiscard]] bool update(CameraPose&, ViewMode, bool& smooth_zoom,
                              vng::Vec2 viewport_origin, vng::Vec2 viewport_size,
                              std::span<const vng::input::Event> unhandled,
                              std::span<const vng::input::Event> raw, bool enabled = true);
    [[nodiscard]] bool update(State&, vng::Vec2 viewport_origin, vng::Vec2 viewport_size,
                              std::span<const vng::input::Event> unhandled,
                              std::span<const vng::input::Event> raw, bool enabled = true);
    [[nodiscard]] bool dragging() const noexcept { return dragging_; }
    [[nodiscard]] bool handledPointer() const noexcept { return handled_; }
    // True when update aborted capture, as opposed to an ordinary release.
    [[nodiscard]] bool cancelled() const noexcept { return cancelled_; }
    void cancel() noexcept;
    [[nodiscard]] DebugReport debug_report() const {
        return {.name="navigation", .role="camera pointer navigation",
            .situation=dragging_ ? (mode_==Mode::orbit ? "Orbit" : mode_==Mode::pan ? "Pan" : "Dolly") : "Idle",
            .received={{"orbit enabled",debug_bool(orbit_enabled_)}, {"look in place",debug_bool(look_in_place_)},
                {"mesh surface reference supplied",debug_bool(surface_!=nullptr)}},
            .owned={{"pointer captured",debug_bool(dragging_)}, {"pointer handled",debug_bool(handled_)},
                {"cancelled",debug_bool(cancelled_)}, {"scroll",scroll_mode_==ScrollMode::zoom ? "optical zoom" : "move forward"},
                {"center supplied",debug_bool(drag_origin_.has_value())},
                {"captured center",debug_bool(captured_origin_.has_value())}}};
    }
    [[nodiscard]] std::string debug_string() const { return debug_report().string(); }

private:
    enum class Mode { orbit, pan, dolly };
    void move(CameraPose&, ViewMode, bool& smooth_zoom, vng::Vec2, bool fast);
    void scroll(CameraPose&, ViewMode, double amount);
    void approach(CameraPose&, ViewMode, vng::Vec3 center, double amount);
    [[nodiscard]] std::optional<double> surface_distance(vng::Vec3 eye, vng::Vec3 center) const;
    Mode mode_{Mode::orbit};
    ScrollMode scroll_mode_{ScrollMode::move_forward};
    CameraDragSpeeds speeds_{};
    bool dragging_{}, handled_{}, cancelled_{};
    bool look_in_place_{};
    bool orbit_enabled_{true};
    std::optional<vng::Vec3> drag_origin_,captured_origin_;
    const vng::editor::EditableMesh* surface_{};
    vng::Mat4 surface_inverse_{vng::Mat4::identity()};
    vng::Vec2 previous_{}, origin_{}, size_{};
};
} // namespace editor_example
