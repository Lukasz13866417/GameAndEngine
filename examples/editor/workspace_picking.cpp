#include "workspace.hpp"

namespace editor_example {
using namespace vng;

ViewportSelectionReply EditingWorkspace::pick_viewport(const ViewportSelectionContext& context) {
    const auto& c=context.frame;
    const auto& image=c.presented;
    auto& tools=interaction();
    auto& parts=blueprint_panel();
    auto& capture=viewport_->pick_capture_;
    auto& box=tools.selection_box;
    auto& input=tools.selection_input;
    const auto& components=mesh_components();
    const auto& current=state();
    auto& view=editing_.viewport();
    ViewportSelectionReply reply;
    const auto same_rect=[](ui::Rect a,ui::Rect b) {
        return a.x==b.x&&a.y==b.y&&a.width==b.width&&a.height==b.height;
    };
    if(!c.input.raw.focused || c.input.raw.overflow || (box.active() && (!capture ||
       !same_rect(capture->bounds,image.bounds) || capture->revision!=current.document.revision ||
       capture->time!=view.time || capture->mode!=view.mode || capture->blueprint!=view.inspected_mesh ||
       capture->components!=components.mode()))) {
        box.cancel();capture.reset();input.cancel();
    }
    if(!box.active())capture.reset();
    const auto mesh_input=[&](MeshInput request,std::optional<ToolPanelInput> panel={}) {
        merge(reply.mesh,route_viewport(InspectMesh{view.inspected_mesh},
            WorkspaceContext{.mesh=request,.tools=panel},false));
    };
    const auto observed_selection=[&](WorkspaceSelection::Change change) {
        if(reply.selection) {
            change.previous_active=reply.selection->previous_active;
            change.active_changed=change.previous_active!=change.active;
            change.changed|=reply.selection->changed;
        }
        reply.selection=change;
    };
    const auto begin_box=[&](const input::Event& event) {
        capture=ViewportPickCapture{selected_instances(),{},image.camera,image.extent,image.bounds,
            current.document.revision,view.time,view.mode,view.inspected_mesh,components.mode()};
        if(c.tools.components)capture->elements.assign(components.selected().begin(),components.selected().end());
        box.begin(event.position,image.bounds,event.modifiers);
        tools.selection_handled();
    };
    const auto accept_parts=[&] {
        if(auto applied=apply_pending(parts);!applied)reply.status=applied.error().message;
        else reply.blueprint_changed|=*applied;
    };
    const bool enabled=c.tools.enabled && c.input.raw.focused && !c.input.raw.overflow &&
        image.bounds.width>0 && image.bounds.height>0 && !image.extent.empty();
    const auto events=input.route(c.input.raw.events,c.input.unhandled,image.bounds,
        context.selected_gizmo_handle,enabled&&!components.menu_open()&&tools.accepts(ViewportTool::selection));
    for(const auto& event:events) {
        if(parts.connecting() && ((event.kind==input::EventKind::key_down&&event.key==input::Key::escape) ||
            (event.kind==input::EventKind::pointer_down&&event.button==1&&!event.modifiers.control&&!event.modifiers.shift))) {
            if(!input.available(event))continue;
            parts.cancel_connection();tools.mesh_sockets.clear();tools.selection_handled();continue;
        }
        if(parts.placing()&&event.kind==input::EventKind::key_down&&event.key==input::Key::escape&&input.available(event)) {
            parts.cancel_placement();tools.selection_handled();continue;
        }
        if(box.active()) {
            if(auto result=box.update(event);result&&capture) {
                const auto& origin=*capture;
                const ui::Rect area{(result->rect.x-origin.bounds.x)/origin.bounds.width,
                    (result->rect.y-origin.bounds.y)/origin.bounds.height,
                    result->rect.width/origin.bounds.width,result->rect.height/origin.bounds.height};
                if(!c.tools.components) {
                    std::vector<u32> hits;
                    for(const auto& point:tools.instance_projection.get(current,origin.extent,origin.camera))
                        if(point.point.x>=area.x&&point.point.x<=area.x+area.width&&
                           point.point.y>=area.y&&point.point.y<=area.y+area.height)hits.push_back(point.object);
                    const auto previous=view.selected_object;
                    const auto restored=restore_selection(origin.instances);
                    auto selected=select_instances(hits,result->mode);
                    selected.previous_active=previous;selected.active_changed=previous!=selected.active;
                    selected.changed|=restored.changed;
                    observed_selection(selected);
                } else if(const auto* mesh=editable_mesh(current)) {
                    const auto hits=components.box(current,area,origin.extent,origin.camera);
                    const std::array changes{MeshSelectionInput{origin.elements,editor::SelectionMode::replace},
                        MeshSelectionInput{hits,result->mode}};
                    mesh_input({.selection=changes});
                    const auto vertices=components.vertices(*mesh);
                    if(!vertices.empty())view.selected_vertex=vertices.back();
                    reply.vertex_changed=reply.viewport_changed=true;
                    reply.status=std::to_string(components.selected().size())+" mesh elements selected";
                }
            }
            if(!box.active())capture.reset();
            continue;
        }
        if(!tools.accepts(ViewportTool::selection))break;
        const bool selection_enabled=enabled&&!components.menu_open();
        if(event.kind==input::EventKind::key_down&&event.key==input::Key::escape&&!event.repeat&&
           input.available(event)&&selection_enabled&&c.input.shortcuts_enabled&&image.bounds.contains(event.position)&&
           view.mode==ViewMode::mesh&&!parts.busy()&&parts.selected_part()) {
            if(parts.select_part({})) {
                tools.mesh_part.cancel();tools.selection_handled();
                reply.status="Whole blueprint / 5: transform mesh";
            }
            continue;
        }
        const bool available_press=event.kind==input::EventKind::pointer_down&&event.button==0&&
            image.bounds.contains(event.position)&&selection_enabled&&image.time_current&&
            image.revision==current.document.revision&&!c.tools.diagnostic&&input.available(event);
        if(available_press&&view.mode!=ViewMode::mesh&&!c.tools.components) {
            begin_box(event);
            observed_selection(select_instance(pick_object(current,
                {(event.position.x-image.bounds.x)/image.bounds.width,(event.position.y-image.bounds.y)/image.bounds.height},
                image.extent,&image.camera).value_or(0),click_selection(event.modifiers)));
            continue;
        }
        if(available_press&&view.mode==ViewMode::mesh&&components.mode()==MeshSelectMode::surface&&!parts.busy()) {
            if(auto camera=image.camera.snapshot(image.extent)) {
                const Vec2 point{(event.position.x-image.bounds.x)/image.bounds.width,(event.position.y-image.bounds.y)/image.bounds.height};
                if(parts.connecting()) {
                    if(auto slot=tools.mesh_sockets.hit(event.position)) {
                        (void)parts.attach_slot(*slot);tools.mesh_sockets.clear();accept_parts();
                    } else {
                        (void)parts.pick_connection_target(point,*camera);
                        tools.mesh_sockets.update(parts.connection_slots(),*camera,image.bounds);
                    }
                } else if(parts.placing()) {
                    (void)parts.place_part(point,*camera);accept_parts();
                } else {
                    const auto part=parts.pick_part(point,*camera);
                    (void)parts.select_part(part);
                    (void)tools.mesh_part.update(parts.gizmo_handles(),parts.selected_handle(),*camera,image.bounds,{},{},true,true);
                    reply.status=part?"Blueprint part selected / drag its surface handle":"Whole blueprint / 5: transform mesh";
                }
            }
            tools.selection_handled();continue;
        }
        if(event.kind==input::EventKind::pointer_down&&event.button==1&&c.tools.components&&
           view.mode==ViewMode::mesh&&components.component_mode()&&selection_enabled&&!editing_.active(EditGesture::vertices)&&
           image.bounds.contains(event.position)&&!tools.camera_navigation().pointer().handledPointer()&&
           !event.modifiers.alt&&!event.modifiers.control&&input.available(event)) {
            mesh_input({.open_menu=MeshMenuPlacement{event.position,c.input.raw.logical_size,image.bounds}},
                ToolPanelInput{.context={.close=true}});
            tools.selection_handled();continue;
        }
        if(available_press&&c.tools.components&&view.mode==ViewMode::mesh&&components.component_mode()&&view.paused&&
           !editing_.awaiting_remote()&&editable_mesh(current)) {
            const auto picked=components.pick(current,
                {(event.position.x-image.bounds.x)/image.bounds.width,(event.position.y-image.bounds.y)/image.bounds.height},
                image.extent,image.camera,image.bounds);
            if(picked) {
                const std::array ids{*picked};
                const std::array change{MeshSelectionInput{ids,event.modifiers.shift||event.modifiers.control
                    ?editor::SelectionMode::toggle:editor::SelectionMode::replace}};
                mesh_input({.selection=change});
                const auto selected=components.vertices(*editable_mesh(current));
                reply.vertex_changed=true;
                if(selected.empty())continue;
                view.selected_vertex=selected.back();
                reply.viewport_changed=reply.inspector_changed=true;
                tools.selection_handled();
            } else {
                begin_box(event);
                if(!event.modifiers.shift&&!event.modifiers.control)mesh_input({.select_all=true});
            }
        }
    }
    return reply;
}
} // namespace editor_example
