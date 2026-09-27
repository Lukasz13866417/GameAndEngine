#pragma once
#include "component_dispatch.hpp"
#include "mesh_editing.hpp"
#include "mesh_navigation.hpp"
#include "scene_lists.hpp"
#include "timeline_editing.hpp"
#include "tool_panel.hpp"
#include "workspace_selection.hpp"
#include "viewport_interaction.hpp"
#include "blueprint_mesh_panel.hpp"
#include "gizmo_selector.hpp"
#include "rotation_pivot_controls.hpp"
#include "viewport_context.hpp"
#include "viewport_picking.hpp"
#include <stdexcept>

namespace editor_example {
class BlueprintMeshPanel;
class RegionEditor;
class ViewportInteraction;
enum class ViewportTool;
class InstanceTransformInteraction;
struct InstanceTransformProposal;
struct InstanceTransformChange;
class MeshTransform;
struct MeshTransformProposal;
class RotationInteraction;
struct RotationProposal;
struct RotationChange;
// The component-authoring branch of the workspace. Contexts contain only
// synchronous routed input. No session/service lookup is hidden in them.
struct ViewportPresentation {
    vng::ui::Rect bounds{};
    bool show_fps{}, can_bake{}, covered{};
};
struct ToolPanelInput {
    std::optional<ToolPanel::Show> show{};
    ToolPanel::Context context{};
};
struct ViewportEditingContext {
    std::optional<MeshInput> mesh{};
    bool accept_input{true};
    std::optional<ViewportPresentation> presentation{};
    bool poll_navigation{};
    std::optional<ToolPanelInput> tools{};
    bool mode_changed{};
};
struct ViewportEditingReply {
    MeshEditingReply mesh;
    std::optional<CameraBakeOptions> bake;
};
class EditingViewport final {
public:
    EditingViewport(const EditingSession& editing, vng::ui::Container controls, vng::ui::Container popup, vng::ui::Container navigation,
                    vng::ui::Container tools)
        : editing_(editing), mesh_(editing, controls, popup), navigation_(navigation), tools_(tools) {}
    [[nodiscard]] const ToolPanel& tools() const { return tools_; }
    [[nodiscard]] const MeshTools& mesh_components() const { return mesh_.components(); }
    [[nodiscard]] bool controls_contain(vng::Vec2 point) const { return navigation_.contains(point); }
    [[nodiscard]] std::optional<vng::Vec3> camera_origin() const { return navigation_.origin(editing_.state()); }
    [[nodiscard]] DebugReport debug_report() const {
        DebugReport report{.name="viewport", .role="component authoring viewport", .situation=std::string(situation_),
            .children={mesh_.debug_report(),navigation_.debug_report(),tools_.debug_report()}};
        if(interaction_)report.children.push_back(interaction_->debug_report());
        if(blueprint_panel_)report.children.push_back(blueprint_panel_->debug_report());
        if(gizmo_selector_)report.children.push_back(gizmo_selector_->debug_report());
        if(rotation_pivot_)report.children.push_back(rotation_pivot_->debug_report());
        return report;
    }
    [[nodiscard]] std::string debug_string() const { return debug_report().string(); }
private:
    friend class EditingWorkspace;
    void attach_tools(vng::ui::Container blueprint,vng::ui::Container controls,vng::ui::Container creation,
                      vng::ui::Container inspector,vng::ui::Container popup);
    ViewportEditingReply handle(const InspectMesh& s, const ViewportEditingContext& c) {
        situation_="InspectMesh";
        present_tools(c);
        ViewportEditingReply result;
        auto& reply=result.mesh;
        if(c.mesh || c.mode_changed)
            reply=mesh_.handle(s,MeshEditingContext{c.mesh.value_or(MeshInput{}),c.accept_input});
        if(c.presentation) presentation_=c.presentation;
        if(presentation_ && (c.presentation || c.poll_navigation || c.mode_changed || reply.mode_changed)) {
            const bool whole=mesh_.components().mode()==MeshSelectMode::whole;
            const auto& p=*presentation_;
            const bool allowed=p.can_bake && c.accept_input && !editing_.busy();
            auto options=navigation_.handle(MeshNavigationControls::MeshView{whole},
                MeshNavigationControls::Context{p.bounds,p.show_fps,allowed,p.covered,c.poll_navigation});
            if(options && whole && allowed) result.bake=options;
        }
        reply.adjustment=mesh_.take_adjustment();
        return result;
    }
    ViewportEditingReply handle(const InspectScene& s, const ViewportEditingContext& c) {
        situation_="InspectScene";
        present_tools(c);
        suspend_navigation(c);
        return {c.mesh || c.mode_changed ? mesh_.handle(s,MeshEditingContext{c.mesh.value_or(MeshInput{}),false}) : MeshEditingReply{}, {}};
    }
    ViewportEditingReply handle(const InspectEffect& s, const ViewportEditingContext& c) {
        situation_="InspectEffect";
        present_tools(c);
        suspend_navigation(c);
        return {c.mesh || c.mode_changed ? mesh_.handle(s,MeshEditingContext{c.mesh.value_or(MeshInput{}),false}) : MeshEditingReply{}, {}};
    }
    MeshEditingReply accept_operation(const MeshEditProposal& request,const vng::content::Result<bool>& result) {
        return mesh_.accept_operation(request,result);
    }
    MeshEditingReply accept_adjustment(const MeshOperationAdjustment& request,const vng::content::Result<bool>& result) {
        return mesh_.accept_adjustment(request,result);
    }
    void suspend_navigation(const ViewportEditingContext& c) {
        if(c.presentation) presentation_=c.presentation;
        if(!c.presentation && !c.mode_changed) return;
        (void)navigation_.handle(MeshNavigationControls::Inactive{},MeshNavigationControls::Context{
            presentation_ ? presentation_->bounds : vng::ui::Rect{}});
    }
    void present_tools(const ViewportEditingContext& c) {
        if(!c.tools) return;
        if(c.tools->show) tools_.handle(*c.tools->show,c.tools->context);
        else tools_.handle(ToolPanel::Current{},c.tools->context);
    }
    const EditingSession& editing_;
    MeshEditing mesh_;
    MeshNavigationControls navigation_;
    ToolPanel tools_;
    std::optional<BlueprintMeshPanel> blueprint_panel_;
    std::optional<ViewportInteraction> interaction_;
    std::optional<GizmoSelector> gizmo_selector_;
    std::optional<RotationPivotControls> rotation_pivot_;
    std::optional<ViewportPresentation> presentation_;
    std::optional<ViewportPickCapture> pick_capture_;
    std::string_view situation_{"Not dispatched"};
};

struct WorkspaceContext {
    std::optional<MeshInput> mesh{};
    bool accept_input{true};
    std::optional<ViewportPresentation> presentation{};
    bool poll_navigation{};
    std::optional<SceneListsContext> lists{};
    std::optional<TimelineContext> timeline{};
    std::optional<ToolPanelInput> tools{};
};
struct WorkspaceReply { MeshEditingReply mesh{}; SceneListsReply lists{}; TimelineReply timeline{}; };
class EditingWorkspace final {
public:
    // UI children borrow this owner's session and each other's owned option
    // objects. Their addresses remain stable for the workspace's lifetime.
    EditingWorkspace(const EditingWorkspace&)=delete;
    EditingWorkspace& operator=(const EditingWorkspace&)=delete;
    EditingWorkspace(EditingWorkspace&&)=delete;
    EditingWorkspace& operator=(EditingWorkspace&&)=delete;
    // The authoring authority exists before windows and widgets. UI children
    // are attached once after their hosts have been successfully constructed.
    explicit EditingWorkspace(State initial, SceneFile file={}) : editing_(std::move(initial),std::move(file)) {
        reconcile_selection();
    }
    EditingWorkspace(State initial, vng::ui::Container controls, vng::ui::Container popup,
                     vng::ui::Container navigation,SceneListHosts lists,TimelineHosts timeline,vng::ui::Container tools,
                     SceneFile file={}) : EditingWorkspace(std::move(initial),std::move(file)) {
        initialize(controls,popup,navigation,lists,timeline,tools);
    }
    void initialize(vng::ui::Container controls,vng::ui::Container popup,vng::ui::Container navigation,
                    SceneListHosts lists,TimelineHosts timeline,vng::ui::Container tools) {
        if(viewport_) throw std::logic_error("Workspace UI is already initialized");
        viewport_.emplace(editing_,controls,popup,navigation,tools);
        lists_.emplace(lists);
        timeline_.emplace(editing_,timeline);
        present_selection(false);
    }
    [[nodiscard]] const EditingSession& session() const { return editing_; }
    void attach_viewport_tools(vng::ui::Container blueprint,vng::ui::Container controls,vng::ui::Container creation,
                               vng::ui::Container inspector,vng::ui::Container popup);
    [[nodiscard]] BlueprintMeshPanel& blueprint_panel();
    [[nodiscard]] ViewportInteraction& interaction();
    void attach_manipulation(vng::ui::Container gizmo,vng::ui::Container pivot);
    [[nodiscard]] GizmoSelector& gizmo_selector();
    [[nodiscard]] RotationPivotControls& rotation_pivot();
    void begin_viewport_frame();
    [[nodiscard]] ViewportInputReply interact_viewport(const ViewportInputContext&);
    void refresh_viewport_gizmos(const ViewportInputContext&);
    [[nodiscard]] ViewportSelectionReply pick_viewport(const ViewportSelectionContext&);
    [[nodiscard]] const State& state() const { return editing_.state(); }
    [[nodiscard]] ViewportState& viewport() { return editing_.viewport(); }
    [[nodiscard]] const vng::editor::Selection<vng::u32>& selected_instances() const { return selection_.instances(); }
    WorkspaceSelection::Change select_instance(vng::u32 id,vng::editor::SelectionMode mode=vng::editor::SelectionMode::replace) {
        return publish_selection(selection_.select(state(),id,mode));
    }
    WorkspaceSelection::Change select_instances(std::span<const vng::u32> ids,vng::editor::SelectionMode mode=vng::editor::SelectionMode::replace) {
        return publish_selection(selection_.select(state(),ids,mode));
    }
    WorkspaceSelection::Change restore_selection(const vng::editor::Selection<vng::u32>& origin) {
        return publish_selection(selection_.restore(state(),origin));
    }
    WorkspaceSelection::Change clear_selection() { return publish_selection(selection_.clear()); }
    WorkspaceSelection::Change reconcile_selection() { return publish_selection(selection_.reconcile(state()),false); }
    WorkspaceSelection::Change reset_selection() { return publish_selection(selection_.reset(state()),false); }
    [[nodiscard]] const ToolPanel& tools() const { return viewport_->tools(); }
    [[nodiscard]] const TimelinePanel& timeline_view() const { return timeline_->view(); }
    [[nodiscard]] const MeshTools& mesh_components() const { return viewport_->mesh_components(); }
    [[nodiscard]] bool viewport_controls_contain(vng::Vec2 point) const { return viewport_->controls_contain(point); }
    [[nodiscard]] std::optional<vng::Vec3> mesh_camera_origin() const { return viewport_->camera_origin(); }
    [[nodiscard]] bool contains_instance(vng::u32 id) const { return lists_->contains(id); }
    [[nodiscard]] bool list_flyout_contains(vng::Vec2 point) const { return lists_->flyout_contains(point); }
    [[nodiscard]] bool list_flyout_open() const { return lists_->flyout_open(); }
    [[nodiscard]] vng::content::Result<bool> apply_pending(BlueprintMeshPanel&);
    [[nodiscard]] vng::content::Result<bool> apply_pending(RegionEditor&);
    [[nodiscard]] vng::content::Result<bool> finish(ViewportInteraction&,ViewportTool,bool cancelled=false);
    [[nodiscard]] vng::content::Result<bool> cancel(ViewportInteraction&);
    [[nodiscard]] vng::content::Result<InstanceTransformChange> execute(
        InstanceTransformInteraction&,vng::content::Result<InstanceTransformProposal>);
    [[nodiscard]] vng::content::Result<bool> execute(MeshTransform&,vng::content::Result<MeshTransformProposal>);
    [[nodiscard]] vng::content::Result<RotationChange> execute(
        RotationInteraction&,vng::content::Result<RotationProposal>);
    [[nodiscard]] vng::content::Result<std::optional<bool>> apply_instance_control(
        const vng::editor::Event&,vng::u64 generation,vng::u64 minimum_context);

