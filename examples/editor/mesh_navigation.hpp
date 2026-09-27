#pragma once
#include "project.hpp"
#include "mesh_camera_bake.hpp"
#include "component_debug.hpp"
#include "../support/mesh_frame.hpp"
#include <vng/ui/ui.hpp>
#include <algorithm>

namespace editor_example {
// Private viewport preference. Supplies geometry to the navigation tool; it
// emits bake requests without authoring a camera/mesh or retaining draft pointers.
class MeshNavigationControls {
public:
    struct MeshView { bool whole{}; };
    struct Inactive {};
    struct Context {
        vng::ui::Rect viewport{};
        bool show_fps{}, can_bake{}, covered{}, poll_input{};
    };
    explicit MeshNavigationControls(vng::ui::Container parent)
        : host_(parent.column().padding(4).gap(2).visible(false)),
          centered_(host_.checkbox("Mesh-centered camera").height(32)) {
        host_.label("MMB: orbit / Ctrl: in-out / Shift: pan").height(22);
        bake_=host_.button("Bake camera transforms...").height(32);
        menu_=host_.column().padding(4).gap(3).visible(false);
        rotation_=menu_.checkbox("Rotation").value(true).height(30);
        scale_=menu_.checkbox("Scale").value(true).height(30);
        menu_.label("Scale: zoom only (no movement)").height(22);
        apply_=menu_.button("Bake to mesh draft").height(32);
        close_=menu_.button("Cancel camera bake").height(28);
    }
    // Main-window menus sit above viewport controls. Hide their presentation
    // and hit targets while covered, without switching off camera centering.
    void layout(vng::ui::Rect viewport,bool mesh_edit,bool whole_mesh,bool show_fps=false,bool can_bake=true,bool covered=false) {
        mesh_edit_=mesh_edit;
        visible_=mesh_edit&&!covered;
        if(!mesh_edit||!whole_mesh)opened_=false;
        bake_.visible(whole_mesh).enabled(can_bake);
        menu_.visible(opened_).enabled(can_bake);
        apply_.enabled(can_bake&&(rotation_.value()||scale_.value()));
        host_.visible(visible_).position({viewport.x+8,viewport.y+8+(show_fps?52.F:0.F)})
            .width(std::min(380.F,std::max(1.F,viewport.width-16))).height(64+(whole_mesh?34:0)+(opened_?173:0));
    }
    std::optional<CameraBakeOptions> poll() {
        if(bake_.clicked())opened_=!opened_;
        if(close_.clicked())opened_=false;
        if(opened_&&apply_.clicked()) {opened_=false;return CameraBakeOptions{rotation_.value(),scale_.value()};}
        return {};
    }
    bool contains(vng::Vec2 p)const{return visible_&&host_.bounds().contains(p);}
    std::optional<vng::Vec3> origin(const State& state)const {
        if(!mesh_edit_||!centered_.value()||state.viewport.mode!=ViewMode::mesh)return {};
        const auto* mesh=editable_mesh(state);
        if(!mesh||!mesh->size())return {};
        return example::mesh_frame::point(mesh_transform(state),mesh->center());
    }
    [[nodiscard]] DebugReport debug_report() const {
        return {.name="mesh_camera",.role="mesh-centered navigation preference and camera bake controls",
            .situation=mesh_edit_ ? "MeshView" : "Inactive",
            .owned={{"visible",debug_bool(visible_)},{"centered",debug_bool(centered_.value())},
                {"bake menu open",debug_bool(opened_)},{"bake rotation",debug_bool(rotation_.value())},
                {"bake optical scale",debug_bool(scale_.value())}}};
    }
    [[nodiscard]] std::string debug_string() const { return debug_report().string(); }
private:
    friend class EditingViewport;
    std::optional<CameraBakeOptions> handle(const MeshView& situation,const Context& context) {
        layout(context.viewport,true,situation.whole,context.show_fps,context.can_bake,context.covered);
        return context.poll_input && !context.covered ? poll() : std::nullopt;
    }
    std::optional<CameraBakeOptions> handle(const Inactive&,const Context& context) {
        layout(context.viewport,false,false,context.show_fps,false,context.covered);
        return {};
    }
    vng::ui::Container host_;
    vng::ui::Checkbox centered_;
    vng::ui::Button bake_,apply_,close_;
    vng::ui::Container menu_;
    vng::ui::Checkbox rotation_,scale_;
    bool mesh_edit_{},visible_{},opened_{};
};
}
