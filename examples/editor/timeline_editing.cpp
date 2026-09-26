#include "timeline_editing.hpp"
#include "animation.hpp"

namespace editor_example {
void TimelineEditing::present(const TimelineInput& input) {
    if(input.inspector_visible) inspector_visible_=*input.inspector_visible;
    if(input.reset || input.clear_selection) pending_focus_.reset();
    if(input.reset) panel_.reset();
    if(input.cancel) panel_.cancel();
    if(input.close_menu) panel_.close_menu();
    if(input.clear_selection) panel_.clear_selection();
    if(input.select) panel_.select_keyframes(*input.select);
    if(input.objects) panel_.selected_objects(*input.objects);
    if(input.focus) {
        if(*input.focus) pending_focus_=input.focus;
        else pending_focus_.reset();
    }
    if(input.presentation) {
        panel_.layout_menu(input.presentation->screen);
        panel_.compact(input.presentation->compact);
    }
    if(input.synchronize) panel_.show(editing_.state());
    // A hidden tab has no layout and cannot accept focus/reveal. Keep the
    // latest semantic request, not stale widget bounds, until its parent shows it.
    if(inspector_visible_ && pending_focus_) {
        panel_.focus_object(*pending_focus_);
        pending_focus_.reset();
    }
    if(input.clear_selection || input.select || input.reset || input.synchronize)
        editing_.select_keyframe(panel_.selected_keyframe());
}
TimelineReply TimelineEditing::handle(const Available&,const TimelineContext& context) {
    enabled_=true;
    panel_.actions_enabled(true);
    present(context.input);
    TimelineReply reply;
    if(context.input.poll && !editing_.awaiting_remote()) {
        if(auto action=panel_.poll(context.input.unhandled,context.input.raw)) reply=apply(*action);
        // Ctrl-click/scrub can clear selection without changing the playhead.
        editing_.select_keyframe(panel_.selected_keyframe());
    }
    return reply;
}
TimelineReply TimelineEditing::handle(const Unavailable&,const TimelineContext& context) {
    enabled_=false;
    panel_.actions_enabled(false);
    present(context.input);
    return {};
}
TimelineReply TimelineEditing::apply(const TimelineAction& action) {
    auto& view=editing_.viewport();
    const auto& state=editing_.state();
    TimelineReply reply{.interacted=true};
    if(action.kind==TimelineAction::Kind::select_object) {
        reply.selection_requested=true;
        if(action.object==camera_animation_object) {
            if(const auto* camera=active_camera(state,view.time))
                reply.select_instance={{camera->id,action.selection_mode}};
            else panel_.focus_object(camera_animation_object);
        } else reply.select_instance={{static_cast<vng::u32>(action.object),action.selection_mode}};
        return reply;
    }
    reply.select_inspector=action.kind==TimelineAction::Kind::add || panel_.selected_keyframe().has_value();
    const auto previous_time=view.time;
    const auto previous_paused=view.paused;
    if(action.kind==TimelineAction::Kind::seek) {
        view.time=action.time;
        view.paused=true;
        panel_.show(state);
    } else {
        vng::content::Result<bool> result{false};
        switch(action.kind) {
        case TimelineAction::Kind::add: result=editing_.add_keyframe(action.time); break;
        case TimelineAction::Kind::edit:
            result=editing_.update_keyframe(action.time,action.destination,action.name,action.values); break;
        case TimelineAction::Kind::apply_range:
            result=editing_.apply_keyframe_range(action.time,action.destination,action.changes); break;
        case TimelineAction::Kind::erase:
            result=editing_.erase_keyframes(action.selection.empty()
                ? std::span{&action.time,1} : std::span{action.selection}); break;
        case TimelineAction::Kind::duration: result=editing_.duration(action.time); break;
        case TimelineAction::Kind::seek: case TimelineAction::Kind::select_object: break;
        }
        if(!result) { last_result_=result.error().message; panel_.error(last_result_); }
        else {
            reply.authored=*result;
            last_result_=*result ? "Applied authoring intent" : "No authored change";
            if(action.kind==TimelineAction::Kind::apply_range)
                reply.message="Applied edited fields to keyframes from " + std::to_string(action.time) +
                    " to " + std::to_string(action.destination) + " seconds / Undo restores the whole range";
            panel_.show(state);
        }
    }
    reply.playback_changed=view.time!=previous_time || view.paused!=previous_paused;
    return reply;
}
DebugReport TimelineEditing::debug_report() const {
    const auto stats=panel_.statistics();
    return {.name="timeline",.role="keyframe drafts, selection and authoring",.situation=enabled_?"Available":"Unavailable",
        .owned={{"selected keys",std::to_string(panel_.selected_keyframes().size())},
            {"active key",panel_.selected_keyframe()?std::to_string(*panel_.selected_keyframe()):"none"},
            {"pointer gesture",debug_bool(panel_.dragging())}, {"range menu",debug_bool(panel_.menu_open())},
            {"inspector visible (parent)",debug_bool(inspector_visible_)},
            {"pending object focus",pending_focus_?std::to_string(*pending_focus_):"none"}},
        .observations={{"last operation result",last_result_},
            {"document refreshes",std::to_string(stats.document_refreshes)},
            {"value refreshes",std::to_string(stats.value_refreshes)},
            {"row visibility updates",std::to_string(stats.row_visibility_updates)},
            {"row layout updates",std::to_string(stats.row_layout_updates)}},
        .children={panel_.debug_report()}};
}
} // namespace editor_example
