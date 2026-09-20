#include "selection.hpp"
#include "animation.hpp"
#include "position_edits.hpp"
#include "blueprint_gizmos.hpp"
#include "camera_glyph.hpp"

#include <algorithm>
#include <cmath>
#include <concepts>
#include <limits>
#include <type_traits>
#include <vector>

namespace editor_example {
namespace {
using namespace vng;
struct Vector {
    double x{}, y{}, z{};
    explicit Vector(Vec3 v) : x(v.x), y(v.y), z(v.z) {}
    Vector(double x, double y, double z) : x(x), y(y), z(z) {}
    Vector operator+(Vector v) const { return {x + v.x, y + v.y, z + v.z}; }
    Vector operator-(Vector v) const { return {x - v.x, y - v.y, z - v.z}; }
    Vector operator*(double v) const { return {x * v, y * v, z * v}; }
};
double dot(Vector a, Vector b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
struct Ray {
    Vector origin, direction;
    double near{}, far{};
    bool contains(double depth) const {
        return std::isfinite(depth) && depth >= near && depth <= far;
    }
};
std::optional<spatial::Ray3> local_ray(const Ray& ray,const Mat4& model,double maximum) {
    // Invert the actual render matrix, including float rounding, rather than
    // regenerating Euler rotations differently from rendering.
    std::array<std::array<double,3>,3> inverse{};
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)
        inverse[r][c]=double(model[(r+1)%3][(c+1)%3])*model[(r+2)%3][(c+2)%3]
                     -double(model[(r+2)%3][(c+1)%3])*model[(r+1)%3][(c+2)%3];
    double determinant{};for(unsigned c=0;c<3;++c)determinant+=double(model[c][0])*inverse[c][0];
    if(!std::isfinite(determinant)||determinant==0)return {};
    const std::array origin{ray.origin.x-model[3][0],ray.origin.y-model[3][1],ray.origin.z-model[3][2]};
    const std::array direction{ray.direction.x,ray.direction.y,ray.direction.z};
    spatial::Ray3 result{{},{},ray.near,std::min(ray.far,maximum)};
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c) {
        result.origin[r]+=inverse[r][c]/determinant*origin[c];
        result.direction[r]+=inverse[r][c]/determinant*direction[c];
    }
    return result;
}
std::optional<double> sphere(const Ray& ray, Vec3 center, double radius) {
    if (!std::isfinite(radius) || radius <= 0)
        return {};
    const auto offset = ray.origin - Vector{center};
    const auto a = dot(ray.direction, ray.direction), b = dot(offset, ray.direction);
    const auto c = dot(offset, offset) - radius * radius;
    const auto discriminant = b * b - a * c;
    if (!std::isfinite(discriminant) || discriminant < 0)
        return {};
    // The sun body culls back faces. An exit surface alone (e.g. when the
    // camera is inside the body) must not become a selectable opaque surface.
    const auto depth = (-b - std::sqrt(discriminant)) / a;
    return ray.contains(depth) ? std::optional{depth} : std::nullopt;
}
} // namespace

