#pragma once
#include "component_transform.hpp"
#include "editing_session.hpp"
#include "mesh_tools_ui.hpp"
#include "animation.hpp"
#include "rotation_math.hpp"
#include "../support/mesh_frame.hpp"

namespace editor_example {
struct MeshTransformProposal {
    TransformAction action{};
    BlueprintId blueprint{};
    vng::u64 revision{};
    ViewMode mode{};
    bool whole{}, owns_transaction{};
    std::optional<vng::u64> transaction{};
    std::vector<vng::u32> vertices{}; // Start identities, owned until parent execution.
    std::vector<VertexPosition> positions{};
    std::optional<vng::Mat4> placement{};
};
// Computes blueprint-local, sparse proposals. Only the parent can execute the
// corresponding mesh transaction; this child borrows a read-only session view.
class MeshTransformGizmo {
public:
    explicit MeshTransformGizmo(const EditingSession& editing):editing_(editing){}
    void scale_limits(const ScaleLimits& limits) { tool_.scale_limits(limits); }
    bool active() const {return tool_.active();}
    bool visible() const {return tool_.visible();}
    bool handled() const {return tool_.handled();}
    bool selected_handle() const {return tool_.selected_handle();}
    GizmoMode gizmo() const { return tool_.gizmo(); }
    ToolOptions* tool_options() {return tool_.tool_options();}
    void cancel() {tool_.cancel();owns_=false;transaction_.reset();}
    void accept_result(const vng::content::Result<bool>& result) {
        if(!result)cancel();
        else if(owns_)transaction_=editing_.active_transaction();
        else transaction_.reset();
    }
    void append(vng::ui::DrawList& list,const vng::text::Font& font) const {tool_.append(list,font);}
    [[nodiscard]] vng::content::Result<MeshTransformProposal> update(const MeshToolsUI& selection,
        const vng::gfx::CameraSnapshot& camera,vng::ui::Rect viewport,
        std::span<const vng::input::Event> input,std::span<const vng::input::Event> raw,bool enabled,float arrow_step=1.F) {
        using namespace vng;
        const auto& state=editing_.state();
        const auto target=mesh_target(state);
        const auto* mesh=editable_mesh(state);
        enabled &= target.has_value()&&mesh;
        if(!active()&&enabled) {
            if(revision_!=state.document.revision||selection_!=selection.selection_revision()||
               blueprint_!=target->blueprint||weld_!=state.viewport.weld||mode_!=state.viewport.mode) {
                points_.clear();ids_=selection.vertices(*mesh,state.viewport.weld);
                whole_=selection.mode()==MeshSelectMode::whole;
                transform_=state.viewport.mode==ViewMode::mesh?InstanceTransform{}:
                    evaluate_transform(state,*find_instance(state,state.viewport.selected_object),state.viewport.time);
                model_=mesh_transform(state,target->blueprint,transform_);
                const auto inverse=example::mesh_frame::inverse(model_);
                if(!inverse)return std::unexpected(inverse.error());
                inverse_=*inverse;
                for(auto id:ids_) {
                    auto p=example::mesh_frame::point(model_,mesh->position(id));
                    points_.push_back({id,p,{}});
                }
                if(whole_ && mesh->size()) {
                    // One synthetic pivot, not a huge implicit selection. The
                    // affine placement moves the unchanged geometry on the GPU.
                    points_.push_back({0,example::mesh_frame::point(model_,mesh->center()),{}});
                }
                revision_=state.document.revision;selection_=selection.selection_revision();
                blueprint_=target->blueprint;weld_=state.viewport.weld;mode_=state.viewport.mode;
            }
        }
        tool_.gizmo(selection.transform_mode());
        tool_.capabilities(whole_?std::span<const GizmoMode>{whole_mesh_gizmos}:std::span<const GizmoMode>{basic_transform_gizmos});
        auto action=tool_.update(points_,{static_cast<u64>(blueprint_),selection_,revision_},camera,viewport,input,raw,enabled,{},arrow_step);
        MeshTransformProposal proposal{.action={action.began,action.changed,action.finished,action.cancelled},
            .blueprint=blueprint_,.revision=state.document.revision,.mode=state.viewport.mode,
            .whole=whole_,.owns_transaction=owns_,.transaction=transaction_};
        if(action.began) {
            owns_=true;proposal.vertices=ids_;
            if(whole_)whole_origin_=mesh_placement(state,blueprint_,true);
        }
        if(owns_ && !action.cancelled && action.changed) {
            if(whole_)proposal.placement=example::mesh_frame::compose(tool_.matrix(),whole_origin_);
            else {
                proposal.positions.reserve(action.points.size());
                for(auto point:action.points)
                    proposal.positions.push_back({point.id,example::mesh_frame::point(inverse_,point.position)});
            }
        }
        if(action.finished||action.cancelled)owns_=false;
        return proposal;
    }
private:
    const EditingSession& editing_;
    ComponentTransform tool_;
    std::vector<vng::editor::ScenePoint> points_;
    std::vector<vng::u32> ids_;
    vng::u64 revision_{UINT64_MAX},selection_{};
    BlueprintId blueprint_{};
    bool weld_{};
    bool whole_{};
    bool owns_{};
    std::optional<vng::u64> transaction_;
    vng::Mat4 model_{vng::Mat4::identity()},inverse_{vng::Mat4::identity()},whole_origin_{vng::Mat4::identity()};
    ViewMode mode_{};
    InstanceTransform transform_{};
};

[[nodiscard]] inline vng::content::Result<bool> execute(
    EditingSession& editing, MeshTransformGizmo& tool,
    vng::content::Result<MeshTransformProposal> proposal) {
    using namespace vng;
    if(!proposal) {auto result=content::Result<bool>{std::unexpected(proposal.error())};tool.accept_result(result);return result;}
    const auto& edit=*proposal;
    bool owns=edit.owns_transaction&&edit.transaction&&editing.active_transaction()==edit.transaction&&
        editing.active(edit.whole?EditGesture::mesh_transform:EditGesture::vertices)&&editing.active_blueprint()==edit.blueprint;
    const auto fail=[&](content::Diagnostic error)->content::Result<bool> {
        if(owns){auto restored=editing.cancel();if(!restored)error=restored.error();}
        auto result=content::Result<bool>{std::unexpected(std::move(error))};tool.accept_result(result);return result;
    };
    const auto invalid=[&](std::string message) {
        content::Diagnostic error{};
        error.message=std::move(message);
        return fail(std::move(error));
    };
    if(edit.owns_transaction&&!owns)return invalid("Mesh transform no longer owns its edit transaction");
    const auto target=mesh_target(editing.state());
    if(!edit.action.cancelled&&(edit.action.began||owns)&&
        (editing.state().document.revision!=edit.revision||editing.state().viewport.mode!=edit.mode||
         !target||target->blueprint!=edit.blueprint))
        return invalid("Mesh transform proposal belongs to a stale target or revision");
    if(edit.action.began) {
        auto begun=edit.whole?editing.begin_mesh_transform(edit.blueprint):editing.begin_vertices(edit.blueprint,edit.vertices);
        if(!begun)return fail(begun.error());
        owns=true;
    }
    bool changed{};
    if(owns) {
        if(edit.action.cancelled) {
            auto restored=editing.cancel();if(!restored)return fail(restored.error());changed=*restored;
        } else {
            content::Result<bool> updated{false};
            if(edit.placement)updated=editing.mesh_transform(*edit.placement);
            else if(edit.action.changed)updated=editing.vertices(edit.positions);
            if(!updated)return fail(updated.error());
            changed=*updated;
            if(edit.action.finished){auto committed=editing.commit();if(!committed)return fail(committed.error());}
        }
    }
    tool.accept_result(changed);return changed;
}
}
