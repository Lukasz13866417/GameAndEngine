#include "animation_gizmo.hpp"
#include "animation.hpp"

namespace editor_example {
using namespace vng;
AnimationGizmo::AnimationGizmo(ui::Container host):host_(host) {host_.visible(false);}
void AnimationGizmo::field(std::string_view name,float& value,float low,float high) {
    fields_.push_back({NumberControl{*fields_host_,name,low,high,value},&value});
}
void AnimationGizmo::vector_fields(std::string_view name,Vec3& value,float low,float high) {
    for(unsigned i=0;i<3;++i)field(std::string(name)+" "+"XYZ"[i],value[i],low,high);
}
void AnimationGizmo::create(const State& state,BlueprintId blueprint) {
    blueprint_=blueprint;creating_=true;preview_=false;target_=0;page_=Page::root;
    value_={};value_.interval={state.viewport.time,state.document.timeline_duration};
    if(blueprint==BlueprintId::spin)value_.root=SpinAnimation{};
    build(state,true);visible_=true;host_.visible(true);
}
void AnimationGizmo::build(const State& state,bool targets_only) {
    fields_.clear();fields_host_.reset();fields_slot_.reset();if(body_)body_->remove();
    body_=host_.column().padding(0).gap(4);targets_only_=targets_only;
    body_->label(creating_?"CREATE ANIMATION / preview before accepting":"ANIMATION INSTANCE / editable forest");
    {
        std::vector<ui::Choice<u32>> objects{{0,"Choose target"}},cameras{{0,"No camera"}};
        for(const auto& instance:state.document.instances) {
            if(is_animation_blueprint(instance.blueprint))continue;
            objects.push_back({instance.id,"#"+std::to_string(instance.id)+" / "+instance.name});
            if(is_camera_instance(state,instance.id))cameras.push_back({instance.id,instance.name});
        }
        object_.emplace(body_->dropdown<u32>("Target",objects));camera_.emplace(body_->dropdown<u32>("Camera",cameras));
        if(std::ranges::any_of(objects,[&](const auto& o){return o.value==state.viewport.selected_object;}))object_->value(state.viewport.selected_object);
        camera_->visible(blueprint_==BlueprintId::departure);
        if(auto* active=active_camera(state,state.viewport.time))camera_->value(active->id);
        if(!targets_only) {
            if(const auto* d=std::get_if<DepartureSequence>(&value_.root)){object_->value(d->ship);camera_->value(d->camera);}
            else object_->value(std::get<SpinAnimation>(value_.root).target);
        }
    }
    if(!targets_only) {
        enabled_=body_->checkbox("Enabled").value(value_.enabled);
        if(const auto* d=std::get_if<DepartureSequence>(&value_.root)) {
            body_->label("Ship #"+std::to_string(d->ship)+" / camera #"+std::to_string(d->camera));
            const std::array labels{"Departure / interval","  Ship motion / Route","    Speed profile","    Turbulence","  Camera follow"};
            for(unsigned i=0;i<nodes_.size();++i)nodes_[i]=body_->button(labels[i]).selected(i==static_cast<unsigned>(page_));
        } else body_->label("Spin target #"+std::to_string(std::get<SpinAnimation>(value_.root).target));
    }
    fields_slot_=body_->column().padding(0).gap(0);
    build_fields();
    auto actions=body_->row().padding(0).gap(4);
    apply_=actions.button(targets_only?"Preview animation":"Create animation").visible(creating_);
    cancel_=actions.button("Cancel").visible(creating_);
    bake_=body_->button("Bake / detach to keyframes").visible(!creating_);
    erase_=body_->button("Remove animation (restore underlying keys)").visible(!creating_);
    status_=body_->label("Underlying keyframes are preserved; controlled values use this tree.").height(42);
}
void AnimationGizmo::build_fields() {
    fields_.clear();if(fields_host_)fields_host_->remove();
    // Keep controls in one retained subtree; rebuild only when changing node.
    fields_host_=fields_slot_->column().padding(0).gap(3);
    if(page_==Page::root) {
        field("Start (seconds)",value_.interval.first,0,86400);
        field("End (seconds)",value_.interval.last,0,86400);
        if(!targets_only_)if(auto* spin=std::get_if<SpinAnimation>(&value_.root))
            vector_fields("Degrees / second",spin->degrees_per_second,-3600,3600);
    } else if(auto* d=std::get_if<DepartureSequence>(&value_.root)) {
        if(page_==Page::route) {
            orient_=fields_host_->checkbox("Orient ship along route").value(d->motion.orient_to_path);
            for(unsigned i=0;i<4;++i)vector_fields("Control point "+std::to_string(i),d->motion.route.points[i],-scene_coordinate_limit,scene_coordinate_limit);
        } else if(page_==Page::speed) {
            field("Start speed weight",d->motion.speed.initial,.001F,10000);
            field("End speed weight",d->motion.speed.final,.001F,10000);
            field("Acceleration fraction",d->motion.speed.ramp,.001F,1);
            fields_host_->label("Route length / duration sets overall speed.");
        } else if(page_==Page::turbulence) {
            field("Amplitude",d->motion.turbulence.amplitude,0,10000);
            field("Frequency",d->motion.turbulence.frequency,0,100);
        } else {
            look_=fields_host_->checkbox("Look at ship").value(d->follow.look_at_ship);
            vector_fields("World offset",d->follow.offset,-scene_coordinate_limit,scene_coordinate_limit);
            vector_fields("Camera rotation",d->follow.rotation,-360,360);
        }
    }
}
void AnimationGizmo::present(const State& state,bool enabled,bool creating) {
    const auto* a=scene_animation(state,state.viewport.selected_object);
    visible_=state.viewport.mode==ViewMode::scene&&(creating_||a);
    host_.visible(visible_).enabled(enabled);allowed_=enabled;
    if(!visible_)return;
    if(creating_&&!creating&&!preview_)return;
    if(!a){creating_=preview_=false;host_.visible(false);visible_=false;return;}
    const bool switched=target_!=state.viewport.selected_object||value_.root.index()!=a->root.index();
    if(switched) {
        target_=state.viewport.selected_object;value_=*a;blueprint_=find_instance(state,target_)->blueprint;
        if(!creating)creating_=false;
        preview_=creating;page_=Page::root;build(state,false);
    } else {
        value_=*a;
        if(const auto* d=std::get_if<DepartureSequence>(&a->root)){object_->value(d->ship);camera_->value(d->camera);}
        else object_->value(std::get<SpinAnimation>(a->root).target);
        for(auto& field:fields_)field.control.value(*field.value);
        enabled_.value(a->enabled);
        if(auto* d=std::get_if<DepartureSequence>(&value_.root)) {
            if(page_==Page::route)orient_.value(d->motion.orient_to_path);
            if(page_==Page::follow)look_.value(d->follow.look_at_ship);
        }
    }
}
AnimationGizmo::Reply AnimationGizmo::poll() {
    Reply reply;reply.blueprint=blueprint_;if(!visible_||!allowed_)return reply;
    if(!targets_only_&&std::holds_alternative<DepartureSequence>(value_.root))
        for(unsigned i=0;i<nodes_.size();++i)if(nodes_[i].clicked()&&page_!=static_cast<Page>(i)) {
            page_=static_cast<Page>(i);for(unsigned j=0;j<nodes_.size();++j)nodes_[j].selected(i==j);build_fields();return reply;
        }
    bool changed{},committed{},dragging{};
    for(auto& field:fields_) {
        field.control.poll();
        if(!field.control.status().empty())status_.text(field.control.status());
        if(auto next=field.control.changedValue()){*field.value=*next;changed=true;}
        committed|=field.control.editCommitted();dragging|=field.control.isPressed();
    }
    if(!targets_only_) {
        if(auto next=object_->changedValue()) {
            if(auto* d=std::get_if<DepartureSequence>(&value_.root))d->ship=*next;
            else std::get<SpinAnimation>(value_.root).target=*next;
            changed=committed=true;
        }
        if(auto next=camera_->changedValue())if(auto* d=std::get_if<DepartureSequence>(&value_.root)) {
            d->camera=*next;changed=committed=true;
        }
        if(auto next=enabled_.changedValue()){value_.enabled=*next;changed=committed=true;}
        if(auto* d=std::get_if<DepartureSequence>(&value_.root)) {
            if(page_==Page::route)if(auto next=orient_.changedValue()){d->motion.orient_to_path=*next;changed=committed=true;}
            if(page_==Page::follow)if(auto next=look_.changedValue()){d->follow.look_at_ship=*next;changed=committed=true;}
        }
    }
    if(cancel_.clicked())reply.action=Action::cancel;
    else if(apply_.clicked())reply.action=targets_only_?Action::preview:Action::finish;
    else if(!creating_&&bake_.clicked())reply.action=Action::bake;
    else if(!creating_&&erase_.clicked())reply.action=Action::erase;
    else if(!creating_&&was_dragging_&&!dragging&&!committed)reply.action=Action::cancel;
    else if(!targets_only_&&(changed||committed))reply.action=Action::edit;
    was_dragging_=dragging;reply.dragging=dragging;reply.value=value_;
    if(object_)reply.targets={object_->value(),camera_->value()};
    return reply;
}
void AnimationGizmo::accepted(bool close) {if(close){creating_=preview_=false;target_=0;}}
void AnimationGizmo::error(std::string_view text) {if(body_)status_.text(text);}
DebugReport AnimationGizmo::debug_report() const {
    return {.name="animation",.role="animation instance gizmo and child controls",.situation=creating_?"Creation draft":"Bound instance",
        .owned={{"target",std::to_string(target_)},{"visible",debug_bool(visible_)},{"tree",animation_debug_string(value_)}}};
}
}
