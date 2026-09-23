#include "editing_session.hpp"
#include "rotation_edits.hpp"
#include "scale_edits.hpp"
#include "position_edits.hpp"
#include "effects.hpp"
#include "animation.hpp"
#include "../support/mesh_frame.hpp"
#include <algorithm>
#include <cmath>
#include <set>

namespace editor_example {
namespace {
using namespace vng;
content::Result<std::vector<u32>> perform_mesh_operation(editor::EditableMesh& mesh,MeshOperation operation,
    std::span<const u32> vertices,std::span<const gfx::Edge> edges,MeshOperationSettings settings) {
    const auto invalid=[](std::string message)->content::Result<std::vector<u32>> {
        content::Diagnostic error;error.message=std::move(message);return std::unexpected(std::move(error));
    };
    if(settings.levels<1 || settings.levels>4)return invalid("Subdivision levels must be between 1 and 4");
    if(!std::isfinite(settings.strength) || settings.strength<0 || settings.strength>1)
        return invalid("Alignment strength must be between 0 and 1");
    if(operation==MeshOperation::fill) {
        auto result=mesh.fill(vertices);if(!result)return std::unexpected(result.error());
        return std::vector<u32>{vertices.begin(),vertices.end()};
    }
    if(operation==MeshOperation::align) {
        std::vector<Vec3> original;for(auto id:vertices) {
            if(id>=mesh.size())return invalid("Alignment refers to a missing vertex");
            original.push_back(mesh.position(id));
        }
        auto aligned=mesh.align_to_line(vertices);if(!aligned)return std::unexpected(aligned.error());
        for(std::size_t i=2;i<vertices.size();++i) {
            auto p=mesh.position(vertices[i]);
            for(unsigned c=0;c<3;++c)p[c]=original[i][c]+settings.strength*(p[c]-original[i][c]);
            if(auto result=mesh.set_position(vertices[i],p);!result)return std::unexpected(result.error());
        }
        return *aligned;
    }
    std::vector<gfx::Edge> patch(edges.begin(),edges.end());
    std::set<u32> patch_vertices;for(auto e:edges){patch_vertices.insert(e[0]);patch_vertices.insert(e[1]);}
    std::vector<u32> added;
    for(u32 level=0;level<settings.levels;++level) {
        auto divided=mesh.subdivide(patch);if(!divided)return std::unexpected(divided.error());
        added.insert(added.end(),divided->begin(),divided->end());
        patch_vertices.insert(divided->begin(),divided->end());
        if(level+1<settings.levels) {
            patch.clear();for(auto e:mesh.edges())
                if(patch_vertices.contains(e[0])&&patch_vertices.contains(e[1]))patch.push_back(e);
        }
    }
    return added;
}
}

content::Result<u32> EditingSession::import_mesh(const std::filesystem::path& path) {
    if (auto ready = available(); !ready) return std::unexpected(ready.error());
    auto candidate = state_;
    auto imported = editor_example::import_mesh(candidate, path);
    if (!imported) return std::unexpected(imported.error());
    if (auto adopted = replace(std::move(candidate), {.full = true}); !adopted) return std::unexpected(adopted.error());
    return *imported;
}
content::Result<u32> EditingSession::import_asset(const std::filesystem::path& path) {
    if(auto ready=available();!ready)return std::unexpected(ready.error());
    auto candidate=state_;
    auto imported=editor_example::import_asset(candidate,path);
    if(!imported)return std::unexpected(imported.error());
    if(auto adopted=replace(std::move(candidate),{.full=true});!adopted)return std::unexpected(adopted.error());
    return *imported;
}
content::Result<u32> EditingSession::instantiate(BlueprintId blueprint) {
    if (auto ready = available(); !ready) return std::unexpected(ready.error());
    auto candidate = state_;
    auto added = editor_example::instantiate(candidate, blueprint);
    if (!added) return std::unexpected(added.error());
    if (auto adopted = replace(std::move(candidate), {.full = true}); !adopted) return std::unexpected(adopted.error());
    return *added;
}
content::Result<bool> EditingSession::erase_instances(std::span<const u32> objects) {
    if (auto ready = available(); !ready) return std::unexpected(ready.error());
    if (objects.empty()) return false;
    auto candidate = state_;
    std::set<u32> unique(objects.begin(), objects.end());
    for (auto id : unique)
        if (auto erased = erase_instance(candidate, id); !erased) return std::unexpected(erased.error());
    return replace(std::move(candidate), {.full = true});
}
content::Result<bool> EditingSession::replace_mesh_draft(BlueprintId blueprint, u64 expected_revision, editor::EditableMesh mesh) {
    if(auto ready=available();!ready)return std::unexpected(ready.error());
    if(state_.document.revision!=expected_revision || !is_mesh_blueprint(state_,blueprint)) {
        content::Diagnostic error;error.message="Blueprint changed while rebuilding; rebuild again from the current draft";
        return std::unexpected(std::move(error));
    }
    const auto* previous=mesh_edit_geometry(state_,blueprint);
    if(!previous)return false;
    auto scope=editor::mesh_changes(previous->document(),mesh.document());
    if(scope.empty())return false;
    DocumentChanges changes;changes.meshes.emplace(static_cast<u32>(blueprint),std::move(scope));
    auto before=capture(changes);if(!before)return std::unexpected(before.error());
    state_.document.mesh_drafts.insert_or_assign(blueprint,std::move(mesh));
    state_.viewport.selected_vertex=std::min(state_.viewport.selected_vertex,static_cast<u32>(mesh_edit_geometry(state_,blueprint)->size()-1));
    remember(std::move(*before));publish(changes,true);return true;
}
content::Result<bool> EditingSession::apply_mesh(BlueprintId blueprint) {
    if (auto ready = available(); !ready) return std::unexpected(ready.error());
    if (!has_mesh_draft(state_,blueprint)) return false;
    auto candidate = state_;
    auto applied = apply_mesh_draft(candidate, blueprint);
    if (!applied) return std::unexpected(applied.error());
    return replace(std::move(candidate), {.full = true});
}
content::Result<void> EditingSession::begin_mesh_transform(BlueprintId blueprint) {
    if(!is_mesh_blueprint(state_,blueprint)) {
        content::Diagnostic error;error.message="Missing mesh blueprint";return std::unexpected(error);
    }
    DocumentChanges scope;scope.mesh_placements.insert(static_cast<u32>(blueprint));
    if(auto result=begin(EditGesture::mesh_transform,scope);!result)return result;
    gesture_->blueprint=blueprint;return {};
}
content::Result<bool> EditingSession::mesh_transform(Mat4 matrix) {
    if(!active(EditGesture::mesh_transform))return false;
    if(auto ready=writable();!ready)return std::unexpected(ready.error());
    const auto id=gesture_->blueprint;
    if(matrix==mesh_placement(state_,id,true))return false;
    // Validate before touching state. Capture/undo contains only the two matrices.
    auto valid=example::mesh_frame::inverse(matrix);
    if(!valid)return std::unexpected(valid.error());
    auto before=capture(gesture_->before.scope);if(!before)return std::unexpected(before.error());
    const auto& original=std::get<DocumentPatch>(gesture_->before.value).mesh_placements.at(id);
    const auto origin=original ? original->draft.value_or(original->applied) : Mat4::identity();
    if(matrix==origin) {
        if(original)state_.document.mesh_placements.insert_or_assign(id,*original);
        else state_.document.mesh_placements.erase(id);
    } else state_.document.mesh_placements[id].draft=matrix;
    return updated(*before);
}
content::Result<void> EditingSession::begin_mesh_draft_edit(BlueprintId blueprint) {
    if (!is_mesh_blueprint(state_, blueprint)) {
        content::Diagnostic error; error.message = "Missing mesh blueprint";
        return std::unexpected(std::move(error));
    }
    DocumentChanges scope;
    scope.meshes[static_cast<u32>(blueprint)].whole = true;
    if (auto started = begin(EditGesture::mesh_draft, scope); !started) return started;
    gesture_->blueprint = blueprint;
    return {};
}
content::Result<bool> EditingSession::preview_mesh_draft(u64 revision, editor::EditableMesh mesh) {
    if (!active(EditGesture::mesh_draft) || revision != state_.document.revision) {
        content::Diagnostic error; error.message = "Stale blueprint gesture result";
        return std::unexpected(std::move(error));
    }
    if (auto ready = writable(); !ready) return std::unexpected(ready.error());
    const auto id = gesture_->blueprint;
    auto changes = editor::mesh_changes(mesh_edit_geometry(state_, id)->document(), mesh.document());
    if (changes.empty()) return false;
    auto& original = std::get<DocumentPatch>(gesture_->before.value).meshes.front();
    gesture_->different = mesh.document() != *original.values.replacement;
    gesture_->mesh_changes.merge(changes);
    if (!gesture_->different && !original.present) state_.document.mesh_drafts.erase(id);
    else state_.document.mesh_drafts.insert_or_assign(id, std::move(mesh));
    DocumentChanges notice;
    notice.meshes.emplace(static_cast<u32>(id), std::move(changes));
    publish(notice);
    return true;
}
content::Result<void> EditingSession::compact_mesh_gesture() {
    if (!active(EditGesture::mesh_draft)) return {};
    // Keep one original snapshot while dragging, then retain only touched values
    // for both history and compensating cancellation. No full-scene snapshots.
    auto& before = gesture_->before;
    auto& mesh = std::get<DocumentPatch>(before.value).meshes.front();
    if (!mesh.values.replacement) return {}; // Already compacted before a failed restore.
    auto values = editor::capture_mesh_patch(*mesh.values.replacement, gesture_->mesh_changes);
    if (!values) return std::unexpected(values.error());
    mesh.values = std::move(*values);
    before.scope.meshes[static_cast<u32>(gesture_->blueprint)] = gesture_->mesh_changes;
    return {};
}
content::Result<bool> EditingSession::mesh_operation(BlueprintId blueprint, MeshOperation operation,
    std::span<const u32> vertices, std::span<const gfx::Edge> edges,MeshOperationSettings settings) {
    if (auto ready = available(); !ready) return std::unexpected(ready.error());
    auto candidate = state_;
    auto draft = begin_mesh_draft(candidate, blueprint);
    if (!draft) return std::unexpected(draft.error());
    auto* mesh = mesh_edit_geometry(candidate, blueprint);
    auto original=*mesh;
    DocumentChanges changes{.full = *draft || operation != MeshOperation::align};
    auto changed=perform_mesh_operation(*mesh,operation,vertices,edges,settings);
    if(!changed)return std::unexpected(changed.error());
    if(mesh->document()==original.document())return false;
    if(operation==MeshOperation::align)
        changes.vertices[static_cast<u32>(blueprint)].insert(changed->begin(),changed->end());
    auto result=replace(std::move(candidate), changes);
    if(result && *result)mesh_adjustment_.emplace(MeshAdjustment{blueprint,operation,std::move(original),
        {vertices.begin(),vertices.end()},{edges.begin(),edges.end()},settings,state_.document.revision});
    return result;
}
bool EditingSession::can_adjust_mesh_operation(u64 revision) const {
    return !busy() && mesh_adjustment_ && mesh_adjustment_->revision==revision &&
        state_.document.revision==revision && !undo_.empty();
}
content::Result<bool> EditingSession::adjust_mesh_operation(u64 revision,MeshOperationSettings settings) {
    if(auto ready=available();!ready)return std::unexpected(ready.error());
    if(!can_adjust_mesh_operation(revision)) {
        content::Diagnostic error;error.message="That operation is no longer the latest edit";
        return std::unexpected(std::move(error));
    }
    auto& operation=*mesh_adjustment_;
    auto mesh=operation.before;
    auto changed=perform_mesh_operation(mesh,operation.operation,operation.vertices,operation.edges,settings);
    if(!changed)return std::unexpected(changed.error());
    auto* target=mesh_edit_geometry(state_,operation.blueprint);
    if(mesh.document()==target->document()) {operation.settings=settings;return false;}
    *target=std::move(mesh);
    // Keep the original undo checkpoint. Saving between adjustments must still
    // become dirty, so each accepted parameter change gets a fresh content ID.
    content_=next_content_++;redo_.clear();
    publish(undo_.back().scope,true);
    operation.settings=settings;operation.revision=state_.document.revision;
    return true;
}
content::Result<bool> EditingSession::translate_vertices(BlueprintId blueprint, std::span<const u32> vertices, Vec3 delta) {
    if (auto begun = begin_vertices(blueprint, vertices); !begun) return std::unexpected(begun.error());
    if (auto moved = move_vertices(delta); !moved) {
        auto cancelled = cancel();
        return cancelled ? content::Result<bool>{std::unexpected(moved.error())} : cancelled;
    }
    return commit();
}
content::Result<bool> EditingSession::replace_animation(State candidate) {
    if (auto valid = validate_animation(candidate); !valid) return std::unexpected(valid.error());
    const auto changes = animation_changes(state_.document, candidate.document);
    return replace(std::move(candidate), changes);
}
content::Result<bool> EditingSession::add_keyframe(f32 time) {
    if (auto ready = available(); !ready) return std::unexpected(ready.error());
    auto candidate = state_;
    if (auto added = editor_example::add_keyframe(candidate, time); !added) return std::unexpected(added.error());
    candidate.viewport.time = time; candidate.viewport.paused = true;
    auto result = replace_animation(std::move(candidate));
    if (result) select_keyframe(time);
    return result;
}
content::Result<bool> EditingSession::update_keyframe(f32 from, f32 to, std::string name, std::span<const KeyframeValue> values) {
    if (auto ready = available(); !ready) return std::unexpected(ready.error());
    auto candidate = state_;
    if (auto edited = editor_example::update_keyframe(candidate, from, to, std::move(name), values); !edited)
        return std::unexpected(edited.error());
    candidate.viewport.time = to; candidate.viewport.paused = true;
    auto result = replace_animation(std::move(candidate));
    if (result) select_keyframe(to);
    return result;
}
content::Result<bool> EditingSession::apply_keyframe_range(f32 first, f32 last, std::span<const KeyframeChange> changes) {
    if (auto ready = available(); !ready) return std::unexpected(ready.error());
    if (!can_edit_scene_pose()) {
        content::Diagnostic error;
        error.message = "Select a keyframe at the playhead to apply a range edit";
        return std::unexpected(std::move(error));
    }
    auto candidate = state_;
    if (auto applied = editor_example::apply_keyframe_range(candidate, first, last, changes); !applied)
        return std::unexpected(applied.error());
    return replace_animation(std::move(candidate));
}
content::Result<bool> EditingSession::erase_keyframes(std::span<const f32> times) {
    if (auto ready = available(); !ready) return std::unexpected(ready.error());
    if (times.empty()) return false;
    auto candidate = state_;
    std::set<f32> unique(times.begin(), times.end());
    unique.erase(0); // Batch deletion preserves the permanent initial pose.
    if (unique.empty()) return false;
    for (auto time : unique)
        if (auto erased = erase_keyframe(candidate, time); !erased) return std::unexpected(erased.error());
    return replace_animation(std::move(candidate));
}
content::Result<bool> EditingSession::duration(f32 time) {
    if (auto ready = available(); !ready) return std::unexpected(ready.error());
    auto candidate = state_;
    candidate.document.timeline_duration = time;
    candidate.viewport.time = std::min(candidate.viewport.time, time);
    return replace_animation(std::move(candidate));
}
content::Result<void> EditingSession::copy_instances(std::span<const u32> objects) { return clipboard_.copy_instances(state_, objects); }
content::Result<void> EditingSession::copy_keyframes(std::span<const f32> times) { return clipboard_.copy_keyframes(state_, times); }
content::Result<EditClipboard::Pasted> EditingSession::paste() {
    if (auto ready = available(); !ready) return std::unexpected(ready.error());
    auto candidate = state_;
    auto pasted = clipboard_.paste(candidate);
    if (!pasted) return std::unexpected(pasted.error());
    const auto changes = pasted->object ? DocumentChanges{.full = true} : animation_changes(state_.document, candidate.document);
    if (auto adopted = replace(std::move(candidate), changes); !adopted) return std::unexpected(adopted.error());
    return pasted;
}

content::Result<bool> EditingSession::set_camera(u32 id, const CameraPose& pose) {
    if (auto ready = available(); !ready) return std::unexpected(ready.error());
    if (!can_edit_scene_pose()) {
        content::Diagnostic error;
        error.message = "Select a keyframe at the playhead, or insert one, to save a camera";
        return std::unexpected(std::move(error));
    }
    const auto* camera = find_instance(state_, id);
    if (!camera || !std::holds_alternative<CameraSettings>(camera->settings)) {
        content::Diagnostic error;
        error.message = "Only a scene camera can take the editor view";
        return std::unexpected(std::move(error));
    }
    DocumentChanges changes;
    for (const auto property : {"position", "rotation", "zoom", "focus"}) changes.properties.insert({id, property});
    auto before = capture(changes);
    if (!before) return std::unexpected(before.error());
    auto placed = evaluate_instance(state_, *camera, state_.viewport.time);
    const auto lens_before = std::get<CameraSettings>(placed.settings);
    place_camera(placed, pose);
    const auto& lens = std::get<CameraSettings>(placed.settings);
    const auto fail = [&](std::string message) -> content::Result<bool> {
        if (auto restored = restore(*before, false); !restored) return std::unexpected(restored.error());
        content::Diagnostic error;
        error.message = std::move(message);
        return std::unexpected(std::move(error));
    };
    auto moved = apply_position_value(state_, id, placed.transform.position);
    if (!moved) return fail(moved.error().message);
    auto turned = apply_rotation_value(state_, id, placed.transform.rotation);
    if (!turned) return fail(turned.error().message);
    DocumentChanges lens_changes;
    if (auto adjusted = apply_lens(state_, lens_changes, id, lens_before, lens); !adjusted) return fail(adjusted.error().message);
    // Saving a camera away from time zero keys four properties; it may not grow
    // the timeline past the user's budget any more than other pose edits may.
    if (auto capacity = check_track_growth(*before); !capacity) return fail(capacity.error().message);
    if (!*moved) changes.properties.erase({id, "position"});
    if (!*turned) changes.properties.erase({id, "rotation"});
    for (const auto property : {"zoom", "focus"})
        if (!lens_changes.properties.contains({id, property})) changes.properties.erase({id, property});
    if (changes.properties.empty()) return false;
    before->scope = changes;
    std::erase_if(std::get<DocumentPatch>(before->value).properties,
        [&](const auto& p) { return !changes.properties.contains(p.target); });
    remember(std::move(*before));
    publish(changes);
    return true;
}
content::Result<bool> EditingSession::set_active_camera(u32 id) {
    if (auto ready = available(); !ready) return std::unexpected(ready.error());
    if (!can_edit_scene_pose()) {
        content::Diagnostic error;
        error.message = "Select a keyframe at the playhead, or insert one, to choose the active camera";
        return std::unexpected(std::move(error));
    }
    DocumentChanges changes;
    for (const auto& instance : state_.document.instances)
        if (std::holds_alternative<CameraSettings>(instance.settings)) changes.properties.insert({instance.id, "active"});
    auto before = capture(changes);
    if (!before) return std::unexpected(before.error());
    DocumentChanges applied;
    if (auto activated = apply_active_camera(state_, applied, id); !activated) {
        if (auto restored = restore(*before, false); !restored) return std::unexpected(restored.error());
        content::Diagnostic error;
        error.message = activated.error().message;
        return std::unexpected(std::move(error));
    }
    if (auto capacity = check_track_growth(*before); !capacity) {
        if (auto restored = restore(*before, false); !restored) return std::unexpected(restored.error());
        return std::unexpected(capacity.error());
    }
    if (applied.properties.empty()) return false;
    before->scope = applied;
    std::erase_if(std::get<DocumentPatch>(before->value).properties,
        [&](const auto& p) { return !applied.properties.contains(p.target); });
    remember(std::move(*before));
    publish(applied);
    return true;
}
content::Result<bool> EditingSession::set_transform(u32 id, Vec3 rotation, f32 scale, std::optional<Vec3> axis_scale) {
    if (auto ready = available(); !ready) return std::unexpected(ready.error());
    if (!can_edit_scene_pose()) {
        content::Diagnostic error;
        error.message = "Select a keyframe at the playhead, or insert one, to edit scene properties";
        return std::unexpected(std::move(error));
    }
    DocumentChanges changes;
    changes.properties.insert({id, "scale"}); changes.properties.insert({id, "rotation"});
    if(axis_scale)changes.properties.insert({id,"axis_scale"});
    auto before = capture(changes);
    if (!before) return std::unexpected(before.error());
    auto rotated = apply_rotation_value(state_, id, rotation);
    auto scaled = rotated ? apply_scale_value(state_, id, scale)
                          : content::Result<bool>{std::unexpected(rotated.error())};
    auto axes=scaled && axis_scale ? apply_axis_scale_value(state_,id,*axis_scale) : content::Result<bool>{false};
    if (!scaled || !axes) {
        if (auto restored = restore(*before, false); !restored) return std::unexpected(restored.error());
        return std::unexpected(!scaled ? scaled.error() : axes.error());
    }
    if (auto capacity = check_track_growth(*before); !capacity) {
        if (auto restored = restore(*before, false); !restored) return std::unexpected(restored.error());
        return std::unexpected(capacity.error());
    }
    if (!*rotated && !*scaled && !*axes) return false;
    if (!*rotated) changes.properties.erase({id, "rotation"});
    if (!*scaled) changes.properties.erase({id, "scale"});
    if (!*axes) changes.properties.erase({id,"axis_scale"});
    // The inverse is already captured; restrict its scope to actual changes.
    before->scope = changes;
    std::erase_if(std::get<DocumentPatch>(before->value).properties,
        [&](const auto& p) { return !changes.properties.contains(p.target); });
    remember(std::move(*before));
    publish(changes);
    return true;
}
} // namespace editor_example
