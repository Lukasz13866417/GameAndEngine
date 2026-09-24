#include "document_patch.hpp"
#include <vng/editor/limits.hpp>
#include "animation.hpp"
#include "authoring_limits.hpp"
#include "../support/mesh_frame.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace editor_example {
namespace {
using namespace vng;
constexpr u64 max_revision = (u64{1} << 53) - 1;
constexpr std::size_t max_bytes = vng::editor::max_document_bytes;
// Up to nine editable properties per instance. This transport ceiling must
// follow the scene ceiling, not the user budget.
constexpr std::size_t max_properties = std::size_t{max_scene_instances} * 9;
auto invalid(std::string message) {
    content::Diagnostic error;
    error.message = std::move(message);
    return std::unexpected(std::move(error));
}
using Reference = std::variant<f32*, Vec3*, bool*>;
template<class S>
using PropertyReference = std::conditional_t<std::is_const_v<S>,
    std::variant<const f32*, const Vec3*, const bool*>, Reference>;
template<class S>
std::optional<PropertyReference<S>> property_reference(S& state, const timeline::Target& target) {
    const auto& key = target.property;
    if (target.object > UINT32_MAX) return {};
    auto* instance = find_instance(state, static_cast<u32>(target.object));
    if (!instance) return {};
    if (key == "position") return &instance->transform.position;
    if (key == "rotation") return &instance->transform.rotation;
    if (key == "scale") return &instance->transform.scale;
    if (key == "axis_scale") return &instance->transform.axis_scale;
    return std::visit([&](auto& settings) -> std::optional<PropertyReference<S>> {
        if (key == "visible") return &settings.visible;
        if constexpr (std::same_as<std::decay_t<decltype(settings)>, MeshSettings>) {
            if (key == "brightness") return &settings.brightness;
            if (key == "wireframe") return &settings.wireframe;
        } else if constexpr (std::same_as<std::decay_t<decltype(settings)>, SunSettings>) {
            if (key == "radius") return &settings.radius;
            if (key == "displacement") return &settings.displacement;
            if (key == "bloom") return &settings.bloom;
            if (key == "white_spots") return &settings.white_spots;
        } else if constexpr (std::same_as<std::decay_t<decltype(settings)>, CameraSettings>) {
            if (key == "zoom") return &settings.zoom;
            if (key == "focus") return &settings.focus;
            if (key == "active") return &settings.active;
            if (key == "orbit") return &settings.orbit;
        }
        return {};
    }, instance->settings);
}
void check(bool valid, std::string_view message) {
    if (!valid) throw std::invalid_argument(std::string(message));
}
void shape(const DocumentPatch& patch) {
    check(patch.mesh_placements.size() <= 256, "Too many mesh placements");
    for (const auto& [id, placement] : patch.mesh_placements) if (placement) {
        check(bool(example::mesh_frame::inverse(placement->applied)), "Invalid applied mesh placement");
        if (placement->draft) check(bool(example::mesh_frame::inverse(*placement->draft)), "Invalid draft mesh placement");
    }
    check(patch.regions.size() <= max_regions, "Too many region patches");
    std::set<u32> regions;
    for (const auto& region : patch.regions) {
        check(region.id && regions.insert(region.id).second, "Duplicate or invalid region instance");
        if (region.replacement) {
            check(region.replacement->id == region.id && region.points.empty() && !region.point_count,
                  "Mixed or inconsistent region patch");
            const auto valid = validate(*region.replacement);
            check(bool(valid), valid ? "" : valid.error().message);
        } else {
            check(region.point_count && region.point_count <= max_region_points &&
                  !region.points.empty() && region.points.size() <= region.point_count, "Invalid region point patch size");
            std::set<u32> points;
            for (const auto& point : region.points) {
                check(point.index < region.point_count && points.insert(point.index).second, "Duplicate or invalid region point");
                for (unsigned a = 0; a < 3; ++a)
                    check(std::isfinite(point.position[a]) && std::abs(point.position[a]) <= scene_coordinate_limit,
                          "Invalid region point position");
            }
        }
    }
    if (patch.world_bounds) check(valid_world_bounds(*patch.world_bounds), "Invalid world bounds in patch");
    check(patch.base_revision && patch.revision > patch.base_revision && patch.revision <= max_revision,
          "Patch needs ordered authored revisions");
    check(patch.vertices.size() <= 256 && patch.properties.size() <= max_properties &&
          patch.markers.size() <= timeline::max_total_keys, "Patch exceeds editor limits");
    std::set<u32> blueprints;
    for (const auto& edit : patch.vertices) {
        check(edit.base_revision == patch.base_revision && edit.revision == patch.revision &&
              blueprints.insert(edit.blueprint).second, "Duplicate blueprint or inconsistent patch revisions");
        auto valid = encode_edit(edit);
        check(bool(valid), valid ? "" : valid.error().message);
    }
    check(patch.meshes.size()<=256,"Too many mesh draft patches");
    for(const auto& mesh:patch.meshes) {
        check(blueprints.insert(mesh.blueprint).second,"Duplicate blueprint mesh patch");
        const auto valid=editor::validate_mesh_patch(mesh.values);
        check(bool(valid),valid?"":valid.error().message);
    }
    std::set<timeline::Target, TargetLess> targets;
    std::size_t keys{};
    for (const auto& property : patch.properties) {
        check(targets.insert(property.target).second, "Duplicate property in patch");
        check(property.target.object && !property.target.property.empty() &&
              property.target.property.size() <= timeline::max_property_bytes, "Invalid patch property target");
        const bool finite = std::visit([](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::same_as<T, bool>) return true;
            else if constexpr (std::same_as<T, f32>) return std::isfinite(value);
            else if constexpr (std::same_as<T, Vec3>)
                return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
            else return false;
        }, property.base);
        check(finite, "Unsupported or nonfinite patch property value");
        if (!property.track) continue;
        check(property.track->target == property.target, "Patch track targets another property");
        keys += property.track->keys.size();
        check(keys <= timeline::max_total_keys, "Too many patch keys");
        timeline::Timeline check_track;
        auto valid = check_track.replace_track(*property.track);
        check(bool(valid), valid ? "" : valid.error().message);
    }
    if (patch.duration) check(std::isfinite(*patch.duration) && *patch.duration >= .1F &&
                              *patch.duration <= timeline::max_time, "Invalid patch duration");
    for (const auto& [time, name] : patch.markers)
        check(std::isfinite(time) && time >= 0 && time <= timeline::max_time &&
              (!name || name->size() <= 256), "Invalid patch marker");
}
struct Writer {
    std::string bytes{"VNGPATCH", 8};
    void integer(u64 value, unsigned width) {
        for (unsigned i = 0; i < width; ++i) bytes += static_cast<char>((value >> (i * 8)) & 255);
    }
    void scalar(f32 value) { integer(std::bit_cast<u32>(value), 4); }
    void string(std::string_view value) { integer(value.size(), 4); bytes += value; }
    void value(const timeline::Value& value) {
        std::visit([&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::same_as<T, bool>) { integer(0, 1); integer(v, 1); }
            else if constexpr (std::same_as<T, f32>) { integer(1, 1); scalar(v); }
            else if constexpr (std::same_as<T, Vec3>) { integer(2, 1); scalar(v.x); scalar(v.y); scalar(v.z); }
            else check(false, "Unsupported patch property value");
        }, value);
    }
};
struct Reader {
    std::string_view bytes;
    std::size_t offset{};
    u64 integer(unsigned width) {
        check(width <= bytes.size() - offset, "Truncated document patch");
        u64 result{};
        for (unsigned i = 0; i < width; ++i) result |= u64(static_cast<unsigned char>(bytes[offset++])) << (i * 8);
        return result;
    }
    bool flag() { const auto value = integer(1); check(value <= 1, "Invalid patch flag"); return value != 0; }
    f32 scalar() {
        const auto result = std::bit_cast<f32>(static_cast<u32>(integer(4)));
        check(std::isfinite(result), "Nonfinite patch value"); return result;
    }
    std::string_view string(std::size_t limit) {
        const auto size = integer(4);
        check(size <= limit && size <= bytes.size() - offset, "Invalid patch string length");
        const auto result = bytes.substr(offset, size); offset += size; return result;
    }
    timeline::Value value() {
        switch (integer(1)) {
        case 0: return flag();
        case 1: return scalar();
        case 2: return Vec3{scalar(), scalar(), scalar()};
        default: throw std::invalid_argument("Unknown patch value type");
        }
    }
};
} // namespace

