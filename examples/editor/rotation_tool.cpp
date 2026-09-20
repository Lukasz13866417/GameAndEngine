#include "rotation_tool.hpp"
#include "rotation_math.hpp"
#include "transform_keys.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace editor_example {
namespace {
using namespace vng;
constexpr f64 pi = std::numbers::pi_v<f64>;
constexpr std::array<f32, 3> radii{56, 62, 68};
constexpr f64 degrees_per_radian = 180.0 / pi;
bool finite(Vec2 p) { return std::isfinite(p.x) && std::isfinite(p.y); }
bool finite(Vec3 p) { return finite(Vec2{p.x, p.y}) && std::isfinite(p.z); }
Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 scale(Vec3 a, f64 s) {
    return {static_cast<f32>(a.x * s), static_cast<f32>(a.y * s), static_cast<f32>(a.z * s)};
}
f64 dot(Vec3 a, Vec3 b) { return gfx::camera_detail::dot(a, b); }
Vec3 unit(Vec3 value) { return gfx::camera_detail::normalize(value, dot(value, value)); }
Vec3 cross(Vec3 a, Vec3 b) { return gfx::camera_detail::cross(a, b); }
f64 squared(Vec2 a) { return static_cast<f64>(a.x) * a.x + static_cast<f64>(a.y) * a.y; }
bool same_rect(ui::Rect a, ui::Rect b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}
struct Projected { Vec2 point; Vec4 clip; };
std::optional<Projected> project(Vec3 point, const gfx::CameraSnapshot& camera, ui::Rect viewport) {
    Vec4 clip{};
    for (std::size_t row = 0; row < 4; ++row) {
        f64 value = camera.view_projection[3][row];
        for (std::size_t column = 0; column < 3; ++column)
            value += static_cast<f64>(camera.view_projection[column][row]) * point[column];
        clip[row] = static_cast<f32>(value);
        if (!std::isfinite(clip[row])) return {};
    }
    if (clip.w <= 1e-6F || clip.z < -clip.w || clip.z > clip.w) return {};
    Vec2 p{viewport.x + (clip.x / clip.w + 1) * .5F * viewport.width,
           viewport.y + (1 - clip.y / clip.w) * .5F * viewport.height};
    if (!finite(p)) return {};
    return Projected{p, clip};
}
Vec3 on_circle(Vec3 center, Vec3 u, Vec3 v, f32 radius, f64 angle) {
    return add(center, add(scale(u, radius * std::cos(angle)), scale(v, radius * std::sin(angle))));
}
} // namespace

void RotationTool::cancel() noexcept {
    dragging_ = visible_ = false;
    keyboard_=false;
    ghost_ = target_.rotation_degrees;
}

