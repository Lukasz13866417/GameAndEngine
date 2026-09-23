#include "project.hpp"
#include "scale_limits.hpp"
#include "authoring_limits.hpp"
#include "animation.hpp"
#include "preview_values.hpp"
#include "effect_presets.hpp"
#include "legacy_camera.hpp"
#include "../support/mesh_frame.hpp"
#include <vng/editor/limits.hpp>
#include <vng/content/document.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cctype>
#include <numbers>

namespace editor_example {
using namespace vng;
namespace {
constexpr std::size_t max_scene_bytes = editor::max_document_bytes;
constexpr u64 max_revision = (u64{1} << 53) - 1;
std::string quote_string(std::string_view s) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 32) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else
            out += static_cast<char>(c);
    }
    return out + '"';
}
void vector(std::ostream& o, Vec3 p) {
    o << '[' << p.x << ',' << p.y << ',' << p.z << ']';
}
auto invalid(std::string message) {
    content::Diagnostic diagnostic;
    diagnostic.code = content::ErrorCode::invalid_document;
    diagnostic.message = std::move(message);
    return std::unexpected(std::move(diagnostic));
}
bool range(f32 value, f32 low, f32 high) {
    return std::isfinite(value) && value >= low && value <= high;
}
bool valid_settings(const MeshSettings& model) {
    return range(model.brightness, 0, 5);
}
bool valid_settings(const RegionSettings& value) {
    Region region;region.id=1;region.note=value.note;static_cast<RegionGeometry&>(region)=value.boundary;
    return bool(validate(region));
}
void write_settings(std::ostream& out,const RegionSettings& value) {
    out<<"{ visible = "<<value.visible<<"; show_walls = "<<value.show_walls<<"; note = "<<quote_string(value.note)<<"; boundary = ";
    write_region_geometry(out,value.boundary);out<<"; }";
}
RegionSettings read_region(content::Reader reader) {
    return {read_region_geometry(reader.child("boundary")),reader.get<std::string>("note"),reader.get_or<bool>("visible",true),reader.get_or<bool>("show_walls",false)};
}
bool valid_settings(const SunSettings& sun) {
    return valid_effect_settings(sun);
}
bool valid_settings(const CameraSettings& lens) {
    return range(lens.zoom, camera_min_zoom, camera_max_zoom) && range(lens.focus, camera_min_distance, camera_max_distance);
}
void write_settings(std::ostream& out, const CameraSettings& lens) {
    out << "{ zoom = " << lens.zoom << "; focus = " << lens.focus << "; active = " << lens.active
        << "; visible = " << lens.visible << "; }";
}
CameraSettings read_camera_settings(content::Reader reader) {
    return {reader.get<f32>("zoom"), reader.get<f32>("focus"), reader.get_or<bool>("active", false),
            reader.get_or<bool>("visible", true)};
}
bool valid_transform(const InstanceTransform& transform) {
    return valid_scene_position(transform.position) && range(transform.scale, min_instance_scale, max_instance_scale) &&
           range(transform.axis_scale.x, min_axis_scale, max_axis_scale) && range(transform.axis_scale.y, min_axis_scale, max_axis_scale) &&
           range(transform.axis_scale.z, min_axis_scale, max_axis_scale) &&
           range(transform.rotation.x, -360, 360) && range(transform.rotation.y, -360, 360) &&
           range(transform.rotation.z, -360, 360);
}
void write_settings(std::ostream& o, const MeshSettings& model) {
    o << "{ brightness = " << model.brightness
      << "; visible = " << model.visible << "; wireframe = " << model.wireframe << "; }";
}
void write_settings(std::ostream& o, const SunSettings& sun) {
    write_effect_settings(o,sun);
}
MeshSettings read_mesh(content::Reader m) {
    return {m.get<f32>("brightness"),
            m.get<bool>("visible"), m.get<bool>("wireframe")};
}
SunSettings read_sun(content::Reader p) {
    return read_effect_settings(p);
}
void write_transform(std::ostream& out, const InstanceTransform& transform) {
    out << "{ position = ";
    vector(out, transform.position);
    out << "; rotation = ";
    vector(out, transform.rotation);
    out << "; scale = " << transform.scale << "; axis_scale = ";
    vector(out,transform.axis_scale);out << "; }";
}
InstanceTransform read_transform(content::Reader reader) {
    return {reader.get<Vec3>("position"), reader.get<Vec3>("rotation"), reader.get<f32>("scale"),
        reader.get_or<Vec3>("axis_scale",{1,1,1})};
}
InstanceTransform read_legacy_transform(content::Reader settings, bool mesh) {
    return {settings.get<Vec3>("position"), {}, mesh ? settings.get<f32>("scale") : 1};
}
void write_camera(std::ostream& out, const CameraPose& pose) {
    out << "yaw = " << pose.yaw << "; pitch = " << pose.pitch
        << "; distance = " << pose.distance << "; zoom = " << pose.zoom << "; camera_target = ";
    vector(out, pose.target);
    out << "; ";
}
CameraPose read_camera(content::Reader reader) {
    return {reader.get<f32>("yaw"), reader.get<f32>("pitch"),
            reader.get<f32>("distance"), reader.get_or<Vec3>("camera_target", {}), reader.get_or<f32>("zoom", 1)};
}
content::Result<void> validate_state(const State& s) {
    if (auto valid = validate(region_snapshot(s)); !valid) return valid;
    if (auto valid = validate_active_cameras(s); !valid) return valid;
    if (!valid_world_bounds(s.document.world_bounds)) return invalid("Invalid world bounds");
    const auto& environment = s.document.environment;
    if (environment.stars > 20000 || !range(environment.exposure, .01F, 10) ||
        !range(environment.bloom_threshold, 0, 100) || !range(environment.bloom_strength, 0, 1))
        return invalid("Scene environment exceeds editor limits");
    if (static_cast<u32>(s.viewport.mode) > static_cast<u32>(ViewMode::sun))
        return invalid("Unknown editor view mode");
    if (!is_mesh_blueprint(s, s.viewport.inspected_mesh))
        return invalid("Unknown inspected mesh blueprint");
    const auto* geometry = editable_mesh(s);
    if ((s.viewport.selected_object && !find_instance(s, s.viewport.selected_object)) ||
        (geometry ? s.viewport.selected_vertex >= geometry->size() : s.viewport.selected_vertex != 0) || !s.document.revision ||
        s.document.revision > max_revision || !s.viewport.sequence || s.viewport.sequence > max_revision)
        return invalid("Invalid selection or revision (revision must be in 1..2^53-1)");
    if (!valid_settings(s.document.mesh_blueprint) || !valid_settings(s.document.sun_blueprint) ||
        !range(s.viewport.time, 0, 86400) ||
        !valid_camera_pose(s.viewport.editor_camera))
        return invalid("Scene settings exceed editor limits");
    if (s.document.instances.size() > max_scene_instances || !s.document.next_instance_id)
        return invalid("Scene instance count/identity exceeds editor limits");
    if (s.document.mesh_assets.size()+s.document.effect_assets.size() > 254 || s.document.next_blueprint_id < 3)
        return invalid("Imported blueprint count/identity exceeds editor limits");
    for(const auto& [id,draft]:s.document.mesh_drafts)
        if(!is_mesh_blueprint(s,id) || !draft.size()) return invalid("Mesh draft references an unknown or empty blueprint");
    for(const auto& [id,placement]:s.document.mesh_placements)
        if(!is_mesh_blueprint(s,id) || !example::mesh_frame::inverse(placement.applied) ||
           (placement.draft && !example::mesh_frame::inverse(*placement.draft)))
            return invalid("Invalid mesh placement");
    std::vector<BlueprintId> blueprint_ids;
    for (const auto& asset : s.document.mesh_assets) {
        const auto id = static_cast<u32>(asset.id);
        if (id < 3 || id >= s.document.next_blueprint_id ||
            std::ranges::find(blueprint_ids, asset.id) != blueprint_ids.end() ||
            id >= first_reserved_blueprint || asset.name.empty() || asset.name.size() > 256 || !valid_settings(asset.settings))
            return invalid("Invalid imported mesh blueprint identity, name or settings");
        blueprint_ids.push_back(asset.id);
    }
    for(const auto& asset:s.document.effect_assets) {
        const auto id=static_cast<u32>(asset.id);
        if(id<3 || id>=s.document.next_blueprint_id || id>=first_reserved_blueprint ||
           std::ranges::find(blueprint_ids,asset.id)!=blueprint_ids.end() ||
           asset.name.empty() || asset.name.size()>256 || !valid_effect_settings(asset.settings))
            return invalid("Invalid imported effect blueprint identity, name or settings");
        blueprint_ids.push_back(asset.id);
    }
    std::vector<u32> ids;
    for (const auto& instance : s.document.instances) {
        if (!instance.id || instance.id >= s.document.next_instance_id ||
            std::ranges::find(ids, instance.id) != ids.end() || instance.name.empty() ||
            instance.name.size() > 256 ||
            (!is_mesh_blueprint(s, instance.blueprint) && !effect_blueprint_settings(s,instance.blueprint) &&
             instance.blueprint != BlueprintId::region && instance.blueprint != BlueprintId::camera) ||
            (is_mesh_blueprint(s, instance.blueprint) !=
                std::holds_alternative<MeshSettings>(instance.settings)) ||
            (instance.blueprint==BlueprintId::region)!=std::holds_alternative<RegionSettings>(instance.settings) ||
            (instance.blueprint==BlueprintId::camera)!=std::holds_alternative<CameraSettings>(instance.settings) ||
            bool(effect_blueprint_settings(s,instance.blueprint))!=std::holds_alternative<SunSettings>(instance.settings))
            return invalid("Invalid scene instance identity or blueprint");
        if (!std::visit([](const auto& value) { return valid_settings(value); }, instance.settings))
            return invalid("Scene instance settings exceed editor limits");
        if (!valid_transform(instance.transform))
            return invalid("Scene instance transform exceeds editor limits");
        ids.push_back(instance.id);
    }
    return validate_animation(s);
}
content::vmesh::ReadOptions mesh_read_options() {
    return {.limits=editor::mesh_limits()};
}
Vec4 transform(const Mat4& m, Vec4 p) {
    Vec4 result{};
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t col = 0; col < 4; ++col)
            result[row] += m[col][row] * p[col];
    return result;
}
void animation_value(std::ostream& out, const timeline::Value& value) {
    std::visit(
        [&](const auto& entry) {
            using T = std::remove_cvref_t<decltype(entry)>;
            if constexpr (std::same_as<T, Vec3>)
                vector(out, entry);
            else if constexpr (std::same_as<T, std::string>)
                out << quote_string(entry);
            else
                out << entry;
        },
        value);
}
// A legacy shot collects the old camera tracks, which are no scene property.
void read_animation(State& state, content::Reader reader, LegacyCameraShot* legacy) {
    state.document.timeline_duration = reader.get<f32>("duration");
    auto properties = animation_properties(state);
    if (legacy) std::ranges::move(legacy_camera_properties(legacy->pose), std::back_inserter(properties));
    std::vector<timeline::Track> tracks;
    std::size_t total_keys{};
    for (const auto track : reader.child("tracks").elements()) {
        if (tracks.size() >= timeline::max_tracks)
            track.fail("Too many animation tracks", content::ErrorCode::limit_exceeded);
        timeline::Target target{track.get<u64>("object"), track.get<std::string>("property")};
        const auto property = std::ranges::find(properties, target, &AnimationProperty::target);
        if (property == properties.end())
            track.fail("Unknown animated scene property '" + target.property + "' on object " +
                       std::to_string(target.object));
        timeline::Track decoded{target,
                                track.get_or<std::string>("label", property->label),
                                track.get_or<std::string>("layer", property->layer),
                                {}};
        for (const auto key : track.child("keys").elements()) {
            if (decoded.keys.size() >= timeline::max_keys_per_track ||
                total_keys >= timeline::max_total_keys)
                key.fail("Too many animation keys", content::ErrorCode::limit_exceeded);
            const auto incoming = key.get<std::string>("incoming");
            if (incoming != "hold" && incoming != "linear")
                key.fail("Key incoming interpolation must be 'hold' or 'linear'");
            const auto value = std::visit(
                [&](const auto& base) -> timeline::Value {
                    return key.get<std::remove_cvref_t<decltype(base)>>("value");
                },
                property->base_value);
            decoded.keys.push_back({key.get<f32>("time"), value,
                                    incoming == "hold" ? timeline::Interpolation::hold
                                                       : timeline::Interpolation::linear});
            ++total_keys;
        }
        if (legacy && decoded.target.object == legacy_camera_object) legacy->tracks.push_back(std::move(decoded));
        else tracks.push_back(std::move(decoded));
    }
    if (auto replaced = state.document.timeline.replace(std::move(tracks)); !replaced)
        reader.fail(replaced.error().message);
    for (const auto member : reader.members()) {
        if (member.name != "keyframes") continue;
        for (const auto frame : member.value.elements()) {
            if (state.document.keyframe_names.size() >= timeline::max_total_keys)
                frame.fail("Too many named keyframes");
            const auto time = frame.get<f32>("time");
            if (!std::isfinite(time) || time < 0 || time > state.document.timeline_duration)
                frame.fail("Keyframe timestamp must lie within the timeline");
            if (!state.document.keyframe_names.emplace(time, frame.get<std::string>("name")).second)
                frame.fail("Duplicate named keyframe timestamp");
        }
    }
}
Mat4 transformed_mesh(ViewMode mode, const InstanceTransform& transform) {
    Mat4 m = Mat4::identity();
    if (mode == ViewMode::mesh) return m;
    const auto angles = transform.rotation;
    constexpr auto radians = std::numbers::pi_v<f32> / 180;
    const auto cx = std::cos(angles.x * radians), sx = std::sin(angles.x * radians);
    const auto cy = std::cos(angles.y * radians), sy = std::sin(angles.y * radians);
    const auto cz = std::cos(angles.z * radians), sz = std::sin(angles.z * radians);
    const auto s = transform.scale;
    // Column-major T * Rz * Ry * Rx * S; scale applies in mesh-local space.
    m[0] = {(cz * cy) * s * transform.axis_scale.x, (sz * cy) * s * transform.axis_scale.x, -sy * s * transform.axis_scale.x, 0};
    m[1] = {(cz * sy * sx - sz * cx) * s * transform.axis_scale.y, (sz * sy * sx + cz * cx) * s * transform.axis_scale.y, (cy * sx) * s * transform.axis_scale.y, 0};
    m[2] = {(cz * sy * cx + sz * sx) * s * transform.axis_scale.z, (sz * sy * cx - cz * sx) * s * transform.axis_scale.z, (cy * cx) * s * transform.axis_scale.z, 0};
    m[3] = {transform.position.x, transform.position.y, transform.position.z, 1};
    return m;
}
} // namespace
bool valid_camera_pose(const CameraPose& pose) {
    return range(pose.distance, camera_min_distance, camera_max_distance) &&
           range(pose.zoom, camera_min_zoom, camera_max_zoom) &&
           range(pose.pitch, -camera_max_pitch, camera_max_pitch) &&
           range(pose.yaw, -180, 180) &&
           range(pose.target.x, -camera_target_limit, camera_target_limit) &&
           range(pose.target.y, -camera_target_limit, camera_target_limit) &&
           range(pose.target.z, -camera_target_limit, camera_target_limit);
}
std::vector<Blueprint> blueprint_catalog(const State& state) {
    std::vector<Blueprint> result{{BlueprintId::mesh, "Mesh", BlueprintKind::mesh},
                                   {BlueprintId::sun, "Sun / effect", BlueprintKind::sun},
                                   {BlueprintId::region, "Region / TODO volume", BlueprintKind::region},
                                   {BlueprintId::camera, "Camera", BlueprintKind::camera}};
    for (const auto& asset : state.document.mesh_assets)
        result.push_back({asset.id, asset.name, BlueprintKind::mesh});
    for(const auto& asset:state.document.effect_assets)
        result.push_back({asset.id,asset.name,BlueprintKind::sun});
    return result;
}
const SunSettings* effect_blueprint_settings(const State& state,BlueprintId id) {
    if(id==BlueprintId::sun)return &state.document.sun_blueprint;
    const auto at=std::ranges::find(state.document.effect_assets,id,&EffectBlueprint::id);
    return at==state.document.effect_assets.end()?nullptr:&at->settings;
}
editor::EditableMesh* mesh_geometry(State& state, BlueprintId id) {
    if (id == BlueprintId::mesh) return &state.document.mesh;
    const auto found = std::ranges::find(state.document.mesh_assets, id, &MeshBlueprint::id);
    return found == state.document.mesh_assets.end() ? nullptr : &found->geometry;
}
const editor::EditableMesh* mesh_geometry(const State& state, BlueprintId id) {
    if (id == BlueprintId::mesh) return &state.document.mesh;
    const auto found = std::ranges::find(state.document.mesh_assets, id, &MeshBlueprint::id);
    return found == state.document.mesh_assets.end() ? nullptr : &found->geometry;
}
bool is_mesh_blueprint(const State& state, BlueprintId id) {
    return mesh_geometry(state, id) != nullptr;
}
editor::EditableMesh* mesh_edit_geometry(State& state, BlueprintId id) {
    const auto draft=state.document.mesh_drafts.find(id);
    return draft==state.document.mesh_drafts.end()?mesh_geometry(state,id):&draft->second;
}
const editor::EditableMesh* mesh_edit_geometry(const State& state, BlueprintId id) {
    const auto draft=state.document.mesh_drafts.find(id);
    return draft==state.document.mesh_drafts.end()?mesh_geometry(state,id):&draft->second;
}
const editor::EditableMesh* mesh_view_geometry(const State& state, BlueprintId id) {
    return state.viewport.mode==ViewMode::mesh && state.viewport.inspected_mesh==id
        ?mesh_edit_geometry(state,id):mesh_geometry(state,id);
}
content::Result<bool> begin_mesh_draft(State& state, BlueprintId id) {
    const auto* source=mesh_geometry(state,id);
    if(!source) return invalid("Cannot edit an unknown mesh blueprint");
    if(state.document.mesh_drafts.contains(id)) return false;
    state.document.mesh_drafts.emplace(id,*source);
    return true;
}
content::Result<bool> apply_mesh_draft(State& state, BlueprintId id) {
    auto* published=mesh_geometry(state,id);
    if(!published) return invalid("Cannot apply an unknown mesh blueprint");
    const auto draft=state.document.mesh_drafts.find(id);
    const bool changed=has_mesh_draft(state,id);
    if(draft!=state.document.mesh_drafts.end()) {
        *published=std::move(draft->second);
        state.document.mesh_drafts.erase(draft);
    }
    if(auto it=state.document.mesh_placements.find(id);it!=state.document.mesh_placements.end() && it->second.draft) {
        it->second.applied=*it->second.draft;
        it->second.draft.reset();
    }
    return changed;
}
bool discard_mesh_draft(State& state, BlueprintId id) {
    bool changed=state.document.mesh_drafts.erase(id)!=0;
    if(auto it=state.document.mesh_placements.find(id);it!=state.document.mesh_placements.end()) {
        changed |= it->second.draft.has_value(); it->second.draft.reset();
    }
    return changed;
}
bool has_mesh_draft(const State& state, BlueprintId id) {
    const auto it=state.document.mesh_placements.find(id);
    return state.document.mesh_drafts.contains(id) || (it!=state.document.mesh_placements.end() && it->second.draft.has_value());
}
Mat4 mesh_placement(const State& state, BlueprintId id, bool draft) {
    const auto it=state.document.mesh_placements.find(id);
    if(it==state.document.mesh_placements.end()) return Mat4::identity();
    return draft ? it->second.draft.value_or(it->second.applied) : it->second.applied;
}
content::Result<State> bake_mesh_placements(const State& source) {
    State baked=source;
    for(const auto& [id,placement]:source.document.mesh_placements) {
        const auto bake=[&](const editor::EditableMesh& mesh,const Mat4& matrix)->content::Result<editor::EditableMesh> {
            if(matrix==Mat4::identity())return mesh;
            auto document=example::mesh_frame::transformed(mesh.document(),matrix);
            if(!document)return std::unexpected(document.error());
            return editor::EditableMesh::create(std::move(*document));
        };
        if(has_mesh_draft(source,id)) {
            auto draft=bake(*mesh_edit_geometry(source,id),placement.draft.value_or(placement.applied));
            if(!draft)return std::unexpected(draft.error());
            baked.document.mesh_drafts.insert_or_assign(id,std::move(*draft));
        }
        auto* geometry=mesh_geometry(baked,id);
        if(!geometry)return invalid("Missing mesh placement blueprint");
        auto applied=bake(*geometry,placement.applied);
        if(!applied)return std::unexpected(applied.error());
        *geometry=std::move(*applied);
    }
    baked.document.mesh_placements.clear();
    return baked;
}
const MeshSettings* mesh_blueprint_settings(const State& state, BlueprintId id) {
    if (id == BlueprintId::mesh) return &state.document.mesh_blueprint;
    const auto found = std::ranges::find(state.document.mesh_assets, id, &MeshBlueprint::id);
    return found == state.document.mesh_assets.end() ? nullptr : &found->settings;
}
editor::EditableMesh* instance_mesh(State& state, u32 id) {
    const auto* instance = find_instance(state, id);
    return instance && is_mesh_instance(state, id) ? mesh_geometry(state, instance->blueprint) : nullptr;
}
const editor::EditableMesh* instance_mesh(const State& state, u32 id) {
    const auto* instance = find_instance(state, id);
    return instance && is_mesh_instance(state, id) ? mesh_geometry(state, instance->blueprint) : nullptr;
}
std::optional<MeshTarget> mesh_target(const State& state) {
    if (state.viewport.mode == ViewMode::mesh)
        return is_mesh_blueprint(state, state.viewport.inspected_mesh)
                   ? std::optional<MeshTarget>{{state.viewport.inspected_mesh, {}}} : std::nullopt;
    if (state.viewport.mode != ViewMode::scene) return {};
    const auto* instance = find_instance(state, state.viewport.selected_object);
    if (!instance || !is_mesh_instance(state, instance->id) ||
        !is_mesh_blueprint(state, instance->blueprint)) return {};
    return MeshTarget{instance->blueprint, instance->id};
}
editor::EditableMesh* editable_mesh(State& state) {
    const auto target = mesh_target(state);
    return target ? mesh_edit_geometry(state, target->blueprint) : nullptr;
}
const editor::EditableMesh* editable_mesh(const State& state) {
    const auto target = mesh_target(state);
    return target ? mesh_edit_geometry(state, target->blueprint) : nullptr;
}
content::Result<void> inspect_mesh(State& state, BlueprintId blueprint) {
    return inspect_mesh(state, state.viewport, blueprint);
}
content::Result<void> inspect_mesh(const State& state, ViewportState& view, BlueprintId blueprint) {
    const auto* mesh = mesh_edit_geometry(state, blueprint);
    if (!mesh || !mesh->size()) return invalid("Cannot inspect an unknown or empty mesh blueprint");
    Vec3 low = mesh->position(0), high = low;
    for (u32 i = 1; i < mesh->size(); ++i) {
        const auto p = mesh->position(i);
        low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    }
    const Vec3 center{(low.x + high.x) * .5F,
                      (low.y + high.y) * .5F,
                      (low.z + high.z) * .5F};
    const Vec3 span{high.x - low.x, high.y - low.y, high.z - low.z};
    const auto radius = std::sqrt(span.x * span.x + span.y * span.y + span.z * span.z) * .5F;
    const auto distance = 1.2F * radius /
        std::sin(camera_vertical_fov * .5F * std::numbers::pi_v<f32> / 180) / .52F;
    view.mode = ViewMode::mesh;
    view.inspected_mesh = blueprint;
    view.paused = true;
    view.selected_vertex = 0;
    view.editor_camera.target = {
        std::clamp(center.x, -camera_target_limit, camera_target_limit),
        std::clamp(center.y, -camera_target_limit, camera_target_limit),
        std::clamp(center.z, -camera_target_limit, camera_target_limit)};
    view.editor_camera.distance = std::clamp(distance, .5F, camera_max_distance);
    view.editor_camera.zoom = 1;
    return {};
}
SceneInstance* find_instance(State& state, u32 id) {
    const auto found = std::ranges::find(state.document.instances, id, &SceneInstance::id);
    return found == state.document.instances.end() ? nullptr : &*found;
}
const SceneInstance* find_instance(const State& state, u32 id) {
    const auto found = std::ranges::find(state.document.instances, id, &SceneInstance::id);
    return found == state.document.instances.end() ? nullptr : &*found;
}
InstanceTransform* instance_transform(State& state, u32 id) {
    auto* instance = find_instance(state, id);
    return instance ? &instance->transform : nullptr;
}
const InstanceTransform* instance_transform(const State& state, u32 id) {
    const auto* instance = find_instance(state, id);
    return instance ? &instance->transform : nullptr;
}
MeshSettings* mesh_settings(State& state, u32 id) {
    auto* instance = find_instance(state, id);
    return instance ? std::get_if<MeshSettings>(&instance->settings) : nullptr;
}
const MeshSettings* mesh_settings(const State& state, u32 id) {
    const auto* instance = find_instance(state, id);
    return instance ? std::get_if<MeshSettings>(&instance->settings) : nullptr;
}
SunSettings* sun_settings(State& state, u32 id) {
    auto* instance = find_instance(state, id);
    return instance ? std::get_if<SunSettings>(&instance->settings) : nullptr;
}
const SunSettings* sun_settings(const State& state, u32 id) {
    const auto* instance = find_instance(state, id);
    return instance ? std::get_if<SunSettings>(&instance->settings) : nullptr;
}
bool is_mesh_instance(const State& state, u32 id) {
    return mesh_settings(state, id) != nullptr;
}
const SceneInstance* view_instance(const State& state, BlueprintKind kind) {
    if (state.viewport.mode == ViewMode::mesh) return nullptr;
    const auto matches = [&](const SceneInstance& instance) {
        return kind == BlueprintKind::mesh
                   ? std::holds_alternative<MeshSettings>(instance.settings)
                   : kind == BlueprintKind::sun ? std::holds_alternative<SunSettings>(instance.settings)
                   : kind == BlueprintKind::camera ? std::holds_alternative<CameraSettings>(instance.settings)
                   : std::holds_alternative<RegionSettings>(instance.settings);
    };
    if (const auto* selected = find_instance(state, state.viewport.selected_object);
        selected && matches(*selected))
        return selected;
    if (kind == BlueprintKind::mesh) return nullptr;
    const auto first = std::ranges::find_if(state.document.instances, matches);
    return first == state.document.instances.end() ? nullptr : &*first;
}
bool instance_in_view(const State& state, const SceneInstance& instance) {
    if (state.viewport.mode == ViewMode::scene) return true;
    if (state.viewport.mode == ViewMode::mesh) return false;
    const auto* viewed = view_instance(state, BlueprintKind::sun);
    return viewed && viewed->id == instance.id;
}
std::string object_name(const State& state, u64 id) {
    const auto* instance = id <= std::numeric_limits<u32>::max()
                               ? find_instance(state, static_cast<u32>(id)) : nullptr;
    return instance ? instance->name : "No object";
}
content::Result<u32> instantiate(State& state, BlueprintId blueprint) {
    const bool mesh = is_mesh_blueprint(state, blueprint);
    const auto* effect=effect_blueprint_settings(state,blueprint);
    if (!mesh && !effect && blueprint != BlueprintId::region && blueprint != BlueprintId::camera)
        return invalid("Unknown scene blueprint");
    if(blueprint==BlueprintId::region && std::ranges::count_if(state.document.instances,[](const auto& instance){
        return instance.blueprint==BlueprintId::region;
    })>=max_regions)return invalid("Region instance limit exceeded");
    if (state.document.instances.size() >= max_scene_instances || !state.document.next_instance_id ||
        state.document.next_instance_id == std::numeric_limits<u32>::max())
        return invalid("Scene instance limit reached");
    const auto id = state.document.next_instance_id;
    std::string name = mesh ? "Mesh" : blueprint==BlueprintId::region ? "Region" : blueprint==BlueprintId::camera ? "Camera" : "Sun";
    if (mesh && blueprint != BlueprintId::mesh)
        name = std::ranges::find(state.document.mesh_assets, blueprint, &MeshBlueprint::id)->name;
    if(effect && blueprint!=BlueprintId::sun)
        name=std::ranges::find(state.document.effect_assets,blueprint,&EffectBlueprint::id)->name;
    const auto suffix = " " + std::to_string(id);
    if (name.size() + suffix.size() > 256) {
        auto end = 256 - suffix.size();
        // Never split a multibyte UTF-8 character when reserving the ID suffix.
        while (end && (static_cast<unsigned char>(name[end]) & 0xc0U) == 0x80U) --end;
        name.resize(end);
    }
    SceneInstance instance{id, blueprint, name + suffix, {}};
    if (blueprint == BlueprintId::mesh) {
        instance.settings = state.document.mesh_blueprint;
        instance.transform = {{1.7F, 0, 0}, {}, .7F};
    } else if (effect) {
        instance.settings = *effect;
        instance.transform.position = {-1.6F, 0, 0};
    } else if(blueprint==BlueprintId::region) {
        RegionSettings settings;settings.boundary=make_region(RegionShape::box,{},2);
        instance.settings=std::move(settings);
    } else if(blueprint==BlueprintId::camera) {
        // A new camera starts where the editor is looking and becomes the
        // scene's camera when it is the first one; later cameras wait to be
        // made active explicitly, so the shot never changes by accident.
        CameraSettings lens;
        lens.active = !has_camera(state);
        instance.settings = lens;
        place_camera(instance, state.viewport.editor_camera);
    } else instance.settings = std::ranges::find(state.document.mesh_assets, blueprint, &MeshBlueprint::id)->settings;
    state.document.instances.push_back(std::move(instance));
    ++state.document.next_instance_id;
    state.viewport.selected_object = id;
    state.viewport.selected_vertex = 0;
    return id;
}
content::Result<u32> import_mesh(State& state, const std::filesystem::path& path) {
    if (path.empty() || path.native().find('\0') != std::string::npos)
        return invalid("Mesh import requires a nonempty file path");
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".vmesh") return invalid("Mesh import currently supports .vmesh files only");
    std::error_code file_error;
    if (!std::filesystem::is_regular_file(path, file_error))
        return invalid("Mesh import requires an existing regular .vmesh file");
    if (state.document.mesh_assets.size()+state.document.effect_assets.size() >= 254 || state.document.next_blueprint_id < 3 ||
        state.document.next_blueprint_id >= first_reserved_blueprint)
        return invalid("Mesh blueprint limit reached");
    auto loaded = editor::EditableMesh::load(path);
    if (!loaded) return std::unexpected(loaded.error());
    auto name = path.stem().string();
    if (const auto found = loaded->document().metadata.find("name");
        found != loaded->document().metadata.end() && !found->second.empty())
        name = found->second;
    if (name.empty() || name.size() > 256) return invalid("Mesh blueprint name must contain 1..256 bytes");
    auto candidate = state;
    const auto blueprint = static_cast<BlueprintId>(candidate.document.next_blueprint_id++);
    candidate.document.mesh_assets.push_back({blueprint, std::move(name), std::move(*loaded), MeshSettings{}});
    auto created = instantiate(candidate, blueprint);
    if (!created) return std::unexpected(created.error());
    candidate.viewport.inspected_mesh = blueprint;
    // Ensure the complete scene (not merely the source file) fits preview and
    // save limits before committing anything, including new identity counters.
    if (auto valid = encode(candidate); !valid) return std::unexpected(valid.error());
    state = std::move(candidate);
    return *created;
}
content::Result<void> erase_instance(State& state, u32 id) {
    if (!find_instance(state, id)) return invalid("The selected scene instance no longer exists");
    auto tracks = std::vector<timeline::Track>(state.document.timeline.tracks().begin(),
                                               state.document.timeline.tracks().end());
    std::erase_if(tracks, [&](const auto& track) { return track.target.object == id; });
    if (auto replaced = state.document.timeline.replace(std::move(tracks)); !replaced)
        return invalid(replaced.error().message);
    std::erase_if(state.document.instances, [&](const auto& instance) { return instance.id == id; });
    if (state.viewport.selected_object == id) {
        state.viewport.selected_object = 0;
        state.viewport.selected_vertex = 0;
    } else {
        const auto* geometry = editable_mesh(state);
        state.viewport.selected_vertex = geometry && geometry->size()
            ? std::min(state.viewport.selected_vertex, static_cast<u32>(geometry->size() - 1)) : 0;
    }
    return {};
}
namespace {
content::Result<std::string> encode_state(const State& s, bool include_editor_view) {
    if (auto valid = validate_state(s); !valid)
        return std::unexpected(valid.error());
    auto mesh = content::vmesh::write_vmesh(s.document.mesh.document());
    if (!mesh)
        return std::unexpected(mesh.error());
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o << std::setprecision(9) << std::boolalpha;
    o << "vscene 1.0\neditor_project = 5;\nrevision = " << s.document.revision
      << ";\nmesh_data = " << quote_string(*mesh) << ";\n";
    o << "blueprints = { mesh = ";
    write_settings(o, s.document.mesh_blueprint);
    o << "; sun = ";
    write_settings(o, s.document.sun_blueprint);
    o << "; };\nnext_blueprint_id = " << s.document.next_blueprint_id << ";\nmesh_assets = [\n";
    for (const auto& asset : s.document.mesh_assets) {
        auto geometry = content::vmesh::write_vmesh(asset.geometry.document());
        if (!geometry) return std::unexpected(geometry.error());
        o << "    { id = " << static_cast<u32>(asset.id) << "; name = " << quote_string(asset.name)
          << "; mesh_data = " << quote_string(*geometry) << "; settings = ";
        write_settings(o, asset.settings);
        o << "; },\n";
    }
    o << "];\neffect_assets = [\n";
    for(const auto& asset:s.document.effect_assets) {
        o<<"    { id = "<<static_cast<u32>(asset.id)<<"; name = "<<quote_string(asset.name)<<"; type = \"sun\"; settings = ";
        write_settings(o,asset.settings);o<<"; },\n";
    }
    o << "];\nmesh_drafts = [\n";
    for(const auto& [id,draft]:s.document.mesh_drafts) {
        auto geometry=content::vmesh::write_vmesh(draft.document());
        if(!geometry) return std::unexpected(geometry.error());
        o << "    { id = " << static_cast<u32>(id) << "; mesh_data = " << quote_string(*geometry) << "; },\n";
    }
    o << "];\nmesh_placements = [\n";
    for(const auto& [id,placement]:s.document.mesh_placements) {
        o << "{ id = " << static_cast<u32>(id) << "; applied = [";
        const auto matrix=[&](const Mat4& m) { for(unsigned c=0;c<4;++c)for(unsigned r=0;r<4;++r)o<<m[c][r]<<", "; };
        matrix(placement.applied);o<<"]; ";
        if(placement.draft){o<<"draft = [";matrix(*placement.draft);o<<"]; ";}
        o<<"},\n";
    }
    o << "];\nnext_instance_id = " << s.document.next_instance_id << ";\ninstances = [\n";
    for (const auto& instance : s.document.instances) {
        o << "    { id = " << instance.id << "; blueprint = "
          << static_cast<u32>(instance.blueprint) << "; name = " << quote_string(instance.name)
          << "; settings = ";
        std::visit([&](const auto& value) { write_settings(o, value); }, instance.settings);
        o << "; transform = ";
        write_transform(o, instance.transform);
        o << "; },\n";
    }
    o << "];\n";
    o << "world_bounds = { minimum = "; vector(o, s.document.world_bounds.minimum);
    o << "; maximum = "; vector(o, s.document.world_bounds.maximum); o << "; };\n";
    const auto& environment = s.document.environment;
    o << "environment = { stars = " << environment.stars
      << "; star_seed = " << environment.star_seed << "; exposure = " << environment.exposure
      << "; bloom_threshold = " << environment.bloom_threshold
      << "; bloom_strength = " << environment.bloom_strength << "; };\n";
    o << "view = { mode = " << static_cast<int>(s.viewport.mode)
      << "; inspected_mesh = " << static_cast<u32>(s.viewport.inspected_mesh)
      << "; selected = " << s.viewport.selected_object
      << "; vertex = " << s.viewport.selected_vertex << "; weld = " << s.viewport.weld << "; paused = " << s.viewport.paused
      << "; time = " << s.viewport.time << "; ";
    if (include_editor_view) {
        o << "sequence = " << s.viewport.sequence << "; ";
        write_camera(o, s.viewport.editor_camera);
        o << "show_regions = " << s.viewport.show_regions << "; show_world_bounds = " << s.viewport.show_world_bounds << "; ";
        o << "gizmo_only = " << s.viewport.gizmo_only << "; ";
    }
    o << "};\n";
    o << "timeline = {\n    duration = " << s.document.timeline_duration << ";\n    tracks = [\n";
    for (const auto& track : s.document.timeline.tracks()) {
        o << "        { object = " << track.target.object
          << "; property = " << quote_string(track.target.property)
          << "; label = " << quote_string(track.label) << "; layer = " << quote_string(track.layer)
          << "; keys = [\n";
        for (const auto& key : track.keys) {
            o << "            { time = " << key.time << "; value = ";
            animation_value(o, key.value);
            o << "; incoming = "
              << quote_string(key.incoming == timeline::Interpolation::hold ? "hold" : "linear")
              << "; },\n";
        }
        o << "        ]; },\n";
    }
    o << "    ];\n    keyframes = [\n";
    for (const auto& [time, name] : s.document.keyframe_names)
        o << "        { time = " << time << "; name = " << quote_string(name) << "; },\n";
    o << "    ];\n};\n";
    auto result = o.str();
    if (result.size() > max_scene_bytes)
        return invalid("Editor scene exceeds the 32 MiB preview limit");
    return result;
}
} // namespace
content::Result<std::string> encode(const State& s) {
    return encode_state(s, true);
}
content::Result<std::string> encode_scene(const State& s) {
    return encode_state(s, false);
}
content::Result<State> decode(std::string_view source) {
    auto doc = content::parse_document(source, {.limits = {.max_source_bytes = max_scene_bytes,
                                                           .max_decoded_bytes = 64 * 1024 * 1024,
                                                           .max_string_bytes = editor::mesh_limits().max_source_bytes}});
    if (!doc)
        return std::unexpected(doc.error());
    auto mesh_source = doc->root().get<std::string>("mesh_data");
    if (!mesh_source)
        return std::unexpected(mesh_source.error());
    auto mesh_doc = content::vmesh::parse_vmesh(*mesh_source, mesh_read_options());
    if (!mesh_doc)
        return std::unexpected(mesh_doc.error());
    auto mesh = editor::EditableMesh::create(std::move(*mesh_doc));
    if (!mesh)
        return std::unexpected(mesh.error());
    return doc->read([&](content::Reader& r) {
        const auto version = r.get<u32>("editor_project");
        if (version < 1 || version > 5)
            r.fail("Unsupported editor project version");
        State s{.document = {.revision = r.get<u64>("revision"), .mesh = std::move(*mesh)}};
        for (const auto member : r.members()) if (member.name == "world_bounds")
            s.document.world_bounds = {member.value.get<Vec3>("minimum"), member.value.get<Vec3>("maximum")};
        Regions legacy_regions;
        for (const auto member : r.members()) if (member.name == "regions")
            legacy_regions = read_regions(member.value);
        for (const auto member : r.members()) {
            if (member.name != "environment") continue;
            const auto e = member.value;
            s.document.environment = {e.get_or<u32>("stars", 0), e.get_or<u32>("star_seed", 32),
                e.get_or<f32>("exposure", .9F), e.get_or<f32>("bloom_threshold", 6.5F),
                e.get_or<f32>("bloom_strength", 0)};
        }
        s.document.next_blueprint_id = r.get_or<u32>("next_blueprint_id", 3);
        for(const auto member:r.members()) if(member.name=="mesh_placements") {
            for(const auto entry:member.value.elements()) {
                if(s.document.mesh_placements.size()>=256)entry.fail("Too many mesh placements");
                const auto read_matrix=[&](content::Reader values) {
                    Mat4 m; unsigned i=0;
                    for(const auto value:values.elements()) {
                        if(i==16)value.fail("Too many matrix values");
                        m[i/4][i%4]=value.as<f32>();++i;
                    }
                    if(i!=16)values.fail("Expected 16 matrix values");
                    return m;
                };
                MeshPlacement placement{read_matrix(entry.child("applied")),{}};
                for(const auto field:entry.members())if(field.name=="draft")placement.draft=read_matrix(field.value);
                if(!s.document.mesh_placements.emplace(static_cast<BlueprintId>(entry.get<u32>("id")),placement).second)
                    entry.fail("Duplicate mesh placement");
            }
        }
        for(const auto member:r.members()) {
            if(member.name!="mesh_drafts") continue;
            for(const auto entry:member.value.elements()) {
                if(s.document.mesh_drafts.size()>=255) entry.fail("Too many mesh drafts");
                auto document=content::vmesh::parse_vmesh(entry.get<std::string>("mesh_data"),mesh_read_options());
                if(!document) entry.fail(document.error().message);
                auto draft=editor::EditableMesh::create(std::move(*document));
                if(!draft) entry.fail(draft.error().message);
                if(!s.document.mesh_drafts.emplace(static_cast<BlueprintId>(entry.get<u32>("id")),std::move(*draft)).second)
                    entry.fail("Duplicate mesh draft identity");
            }
        }
        for (const auto member : r.members()) {
            if (member.name != "mesh_assets") continue;
            for (const auto entry : member.value.elements()) {
                if (s.document.mesh_assets.size() >= 254) entry.fail("Too many imported mesh blueprints");
                auto document = content::vmesh::parse_vmesh(entry.get<std::string>("mesh_data"), mesh_read_options());
                if (!document) entry.fail(document.error().message);
                auto geometry = editor::EditableMesh::create(std::move(*document));
                if (!geometry) entry.fail(geometry.error().message);
                s.document.mesh_assets.push_back({static_cast<BlueprintId>(entry.get<u32>("id")),
                                         entry.get<std::string>("name"), std::move(*geometry),
                                         read_mesh(entry.child("settings"))});
            }
        }
        bool has_instances{};
        for(const auto member:r.members())if(member.name=="effect_assets") {
            for(const auto entry:member.value.elements()) {
                if(s.document.effect_assets.size()>=254)entry.fail("Too many imported effect blueprints");
                if(entry.get<std::string>("type")!="sun")entry.fail("Unsupported saved effect type");
                s.document.effect_assets.push_back({static_cast<BlueprintId>(entry.get<u32>("id")),
                    entry.get<std::string>("name"),read_sun(entry.child("settings"))});
            }
        }
        for (const auto member : r.members())
            if (member.name == "instances") has_instances = true;
        if (has_instances) {
            s.document.mesh_blueprint = read_mesh(r.child("blueprints").child("mesh"));
            s.document.sun_blueprint = read_sun(r.child("blueprints").child("sun"));
            s.document.next_instance_id = r.get<u32>("next_instance_id");
            s.document.instances.clear();
            for (const auto entry : r.child("instances").elements()) {
                if (s.document.instances.size() >= max_scene_instances) entry.fail("Too many scene instances");
                SceneInstance instance{entry.get<u32>("id"),
                                       static_cast<BlueprintId>(entry.get<u32>("blueprint")),
                                       entry.get<std::string>("name"), {}};
                if (is_mesh_blueprint(s, instance.blueprint))
                    instance.settings = read_mesh(entry.child("settings"));
                else if (effect_blueprint_settings(s,instance.blueprint))
                    instance.settings = read_sun(entry.child("settings"));
                else if(instance.blueprint==BlueprintId::region)
                    instance.settings=read_region(entry.child("settings"));
                else if(instance.blueprint==BlueprintId::camera)
                    instance.settings=read_camera_settings(entry.child("settings"));
                else entry.fail("Unknown scene blueprint");
                instance.transform = version >= 2 ? read_transform(entry.child("transform"))
                    : read_legacy_transform(entry.child("settings"), is_mesh_blueprint(s, instance.blueprint));
                s.document.instances.push_back(std::move(instance));
            }
        } else {
            if (version != 1) r.fail("Editor project v2 requires explicit scene instances");
            // Previous scenes always contained one mesh and one sun. Upgrade
            // both as real instances, preserving their IDs and animation tracks.
            s.document.instances[0].settings = read_mesh(r.child("model"));
            s.document.instances[1].settings = read_sun(r.child("sun"));
            s.document.instances[0].transform = read_legacy_transform(r.child("model"), true);
            s.document.instances[1].transform = read_legacy_transform(r.child("sun"), false);
            s.document.mesh_blueprint = std::get<MeshSettings>(s.document.instances[0].settings);
            s.document.sun_blueprint = std::get<SunSettings>(s.document.instances[1].settings);
        }
        auto v = r.child("view");
        for (const auto member : v.members())
            if (member.name == "sequence") s.viewport.sequence = v.get<u64>("sequence");
        const auto mode = v.get<u32>("mode");
        if (mode > 2)
            v.fail("Unknown editor view mode");
        s.viewport.mode = static_cast<ViewMode>(mode);
        s.viewport.selected_object = v.get<u32>("selected");
        s.viewport.selected_vertex = v.get<u32>("vertex");
        // Legacy region IDs lived in a separate registry. Allocate fresh normal
        // instance IDs so an old region #1 cannot collide with mesh instance #1.
        for(const auto& old:legacy_regions.items) {
            const auto selected=s.viewport.selected_object;
            const auto vertex=s.viewport.selected_vertex;
            auto created=instantiate(s,BlueprintId::region);if(!created)r.fail(created.error().message);
            auto* instance=find_instance(s,*created);
            instance->name=old.name;instance->transform.position=old.center();
            auto& settings=std::get<RegionSettings>(instance->settings);
            settings.boundary=static_cast<const RegionGeometry&>(old);settings.note=old.note;
            settings.show_walls=old.show_walls;
            for(auto& p:settings.boundary.points)for(unsigned a=0;a<3;++a)p[a]-=instance->transform.position[a];
            s.viewport.selected_object=selected;
            s.viewport.selected_vertex=vertex;
        }
        bool has_inspected_mesh{};
        for (const auto member : v.members())
            if (member.name == "inspected_mesh") has_inspected_mesh = true;
        if (has_inspected_mesh)
            s.viewport.inspected_mesh = static_cast<BlueprintId>(v.get<u32>("inspected_mesh"));
        else {
            // Migrate the old implicit target once; subsequent scene selection
            // must never silently retarget the blueprint editor.
            const auto* selected = find_instance(s, s.viewport.selected_object);
            if (selected && is_mesh_instance(s, selected->id)) s.viewport.inspected_mesh = selected->blueprint;
            if (!mesh_target(s)) s.viewport.selected_vertex = 0;
        }
        s.viewport.weld = v.get<bool>("weld");
        s.viewport.paused = v.get<bool>("paused");
        s.viewport.time = v.get<f32>("time");
        bool has_editor_camera{};
        for (const auto member : v.members())
            if (member.name == "yaw" || member.name == "pitch" ||
                member.name == "distance" || member.name == "camera_target")
                has_editor_camera = true;
        if (has_editor_camera)
            s.viewport.editor_camera = read_camera(v);
        s.viewport.show_regions = v.get_or<bool>("show_regions", false);
        s.viewport.show_world_bounds = v.get_or<bool>("show_world_bounds", false);
        s.viewport.gizmo_only = v.get_or<bool>("gizmo_only", false);
        std::optional<LegacyCameraShot> legacy;
        if (version < 5) legacy = read_legacy_camera_shot(r, s.viewport.editor_camera);
        // Legacy scenes had a playhead but no duration; retain their current
        // view instead of placing a saved time outside the new timeline ruler.
        s.document.timeline_duration = std::max(10.F, s.viewport.time);
        for (const auto member : r.members())
            if (member.name == "timeline")
                read_animation(s, member.value, legacy ? &*legacy : nullptr);
        if (legacy)
            if (auto migrated = migrate_legacy_camera(s, *legacy); !migrated) r.fail(migrated.error().message);
        // Scene files don't persist the editor view; open looking through the
        // scene's active camera where it starts. Its derived pivot can lie
        // outside the editor camera's range; then keep a valid default.
        if (!has_editor_camera) {
            if (const auto pose = evaluate_camera(s, 0); pose && valid_camera_pose(*pose)) s.viewport.editor_camera = *pose;
            else if (legacy && valid_camera_pose(legacy->pose)) s.viewport.editor_camera = legacy->pose;
        }
        if (auto valid = validate_state(s); !valid)
            r.fail(valid.error().message);
        return s;
    });
}
gfx::Camera camera(const State& s) {
    return camera(s.viewport.editor_camera, s.viewport.mode);
}
gfx::Camera camera(const CameraPose& pose, ViewMode mode, f32 maximum_distance) {
    gfx::Camera camera;
    const auto distance = mode == ViewMode::scene ? pose.distance : pose.distance * .52F;
    const auto yaw = pose.yaw * std::numbers::pi_v<f32> / 180,
               pitch = pose.pitch * std::numbers::pi_v<f32> / 180;
    camera
        .set_position({pose.target.x + std::sin(yaw) * std::cos(pitch) * distance,
                       pose.target.y + std::sin(pitch) * distance,
                       pose.target.z + std::cos(yaw) * std::cos(pitch) * distance})
        .look_at(pose.target)
        .set_perspective(
            {.vertical_fov = degrees(2 * std::atan(std::tan(camera_vertical_fov * std::numbers::pi_v<f32> / 360) / pose.zoom) * 180 / std::numbers::pi_v<f32>),
             .near_plane = std::min(.05F, distance * .25F),
             .far_plane = maximum_distance > 0 ? maximum_distance : std::max(200.F, distance * 4)});
    return camera;
}
Mat4 mesh_transform(const State& s) {
    return mesh_transform(s, s.viewport.time);
}
Mat4 mesh_transform(const State& s, f32 time) {
    const auto preview = preview_mesh(s, time);
    return preview ? mesh_transform(s,preview->target.blueprint,preview->transform) : Mat4::identity();
}
Mat4 mesh_transform(const State& state, BlueprintId id, const InstanceTransform& transform) {
    return example::mesh_frame::compose(transformed_mesh(state.viewport.mode,transform),
        mesh_placement(state,id,state.viewport.mode==ViewMode::mesh && state.viewport.inspected_mesh==id));
}
Mat4 mesh_transform(ViewMode mode, const InstanceTransform& transform) {
    return transformed_mesh(mode, transform);
}
Vec3 sun_position(const State& s) {
    return sun_position(s, s.viewport.time);
}
Vec3 sun_position(const State& s, f32 time) {
    const auto* instance = view_instance(s, BlueprintKind::sun);
    if (!instance || s.viewport.mode == ViewMode::sun) return {};
    return evaluate_instance(s, *instance, time).transform.position;
}
namespace {
std::optional<Vec3> projected(Vec3 p, const Mat4& m, const Mat4& view_projection, bool clip_to_viewport = true) {
    const auto world = transform(m, Vec4{p.x, p.y, p.z, 1});
    const auto clip = transform(view_projection, world);
    if (!std::isfinite(clip.w) || clip.w <= 0)
        return {};
    const Vec3 screen{(clip.x / clip.w + 1) * .5F, (1 - clip.y / clip.w) * .5F, clip.z / clip.w};
    if (!std::isfinite(screen.x) || !std::isfinite(screen.y) || !range(screen.z, -1, 1) ||
        (clip_to_viewport && (!range(screen.x, 0, 1) || !range(screen.y, 0, 1))))
        return {};
    return screen;
}
} // namespace
std::optional<Vec3> project_vertex(const State& s, u32 index, Extent2D extent, const gfx::Camera* view) {
    const auto preview = preview_mesh(s, s.viewport.time);
    if (!preview) return {};
    const auto* geometry = mesh_view_geometry(s, preview->target.blueprint);
    if (!geometry || !preview->settings.visible || index >= geometry->size())
        return {};
    auto c = (view ? *view : camera(s)).snapshot(extent);
    if (!c)
        return {};
    return projected(geometry->position(index), mesh_transform(s,preview->target.blueprint,preview->transform),
                     c->view_projection);
}
std::vector<std::optional<Vec3>> project_vertices(const State& s, Extent2D extent, const gfx::Camera* view, bool clip_to_viewport) {
    const auto preview = preview_mesh(s, s.viewport.time);
    if (!preview) return {};
    const auto* geometry = mesh_view_geometry(s, preview->target.blueprint);
    if (!geometry) return {};
    std::vector<std::optional<Vec3>> result(geometry->size());
    if (!preview->settings.visible)
        return result;
    auto c = (view ? *view : camera(s)).snapshot(extent);
    if (!c)
        return result;
    const auto model = mesh_transform(s,preview->target.blueprint,preview->transform);
    for (u32 i = 0; i < geometry->size(); ++i)
        result[i] = projected(geometry->position(i), model, c->view_projection, clip_to_viewport);
    return result;
}
std::optional<u32> pick_vertex(const State& s, Vec2 p, Extent2D extent, f32 radius, const gfx::Camera* view) {
    if (!range(p.x, 0, 1) || !range(p.y, 0, 1) || !std::isfinite(radius) || radius < 0 ||
        !extent.width || !extent.height)
        return {};
    f32 distance = radius * radius;
    std::optional<u32> found;
    f32 depth = 2;
    const auto projected = project_vertices(s, extent, view);
    for (u32 i = 0; i < projected.size(); ++i) {
        const auto v = projected[i];
        if (!v)
            continue;
        const auto x = (v->x - p.x) * static_cast<f32>(extent.width),
                   y = (v->y - p.y) * static_cast<f32>(extent.height);
        const auto d = x * x + y * y;
        if (d < distance - .01F || (std::abs(d - distance) < .01F && v->z < depth)) {
            distance = d;
            depth = v->z;
            found = i;
        }
    }
    return found;
}
content::Result<void> save_new(const std::filesystem::path& path, std::string_view bytes) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0)
        return invalid("Cannot create new file '" + path.string() + "': " + std::strerror(errno));
    std::size_t offset{};
    bool ok = true;
    while (offset < bytes.size()) {
        const auto n = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ok = false;
            break;
        }
        offset += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0)
        ok = false;
    ::close(fd);
    if (!ok) {
        ::unlink(path.c_str());
        return invalid("Failed to write new scene/mesh file");
    }
    return {};
}
content::Result<State> load_scene(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return invalid("Cannot open scene");
    std::string data;
    char buffer[8192];
    while (file) {
        file.read(buffer, sizeof buffer);
        data.append(buffer, static_cast<std::size_t>(file.gcount()));
        if (data.size() > max_scene_bytes)
            return invalid("Scene exceeds 32 MiB");
    }
    if (!file.eof())
        return invalid("Failed reading scene");
    return decode(data);
}
} // namespace editor_example
