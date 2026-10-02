#pragma once
#include "project.hpp"
#include "number_control.hpp"
#include "component_debug.hpp"

namespace editor_example {
// A normal active gizmo: owns a tree chooser and child parameter controls.
// No session reference or mutation callback. Parent polls and applies proposals.
class AnimationGizmo {
public:
    enum class Action { none, preview, edit, finish, cancel, bake, erase };
    struct Reply {
        Action action{Action::none};
        BlueprintId blueprint{BlueprintId::departure};
        AnimationTargets targets;
        AnimationSettings value;
        bool dragging{};
    };
    explicit AnimationGizmo(vng::ui::Container);
    void create(const State&,BlueprintId);
    void present(const State&,bool enabled,bool creating);
    Reply poll();
    void accepted(bool close_creation=false);
    void error(std::string_view);
    bool creating() const {return creating_;}
    bool visible() const {return visible_;}
    vng::u32 target() const {return target_;}
    DebugReport debug_report() const;
private:
    enum class Page { root, route, speed, turbulence, follow };
    void build(const State&,bool targets_only);
    void build_fields();
    void field(std::string_view,vng::f32&,vng::f32,vng::f32);
    void vector_fields(std::string_view,vng::Vec3&,vng::f32,vng::f32);
    struct Field {NumberControl control;vng::f32* value;};
    vng::ui::Container host_;
    std::optional<vng::ui::Container> body_,fields_slot_,fields_host_;
    std::optional<vng::ui::Dropdown<vng::u32>> object_,camera_;
    vng::ui::Label status_;
    vng::ui::Button apply_,cancel_,bake_,erase_;
    std::array<vng::ui::Button,5> nodes_;
    vng::ui::Checkbox enabled_,orient_,look_;
    std::vector<Field> fields_;
    AnimationSettings value_;
    BlueprintId blueprint_{BlueprintId::departure};
    vng::u32 target_{};
    Page page_{Page::root};
    bool creating_{},preview_{},visible_{},allowed_{},was_dragging_{},targets_only_{};
};
}
