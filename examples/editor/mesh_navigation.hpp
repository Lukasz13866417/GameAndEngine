#pragma once
#include "project.hpp"
#include "mesh_camera_bake.hpp"
#include "../support/mesh_frame.hpp"
#include <vng/ui/ui.hpp>
#include <algorithm>

namespace editor_example {
// Private viewport preference. Supplies geometry to the navigation tool; it
// emits bake requests without authoring a camera/mesh or retaining draft pointers.
class MeshNavigationControls {
public:
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
    void layout(vng::ui::Rect viewport,bool mesh_edit,bool whole_mesh,bool show_fps=false,bool can_bake=true) {
        visible_=mesh_edit;
        if(!visible_||!whole_mesh)opened_=false;
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
        if(!visible_||!centered_.value()||state.viewport.mode!=ViewMode::mesh)return {};
        const auto* mesh=editable_mesh(state);
        if(!mesh||!mesh->size())return {};
        return example::mesh_frame::point(mesh_transform(state),mesh->center());
    }
private:
    vng::ui::Container host_;
    vng::ui::Checkbox centered_;
    vng::ui::Button bake_,apply_,close_;
    vng::ui::Container menu_;
    vng::ui::Checkbox rotation_,scale_;
    bool visible_{},opened_{};
};
}
