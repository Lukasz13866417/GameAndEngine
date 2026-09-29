#include "workspace_ui.hpp"
#include "animation.hpp"
#include <charconv>

namespace editor_example {
using namespace vng;
void EditingWorkspaceUI::create_panels(ui::Screen& screen) {
    if(panels_) throw std::logic_error("Workspace panels are already initialized");
    panels_.emplace(screen,viewport());
}
void EditingWorkspaceUI::initialize_panels(ui::Screen& screen,ui::Screen& popups,ui::Container keyframe_actions,
                                          ui::Container region_controls,ui::Container bounds_controls) {
    auto& p=*panels_;
    bounds_panel_.emplace(bounds_controls,p.manipulation_panel);
    initialize(p.vertex_tools,popups.column(),popups.root(),
        {p.scene_list,p.region_list,p.blueprint_list,screen.column(),screen.column()},
        {p.timeline_host,p.keyframe_list,p.keyframe_inspector,keyframe_actions,screen.column()},popups.column());
    attach_viewport_tools(p.blueprint_panel_host,region_controls,p.sidebar_sections[4],p.region_inspector,popups.root());
    attach_manipulation(p.gizmo_host,p.pivot_host);
}
void EditingWorkspaceUI::layout_panels(EditorLayout& geometry,Vec2 size) {
    panels_->layout(geometry);
    (void)dispatch(*this,workspace_situation(viewport()),WorkspaceContext{
        .timeline=TimelineContext{.input={.presentation=TimelinePresentation{size,geometry.timeline.height<160}}}});
}
bool EditingWorkspaceUI::resize_panels(const EditorLayout& geometry) { return panels_->resize(geometry); }
void EditingWorkspaceUI::reset_panel_scroll() {
    revealed_instance_.reset();
    if(!panels_)return;
    auto& p=*panels_;
    p.scene_panel.scroll(0);p.scene_list.scroll(0);p.region_list.scroll(0);p.blueprint_list.scroll(0);
    for(auto section:p.sidebar_sections)section.scroll(0);
    (void)dispatch(*this,workspace_situation(viewport()),WorkspaceContext{.lists=SceneListsContext{.input={.reset_scroll=true}}});
}
void EditingWorkspaceUI::sync_sidebar() {
    if(!panels_)return;
    panels_->show(viewport_&&viewport_->interaction_&&interaction_child().custom_inspector());
    (void)dispatch(*this,workspace_situation(viewport()),WorkspaceContext{
        .timeline=TimelineContext{.input={.inspector_visible=sidebar_tab()==SidebarTab::keyframe}}});
}
void EditingWorkspaceUI::poll_tabs() {
    auto& p=*panels_;
    if(p.show_scene.clicked())p.sidebar_tab=SidebarTab::scene;
    if(p.show_keyframe.clicked())p.sidebar_tab=SidebarTab::keyframe;
    if(p.show_base.clicked()) {p.sidebar_tab=SidebarTab::properties; keyboard_target_=SelectionTarget::instances;}
    sync_sidebar();
}
SidebarTab EditingWorkspaceUI::sidebar_tab() const {return panels_?panels_->sidebar_tab:SidebarTab::scene;}
void EditingWorkspaceUI::show_tab(SidebarTab tab) {if(panels_)panels_->sidebar_tab=tab;sync_sidebar();}
InteractionMode EditingWorkspaceUI::interaction_mode() const {return panels_->interaction->value();}
void EditingWorkspaceUI::interaction_mode(InteractionMode mode) {panels_->interaction->value(mode);}
std::optional<InteractionMode> EditingWorkspaceUI::changed_interaction_mode() {return panels_->interaction->changedValue();}
void EditingWorkspaceUI::refresh_selection(bool synchronize_document) {
    if(!panels_)return;
    auto& p=*panels_;
    const auto& state=this->state();auto& view_state=viewport();const auto& selected_instances=this->selected_instances();
    const auto list_input=[&](SceneListsContext c) {return dispatch(*this,workspace_situation(view_state),WorkspaceContext{.lists=c}).lists;};
    const auto blueprints = blueprint_catalog(state);
    const bool synchronize = synchronize_document || (view_state.selected_object&&!contains_instance(view_state.selected_object));
    if(synchronize) list_input({.catalog=SceneListCatalog{scene_instances(state),blueprints,selected_instances}});
    reconcile_selection();
    list_input({.catalog=SceneListCatalog{scene_instances(state),blueprints,selected_instances,false,
        revealed_instance_ && revealed_instance_!=view_state.selected_object ? std::optional{view_state.selected_object} : std::nullopt}});
    // Initial selection must not scroll either list away from its top.
    // Later explicit selection reveals the actual row, independent of sizes.
    revealed_instance_ = view_state.selected_object;
    const auto* selected_instance = find_instance(state, view_state.selected_object);
    if (view_state.mode == ViewMode::mesh) {
        const auto blueprint = std::ranges::find(blueprints, view_state.inspected_mesh, &Blueprint::id);
        p.object_title.text("Editing blueprint: " + std::string(blueprint->name));
        p.blueprint_title.text(std::to_string(std::ranges::count(state.document.instances, view_state.inspected_mesh,
                                                              &SceneInstance::blueprint)) +
                             " scene instance(s) use the applied blueprint");
    } else if (selected_instance) {
        p.object_title.text((selected_instances.size()>1 ? std::to_string(selected_instances.size())+" selected / active: " : "Instance: ") + selected_instance->name);
        const auto blueprint = std::ranges::find(blueprints, selected_instance->blueprint, &Blueprint::id);
        const auto count = std::ranges::count(state.document.instances, selected_instance->blueprint, &SceneInstance::blueprint);
        p.blueprint_title.text("Blueprint: " + std::string(blueprint->name) + " / " + std::to_string(count) + " instance(s)");
    } else {
        p.object_title.text("No instance selected");
        p.blueprint_title.text("");
    }
    p.show_base.text(view_state.mode == ViewMode::mesh ? "Blueprint geometry" : "Instance properties");
    p.transform_hint.text(view_state.mode == ViewMode::mesh
        ? "Mesh draft / Apply publishes to scene; Save writes the project"
        : selected_instances.size()>1 ? "Inspector: active only / gizmos, Copy and Delete: whole selection"
        : "Instance only / select a paused keyframe to edit");

}
void EditingWorkspaceUI::refresh_vertex() {
    if(!panels_)return;
    auto& p=*panels_;const auto& state=this->state();auto& view_state=viewport();
    p.weld.value(view_state.weld);
    (void)dispatch(*this,workspace_situation(viewport()),WorkspaceContext{.mesh=MeshInput{}});
    const auto target=mesh_target(state);
    const bool draft=target && has_mesh_draft(state,target->blueprint);
    p.draft_status.text(draft ? "Unapplied mesh changes / scene unchanged" : "No unapplied mesh changes");
    const auto* mesh = editable_mesh(state);
    if (!mesh || !mesh->size()) {
        view_state.selected_vertex = 0;
        p.selected.text("No mesh blueprint selected");
        p.x.value(""); p.y.value(""); p.z.value("");
        return;
    }
    view_state.selected_vertex = std::min(view_state.selected_vertex, static_cast<u32>(mesh->size() - 1));
    const auto position = mesh->position(view_state.selected_vertex);
    p.selected.text("Vertex " + std::to_string(view_state.selected_vertex) + " / " +
                  std::to_string(mesh->size()));
    p.x.value(std::to_string(position.x));
    p.y.value(std::to_string(position.y));
    p.z.value(std::to_string(position.z));

}
std::string EditingWorkspaceUI::selection_changed(bool active_changed) {
    keyboard_target_=SelectionTarget::instances;
    if(active_changed)clear_inspector();
    invalidate_gizmos();
    refresh_selection(false);
    if(viewport().mode==ViewMode::mesh)refresh_vertex();
    if(active_changed){++viewport().sequence;invalidate_inspector();}
    const auto object=selected_instances().active().value_or(0);
    if(selected_instances().size()>1)return "Selected "+std::to_string(selected_instances().size())+
        " instances / gizmos: whole selection / inspector: active only";
    if(state().document.timeline.find({object,"position"}))
        return "Selected 1 instance: "+object_name(state(),object)+(editing_.can_edit_scene_pose()?
            " / dragging edits the selected keyframe":" / read-only pose: select a keyframe to edit");
    return object?"Selected 1 instance: "+object_name(state(),object)+" / Delete removes this instance":
        "Selection cleared / 0 instances selected";
}
MeshEditingReply EditingWorkspaceUI::poll_vertex_controls(bool allowed) {
    auto& p=*panels_;MeshEditingReply reply;
    if(!allowed)return reply;
    if(auto value=p.weld.changedValue()){viewport().weld=*value;reply.visibility_changed=true;}
    const auto* mesh=editable_mesh(state());
    if(!viewport().paused || !mesh || interaction_mode()!=InteractionMode::vertices || mesh_components().selected().empty())return reply;
    if(!p.apply_vertex.clicked()&&!p.minus.clicked()&&!p.plus.clicked())return reply;
    const auto number=[](std::string_view text)->std::optional<f32>{
        f32 value{};const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
        if(error!=std::errc{}||end!=text.data()+text.size()||!std::isfinite(value))return {};
        return value;
    };
    const auto x=number(p.x.getText()),y=number(p.y.getText()),z=number(p.z.getText());
    if(!x||!y||!z){reply.message="Vertex components must be finite numbers.";return reply;}
    auto position=Vec3{*x,*y,*z};
    const auto before=mesh->position(viewport().selected_vertex);
    if(p.minus.clicked()||p.plus.clicked())position={before.x+(p.minus.clicked()?-.1F:.1F),before.y,before.z};
    if(position==before)return reply;
    const auto ids=mesh_components().vertices(*mesh,viewport().weld);
    auto moved=editing_.translate_vertices(mesh_target(state())->blueprint,ids,{position.x-before.x,position.y-before.y,position.z-before.z});
    if(!moved)reply.message=moved.error().message;
    else {reply.authored=*moved;refresh_vertex();}
    return reply;
}
bool EditingWorkspaceUI::save_mesh_requested() {return panels_->save_mesh_draft.clicked();}
bool EditingWorkspaceUI::publish_mesh_requested() {return panels_->publish_mesh.clicked();}
void EditingWorkspaceUI::show_inspector(const editor::Schema& schema) {if(panels_)panels_->inspector->show(schema);}
void EditingWorkspaceUI::clear_inspector() {if(panels_)panels_->inspector->clear();}
void EditingWorkspaceUI::reset_inspector_scale(f32 value) {if(panels_)panels_->inspector->reset_number("transform","scale",value);}
InspectorReply EditingWorkspaceUI::poll_inspector(bool local_scale) {
    auto& inspector=*panels_->inspector;
    InspectorReply reply{inspector.poll(),{}};
    if(const auto edit=inspector.number_edit("transform","scale");edit&&local_scale&&
       viewport().mode==ViewMode::scene&&editing_.can_edit_scene_pose()) {
        if(edit->changed||edit->pressed)reply.feedback=edit_instance_scale(edit->value,!edit->pressed);
        else if(editing_.active(EditGesture::scale))reply.feedback=finish_interaction(ViewportTool::scale,false);
    }
    if(!inspector.status().empty())reply.feedback.message=inspector.status();
    return reply;
}
void EditingWorkspaceUI::sync_bounds() {if(bounds_panel_)bounds_panel_->sync(state().document.world_bounds);}
void EditingWorkspaceUI::bounds_available(bool enabled) {bounds_panel_->available(viewport().mode==ViewMode::scene,enabled);}
void EditingWorkspaceUI::present_pose_controls(const WorkspacePanelFrame& f) {
    auto& p=*panels_;auto& tools=interaction_child();const auto& view=viewport();
    const bool remote=editing_.awaiting_remote(),busy=editing_.busy();
    const bool numeric_scale=editing_.active(EditGesture::scale)&&!tools.scale.dragging()&&!tools.instances.active();
    p.properties_panel.enabled(view.mode==ViewMode::mesh?(!f.modal&&!f.mode_pending&&!busy&&!f.gesture):
        editing_.can_edit_scene_pose()&&!f.modal&&!f.mode_pending&&!remote&&(numeric_scale||(!f.gesture&&f.inspector_ready)));
    gizmo_selector_child().enabled(editing_.can_edit_scene_pose()&&!f.playing&&!f.mode_pending&&!f.gesture&&
        selected_instances().size()!=0&&view.mode==ViewMode::scene&&interaction_mode()==InteractionMode::objects);
    blueprint_panel_child().enabled(!f.modal&&!f.gesture&&!f.mode_pending&&!busy);
}
void EditingWorkspaceUI::present_panels(const WorkspacePanelFrame& f) {
    auto& p=*panels_;auto& tools=interaction_child();const auto& view=viewport();
    const bool gesture=f.gesture;
    const bool camera_numeric=editing_.active(EditGesture::camera)&&!tools.camera_gizmo().dragging()&&!tools.camera_gizmo().walking().moving();
    const bool remote=editing_.awaiting_remote(),busy=editing_.busy();
    const bool timeline_enabled=!f.modal&&!remote&&!f.playing&&!f.mode_pending&&!tools.busy();
    p.timeline_host.enabled(timeline_enabled);p.keyframe_list.enabled(timeline_enabled);p.keyframe_inspector.enabled(timeline_enabled);
    (void)dispatch(*this,workspace_situation(view),WorkspaceContext{.timeline=TimelineContext{.input={.cancel=!timeline_enabled},.enabled=timeline_enabled}});
    p.inspector_tabs.enabled(!f.modal&&!busy);
    const bool lists_enabled=!f.modal&&!remote&&!gesture;
    p.panels_splitter.enabled(lists_enabled);for(auto splitter:p.section_splitters)splitter.enabled(lists_enabled);
    (void)dispatch(*this,workspace_situation(view),WorkspaceContext{.lists=SceneListsContext{.enabled=lists_enabled}});
    blueprint_panel_child().sync();
    present_pose_controls(f);
    p.scene_panel.enabled(!f.modal&&!remote&&(!gesture||camera_numeric));
    sync_bounds();bounds_available(!gesture&&!f.playing&&!f.mode_pending&&view.paused);
    p.scene_list.enabled(!gesture);p.region_list.enabled(!gesture);p.blueprint_list.enabled(!gesture);
    p.interaction->enabled(!gesture);
    const bool editing_mesh=view.paused&&editable_mesh(state())&&interaction_mode()==InteractionMode::vertices&&!mesh_components().selected().empty();
    p.x.enabled(editing_mesh);p.y.enabled(editing_mesh);p.z.enabled(editing_mesh);p.weld.enabled(editing_mesh);
    p.apply_vertex.enabled(editing_mesh);p.nudges.enabled(editing_mesh);
    p.publish_mesh.enabled(!remote&&!gesture&&!f.playing&&view.mode==ViewMode::mesh&&has_mesh_draft(state(),view.inspected_mesh));
    p.save_mesh_draft.enabled(!f.mode_pending&&!remote&&!gesture);
}
} // namespace editor_example