void RotationTool::geometry() {
    rings_ = {};
    planes_ = {};
    visible_ = false;
    const auto center = project(target_.position, camera_, viewport_);
    if (!center || !viewport_.contains(center->point)) return;
    screen_origin_ = center->point;
    // A camera-up world unit has a stable screen scale for both lens kinds.
    // Use the exact perspective derivative, not an offset that can cross a plane.
    Vec4 direction{};
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 3; ++column)
            direction[row] += camera_.view_projection[column][row] * camera_.up[column];
    const auto w = static_cast<f64>(center->clip.w);
    const auto pixels_per_unit = std::hypot(
        (direction.x * w - center->clip.x * direction.w) / (w * w) * viewport_.width * .5,
        (direction.y * w - center->clip.y * direction.w) / (w * w) * viewport_.height * .5);
    if (!std::isfinite(pixels_per_unit) || pixels_per_unit < 1e-6) return;

    if (target_.free_rotation) {
        // A single camera-facing halo marks the center, not three axis handles.
        auto& ring = rings_[0];
        ring.normal = camera_.forward;
        ring.label = "MMB drag";
        for (std::size_t i=0; i<=ring_segments; ++i) {
            const auto p=project(on_circle(target_.position,camera_.right,camera_.up,
                static_cast<f32>(68/pixels_per_unit),2*pi*static_cast<f64>(i)/ring_segments),camera_,viewport_);
            if(p) { ring.projected[i]=true;ring.points[i]=p->point;visible_=true; }
        }
        return;
    }

    const auto y = static_cast<f64>(ghost_.y) / degrees_per_radian;
    const auto z = static_cast<f64>(ghost_.z) / degrees_per_radian;
    const auto sy = std::sin(y), cy = std::cos(y), sz = std::sin(z), cz = std::cos(z);
    const std::array normals{
        Vec3{static_cast<f32>(cz * cy), static_cast<f32>(sz * cy), static_cast<f32>(-sy)},
        Vec3{static_cast<f32>(-sz), static_cast<f32>(cz), 0}, Vec3{0, 0, 1}};
    for (u32 axis = 0; axis < 3; ++axis) {
        if(target_.only_axis && axis!=*target_.only_axis)continue;
        const auto normal = target_.local_axes ? rotation_math::direction(ghost_,(*target_.local_axes)[axis]) : normals[axis];
        const auto reference = std::abs(normal.z) < .8F ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
        const auto u = unit(cross(reference, normal));
        const auto v = cross(normal, u);
        auto& plane = planes_[axis];
        plane = {normal, u, v, static_cast<f32>(radii[axis] / pixels_per_unit)};
        auto& ring = rings_[axis];
        ring.normal = normal;
        ring.label = target_.local_axes ? std::array<std::string_view,3>{"Yaw","Pitch","Roll"}[axis]
                                       : std::array<std::string_view,3>{"X","Y","Z"}[axis];
        if(target_.only_axis)ring.label="Heading (R)";
        for (std::size_t i = 0; i <= ring_segments; ++i) {
            const auto point = project(on_circle(target_.position, u, v, plane.radius,
                                                 2 * pi * static_cast<f64>(i) / ring_segments), camera_, viewport_);
            if (point) {
                ring.projected[i] = true;
                ring.points[i] = point->point;
                visible_ = true;
            }
        }
    }
}

std::optional<RotationTool::Hit> RotationTool::hit(vng::Vec2 pointer) const noexcept {
    if (!visible_ || !finite(pointer) || !viewport_.contains(pointer)) return {};
    f64 best = 7 * 7;
    std::optional<Hit> result;
    for (u32 axis = 0; axis < 3; ++axis) {
        const auto& ring = rings_[axis];
        for (std::size_t i = 0; i < ring_segments; ++i) {
            if (!ring.projected[i] || !ring.projected[i + 1]) continue;
            const auto a = ring.points[i], b = ring.points[i + 1];
            const Vec2 line{b.x - a.x, b.y - a.y}, offset{pointer.x - a.x, pointer.y - a.y};
            const auto length = squared(line);
            if (length < 1e-8) continue;
            const auto t = std::clamp((static_cast<f64>(offset.x) * line.x +
                                       static_cast<f64>(offset.y) * line.y) / length, 0.0, 1.0);
            const auto distance = squared({static_cast<f32>(offset.x - t * line.x),
                                            static_cast<f32>(offset.y - t * line.y)});
            if (distance < best) {
                best = distance;
                result = Hit{axis, 2 * pi * (static_cast<f64>(i) + t) / ring_segments};
            }
        }
    }
    return result;
}
std::optional<vng::u32> RotationTool::hit_axis(vng::Vec2 pointer) const noexcept {
    const auto result = hit(pointer);
    return result ? std::optional{result->axis} : std::nullopt;
}

std::optional<vng::f64> RotationTool::angle(vng::Vec2 pointer, const Plane& plane) const {
    if (!finite(pointer)) return {};
    const auto nx = (static_cast<f64>(pointer.x) - viewport_.x) / viewport_.width * 2 - 1;
    const auto ny = 1 - (static_cast<f64>(pointer.y) - viewport_.y) / viewport_.height * 2;
    const auto x = nx / camera_.projection[0][0], y = ny / camera_.projection[1][1];
    auto origin = camera_.position;
    auto direction = camera_.forward;
    const auto screen_offset = add(scale(camera_.right, x), scale(camera_.up, y));
    if (std::abs(camera_.projection[2][3]) > .5F)
        direction = unit(add(direction, screen_offset));
    else
        origin = add(origin, screen_offset);
    const auto denominator = dot(direction, plane.normal);
    if (!std::isfinite(denominator) || std::abs(denominator) < .025) return {};
    const auto distance = dot(sub(target_.position, origin), plane.normal) / denominator;
    if (!std::isfinite(distance) || distance <= 0) return {};
    const auto radial = sub(add(origin, scale(direction, distance)), target_.position);
    if (!finite(radial) || dot(radial, radial) < static_cast<f64>(plane.radius) * plane.radius * 1e-8)
        return {};
    return std::atan2(dot(radial, plane.v), dot(radial, plane.u));
}

