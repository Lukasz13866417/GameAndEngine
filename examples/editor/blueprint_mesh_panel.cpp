#include "blueprint_mesh_panel.hpp"
#include "../support/mesh_frame.hpp"
#include <chrono>
#include <algorithm>
#include <cmath>

namespace editor_example {
using namespace vng;
BlueprintMeshPanel::BlueprintMeshPanel(ui::Container host,EditingSession& editing)
    :host_(host),hint_(host_.label("").height(56)),
      parts_host_(host_.column().padding(0)),
      editing_(editing),panel_(host_) {host_.visible(false);}
void BlueprintMeshPanel::sync() {
    const auto& state=editing_.state();
    const auto id=state.viewport.mode==ViewMode::mesh?std::optional{state.viewport.inspected_mesh}:std::nullopt;
    if(gesture_source_) {
        if(id==shown_ && editing_.active(EditGesture::mesh_draft))return;
        if(auto cancelled=finish_gesture(true);!cancelled)status_=cancelled.error().message;
    }
    if(id==shown_ && (editing_.active(EditGesture::mesh_draft) || editing_.active(EditGesture::mesh_transform)))return;
    if(id==shown_ && revision_==state.document.revision)return;
    if(id!=shown_)selected_part_=0;
    shown_=id;revision_=state.document.revision;
    description_.reset();gizmo_.reset();declared_parts_.clear();part_field_.clear();host_.visible(false);panel_.clear();
    if(parts_)parts_->remove();
    parts_.reset();parts_host_.visible(false);
    if(!id)return;
    const auto* mesh=mesh_edit_geometry(state,*id);if(!mesh)return;
    description_.emplace(static_cast<u32>(*id),1,revision_);
    // Undo may restore an older mesh without independently editable parts.
    auto described=describe_blueprint_mesh(*description_,*mesh,[this](MeshDraftEdit edit){start(std::move(edit));},selected_part_);
    if(!described){status_=described.error().message;hint_.text(status_);host_.visible(true);return;}
    gizmo_=std::move(described->gizmo);
    declared_parts_=described->parts;part_field_=std::move(described->part_field);
    if(!described->parts.empty()) {
        std::vector<ui::Choice<u32>> choices{{0,"Whole blueprint"}};
        bool present=selected_part_==0;
        for(const auto& part:described->parts){choices.push_back({part.id,part.label});present|=part.id==selected_part_;}
        if(!present)selected_part_=0;
        parts_.emplace(parts_host_.dropdown<u32>("Edit part",choices).value(selected_part_));
        parts_host_.visible(true);
    } else selected_part_=0;
    if(description_->schema().controls.empty())return;
    hint_.text(described->hint);
    panel_.show(description_->schema());host_.visible(true);
}
void BlueprintMeshPanel::enabled(bool value){host_.enabled(value&&!busy());}
std::optional<SurfaceMove> BlueprintMeshPanel::gizmo() const {
    if(!gizmo_ || !shown_)return {};
    auto surface=gizmo_->surface;
    surface.frame=example::mesh_frame::compose(mesh_placement(editing_.state(),*shown_,true),surface.frame);
    return surface;
}
bool BlueprintMeshPanel::select_part(u32 id) {
    if(busy() || editing_.busy() || selected_part_==id ||
       (id && std::ranges::find(declared_parts_,id,&MeshPart::id)==declared_parts_.end()))return false;
    selected_part_=id;revision_=0;sync();return true;
}
u32 BlueprintMeshPanel::pick_part(Vec2 p,const gfx::CameraSnapshot& camera) const {
    if(!shown_ || part_field_.empty() || busy() || editing_.busy() ||
       !std::isfinite(p.x)||!std::isfinite(p.y)||p.x<0||p.x>1||p.y<0||p.y>1)return 0;
    const auto* mesh=mesh_edit_geometry(editing_.state(),*shown_);if(!mesh)return 0;
    const auto& d=mesh->document();
    const auto field=std::ranges::find(d.vertex_fields,part_field_,&content::vmesh::VertexField::name);
    if(field==d.vertex_fields.end() || field->type!=content::vmesh::FieldType{content::vmesh::ScalarType::UInt32,1})return 0;
    const auto& owners=std::get<std::vector<u32>>(field->values);
    const double x=(2.*p.x-1)/camera.projection[0][0],y=(1-2.*p.y)/camera.projection[1][1];
    const bool perspective=camera.projection[3][3]==0;
    spatial::Ray3 ray;
    for(unsigned c=0;c<3;++c) {
        const auto offset=camera.right[c]*x+camera.up[c]*y;
        ray.origin[c]=camera.position[c]+(perspective?0:offset);
        ray.direction[c]=camera.forward[c]+(perspective?offset:0);
    }
    // Projection matrix encodes view-space near/far; ray's forward component is one.
    const auto a=camera.projection[2][2],b=camera.projection[3][2];
    ray.minimum=perspective?b/(a-1):(b+1)/a;
    ray.maximum=perspective?b/(a+1):(b-1)/a;
    const auto inverse=example::mesh_frame::inverse(mesh_placement(editing_.state(),*shown_,true));
    if(!inverse)return 0;
    const auto origin=example::mesh_frame::point(*inverse,{f32(ray.origin[0]),f32(ray.origin[1]),f32(ray.origin[2])});
    const auto direction=example::mesh_frame::vector(*inverse,{f32(ray.direction[0]),f32(ray.direction[1]),f32(ray.direction[2])});
    for(unsigned c=0;c<3;++c){ray.origin[c]=origin[c];ray.direction[c]=direction[c];}
    const auto hit=mesh->picking_index().intersect(ray);if(!hit)return 0;
    const auto face=d.faces[hit->triangle];const auto id=owners[face[0]];
    return owners[face[1]]==id && owners[face[2]]==id &&
        std::ranges::find(declared_parts_,id,&MeshPart::id)!=declared_parts_.end()?id:0;
}
void BlueprintMeshPanel::start(MeshDraftEdit edit) {
    if(busy() || !shown_ || editing_.busy())return;
    launch(std::move(edit));
}
void BlueprintMeshPanel::launch(MeshDraftEdit edit) {
    const auto& state=editing_.state();
    const auto* source=mesh_edit_geometry(state,*shown_);if(!source)return;
    job_blueprint_=*shown_;job_revision_=state.document.revision;
    job_gesture_=gesture_source_!=nullptr;discard_job_=false;
    job_select_new_part_=edit.select_new_part;
    try {
        auto snapshot=gesture_source_ ? gesture_source_ : std::make_shared<const editor::EditableMesh>(*source);
        job_=std::async(std::launch::async,[snapshot=std::move(snapshot),apply=std::move(edit.apply)]() -> content::Result<editor::EditableMesh> {
            try{return apply(*snapshot);}
            catch(const std::exception& e){content::Diagnostic d;d.message=e.what();return std::unexpected(std::move(d));}
        });
        status_="Updating "+edit.label+" in background...";
    } catch(const std::exception& e){status_=e.what();if(gesture_source_)(void)finish_gesture(true);}
}
content::Result<bool> BlueprintMeshPanel::finish_gesture(bool cancel) {
    queued_.reset();
    if(cancel)discard_job_=true; // Drain, never wait for a cancelled std::async job on the UI thread.
    auto result=editing_.active(EditGesture::mesh_draft) ? (cancel?editing_.cancel():editing_.commit()) : content::Result<bool>{false};
    if(!result)return result;
    pending_blueprint_=shown_;pending_revision_=editing_.state().document.revision;
    gesture_source_.reset();finishing_=false;revision_=0;
    return result;
}
content::Result<bool> BlueprintMeshPanel::edit_part(const SurfacePartAction& action) {
    if(action.cancelled) {
        auto result=finish_gesture(true);
        if(result)status_="Blueprint edit cancelled / original placement restored";
        sync();return result;
    }
    if(action.began) {
        if(busy() || !gizmo_ || !shown_)return false;
        auto source=std::make_shared<const editor::EditableMesh>(*mesh_edit_geometry(editing_.state(),*shown_));
        if(auto begun=editing_.begin_mesh_draft_edit(*shown_);!begun)return std::unexpected(begun.error());
        gesture_source_=std::move(source);finishing_=false;
    }
    if(!gesture_source_)return false;
    if(action.changed) {
        auto edit=action.rotation_degrees && gizmo_->rotate ? gizmo_->rotate(*action.rotation_degrees) : gizmo_->move(action.position);
        if(job_.valid())queued_=std::move(edit); // At most one running job and one latest target.
        else launch(std::move(edit));
    }
    finishing_|=action.finished;
    if(finishing_ && !job_.valid() && !queued_) {
        auto result=finish_gesture(false);sync();return result;
    }
    return false;
}
bool BlueprintMeshPanel::poll(bool accept_input) {
    bool changed{};
    if(gesture_source_ && !editing_.active(EditGesture::mesh_draft)) {
        if(auto cancelled=finish_gesture(true);!cancelled)status_=cancelled.error().message;
    }
    if(job_.valid() && job_.wait_for(std::chrono::seconds{0})==std::future_status::ready) {
        const auto old_parts=declared_parts_;
        const bool select_created=job_select_new_part_;
        auto mesh=job_.get();
        bool failed=discard_job_;
        if(discard_job_) { /* A cancelled job's immutable result is simply released. */ }
        else if(!mesh){status_=mesh.error().message;failed=true;}
        else if(shown_!=job_blueprint_ || editing_.state().viewport.mode!=ViewMode::mesh ||
            editing_.state().viewport.inspected_mesh!=job_blueprint_) {
            status_="Edit discarded: the inspected blueprint changed";failed=true;
        }
        else {
            auto adopted=job_gesture_ ? editing_.preview_mesh_draft(job_revision_,std::move(*mesh)) :
                editing_.replace_mesh_draft(job_blueprint_,job_revision_,std::move(*mesh));
            if(!adopted){status_=adopted.error().message;failed=true;}
            else {
                changed=*adopted;
                if(changed){pending_blueprint_=shown_;pending_revision_=editing_.state().document.revision;}
                status_=changed?"Blueprint draft updated / Apply mesh to scene to publish / Undo restores the draft":"Blueprint draft unchanged";
            }
        }
        if(gesture_source_) {
            if(failed) {
                auto cancelled=finish_gesture(true);
                if(!cancelled)status_=cancelled.error().message;
                else changed|=*cancelled;
            } else if(queued_) {
                auto next=std::move(*queued_);queued_.reset();launch(std::move(next));
            } else if(finishing_) {
                if(auto done=finish_gesture(false);!done)status_=done.error().message;
            }
        }
        // Re-show committed values on success and original values on rejection.
        if(!gesture_source_){
            revision_=0;sync();
            if(changed && !failed && select_created) {
                const auto created=std::ranges::find_if(declared_parts_,[&](const auto& part){
                    return std::ranges::find(old_parts,part.id,&MeshPart::id)==old_parts.end();
                });
                if(created!=declared_parts_.end()) {
                    const auto id=created->id;(void)select_part(id);
                }
            }
        }
    }
    if(accept_input && !busy() && description_) {
        if(parts_)if(const auto selected=parts_->changedValue()) {
            (void)select_part(*selected);return changed;
        }
        for(const auto& event:panel_.poll()) {
            const auto applied=description_->dispatch(event);
            if(!applied)status_=applied.error().message;
            if(busy())break;
        }
        if(!panel_.status().empty())status_=panel_.status();
    }
    return changed;
}
}