std::optional<vng::u32> pick_object(const State& state, vng::Vec2 pixel, vng::Extent2D extent, const gfx::Camera* presented,PickStats* statistics) {
    if(statistics)*statistics={};
    // Blueprint view edits an asset, not one of its scene instances.
    if (state.viewport.mode == ViewMode::mesh) return {};
    if (!std::isfinite(pixel.x) || !std::isfinite(pixel.y) || pixel.x < 0 || pixel.x > 1 ||
        pixel.y < 0 || pixel.y > 1)
        return {};
    const auto view = presented ? *presented : camera(state);
    const auto snapshot = view.snapshot(extent);
    if (!snapshot)
        return {};
    const auto x = (static_cast<double>(pixel.x) * 2 - 1) / snapshot->projection[0][0];
    const auto y = (1 - static_cast<double>(pixel.y) * 2) / snapshot->projection[1][1];
    const auto offset = Vector{snapshot->right} * x + Vector{snapshot->up} * y;
    Ray ray{Vector{snapshot->position}, Vector{snapshot->forward}};
    std::visit(
        [&](const auto& lens) {
            ray.near = lens.near_plane;
            ray.far = lens.far_plane;
            if constexpr (std::same_as<std::remove_cvref_t<decltype(lens)>, gfx::PerspectiveLens>)
                ray.direction = ray.direction + offset;
            else
                ray.origin = ray.origin + offset;
        },
        view.lens());
    // The direction's forward component is one, so intersection parameters
    // are view-space depths rather than Euclidean distances. This matches the
    // render camera's clipping planes even near the viewport's corners.
    double nearest = std::numeric_limits<double>::infinity();
    std::optional<u32> selected;
    for (const auto& source : state.document.instances) {
        if(state.viewport.hides_surface(source.id))continue;
        if (!std::holds_alternative<SunSettings>(source.settings) || !instance_in_view(state, source)) continue;
        const auto instance = evaluate_instance(state, source, state.viewport.time);
        const auto* sun = std::get_if<SunSettings>(&instance.settings);
        if (!sun->visible) continue;
        // sun_shaders.cpp: |height| <= .5*.013 + .5*.004 + .001.
        const auto radius = sun->radius * (1 + .0095 * sun->displacement);
        const auto placement=state.viewport.mode==ViewMode::sun ? InstanceTransform{} : instance.transform;
        const auto local=local_ray(ray,mesh_transform(ViewMode::scene,placement),nearest);
        if(!local)continue;
        const Ray local_view{{local->origin[0],local->origin[1],local->origin[2]},
            {local->direction[0],local->direction[1],local->direction[2]},ray.near,ray.far};
        if (const auto hit = sphere(local_view,{},radius);
            hit && *hit < nearest) {
            nearest = *hit;
            selected = instance.id;
        }
    }
    for (const auto& source : state.document.instances) {
        if(state.viewport.hides_surface(source.id))continue;
        if (!std::holds_alternative<CameraSettings>(source.settings) || !instance_in_view(state, source) ||
            !evaluate_visibility(state, source, state.viewport.time)) continue;
        // A camera has no surface; its drawn body is the click target.
        const auto glyph = camera_glyph(evaluate_instance(state, source, state.viewport.time));
        if (const auto hit = sphere(ray, glyph.body_center(), glyph.pick_radius()); hit && *hit < nearest) {
            nearest = *hit;
            selected = source.id;
        }
    }
    for (const auto& instance : state.document.instances) {
        if(state.viewport.hides_surface(instance.id))continue;
        const auto* settings = std::get_if<MeshSettings>(&instance.settings);
        if (!settings || !instance_in_view(state, instance) || !evaluate_visibility(state, instance, state.viewport.time)) continue;
        const auto* geometry = mesh_geometry(state, instance.blueprint);
        if (!geometry) continue;
        const auto model = mesh_transform(state,instance.blueprint,evaluate_transform(state, instance, state.viewport.time));
        if(statistics)++statistics->mesh_instances;
        const auto local=local_ray(ray,model,nearest);if(!local)continue;
        const auto hit=geometry->picking_index().intersect(*local,statistics?&statistics->geometry:nullptr);
        // Sun is drawn first; preserve strict-less depth and scene-order ties.
        if(hit&&hit->distance<nearest) {
            nearest=hit->distance;
            selected=instance.id;
        }
    }
    return selected;
}

vng::editor::Schema local_position_gizmo(const State& state, vng::u64 generation) {
    using namespace vng;
    editor::Schema schema{{state.viewport.selected_object, generation, state.document.revision}, {}};
    const auto* instance = find_instance(state, state.viewport.selected_object);
    if (!instance || state.viewport.mode != ViewMode::scene) return schema;
    const auto evaluated = evaluate_instance(state, *instance, state.viewport.time);
    if (!std::visit([](const auto& value) { return value.visible; }, evaluated.settings))
        return schema;
    const auto position = evaluated.transform.position;
    schema.controls.push_back({"position", "Position", editor::Kind::translation_gizmo,
                               {{"position", "Position", position, {}, {}}}, true, {},
                               blueprint_translation_axes(state, evaluated)});
    return schema;
}

