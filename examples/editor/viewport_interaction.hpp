#pragma once

#include "box_selection.hpp"
#include "navigation.hpp"
#include "camera_walk.hpp"
#include "region_editor.hpp"
#include "rotation_interaction.hpp"
#include "scale_tool.hpp"
#include "world_bounds_tool.hpp"
#include "selection.hpp"
#include "mesh_transform.hpp"
#include "instance_transform.hpp"
#include "surface_part_tool.hpp"
#include "gizmo_input.hpp"
#include <functional>
#include <type_traits>

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
    ViewportInteraction(EditingSession& editing, vng::ui::Container controls,
                        vng::ui::Container creation, vng::ui::Container inspector, vng::ui::Container popup)
        : regions(controls, creation, inspector, popup), instances(editing), rotation(editing), mesh(editing), editing_(editing) {}

    NavigationTool navigation;
    GizmoInput gizmo_input;
    CameraWalk walk;
    RegionEditor regions;
    WorldBoundsTool bounds;
    InstanceTransformInteraction instances;
    TranslationTool translation;
    RotationInteraction rotation;
    ScaleTool scale;
    MeshTransform mesh;
    SurfacePartTool mesh_part;
    TranslationTool pivot;
    BoxSelection selection_box;
    InstanceProjection instance_projection;

    void begin_frame() {
        handled_ = ViewportTool::none;
        const auto& limits=gizmo_input.scale_limits();
        scale.maximum(limits.instance);
        instances.scale_limits(limits);
        mesh.scale_limits(limits);
        regions.scale_limits(limits);
    }
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
        if (navigation.dragging() || walk.moving()) return ViewportTool::navigation;
        if (selection_box.active() || editing_.active(EditGesture::vertices)) return ViewportTool::selection;
        return ViewportTool::none;
    }
    [[nodiscard]] bool busy() const { return active() != ViewportTool::none; }
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
    // Call in viewport priority order: navigation, keyboard transforms, blueprint
    // boundary, world bounds, transform handles, then selection. A same-frame press+release still
    // consumes its input. GizmoInput removes navigation/panel pointer motion
    // from a captured edit; the tool rebases its math when the view changes.
    template<class Function>
    decltype(auto) update(ViewportTool tool, Function&& function) {
        const bool allowed = accepts(tool);
        if constexpr (std::is_void_v<std::invoke_result_t<Function, bool>>) {
            std::invoke(std::forward<Function>(function), allowed);
            observe(tool, allowed);
        } else {
            auto result = std::invoke(std::forward<Function>(function), allowed);
            observe(tool, allowed);
            return result;
        }
    }
    // Selection is processed event-by-event, after the higher-priority tools.
    void selection_handled() { handled_ = ViewportTool::selection; }

    [[nodiscard]] vng::content::Result<bool> finish(ViewportTool tool, bool cancelled = false) {
        if (active() != tool || !editing_.busy() || editing_.awaiting_remote()) return false;
        auto result = cancelled ? editing_.cancel() : editing_.commit();
        if (!result) return result;
        // Retain consumption for the rest of this input batch after release.
        handled_ = tool;
        return result;
    }

    // Model rollback and tool capture teardown have one owner. UI labels and
    // preview notification remain the application's ordinary model reactions.
    [[nodiscard]] vng::content::Result<bool> cancel() {
        bool changed{};
        if (busy() && editing_.busy() && !editing_.awaiting_remote()) {
            auto restored = editing_.cancel();
            if (!restored) return std::unexpected(restored.error());
            changed = *restored;
        }
        auto rotated = rotation.cancel();
        if (!rotated) return std::unexpected(rotated.error());
        navigation.cancel();
        walk.active(false);
        regions.cancel();
        bounds.cancel();
        translation.cancel();
        scale.cancel();
        mesh.cancel();
        mesh_part.cancel();
        instances.reset();
        pivot.cancel();
        selection_box.cancel();
        handled_ = ViewportTool::none;
        return changed;
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
    void observe(ViewportTool tool, bool allowed) {
        if (!allowed) return;
        bool handled{};
        switch (tool) {
        case ViewportTool::navigation: handled = navigation.handledPointer() || walk.moving(); break;
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
    EditingSession& editing_;
    ViewportTool handled_{ViewportTool::none};
};
} // namespace editor_example
