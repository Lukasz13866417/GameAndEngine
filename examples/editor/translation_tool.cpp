#include "translation_tool.hpp"
#include "scene_coordinates.hpp"
#include "transform_keys.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace editor_example {
namespace {
using namespace vng;
struct Projected {
    Vec2 point;
    Vec4 clip;
};
std::optional<Projected> project(Vec3 point, const gfx::CameraSnapshot& camera, ui::Rect viewport) {
    Vec4 clip{};
    for (std::size_t row = 0; row < 4; ++row) {
        f64 value = camera.view_projection[3][row];
        for (std::size_t column = 0; column < 3; ++column)
            value += static_cast<f64>(camera.view_projection[column][row]) * point[column];
        clip[row] = static_cast<f32>(value);
        if (!std::isfinite(clip[row]))
            return {};
    }
    if (clip.w <= 1e-6F || clip.z < -clip.w || clip.z > clip.w)
        return {};
    return Projected{{viewport.x + (clip.x / clip.w + 1) * .5F * viewport.width,
                      viewport.y + (1 - clip.y / clip.w) * .5F * viewport.height},
                     clip};
}
bool same_rect(ui::Rect a, ui::Rect b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}
bool finite(Vec2 p) {
    return std::isfinite(p.x) && std::isfinite(p.y);
}
bool finite(Vec3 p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
f32 dot(Vec2 a, Vec2 b) {
    return a.x * b.x + a.y * b.y;
}
Vec2 sub(Vec2 a, Vec2 b) {
    return {a.x - b.x, a.y - b.y};
}
} // namespace

void TranslationTool::cancel() noexcept {
    dragging_ = false;
    keyboard_=false;
    visible_ = false;
    ghost_ = origin_;
}
void TranslationTool::geometry() {
    axes_.clear();
    visible_ = false;
    const auto projected = project(ghost_, camera_, viewport_);
    if (!projected || !finite(projected->point) || !viewport_.contains(projected->point))
        return;
    screen_origin_ = projected->point;
    for (std::size_t i = 0; world_axes_ && i < 3; ++i) {
        Axis axis;
        axis.world[i] = 1;
        axis.label = std::string(1, "XYZ"[i]);
        axes_.push_back(axis);
    }
    for (std::size_t i = 0; i < extra_axes_.size(); ++i) {
        const auto& definition = extra_axes_[i];
        const auto magnitude = std::hypot(static_cast<f64>(definition.direction.x),
                                         static_cast<f64>(definition.direction.y),
                                         static_cast<f64>(definition.direction.z));
        if (!finite(definition.direction) || magnitude <= 1e-6) continue;
        for (const auto sign : {1.F, -1.F}) {
            Axis axis;
            for (std::size_t c = 0; c < 3; ++c)
                axis.world[c] = static_cast<f32>(sign * definition.direction[c] / magnitude);
            axis.label = definition.label;
            axis.length = 85.F + static_cast<f32>(i) * 16.F;
            axis.custom = true;
            axis.reverse = sign < 0;
            axes_.push_back(std::move(axis));
        }
    }
    for (auto& projected_axis : axes_) {
        const auto c = projected->clip;
        // Derivative of perspective divide, in logical pixels per world unit.
        // It also works for orthographic projection, where dw/daxis is zero.
        Vec4 axis{};
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t column = 0; column < 3; ++column)
                axis[row] += camera_.view_projection[column][row] * projected_axis.world[column];
        const auto w = static_cast<f64>(c.w);
        const Vec2 derivative{
            static_cast<f32>((static_cast<f64>(axis.x) * w - static_cast<f64>(c.x) * axis.w) /
                             (w * w) * viewport_.width * .5),
            static_cast<f32>(-(static_cast<f64>(axis.y) * w - static_cast<f64>(c.y) * axis.w) /
                             (w * w) * viewport_.height * .5)};
        const auto length = std::hypot(derivative.x, derivative.y);
        // A view-parallel axis has no stable screen-space drag direction.
        if (finite(derivative) && std::isfinite(length) && length > .05F) {
            projected_axis.direction = {derivative.x / length, derivative.y / length};
            projected_axis.pixels_per_unit = length;
            projected_axis.perspective_ratio = static_cast<f32>(axis.w / w);
            projected_axis.visible = true;
        }
    }
    visible_ = true;
}
void TranslationTool::move(vng::Vec2 pointer) {
    if (!finite(pointer))
        return;
    pointer_=pointer;
    const auto pixels = static_cast<f64>(dot(sub(pointer, start_), drag_axis_.direction));
    // Invert the perspective divide along the selected world axis, rather than
    // treating a screen-space derivative as constant over a long drag. Clamp
    // at its vanishing point instead of crossing through infinity.
    const auto denominator = std::max(static_cast<f64>(drag_axis_.pixels_per_unit) * 1e-5,
                                      static_cast<f64>(drag_axis_.pixels_per_unit) -
                                          pixels * drag_axis_.perspective_ratio);
    auto movement = pixels / denominator;
    if (!std::isfinite(movement))
        return;
    // Clip distance along the line, not the individual coordinates: independent
    // XYZ clamping would bend a rotated ship's forward/back drag at the bounds.
    auto low = -std::numeric_limits<f64>::infinity(), high = -low;
    for (std::size_t i = 0; i < 3; ++i) {
        const auto direction = static_cast<f64>(drag_axis_.world[i]);
        if (std::abs(direction) < 1e-12) continue;
        const auto a = (-double(scene_coordinate_limit) - segment_origin_[i] - keyboard_delta_[i]) / direction;
        const auto b = (double(scene_coordinate_limit) - segment_origin_[i] - keyboard_delta_[i]) / direction;
        low = std::max(low, std::min(a, b));
        high = std::min(high, std::max(a, b));
    }
    if (low > high) return;
    movement = std::clamp(movement, low, high);
    for (std::size_t i = 0; i < 3; ++i)
        ghost_[i] = static_cast<f32>(segment_origin_[i] + movement * drag_axis_.world[i])+keyboard_delta_[i];
}
std::optional<vng::editor::Event>
TranslationTool::update(const vng::editor::Schema& schema, const vng::gfx::CameraSnapshot& camera,
                        vng::ui::Rect viewport, std::span<const vng::input::Event> unhandled,
                        std::span<const vng::input::Event> raw, bool enabled, bool world_axes, float arrow_step) {
    using namespace vng;
    handled_ = false;
    const auto control = std::ranges::find_if(
        schema.controls, [](const auto& c) { return c.kind == editor::Kind::translation_gizmo; });
    const editor::Field* position{};
    if (control != schema.controls.end()) {
        const auto field = std::ranges::find_if(control->fields, [](const auto& f) {
            return f.key == "position" && std::holds_alternative<Vec3>(f.value);
        });
        if (field != control->fields.end())
            position = &*field;
    }
    if (!enabled || !position || (!world_axes && control->translation_axes.empty()) || !finite(Vec2{viewport.x, viewport.y}) ||
        !finite(Vec2{viewport.width, viewport.height}) || viewport.width <= 0 ||
        viewport.height <= 0 || !finite(std::get<Vec3>(position->value))) {
        handled_ = dragging_;
        cancel();
        visible_ = false;
        return {};
    }
    const auto value = std::get<Vec3>(position->value);
    if (dragging_ &&
        (stamp_ != schema.stamp || key_ != control->key || origin_ != value ||
         extra_axes_ != control->translation_axes || world_axes_ != world_axes ||
         !same_rect(viewport_, viewport))) {
        handled_ = true;
        cancel();
    }
    stamp_ = schema.stamp;
    key_ = control->key;
    extra_axes_ = control->translation_axes;
    world_axes_ = world_axes;
    const bool reframe=dragging_ && camera_.view_projection!=camera.view_projection;
    camera_ = camera;
    viewport_ = viewport;
    if (!dragging_)
        segment_origin_ = origin_ = ghost_ = value;
    geometry();
    if(reframe) {
        segment_origin_=ghost_;start_=pointer_;keyboard_delta_={};
        if(!keyboard_) {
            const auto found=std::ranges::find_if(axes_,[&](const auto& a){return a.label==drag_axis_.label&&a.reverse==drag_axis_.reverse;});
            if(found!=axes_.end()&&found->visible)drag_axis_=*found;
            else {
                drag_axis_.direction={0,-1};drag_axis_.perspective_ratio=0;
                const auto p=project(ghost_,camera_,viewport_);
                drag_axis_.pixels_per_unit=p?std::max(.0001F,viewport_.height*camera_.projection[1][1]/(2*p->clip.w)):1.F;
            }
        }
    }

    // Begin only from unhandled viewport input. Once captured, use the original
    // ordered event stream so release over an inspector still ends the gesture.
    const auto begins = [&](const input::Event& event) {
        return std::ranges::any_of(unhandled, [&](const auto& available) {
            return available.kind == event.kind && available.button == event.button &&
                   available.position == event.position;
        });
    };
    const auto events = raw.empty() ? unhandled : raw;
    bool moved{};
    for (std::size_t index = 0; index < events.size(); ++index) {
        const auto& event = events[index];
        if(auto arrow=transform_arrow(event,arrow_step);arrow && (dragging_ || (visible_ && viewport_.contains(event.position) &&
            std::ranges::any_of(unhandled,[&](const auto& e){return e.kind==event.kind&&e.key==event.key;})))) {
            if(!dragging_) {keyboard_=dragging_=true;keyboard_delta_={};pointer_=event.position;}
            const auto p=project(origin_,camera_,viewport_);
            const auto units=p?10*p->clip.w/(viewport_.height*camera_.projection[1][1]):0;
            Vec3 delta{};
            for(unsigned c=0;c<3;++c)
                delta[c]=(keyboard_&&world_axes_?camera_.right[c]*arrow->x-camera_.up[c]*arrow->y:
                    (keyboard_?extra_axes_.front().direction[c]:drag_axis_.world[c])*(arrow->x-arrow->y))*units;
            if(!finite(delta))continue;
            f64 fraction=1;
            for(unsigned c=0;c<3;++c)if(std::abs(delta[c])>1e-12F) {
                const auto boundary=delta[c]>0?double(scene_coordinate_limit):-double(scene_coordinate_limit);
                fraction=std::min(fraction,std::max(0.,(boundary-ghost_[c])/delta[c]));
            }
            for(unsigned c=0;c<3;++c) {
                const auto step=static_cast<f32>(delta[c]*fraction);
                keyboard_delta_[c]+=step;ghost_[c]+=step;
            }
            handled_=moved=true;geometry();continue;
        }
        if (dragging_) {
            if(keyboard_ && ((event.kind==input::EventKind::key_down&&event.key==input::Key::enter) ||
                (event.kind==input::EventKind::pointer_down&&event.button==0))) {
                handled_=true;dragging_=keyboard_=false;
                return editor::Event{stamp_,key_,editor::Phase::apply,{{"position",ghost_}}};
            }
            if (event.kind == input::EventKind::focus_lost ||
                (event.kind == input::EventKind::key_down && event.key == input::Key::escape) ||
                (event.kind==input::EventKind::pointer_down && event.button==1)) {
                handled_ = true;
                cancel();
                geometry();
                return {};
            }
            if (!keyboard_ && event.kind == input::EventKind::pointer_move) {
                handled_ = true;
                // Pointer queues may contain many samples per UI frame. Each
                // absolute position is relative to the same gesture origin;
                // only its newest contiguous valid sample affects this frame.
                if (index + 1 < events.size() &&
                    events[index + 1].kind == input::EventKind::pointer_move &&
                    finite(events[index + 1].position))
                    continue;
                move(event.position);
                moved = true;
            }
            if (!keyboard_ && event.kind == input::EventKind::pointer_up && event.button == 0) {
                handled_ = true;
                move(event.position);
                geometry();
                dragging_ = false;
                return editor::Event{stamp_, key_, editor::Phase::apply, {{"position", ghost_}}};
            }
            continue;
        }
        if (event.kind != input::EventKind::pointer_down || event.button != 0 || !visible_ ||
            !viewport_.contains(event.position) || !begins(event))
            continue;
        f32 best = 7 * 7;
        std::optional<u32> hit;
        const auto from_origin = sub(event.position, screen_origin_);
        for (u32 i = 0; i < axes_.size(); ++i) {
            const auto& axis = axes_[i];
            if (!axis.visible)
                continue;
            // Extra handles begin outside XYZ so even coincident axes can be
            // picked individually, without stealing presses on the XYZ shafts.
            const auto distance = std::clamp(dot(from_origin, axis.direction),
                                              axis.custom ? 57.0F : 9.0F, axis.length);
            const Vec2 closest{axis.direction.x * distance, axis.direction.y * distance};
            const auto difference = sub(from_origin, closest);
            const auto squared = dot(difference, difference);
            if (squared < best) {
                best = squared;
                hit = i;
            }
        }
        if (hit) {
            axis_ = *hit;
            drag_axis_ = axes_[axis_];
            pointer_=start_ = event.position;
            keyboard_=false;keyboard_delta_={};
            dragging_ = handled_ = true;
        }
    }
    if (moved)
        geometry();
    return {};
}
std::optional<vng::Vec2> TranslationTool::handle(std::string_view label, bool reverse) const {
    if (!visible_) return {};
    for (const auto& axis : axes_)
        if (axis.visible && axis.label == label && axis.reverse == reverse)
            return vng::Vec2{screen_origin_.x + axis.direction.x * axis.length,
                             screen_origin_.y + axis.direction.y * axis.length};
    return {};
}

