#pragma once
#include "project.hpp"
#include "editor_layout.hpp"
#include "sidebar_sizing.hpp"
#include "inspector_panel.hpp"
#include "component_debug.hpp"
#include <vng/ui/ui.hpp>

namespace editor_example {
enum class InteractionMode { objects, vertices };
enum class SidebarTab { scene, keyframe, properties };

// Owns sidebar widgets, tab presentation and splitter drafts. The workspace
// coordinates edits and selection; neither the app nor sibling panels get
// mutable widget handles.
class WorkspacePanels final {
public:
    WorkspacePanels(vng::ui::Screen&, const ViewportState&);
    [[nodiscard]] DebugReport debug_report() const;
private:
    friend class EditingWorkspaceUI;
    void layout(EditorLayout&);
    bool resize(const EditorLayout&);
    void show(bool custom_inspector);
    vng::ui::Container scene_panel,scene_list,region_list,blueprint_list,manipulation_panel;
    vng::ui::Container gizmo_host,pivot_host,properties_panel,region_inspector,blueprint_panel_host;
    vng::ui::Container vertex_tools,xyz,nudges,inspector_tabs,keyframe_inspector,keyframe_list,timeline_host;
    std::array<vng::ui::Container,SidebarSizing::section_count> sidebar_sections;
    std::array<vng::ui::Splitter,SidebarSizing::section_count-1> section_splitters;
    vng::ui::Splitter panels_splitter;
    std::optional<vng::ui::Dropdown<InteractionMode>> interaction;
    vng::ui::Label object_title,blueprint_title,transform_hint,selected,draft_status;
    vng::ui::TextField x,y,z;
    vng::ui::Checkbox weld;
    vng::ui::Button apply_vertex,publish_mesh,save_mesh_draft,minus,plus,show_scene,show_keyframe,show_base;
    std::optional<InspectorPanel> inspector;
    SidebarTab sidebar_tab{SidebarTab::scene};
    SidebarSizing sidebar_sizing;
    std::optional<SidebarSizing> sizing_before_drag;
};
}
