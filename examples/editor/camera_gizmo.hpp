#pragma once

#include "camera_pointer_logic.hpp"
#include "camera_walk_logic.hpp"
#include "number_control.hpp"
#include "settings.hpp"
#include <array>
#include <optional>

namespace editor_example {

enum class CameraGizmoMode { orbit, look, pan, forward, zoom, walk };
struct CameraGizmoDescription {
    CameraGizmoMode mode;
    std::string_view label, shortcut, hint;
};
inline constexpr std::array camera_gizmo_descriptions{
    CameraGizmoDescription{CameraGizmoMode::orbit,"Orbit","MMB","LMB / MMB: orbit reference"},
    CameraGizmoDescription{CameraGizmoMode::look,"Look","MMB","LMB / MMB: turn in place"},
    CameraGizmoDescription{CameraGizmoMode::pan,"Pan","Shift + MMB","LMB / Shift+MMB: pan"},
    CameraGizmoDescription{CameraGizmoMode::forward,"Forward / back","Ctrl+MMB","LMB / wheel: move forward/back"},
    CameraGizmoDescription{CameraGizmoMode::zoom,"Optical zoom","wheel","LMB / wheel: optical zoom"},
    CameraGizmoDescription{CameraGizmoMode::walk,"Walk","WASD / Q E","Shift: fast / MMB: look"}
};
inline const CameraGizmoDescription& camera_gizmo_description(CameraGizmoMode mode) {
    return camera_gizmo_descriptions[static_cast<std::size_t>(mode)];
}

// Immutable target/input supplied by the owning viewport. Editor and entered
// simulation cameras use the same pose contract; the gizmos never author a
// camera instance, reach into another node, or publish a document revision.
struct NavigationFrame {
    CameraPose pose;
    ViewMode mode{};
    bool smooth_zoom{};
    vng::ui::Rect viewport;
    const vng::input::Frame& raw;
    std::span<const vng::input::Event> unhandled;
    double seconds{};
    CameraDragSpeeds drag_speeds;
    WalkSpeeds walk_speeds;
    // escape_claimed: this frame's Escape already belongs to a text edit, an
    // open popup or flyout. It still releases captures, never the chosen mode.
    bool move_forward{}, orbit_enabled{true}, keyboard_enabled{true}, escape_claimed{};
    std::optional<vng::Vec3> origin{};
    const vng::editor::EditableMesh* mesh{};
    vng::Mat4 mesh_to_world{vng::Mat4::identity()};
};
struct NavigationContext {
    std::optional<NavigationFrame> frame{};
    bool cancel{};
    bool enabled{true};
};
struct NavigationReply {
    CameraPose pose{};
    bool changed{}, smooth_zoom{}, cancelled{};
};
struct CameraPreferenceEdit {
    Settings value;
    bool changed{}, committed{};
    std::string error;
};

// The five pointer children reuse one implementation, with a fixed operation
// per object. Each owns its capture and its own settings UI, not just a label
// in a global mode switch. No semantic concepts, registry or visitor required.
class CameraPointerGizmo {
public:
    explicit CameraPointerGizmo(CameraGizmoMode mode) : mode_(mode) {}
    void attach(vng::ui::Container);
    void present(bool, const Settings&);
    void poll(CameraPreferenceEdit&);
    bool update(CameraPose&, bool& smooth, const NavigationFrame&,
                std::span<const vng::input::Event> raw, std::span<const vng::input::Event> available,
                bool primary_button, bool enabled);
    void cancel() { pointer_.cancel(); }
    bool dragging() const { return pointer_.dragging(); }
    bool handled() const { return pointer_.handledPointer(); }
    bool cancelled() const { return pointer_.cancelled(); }
    DebugReport debug_report() const;
private:
    CameraGizmoMode mode_;
    CameraPointerLogic pointer_;
    std::optional<NumberControl> speed_;
    bool visible_{};
};

class CameraWalkGizmo {
public:
    void attach(vng::ui::Container);
    void present(bool, const Settings&);
    void poll(CameraPreferenceEdit&);
    bool update(CameraPose&, const NavigationFrame&, bool enabled);
    void active(bool value) { motion_.active(value); }
    bool active() const { return motion_.active(); }
    bool moving() const { return motion_.moving(); }
    void stop() { motion_.stop(); }
    DebugReport debug_report() const;
private:
    CameraWalkLogic motion_;
    std::optional<vng::ui::Container> controls_;
    std::vector<NumberControl> speeds_;
    bool visible_{};
};

// A gizmo which owns gizmos. The viewport chooses whether it is the fallback
// target; explicit mode selection borrows the viewport from object tools until
// Escape/"Object tools". Normal camera shortcuts remain usable during edits.
class CameraGizmo {
public:
    CameraGizmo() = default;
    explicit CameraGizmo(vng::ui::Container host) { attach(host); }
    void attach(vng::ui::Container);
    struct Presentation {
        vng::ui::Rect viewport;
        std::string_view target{"Editor camera"};
        bool visible{true}, enabled{true}, other_gizmo{};
    };
    // The menu unfolds when this becomes the camera target and folds when an
    // object gizmo takes over; its heading only toggles it in between. Hiding
    // it also ends an explicitly chosen mode: no mode exists without its menu.
    void present(const Presentation&, const Settings&);
    CameraPreferenceEdit poll(const Settings&);
    NavigationReply update(const NavigationFrame&, bool enabled = true);
    void append(vng::ui::DrawList&) const;
    void select(CameraGizmoMode);
    void object_tools();
    const CameraWalkGizmo& walking() const { return walk_; }
    void cancel_pointer();
    void cancel();
    bool dragging() const;
    bool handledPointer() const { return handled_; }
    bool cancelled() const { return cancelled_; }
    bool exclusive() const { return selected_; }
    bool active() const { return selected_ || fallback_ || dragging() || handled_ || walk_.active(); }
    CameraGizmoMode mode() const { return mode_; }
    CameraGizmoMode operating_mode() const { return captured_.value_or(mode_); }
    bool contains(vng::Vec2 p) const { return visible_ && host_ && host_->bounds().contains(p); }
    bool expanded() const { return visible_ && opened_; }
    vng::ui::Rect bounds() const { return visible_ && host_ ? host_->bounds() : vng::ui::Rect{}; }
    [[nodiscard]] DebugReport debug_report() const;
    [[nodiscard]] std::string debug_string() const { return debug_report().string(); }
private:
    std::array<CameraPointerGizmo,5> pointer_{
        CameraPointerGizmo{CameraGizmoMode::orbit},CameraPointerGizmo{CameraGizmoMode::look},
        CameraPointerGizmo{CameraGizmoMode::pan},CameraPointerGizmo{CameraGizmoMode::forward},
        CameraPointerGizmo{CameraGizmoMode::zoom}};
    CameraWalkGizmo walk_;
    CameraGizmoMode mode_{CameraGizmoMode::orbit};
    std::optional<CameraGizmoMode> captured_;
    bool selected_{},fallback_{true},handled_{},cancelled_{},visible_{},enabled_{true},input_enabled_{true},opened_{true};
    std::string target_{"Editor camera"};
    std::optional<vng::ui::Container> host_,body_;
    vng::ui::Button heading_,objects_;
    vng::ui::Label hint_;
    std::array<vng::ui::Button,6> choices_;
    vng::ui::Checkbox scroll_;
};
} // namespace editor_example
