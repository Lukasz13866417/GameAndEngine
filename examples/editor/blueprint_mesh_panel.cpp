#include "blueprint_mesh_panel.hpp"
#include "../support/mesh_frame.hpp"
#include <chrono>
#include <algorithm>
#include <cmath>

namespace editor_example {
using namespace vng;
BlueprintMeshPanel::BlueprintMeshPanel(ui::Container host,EditingSession& editing)
    :host_(host),hint_(host_.label("").height(56)),
      menu_buttons_host_(host.column().padding(0)),menu_body_(host.column().padding(0)),menu_panel_(menu_body_),
      parts_host_(host_.column().padding(0)),
      editing_(editing),panel_(host_) {host_.visible(false);}
void BlueprintMeshPanel::sync() {
    const auto& state=editing_.state();
    const auto id=state.viewport.mode==ViewMode::mesh?std::optional{state.viewport.inspected_mesh}:std::nullopt;
    if(setup_gesture_) {
        if(id==shown_&&revision_==state.document.revision)return;
        declared_gizmos_[selected_gizmo_].setup->finish(true);setup_gesture_=false;
    }
    if(gesture_source_) {
        if(id==shown_ && editing_.active(EditGesture::mesh_draft))return;
        if(auto cancelled=finish_gesture(true);!cancelled)status_=cancelled.error().message;
    }
    if(id==shown_ && (editing_.active(EditGesture::mesh_draft) || editing_.active(EditGesture::mesh_transform)))return;
    if(id==shown_ && revision_==state.document.revision)return;
    const auto previous_mode=has_gizmo()?std::string(title()):std::string{};
    ++options_revision_;
    cancel_placement();cancel_connection();
    if(id!=shown_){selected_part_={};selected_gizmo_=selected_handle_=0;selected_menu_.clear();}
    shown_=id;revision_=state.document.revision;
    description_.reset();gizmo_.reset();declared_parts_.clear();declared_gizmos_.clear();host_.visible(false);panel_.clear();
    menu_description_.reset();menu_panel_.clear();menu_body_.visible(false);
    for(auto& button:menu_buttons_)button.remove();
    menu_buttons_.clear();declared_menus_.clear();menu_buttons_host_.visible(false);
    if(parts_)parts_->remove();
    parts_.reset();parts_host_.visible(false);
    if(handles_)handles_->remove();
    handles_.reset();
    if(handles_host_)handles_host_->remove();
    handles_host_.reset();
    if(gizmos_)gizmos_->remove();
    gizmos_.reset();
    for(auto& button:placement_buttons_)button.remove();
    placement_buttons_.clear();declared_placements_.clear();
    for(auto& control:choice_controls_)control.remove();
    choice_controls_.clear();declared_choices_.clear();
    for(auto& button:connection_buttons_)button.remove();
    connection_buttons_.clear();declared_connections_.clear();
    if(cancel_connection_button_)cancel_connection_button_->remove();
    cancel_connection_button_.reset();
    if(!id)return;
    const auto* mesh=mesh_edit_geometry(state,*id);if(!mesh)return;
    description_.emplace(static_cast<u32>(*id),1,revision_);
    // Undo may restore an older mesh without independently editable parts.
    auto described=describe_blueprint_mesh(*description_,*mesh,[this](MeshDraftEdit edit){start(std::move(edit));},selected_part_);
    if(!described){status_=described.error().message;hint_.text(status_);host_.visible(true);return;}
    declared_gizmos_=std::move(described->gizmos);
    declared_menus_=std::move(described->menus);
    for(const auto& menu:declared_menus_)menu_buttons_.push_back(menu_buttons_host_.button(menu.label));
    menu_buttons_host_.visible(!declared_menus_.empty());show_menu();
    declared_placements_=std::move(described->placements);
    declared_choices_=std::move(described->choices);
    declared_connections_=std::move(described->connections);
    declared_parts_=described->parts;
    if(!described->parts.empty()) {
        std::vector<ui::Choice<MeshPartId>> choices{{{},"Whole blueprint"}};
        bool present=!selected_part_;
        for(const auto& part:described->parts){choices.push_back({part.id,part.label});present|=part.id==selected_part_;}
        if(!present){selected_part_={};selected_gizmo_=selected_handle_=0;}
        parts_.emplace(parts_host_.dropdown<MeshPartId>("Edit part",choices).value(selected_part_));
        parts_host_.visible(true);
    } else selected_part_={};
    if(!declared_gizmos_.empty()) {
        const auto previous=std::ranges::find(declared_gizmos_,previous_mode,&MeshPartGizmo::label);
        selected_gizmo_=previous==declared_gizmos_.end()?0:std::size_t(previous-declared_gizmos_.begin());
        for(auto& mode:declared_gizmos_)if(mode.setup)mode.handles=mode.setup->handles();
        if(declared_gizmos_.size()>1) {
            std::vector<ui::Choice<std::size_t>> choices;
            for(std::size_t i=0;i<declared_gizmos_.size();++i)choices.push_back({i,declared_gizmos_[i].label});
            gizmos_.emplace(parts_host_.dropdown<std::size_t>("Gizmo",choices).value(selected_gizmo_));
        }
        handles_host_.emplace(parts_host_.column().padding(0));
        show_handles();
    }
    for(const auto& choice:declared_choices_) {
        std::vector<ui::Choice<std::size_t>> options;
        for(std::size_t i=0;i<choice.options.size();++i)options.push_back({i,choice.options[i]});
        choice_controls_.push_back(parts_host_.dropdown<std::size_t>(choice.label,options).value(choice.selected));
    }
    for(const auto& connection:declared_connections_)
        connection_buttons_.push_back(parts_host_.button(connection.label).enabled(!connection.targets.empty()));
    if(!declared_connections_.empty())cancel_connection_button_.emplace(parts_host_.button("Cancel attachment").visible(false));
    for(const auto& placement:declared_placements_)placement_buttons_.push_back(parts_host_.button(placement.label));
    if(!declared_placements_.empty())parts_host_.visible(true);
    if(description_->schema().controls.empty())return;
    hint_.text(described->hint);
    panel_.show(description_->schema());host_.visible(true);
}
void BlueprintMeshPanel::enabled(bool value){host_.enabled(value&&!busy());}
void BlueprintMeshPanel::show_menu() {
    menu_description_.reset();menu_panel_.clear();menu_body_.visible(false);
    const auto menu=std::ranges::find(declared_menus_,selected_menu_,&MeshEditMenu::label);
    if(menu==declared_menus_.end()||!shown_)return;
    menu_description_.emplace(vng::editor::Stamp{static_cast<u32>(*shown_),1,++menu_revision_});
    menu->describe(*menu_description_,[this](MeshDraftEdit edit){start(std::move(edit));});
    menu_panel_.show(menu_description_->schema());menu_body_.visible(true);
}
void BlueprintMeshPanel::show_handles() {
    ++options_revision_;
    if(handles_)handles_->remove();
    handles_.reset();gizmo_.reset();
    const auto& mode=declared_gizmos_[selected_gizmo_];
    handles_host_->visible(mode.handles.size()>1);
    if(mode.handles.empty())return;
    selected_handle_=std::min(selected_handle_,mode.handles.size()-1);
    gizmo_=mode.handles[selected_handle_];
    if(mode.handles.size()>1) {
        std::vector<ui::Choice<std::size_t>> choices;
        for(std::size_t i=0;i<mode.handles.size();++i)choices.push_back({i,mode.handles[i].surface.label});
        handles_.emplace(handles_host_->dropdown<std::size_t>("Handle",choices).value(selected_handle_));
    }
}
bool BlueprintMeshPanel::has_gizmo() const {
    return shown_&&selected_part_&&!placement_&&!connection_&&selected_gizmo_<declared_gizmos_.size()&&
        editing_.state().viewport.mode==ViewMode::mesh&&editing_.state().viewport.inspected_mesh==shown_;
}
std::string_view BlueprintMeshPanel::title() const {
    if(has_gizmo())return declared_gizmos_[selected_gizmo_].label;
    return "Blueprint gizmo";
}
bool BlueprintMeshPanel::options_available() const {return has_gizmo()&&!busy()&&!editing_.busy();}
void BlueprintMeshPanel::describe_options(editor::Inspector& ui) {
    if(!options_available())return;
    auto& mode=declared_gizmos_[selected_gizmo_];
    const SubmitMeshDraftEdit submit=[this](MeshDraftEdit edit){start(std::move(edit));};
    if(mode.options)mode.options(ui,submit);
    if(mode.setup) {
        mode.setup->describe(ui,submit);
        // Menus can update setup coordinates without touching the document.
        // Pull these on the next panel poll, after the inspector has dispatched.
    }
    if(gizmo_&&gizmo_->erase)ui.action("erase_gizmo_handle",[this]{(void)erase_handle();},"Delete "+gizmo_->surface.label+" (Delete)");
}
bool BlueprintMeshPanel::erase_handle() {
    if(!options_available()||!gizmo_||!gizmo_->erase)return false;
    start(gizmo_->erase());return true;
}
std::optional<SurfaceMove> BlueprintMeshPanel::gizmo() const {
    if(!gizmo_ || !shown_ || placement_ || connection_)return {};
    auto surface=gizmo_->surface;
    surface.frame=example::mesh_frame::compose(mesh_placement(editing_.state(),*shown_,true),surface.frame);
    return surface;
}
std::vector<SurfaceMove> BlueprintMeshPanel::gizmo_handles() const {
    std::vector<SurfaceMove> result;
    if(!gizmo_||!shown_||placement_||connection_)return result;
    const auto frame=mesh_placement(editing_.state(),*shown_,true);
    for(const auto& handle:declared_gizmos_[selected_gizmo_].handles) {
        auto surface=handle.surface;
        surface.frame=example::mesh_frame::compose(frame,surface.frame);
        result.push_back(std::move(surface));
    }
    return result;
}
bool BlueprintMeshPanel::select_part(MeshPartId id) {
    if(busy() || editing_.busy() || selected_part_==id ||
       (id && std::ranges::find(declared_parts_,id,&MeshPart::id)==declared_parts_.end()))return false;
    selected_part_=std::move(id);declared_gizmos_.clear();selected_gizmo_=selected_handle_=0;revision_=0;sync();return true;
}
bool BlueprintMeshPanel::select_gizmo(std::size_t index) {
    if(busy()||editing_.busy()||index>=declared_gizmos_.size()||index==selected_gizmo_)return false;
    selected_gizmo_=index;selected_handle_=0;show_handles();
    if(gizmos_)gizmos_->value(index);
    return true;
}
bool BlueprintMeshPanel::cycle_gizmo(int direction) {
    const auto count=declared_gizmos_.size();
    if(direction==0 || count<2 || !has_gizmo())return false;
    return select_gizmo(direction>0 ? (selected_gizmo_+1)%count : (selected_gizmo_+count-1)%count);
}
bool BlueprintMeshPanel::select_handle(std::size_t index) {
    if(busy()||editing_.busy()||declared_gizmos_.empty()||index>=declared_gizmos_[selected_gizmo_].handles.size()||index==selected_handle_)return false;
    selected_handle_=index;gizmo_=declared_gizmos_[selected_gizmo_].handles[index];if(handles_)handles_->value(index);++options_revision_;return true;
}
bool BlueprintMeshPanel::begin_placement(std::size_t index) {
    if(busy()||editing_.busy()||index>=declared_placements_.size())return false;
    cancel_connection();placement_=declared_placements_[index];status_=placement_->label+": click Earth to place / Esc cancels";return true;
}
bool BlueprintMeshPanel::begin_connection(std::size_t index) {
    if(busy()||editing_.busy()||index>=declared_connections_.size()||declared_connections_[index].targets.empty())return false;
    cancel_placement();connection_=index;connection_target_.reset();
    if(cancel_connection_button_)cancel_connection_button_->visible(true);
    status_=declared_connections_[index].label+": click a structure, then a socket / Esc cancels";return true;
}
void BlueprintMeshPanel::cancel_connection() {
    if(connection_)status_="Attachment cancelled";
    connection_.reset();connection_target_.reset();
    if(cancel_connection_button_)cancel_connection_button_->visible(false);
}
bool BlueprintMeshPanel::pick_connection_target(Vec2 p,const gfx::CameraSnapshot& camera) {
    if(!connection_||busy()||editing_.busy())return false;
    const auto part=pick_part(p,camera);const auto& targets=declared_connections_[*connection_].targets;
    const auto found=std::ranges::find(targets,part,&MeshPartSocketTarget::part);
    if(found==targets.end()) {status_="Choose a visible structure with a free socket / Esc cancels";return false;}
    connection_target_=std::size_t(found-targets.begin());status_="Click a socket handle to attach / Esc cancels";return true;
}
std::vector<SurfaceMove> BlueprintMeshPanel::connection_slots() const {
    std::vector<SurfaceMove> result;
    if(!connection_||!connection_target_||!shown_)return result;
    const auto frame=mesh_placement(editing_.state(),*shown_,true);
    for(const auto& slot:declared_connections_[*connection_].targets[*connection_target_].slots) {
        auto marker=slot.marker;marker.frame=example::mesh_frame::compose(frame,marker.frame);result.push_back(std::move(marker));
    }
    return result;
}
bool BlueprintMeshPanel::attach_slot(std::size_t index) {
    if(!connection_||!connection_target_||busy()||editing_.busy())return false;
    const auto& slots=declared_connections_[*connection_].targets[*connection_target_].slots;
    if(index>=slots.size())return false;
    auto edit=slots[index].attach();cancel_connection();start(std::move(edit));return true;
}
void BlueprintMeshPanel::cancel_placement() {
    if(placement_)status_="Part placement cancelled";
    placement_.reset();
}
bool BlueprintMeshPanel::place_part(Vec2 p,const gfx::CameraSnapshot& camera) {
    if(!placement_||busy()||editing_.busy())return false;
    const auto picked=hit(p,camera);
    if(!picked){status_="Click the blueprint surface to place the part";return false;}
    const auto inverse=example::mesh_frame::inverse(placement_->frame);
    if(!inverse){status_=inverse.error().message;return false;}
    auto edit=placement_->place(example::mesh_frame::point(*inverse,picked->position));
    placement_.reset();start(std::move(edit));return true;
}
std::optional<BlueprintMeshPanel::Hit> BlueprintMeshPanel::hit(Vec2 p,const gfx::CameraSnapshot& camera) const {
    if(!shown_ || busy() || editing_.busy() ||
       !std::isfinite(p.x)||!std::isfinite(p.y)||p.x<0||p.x>1||p.y<0||p.y>1)return {};
    const auto* mesh=mesh_edit_geometry(editing_.state(),*shown_);if(!mesh)return {};
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
    if(!inverse)return {};
    const auto origin=example::mesh_frame::point(*inverse,{f32(ray.origin[0]),f32(ray.origin[1]),f32(ray.origin[2])});
    const auto direction=example::mesh_frame::vector(*inverse,{f32(ray.direction[0]),f32(ray.direction[1]),f32(ray.direction[2])});
    for(unsigned c=0;c<3;++c){ray.origin[c]=origin[c];ray.direction[c]=direction[c];}
    const auto hit=mesh->picking_index().intersect(ray);if(!hit)return {};
    return Hit{hit->triangle,{f32(ray.origin[0]+ray.direction[0]*hit->distance),f32(ray.origin[1]+ray.direction[1]*hit->distance),f32(ray.origin[2]+ray.direction[2]*hit->distance)}};
}
MeshPartId BlueprintMeshPanel::pick_part(Vec2 p,const gfx::CameraSnapshot& camera) const {
    const auto picked=hit(p,camera);if(!picked||declared_parts_.empty())return {};
    const auto& d=mesh_edit_geometry(editing_.state(),*shown_)->document();
    const auto face=d.faces[picked->face];
    for(const auto& field:d.vertex_fields)if(field.type==content::vmesh::FieldType{content::vmesh::ScalarType::UInt32,1}) {
        const auto& owners=std::get<std::vector<u32>>(field.values);const auto id=owners[face[0]];
        const MeshPartId key{field.name,id};
        if(id&&owners[face[1]]==id&&owners[face[2]]==id&&std::ranges::find(declared_parts_,key,&MeshPart::id)!=declared_parts_.end())return key;
    }
    return {};
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
    if(has_gizmo()&&declared_gizmos_[selected_gizmo_].setup) {
        auto& setup=declared_gizmos_[selected_gizmo_].setup;
        if(action.began&&!busy()&&!editing_.busy()){setup->begin();setup_gesture_=true;}
        if(!setup_gesture_)return false;
        if(action.changed)setup->move(selected_handle_,action.position);
        if(action.finished||action.cancelled) {
            setup->finish(action.cancelled);setup_gesture_=false;
            declared_gizmos_[selected_gizmo_].handles=setup->handles();show_handles();
        }
        return false;
    }
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
        auto edit=action.scale_value && gizmo_->scale ? gizmo_->scale(*action.scale_value) :
            action.rotation_degrees && gizmo_->rotate ? gizmo_->rotate(*action.rotation_degrees) : gizmo_->move(action.position);
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
    if(has_gizmo()&&!setup_gesture_)if(auto& mode=declared_gizmos_[selected_gizmo_];mode.setup) {
        const auto next=mode.setup->handles();
        const bool moved=next.size()!=mode.handles.size()||!std::equal(next.begin(),next.end(),mode.handles.begin(),
            [](const auto& a,const auto& b){return a.surface==b.surface;});
        if(moved){mode.handles=next;show_handles();}
    }
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
        for(std::size_t i=0;i<menu_buttons_.size();++i)if(menu_buttons_[i].clicked()) {
            selected_menu_=selected_menu_==declared_menus_[i].label?std::string{}:declared_menus_[i].label;
            show_menu();return changed;
        }
        if(menu_description_)for(const auto& event:menu_panel_.poll()) {
            const auto applied=menu_description_->dispatch(event);
            if(!applied)status_=applied.error().message;
            if(busy())return changed;
        }
        if(!menu_panel_.status().empty())status_=menu_panel_.status();
        if(cancel_connection_button_&&cancel_connection_button_->clicked()){cancel_connection();return changed;}
        for(std::size_t i=0;i<connection_buttons_.size();++i)if(connection_buttons_[i].clicked()){(void)begin_connection(i);return changed;}
        for(std::size_t i=0;i<choice_controls_.size();++i)if(const auto value=choice_controls_[i].changedValue()) {
            if(*value!=declared_choices_[i].selected)start(declared_choices_[i].choose(*value));
            return changed;
        }
        for(std::size_t i=0;i<placement_buttons_.size();++i)if(placement_buttons_[i].clicked()){(void)begin_placement(i);return changed;}
        if(gizmos_)if(const auto gizmo=gizmos_->changedValue()){(void)select_gizmo(*gizmo);return changed;}
        if(handles_)if(const auto handle=handles_->changedValue()){(void)select_handle(*handle);return changed;}
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