void RotationTool::move(vng::Vec2 pointer) {
    if (!finite(pointer)) return;
    pointer_=pointer;
    if (target_.free_rotation) {
        // Compose camera-relative turns from the captured pose, never add to
        // its Euler components. This also works on already rotated objects.
        constexpr f64 degrees_per_pixel=.25;
        const auto yaw=rotation_math::matrix(rotation_math::turn({},camera_.up,(pointer.x-start_.x)*degrees_per_pixel));
        const auto pitch=rotation_math::matrix(rotation_math::turn({},camera_.right,(pointer.y-start_.y)*degrees_per_pixel));
        const auto spin=rotation_math::matrix(rotation_math::turn({},scale(camera_.forward,-1),keyboard_angle_));
        const auto delta=rotation_math::multiply(rotation_math::multiply(spin,rotation_math::multiply(pitch,yaw)),rotation_math::matrix(segment_delta_));
        delta_=rotation_math::euler(delta,delta_);
        ghost_=rotation_math::euler(rotation_math::multiply(delta,rotation_math::matrix(target_.rotation_degrees)),ghost_);
        return;
    }
    if (fallback_) {
        // An edge-on plane has no stable ray intersection. Tangential screen
        // motion remains well-defined; one ring radius is one radian of travel.
        accumulated_angle_ = segment_angle_+((static_cast<f64>(pointer.x) - start_.x) * fallback_tangent_.x +
                               (static_cast<f64>(pointer.y) - start_.y) * fallback_tangent_.y) /
                              radii[axis_];
    } else if (const auto next = angle(pointer, drag_plane_)) {
        accumulated_angle_ += std::remainder(*next - previous_angle_, 2 * pi);
        previous_angle_ = *next;
    }
    apply_turn();
}
void RotationTool::apply_turn() {
    const auto degrees=accumulated_angle_*degrees_per_radian+keyboard_angle_;
    if(target_.local_axes)
        ghost_=rotation_math::turn(target_.rotation_degrees,(*target_.local_axes)[axis_],degrees);
    else {
        ghost_ = target_.rotation_degrees;
        ghost_[axis_] = static_cast<f32>(std::clamp(
            static_cast<f64>(ghost_[axis_]) + degrees, -360.0, 360.0));
    }
}
RotationTool::Turn RotationTool::turn() const noexcept { return {axis_,accumulated_angle_*degrees_per_radian+keyboard_angle_}; }

