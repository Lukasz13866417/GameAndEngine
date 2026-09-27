#pragma once
#include "editing_session.hpp"
#include "timeline_panel.hpp"

namespace editor_example {
struct TimelineHosts {
    vng::ui::Container strip, list, inspector, actions, menu;
};
struct TimelinePresentation { vng::Vec2 screen; bool compact{}; };
struct TimelineInput {
    bool synchronize{}, clear_selection{}, reset{}, cancel{}, close_menu{}, poll{};
    std::optional<std::span<const vng::f32>> select{};
    std::optional<std::span<const vng::u64>> objects{};
    std::optional<vng::u64> focus{};
    std::optional<TimelinePresentation> presentation{};
    std::span<const vng::input::Event> unhandled{}, raw{};
    std::optional<bool> inspector_visible{};
};
struct TimelineContext {
    TimelineInput input{};
    std::optional<bool> enabled{};
};
struct TimelineReply {
    bool interacted{}, authored{}, playback_changed{}, select_inspector{}, selection_requested{};
    std::optional<std::pair<vng::u32,vng::editor::SelectionMode>> select_instance{};
    std::string message{};
    std::optional<TimelineAction> action{};
};

// Timeline owns drafts and selection presentation. It reports local actions;
// its workspace parent executes them and supplies the result downward.
class TimelineEditingUI final {
public:
    struct Available {};
    struct Unavailable {};
    TimelineEditingUI(const EditingSession& editing, TimelineHosts hosts)
        : editing_(editing), panel_(hosts.strip,hosts.list,hosts.inspector,hosts.actions,hosts.menu) {}
    [[nodiscard]] const TimelinePanel& view() const { return panel_; }
    [[nodiscard]] bool enabled() const { return enabled_; }
    [[nodiscard]] DebugReport debug_report() const;
    [[nodiscard]] std::string debug_string() const { return debug_report().string(); }
private:
    friend class EditingWorkspaceUI;
    TimelineReply handle(const Available&,const TimelineContext&);
    TimelineReply handle(const Unavailable&,const TimelineContext&);
    void present(const TimelineInput&);
    TimelineReply propose(TimelineAction);
    TimelineReply accept(const TimelineAction&, const vng::content::Result<bool>&);
    const EditingSession& editing_;
    TimelinePanel panel_;
    bool enabled_{true};
    bool inspector_visible_{};
    std::optional<vng::u64> pending_focus_;
    std::string last_result_;
};
} // namespace editor_example
