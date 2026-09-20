#include "navigation.hpp"

#include <algorithm>
#include <cmath>

namespace editor_example {
namespace {
using namespace vng;
bool finite(Vec2 p) {
    return std::isfinite(p.x) && std::isfinite(p.y);
}
bool contains(Vec2 origin, Vec2 size, Vec2 p) {
    return finite(p) && p.x >= origin.x && p.y >= origin.y &&
           static_cast<f64>(p.x) < static_cast<f64>(origin.x) + size.x &&
           static_cast<f64>(p.y) < static_cast<f64>(origin.y) + size.y;
}
} // namespace

void NavigationTool::scroll(CameraPose& s, ViewMode view, double amount) {
    if (scroll_mode_ == ScrollMode::zoom) {
        s.zoom = static_cast<f32>(std::clamp(static_cast<double>(s.zoom) *
            std::exp(std::clamp(amount, -32., 32.)), double(camera_min_zoom), double(camera_max_zoom)));
        return;
    }
    // Translate eye AND pivot: dollying never changes the lens or gets stuck
    // approaching the old pivot. Scale speed to the current orbit working size.
    const auto yaw = s.yaw * std::numbers::pi / 180;
    const auto pitch = s.pitch * std::numbers::pi / 180;
    const std::array<double, 3> forward{-std::sin(yaw) * std::cos(pitch),
        -std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
    auto step = s.distance * (view == ViewMode::scene ? 1. : .52) * std::clamp(amount, -32., 32.);
    // Limit the single translation, not its components, to stay on the axis.
    double fraction = 1;
    for (std::size_t i=0; i<3; ++i) {
        const auto delta = forward[i] * step;
        if (delta != 0) fraction = std::min(fraction,
            ((delta > 0 ? camera_target_limit : -camera_target_limit) - s.target[i]) / delta);
    }
    for (std::size_t i=0; i<3; ++i)
        s.target[i] = static_cast<float>(s.target[i] + forward[i] * step * fraction);
}

void NavigationTool::cancel() noexcept {
    cancelled_ |= dragging_;
    dragging_ = false;
}

void NavigationTool::move(CameraPose& s, ViewMode view, bool& smooth_zoom, Vec2 pointer, bool fast) {
    if (!finite(pointer))
        return;
    smooth_zoom = false;
    const auto dx = static_cast<f64>(pointer.x) - previous_.x;
    const auto dy = static_cast<f64>(pointer.y) - previous_.y;
    previous_ = pointer;
    if(dx==0 && dy==0)return;
    const double boost=fast?4.:1.;
    if (mode_ == Mode::orbit) {
        const auto before=s;
        const auto eye = look_in_place_ ? camera(s,view).position() : Vec3{};
        s.yaw = static_cast<f32>(std::remainder(s.yaw - dx * speeds_.rotation, 360.0));
        s.pitch =
            static_cast<f32>(std::clamp(s.pitch + dy * speeds_.rotation, -static_cast<f64>(camera_max_pitch),
                                        static_cast<f64>(camera_max_pitch)));
        if (look_in_place_) {
            const auto orbited = camera(s,view).position();
            for(std::size_t i=0;i<3;++i)
                s.target[i]=std::clamp(s.target[i]+eye[i]-orbited[i],-camera_target_limit,camera_target_limit);
        } else if(captured_origin_) {
            // Rotate the entire view frame about the reference, not just its
            // look direction. R(new) * transpose(R(old)) preserves the mesh's
            // camera-space position even after panning it off screen center.
            const auto basis=[](const CameraPose& pose) {
                const double yaw=pose.yaw*std::numbers::pi/180;
                const double pitch=pose.pitch*std::numbers::pi/180;
                return std::array{
                    std::array{std::cos(yaw),0.,-std::sin(yaw)},
                    std::array{-std::sin(yaw)*std::sin(pitch),std::cos(pitch),-std::cos(yaw)*std::sin(pitch)},
                    std::array{std::sin(yaw)*std::cos(pitch),std::sin(pitch),std::cos(yaw)*std::cos(pitch)}};
            };
            const auto old_axes=basis(before),new_axes=basis(s);
            std::array<double,3> local{};
            for(unsigned axis=0;axis<3;++axis)for(unsigned c=0;c<3;++c)
                local[axis]+=(double(before.target[c])-(*captured_origin_)[c])*old_axes[axis][c];
            for(unsigned c=0;c<3;++c) {
                double target=(*captured_origin_)[c];
                for(unsigned axis=0;axis<3;++axis)target+=local[axis]*new_axes[axis][c];
                s.target[c]=static_cast<float>(std::clamp(target,-double(camera_target_limit),double(camera_target_limit)));
            }
        }
    } else if (mode_ == Mode::dolly) {
        if(captured_origin_) {
            // Translate eye and look target together toward the object, not
            // the old camera-forward axis. Exponential approach cannot cross
            // the center and is independent of how motion events are batched.
            const auto eye=camera(s,view).position();
            const auto amount=1-std::exp(std::clamp(dy*.01*speeds_.forward*boost,-8.,8.));
            std::array<double,3> delta{};
            double fraction=1;
            for(unsigned i=0;i<3;++i) {
                delta[i]=(double((*captured_origin_)[i])-eye[i])*amount;
                if(delta[i]!=0)fraction=std::min(fraction,
                    ((delta[i]>0?camera_target_limit:-camera_target_limit)-s.target[i])/delta[i]);
            }
            for(unsigned i=0;i<3;++i)s.target[i]=static_cast<float>(s.target[i]+delta[i]*fraction);
        } else scroll(s, view, -dy * .01 * speeds_.forward * boost);
    } else {
        const auto yaw = static_cast<f64>(s.yaw) * std::numbers::pi / 180;
        const auto pitch = static_cast<f64>(s.pitch) * std::numbers::pi / 180;
        auto distance = s.distance * (view == ViewMode::scene ? 1.0 : .52);
        if(captured_origin_) {
            const auto eye=camera(s,view).position();
            const std::array<double,3> forward{-std::sin(yaw)*std::cos(pitch),-std::sin(pitch),-std::cos(yaw)*std::cos(pitch)};
            distance=0;
            for(unsigned i=0;i<3;++i)distance+=(double((*captured_origin_)[i])-eye[i])*forward[i];
            distance=std::max(.001,std::abs(distance));
        }
        const auto scale =
            2 * distance * std::tan(camera_vertical_fov * std::numbers::pi / 360) / (s.zoom * size_.y);
        const std::array<f64, 3> right{std::cos(yaw), 0, -std::sin(yaw)};
        const std::array<f64, 3> up{-std::sin(yaw) * std::sin(pitch), std::cos(pitch),
                                    -std::cos(yaw) * std::sin(pitch)};
        for (std::size_t i = 0; i < 3; ++i)
            s.target[i] = static_cast<f32>(std::clamp(
                s.target[i] + (-dx * right[i] + dy * up[i]) * scale * speeds_.pan * boost,
                -static_cast<f64>(camera_target_limit), static_cast<f64>(camera_target_limit)));
    }
}

bool NavigationTool::update(State& s, Vec2 origin, Vec2 size,
                            std::span<const input::Event> unhandled,
                            std::span<const input::Event> raw, bool enabled) {
    return update(view_camera(s), s.viewport.mode, s.viewport.smooth_zoom, origin, size, unhandled, raw, enabled);
}
bool NavigationTool::update(CameraPose& pose, ViewMode view, bool& smooth_zoom, Vec2 origin, Vec2 size,
                            std::span<const input::Event> unhandled,
                            std::span<const input::Event> raw, bool enabled) {
    handled_ = cancelled_ = false;
    if (!enabled || !finite(origin) || !finite(size) || size.x <= 0 || size.y <= 0 ||
        !valid_camera_pose(pose)) {
        handled_ = dragging_;
        cancel();
        return false;
    }
    if (dragging_ && (origin != origin_ || size != size_)) {
        handled_ = true;
        cancel();
    }
    const auto before = pose;
    const auto available = [&](const input::Event& event) {
        return std::ranges::any_of(unhandled, [&](const auto& candidate) {
            return candidate.kind == event.kind && candidate.button == event.button &&
                   candidate.position == event.position && candidate.scroll == event.scroll &&
                   candidate.modifiers.shift == event.modifiers.shift &&
                   candidate.modifiers.control == event.modifiers.control;
        });
    };
    for (const auto& event : raw.empty() ? unhandled : raw) {
        if (event.kind == input::EventKind::focus_lost ||
            (event.kind == input::EventKind::key_down && event.key == input::Key::escape)) {
            handled_ = handled_ || dragging_;
            cancel();
            break;
        }
        if (dragging_) {
            if (event.kind == input::EventKind::pointer_move) {
                handled_ = true;
                move(pose, view, smooth_zoom, event.position,event.modifiers.left_alt);
            } else if (event.kind == input::EventKind::pointer_up && event.button == 2) {
                handled_ = true;
                move(pose, view, smooth_zoom, event.position,event.modifiers.left_alt);
                dragging_ = false; // Successful release, not cancellation.
            }
            continue;
        }
        if (!contains(origin, size, event.position) || !available(event))
            continue;
        if (event.kind == input::EventKind::pointer_down && event.button == 2) {
            if(!orbit_enabled_ && !event.modifiers.shift && !event.modifiers.control && !event.modifiers.alt) continue;
            mode_ = event.modifiers.shift     ? Mode::pan
                    : event.modifiers.control ? Mode::dolly
                                              : Mode::orbit;
            captured_origin_=drag_origin_;
            if(captured_origin_ && (!std::isfinite(captured_origin_->x)||!std::isfinite(captured_origin_->y)||!std::isfinite(captured_origin_->z)))
                captured_origin_.reset();
            previous_ = event.position;
            origin_ = origin;
            size_ = size;
            dragging_ = handled_ = true;
        } else if (event.kind == input::EventKind::scroll && finite(event.scroll)) {
            if (event.scroll.y != 0) {
                handled_ = true;
                scroll(pose, view, static_cast<f64>(event.scroll.y) * .15 * (event.modifiers.left_alt?4.:1.));
                smooth_zoom = true;
            }
        }
    }
    return before != pose;
}
} // namespace editor_example
