#include "editing_session.hpp"
#include <algorithm>
#include <cmath>

namespace editor_example {
namespace {
auto invalid(std::string message) {
    vng::content::Diagnostic error;
    error.message = std::move(message);
    return std::unexpected(std::move(error));
}
}
vng::content::Result<vng::u32> EditingSession::add_region(RegionShape shape,vng::Vec3 center,vng::f32 radius) {
    if(auto ready=available();!ready)return std::unexpected(ready.error());
    if(!std::isfinite(radius)||radius<=0)return invalid("Region radius must be a positive finite number");
    auto candidate=state_;
    auto id=editor_example::instantiate(candidate,BlueprintId::region);
    if(!id)return std::unexpected(id.error());
    find_instance(candidate,*id)->transform.position=center;
    region_settings(candidate,*id)->boundary=make_region(shape,{},radius);
    if(auto valid=validate(region_snapshot(candidate));!valid)return std::unexpected(valid.error());
    if(auto changed=replace(std::move(candidate),{.full=true});!changed)return std::unexpected(changed.error());
    return *id;
}
vng::content::Result<bool> EditingSession::erase_region(vng::u32 id) {
    if(auto ready=available();!ready)return std::unexpected(ready.error());
    auto candidate=state_;
    if(!region_settings(candidate,id))return false;
    auto erased=erase_instance(candidate,id);if(!erased)return std::unexpected(erased.error());
    return replace(std::move(candidate),{.full=true});
}
vng::content::Result<void> EditingSession::begin_region(vng::u32 id) {
    if(!region_settings(state_,id))return invalid("Unknown region");
    DocumentChanges changes;
    changes.regions[id].whole = true;
    return begin(EditGesture::region, std::move(changes), {id});
}
vng::content::Result<bool> EditingSession::region(const Region& value) {
    if(!active(EditGesture::region)||active_object()!=value.id)return invalid("Begin editing this region first");
    if(!gesture_->before.scope.regions.at(value.id).whole)
        return invalid("A point gesture cannot replace a region boundary");
    if(auto ready=writable();!ready)return std::unexpected(ready.error());
    if(auto valid=validate(value);!valid)return std::unexpected(valid.error());
    auto before=capture(gesture_->before.scope);if(!before)return std::unexpected(before.error());
    // Allocate the replacement before touching the instance. No other boundary
    // is copied, traversed or included in the gesture checkpoint.
    auto replacement = value;
    auto* instance = find_instance(state_, value.id);
    auto& settings = std::get<RegionSettings>(instance->settings);
    settings.boundary = std::move(static_cast<RegionGeometry&>(replacement));
    settings.note = std::move(replacement.note);
    settings.show_walls = replacement.show_walls;
    instance->name = std::move(replacement.name);
    return updated(*before);
}
vng::content::Result<void> EditingSession::begin_region_points(vng::u32 id, std::span<const vng::u32> points) {
    const auto* settings = region_settings(state_, id);
    if (!settings) return invalid("Unknown region");
    if (points.empty()) return invalid("A point gesture needs at least one vertex");
    DocumentChanges changes;
    auto& selected = changes.regions[id].points;
    for (auto point : points) {
        if (point >= settings->boundary.points.size()) return invalid("Unknown region vertex");
        selected.insert(point);
    }
    return begin(EditGesture::region, std::move(changes), {id});
}
vng::content::Result<bool> EditingSession::region_points(std::span<const RegionPointEdit> points) {
    if (!active(EditGesture::region)) return invalid("Begin a region point gesture first");
    const auto id = active_object();
    const auto& scope = gesture_->before.scope.regions.at(id);
    if (scope.whole) return invalid("Begin a region point gesture first");
    if (auto ready = writable(); !ready) return std::unexpected(ready.error());
    auto* settings = region_settings(state_, id);
    const auto& original = std::get<DocumentPatch>(gesture_->before.value).regions.front();
    if (!settings || settings->boundary.points.size() != original.point_count)
        return invalid("Region topology changed during point editing");
    std::set<vng::u32> seen;
    for (const auto& point : points) {
        if (!scope.points.contains(point.index) || !seen.insert(point.index).second ||
            !valid_scene_position(point.position))
            return invalid("Invalid, duplicate or unselected region vertex");
    }
    if (seen != scope.points) return invalid("Supply every selected region vertex");
    auto before = capture(gesture_->before.scope);
    if (!before) return std::unexpected(before.error());
    for (const auto& point : points) settings->boundary.points[point.index] = point.position;
    return updated(*before);
}
} // namespace editor_example
