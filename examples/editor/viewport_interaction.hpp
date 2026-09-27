#pragma once

#include "box_selection.hpp"
#include "component_dispatch.hpp"
#include "navigation.hpp"
#include "camera_navigation.hpp"
#include "camera_walk.hpp"
#include "region_editor.hpp"
#include "rotation_interaction.hpp"
#include "scale_tool.hpp"
#include "world_bounds_tool.hpp"
#include "selection.hpp"
#include "mesh_transform.hpp"
#include "instance_transform.hpp"
#include "surface_part_tool.hpp"
#include "socket_pick_tool.hpp"
#include "gizmo_input.hpp"
#include "move_gizmo.hpp"
#include "rotation_origin_movement.hpp"
#include "scene_movement.hpp"

namespace editor_example {
enum class ViewportTool { none, navigation, boundary, bounds, instances, translation, rotation, scale, components, mesh_part, pivot, selection };

// The viewport owns its tools and arbitrates their input. Tools know only their
// own gesture; the application supplies document/preview eligibility, never a
// list of siblings which must be idle. One tool owns document editing; camera
// navigation may temporarily borrow pointer input without taking that ownership.
// Passive gizmos may still be presented by updating them with empty
// input. Input availability must not be used as their visibility predicate.
class ViewportInteraction {
public:
    ViewportInteraction(const EditingSession& editing, vng::ui::Container controls,
                        vng::ui::Container creation, vng::ui::Container inspector, vng::ui::Container popup)
        : regions(controls, creation, inspector, popup), instances(editing), rotation(editing), mesh(editing), editing_(editing) {}

    struct Navigate {};
    [[nodiscard]] const CameraNavigation& camera_navigation() const { return navigation_; }
    [[nodiscard]] DebugReport debug_report() const {
        const auto name=[](ViewportTool tool) {
            constexpr std::array names{"none","navigation","boundary","bounds","instances","translation",
                "rotation","scale","components","mesh part","pivot","selection"};
            return std::string(names[static_cast<std::size_t>(tool)]);
        };
        return {.name="interaction",.role="viewport input arbitration and local tool captures",.situation=name(active()),
            .owned={{"handled this occurrence",name(handled_)},{"selected handle",debug_bool(selected_handle())},
                {"transforming",debug_bool(transforming())},{"instance gesture",debug_bool(instances.active())},
                {"region selected",debug_bool(regions.selected())},{"region gesture",debug_bool(regions.dragging())},
                {"region menu",debug_bool(regions.menu_open())},{"bounds gesture",debug_bool(bounds.dragging())},
                {"move gesture",debug_bool(translation.dragging())},{"rotate gesture",debug_bool(rotation.dragging())},
                {"scale gesture",debug_bool(scale.dragging())},{"mesh gesture",debug_bool(mesh.active())},
                {"part gesture",debug_bool(mesh_part.dragging())},{"box selection",debug_bool(selection_box.active())}},
            .children={navigation_.debug_report(),translation.debug_report(),pivot.debug_report()}};
    }
    [[nodiscard]] std::string debug_string() const { return debug_report().string(); }
    GizmoInput gizmo_input;
    RegionEditor regions;
    WorldBoundsTool bounds;
    InstanceTransformInteraction instances;
    SceneMovement translation;
    RotationInteraction rotation;
    ScaleTool scale;
    MeshTransform mesh;
    SurfacePartTool mesh_part;
    SocketPickTool mesh_sockets;
    MoveGizmo<RotationOriginMovement> pivot;
    BoxSelection selection_box;
    SelectionInput selection_input;
    InstanceProjection instance_projection;