DocumentChanges changes_of(const DocumentPatch& patch) {
    DocumentChanges changes{.duration = patch.duration.has_value(), .world_bounds = patch.world_bounds.has_value()};
    for (const auto& region : patch.regions) {
        auto& scope = changes.regions[region.id];
        scope.whole = region.replacement.has_value();
        for (const auto& point : region.points) scope.points.insert(point.index);
    }
    for (const auto& edit : patch.vertices)
        for (const auto& vertex : edit.vertices) changes.vertices[edit.blueprint].insert(vertex.index);
    for (const auto& property : patch.properties) changes.properties.insert(property.target);
    for (const auto& [time, name] : patch.markers) { (void)name; changes.markers.insert(time); }
    for(const auto& mesh:patch.meshes)changes.meshes[mesh.blueprint]=editor::changes_of(mesh.values);
    for (const auto& [id, value] : patch.mesh_placements) changes.mesh_placements.insert(static_cast<u32>(id));
    return changes;
}
DocumentChanges animation_changes(const Document& before, const Document& after) {
    DocumentChanges changes{.duration = before.timeline_duration != after.timeline_duration};
    for (const auto& track : before.timeline.tracks()) {
        const auto* next = after.timeline.find(track.target);
        if (!next || *next != track) changes.properties.insert(track.target);
    }
    for (const auto& track : after.timeline.tracks())
        if (!before.timeline.find(track.target)) changes.properties.insert(track.target);
    for (const auto& [time, name] : before.keyframe_names) {
        const auto found = after.keyframe_names.find(time);
        if (found == after.keyframe_names.end() || found->second != name) changes.markers.insert(time);
    }
    for (const auto& [time, name] : after.keyframe_names) {
        (void)name;
        if (!before.keyframe_names.contains(time)) changes.markers.insert(time);
    }
    return changes;
}
content::Result<DocumentPatch> capture_patch(u64 base, const State& state, const DocumentChanges& changes) {
    if (changes.full) return invalid("Structural changes need a snapshot, not a value patch");
    DocumentPatch patch{base, state.document.revision};
    for (auto id : changes.mesh_placements) {
        const auto blueprint = static_cast<BlueprintId>(id);
        if (!mesh_geometry(state, blueprint)) return invalid("Missing mesh placement blueprint");
        const auto it = state.document.mesh_placements.find(blueprint);
        patch.mesh_placements.emplace(blueprint, it == state.document.mesh_placements.end()
            ? std::nullopt : std::optional{it->second});
    }
    for(const auto& [id,scope]:changes.meshes) {
        const auto blueprint=static_cast<BlueprintId>(id);
        const auto* mesh=mesh_edit_geometry(state,blueprint);
        if(!mesh)return invalid("Missing blueprint for mesh draft patch");
        auto values=editor::capture_mesh_patch(mesh->document(),scope);
        if(!values)return std::unexpected(values.error());
        patch.meshes.push_back({id,state.document.mesh_drafts.contains(blueprint),std::move(*values)});
    }
    for (const auto& [blueprint, ids] : changes.vertices) {
        const auto* mesh = mesh_edit_geometry(state, static_cast<BlueprintId>(blueprint));
        if (!mesh) return invalid("Patch references missing mesh blueprint");
        VertexEdit edit{base, patch.revision, {}, blueprint};
        for (auto id : ids) {
            if (id >= mesh->size()) return invalid("Patch references missing vertex");
            edit.vertices.push_back({id, mesh->position(id)});
        }
        patch.vertices.push_back(std::move(edit));
    }
    for (const auto& target : changes.properties) {
        const auto reference = property_reference(state, target);
        if (!reference) return invalid("Patch references missing property");
        PropertyPatch value{target, std::visit([](const auto* p) -> timeline::Value { return *p; }, *reference), {}};
        if (const auto* track = state.document.timeline.find(target)) value.track = *track;
        patch.properties.push_back(std::move(value));
    }
    if (changes.duration) patch.duration = state.document.timeline_duration;
    if (changes.world_bounds) patch.world_bounds = state.document.world_bounds;
    for (const auto& [id, scope] : changes.regions) {
        const auto* instance = find_instance(state, id);
        const auto* settings = instance ? std::get_if<RegionSettings>(&instance->settings) : nullptr;
        if (!settings) return invalid("Patch references missing region instance");
        RegionPatch region{.id = id};
        if (scope.whole) {
            Region value; value.id = id; value.name = instance->name; value.note = settings->note;
            value.show_walls = settings->show_walls;
            static_cast<RegionGeometry&>(value) = settings->boundary;
            region.replacement = std::move(value);
        } else {
            if (scope.points.empty()) continue;
            region.point_count = static_cast<u32>(settings->boundary.points.size());
            for (const auto index : scope.points) {
                if (index >= region.point_count) return invalid("Patch references missing region point");
                region.points.push_back({index, settings->boundary.points[index]});
            }
        }
        patch.regions.push_back(std::move(region));
    }
    for (auto time : changes.markers) {
        const auto name = state.document.keyframe_names.find(time);
        patch.markers.emplace(time, name == state.document.keyframe_names.end() ? std::nullopt : std::optional{name->second});
    }
    return patch;
}
content::Result<void> apply_patch(State& state, const DocumentPatch& patch) {
    try { shape(patch); } catch (const std::invalid_argument& error) { return invalid(error.what()); }
    if (state.document.revision != patch.base_revision) return invalid("Stale document patch base revision");
    for (const auto& [id, placement] : patch.mesh_placements)
        if (!mesh_geometry(state, id)) return invalid("Missing mesh placement blueprint");
    // Prepare only changed blueprints, without touching unrelated scene data.
    std::map<BlueprintId,editor::EditableMesh> meshes;
    for(const auto& edit:patch.meshes) {
        const auto id=static_cast<BlueprintId>(edit.blueprint);
        const auto* source=mesh_edit_geometry(state,id);
        if(!source)return invalid("Mesh draft patch references missing blueprint");
        if(!edit.present)continue;
        auto next=editor::apply_mesh_patch(*source,edit.values);
        if(!next)return std::unexpected(next.error());
        meshes.emplace(id,std::move(*next));
    }
    const bool needs_animation_validation = !patch.properties.empty() || patch.duration || !patch.markers.empty();
    auto properties = needs_animation_validation ? animation_properties(state) : std::vector<AnimationProperty>{};
    std::vector<Reference> references;
    std::optional<timeline::Timeline> animation;
    for (const auto& property : patch.properties) {
        const auto description = std::ranges::find(properties, property.target, &AnimationProperty::target);
        auto reference = property_reference(state, property.target);
        if (description == properties.end() || !reference) return invalid("Unknown patch property");
        if (auto valid = validate_property_value(*description, property.base); !valid) return valid;
        references.push_back(*reference);
        const auto* current = state.document.timeline.find(property.target);
        if (bool(current) != property.track.has_value() || (current && *current != *property.track)) {
            if (!animation) animation = state.document.timeline;
        }
    }
    if (animation) {
        // Delete replaced tracks before inserting: a valid batch may swap
        // tracks at the capacity limit. Unrelated tracks are never transmitted.
        for (const auto& property : patch.properties) (void)animation->erase(property.target);
        for (const auto& property : patch.properties) if (property.track)
            if (auto valid = animation->replace_track(*property.track); !valid) return invalid(valid.error().message);
    }
    std::optional<std::map<f32, std::string>> names;
    if (!patch.markers.empty()) {
        names = state.document.keyframe_names;
        for (const auto& [time, name] : patch.markers) {
            if (name) names->insert_or_assign(time, *name); else names->erase(time);
        }
    }
    if (animation || names || patch.duration)
      if (auto valid = validate_animation(properties, animation ? *animation : state.document.timeline,
            patch.duration.value_or(state.document.timeline_duration), names ? *names : state.document.keyframe_names); !valid)
        return valid;
    for (const auto& edit : patch.vertices) {
        const auto* mesh = mesh_edit_geometry(state, static_cast<BlueprintId>(edit.blueprint));
        if (!mesh) return invalid("Patch references missing mesh blueprint");
        for (const auto& vertex : edit.vertices)
            if (vertex.index >= mesh->size()) return invalid("Patch vertex is outside mesh");
    }
    auto regions=patch.regions;
    for (const auto& region : regions) {
        const auto* settings = region_settings(state, region.id);
        if (!settings) return invalid("Unknown region instance in patch");
        if (!region.replacement && settings->boundary.points.size() != region.point_count)
            return invalid("Region point patch targets changed topology");
    }
    // Everything is validated/allocated before committing. Mesh drafts above
    // copy only affected blueprints; unrelated geometry/instances stay untouched.
    for(const auto& edit:patch.meshes) {
        const auto id=static_cast<BlueprintId>(edit.blueprint);
        if(!edit.present)state.document.mesh_drafts.erase(id);
        else if(auto it=state.document.mesh_drafts.find(id);it!=state.document.mesh_drafts.end())it->second=std::move(meshes.at(id));
        else state.document.mesh_drafts.insert(meshes.extract(id));
    }
    for (const auto& [id, placement] : patch.mesh_placements) {
        if (placement) state.document.mesh_placements.insert_or_assign(id, *placement);
        else state.document.mesh_placements.erase(id);
    }
    if (animation) state.document.timeline = std::move(*animation);
    if (names) state.document.keyframe_names = std::move(*names);
    if (patch.duration) state.document.timeline_duration = *patch.duration;
    if (patch.world_bounds) state.document.world_bounds = *patch.world_bounds;
    for (auto& region : regions) {
        auto* instance = find_instance(state, region.id);
        auto& settings = std::get<RegionSettings>(instance->settings);
        if (region.replacement) {
            settings.boundary = std::move(static_cast<RegionGeometry&>(*region.replacement));
            settings.note = std::move(region.replacement->note); instance->name = std::move(region.replacement->name);
            settings.show_walls = region.replacement->show_walls;
        } else for (const auto& point : region.points) settings.boundary.points[point.index] = point.position;
    }
    for (std::size_t i = 0; i < patch.properties.size(); ++i)
        std::visit([&](auto* pointer) { *pointer = std::get<std::remove_pointer_t<decltype(pointer)>>(patch.properties[i].base); }, references[i]);
    for (const auto& edit : patch.vertices) {
        auto* mesh = mesh_edit_geometry(state, static_cast<BlueprintId>(edit.blueprint));
        for (const auto& vertex : edit.vertices) (void)mesh->set_position(vertex.index, vertex.position);
    }
    state.document.revision = patch.revision;
    return {};
}
content::Result<std::string> encode_patch(const DocumentPatch& patch) {
    try {
        shape(patch);
        Writer out;
        out.integer(9, 1); out.integer(patch.base_revision, 8); out.integer(patch.revision, 8);
        out.integer(patch.vertices.size(), 4); out.integer(patch.properties.size(), 4); out.integer(patch.markers.size(), 4);
        out.integer(patch.duration.has_value(), 1); if (patch.duration) out.scalar(*patch.duration);
        for (const auto& edit : patch.vertices) out.string(*encode_edit(edit));
        for (const auto& property : patch.properties) {
            out.integer(property.target.object, 8); out.string(property.target.property); out.value(property.base);
            out.integer(property.track.has_value(), 1);
            if (!property.track) continue;
            out.string(property.track->label); out.string(property.track->layer);
            out.integer(property.track->keys.size(), 4);
            for (const auto& key : property.track->keys) {
                out.scalar(key.time); out.integer(static_cast<unsigned>(key.incoming), 1); out.value(key.value);
            }
        }
        for (const auto& [time, name] : patch.markers) {
            out.scalar(time); out.integer(name.has_value(), 1); if (name) out.string(*name);
        }
        out.integer(patch.world_bounds.has_value(), 1);
        if (patch.world_bounds) {
            for (auto point : {patch.world_bounds->minimum, patch.world_bounds->maximum})
                for (unsigned i = 0; i < 3; ++i) out.scalar(point[i]);
        }
        out.integer(!patch.regions.empty(),1);
        if(!patch.regions.empty()) {
            out.integer(patch.regions.size(),4);
            for(const auto& region:patch.regions) {
                out.integer(region.id,4); out.integer(region.replacement.has_value(),1);
                if (!region.replacement) {
                    out.integer(region.point_count,4); out.integer(region.points.size(),4);
                    for (const auto& point : region.points) {
                        out.integer(point.index,4);
                        for (unsigned a=0;a<3;++a) out.scalar(point.position[a]);
                    }
                    continue;
                }
                const auto& r=*region.replacement;
                out.string(r.name); out.string(r.note); out.integer(r.show_walls,1); out.integer(r.points.size(),4);
                for(auto p:r.points) for(unsigned a=0;a<3;++a)out.scalar(p[a]);
                out.integer(r.faces.size(),4);
                for(const auto& face:r.faces) {
                    out.integer(face.size(),4);for(auto v:face)out.integer(v,4);
                }
                out.integer(r.loose_edges.size(),4);
                for(auto e:r.loose_edges){out.integer(e[0],4);out.integer(e[1],4);}
            }
        }
        out.integer(patch.meshes.size(),4);
        for(const auto& mesh:patch.meshes) {
            out.integer(mesh.blueprint,4);out.integer(mesh.present,1);
            out.string(*editor::encode_mesh_patch(mesh.values));
        }
        out.integer(patch.mesh_placements.size(), 4);
        for (const auto& [id, placement] : patch.mesh_placements) {
            out.integer(static_cast<u32>(id), 4); out.integer(placement.has_value(), 1);
            if (!placement) continue;
            for (unsigned c=0;c<4;++c) for(unsigned r=0;r<4;++r) out.scalar(placement->applied[c][r]);
            out.integer(placement->draft.has_value(), 1);
            if (placement->draft) for (unsigned c=0;c<4;++c) for(unsigned r=0;r<4;++r) out.scalar((*placement->draft)[c][r]);
        }
        check(out.bytes.size() <= max_bytes, "Document patch exceeds transport capacity");
        return std::move(out.bytes);
    } catch (const std::invalid_argument& error) { return invalid(error.what()); }
}
content::Result<DocumentPatch> decode_patch(std::string_view bytes) {
    try {
        check(bytes.size() <= max_bytes && bytes.substr(0, 8) == "VNGPATCH", "Unknown document patch");
        Reader in{bytes, 8};
        const auto version = in.integer(1);
        check(version >= 1 && version <= 9, "Unknown document patch version");
        DocumentPatch patch{in.integer(8), in.integer(8)};
        const auto vertices = in.integer(4), properties = in.integer(4), markers = in.integer(4);
        check(vertices <= 256 && properties <= max_properties && markers <= timeline::max_total_keys, "Patch counts exceed limits");
        if (in.flag()) patch.duration = in.scalar();
        for (u64 i = 0; i < vertices; ++i) {
            auto edit = decode_edit(in.string(32 + 65536 * 16));
            check(bool(edit), edit ? "" : edit.error().message);
            patch.vertices.push_back(std::move(*edit));
        }
        std::size_t total_keys{};
        for (u64 i = 0; i < properties; ++i) {
            PropertyPatch property{{in.integer(8), std::string(in.string(timeline::max_property_bytes))}, in.value(), {}};
            if (in.flag()) {
                timeline::Track track{property.target, std::string(in.string(timeline::max_label_bytes)),
                    std::string(in.string(timeline::max_layer_bytes)), {}};
                const auto keys = in.integer(4); total_keys += keys;
                check(keys <= timeline::max_keys_per_track && total_keys <= timeline::max_total_keys, "Patch key count exceeds limits");
                for (u64 n = 0; n < keys; ++n) {
                    const auto time = in.scalar();
                    const auto incoming = static_cast<timeline::Interpolation>(in.integer(1));
                    track.keys.push_back({time, in.value(), incoming});
                }
                property.track = std::move(track);
            }
            patch.properties.push_back(std::move(property));
        }
        for (u64 i = 0; i < markers; ++i) {
            const auto time = in.scalar();
            auto name = in.flag() ? std::optional{std::string(in.string(256))} : std::nullopt;
            check(patch.markers.emplace(time, std::move(name)).second, "Duplicate patch marker");
        }
        if (version >= 2 && in.flag())
            patch.world_bounds = WorldBounds{{in.scalar(), in.scalar(), in.scalar()}, {in.scalar(), in.scalar(), in.scalar()}};
        if(version>=3 && in.flag()) {
            // Older packets snapshot a registry (including its allocator).
            // Normalize them into independent whole-instance replacements.
            u32 legacy_next{};
            if (version<6) {
                legacy_next=static_cast<u32>(in.integer(4));
                check(legacy_next!=0,"Invalid legacy region allocator");
            }
            const auto count=in.integer(4);check(count<=max_regions,"Too many patch regions");
            for(u64 i=0;i<count;++i) {
                const auto id=static_cast<u32>(in.integer(4));
                if (version>=6 && !in.flag()) {
                    RegionPatch region{.id=id,.point_count=static_cast<u32>(in.integer(4))};
                    const auto points=in.integer(4);check(points<=max_region_points,"Too many region point edits");
                    for(u64 j=0;j<points;++j)
                        region.points.push_back({static_cast<u32>(in.integer(4)),{in.scalar(),in.scalar(),in.scalar()}});
                    patch.regions.push_back(std::move(region));continue;
                }
                check(version>=6 || (legacy_next && id<legacy_next),"Invalid legacy region identity");
                Region r; r.id=id;r.name=in.string(version>=5?256:128);r.note=in.string(4096);
                if(version>=7)r.show_walls=in.flag();
                const auto points=in.integer(4);check(points<=max_region_points,"Too many region points");
                for(u64 j=0;j<points;++j)r.points.push_back({in.scalar(),in.scalar(),in.scalar()});
                if(version>=4) {
                    const auto faces=in.integer(4);check(faces<=max_region_faces,"Too many region faces");
                    u64 corners{};
                    for(u64 f=0;f<faces;++f) {
                        const auto count=in.integer(4);corners+=count;
                        check(count>=3&&corners<=max_region_corners,"Invalid region face size");
                        RegionFace face;for(u64 v=0;v<count;++v)face.push_back(static_cast<u32>(in.integer(4)));
                        r.faces.push_back(std::move(face));
                    }
                    const auto edges=in.integer(4);check(edges<=max_region_corners,"Too many region edges");
                    for(u64 e=0;e<edges;++e)r.loose_edges.push_back({static_cast<u32>(in.integer(4)),static_cast<u32>(in.integer(4))});
                } else {
                    auto migrated=migrate_region_hull(r);check(bool(migrated),migrated?"":migrated.error().message);
                }
                patch.regions.push_back({.id=id,.replacement=std::move(r)});
            }
        }
        if(version>=8) {
            const auto count=in.integer(4);check(count<=256,"Too many mesh patches");
            for(u64 i=0;i<count;++i) {
                const auto id=static_cast<u32>(in.integer(4));const auto present=in.flag();
                auto value=editor::decode_mesh_patch(in.string(editor::mesh_limits().max_source_bytes));
                check(bool(value),value?"":value.error().message);
                patch.meshes.push_back({id,present,std::move(*value)});
            }
        }
        if (version >= 9) {
            const auto count = in.integer(4); check(count <= 256, "Too many mesh placements");
            for (u64 i = 0; i < count; ++i) {
                const auto id = static_cast<BlueprintId>(in.integer(4));
                std::optional<MeshPlacement> placement;
                if (in.flag()) {
                    placement.emplace();
                    for (unsigned c=0;c<4;++c) for(unsigned r=0;r<4;++r) placement->applied[c][r] = in.scalar();
                    if (in.flag()) {
                        placement->draft.emplace();
                        for (unsigned c=0;c<4;++c) for(unsigned r=0;r<4;++r) (*placement->draft)[c][r] = in.scalar();
                    }
                }
                check(patch.mesh_placements.emplace(id, placement).second, "Duplicate mesh placement");
            }
        }
        check(in.offset == bytes.size(), "Trailing patch data"); shape(patch); return patch;
    } catch (const std::invalid_argument& error) { return invalid(error.what()); }
}
} // namespace editor_example