std::optional<vng::Vec3>
RotationTool::update(const RotationGizmo& target, const vng::gfx::CameraSnapshot& camera,
                     vng::ui::Rect viewport, std::span<const vng::input::Event> unhandled,
                     std::span<const vng::input::Event> raw, bool enabled, float arrow_step) {
    using namespace vng;
    handled_ = false;
    if(target.only_axis && *target.only_axis>=3)enabled=false;
    if(target.local_axes) for(unsigned i=0;i<3;++i) {
        const auto axis=(*target.local_axes)[i];
        enabled &= finite(axis) && std::abs(dot(axis,axis)-1)<1e-4;
        for(unsigned j=0;j<i;++j) enabled &= std::abs(dot(axis,(*target.local_axes)[j]))<1e-4;
    }
    if (!enabled || !finite(target.position) || !finite(target.rotation_degrees) ||
        !finite(Vec2{viewport.x, viewport.y}) || !finite(Vec2{viewport.width, viewport.height}) ||
        viewport.width <= 0 || viewport.height <= 0 || !finite(camera.position) ||
        !finite(camera.up) || !finite(camera.right) || !finite(camera.forward) ||
        !std::isfinite(camera.projection[0][0]) || !std::isfinite(camera.projection[1][1]) ||
        camera.projection[0][0] == 0 || camera.projection[1][1] == 0) {
        handled_ = dragging_;
        cancel();
        return {};
    }
    if (dragging_ && (target.stamp != target_.stamp || target.position != target_.position ||
                     target.rotation_degrees != target_.rotation_degrees || target.local_axes != target_.local_axes ||
                     target.free_rotation != target_.free_rotation || target.only_axis!=target_.only_axis ||
                     !same_rect(viewport, viewport_))) {
        handled_ = true;
        cancel();
    }
    const bool reframe=dragging_&&camera.view_projection!=camera_.view_projection;
    target_ = target;
    camera_ = camera;
    viewport_ = viewport;
    if (!dragging_) ghost_ = target.rotation_degrees;
    geometry();
    if(reframe) {
        start_=pointer_;
        if(target_.free_rotation) {segment_delta_=delta_;keyboard_angle_=0;}
        else {
            accumulated_angle_+=keyboard_angle_/degrees_per_radian;keyboard_angle_=0;
            segment_angle_=accumulated_angle_;drag_plane_=planes_[axis_];
            const auto initial=angle(pointer_,drag_plane_);
            fallback_=!initial;previous_angle_=initial.value_or(0);
            const auto dx=pointer_.x-screen_origin_.x,dy=pointer_.y-screen_origin_.y;
            const auto length=std::hypot(dx,dy);
            fallback_tangent_=length>1?Vec2{dy/length,-dx/length}:Vec2{1,0};
        }
    }
    const auto begins = [&](const input::Event& event) {
        return std::ranges::any_of(unhandled, [&](const auto& available) {
            return available.kind == event.kind && available.button == event.button &&
                   available.position == event.position;
        });
    };
    const auto events = raw.empty() ? unhandled : raw;
    for (const auto& event : events) {
        if(auto arrow=transform_arrow(event,arrow_step);arrow && (dragging_ || (visible_ && viewport_.contains(event.position) &&
            std::ranges::any_of(unhandled,[&](const auto& e){return e.kind==event.kind&&e.key==event.key;})))) {
            if(!dragging_) {
                axis_=target_.only_axis.value_or(hit_axis(event.position).value_or(2));
                pointer_=start_=event.position;segment_delta_={};segment_angle_=accumulated_angle_=keyboard_angle_=0;keyboard_=dragging_=true;
            }
            keyboard_angle_+=rotation_arrow(*arrow);handled_=true;
            if(target_.free_rotation)move(pointer_);else apply_turn();
            geometry();continue;
        }
        if (dragging_) {
            if(keyboard_ && ((event.kind==input::EventKind::key_down&&event.key==input::Key::enter) ||
                (event.kind==input::EventKind::pointer_down&&event.button==0))) {
                handled_=true;dragging_=keyboard_=false;return ghost_;
            }
            if (event.kind == input::EventKind::focus_lost ||
                (event.kind == input::EventKind::key_down && event.key == input::Key::escape) ||
                (event.kind == input::EventKind::pointer_down && event.button == 1)) {
                handled_ = true;
                cancel();
                geometry();
                return {};
            }
            if (!keyboard_ && event.kind == input::EventKind::pointer_move) {
                handled_ = true;
                // Keep ordered angular samples so crossing +/-pi cannot flip
                // direction when several pointer events arrive in one UI frame.
                move(event.position);
            }
            if (!keyboard_ && event.kind == input::EventKind::pointer_up && event.button == (target_.free_rotation ? 2 : 0)) {
                handled_ = true;
                move(event.position);
                geometry();
                dragging_ = false;
                return ghost_;
            }
            continue;
        }
        if (event.kind != input::EventKind::pointer_down || event.button != (target_.free_rotation ? 2 : 0) || !begins(event))
            continue;
        if (target_.free_rotation) {
            if(!visible_ || !finite(event.position) || !viewport_.contains(event.position) ||
               event.modifiers.shift || event.modifiers.control || event.modifiers.alt || event.modifiers.super) continue;
            pointer_=start_=event.position;segment_delta_=delta_={};segment_angle_=keyboard_angle_=0;keyboard_=false;dragging_=handled_=true;
            continue;
        }
        const auto picked = hit(event.position);
        if (!picked) continue;
        axis_ = picked->axis;
        drag_plane_ = planes_[axis_];
        pointer_=start_ = event.position;
        segment_angle_=accumulated_angle_ = 0;
        keyboard_angle_=0;keyboard_=false;
        const auto initial = angle(start_, drag_plane_);
        const auto view_direction = std::abs(camera_.projection[2][3]) > .5F
            ? unit(sub(target_.position, camera_.position)) : camera_.forward;
        fallback_ = std::abs(dot(drag_plane_.normal, view_direction)) < .15 || !initial;
        previous_angle_ = initial.value_or(picked->angle);
        if (fallback_) {
            auto tangent_angle = picked->angle;
            Vec2 tangent{};
            f64 length{};
            // At the tip of a collapsed ellipse the derivative vanishes. Step
            // slightly around the same ring for a deterministic finite tangent.
            for (int attempt = 0; attempt < 3; ++attempt, tangent_angle += .2) {
                const auto a = project(on_circle(target_.position, drag_plane_.u, drag_plane_.v,
                                                  drag_plane_.radius, tangent_angle - .01), camera_, viewport_);
                const auto b = project(on_circle(target_.position, drag_plane_.u, drag_plane_.v,
                                                  drag_plane_.radius, tangent_angle + .01), camera_, viewport_);
                if (!a || !b) continue;
                tangent = {b->point.x - a->point.x, b->point.y - a->point.y};
                length = std::hypot(tangent.x, tangent.y);
                if (length > .1) break;
            }
            if (!std::isfinite(length) || length <= 1e-6) continue;
            fallback_tangent_ = {static_cast<f32>(tangent.x / length),
                                 static_cast<f32>(tangent.y / length)};
        }
        dragging_ = handled_ = true;
    }
    if (dragging_) geometry();
    return {};
}