    // Parent-facing authoring actions. Children only receive const session
    // observations; no callback or mutable session escape bypasses this owner.
    void select_keyframe(std::optional<vng::f32> time) { editing_.select_keyframe(time); }
    bool can_edit_scene_pose() const { return editing_.can_edit_scene_pose(); }
    bool dirty() const { return editing_.dirty(); }
    bool can_undo() const { return editing_.can_undo(); }
    bool can_redo() const { return editing_.can_redo(); }
    bool busy() const { return editing_.busy(); }
    bool active(EditGesture kind) const { return editing_.active(kind); }
    vng::u32 active_object() const { return editing_.active_object(); }
    auto take_changes() { return editing_.take_changes(); }
    unsigned timeline_track_limit() const { return editing_.timeline_track_limit(); }
    auto timeline_track_limit(unsigned limit) { return editing_.timeline_track_limit(limit); }
    unsigned instance_limit() const { return editing_.instance_limit(); }
    auto instance_limit(unsigned limit) { return editing_.instance_limit(limit); }
    auto undo() { return editing_.undo(); }
    auto redo() { return editing_.redo(); }
    auto begin_move(vng::u32 object,std::span<const vng::u32> selected={}) { return editing_.begin_move(object,selected); }
    auto begin_rotation(vng::u32 object,std::span<const vng::u32> selected={},TransformPivot pivot={}) { return editing_.begin_rotation(object,selected,pivot); }
    auto begin_attitude(vng::u32 object,std::span<const vng::u32> selected={},TransformPivot pivot={}) { return editing_.begin_attitude(object,selected,pivot); }
    auto begin_scale(vng::u32 object,std::span<const vng::u32> selected={},bool axes=false) { return editing_.begin_scale(object,selected,axes); }
    auto begin_vertices(BlueprintId id,std::span<const vng::u32> vertices) { return editing_.begin_vertices(id,vertices); }
    auto begin_camera() { return editing_.begin_camera(); }
    auto begin_world_bounds() { return editing_.begin_world_bounds(); }
    auto world_bounds(const WorldBounds& bounds) { return editing_.world_bounds(bounds); }
    auto add_region(RegionShape shape,vng::Vec3 center,vng::f32 radius) { return editing_.add_region(shape,center,radius); }
    auto erase_region(vng::u32 object) { return editing_.erase_region(object); }
    auto begin_region(vng::u32 object) { return editing_.begin_region(object); }
    auto region(const Region& region) { return editing_.region(region); }
    auto begin_region_points(vng::u32 object,std::span<const vng::u32> points) { return editing_.begin_region_points(object,points); }
    auto region_points(std::span<const RegionPointEdit> points) { return editing_.region_points(points); }
    auto move(vng::Vec3 position) { return editing_.move(position); }
    auto apply(const InstanceMovement::Edit& edit) { return editing_.apply(edit); }
    auto rotate(vng::Vec3 rotation) { return editing_.rotate(rotation); }
    auto rotate_by(vng::Vec3 rotation) { return editing_.rotate_by(rotation); }
    auto attitude(vng::u32 axis,vng::f64 degrees) { return editing_.attitude(axis,degrees); }
    auto attitude(const std::array<vng::f64,3>& degrees) { return editing_.attitude(degrees); }
    auto scale(vng::f32 value,std::optional<vng::f32> maximum={}) { return editing_.scale(value,maximum); }
    auto scale_factor(vng::f32 factor,int axis=-1,ScaleLimits limits={}) { return editing_.scale_factor(factor,axis,limits); }
    auto move_vertices(vng::Vec3 delta) { return editing_.move_vertices(delta); }
    auto vertices(std::span<const VertexPosition> values) { return editing_.vertices(values); }
    auto camera(const CameraPose& pose) { return editing_.camera(pose); }
    auto commit() { return editing_.commit(); }
    auto cancel() { return editing_.cancel(); }
    auto import_mesh(const std::filesystem::path& path) { return editing_.import_mesh(path); }
    auto import_asset(const std::filesystem::path& path) { return editing_.import_asset(path); }
    auto instantiate(BlueprintId id) { return editing_.instantiate(id); }
    auto erase_instances(std::span<const vng::u32> objects) { return editing_.erase_instances(objects); }
    auto apply_mesh(BlueprintId id) { return editing_.apply_mesh(id); }
    auto replace_mesh_draft(BlueprintId id,vng::u64 revision,vng::editor::EditableMesh mesh) { return editing_.replace_mesh_draft(id,revision,std::move(mesh)); }
    auto begin_mesh_draft_edit(BlueprintId id) { return editing_.begin_mesh_draft_edit(id); }
    auto begin_mesh_transform(BlueprintId id) { return editing_.begin_mesh_transform(id); }
    auto mesh_transform(vng::Mat4 transform) { return editing_.mesh_transform(transform); }
    auto preview_mesh_draft(vng::u64 revision,vng::editor::EditableMesh mesh) { return editing_.preview_mesh_draft(revision,std::move(mesh)); }
    auto translate_vertices(BlueprintId id,std::span<const vng::u32> vertices,vng::Vec3 delta) { return editing_.translate_vertices(id,vertices,delta); }
    auto add_keyframe(vng::f32 time) { return editing_.add_keyframe(time); }
    auto erase_keyframes(std::span<const vng::f32> times) { return editing_.erase_keyframes(times); }
    auto duration(vng::f32 value) { return editing_.duration(value); }
    auto copy_instances(std::span<const vng::u32> objects) { return editing_.copy_instances(objects); }
    auto copy_keyframes(std::span<const vng::f32> times) { return editing_.copy_keyframes(times); }
    auto paste() { return editing_.paste(); }
    auto set_transform(vng::u32 object,vng::Vec3 rotation,vng::f32 scale,std::optional<vng::Vec3> axes={}) { return editing_.set_transform(object,rotation,scale,axes); }
    auto set_camera(vng::u32 object,const CameraPose& pose) { return editing_.set_camera(object,pose); }
    auto set_active_camera(vng::u32 object) { return editing_.set_active_camera(object); }
    auto begin_remote(vng::u64 generation) { return editing_.begin_remote(generation); }
    bool awaiting_remote() const { return editing_.awaiting_remote(); }
    void abandon_remote() { editing_.abandon_remote(); }
    auto accept_remote(vng::u64 generation,const DocumentPatch& patch) { return editing_.accept_remote(generation,patch); }
    const auto& path() const { return editing_.path(); }
    auto load(const std::filesystem::path& path) { return editing_.load(path); }
    auto save() { return editing_.save(); }
    auto save_as(const std::filesystem::path& path,bool replace=false) { return editing_.save_as(path,replace); }
    [[nodiscard]] DebugReport debug_report() const {
        DebugReport report{.name="workspace", .role="authoring authority and child coordination", .situation=std::string(situation_)};
        if(viewport_) report.children={viewport_->debug_report(),lists_->debug_report(),timeline_->debug_report()};
        report.children.push_back(editing_.debug_report());
        report.children.push_back(selection_.debug_report());
        return report;
    }
    [[nodiscard]] std::string debug_string() const { return debug_report().string(); }
private:
    friend struct Dispatcher;
    WorkspaceSelection::Change publish_selection(WorkspaceSelection::Change change,bool reset_vertex=true) {
        auto& view=editing_.viewport();
        change.previous_active=view.selected_object;
        change.active_changed=change.previous_active!=change.active;
        if(reset_vertex && (change.active_changed || change.changed)) view.selected_vertex=0;
        view.selected_object=change.active;
        synchronize_selection_gizmos(change.active_changed || change.changed);
        if(change.active_changed || change.changed) present_selection(reset_vertex);
        return change;
    }
    void synchronize_selection_gizmos(bool changed) {
        if(!viewport_) return;
        auto& child=*viewport_;
        // Reconciliation may observe changed blueprint capabilities without
        // changing IDs. Refresh the menu, but retain selected handles/capture.
        if(child.gizmo_selector_) child.gizmo_selector_->show(state(),selected_instances().items());
        if(child.interaction_) {
            if(changed) child.interaction_->translation.cancel();
            child.interaction_->selected(state().viewport.selected_object,
                child.gizmo_selector_ ? child.gizmo_selector_->value() : GizmoMode::move);
        }
    }
    void present_selection(bool reveal) {
        if(!lists_) return;
        const auto& selected=selection_.instances();
        const auto active=selected.active().value_or(0);
        lists_->selection(selected,reveal && active ? std::optional{active} : std::nullopt);
        const std::vector<vng::u64> objects(selected.items().begin(),selected.items().end());
        // The workspace owns the relationship. Timeline decides how an object
        // is expanded/revealed and defers focus while its inspector is hidden.
        timeline_->present(TimelineInput{.objects=std::span<const vng::u64>{objects},
            .focus=timeline_->view().selected_keyframe() ? std::optional<vng::u64>{active} : std::nullopt});
    }
    WorkspaceReply handle(const InspectMesh& s, const WorkspaceContext& c) {
        const bool transitioned=situation_!="InspectMesh" || blueprint_!=s.blueprint;
        situation_="InspectMesh";
        blueprint_=s.blueprint;
        return {route_viewport(s,c,transitioned),lists(c),timeline(c)};
    }
    WorkspaceReply handle(const InspectScene& s, const WorkspaceContext& c) {
        const bool transitioned=situation_!="InspectScene";
        situation_="InspectScene";
        blueprint_.reset();
        return {route_viewport(s,c,transitioned),lists(c),timeline(c)};
    }
    WorkspaceReply handle(const InspectEffect& s, const WorkspaceContext& c) {
        const bool transitioned=situation_!="InspectEffect";
        situation_="InspectEffect";
        blueprint_.reset();
        return {route_viewport(s,c,transitioned),lists(c),timeline(c)};
    }
    template<class Situation>
    MeshEditingReply route_viewport(const Situation& s,const WorkspaceContext& c,bool transitioned) {
        if(!transitioned && !c.mesh && !c.presentation && !c.poll_navigation && !c.tools) return {};
        if(!viewport_) throw std::logic_error("Workspace UI has not been initialized");
        // Execute every authoring proposal before routing the next shortcut.
        // A select-all after subdivision must see the newly produced topology.
        auto input=c.mesh;
        const auto shortcuts=input ? input->shortcuts : std::span<const vng::input::Event>{};
        if(input) input->shortcuts={};
        auto reply=execute(viewport_->handle(s,ViewportEditingContext{input,c.accept_input,c.presentation,c.poll_navigation,c.tools,transitioned}));
        for(const auto& event:shortcuts) {
            MeshInput next{.shortcuts=std::span{&event,1}};
            merge(reply,execute(viewport_->handle(s,ViewportEditingContext{next,c.accept_input})));
        }
        return reply;
    }
    static void merge(MeshEditingReply& target,MeshEditingReply next) {
        target.authored|=next.authored;
        target.visibility_changed|=next.visibility_changed;
        target.selection_changed|=next.selection_changed;
        target.mode_changed|=next.mode_changed;
        target.gizmo_changed|=next.gizmo_changed;
        target.clear_part_selection|=next.clear_part_selection;
        target.camera_changed|=next.camera_changed;
        if(next.operation_options) target.operation_options=next.operation_options;
        if(!next.message.empty()) target.message=std::move(next.message);
    }
    MeshEditingReply execute(ViewportEditingReply result) {
        auto reply=std::move(result.mesh);
        if(auto request=std::exchange(reply.proposal,{})) {
            const auto applied=editing_.mesh_operation(request->blueprint,request->operation,request->vertices,request->edges);
            merge(reply,viewport_->accept_operation(*request,applied));
        }
        if(auto request=std::exchange(reply.adjustment,{})) {
            vng::content::Result<bool> applied;
            if(!request->undo) applied=editing_.adjust_mesh_operation(request->revision,request->settings);
            else if(editing_.can_adjust_mesh_operation(request->revision)) applied=editing_.undo();
            else {
                vng::content::Diagnostic error;
                error.message="The operation is no longer current";
                applied=std::unexpected(std::move(error));
            }
            merge(reply,viewport_->accept_adjustment(*request,applied));
        }
        if(result.bake) {
            const auto applied=bake_mesh_camera(editing_,*result.bake);
            if(!applied) reply.message=applied.error().message;
            else {
                reply.authored|=*applied; reply.camera_changed|=*applied;
                reply.message=*applied ? "Camera rotation/scale baked to mesh draft / Apply to publish, Save on disk to persist"
                    : "No selected camera transform to bake";
            }
        }
        return reply;
    }
    SceneListsReply lists(const WorkspaceContext& c) {
        return c.lists ? lists_->handle(SceneLists::Browsing{},*c.lists) : SceneListsReply{};
    }
    TimelineReply timeline(const WorkspaceContext& c) {
        if(!c.timeline) return {};
        auto reply=c.timeline->enabled.value_or(timeline_->enabled())
            ? timeline_->handle(TimelineEditing::Available{},*c.timeline)
            : timeline_->handle(TimelineEditing::Unavailable{},*c.timeline);
        if(auto action=std::exchange(reply.action,{})) {
            const auto previous_time=editing_.state().viewport.time;
            const auto previous_paused=editing_.state().viewport.paused;
            vng::content::Result<bool> result{false};
            switch(action->kind) {
            case TimelineAction::Kind::seek:
                editing_.viewport().time=action->time;
                editing_.viewport().paused=true;
                break;
            case TimelineAction::Kind::add: result=editing_.add_keyframe(action->time); break;
            case TimelineAction::Kind::edit:
                result=editing_.update_keyframe(action->time,action->destination,action->name,action->values); break;
            case TimelineAction::Kind::apply_range:
                result=editing_.apply_keyframe_range(action->time,action->destination,action->changes); break;
            case TimelineAction::Kind::erase:
                result=editing_.erase_keyframes(action->selection.empty()
                    ? std::span{&action->time,1} : std::span{action->selection}); break;
            case TimelineAction::Kind::duration: result=editing_.duration(action->time); break;
            case TimelineAction::Kind::select_object: break;
            }
            const auto accepted=timeline_->accept(*action,result);
            reply.authored=accepted.authored;
            reply.message=accepted.message;
            reply.playback_changed=editing_.state().viewport.time!=previous_time || editing_.state().viewport.paused!=previous_paused;
        }
        // Includes Ctrl-click/scrub clearing selection without moving playhead.
        editing_.select_keyframe(timeline_->view().selected_keyframe());
        return reply;
    }
    EditingSession editing_;
    WorkspaceSelection selection_;
    std::optional<EditingViewport> viewport_;
    std::optional<SceneLists> lists_;
    std::optional<TimelineEditing> timeline_;
    std::string_view situation_{"Not dispatched"};
    std::optional<BlueprintId> blueprint_;
};
} // namespace editor_example
