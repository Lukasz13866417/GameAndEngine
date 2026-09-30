#pragma once
#include "camera_panel.hpp"
#include "camera_preferences.hpp"
#include "component_debug.hpp"
namespace editor_example {
// UI state stays with its viewport owner. This component never writes a scene
// or a preferences file; the workspace applies poses, the application saves preferences.
class ViewportCameraUI final {
public:
    ViewportCameraUI(vng::ui::Container menu,vng::ui::Container overlay,const CameraPose&,const Settings&);
    bool opened() const {return opened_;}
    bool inspecting() const {return visit_&&!visit_->editable;}
    bool visiting() const {return visit_.has_value();}
    bool overlay_shown() const {return overlay_.shown();}
    bool overlay_contains(vng::Vec2 point) const {return overlay_.contains(point);}
    DebugReport debug_report() const;
private:
    friend class EditingWorkspaceUI;
    void sync(const CameraPose&,bool reset=false,bool idle_only=false);
    void layout(vng::ui::Rect button,vng::Vec2 screen);
    void close();
    void toggle(const Settings&);
    std::string poll(std::span<const vng::input::Event>,bool modal);
    std::optional<vng::content::Result<Settings>> preferences(const Settings&);
    std::optional<CameraPose> pose_edit(const CameraPose&,const Settings&,std::string& error);
    void enabled(bool menu,bool pose);
    vng::ui::Container menu_;
    vng::ui::Button close_;
    NumberControl yaw_,pitch_,distance_,zoom_;
    CameraPreferences preferences_;
    CameraPanel overlay_;
    std::optional<CameraVisit> visit_;
    bool opened_{};
};
}