void RotationTool::append(vng::ui::DrawList& list, const vng::text::Font& font, int only_axis) const {
    using namespace vng;
    if (!visible_) return;
    constexpr std::array colors{Vec4{1, .12F, .08F, 1}, Vec4{.1F, 1, .2F, 1},
                                Vec4{.15F, .45F, 1, 1}};
    for (u32 axis = 0; axis < 3; ++axis) {
        if (target_.free_rotation && axis!=0) continue;
        if (only_axis >= 0 && axis != static_cast<u32>(only_axis)) continue;
        const auto& ring = rings_[axis];
        const bool active = dragging_ && (target_.free_rotation || axis == axis_);
        const auto color = active ? Vec4{1, .8F, .15F, 1} : target_.free_rotation ? Vec4{.3F,.85F,1,1} : colors[target_.local_axes && axis<2 ? 1-axis : axis];
        const f32 radius = active ? 2.2F : 1.65F;
        for (std::size_t i = 0; i < ring_segments; ++i) {
            if (!ring.projected[i] || !ring.projected[i + 1]) continue;
            const auto a = ring.points[i], b = ring.points[i + 1];
            const auto length = std::hypot(b.x - a.x, b.y - a.y);
            // Do not generate unbounded UI work for almost-clipped geometry.
            const auto samples = std::clamp(static_cast<int>(std::min(length / 1.8F, 128.F)) + 1, 1, 128);
            for (int j = 0; j < samples; ++j) {
                const auto t = static_cast<f32>(j) / static_cast<f32>(samples);
                const Vec2 point{a.x + t * (b.x - a.x), a.y + t * (b.y - a.y)};
                if (!viewport_.contains(point)) continue;
                list.commands.emplace_back(ui::BoxDraw{
                    {point.x - radius, point.y - radius, radius * 2, radius * 2}, viewport_,
                    color, {0, 0, 0, .7F}, radius, .3F});
            }
        }
        if(font && target_.free_rotation) {
            const Vec2 label{screen_origin_.x-78,screen_origin_.y+78};
            list.commands.emplace_back(ui::TextDraw{"Free rotate / MMB drag",label,viewport_,font,14,color});
        } else if(font && target_.local_axes) {
            const Vec2 label{screen_origin_.x+80,screen_origin_.y-32+static_cast<f32>(axis)*23.F};
            list.commands.emplace_back(ui::BoxDraw{{label.x-3,label.y-2,target_.only_axis?108.F:64.F,21},viewport_,{.035F,.045F,.07F,.9F},{},3,0});
            list.commands.emplace_back(ui::TextDraw{std::string(ring.label),label,viewport_,font,14,color});
        }
    }
}
} // namespace editor_example
