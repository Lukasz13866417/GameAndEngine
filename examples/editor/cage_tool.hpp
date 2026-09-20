#pragma once
#include "component_transform.hpp"
#include "box_selection.hpp"
#include <vng/editor/scene_cage.hpp>

namespace editor_example {
struct CageAction {
    vng::u64 object{};
    vng::u32 point{};
    vng::Vec3 position{};
    bool began{}, changed{}, finished{}, cancelled{};
    std::vector<vng::editor::ScenePoint> points;
};
// Interprets value-only cages. It knows nothing about regions, blueprints,
// document transactions, IPC or GPU objects.
class CageTool {
public:
    void scale_limits(const ScaleLimits& limits) { transform_.scale_limits(limits); }
    CageAction update(const vng::gfx::CameraSnapshot&,
        vng::ui::Rect, std::span<const vng::input::Event>, std::span<const vng::input::Event>, bool enabled, float arrow_step=1.F);
    void append(vng::ui::DrawList&, const vng::text::Font&, bool boundaries = true) const;
    void refresh(std::span<const vng::editor::SceneCage>);
    void refresh(vng::editor::SceneCage);
    void refresh_points(vng::u64, std::span<const vng::editor::ScenePoint>);
    void erase(vng::u64);
    void components(bool enabled) {if(components_!=enabled){transform_.cancel();box_.cancel();components_=enabled;}}
    void transform_kind(TransformKind value) {transform_.kind(value);}
    TransformKind transform_kind() const {return transform_.kind();}
    void transform_mode(GizmoMode mode) {transform_.gizmo(mode);}
    GizmoMode transform_mode() const {return transform_.gizmo();}
    void select(vng::u64 object, vng::u32 point);
    void select_elements(vng::editor::CageElement, std::span<const vng::u32>);
    void select_all();
    void mode(vng::editor::CageElement value) {select_elements(value,{});}
    vng::editor::CageElement mode() const {return mode_;}
    std::span<const vng::u32> elements() const {return elements_;}
    std::vector<vng::u32> vertices() const;
    std::optional<vng::Vec3> pivot() const;
    vng::u64 selection_revision() const {return selection_revision_;}
    void clear() { transform_.cancel();box_.cancel();object_=0;elements_.clear();++selection_revision_; }
    void hide() { transform_.cancel();box_.cancel();hidden_=true;handled_=false; }
    bool dragging() const {return transform_.active();}
    bool gizmo_visible() const {return !hidden_&&transform_.visible();}
    ToolOptions* tool_options() {return transform_.tool_options();}
    bool selecting() const {return box_.active();}
    bool handled() const {return handled_;}
    std::optional<vng::u64> picked_object() const {return picked_;}
    vng::u64 selected() const {return object_;}
    vng::u32 point() const {return point_;}
    std::optional<vng::Vec2> handle(std::string_view axis) const {return transform_.handle(axis);}
    std::optional<vng::Vec2> point_handle(vng::u64 object,vng::u32 point) const;
    std::optional<vng::Vec2> element_handle(vng::editor::CageElement, vng::u32 element) const;
private:
    std::vector<vng::editor::SceneCage> cages_;
    vng::gfx::CameraSnapshot camera_{};
    vng::ui::Rect viewport_{};
    vng::u64 object_{};
    vng::u32 point_{};
    vng::editor::CageElement mode_{vng::editor::CageElement::vertex};
    std::vector<vng::u32> elements_;
    vng::u64 selection_revision_{};
    bool handled_{};
    bool hidden_{};
    bool components_{true};
    std::optional<vng::u64> picked_;
    ComponentTransform transform_;
    BoxSelection box_;
    void validate_selection();
};
} // namespace editor_example
