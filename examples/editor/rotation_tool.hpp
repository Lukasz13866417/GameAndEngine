#pragma once

#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <vng/editor/inspector.hpp>
#include <vng/gfx/camera.hpp>
#include <vng/input/input.hpp>
#include <vng/ui/draw_list.hpp>

namespace editor_example {
struct RotationGizmo final {
    vng::editor::Stamp stamp;
    vng::Vec3 position;
    vng::Vec3 rotation_degrees;
    std::optional<std::array<vng::Vec3,3>> local_axes{}; // yaw, pitch, roll; null = Euler X/Y/Z
    bool free_rotation{}; // Camera-relative MMB drag anywhere in the viewport.
    std::optional<vng::u32> only_axis{}; // Blueprint-constrained single rotation ring.
};

// Editor-only, backend-independent rotation interaction. Default Euler-coordinate
// rings use Rz * Ry * Rx; their normals are
// Rz * Ry * X (red), Rz * Y (green), and world Z (blue). Dragging a ring therefore
// changes exactly its named inspector component, even on an already rotated object.
// Their slightly different radii keep coincident planes selectable at gimbal lock.
// Explicit local_axes instead produce body-relative yaw/pitch/roll rings. These
// compose an axis-angle turn with the original orientation, not Euler addition.
// The host owns preview streaming, Apply, and a single history entry per gesture.
class RotationTool final {
public:
    static constexpr std::size_t ring_segments = 96;
    struct Ring final {
        vng::Vec3 normal{};
        std::string_view label{};
        std::array<vng::Vec2, ring_segments + 1> points{};
        std::array<bool, ring_segments + 1> projected{};
    };

    [[nodiscard]] std::optional<vng::Vec3>
    update(const RotationGizmo&, const vng::gfx::CameraSnapshot&, vng::ui::Rect viewport,
           std::span<const vng::input::Event> unhandled, std::span<const vng::input::Event> raw,
           bool enabled, float arrow_step = 1.F);
    void append(vng::ui::DrawList&, const vng::text::Font& = {}, int axis = -1) const;
    void cancel() noexcept;
    [[nodiscard]] bool dragging() const noexcept { return dragging_; }
    [[nodiscard]] bool visible() const noexcept { return visible_; }
    [[nodiscard]] bool handledPointer() const noexcept { return handled_; }
    [[nodiscard]] std::optional<vng::Vec3> preview_rotation() const noexcept {
        return dragging_ ? std::optional{ghost_} : std::nullopt;
    }
    // Read-only geometry supports picking inspection and real-pointer UI tests.
    [[nodiscard]] const std::array<Ring, 3>& rings() const noexcept { return rings_; }
    [[nodiscard]] std::optional<vng::u32> hit_axis(vng::Vec2) const noexcept;
    struct Turn { vng::u32 axis; vng::f64 degrees; };
    // Last absolute drag angle, also available on the release update.
    [[nodiscard]] Turn turn() const noexcept;
    [[nodiscard]] vng::Vec3 rotation_delta() const noexcept { return delta_; }

private:
    struct Plane {
        vng::Vec3 normal{}, u{}, v{};
        vng::f32 radius{};
    };
    struct Hit {
        vng::u32 axis{};
        vng::f64 angle{};
    };
    std::array<Ring, 3> rings_{};
    std::array<Plane, 3> planes_{};
    RotationGizmo target_{};
    vng::gfx::CameraSnapshot camera_{};
    vng::ui::Rect viewport_{};
    vng::Vec3 ghost_{};
    vng::Vec3 delta_{};
    vng::Vec3 segment_delta_{};
    vng::Vec2 screen_origin_{}, start_{}, pointer_{}, fallback_tangent_{};
    Plane drag_plane_{};
    vng::f64 previous_angle_{}, accumulated_angle_{};
    vng::f64 keyboard_angle_{};
    vng::f64 segment_angle_{};
    bool keyboard_{};
    vng::u32 axis_{};
    bool visible_{}, dragging_{}, handled_{}, fallback_{};

    void geometry();
    void move(vng::Vec2);
    void apply_turn();
    [[nodiscard]] std::optional<Hit> hit(vng::Vec2) const noexcept;
    [[nodiscard]] std::optional<vng::f64> angle(vng::Vec2, const Plane&) const;
};
} // namespace editor_example