namespace {
std::vector<InstancePoint> project_instances(const State& state,const vng::gfx::CameraSnapshot& snapshot) {
    std::vector<InstancePoint> points;
    if(state.viewport.mode==ViewMode::mesh) return points;
    points.reserve(state.document.instances.size());
    const auto& m=snapshot.view_projection;
    for(const auto& source:state.document.instances) {
        // Marker projection needs neither names/materials nor per-instance
        // region boundaries. Do not clone those just to obtain an origin.
        if(!instance_in_view(state,source) || !evaluate_visibility(state,source,state.viewport.time)) continue;
        const auto p=evaluate_transform(state,source,state.viewport.time).position;
        vng::Vec4 clip{};
        for(std::size_t r=0;r<4;++r) clip[r]=m[0][r]*p.x+m[1][r]*p.y+m[2][r]*p.z+m[3][r];
        if(clip.w<=1e-6F || std::abs(clip.z)>clip.w || std::abs(clip.x)>clip.w || std::abs(clip.y)>clip.w) continue;
        points.push_back({source.id,{(clip.x/clip.w+1)*.5F,(1-clip.y/clip.w)*.5F}});
    }
    return points;
}
}
std::vector<InstancePoint> instance_points(const State& state,vng::Extent2D extent,const vng::gfx::Camera& camera) {
    const auto snapshot=camera.snapshot(extent);
    return snapshot ? project_instances(state,*snapshot) : std::vector<InstancePoint>{};
}
std::span<const InstancePoint> InstanceProjection::get(const State& state,vng::Extent2D extent,const vng::gfx::Camera& camera) {
    const auto snapshot=camera.snapshot(extent);
    if(!snapshot) { key_.reset(); points_.clear(); return points_; }
    const Key key{&state,state.document.revision,state.viewport.time,state.viewport.mode,
        state.viewport.mode==ViewMode::sun ? state.viewport.selected_object : 0,extent,snapshot->view_projection};
    if(key_!=key) {
        points_=project_instances(state,*snapshot);
        key_=key;
        ++rebuilds_;
    }
    return points_;
}

vng::editor::Schema selection_gizmo_schema(const State& state,
                                           const vng::editor::Schema& source) {
    auto result = source;
    const auto* selected = find_instance(state, state.viewport.selected_object);
    const auto values = selected ? std::optional{evaluate_instance(state, *selected, state.viewport.time)}
                                 : std::nullopt;
    const bool visible = values && std::visit([](const auto& value) { return value.visible; }, values->settings);
    if (!selected || !visible ||
        source.stamp.object != state.viewport.selected_object || source.stamp.revision != state.document.revision) {
        std::erase_if(result.controls, [](const auto& control) {
            return control.kind == vng::editor::Kind::translation_gizmo;
        });
        return result;
    }
    for (auto& control : result.controls) {
        if (control.kind != vng::editor::Kind::translation_gizmo || control.key != "position")
            continue;
        control.translation_axes = blueprint_translation_axes(state, *values);
        for (auto& field : control.fields)
            if (field.key == "position" && std::holds_alternative<vng::Vec3>(field.value))
                field.value = values->transform.position;
    }
    return result;
}

vng::content::Result<bool> apply_animated_translation(State& state,
                                                      const vng::editor::Event& event) {
    using namespace vng;
    const auto* selected = find_instance(state, state.viewport.selected_object);
    if (event.control != "position" || !selected)
        return false;
    const timeline::Target target{state.viewport.selected_object, "position"};
    const auto* track = state.document.timeline.find(target);
    if (!track)
        return false;
    const auto invalid = [](std::string message) {
        content::Diagnostic error;
        error.code = content::ErrorCode::invalid_document;
        error.message = std::move(message);
        return std::unexpected(std::move(error));
    };
    if (event.stamp.object != state.viewport.selected_object || event.stamp.revision != state.document.revision)
        return invalid("Stale selection or scene revision for animated translation");
    if (!state.viewport.paused || state.viewport.mode != ViewMode::scene)
        return invalid("Pause scene playback before translating an animated object");
    if (event.phase != editor::Phase::apply || event.values.size() != 1 ||
        event.values.front().key != "position" ||
        !std::holds_alternative<Vec3>(event.values.front().value))
        return invalid("Animated translation requires one applied position value");
    const auto values = evaluate_instance(state, *selected, state.viewport.time);
    if (!std::visit([](const auto& value) { return value.visible; }, values.settings))
        return invalid("Cannot translate a hidden animated object");
    const auto position = std::get<Vec3>(event.values.front().value);
    const auto current = values.transform.position;
    if (position == current)
        return true; // A click without movement must not silently create a key.
    if (auto applied = apply_position_value(state, state.viewport.selected_object, position);
        !applied)
        return std::unexpected(applied.error());
    return true;
}
} // namespace editor_example
