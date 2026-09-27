#include "mesh_editing.hpp"

namespace editor_example {
using namespace vng;
MeshEditingReply MeshEditing::handle(const InspectMesh& situation, const MeshEditingContext& context) {
    situation_ = "InspectMesh";
    target_ = situation.blueprint;
    input_allowed_ = context.accept_input;
    MeshEditingReply reply;
    const auto& state = editing_.state();
    // An input batch may outlive its inspected target. Never reinterpret an
    // operation/selection against a newer blueprint or a different view.
    if (state.viewport.mode != ViewMode::mesh || state.viewport.inspected_mesh != situation.blueprint) {
        components_.close();
        reply.message = "Mesh input ignored: inspection target changed";
        input_allowed_ = false;
        last_result_ = reply.message;
        return reply;
    }
    const auto selection = components_.selection_revision();
    const auto mode = components_.mode();
    const auto gizmo = components_.transform_mode();
    if (context.input.reset) components_.reset();
    components_.sync(state, context.accept_input);
    if (context.input.close_menu) components_.close();
    if (context.accept_input) {
        if (context.input.mode) components_.mode(*context.input.mode);
        if (context.input.gizmo) components_.transform_mode(*context.input.gizmo);
        if (context.input.cycle_gizmo) components_.cycle(context.input.cycle_gizmo);
        for (const auto& change : context.input.selection) components_.select(change.elements, change.mode);
        if (context.input.select_all)
            if (const auto* mesh = editable_mesh(state)) components_.select_all(*mesh, *context.input.select_all);
        if (context.input.open_menu) {
            const auto& place = *context.input.open_menu;
            components_.open(place.at, place.screen, place.viewport);
        }
        auto requested = context.input.operation;
        if (context.input.poll_menu) {
            if (const auto from_menu = components_.poll(context.input.menu_events)) requested = from_menu;
        }
        if (requested) operation(*requested, reply);
        // Ordered input: a mode key affects the very next shortcut in this
        // batch. No stale selection snapshot is carried across a topology edit.
        for (const auto& event : context.input.shortcuts) {
            if (event.kind != input::EventKind::key_down || event.repeat || event.modifiers.control || event.modifiers.super) continue;
            if (event.key == input::Key::h && !event.modifiers.shift && components_.component_mode())
                operation(event.modifiers.alt ? MeshAction::reveal : MeshAction::hide, reply);
            if (event.modifiers.alt) continue;
            switch (event.key) {
            case input::Key::one: components_.mode(MeshSelectMode::vertex); reply.clear_part_selection=true; break;
            case input::Key::two: components_.mode(MeshSelectMode::edge); reply.clear_part_selection=true; break;
            case input::Key::three: components_.mode(MeshSelectMode::face); reply.clear_part_selection=true; break;
            case input::Key::four: components_.mode(MeshSelectMode::surface); break;
            case input::Key::five: components_.mode(MeshSelectMode::whole); reply.clear_part_selection=true; break;
            case input::Key::a:
                if (const auto* mesh = editable_mesh(state)) components_.select_all(*mesh, event.modifiers.shift);
                break;
            case input::Key::f:
                if (components_.component_mode()) operation(MeshAction::fill, reply);
                break;
            default: break;
            }
        }
    }
    reply.selection_changed = selection != components_.selection_revision();
    reply.mode_changed = mode != components_.mode();
    reply.gizmo_changed = gizmo != components_.transform_mode();
    if (!reply.message.empty()) last_result_ = reply.message;
    return reply;
}
MeshEditingReply MeshEditing::suspend(const MeshEditingContext& context) {
    input_allowed_ = false;
    if (context.input.reset) components_.reset();
    components_.close();
    // Retain local selection when only presentation changes. Applied/draft mesh
    // resolution is still synchronized by the same model code as before.
    components_.sync(editing_.state(), false);
    return {};
}
MeshEditingReply MeshEditing::handle(const InspectScene&, const MeshEditingContext& context) {
    situation_ = "InspectScene / suspended";
    return suspend(context);
}
MeshEditingReply MeshEditing::handle(const InspectEffect&, const MeshEditingContext& context) {
    situation_ = "InspectEffect / suspended";
    return suspend(context);
}
void MeshEditing::operation(MeshAction action, MeshEditingReply& reply) {
    const auto& state = editing_.state();
    const auto* mesh = editable_mesh(state);
    const auto target = mesh_target(state);
    if (!mesh || !target || state.viewport.mode != ViewMode::mesh) return;
    if (action == MeshAction::hide || action == MeshAction::reveal) {
        const auto count = action == MeshAction::hide ? components_.hide_selected() : components_.reveal_hidden();
        reply.visibility_changed |= count != 0;
        reply.message = count ? (action == MeshAction::hide ? "Hidden " : "Revealed ") + std::to_string(count) + " faces / Alt+H reveals all"
            : std::string("No faces to ") + (action == MeshAction::hide ? "hide" : "reveal");
        return;
    }
    const auto operation = action == MeshAction::fill ? MeshOperation::fill
        : action == MeshAction::subdivide ? MeshOperation::subdivide : MeshOperation::align;
    reply.proposal = MeshEditProposal{target->blueprint, operation,
        components_.vertices(*mesh), components_.edges(*mesh)};
}
MeshEditingReply MeshEditing::accept_operation(const MeshEditProposal& proposal, const vng::content::Result<bool>& result) {
    MeshEditingReply reply;
    if (!result) { reply.message = result.error().message; last_result_=reply.message; return reply; }
    if (!*result) { reply.message = "Mesh unchanged"; last_result_=reply.message; return reply; }
    reply.authored = true;
    operation_.observe(proposal.blueprint, proposal.operation);
    reply.operation_options = &operation_;
    const auto selection=components_.selection_revision();
    components_.sync(editing_.state());
    reply.selection_changed=selection!=components_.selection_revision();
    reply.message = proposal.operation == MeshOperation::fill ? "Created edge/face / Undo restores topology"
        : proposal.operation == MeshOperation::subdivide ? "Subdivided selected edges / shared midpoints / Undo restores topology"
        : "Aligned to line / first two vertices unchanged";
    last_result_ = reply.message;
    return reply;
}
MeshEditingReply MeshEditing::accept_adjustment(const MeshOperationAdjustment& request, const vng::content::Result<bool>& result) {
    operation_.accept_adjustment(request, result);
    MeshEditingReply reply;
    if (!result) reply.message = result.error().message;
    else {
        reply.authored = *result;
        const auto selection=components_.selection_revision();
        components_.sync(editing_.state());
        reply.selection_changed=selection!=components_.selection_revision();
        reply.message = request.undo ? "Undid mesh operation" : "Updated mesh operation";
    }
    last_result_ = reply.message;
    return reply;
}
DebugReport MeshEditing::debug_report() const {
    return {.name="mesh", .role="blueprint component authoring", .situation=std::string(situation_),
        .received={{"input allowed",debug_bool(input_allowed_)},
                   {"last dispatched blueprint",target_ ? std::to_string(static_cast<u32>(*target_)) : "none"}},
        .owned={{"operation options available",debug_bool(operation_.options_available())}},
        .observations={{"last operation result",last_result_.empty() ? "none" : last_result_}},
        .children={components_.debug_report()}};
}
} // namespace editor_example
