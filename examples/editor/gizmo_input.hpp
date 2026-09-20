#pragma once
#include "tool_options.hpp"
#include "scale_limits.hpp"
#include "transform_keys.hpp"
#include <vng/input/input.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace editor_example {
// Viewport-owned input policy, not a document edit. A virtual pointer keeps
// sensitivity changes, camera drags and visits to tool options out of a captured
// transform. Tools still own their math, transaction and blueprint vocabulary.
class GizmoInput final : public ToolOptions {
public:
    struct Settings { vng::f32 sensitivity{1}; };
    std::string_view title() const override { return options_ ? options_->title() : "Gizmo"; }
    bool options_available() const override { return active_; }
    void describe_options(vng::editor::Inspector& ui) override {
        auto edit=ui.edit("gizmo_pointer",settings_,"Mouse and arrow control");
        edit.slider("sensitivity",&Settings::sensitivity,.05F,4.F,"Sensitivity");
        edit.live([this](const Settings& next){settings_=next;});
        if(scale_options_) {
            auto limits=ui.edit("scale_limits",scale_limits_,"Scale limits (this editor session)");
            limits.field("instance",&ScaleLimits::instance,"Maximum instance scale");
            limits.field("axis",&ScaleLimits::axis,"Maximum axis scale");
            limits.field("factor",&ScaleLimits::factor,"Maximum gesture factor");
            limits.apply("Apply scale limits",[this](const ScaleLimits& next)->vng::editor::Result<void> {
                if(!next.valid())return std::unexpected(vng::editor::Diagnostic{"Scale limits must be finite numbers from 1 to 1000000."});
                scale_limits_=next;
                return {};
            });
        }
        if(options_)options_->describe_options(ui);
    }
    // Returns whether the panel must be refreshed for a new owning tool.
    bool show(bool active,ToolOptions* options,bool captured=false,bool scale_options=false) {
        const bool changed=active_!=active || options_!=options || scale_options_!=scale_options || (captured&&!captured_);
        scale_options_=scale_options;
        active_=active;options_=active?options:nullptr;captured_=captured;
        if(!active)held_.fill(false);
        return changed;
    }
    void sensitivity(float value) { if(std::isfinite(value))settings_.sensitivity=std::clamp(value,.05F,4.F); }
    float sensitivity()const{return settings_.sensitivity;}
    const ScaleLimits& scale_limits()const{return scale_limits_;}
    // Tools consume normalized arrows, not OS repeats. One unit is a degree
    // (rotation) or five screen pixels (translation), at 60 units/second.
    float arrow_step()const{return arrow_step_;}
    void route(bool captured,bool camera_held,bool menu_held,vng::Vec2 pointer,
        std::span<const vng::input::Event> raw,std::span<const vng::input::Event> unhandled,
        float seconds=1.F/60.F,bool keyboard_enabled=true) {
        using namespace vng;
        events_.clear();available_.clear();
        arrow_step_=60.F*(std::isfinite(seconds)?std::clamp(seconds,0.F,.05F):0.F)*settings_.sensitivity;
        bool arrows_enabled=keyboard_enabled&&!camera_held&&!menu_held;
        if(!arrows_enabled || (route_captured_&&!captured))held_.fill(false);
        route_captured_=captured;
        std::array<bool,4> emitted{};
        if(!captured) {virtual_=previous_=pointer;tracking_=false;menu_paused_=false;}
        bool resume_menu=menu_paused_&&!menu_held;
        bool navigating=camera_held;
        for(auto e:raw) {
            const bool pointer_event=e.kind==input::EventKind::pointer_move || e.kind==input::EventKind::pointer_down || e.kind==input::EventKind::pointer_up;
            const bool camera_press=camera_held&&e.kind==input::EventKind::pointer_down&&e.button==2;
            const bool camera_release=e.kind==input::EventKind::pointer_up&&e.button==2;
            const bool camera_event=navigating||camera_press||e.kind==input::EventKind::scroll;
            if(captured && !tracking_) {previous_=virtual_;tracking_=true;}
            const auto original=e;
            if(captured && pointer_event) {
                if(!camera_event&&!menu_held&&!resume_menu&&e.kind==input::EventKind::pointer_move) {
                    virtual_.x+=(e.position.x-previous_.x)*settings_.sensitivity;
                    virtual_.y+=(e.position.y-previous_.y)*settings_.sensitivity;
                }
                previous_=e.position;e.position=virtual_;
                resume_menu=false;
            }
            if(camera_press)navigating=true;
            if(camera_release)navigating=false;
            const bool available=std::ranges::any_of(unhandled,[&](const auto& u){
                return u.kind==original.kind&&u.key==original.key&&u.button==original.button&&u.position==original.position;
            });
            modifiers_=original.modifiers;
            if(modifiers_.control||modifiers_.alt||modifiers_.super)held_.fill(false);
            const bool finish=e.kind==input::EventKind::focus_lost ||
                (e.kind==input::EventKind::key_down && (e.key==input::Key::escape||e.key==input::Key::enter)) ||
                (e.kind==input::EventKind::pointer_down && (e.button==0||e.button==1));
            if(finish) {held_.fill(false);arrows_enabled=false;}
            const auto key=std::ranges::find(arrow_keys_,e.key);
            if(key!=arrow_keys_.end()) {
                const auto index=static_cast<std::size_t>(key-arrow_keys_.begin());
                if(e.kind==input::EventKind::key_up)held_[index]=false;
                if(e.kind==input::EventKind::key_down && !e.modifiers.control && !e.modifiers.alt && !e.modifiers.super) {
                    if(e.repeat || !arrows_enabled || !available)continue;
                    held_[index]=true;emitted[index]=true;
                }
            }
            const bool lifecycle=e.kind==input::EventKind::focus_lost ||
                (e.kind==input::EventKind::key_down&&e.key==input::Key::escape&&(!menu_held||available)) ||
                (e.kind==input::EventKind::pointer_up&&e.button==0);
            const bool menu_key=!pointer_event&&available;
            if(captured && !lifecycle && (camera_event||(menu_held&&!menu_key)))continue;
            events_.push_back(e);if(available)available_.push_back(e);
        }
        if(captured&&(menu_held||navigating))previous_=pointer;
        menu_paused_=captured&&menu_held;
        if(arrows_enabled)for(std::size_t i=0;i<held_.size();++i)if(held_[i]&&!emitted[i]) {
            input::Event tick{.kind=input::EventKind::key_down,.position=captured?virtual_:pointer,
                .key=arrow_keys_[i],.modifiers=modifiers_};
            events_.push_back(tick);available_.push_back(tick);
        }
    }
    std::span<const vng::input::Event> events()const{return events_;}
    std::span<const vng::input::Event> unhandled()const{return available_;}
private:
    Settings settings_;
    ScaleLimits scale_limits_;
    bool scale_options_{};
    ToolOptions* options_{};
    bool active_{},tracking_{},menu_paused_{},captured_{};
    bool route_captured_{};
    float arrow_step_{1};
    static constexpr std::array arrow_keys_{vng::input::Key::left,vng::input::Key::right,vng::input::Key::up,vng::input::Key::down};
    std::array<bool,4> held_{};
    vng::input::Modifiers modifiers_{};
    vng::Vec2 previous_{},virtual_{};
    std::vector<vng::input::Event> events_,available_;
};
}
