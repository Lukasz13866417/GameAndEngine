#include "editing_session.hpp"
#include "animation.hpp"
#include "position_edits.hpp"
#include "rotation_edits.hpp"
#include "scale_edits.hpp"
#include "blueprint_gizmos.hpp"
#include "rotation_math.hpp"
#include <algorithm>
#include <limits>
#include <utility>

namespace editor_example {
namespace {
using namespace vng;
constexpr u64 max_revision = (u64{1} << 53) - 1;
auto invalid(std::string message) {
    content::Diagnostic error;
    error.message = std::move(message);
    return std::unexpected(std::move(error));
}
void rebase(DocumentPatch& patch, u64 revision) {
    patch.base_revision = revision;
    patch.revision = revision + 1;
    for (auto& edit : patch.vertices) {
        edit.base_revision = revision;
        edit.revision = revision + 1;
    }
}
bool same_values(DocumentPatch a, DocumentPatch b) {
    rebase(a, 1); rebase(b, 1);
    return a == b;
}
DocumentChanges property_scope(std::span<const u32> objects, std::string_view property) {
    DocumentChanges changes;
    for (auto id : objects) changes.properties.insert({id, std::string(property)});
    return changes;
}
} // namespace

EditingSession::EditingSession(State initial, SceneFile file)
    : state_(std::move(initial)), file_(std::move(file)) {}

bool EditingSession::can_edit_scene_pose() const {
    return selected_keyframe_ == std::optional{state_.viewport.time} &&
           editor_example::at_paused_keyframe(state_);
}

bool EditingSession::dirty() const noexcept {
    return content_ != saved_ || (gesture_ && gesture_->different);
}
bool EditingSession::busy() const noexcept { return gesture_.has_value() || remote_.has_value(); }
bool EditingSession::active(EditGesture kind) const noexcept { return gesture_ && gesture_->kind == kind; }
u32 EditingSession::active_object() const noexcept {
    return gesture_ && !gesture_->objects.empty() ? gesture_->objects.front() : 0;
}
bool EditingSession::can_undo() const noexcept { return !busy() && !undo_.empty(); }
bool EditingSession::can_redo() const noexcept { return !busy() && !redo_.empty(); }
std::optional<EditNotice> EditingSession::take_changes() { return std::exchange(notice_, {}); }
content::Result<void> EditingSession::timeline_track_limit(unsigned value) {
    if (!valid_timeline_track_limit(value))
        return invalid("Timeline track limit must be 1.." + std::to_string(timeline::max_tracks) + ".");
    track_limit_ = value;
    return {};
}
content::Result<void> EditingSession::instance_limit(unsigned value) {
    if (!valid_instance_limit(value))
        return invalid("Instance limit must be 1.." + std::to_string(max_scene_instances) + ".");
    instance_limit_ = value;
    return {};
}
content::Result<void> EditingSession::check_track_growth(std::size_t before, std::size_t after) const {
    if (after > before && after > track_limit_)
        return invalid("Timeline needs " + std::to_string(after) + " tracks; limit is " +
            std::to_string(track_limit_) + ". Increase Settings > Timeline track limit.");
    return {};
}
content::Result<void> EditingSession::check_track_growth(const Checkpoint& checkpoint) const {
    const auto after = state_.document.timeline.tracks().size();
    auto before = after;
    if (const auto* document = std::get_if<Document>(&checkpoint.value)) before = document->timeline.tracks().size();
    else for (const auto& property : std::get<DocumentPatch>(checkpoint.value).properties) {
        if (state_.document.timeline.find(property.target)) --before;
        if (property.track) ++before;
    }
    return check_track_growth(before, after);
}
content::Result<void> EditingSession::writable() const {
    // Reserve a revision for compensating cancellation of the last preview.
    if (!state_.document.revision || state_.document.revision >= max_revision - 1)
        return invalid("Document revision capacity exhausted");
    if (gesture_ && (state_.viewport.time != gesture_->before.bookmark.time ||
                    state_.viewport.paused != gesture_->before.bookmark.paused))
        return invalid("Finish or cancel the edit before changing playback time");
    if (gesture_ && !gesture_->before.scope.properties.empty() && !can_edit_scene_pose())
        return invalid("Keep the edited keyframe selected until the edit is finished or cancelled");
    return {};
}
content::Result<void> EditingSession::available() const {
    if (busy()) return invalid("Finish or cancel the active edit before starting another");
    return writable();
}

content::Result<EditingSession::Checkpoint> EditingSession::capture(const DocumentChanges& scope) const {
    const auto& view = state_.viewport;
    Checkpoint result{.value = DocumentPatch{}, .scope = scope, .drafts = {},
        .bookmark = {view.inspected_mesh, view.selected_object, view.selected_vertex,
                     view.time, view.paused, view.weld}, .content = content_};
    if (scope.full) result.value = state_.document;
    else {
        auto values = capture_patch(state_.document.revision, state_, scope);
        if (!values) return std::unexpected(values.error());
        result.value = std::move(*values);
        for (const auto& [id, vertices] : scope.vertices) {
            (void)vertices;
            const auto blueprint = static_cast<BlueprintId>(id);
            result.drafts.emplace(blueprint, state_.document.mesh_drafts.contains(blueprint));
        }
    }
    return result;
}
content::Result<bool> EditingSession::differs(const Checkpoint& before) const {
    if (before.scope.full) return invalid("Structural comparisons require an explicit operation result");
    auto current = capture_patch(state_.document.revision, state_, before.scope);
    if (!current) return std::unexpected(current.error());
    return !same_values(std::get<DocumentPatch>(before.value), std::move(*current));
}

content::Result<void> EditingSession::restore(const Checkpoint& before, bool restore_bookmark) {
    const auto revision = state_.document.revision;
    const auto next_instance = state_.document.next_instance_id;
    const auto next_blueprint = state_.document.next_blueprint_id;
    if (const auto* document = std::get_if<Document>(&before.value)) state_.document = *document;
    else {
        auto patch = std::get<DocumentPatch>(before.value);
        std::vector<BlueprintId> created;
        for (const auto& [blueprint, present] : before.drafts) {
            if (present && !state_.document.mesh_drafts.contains(blueprint)) {
                auto draft = begin_mesh_draft(state_, blueprint);
                if (!draft) {
                    for (auto id : created) state_.document.mesh_drafts.erase(id);
                    return std::unexpected(draft.error());
                }
                if (*draft) created.push_back(blueprint);
            }
            // Undoing first movement removes its draft, never writes published geometry.
            if (!present) std::erase_if(patch.vertices, [&](const auto& edit) {
                return edit.blueprint == static_cast<u32>(blueprint);
            });
        }
        rebase(patch, revision);
        if (auto restored = editor_example::apply_patch(state_, patch); !restored) {
            for (auto id : created) state_.document.mesh_drafts.erase(id);
            return restored;
        }
        for (const auto& [blueprint, present] : before.drafts)
            if (!present) state_.document.mesh_drafts.erase(blueprint);
    }
    // Internal restoration does not publish a revision; the owning transaction does.
    state_.document.revision = revision;
    state_.document.next_instance_id = std::max(next_instance, state_.document.next_instance_id);
    state_.document.next_blueprint_id = std::max(next_blueprint, state_.document.next_blueprint_id);
    if (restore_bookmark) {
        const auto& b = before.bookmark;
        auto& view = state_.viewport;
        view.inspected_mesh = b.blueprint;
        view.selected_object = b.object; view.selected_vertex = b.vertex;
        view.time = b.time; view.paused = b.paused; view.weld = b.weld;
        const auto* mesh = editable_mesh(state_);
        view.selected_vertex = mesh && mesh->size() ? std::min(view.selected_vertex, static_cast<u32>(mesh->size() - 1)) : 0;
        ++view.sequence;
    }
    return {};
}
void EditingSession::publish(const DocumentChanges& changes, bool discontinuous) {
    ++state_.document.revision;
    if (!notice_) notice_ = EditNotice{};
    notice_->revision = state_.document.revision;
    notice_->changes.merge(changes);
    notice_->discontinuous |= discontinuous;
}
void EditingSession::remember(Checkpoint before) {
    mesh_adjustment_.reset();
    if (undo_.size() == 64) undo_.pop_front();
    undo_.push_back(std::move(before));
    redo_.clear();
    content_ = next_content_++;
}
content::Result<bool> EditingSession::replace(State candidate, const DocumentChanges& changes) {
    if (auto ready = available(); !ready) return std::unexpected(ready.error());
    if (candidate.document.instances.size() > state_.document.instances.size() &&
        candidate.document.instances.size() > instance_limit_)
        return invalid("Scene needs " + std::to_string(candidate.document.instances.size()) +
            " instances; limit is " + std::to_string(instance_limit_) + ". Increase Settings > Instance limit.");
    if (auto capacity = check_track_growth(state_.document.timeline.tracks().size(), candidate.document.timeline.tracks().size()); !capacity)
        return std::unexpected(capacity.error());
    candidate.viewport.sequence = state_.viewport.sequence;
    if (candidate.viewport != state_.viewport) ++candidate.viewport.sequence;
    if (changes.empty()) {
        if (candidate.viewport != state_.viewport) {
            state_.viewport = candidate.viewport;
        }
        return false;
    }
    auto before = capture(changes);
    if (!before) return std::unexpected(before.error());
    candidate.document.revision = state_.document.revision;
    state_ = std::move(candidate);
    remember(std::move(*before));
    publish(changes, true);
    return true;
}
content::Result<bool> EditingSession::traverse(std::deque<Checkpoint>& from, std::deque<Checkpoint>& to) {
    if (auto ready = available(); !ready) return std::unexpected(ready.error());
    if (from.empty()) return false;
    mesh_adjustment_.reset();
    auto inverse = capture(from.back().scope);
    if (!inverse) return std::unexpected(inverse.error());
    auto changes = from.back().scope;
    if (inverse->drafts != from.back().drafts) changes = {.full = true};
    if (auto restored = restore(from.back(), true); !restored) return std::unexpected(restored.error());
    content_ = from.back().content;
    to.push_back(std::move(*inverse));
    from.pop_back();
    publish(changes, true);
    return true;
}
content::Result<bool> EditingSession::undo() { return traverse(undo_, redo_); }
content::Result<bool> EditingSession::redo() { return traverse(redo_, undo_); }

content::Result<void> EditingSession::begin(EditGesture kind, DocumentChanges scope, std::vector<u32> objects) {
    if (auto ready = available(); !ready) return ready;
    if (!scope.properties.empty() && !can_edit_scene_pose())
        return invalid("Select a keyframe at the playhead, or insert one, to edit scene properties");
    if (scope.empty()) return invalid("An edit gesture needs at least one target");
    auto before = capture(scope);
    if (!before) return std::unexpected(before.error());
    gesture_ = Gesture{kind, std::move(*before), std::move(objects), {}, {}, {}, false};
    for (const auto& property : std::get<DocumentPatch>(gesture_->before.value).properties)
        gesture_->sampled.push_back(state_.document.timeline.sample(property.target, state_.viewport.time).value_or(property.base));
    return {};
}
content::Result<void> EditingSession::begin_move(u32 primary, std::span<const u32> selection) {
    std::vector<u32> objects{primary};
    for (auto id : selection) if (std::ranges::find(objects, id) == objects.end()) objects.push_back(id);
    if (auto begun = begin(EditGesture::move, property_scope(objects, "position"), objects); !begun) return begun;
    for (auto id : objects)
        gesture_->origins.push_back(evaluate_instance(state_, *find_instance(state_, id), state_.viewport.time).transform.position);
    return {};
}
content::Result<void> EditingSession::begin_rotation(u32 primary, std::span<const u32> selection, TransformPivot pivot) {
    if(pivot.mode==PivotMode::custom&&(!std::isfinite(pivot.point.x)||!std::isfinite(pivot.point.y)||!std::isfinite(pivot.point.z)))
        return invalid("Rotation origin needs finite coordinates");
    std::vector<u32> objects{primary};
    for(auto id:selection) if(std::ranges::find(objects,id)==objects.end()) objects.push_back(id);
    auto scope=property_scope(objects,"rotation");
    for(auto id:objects)scope.properties.insert({id,"position"});
    if(auto begun=begin(EditGesture::rotation,std::move(scope),objects); !begun)return begun;
    gesture_->centers=instance_centers(state_,primary,selection);
    gesture_->pivot=pivot;
    if(pivot.mode!=PivotMode::custom)gesture_->pivot.point=selection_center(gesture_->centers);
    for(const auto& center:gesture_->centers)gesture_->origins.push_back(center.transform.rotation);
    return {};
}
content::Result<void> EditingSession::begin_attitude(u32 primary, std::span<const u32> selection, TransformPivot pivot) {
    std::vector<u32> objects{primary};
    for(auto id:selection) if(std::ranges::find(objects,id)==objects.end()) objects.push_back(id);
    std::vector<std::array<Vec3,3>> axes;
    for(auto id:objects) {
        const auto* instance=find_instance(state_,id);
        const auto basis=instance ? blueprint_attitude_axes(state_,instance->blueprint) : std::nullopt;
        if(!basis)return invalid("Yaw / pitch / roll requires forward and up axes on every selected blueprint");
        axes.push_back(*basis);
    }
    if(auto begun=begin_rotation(primary,selection,pivot); !begun)return begun;
    gesture_->attitude_axes=std::move(axes);
    return {};
}
content::Result<void> EditingSession::begin_scale(u32 primary, std::span<const u32> selection, bool with_axes) {
    std::vector<u32> objects{primary};
    for(auto id:selection) if(std::ranges::find(objects,id)==objects.end()) objects.push_back(id);
    auto scope=property_scope(objects,"scale");
    if(with_axes)for(auto id:objects)scope.properties.insert({id,"axis_scale"});
    if(auto begun=begin(EditGesture::scale,std::move(scope),objects); !begun)return begun;
    gesture_->sampled.clear();
    for(auto id:objects) gesture_->sampled.push_back(evaluate_instance(state_,*find_instance(state_,id),state_.viewport.time).transform.scale);
    if(with_axes)for(auto id:objects)gesture_->origins.push_back(evaluate_transform(state_,*find_instance(state_,id),state_.viewport.time).axis_scale);
    return {};
}
content::Result<void> EditingSession::begin_vertices(BlueprintId blueprint, std::span<const u32> vertices) {
    if (vertices.empty()) return invalid("Select vertices before beginning a mesh edit");
    DocumentChanges scope;
    scope.vertices[static_cast<u32>(blueprint)].insert(vertices.begin(), vertices.end());
    if (auto begun = begin(EditGesture::vertices, std::move(scope)); !begun) return begun;
    gesture_->blueprint = blueprint;
    return {};
}
content::Result<bool> EditingSession::updated(const Checkpoint& before_update) {
    if (auto capacity = check_track_growth(before_update); !capacity) {
        if (auto restored = restore(before_update, false); !restored) return std::unexpected(restored.error());
        return std::unexpected(capacity.error());
    }
    auto changed = differs(before_update);
    if (!changed || !*changed) return changed;
    auto different = differs(gesture_->before);
    if (!different) return std::unexpected(different.error());
    gesture_->different = *different;
    auto changes = gesture_->before.scope;
    for (const auto& [id, existed] : before_update.drafts)
        if (existed != state_.document.mesh_drafts.contains(id)) changes = {.full = true};
    publish(changes);
    return true;
}
content::Result<bool> EditingSession::move(Vec3 position) {
    if (!active(EditGesture::move)) return invalid("Begin a move gesture first");
    if (auto ready = writable(); !ready) return std::unexpected(ready.error());
    if (!gesture_->different && position == gesture_->origins.front()) return false;
    auto rollback = capture(gesture_->before.scope);
    if (!rollback) return std::unexpected(rollback.error());
    const auto p = gesture_->origins.front();
    if (position == p) {
        if (auto restored = restore(gesture_->before, false); !restored) return std::unexpected(restored.error());
        return updated(*rollback);
    }
    const Vec3 delta{position.x - p.x, position.y - p.y, position.z - p.z};
    for (std::size_t i = 0; i < gesture_->objects.size(); ++i) {
        const auto origin = gesture_->origins[i];
        auto moved = apply_position_value(state_, gesture_->objects[i],
            i == 0 ? position : Vec3{origin.x + delta.x, origin.y + delta.y, origin.z + delta.z});
        if (!moved) {
            if (auto restored = restore(*rollback, false); !restored) return std::unexpected(restored.error());
            return std::unexpected(moved.error());
        }
    }
    return updated(*rollback);
}
content::Result<bool> EditingSession::rotate(Vec3 rotation) {
    if (!active(EditGesture::rotation) || !gesture_->attitude_axes.empty()) return invalid("Begin an Euler rotation gesture first");
    std::vector<Vec3> values;
    const auto delta=rotation_math::multiply(rotation_math::matrix(rotation),rotation_math::transpose(rotation_math::matrix(gesture_->origins.front())));
    for(std::size_t i=0;i<gesture_->objects.size();++i) {
        auto value=rotation;
        if(i) {
            if(gesture_->pivot.mode==PivotMode::individual) {
                for(unsigned axis=0;axis<3;++axis)value[axis]=gesture_->origins[i][axis]+rotation[axis]-gesture_->origins.front()[axis];
                value=rotation_math::near(value,gesture_->origins[i]);
            } else value=rotation==gesture_->origins.front() ? gesture_->origins[i]
                : rotation_math::euler(rotation_math::multiply(delta,rotation_math::matrix(gesture_->origins[i])),gesture_->origins[i]);
        }
        values.push_back(value);
    }
    return rotations(values);
}
content::Result<bool> EditingSession::rotate_by(Vec3 delta) {
    if(!active(EditGesture::rotation) || !gesture_->attitude_axes.empty()) return invalid("Begin a rotation gesture first");
    if(!std::isfinite(delta.x)||!std::isfinite(delta.y)||!std::isfinite(delta.z)) return invalid("Rotation needs finite angles");
    std::vector<Vec3> values;
    const auto turn=rotation_math::matrix(delta);
    for(auto origin:gesture_->origins)
        values.push_back(delta==Vec3{} ? origin : rotation_math::euler(
            rotation_math::multiply(turn,rotation_math::matrix(origin)),origin));
    return rotations(values);
}
content::Result<bool> EditingSession::attitude(u32 axis, f64 degrees) {
    if(!active(EditGesture::rotation) || gesture_->attitude_axes.empty())return invalid("Begin a yaw / pitch / roll gesture first");
    if(axis>=3 || !std::isfinite(degrees))return invalid("Attitude needs a valid axis and finite angle");
    std::vector<Vec3> values;
    const auto primary=rotation_math::turn(gesture_->origins.front(),gesture_->attitude_axes.front()[axis],degrees);
    const auto delta=rotation_math::multiply(rotation_math::matrix(primary),rotation_math::transpose(rotation_math::matrix(gesture_->origins.front())));
    for(std::size_t i=0;i<gesture_->objects.size();++i) {
        values.push_back(std::remainder(degrees,360.)==0 ? gesture_->origins[i]
            : gesture_->pivot.mode==PivotMode::individual ? rotation_math::turn(gesture_->origins[i],gesture_->attitude_axes[i][axis],degrees)
            : rotation_math::euler(rotation_math::multiply(delta,rotation_math::matrix(gesture_->origins[i])),gesture_->origins[i]));
    }
    return rotations(values);
}
content::Result<bool> EditingSession::rotations(std::span<const Vec3> values) {
    if(auto ready=writable(); !ready)return std::unexpected(ready.error());
    auto before=capture(gesture_->before.scope); if(!before)return std::unexpected(before.error());
    const auto delta=rotation_math::multiply(rotation_math::matrix(values.front()),rotation_math::transpose(rotation_math::matrix(gesture_->origins.front())));
    if(std::ranges::equal(values,gesture_->origins)) {
        if(auto restored=restore(gesture_->before,false); !restored)return std::unexpected(restored.error());
    } else for(std::size_t i=0;i<gesture_->objects.size();++i) {
        const auto& center=gesture_->centers[i];
        Vec3 world=center.world;
        if(gesture_->pivot.mode!=PivotMode::individual) {
            Vec3 relative{};for(unsigned c=0;c<3;++c)relative[c]=world[c]-gesture_->pivot.point[c];
            relative=rotation_math::apply(delta,relative);
            for(unsigned c=0;c<3;++c)world[c]=gesture_->pivot.point[c]+relative[c];
        }
        auto local=center.local;for(unsigned c=0;c<3;++c)local[c]*=center.transform.axis_scale[c];
        const auto offset=rotation_math::direction(values[i],local);
        Vec3 position{};for(unsigned c=0;c<3;++c)position[c]=world[c]-offset[c]*center.transform.scale;
        auto changed=apply_rotation_value(state_,center.object,values[i]);
        if(changed && position!=center.transform.position)changed=apply_position_value(state_,center.object,position);
        // Restore the sampled position too when a later update returns this
        // object's center to its original position during the same gesture.
        else if(changed && evaluate_transform(state_,*find_instance(state_,center.object),state_.viewport.time).position!=position)
            changed=apply_position_value(state_,center.object,position);
        if(!changed) {
            if(auto restored=restore(*before,false); !restored)return std::unexpected(restored.error());
            return std::unexpected(changed.error());
        }
    }
    return updated(*before);
}
content::Result<bool> EditingSession::scale(f32 scale,std::optional<f32> tool_maximum) {
    if (!active(EditGesture::scale)) return invalid("Begin a scale gesture first");
    if (auto ready = writable(); !ready) return std::unexpected(ready.error());
    if(tool_maximum) {
        if(!std::isfinite(scale) || scale<=0 || !std::isfinite(*tool_maximum) ||
            *tool_maximum<1 || *tool_maximum>max_instance_scale)return invalid("Invalid scale tool value or limit");
        f32 minimum{},maximum=std::numeric_limits<f32>::max();
        for(const auto& sampled:gesture_->sampled) {
            const auto initial=std::get<f32>(sampled);
            minimum=std::max(minimum,min_instance_scale/initial);
            maximum=std::min(maximum,std::max(initial,*tool_maximum)/initial);
        }
        const auto primary=std::get<f32>(gesture_->sampled.front());
        scale=primary*std::clamp(scale/primary,minimum,maximum);
    }
    if (!gesture_->different && scale == std::get<f32>(gesture_->sampled.front())) return false;
    auto before = capture(gesture_->before.scope);
    if (!before) return std::unexpected(before.error());
    if (scale == std::get<f32>(gesture_->sampled.front())) {
        if (auto restored = restore(gesture_->before, false); !restored) return std::unexpected(restored.error());
    } else for(std::size_t i=0;i<gesture_->objects.size();++i) {
        const auto value=i ? std::get<f32>(gesture_->sampled[i])*scale/std::get<f32>(gesture_->sampled.front()) : scale;
        if(auto changed=apply_scale_value(state_,gesture_->objects[i],value); !changed) {
            if(auto restored=restore(*before,false); !restored)return std::unexpected(restored.error());
            return std::unexpected(changed.error());
        }
    }
    return updated(*before);
}
content::Result<bool> EditingSession::scale_factor(f32 factor,int axis,ScaleLimits limits) {
    if(!active(EditGesture::scale) || gesture_->origins.empty())return invalid("Begin a scale gesture with axis support first");
    if(!std::isfinite(factor) || factor<=0 || axis < -1 || axis>2)return invalid("Scale needs a positive factor and valid axis");
    if(!limits.valid())return invalid("Invalid scale tool limits");
    if(auto ready=writable(); !ready)return std::unexpected(ready.error());
    const auto low=axis<0 ? min_instance_scale : min_axis_scale;
    const auto high=axis<0 ? limits.instance : limits.axis;
    f32 minimum{},maximum=std::numeric_limits<f32>::max();
    for(std::size_t i=0;i<gesture_->objects.size();++i) {
        const auto initial=axis<0 ? std::get<f32>(gesture_->sampled[i]) : gesture_->origins[i][static_cast<unsigned>(axis)];
        minimum=std::max(minimum,low/initial);maximum=std::min(maximum,std::max(initial,high)/initial);
    }
    factor=std::clamp(factor,minimum,maximum);
    auto before=capture(gesture_->before.scope);if(!before)return std::unexpected(before.error());
    if(auto restored=restore(gesture_->before,false); !restored)return std::unexpected(restored.error());
    if(factor!=1)for(std::size_t i=0;i<gesture_->objects.size();++i) {
        content::Result<bool> changed;
        if(axis<0) changed=apply_scale_value(state_,gesture_->objects[i],
            std::clamp(std::get<f32>(gesture_->sampled[i])*factor,min_instance_scale,max_instance_scale));
        else {
            auto value=gesture_->origins[i];
            value[static_cast<unsigned>(axis)]=std::clamp(value[static_cast<unsigned>(axis)]*factor,min_axis_scale,max_axis_scale);
            changed=apply_axis_scale_value(state_,gesture_->objects[i],value);
        }
        if(!changed) {
            if(auto restored=restore(*before,false); !restored)return std::unexpected(restored.error());
            return std::unexpected(changed.error());
        }
    }
    return updated(*before);
}
content::Result<bool> EditingSession::move_vertices(Vec3 delta) {
    if (!active(EditGesture::vertices)) return invalid("Begin a vertex gesture first");
    auto positions=std::get<DocumentPatch>(gesture_->before.value).vertices.front().vertices;
    for(auto& vertex:positions)for(unsigned c=0;c<3;++c)vertex.position[c]+=delta[c];
    return vertices(positions);
}
content::Result<bool> EditingSession::vertices(std::span<const VertexPosition> positions) {
    if (!active(EditGesture::vertices)) return invalid("Begin a vertex gesture first");
    if (auto ready = writable(); !ready) return std::unexpected(ready.error());
    auto before = capture(gesture_->before.scope);
    if (!before) return std::unexpected(before.error());
    auto edit = std::get<DocumentPatch>(gesture_->before.value).vertices.front();
    if(positions.size()!=edit.vertices.size())return invalid("Vertex update must match the captured selection");
    std::set<u32> seen;
    for(const auto& vertex:positions) {
        if(!gesture_->before.scope.vertices.at(static_cast<u32>(gesture_->blueprint)).contains(vertex.index) || !seen.insert(vertex.index).second)
            return invalid("Vertex update must match the captured selection");
    }
    edit.vertices.assign(positions.begin(),positions.end());
    const auto* mesh = mesh_edit_geometry(state_, gesture_->blueprint);
    bool differs_now{};
    for (auto& vertex : edit.vertices) {
        differs_now |= vertex.position != mesh->position(vertex.index);
    }
    if (!differs_now) return false;
    // Validate positions before adopting a new draft; no failed edit leaves one behind.
    edit.base_revision = state_.document.revision; edit.revision = edit.base_revision + 1;
    auto draft = begin_mesh_draft(state_, gesture_->blueprint);
    if (!draft) return std::unexpected(draft.error());
    if (auto moved = apply_edit(state_, edit); !moved) {
        if (*draft) state_.document.mesh_drafts.erase(gesture_->blueprint);
        return std::unexpected(moved.error());
    }
    state_.document.revision = edit.base_revision;
    return updated(*before);
}
content::Result<bool> EditingSession::commit() {
    if (!gesture_) return false;
    if (auto compacted = compact_mesh_gesture(); !compacted) return std::unexpected(compacted.error());
    if (!gesture_->different) {
        // Returning to the starting coordinates is not a new draft or history entry.
        bool dropped{};
        for (const auto& [id, existed] : gesture_->before.drafts)
            if (!existed) dropped |= state_.document.mesh_drafts.erase(id)!=0;
        gesture_.reset();
        if (dropped) publish({.full = true});
        return false;
    }
    remember(std::move(gesture_->before));
    gesture_.reset();
    return true;
}
content::Result<void> EditingSession::begin_world_bounds() {
    return begin(EditGesture::world_bounds, {.world_bounds = true});
}
content::Result<bool> EditingSession::world_bounds(const WorldBounds& bounds) {
    if (!active(EditGesture::world_bounds)) return invalid("Begin a world bounds edit first");
    if (auto ready = writable(); !ready) return std::unexpected(ready.error());
    if (!valid_world_bounds(bounds)) return invalid("World bounds need finite, ordered corners");
    auto before = capture(gesture_->before.scope);
    if (!before) return std::unexpected(before.error());
    state_.document.world_bounds = bounds;
    return updated(*before);
}
content::Result<bool> EditingSession::cancel() {
    if (!gesture_) return false;
    if (auto compacted = compact_mesh_gesture(); !compacted) return std::unexpected(compacted.error());
    auto changes = gesture_->before.scope;
    bool changed = gesture_->different;
    for (const auto& [id, existed] : gesture_->before.drafts)
        if (existed != state_.document.mesh_drafts.contains(id)) { changes = {.full = true}; changed = true; }
    if (changed) {
        if (auto restored = restore(gesture_->before, false); !restored) return std::unexpected(restored.error());
        publish(changes);
    }
    gesture_.reset();
    return changed;
}

content::Result<void> EditingSession::begin_remote(u64 generation) {
    if (auto ready = available(); !ready) return ready;
    if (!can_edit_scene_pose()) return invalid("Select a keyframe at the playhead, or insert one, to edit scene properties");
    if (!generation) return invalid("A native edit requires a worker generation");
    remote_ = Remote{generation, state_.document.revision};
    return {};
}
content::Result<bool> EditingSession::accept_remote(u64 generation, const DocumentPatch& patch) {
    if (!remote_ || remote_->generation != generation || remote_->revision != patch.base_revision ||
        state_.document.revision != patch.base_revision || patch.revision != patch.base_revision + 1)
        return invalid("Stale or unsolicited native edit result");
    // Native inspector callbacks currently describe property edits, not topology
    // or mesh-draft publication. Those have explicit authoring operations.
    if (!patch.vertices.empty() || patch.duration || patch.world_bounds || !patch.regions.empty() || !patch.markers.empty())
        return invalid("Native inspector results may only change properties");
    auto changes = changes_of(patch);
    auto before = capture(changes);
    if (!before) return std::unexpected(before.error());
    if (auto applied = editor_example::apply_patch(state_, patch); !applied) return std::unexpected(applied.error());
    if (auto capacity = check_track_growth(*before); !capacity) {
        if (auto restored = restore(*before, false); !restored) return std::unexpected(restored.error());
        // The worker has already applied its callback at patch.revision. Send
        // a newer compensating revision so it accepts our restored snapshot;
        // no content identity or undo entry is created for the failed edit.
        publish(changes, true);
        return std::unexpected(capacity.error());
    }
    auto different = differs(*before);
    if (!different) return std::unexpected(different.error());
    state_.document.revision = patch.base_revision;
    if (*different) remember(std::move(*before));
    // Even a no-op callback has a protocol ACK revision, not a new content identity.
    publish(*different ? changes : DocumentChanges{});
    remote_.reset();
    return *different;
}

content::Result<void> EditingSession::load(const std::filesystem::path& path) {
    if (auto ready = available(); !ready) return ready;
    auto loaded = file_.load(path);
    if (!loaded) return std::unexpected(loaded.error());
    loaded->document.revision = state_.document.revision;
    loaded->viewport.sequence = state_.viewport.sequence + 1;
    state_ = std::move(*loaded);
    selected_keyframe_.reset();
    mesh_adjustment_.reset();
    undo_.clear(); redo_.clear(); clipboard_.clear();
    content_ = saved_ = next_content_++;
    publish({.full = true}, true);
    return {};
}
content::Result<void> EditingSession::save() {
    if (busy()) return invalid("Finish the current edit before saving");
    if (auto saved = file_.save(state_); !saved) return saved;
    saved_ = content_;
    return {};
}
content::Result<void> EditingSession::save_as(const std::filesystem::path& path, bool replace_existing) {
    if (busy()) return invalid("Finish the current edit before saving");
    if (auto saved = file_.save_as(path, state_, replace_existing); !saved) return saved;
    saved_ = content_;
    return {};
}
} // namespace editor_example
