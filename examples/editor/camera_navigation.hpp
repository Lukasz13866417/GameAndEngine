#pragma once
#include "camera_walk.hpp"
#include "navigation.hpp"
#include <vng/ui/ui.hpp>

namespace editor_example {
// Borrowed input is consumed during this call. Geometry is supplied only
// by the viewport which owns the inspected target; no lookup by component type.
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
    bool move_forward{}, orbit_enabled{true}, keyboard_enabled{true}, controls_have_focus{};
    std::optional<vng::Vec3> origin{};
    const vng::editor::EditableMesh* mesh{};
    vng::Mat4 mesh_to_world{vng::Mat4::identity()};
};
struct NavigationContext {
    std::optional<NavigationFrame> frame{};
    std::optional<bool> walk_active{};
    bool cancel{};
    bool enabled{true};
};
struct NavigationReply {
    CameraPose pose{};
    bool changed{}, smooth_zoom{}, cancelled{};
};
class CameraNavigation final {
public:
    struct Orbiting {};
    struct Walking {};
    struct Unavailable {};
    [[nodiscard]] const NavigationTool& pointer() const { return pointer_; }
    [[nodiscard]] const CameraWalk& walking() const { return walk_; }
    void walking(bool active) { walk_.active(active); }
    void cancel_pointer() { pointer_.cancel(); }
    void cancel() { pointer_.cancel(); walk_.active(false); }
    [[nodiscard]] DebugReport debug_report() const {
        return {.name="camera_navigation",.role="viewport navigation input ownership",.situation=std::string(situation_),
            .owned={{"walk active",debug_bool(walk_.active())},{"walk moving",debug_bool(walk_.moving())}},
            .children={pointer_.debug_report()}};
    }
    [[nodiscard]] std::string debug_string() const { return debug_report().string(); }
private:
    friend class ViewportInteraction;
    NavigationReply handle(const Orbiting&,const NavigationFrame& input) {
        situation_="Orbiting";
        return update(input,true,false);
    }
    NavigationReply handle(const Walking&,const NavigationFrame& input) {
        situation_="Walking";
        return update(input,true,true);
    }
    NavigationReply handle(const Unavailable&,const NavigationFrame& input) {
        situation_="Unavailable";
        return update(input,false,walk_.active());
    }
    NavigationReply update(const NavigationFrame& input,bool enabled,bool walking) {
        pointer_.scroll_mode(input.move_forward?NavigationTool::ScrollMode::move_forward:NavigationTool::ScrollMode::zoom);
        pointer_.look_in_place(walking);
        pointer_.speeds(input.drag_speeds);
        pointer_.drag_origin(walking?std::nullopt:input.origin,walking?nullptr:input.mesh,input.mesh_to_world);
        pointer_.orbit_enabled(walking || input.orbit_enabled);
        NavigationReply reply{.pose=input.pose,.smooth_zoom=input.smooth_zoom};
        reply.changed=pointer_.update(reply.pose,input.mode,reply.smooth_zoom,
            {input.viewport.x,input.viewport.y},{input.viewport.width,input.viewport.height},
            input.unhandled,input.raw.events,enabled && input.raw.focused && !input.raw.overflow);
        // A popout changes keyboard focus without disarming walk mode.
        if(input.controls_have_focus && !input.raw.focused) walk_.stop();
        else if(walk_.update(reply.pose,input.seconds,input.raw,input.walk_speeds,enabled && input.keyboard_enabled,input.unhandled)) {
            reply.changed=true;
            reply.smooth_zoom=false;
        }
        reply.cancelled=pointer_.cancelled();
        return reply;
    }
    NavigationTool pointer_;
    CameraWalk walk_;
    std::string_view situation_{"Not dispatched"};
};
} // namespace editor_example
