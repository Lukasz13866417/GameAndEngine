#pragma once

#include "project.hpp"
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
    void drag_origin(std::optional<vng::Vec3> origin) { drag_origin_=origin; }
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

private:
    enum class Mode { orbit, pan, dolly };
    void move(CameraPose&, ViewMode, bool& smooth_zoom, vng::Vec2, bool fast);
    void scroll(CameraPose&, ViewMode, double amount);
    Mode mode_{Mode::orbit};
    ScrollMode scroll_mode_{ScrollMode::move_forward};
    CameraDragSpeeds speeds_{};
    bool dragging_{}, handled_{}, cancelled_{};
    bool look_in_place_{};
    bool orbit_enabled_{true};
    std::optional<vng::Vec3> drag_origin_,captured_origin_;
    vng::Vec2 previous_{}, origin_{}, size_{};
};
} // namespace editor_example