    void begin_frame() {
        handled_ = ViewportTool::none;
        gizmo_input.begin_frame();
        const auto& limits=gizmo_input.scale_limits();
        scale.maximum(limits.instance);
        instances.scale_limits(limits);
        mesh.scale_limits(limits);
        regions.scale_limits(limits);
    }
    // Input consumption belongs to one occurrence. Captured tools, held keys,
    // selected handles and domain transactions survive between these steps.
    void begin_step() {handled_=ViewportTool::none;}
    [[nodiscard]] ViewportTool active() const {
        if (instances.active()) return ViewportTool::instances;
        if (regions.dragging() || editing_.active(EditGesture::region)) return ViewportTool::boundary;
        if (bounds.dragging() || editing_.active(EditGesture::world_bounds)) return ViewportTool::bounds;
        if (translation.dragging() || editing_.active(EditGesture::move)) return ViewportTool::translation;
        if (rotation.dragging() || rotation.active()) return ViewportTool::rotation;
        if (scale.dragging() || editing_.active(EditGesture::scale)) return ViewportTool::scale;
        if (mesh.active() || editing_.active(EditGesture::vertices) || editing_.active(EditGesture::mesh_transform)) return ViewportTool::components;
        if (mesh_part.dragging() || editing_.active(EditGesture::mesh_draft)) return ViewportTool::mesh_part;
        if (pivot.dragging()) return ViewportTool::pivot;
        if (navigation_.pointer().dragging() || navigation_.walking().moving() || editing_.active(EditGesture::camera)) return ViewportTool::navigation;
        if (selection_box.active() || editing_.active(EditGesture::vertices)) return ViewportTool::selection;
        return ViewportTool::none;
    }
    [[nodiscard]] bool busy() const { return active() != ViewportTool::none; }
    [[nodiscard]] bool selected_handle() const {
        return (translation.visible() && translation.selected_axis().has_value()) ||
            (rotation.visible() && rotation.tool().selected_axis().has_value()) ||
            (scale.visible() && scale.selected()) || (mesh.visible() && mesh.selected_handle()) ||
            (mesh_part.visible() && (mesh_part.rotation().selected_axis().has_value()||mesh_part.altitude().selected_axis().has_value())) ||
            (pivot.visible() && pivot.selected_axis().has_value());
    }
    [[nodiscard]] bool transforming() const {
        const auto owner=active();
        return owner!=ViewportTool::none && owner!=ViewportTool::navigation && owner!=ViewportTool::selection;
    }
    [[nodiscard]] bool accepts(ViewportTool tool) const {
        const auto owner = active();
        if(tool==ViewportTool::navigation && transforming())return true;
        return (owner == ViewportTool::none || owner == tool) &&
               (handled_ == ViewportTool::none || handled_ == tool || (handled_==ViewportTool::navigation&&owner==tool));
    }
    // Selection is processed event-by-event, after the higher-priority tools.
    void selection_handled() { handled_ = ViewportTool::selection; }

    // The parent acknowledges a successfully finished transaction. Retain
    // consumption after release so the same input cannot select through it.
    void finished(ViewportTool tool) { handled_=tool; }

    // Local capture teardown only. The workspace performs any document rollback
    // first; this child cannot mutate its read-only authoring observations.
    void reset() {
        rotation.reset();
        navigation_.cancel();
        regions.cancel();
        bounds.cancel();
        translation.cancel();
        scale.cancel();
        mesh.cancel();
        mesh_part.cancel();
        instances.reset();
        pivot.cancel();
        selection_box.cancel();
        selection_input.cancel();
        handled_ = ViewportTool::none;
    }

    void selected(vng::u32 object, GizmoMode mode) {
        const auto& state = editing_.state();
        const auto* instance = find_instance(state, object);
        const auto surface = instance ? blueprint_manipulation(state, instance->blueprint).surface
                                      : ManipulationSurface::object;
        regions.selection(surface == ManipulationSurface::boundary ? object : 0, mode);
    }
    [[nodiscard]] bool custom_inspector() const { return regions.selected(); }
    [[nodiscard]] vng::editor::SelectionMode picked_selection(vng::input::Modifiers modifiers) const {
        return regions.component_editing() ? vng::editor::SelectionMode::replace : click_selection(modifiers);
    }

private:
    friend struct Dispatcher;
    NavigationReply handle(const Navigate&,const NavigationContext& context) {
        if(context.cancel) navigation_.cancel_pointer();
        if(context.walk_active) navigation_.walking(*context.walk_active);
        if(!context.frame) return {};
        const bool allowed=context.enabled && accepts(ViewportTool::navigation);
        NavigationReply reply;
        if(!allowed) reply=navigation_.handle(CameraNavigation::Unavailable{},*context.frame);
        else if(navigation_.walking().active()) reply=navigation_.handle(CameraNavigation::Walking{},*context.frame);
        else reply=navigation_.handle(CameraNavigation::Orbiting{},*context.frame);
        observe(ViewportTool::navigation,allowed);
        return reply;
    }
public:
    // The parent checks accepts(), invokes the child, then acknowledges its
    // input handling. No stored or synchronously invoked parent callback.
    void observe(ViewportTool tool, bool allowed) {
        if (!allowed) return;
        bool handled{};
        switch (tool) {
        case ViewportTool::navigation: handled = navigation_.pointer().handledPointer() || navigation_.walking().moving(); break;
        case ViewportTool::boundary: handled = regions.handled() || regions.component_editing(); break;
        case ViewportTool::bounds: handled = bounds.handledPointer(); break;
        case ViewportTool::instances: handled = instances.handled(); break;
        case ViewportTool::translation: handled = translation.handledPointer(); break;
        case ViewportTool::rotation: handled = rotation.handledPointer(); break;
        case ViewportTool::scale: handled = scale.handledPointer(); break;
        case ViewportTool::components: handled = mesh.handled(); break;
        case ViewportTool::mesh_part: handled = mesh_part.handled(); break;
        case ViewportTool::pivot: handled = pivot.handledPointer(); break;
        case ViewportTool::selection: handled = selection_box.active(); break;
        case ViewportTool::none: break;
        }
        if (handled) handled_ = tool;
    }
private:
    const EditingSession& editing_;
    CameraNavigation navigation_;
    ViewportTool handled_{ViewportTool::none};
};
} // namespace editor_example
