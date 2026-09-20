#pragma once
#include "cage_tool.hpp"
#include "editing_session.hpp"
#include "blueprint_gizmos.hpp"
#include <vng/ui/ui.hpp>
#include <utility>

namespace editor_example {
// Application-owned UI child. Session owns data/history; CageTool owns only
// interaction state. Region::scene_cage() supplies its manipulation contract.
class RegionEditor {
public:
    void scale_limits(const ScaleLimits& limits) { tool_.scale_limits(limits); }
    RegionEditor(vng::ui::Container controls, vng::ui::Container creation,
                 vng::ui::Container inspector, vng::ui::Container popup);
    void open_menu(vng::Vec2 at, vng::Vec2 screen, std::optional<vng::ui::Rect> viewport = {});
    void poll_menu(EditingSession&,std::span<const vng::input::Event>);
    bool menu_open() const {return menu_open_;}
    bool boundaries_visible() const { return show_.value(); }
    void selection(vng::u32 object, GizmoMode mode);
    bool component_editing() const {return selected()&&region_component_mode(gizmo_);}
    bool free_rotation_selected() const {return component_editing() && transform_.value()==GizmoMode::free_rotate && !tool_.elements().empty();}
    std::optional<vng::u32> take_selection(){return std::exchange(selection_,{});}
    std::optional<GizmoMode> take_mode(){return std::exchange(requested_mode_,{});}
    void update(EditingSession&, const vng::gfx::CameraSnapshot&, vng::ui::Rect,
        std::span<const vng::input::Event>,std::span<const vng::input::Event>,bool visible,bool editable,float arrow_step=1.F);
    // The worker depth-tests boundaries; local interaction handles stay immediate.
    void append(vng::ui::DrawList& list,const vng::text::Font& font) const {tool_.append(list,font,false);}
    bool selected() const {return visible_&&show_.value()&&tool_.selected()!=0;}
    bool handled() const {return tool_.handled();}
    bool dragging() const {return tool_.dragging()||tool_.selecting();}
    void cancel() { close_menu(); tool_.hide(); }
    void changed(const DocumentChanges& changes) { pending_.merge(changes); }
    void deselect() {close_menu();tool_.select(0,0);}
    vng::content::Result<bool> erase(EditingSession&);
    std::optional<std::string> take_message(){return std::exchange(message_,{});}
    const CageTool& tool() const {return tool_;}
    ToolOptions* tool_options() {return tool_.tool_options();}
private:
    vng::ui::Container controls_,host_,details_;
    vng::ui::Container popup_;
    std::array<vng::ui::Button,3> modes_;
    vng::ui::Button all_,clear_,add_vertex_,fill_,subdivide_,align_,erase_elements_,erase_region_;
    vng::ui::Checkbox show_, walls_;
    vng::ui::Dropdown<RegionShape> shape_;
    vng::ui::Dropdown<GizmoMode> transform_;
    vng::ui::TextField radius_,name_,note_;
    std::array<vng::ui::TextField,3> xyz_;
    vng::ui::Label point_;
    vng::ui::Button add_,apply_,remove_,deselect_;
    vng::ui::Button previous_vertex_,next_vertex_;
    std::optional<Region> displayed_;
    vng::u32 displayed_point_{};
    vng::u64 displayed_selection_{};
    std::optional<Regions> cached_;
    DocumentChanges pending_;
    vng::u64 cached_revision_{};
    vng::f32 cached_time_{-1};
    CageTool tool_;
    bool visible_{};
    GizmoMode gizmo_{GizmoMode::move};
    std::optional<GizmoMode> requested_mode_;
    std::optional<vng::u32> selection_;
    bool menu_open_{},editable_{};
    std::optional<std::string> message_;
    void sync_inspector(const Regions&);
    void sync(const State&);
    void close_menu();
    void geometry(EditingSession&,RegionAction);
};
} // namespace editor_example
