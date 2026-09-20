#include "world_bounds_tool.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace editor_example {
namespace {
using namespace vng;
Vec4 clip(Vec3 point, const gfx::CameraSnapshot& camera) {
    Vec4 result{};
    for (unsigned r = 0; r < 4; ++r) {
        double value = camera.view_projection[3][r];
        for (unsigned c = 0; c < 3; ++c) value += double(camera.view_projection[c][r]) * point[c];
        result[r] = static_cast<f32>(value);
    }
    return result;
}
Vec2 screen(Vec4 p, ui::Rect viewport) {
    return {viewport.x + (p.x / p.w + 1) * .5F * viewport.width,
            viewport.y + (1 - p.y / p.w) * .5F * viewport.height};
}
bool inside(Vec4 p) {
    return std::isfinite(p.w) && p.w > 0 && std::abs(p.x) <= p.w &&
        std::abs(p.y) <= p.w && std::abs(p.z) <= p.w;
}
// Clip before perspective divide: borders remain visible even with the camera
// inside the cuboid or with an edge crossing the near plane.
bool segment(Vec4& a, Vec4& b) {
    for (unsigned plane = 0; plane < 6; ++plane) {
        const auto axis = plane / 2;
        const auto sign = plane % 2 ? 1.F : -1.F;
        const auto da = a.w + sign * a[axis], db = b.w + sign * b[axis];
        if (da < 0 && db < 0) return false;
        if ((da < 0) != (db < 0)) {
            const auto t = da / (da - db);
            Vec4 intersection{};
            for (unsigned i = 0; i < 4; ++i) intersection[i] = a[i] + t * (b[i] - a[i]);
            (da < 0 ? a : b) = intersection;
        }
    }
    return a.w > 0 && b.w > 0;
}
void line(ui::DrawList& list, Vec2 a, Vec2 b, ui::Rect viewport, Vec4 color) {
    const auto dx = b.x - a.x, dy = b.y - a.y, length = std::hypot(dx, dy);
    if (!std::isfinite(length) || length < .01F) return;
    const Vec2 n{-dy / length, dx / length};
    const Vec2 p{a.x+n.x, a.y+n.y}, q{a.x-n.x, a.y-n.y};
    const Vec2 r{b.x+n.x, b.y+n.y}, s{b.x-n.x, b.y-n.y};
    list.commands.emplace_back(ui::TriangleDraw{{p,q,r}, viewport, color});
    list.commands.emplace_back(ui::TriangleDraw{{q,s,r}, viewport, color});
}
}
void WorldBoundsTool::geometry() {
    handles_ = {};
    for (unsigned face = 0; face < 6; ++face) {
        Vec3 point{};
        for (unsigned c = 0; c < 3; ++c) point[c] = (value_.minimum[c] + value_.maximum[c]) * .5F;
        const auto axis = face / 2;
        point[axis] = face % 2 ? value_.maximum[axis] : value_.minimum[axis];
        const auto p = clip(point, camera_);
        if (!inside(p)) continue;
        const auto d = camera_.view_projection[axis];
        const double w = p.w;
        const Vec2 derivative{
            static_cast<f32>((double(d.x)*w - double(p.x)*d.w)/(w*w)*viewport_.width*.5),
            static_cast<f32>(-(double(d.y)*w - double(p.y)*d.w)/(w*w)*viewport_.height*.5)};
        const auto length = std::hypot(derivative.x, derivative.y);
        if (!std::isfinite(length) || length < 1e-7F) continue;
        handles_[face] = {screen(p, viewport_), {derivative.x/length, derivative.y/length}, length, d.w/p.w, true};
    }
}
void WorldBoundsTool::move(Vec2 point) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) return;
    pointer_=point;
    const double pixels = (point.x-start_.x)*axis_.direction.x + (point.y-start_.y)*axis_.direction.y;
    const double denominator = std::max(double(axis_.pixels)*1e-5, axis_.pixels-pixels*axis_.ratio);
    const auto axis = face_ / 2;
    auto next = initial_;
    if (face_ % 2) next.maximum[axis] = static_cast<f32>(std::clamp(initial_.maximum[axis] + pixels/denominator,
        std::max(double(initial_.minimum[axis]) + world_min_extent,
                 double(std::nextafter(initial_.minimum[axis], std::numeric_limits<f32>::infinity()))), double(scene_coordinate_limit)));
    else next.minimum[axis] = static_cast<f32>(std::clamp(initial_.minimum[axis] + pixels/denominator,
        -double(scene_coordinate_limit),
        std::min(double(initial_.maximum[axis]) - world_min_extent,
                 double(std::nextafter(initial_.maximum[axis], -std::numeric_limits<f32>::infinity())))));
    if (valid_world_bounds(next)) value_ = next;
}
BoundsAction WorldBoundsTool::update(const WorldBounds& bounds, const gfx::CameraSnapshot& camera,
    ui::Rect viewport, std::span<const input::Event> unhandled, std::span<const input::Event> raw,
    bool visible, bool editable) {
    BoundsAction action{bounds};
    handled_ = false;
    const bool reframe=camera.view_projection!=camera_.view_projection;
    const bool changed_view = viewport.x != viewport_.x ||
        viewport.y != viewport_.y || viewport.width != viewport_.width || viewport.height != viewport_.height;
    if (!visible || !valid_world_bounds(bounds) || viewport.width <= 0 || viewport.height <= 0 ||
        (dragging_ && (!editable || changed_view || bounds != value_))) {
        action.cancelled = action.finished = handled_ = dragging_;
        cancel(); return action;
    }
    camera_ = camera; viewport_ = viewport; visible_ = visible; editable_ = editable;
    if (!dragging_) value_ = bounds;
    geometry();
    if(dragging_&&reframe) {
        initial_=value_;start_=pointer_;
        if(handles_[face_].visible)axis_=handles_[face_];
    }
    for (const auto& event : raw.empty() ? unhandled : raw) {
        using K = input::EventKind;
        if (event.kind == K::focus_lost || (event.kind == K::key_down && event.key == input::Key::escape)) {
            action.cancelled = action.finished = handled_ = dragging_;
            cancel(); return action;
        }
        if (!dragging_ && editable && event.kind == K::pointer_down && event.button == 0 && viewport.contains(event.position) &&
            std::ranges::any_of(unhandled, [&](const auto& e) {return e.kind == event.kind && e.button == 0 && e.position == event.position;})) {
            float best = 12.F;
            for (unsigned face = 0; face < 6; ++face) {
                const auto& handle = handles_[face];
                const auto distance = std::hypot(event.position.x-handle.point.x, event.position.y-handle.point.y);
                if (handle.visible && distance < best) { best = distance; face_ = face; }
            }
            if (best < 12.F) {
                axis_ = handles_[face_]; pointer_=start_ = event.position; initial_ = bounds;
                dragging_ = handled_ = action.began = true;
            }
        }
        if (dragging_ && (event.kind == K::pointer_move || (event.kind == K::pointer_up && event.button == 0))) {
            const auto before = value_;
            move(event.position); action.changed |= before != value_; handled_ = true;
            if (event.kind == K::pointer_up) { dragging_ = false; action.finished = true; }
        }
    }
    geometry(); action.value = value_;
    return action;
}
std::optional<Vec2> WorldBoundsTool::handle(unsigned face) const {
    if (visible_ && editable_ && face < 6 && handles_[face].visible) return handles_[face].point;
    return {};
}
void WorldBoundsTool::append(ui::DrawList& list, const text::Font& font, bool boundary) const {
    if (!visible_) return;
    std::array<Vec4, 8> corners;
    for (unsigned i = 0; i < 8; ++i) {
        Vec3 point;
        for (unsigned axis = 0; axis < 3; ++axis) point[axis] = i & (1U << axis) ? value_.maximum[axis] : value_.minimum[axis];
        corners[i] = clip(point, camera_);
    }
    for (unsigned i = 0; boundary && i < 8; ++i) for (unsigned axis = 0; axis < 3; ++axis) {
        if (i & (1U << axis)) continue;
        auto a = corners[i], b = corners[i | (1U << axis)];
        if (segment(a,b)) line(list, screen(a,viewport_), screen(b,viewport_), viewport_, {.4F,.8F,1,.65F});
    }
    constexpr std::array labels{"-X","+X","-Y","+Y","-Z","+Z"};
    constexpr std::array colors{Vec4{1,.25F,.18F,1},Vec4{.25F,1,.3F,1},Vec4{.2F,.55F,1,1}};
    for (unsigned face = 0; face < 6; ++face) if (auto p = handle(face)) {
        const auto color = dragging_ && face_ == face ? Vec4{1,.85F,.15F,1} : colors[face/2];
        list.commands.emplace_back(ui::BoxDraw{{p->x-6,p->y-6,12,12},viewport_,color,{0,0,0,1},1,1});
        if (font) list.commands.emplace_back(ui::TextDraw{labels[face],{p->x+9,p->y-8},viewport_,font,14,color});
    }
}
}