void TranslationTool::append(vng::ui::DrawList& list, const vng::text::Font& font, int only_axis) const {
    using namespace vng;
    if (!visible_)
        return;
    const std::array<Vec4, 3> colors{Vec4{1, .07F, .04F, 1}, Vec4{.08F, 1, .12F, 1},
                                     Vec4{.1F, .4F, 1, 1}};
    const auto dot = [&](Vec2 point, f32 radius, Vec4 color, f32 border = 0) {
        list.commands.emplace_back(
            ui::BoxDraw{{point.x - radius, point.y - radius, radius * 2, radius * 2},
                        viewport_,
                        color,
                        {0, 0, 0, .9F},
                        radius,
                        border});
    };
    // Small overlapping round boxes form anti-aliased lines without introducing
    // backend primitives or a second engine renderer into the example adapter.
    for (u32 i = 0; i < axes_.size(); ++i) {
        if (only_axis >= 0 && i != static_cast<u32>(only_axis)) continue;
        if (!axes_[i].visible) {
            // A keyboard constraint can still move along a view-parallel axis.
            // Its head-on handle is a colored dot, not an invented screen axis.
            if(only_axis>=0)dot(screen_origin_,6,colors[i],1);
            continue;
        }
        const auto& axis = axes_[i];
        const auto direction = axis.direction;
        const auto color = dragging_ && axis_ == i ? Vec4{1, .7F, .05F, 1}
            : axis.custom ? Vec4{.1F, .9F, 1, 1} : colors[i];
        for (f32 distance = axis.custom ? 53.F : 7.F; distance < axis.length; distance += 1.5F)
            dot({screen_origin_.x + direction.x * distance,
                 screen_origin_.y + direction.y * distance},
                2, color);
        const Vec2 tip{screen_origin_.x + direction.x * axis.length,
                       screen_origin_.y + direction.y * axis.length};
        if (axis.custom) {
            // Broad arrowhead, distinct from the world-axis round handles.
            const Vec2 back{tip.x - direction.x * 11, tip.y - direction.y * 11};
            list.commands.emplace_back(ui::TriangleDraw{{tip,
                {back.x - direction.y * 6, back.y + direction.x * 6},
                {back.x + direction.y * 6, back.y - direction.x * 6}}, viewport_, color});
            if (font && !axis.reverse) {
                const Vec2 label{std::clamp(tip.x + 9, viewport_.x, viewport_.x + std::max(0.F, viewport_.width - 150)),
                                  std::clamp(tip.y - 8, viewport_.y, viewport_.y + std::max(0.F, viewport_.height - 22))};
                list.commands.emplace_back(ui::BoxDraw{{label.x - 3, label.y - 1, 144, 22},
                    viewport_, {.02F,.03F,.05F,.85F}, {}, 3, 0});
                list.commands.emplace_back(ui::TextDraw{axis.label, label, viewport_, font, 14, color});
            }
        } else dot(tip, 5, color, 1);
    }
    dot(screen_origin_, 4, {1, 1, 1, 1}, 1);
}
} // namespace editor_example
