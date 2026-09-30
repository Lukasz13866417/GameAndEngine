#include "camera_pointer_logic.hpp"
#include <vng/input/routing.hpp>
#include "../support/mesh_frame.hpp"

#include <algorithm>
#include <cmath>

namespace editor_example {
namespace {
using namespace vng;
constexpr double surface_navigation_speed=.7; // Finer control during surface-aware approach/zoom.
bool finite(Vec2 p) {
    return std::isfinite(p.x) && std::isfinite(p.y);
}
bool contains(Vec2 origin, Vec2 size, Vec2 p) {
    return finite(p) && p.x >= origin.x && p.y >= origin.y &&
           static_cast<f64>(p.x) < static_cast<f64>(origin.x) + size.x &&
           static_cast<f64>(p.y) < static_cast<f64>(origin.y) + size.y;
}
} // namespace

void CameraPointerLogic::drag_origin(std::optional<Vec3> origin,const editor::EditableMesh* mesh,Mat4 transform) {
    if(origin&&(!std::isfinite(origin->x)||!std::isfinite(origin->y)||!std::isfinite(origin->z)))origin.reset();
    drag_origin_=origin;
    surface_=nullptr;
    if(origin&&mesh)if(auto inverse=example::mesh_frame::inverse(transform)) {
        surface_=mesh;surface_inverse_=*inverse;
    }
}

std::optional<double> CameraPointerLogic::surface_distance(Vec3 eye,Vec3 center) const {
    if(!surface_)return {};
    const double distance=std::hypot(double(center.x)-eye.x,double(center.y)-eye.y,double(center.z)-eye.z);
    if(distance<=0)return {};
    // Transform the ray, not the mesh. Keep its parameter in world units even
    // under a non-uniform pending whole-mesh transform.
    spatial::Ray3 ray;
    for(unsigned r=0;r<3;++r) {
        ray.origin[r]=surface_inverse_[3][r];
        for(unsigned c=0;c<3;++c) {
            ray.origin[r]+=double(surface_inverse_[c][r])*eye[c];
            ray.direction[r]+=double(surface_inverse_[c][r])*(double(center[c])-eye[c])/distance;
        }
    }
    ray.maximum=distance;
    const auto hit=surface_->picking_index().intersect(ray);
    if(!hit||!std::isfinite(hit->distance))return {};
    return hit->distance;
}

void CameraPointerLogic::approach(CameraPose& s,ViewMode view,Vec3 center,double amount) {
    const auto eye=camera(s,view).position();
    const auto distance=std::hypot(double(center.x)-eye.x,double(center.y)-eye.y,double(center.z)-eye.z);
    if(distance<=0)return;
    const auto surface=surface_distance(eye,center);
    const auto clearance=surface.value_or(distance);
    if(surface)amount*=surface_navigation_speed;
    // Integrating proportional clearance produces the same approach for one
    // large input or many fractional ones, and cannot overshoot the surface.
    // A tiny floor keeps backing away possible at a rounded-to-zero clearance.
    const auto working=std::max(clearance,distance*1e-6);
    auto step=working*-std::expm1(std::clamp(-amount,-8.,8.));
    if(step>0)step=std::min(step,clearance);
    std::array<double,3> delta{};
    double fraction=1;
    for(unsigned i=0;i<3;++i) {
        delta[i]=(double(center[i])-eye[i])*step/distance;
        if(delta[i]!=0)fraction=std::min(fraction,
            ((delta[i]>0?camera_target_limit:-camera_target_limit)-s.target[i])/delta[i]);
    }
    for(unsigned i=0;i<3;++i)s.target[i]=static_cast<float>(s.target[i]+delta[i]*fraction);
}

void CameraPointerLogic::scroll(CameraPose& s, ViewMode view, double amount) {
    if (scroll_mode_ == ScrollMode::zoom) {
        if(view==ViewMode::mesh&&drag_origin_) {
            const auto eye=camera(s,view).position();
            if(const auto clearance=surface_distance(eye,*drag_origin_)) {
                const auto distance=std::hypot(double(drag_origin_->x)-eye.x,double(drag_origin_->y)-eye.y,double(drag_origin_->z)-eye.z);
                amount*=surface_navigation_speed*std::clamp(*clearance/distance,.015,1.);
            }
        }
        s.zoom = static_cast<f32>(std::clamp(static_cast<double>(s.zoom) *
            std::exp(std::clamp(amount, -32., 32.)), double(camera_min_zoom), double(camera_max_zoom)));
        return;
    }
    if(view==ViewMode::mesh&&drag_origin_&&surface_&&surface_distance(camera(s,view).position(),s.target)) {
        // Keep wheel movement camera-forward, including after panning. Only
        // its speed changes; Ctrl-drag still approaches the mesh center.
        approach(s,view,s.target,amount);
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

void CameraPointerLogic::cancel() noexcept {
    cancelled_ |= dragging_;
    dragging_ = false;
}

void CameraPointerLogic::move(CameraPose& s, ViewMode view, bool& smooth_zoom, Vec2 pointer, bool fast) {
    if (!finite(pointer))
        return;
    smooth_zoom = false;
    const auto dx = static_cast<f64>(pointer.x) - previous_.x;
    const auto dy = static_cast<f64>(pointer.y) - previous_.y;
    previous_ = pointer;
    if(dx==0 && dy==0)return;
    const double boost=fast?4.:1.;
    if (mode_ == DragMode::orbit || mode_ == DragMode::look) {
        const auto before=s;
        const bool look=mode_==DragMode::look;
        const auto eye = look ? camera(s,view).position() : Vec3{};
        s.yaw = static_cast<f32>(std::remainder(s.yaw - dx * speeds_.rotation, 360.0));
        s.pitch =
            static_cast<f32>(std::clamp(s.pitch + dy * speeds_.rotation, -static_cast<f64>(camera_max_pitch),
                                        static_cast<f64>(camera_max_pitch)));
        if (look) {
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
    } else if (mode_ == DragMode::dolly || mode_ == DragMode::zoom) {
        if(captured_origin_ && mode_!=DragMode::zoom) {
            approach(s,view,*captured_origin_,-dy*.01*speeds_.forward*boost);
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
            if(view==ViewMode::mesh)if(const auto clearance=surface_distance(eye,*captured_origin_)) {
                const auto radius=std::hypot(double(captured_origin_->x)-eye.x,
                    double(captured_origin_->y)-eye.y,double(captured_origin_->z)-eye.z);
                // Pan in the same camera plane, but at the visible surface's
                // depth rather than the much deeper mesh-center plane.
                distance*=surface_navigation_speed*std::clamp(*clearance/radius,1e-6,1.);
            }
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

bool CameraPointerLogic::update(State& s, Vec2 origin, Vec2 size,
                            std::span<const input::Event> unhandled,
                            std::span<const input::Event> raw, bool enabled) {
    return update(view_camera(s), s.viewport.mode, s.viewport.smooth_zoom, origin, size, unhandled, raw, enabled);
}
bool CameraPointerLogic::update(CameraPose& pose, ViewMode view, bool& smooth_zoom, Vec2 origin, Vec2 size,
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
    const input::AvailableEvents available{unhandled};
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
            } else if (event.kind == input::EventKind::pointer_up && event.button == captured_button_) {
                handled_ = true;
                move(pose, view, smooth_zoom, event.position,event.modifiers.left_alt);
                dragging_ = false; // Successful release, not cancellation.
            }
            continue;
        }
        if (!contains(origin, size, event.position) || !available.contains(event))
            continue;
        if (event.kind == input::EventKind::pointer_down && (event.button == 2 || (primary_button_ && event.button==0))) {
            if(!orbit_enabled_ && !event.modifiers.shift && !event.modifiers.control && !event.modifiers.alt) continue;
            mode_ = gesture_.value_or(event.modifiers.shift ? DragMode::pan
                    : event.modifiers.control ? DragMode::dolly : DragMode::orbit);
            captured_button_=event.button;
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
                scroll(pose, view, static_cast<f64>(event.scroll.y) * .15 *
                    (scroll_mode_==ScrollMode::zoom?speeds_.zoom:speeds_.forward) * (event.modifiers.left_alt?4.:1.));
                smooth_zoom = true;
            }
        }
    }
    return before != pose;
}
} // namespace editor_example
