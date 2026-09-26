#pragma once
#include "mesh_editing.hpp"
#include "mesh_navigation.hpp"
#include "scene_lists.hpp"
#include "timeline_editing.hpp"
#include "tool_panel.hpp"

namespace editor_example {
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
class EditingViewport final {
public:
    EditingViewport(EditingSession& editing, vng::ui::Container controls, vng::ui::Container popup, vng::ui::Container navigation,
                    vng::ui::Container tools)
        : editing_(editing), mesh_(editing, controls, popup), navigation_(navigation), tools_(tools) {}
    [[nodiscard]] const ToolPanel& tools() const { return tools_; }
    [[nodiscard]] const MeshTools& mesh_components() const { return mesh_.components(); }
    [[nodiscard]] bool controls_contain(vng::Vec2 point) const { return navigation_.contains(point); }
    [[nodiscard]] std::optional<vng::Vec3> camera_origin() const { return navigation_.origin(editing_.state()); }
    [[nodiscard]] DebugReport debug_report() const {
        return {.name="viewport", .role="component authoring viewport", .situation=std::string(situation_),
            .children={mesh_.debug_report(),navigation_.debug_report(),tools_.debug_report()}};
    }
    [[nodiscard]] std::string debug_string() const { return debug_report().string(); }
private:
    friend struct Dispatcher;
    MeshEditingReply handle(const InspectMesh& s, const ViewportEditingContext& c) {
        situation_="InspectMesh";
        present_tools(c);
        MeshEditingReply reply;
        if(c.mesh || c.mode_changed)
            reply=dispatch(mesh_,s,MeshEditingContext{c.mesh.value_or(MeshInput{}),c.accept_input});
        if(c.presentation) presentation_=c.presentation;
        if(presentation_ && (c.presentation || c.poll_navigation || c.mode_changed || reply.mode_changed)) {
            const bool whole=mesh_.components().mode()==MeshSelectMode::whole;
            const auto& p=*presentation_;
            const bool allowed=p.can_bake && c.accept_input && !editing_.busy();
            auto options=dispatch(navigation_,MeshNavigationControls::MeshView{whole},
                MeshNavigationControls::Context{p.bounds,p.show_fps,allowed,p.covered,c.poll_navigation});
            if(options && whole && allowed) {
                auto baked=bake_mesh_camera(editing_,*options);
                if(!baked) reply.message=baked.error().message;
                else {
                    reply.authored |= *baked; reply.camera_changed |= *baked;
                    reply.message=*baked ? "Camera rotation/scale baked to mesh draft / Apply to publish, Save on disk to persist"
                        : "No selected camera transform to bake";
                }
            }
        }
        return reply;
    }
    MeshEditingReply handle(const InspectScene& s, const ViewportEditingContext& c) {
        situation_="InspectScene";
        present_tools(c);
        suspend_navigation(c);
        return c.mesh || c.mode_changed ? dispatch(mesh_,s,MeshEditingContext{c.mesh.value_or(MeshInput{}),false}) : MeshEditingReply{};
    }
    MeshEditingReply handle(const InspectEffect& s, const ViewportEditingContext& c) {
        situation_="InspectEffect";
        present_tools(c);
        suspend_navigation(c);
        return c.mesh || c.mode_changed ? dispatch(mesh_,s,MeshEditingContext{c.mesh.value_or(MeshInput{}),false}) : MeshEditingReply{};
    }
    void suspend_navigation(const ViewportEditingContext& c) {
        if(c.presentation) presentation_=c.presentation;
        if(!c.presentation && !c.mode_changed) return;
        (void)dispatch(navigation_,MeshNavigationControls::Inactive{},MeshNavigationControls::Context{
            presentation_ ? presentation_->bounds : vng::ui::Rect{}});
    }
    void present_tools(const ViewportEditingContext& c) {
        if(!c.tools) return;
        if(c.tools->show) dispatch(tools_,*c.tools->show,c.tools->context);
        else dispatch(tools_,ToolPanel::Current{},c.tools->context);
    }
    EditingSession& editing_;
    MeshEditing mesh_;
    MeshNavigationControls navigation_;
    ToolPanel tools_;
    std::optional<ViewportPresentation> presentation_;
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
    EditingWorkspace(EditingSession& editing, vng::ui::Container controls, vng::ui::Container popup,
                     vng::ui::Container navigation,SceneListHosts lists,TimelineHosts timeline,vng::ui::Container tools)
        : viewport_(editing,controls,popup,navigation,tools), lists_(lists), timeline_(editing,timeline) {}
    [[nodiscard]] const ToolPanel& tools() const { return viewport_.tools(); }
    [[nodiscard]] const TimelinePanel& timeline_view() const { return timeline_.view(); }
    [[nodiscard]] const MeshTools& mesh_components() const { return viewport_.mesh_components(); }
    [[nodiscard]] bool viewport_controls_contain(vng::Vec2 point) const { return viewport_.controls_contain(point); }
    [[nodiscard]] std::optional<vng::Vec3> mesh_camera_origin() const { return viewport_.camera_origin(); }
    [[nodiscard]] bool contains_instance(vng::u32 id) const { return lists_.contains(id); }
    [[nodiscard]] bool list_flyout_contains(vng::Vec2 point) const { return lists_.flyout_contains(point); }
    [[nodiscard]] bool list_flyout_open() const { return lists_.flyout_open(); }
    [[nodiscard]] DebugReport debug_report() const {
        return {.name="workspace", .role="component authoring workspace", .situation=std::string(situation_),
            .children={viewport_.debug_report(),lists_.debug_report(),timeline_.debug_report()}};
    }
    [[nodiscard]] std::string debug_string() const { return debug_report().string(); }
private:
    friend struct Dispatcher;
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
        return dispatch(viewport_,s,ViewportEditingContext{c.mesh,c.accept_input,c.presentation,c.poll_navigation,c.tools,transitioned});
    }
    SceneListsReply lists(const WorkspaceContext& c) {
        return c.lists ? dispatch(lists_,SceneLists::Browsing{},*c.lists) : SceneListsReply{};
    }
    TimelineReply timeline(const WorkspaceContext& c) {
        if(!c.timeline) return {};
        if(c.timeline->enabled.value_or(timeline_.enabled()))
            return dispatch(timeline_,TimelineEditing::Available{},*c.timeline);
        return dispatch(timeline_,TimelineEditing::Unavailable{},*c.timeline);
    }
    EditingViewport viewport_;
    SceneLists lists_;
    TimelineEditing timeline_;
    std::string_view situation_{"Not dispatched"};
    std::optional<BlueprintId> blueprint_;
};
} // namespace editor_example
