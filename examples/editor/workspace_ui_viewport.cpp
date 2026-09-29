#include "workspace_ui.hpp"

namespace editor_example {
using namespace vng;

ViewportInputReply EditingWorkspaceUI::interact_viewport(const ViewportInputContext& context) {
    auto& tools=interaction_child();
    tools.begin_step();
    auto& blueprint=blueprint_panel_child();
    auto& gizmos=gizmo_selector_child();
    auto& pivot=rotation_pivot_child();
    const auto& state=editing_.state();
    auto& view=editing_.viewport();
    const auto& presented=context.presented;
    const auto& input=context.input;
    const auto& eligibility=context.tools;
    const auto& components=mesh_components();
    const auto selection=selected_instances().items();
    const auto viewport=presented.bounds;
    const auto snapshot=presented.camera.snapshot(presented.extent);
    ViewportInputReply reply;
    const auto previous_gizmo=gizmos.value();
    const auto error=[&](const content::Diagnostic& value){reply.status=value.message;};
    const auto finish_pose=[&](ViewportTool tool,bool cancel,std::string_view label) {
        auto result=finish(tools,tool,cancel);
        if(!result){error(result.error());return;}
        reply.pose_finished=true;
        reply.status=std::string(label)+(cancel?" cancelled / original values restored":*result?" updated / Undo restores the whole gesture":" unchanged");
        if(tool==ViewportTool::bounds)reply.bounds_changed=true;
        if(cancel) {
            if(tool==ViewportTool::translation)tools.translation.cancel();
            if(tool==ViewportTool::scale)tools.scale.cancel();
            if(tool==ViewportTool::bounds)tools.bounds.cancel();
        }
    };
    const auto finish_scale=[&](bool cancel) {
        if(!editing_.active(EditGesture::scale))return;
        const auto object=editing_.active_object();
        finish_pose(ViewportTool::scale,cancel,"Scale");
        if(cancel)if(const auto* instance=find_instance(state,object))
            reply.scale=ViewportInputReply::ScaleObservation{object,evaluate_instance(state,*instance,view.time).transform.scale,true};
    };
    const auto mesh_input=[&](MeshInput value) {
        auto result=dispatch(*this,workspace_situation(view),WorkspaceContext{.mesh=value}).mesh;
        merge(reply.mesh,std::move(result));
    };

    // Navigation is a sibling input consumer, not a replacement for an active
    // edit transaction. The manipulation remains captured while the view moves.
    reply.previous_camera=preview_camera_pose(state,view.time);
    reply.navigation_was_dragging=tools.camera_gizmo().dragging();
    const auto& walk=tools.camera_gizmo().walking();
    const auto mesh_origin=walk.active()?std::nullopt:mesh_camera_origin();
    const auto* instance=find_instance(state,view.selected_object);
    const bool free_object=view.mode==ViewMode::scene&&editing_.can_edit_scene_pose()&&!eligibility.components&&
        gizmos.value()==GizmoMode::free_rotate&&instance&&evaluate_visibility(state,*instance,view.time);
    const bool free_mesh=view.mode==ViewMode::mesh&&eligibility.components&&components.transform_mode()==GizmoMode::free_rotate&&
        (components.mode()==MeshSelectMode::whole||!components.selected().empty());
    reply.navigation=dispatch(tools,ViewportToolsUI::Navigate{},NavigationContext{.frame=NavigationFrame{
        .pose=reply.previous_camera,.mode=view.mode,.smooth_zoom=view.smooth_zoom,.viewport=viewport,
        .raw=input.raw,.unhandled=input.unhandled,.seconds=input.seconds,
        .drag_speeds=context.navigation.drag_speeds,.walk_speeds=context.navigation.walk_speeds,
        .move_forward=context.navigation.move_forward,.orbit_enabled=!(free_object||free_mesh||tools.regions.free_rotation_selected()),
        .keyboard_enabled=input.keyboard_enabled,.controls_have_focus=context.navigation.controls_have_focus,
        .origin=mesh_origin,.mesh=mesh_origin?editable_mesh(state):nullptr,
        .mesh_to_world=mesh_origin?editor_example::mesh_transform(state):Mat4::identity()},
        .enabled=eligibility.enabled&&context.navigation.enabled&&!context.navigation.numeric_active});
    view.smooth_zoom=reply.navigation.smooth_zoom;
    if(reply.navigation.changed)view.editor_camera=reply.navigation.pose;
    if(editing_.active(EditGesture::camera)&&
        (reply.navigation.cancelled||(!tools.camera_gizmo().dragging()&&!walk.moving()&&!context.navigation.numeric_active)))
        finish_pose(ViewportTool::navigation,reply.navigation.cancelled,"Camera");
    // Explicitly selecting a camera child borrows the viewport, not the scene
    // selection. Keep object captures and pickers from competing with its LMB.
    if(tools.object_tools_suspended())return reply;
    tools.gizmo_input.route(tools.transforming(),reply.navigation_was_dragging||tools.camera_gizmo().handledPointer(),
        input.tool_menu,input.raw.pointer,input.raw.events,input.unhandled,static_cast<float>(input.frame_seconds),
        eligibility.enabled&&input.raw.focused&&!walk.active()&&input.keyboard_enabled&&
        (tools.transforming()||viewport.contains(input.raw.pointer)),
        input.tick?GizmoControls::Phase::tick:GizmoControls::Phase::event);
    const auto raw=tools.gizmo_input.events(),available_input=tools.gizmo_input.unhandled();
    const auto arrow_step=tools.gizmo_input.arrow_step();
    reply.selected_gizmo_handle=tools.selected_handle();

    if(snapshot) {
        if(eligibility.enabled&&view.mode==ViewMode::mesh&&components.mode()==MeshSelectMode::surface&&blueprint.connecting())
            tools.mesh_sockets.update(blueprint.connection_slots(),*snapshot,viewport);
        else tools.mesh_sockets.clear();
        const bool allowed=tools.accepts(ViewportTool::mesh_part);
        const auto handles=blueprint.gizmo_handles();
        const auto action=tools.mesh_part.update(handles,blueprint.selected_handle(),*snapshot,viewport,
            allowed?available_input:std::span<const input::Event>{},allowed?raw:std::span<const input::Event>{},
            eligibility.enabled&&view.mode==ViewMode::mesh&&components.mode()!=MeshSelectMode::whole&&!eligibility.diagnostic&&!walk.active(),
            allowed&&!editing_.busy()&&!blueprint.busy(),arrow_step);
        tools.observe(ViewportTool::mesh_part,allowed);
        if(action.selected_handle)(void)blueprint.select_handle(*action.selected_handle);
        if(action.began||action.changed||action.finished||action.cancelled) {
            auto applied=blueprint.edit_part(action);
            if(applied)applied=apply_pending(blueprint);
            if(!applied){error(applied.error());tools.mesh_part.cancel();}
            else reply.geometry_changed|=*applied;
        }
        if(eligibility.enabled&&!tools.busy()&&!walk.active()&&input.shortcuts_enabled&&editing_.can_edit_scene_pose()&&
            view.mode==ViewMode::scene&&!eligibility.components)
            for(const auto& event:input.unhandled)if(viewport.contains(event.position))
                if(auto mode=gizmo_shortcut(event,gizmos.common());mode&&!gizmo_description(*mode).gesture){gizmos.value(*mode);reply.overlay_changed=true;}
        const bool instances_allowed=tools.accepts(ViewportTool::instances);
        auto transformed=execute(tools.instances,tools.instances.update(presented.generation,selection,pivot.value(),*snapshot,viewport,
            instances_allowed&&input.shortcuts_enabled?available_input:std::span<const input::Event>{},
            instances_allowed?raw:std::span<const input::Event>{},
            instances_allowed&&eligibility.enabled&&!walk.active()&&!tools.regions.component_editing()&&!eligibility.components&&
            view.mode==ViewMode::scene&&presented.time_current&&!eligibility.diagnostic&&!eligibility.worker_busy,arrow_step));
        tools.observe(ViewportTool::instances,instances_allowed);
        if(!transformed)error(transformed.error());
        else {
            if(transformed->began){gizmos.value(tools.instances.gizmo());reply.status="Move mouse / Click or Enter confirms / Escape or RMB cancels";}
            if(transformed->finished){reply.pose_finished=reply.overlay_changed=true;reply.status=transformed->cancelled?
                "Transform cancelled / original values restored":transformed->committed?"Transformed selection / Ctrl+Z undoes the whole gesture":"Transform unchanged";}
        }
    }

    if(auto mode=tools.regions.take_mode()){gizmos.value(*mode);reply.overlay_changed=true;}
    tools.selected(view.selected_object,gizmos.value());
    const bool region_allowed=tools.accepts(ViewportTool::boundary);
    tools.regions.update(editing_,snapshot?*snapshot:gfx::CameraSnapshot{},viewport,
        eligibility.toolbar_blocked||!input.shortcuts_enabled?std::span<const input::Event>{}:available_input,
        eligibility.toolbar_blocked?std::span<const input::Event>{}:raw,
        snapshot&&view.mode==ViewMode::scene&&eligibility.scene_aids_visible,
        region_allowed&&eligibility.ready&&!walk.active()&&view.paused&&(!editing_.busy()||editing_.active(EditGesture::region)),arrow_step,input.tick);
    if(auto applied=apply_pending(tools.regions);!applied)error(applied.error());
    tools.observe(ViewportTool::boundary,region_allowed);
    if(eligibility.enabled&&view.paused&&!tools.regions.handled()&&tools.accepts(ViewportTool::boundary))
        for(const auto& event:input.unhandled)
            if(event.kind==input::EventKind::pointer_down&&event.button==1&&viewport.contains(event.position)&&!event.modifiers.alt&&!event.modifiers.control) {
                (void)dispatch(*this,workspace_situation(view),WorkspaceContext{.tools=ToolPanelInput{.context={.close=true}}});
                tools.regions.open_menu(event.position,input.raw.logical_size,viewport);
            }
    if(auto selected=tools.regions.take_selection()) {
        const auto mode=tools.picked_selection(selected->modifiers);
        reply.selection=select_instance(selected->object,mode);
        gizmos.show(state,selected_instances().items());
        tools.translation.cancel();reply.overlay_changed=true;
    }
    if(auto message=tools.regions.take_message())reply.status=*message;
    if(view.show_regions!=tools.regions.boundaries_visible()||view.show_world_bounds!=eligibility.bounds_visible) {
        view.show_regions=tools.regions.boundaries_visible();view.show_world_bounds=eligibility.bounds_visible;reply.view_changed=true;
    }
    if(snapshot) {
        const bool allowed=tools.accepts(ViewportTool::bounds);
        const auto action=tools.bounds.update(state.document.world_bounds,*snapshot,viewport,available_input,raw,
            eligibility.bounds_visible&&view.mode==ViewMode::scene&&eligibility.scene_aids_visible,
            allowed&&eligibility.enabled&&view.paused&&(!editing_.busy()||editing_.active(EditGesture::world_bounds)));
        tools.observe(ViewportTool::bounds,allowed);
        if(action.cancelled&&editing_.active(EditGesture::world_bounds))finish_pose(ViewportTool::bounds,true,"World bounds");
        else {
            if(action.began)if(auto begun=editing_.begin_world_bounds();!begun){tools.bounds.cancel();error(begun.error());}
            if(editing_.active(EditGesture::world_bounds)&&(action.changed||action.finished)) {
                auto changed=editing_.world_bounds(action.value);
                if(!changed){finish_pose(ViewportTool::bounds,true,"World bounds");error(changed.error());}
                else {reply.bounds_changed|=*changed;if(action.finished)finish_pose(ViewportTool::bounds,false,"World bounds");}
            }
        }
    } else {if(editing_.active(EditGesture::world_bounds))finish_pose(ViewportTool::bounds,true,"World bounds");tools.bounds.cancel();}

    if(eligibility.enabled&&!walk.active()&&!editing_.active(EditGesture::mesh_draft)&&!editing_.active(EditGesture::mesh_transform)&&
        !editing_.active(EditGesture::vertices)&&eligibility.components&&view.mode==ViewMode::mesh&&input.shortcuts_enabled&&viewport.contains(input.raw.pointer))
        mesh_input({.shortcuts=input.unhandled});

    // Modal G/R/S owns its transaction until completion; passive handles must
    // not finish a sibling's edit while sharing its presentation.
    if(!tools.instances.active()&&!tools.instances.handled()) {
        if(snapshot&&(presented.schema||presented.generation)) {
            const editor::Schema* native{};
            if(presented.schema&&presented.schema->stamp.object==view.selected_object) {
                const auto control=std::ranges::find(presented.schema->controls,editor::Kind::translation_gizmo,&editor::Control::kind);
                if(control!=presented.schema->controls.end()&&control->key!="position")native=presented.schema;
            }
            instance=find_instance(state,view.selected_object);
            const bool scene_position=!native&&instance&&evaluate_visibility(state,*instance,view.time);
            const bool native_ready=presented.time_current&&presented.schema&&presented.revision==state.document.revision&&
                presented.schema->stamp.object==view.selected_object&&presented.schema->stamp.revision==state.document.revision&&presented.delivery_ready;
            const bool allowed=tools.accepts(ViewportTool::translation);
            const bool visible=(allowed||!tools.translation.dragging())&&translation_mode(gizmos.value())&&!eligibility.components&&
                eligibility.enabled&&editing_.can_edit_scene_pose()&&presented.time_current&&(scene_position||presented.revision>=presented.minimum_overlay_revision)&&
                view.mode==ViewMode::scene&&!eligibility.diagnostic&&!eligibility.worker_busy;
            const MoveGizmoContext movement{{view.selected_object,presented.generation,state.document.revision},*snapshot,viewport,
                allowed?available_input:std::span<const input::Event>{},allowed?raw:std::span<const input::Event>{},
                native?native_ready:!editing_.busy(),gizmos.value()!=GizmoMode::forward,arrow_step};
            SceneMoveGizmo::Reply action;
            if(!visible||(!native&&!scene_position))action=dispatch(tools.translation,SceneMoveGizmo::Inactive{},movement);
            else if(native)action=dispatch(tools.translation,SceneMoveGizmo::Native{*native},movement);
            else {
                const auto transform=evaluate_transform(state,*instance,view.time);
                std::vector<editor::TranslationAxis> axes;
                if(!tools.translation.dragging()&&std::ranges::find(gizmos.common(),GizmoMode::forward)!=gizmos.common().end())
                    if(auto forward=blueprint_manipulation(state,instance->blueprint).forward)
                        axes.push_back({"Forward / back",rotation_math::direction(transform.rotation,*forward)});
                action=dispatch(tools.translation,SceneMoveGizmo::Instance{{instance->id,transform.position},axes},movement);
            }
            tools.observe(ViewportTool::translation,allowed);
            if(action.cancelled&&editing_.active(EditGesture::move))finish_pose(ViewportTool::translation,true,"Move");
            reply.native=std::move(action.native);
            if(scene_position&&!editing_.active(EditGesture::move)&&action.began) {
                if(auto begun=editing_.begin_move(view.selected_object,selected_instances().items());!begun){error(begun.error());tools.translation.cancel();action.edit.reset();}
            }
            if(editing_.active(EditGesture::move)&&action.edit)
                if(auto moved=editing_.apply(*action.edit);!moved){finish_pose(ViewportTool::translation,true,"Move");error(moved.error());}
            if(editing_.active(EditGesture::move)&&action.finished)finish_pose(ViewportTool::translation,action.cancelled,"Move");
        } else tools.translation.cancel();
        if(editing_.active(EditGesture::move)&&!tools.translation.dragging())finish_pose(ViewportTool::translation,true,"Move");

        const bool show_pivot=rotation_mode(gizmos.value())&&view.mode==ViewMode::scene&&!eligibility.components&&view.selected_object;
        pivot.update(show_pivot?tools.rotation.center(selected_instances().items()):Vec3{},show_pivot,tools.busy(),input.tick);
        if(snapshot) {
            const bool allowed=tools.accepts(ViewportTool::pivot);
            pivot.update_tool(tools.pivot,*snapshot,viewport,allowed?available_input:std::span<const input::Event>{},
                allowed?raw:std::span<const input::Event>{},allowed&&eligibility.enabled&&show_pivot,arrow_step);
            tools.observe(ViewportTool::pivot,allowed);
            const bool rotate_allowed=tools.accepts(ViewportTool::rotation);
            const bool can_begin=presented.time_current&&presented.revision==state.document.revision&&presented.delivery_ready;
            auto action=execute(tools.rotation,tools.rotation.update(presented.generation,*snapshot,viewport,
                rotate_allowed&&can_begin?available_input:std::span<const input::Event>{},rotate_allowed?raw:std::span<const input::Event>{},
                (rotate_allowed||!tools.rotation.active())&&!pivot.moving()&&rotation_mode(gizmos.value())&&!eligibility.components&&eligibility.enabled&&
                view.mode==ViewMode::scene&&editing_.can_edit_scene_pose()&&presented.time_current&&presented.revision>=presented.minimum_overlay_revision&&
                !eligibility.diagnostic&&!eligibility.worker_busy,selected_instances().items(),gizmos.value()==GizmoMode::attitude,pivot.value(),
                gizmos.value()==GizmoMode::free_rotate,arrow_step));
            tools.observe(ViewportTool::rotation,rotate_allowed);
            if(!action){(void)execute(tools.rotation,tools.rotation.cancel());error(action.error());}
            else if(action->finished){reply.pose_finished=true;reply.status=action->cancelled?"Rotation cancelled / original transform restored":
                action->committed?"Rotated selection / Undo restores the whole drag":"Rotation unchanged";}
        } else if(auto cancelled=execute(tools.rotation,tools.rotation.cancel());!cancelled)error(cancelled.error());
        instance=find_instance(state,view.selected_object);
        if(instance&&snapshot) {
            const auto value=evaluate_instance(state,*instance,view.time);
            const bool current=presented.time_current&&presented.revision==state.document.revision&&presented.delivery_ready;
            const bool allowed=tools.accepts(ViewportTool::scale);
            const auto action=tools.scale.update({instance->id,presented.generation,state.document.revision},value.transform.position,value.transform.scale,
                *snapshot,viewport,allowed&&current?available_input:std::span<const input::Event>{},allowed?raw:std::span<const input::Event>{},
                (allowed||!tools.scale.dragging())&&gizmos.value()==GizmoMode::scale&&!eligibility.components&&eligibility.enabled&&
                view.mode==ViewMode::scene&&editing_.can_edit_scene_pose()&&presented.time_current&&!eligibility.diagnostic&&
                (!editing_.active(EditGesture::scale)||tools.scale.dragging())&&evaluate_visibility(state,*instance,view.time),{},arrow_step);
            tools.observe(ViewportTool::scale,allowed);
            if(action.cancelled)finish_scale(true);
            else if(action.began||action.changed||action.finished) {
                if(!editing_.active(EditGesture::scale)) {
                    auto begun=editing_.begin_scale(view.selected_object,selected_instances().items());
                    if(!begun){error(begun.error());tools.scale.cancel();}
                }
                if(editing_.active(EditGesture::scale)) {
                    auto edited=editing_.scale(action.value,tools.gizmo_input.scale_limits().instance);
                    if(!edited){finish_scale(true);error(edited.error());}
                    else {
                        if(tools.scale.dragging())reply.scale=ViewportInputReply::ScaleObservation{instance->id,evaluate_transform(state,*instance,view.time).scale,false};
                        if(action.finished)finish_scale(false);
                    }
                }
            }
        } else {finish_scale(true);tools.scale.cancel();}
    }
    if(snapshot) {
        const bool allowed=tools.accepts(ViewportTool::components);
        auto changed=execute(tools.mesh,tools.mesh.update(components,*snapshot,viewport,
            allowed&&input.shortcuts_enabled?available_input:std::span<const input::Event>{},allowed?raw:std::span<const input::Event>{},
            allowed&&eligibility.enabled&&!walk.active()&&eligibility.components&&view.mode==ViewMode::mesh&&view.paused&&
            !editing_.awaiting_remote()&&!blueprint.connecting()&&(components.mode()==MeshSelectMode::whole||!blueprint.gizmo()),arrow_step));
        mesh_input({.gizmo=tools.mesh.gizmo()});
        tools.observe(ViewportTool::components,allowed);
        if(!changed)error(changed.error());else reply.geometry_changed|=*changed;
    }
    reply.gizmo_changed=gizmos.value()!=previous_gizmo;
    if(reply.geometry_changed)refresh_vertex();
    if(reply.bounds_changed)sync_bounds();
    if(reply.scale)reset_inspector_scale(reply.scale->value);
    if(reply.pose_finished)refresh_after_pose();
    if(reply.overlay_changed)invalidate_gizmos();
    return reply;
}

void EditingWorkspaceUI::refresh_viewport_gizmos(const ViewportInputContext& context) {
    const auto& state=editing_.state();const auto& view=state.viewport;
    const auto& presented=context.presented;
    const auto snapshot=presented.camera.snapshot(presented.extent);
    if(!snapshot)return;
    auto& tools=interaction_child();auto& gizmos=gizmo_selector_child();auto& pivot=rotation_pivot_child();
    const auto* instance=find_instance(state,view.selected_object);
    const bool enabled=context.tools.enabled&&!context.tools.components&&editing_.can_edit_scene_pose()&&
        presented.time_current&&view.mode==ViewMode::scene&&!context.tools.diagnostic;
    const MoveGizmoContext movement{{view.selected_object,presented.generation,state.document.revision},*snapshot,presented.bounds,
        {},{},false,gizmos.value()!=GizmoMode::forward};
    if(instance&&evaluate_visibility(state,*instance,view.time)&&translation_mode(gizmos.value())&&enabled) {
        const auto transform=evaluate_transform(state,*instance,view.time);
        std::vector<editor::TranslationAxis> axes;
        if(std::ranges::find(gizmos.common(),GizmoMode::forward)!=gizmos.common().end())
            if(auto forward=blueprint_manipulation(state,instance->blueprint).forward)
                axes.push_back({"Forward / back",rotation_math::direction(transform.rotation,*forward)});
        (void)dispatch(tools.translation,SceneMoveGizmo::Instance{{instance->id,transform.position},axes},movement);
    } else (void)dispatch(tools.translation,SceneMoveGizmo::Inactive{},movement);
    (void)execute(tools.rotation,tools.rotation.update(presented.generation,*snapshot,presented.bounds,{},{},
        !pivot.moving()&&rotation_mode(gizmos.value())&&enabled&&presented.revision>=presented.minimum_overlay_revision,
        selected_instances().items(),gizmos.value()==GizmoMode::attitude,pivot.value(),gizmos.value()==GizmoMode::free_rotate));
    if(instance) {
        const auto value=evaluate_instance(state,*instance,view.time);
        (void)tools.scale.update({instance->id,presented.generation,state.document.revision},value.transform.position,value.transform.scale,
            *snapshot,presented.bounds,{},{},gizmos.value()==GizmoMode::scale&&enabled&&evaluate_visibility(state,*instance,view.time));
    } else tools.scale.cancel();
}

} // namespace editor_example
