#pragma once
#include "project.hpp"
#include <vng/input/input.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace editor_example {
// Private input state, not a scene component. The host decides whether typing,
// a modal dialog, or another viewport tool owns this input batch.
class CameraWalk {
public:
    void active(bool value) { active_=value; stop(); }
    bool active() const { return active_; }
    bool moving() const { return std::ranges::any_of(held_, [](bool key) { return key; }); }
    void stop() { held_.fill(false); fast_=false; }
    bool update(CameraPose& pose, double seconds, const vng::input::Frame& input,
                WalkSpeeds speeds, bool enabled) {
        using namespace vng::input;
        if (!input.focused || input.overflow) { active(false); return false; }
        for (const auto& event:input.events)
            if (event.kind==EventKind::focus_lost ||
                (event.kind==EventKind::key_down && event.key==Key::escape)) {
                active(false); return false;
            }
        if (!active_ || !enabled) { stop(); return false; }
        constexpr std::array keys{Key::w,Key::s,Key::d,Key::a,Key::e,Key::q};
        for (const auto& event:input.events) {
            if(event.kind!=EventKind::text) fast_=event.modifiers.shift;
            if (event.modifiers.control || event.modifiers.alt || event.modifiers.super) { stop(); continue; }
            for (std::size_t i=0;i<keys.size();++i) if(event.key==keys[i]) {
                if(event.kind==EventKind::key_up) held_[i]=false;
                if(event.kind==EventKind::key_down && !event.repeat) held_[i]=true;
            }
        }
        if (!valid_camera_pose(pose) || !std::isfinite(seconds) || seconds<=0) return false;
        const auto yaw=pose.yaw*std::numbers::pi/180, pitch=pose.pitch*std::numbers::pi/180;
        const auto forward=int(held_[0])-int(held_[1]);
        const auto side=int(held_[2])-int(held_[3]);
        const auto up=int(held_[4])-int(held_[5]);
        const auto length=std::sqrt(double(forward*forward+side*side+up*up));
        if (!length) return false;
        // World up for Q/E; W/S follows pitch as well as yaw. Limit pauses to
        // 100ms, and normalize combined input so diagonals are not faster.
        const auto dt=std::min(seconds,.1)*(fast_?speeds.fast_multiplier:1)/length;
        const std::array delta{
            (-std::sin(yaw)*std::cos(pitch)*forward*speeds.forward + std::cos(yaw)*side*speeds.sideways)*dt,
            (-std::sin(pitch)*forward*speeds.forward + up*speeds.vertical)*dt,
            (-std::cos(yaw)*std::cos(pitch)*forward*speeds.forward - std::sin(yaw)*side*speeds.sideways)*dt};
        double fraction=1;
        for(std::size_t i=0;i<3;++i) if(delta[i]!=0)
            fraction=std::min(fraction,((delta[i]>0?camera_target_limit:-camera_target_limit)-pose.target[i])/delta[i]);
        const auto before=pose.target;
        for(std::size_t i=0;i<3;++i) pose.target[i]=static_cast<float>(pose.target[i]+delta[i]*fraction);
        return pose.target!=before;
    }
private:
    bool active_{},fast_{};
    std::array<bool,6> held_{};
};
} // namespace editor_example
