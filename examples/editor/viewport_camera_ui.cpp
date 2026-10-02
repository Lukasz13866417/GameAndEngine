#include "viewport_camera_ui.hpp"
#include "camera_limits.hpp"
namespace editor_example {
using namespace vng;
namespace {
ui::Container heading(ui::Container menu) {menu.label("CAMERA SETTINGS").height(28);return menu;}
ui::Container preferences_heading(ui::Container menu) {
    menu.label("Navigation modes and speeds: camera gizmo in viewport corner").height(44);
    return menu;
}
}
ViewportCameraUI::ViewportCameraUI(ui::Container menu,ui::Container overlay,const CameraPose& pose,const Settings& settings)
    :menu_(heading(menu.padding(12).gap(6).scrollbar(ui::ScrollBar::automatic).visible(false))),
     close_(menu_.button("Close camera settings").height(32)),
     yaw_(menu_,"Orbit",-180,180,pose.yaw),
     pitch_(menu_,"Elevation",-camera_max_pitch,camera_max_pitch,pose.pitch),
     distance_(menu_,"Orbit distance",camera_min_distance,camera_max_distance,pose.distance),
     zoom_(menu_,"Zoom",camera_min_zoom,camera_max_zoom,pose.zoom),
     preferences_(preferences_heading(menu_)),overlay_(overlay.padding(8).gap(6)) {
    distance_.slider_range(settings.orbit_distance.minimum,settings.orbit_distance.maximum);
    zoom_.slider_range(camera_min_zoom,10.F);
}
void ViewportCameraUI::sync(const CameraPose& pose,bool reset,bool idle_only) {
    const std::array values{pose.yaw,pose.pitch,pose.distance,pose.zoom};
    const std::array controls{&yaw_,&pitch_,&distance_,&zoom_};
    for(std::size_t i=0;i<controls.size();++i) {
        auto& control=*controls[i];
        if(idle_only&&control.editing())continue;
        if(i==3)control.slider_range(camera_min_zoom,std::max(10.F,pose.zoom));
        if(reset)control.reset(values[i]);else control.value(values[i]);
    }
}
void ViewportCameraUI::layout(ui::Rect button,Vec2 size) {
    const auto y=button.y+button.height+4;
    menu_.position({std::clamp(button.x,8.F,std::max(8.F,size.x-388)),y}).width(380)
        .height(std::max(100.F,std::min(820.F,size.y-y-16)));
}
void ViewportCameraUI::close(){opened_=false;menu_.visible(false);}
void ViewportCameraUI::toggle(const Settings& settings) {
    opened_=!opened_;menu_.visible(opened_);
    if(opened_){preferences_.show(settings);menu_.scroll(0);}
}
void ViewportCameraUI::enabled(bool menu,bool pose) {
    menu_.enabled(menu);
    for(auto* control:{&yaw_,&pitch_,&distance_,&zoom_})control->enabled(pose);
}
std::string ViewportCameraUI::poll(std::span<const input::Event> events,bool modal) {
    std::string error;
    for(auto* control:{&yaw_,&pitch_,&distance_,&zoom_}) {
        control->poll();if(!control->status().empty())error=control->status();
    }
    if(opened_&&(modal||close_.clicked()||std::ranges::any_of(events,[](const auto& event){
        return event.kind==input::EventKind::key_down&&event.key==input::Key::escape;
    })))close();
    return error;
}
std::optional<content::Result<Settings>> ViewportCameraUI::preferences(const Settings& settings) {
    if(!opened_)return {};
    preferences_.poll();if(!preferences_.applied())return {};
    return preferences_.read(settings);
}
std::optional<CameraPose> ViewportCameraUI::pose_edit(const CameraPose& before,const Settings& settings,std::string& error) {
    if(inspecting()||(!yaw_.changedValue()&&!pitch_.changedValue()&&!distance_.changedValue()&&!zoom_.changedValue()))return {};
    auto after=before;after.yaw=yaw_.value();after.pitch=pitch_.value();
    if(zoom_.changedValue())after.zoom=zoom_.value();
    if(distance_.changedValue()) {
        if(distance_.value()<settings.orbit_distance.minimum||distance_.value()>settings.orbit_distance.maximum) {
            distance_.reset(before.distance);
            error="Orbit distance is outside its range; adjust its limits in Settings";
        } else after.distance=distance_.value();
    }
    return after;
}
DebugReport ViewportCameraUI::debug_report() const {
    return {.name="camera",.role="camera controls and private view visit",.situation=inspecting()?"Inspecting":visiting()?"Entered":"Editor",
        .owned={{"settings open",debug_bool(opened_)},{"visited instance",visit_?std::to_string(visit_->camera):"none"},
            {"overlay visible",debug_bool(overlay_.shown())}}};
}
}
