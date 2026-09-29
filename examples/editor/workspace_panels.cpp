#include "workspace_panels.hpp"
namespace editor_example {
using namespace vng;
namespace {
template<class T> void place(T& widget,ui::Rect b) { widget.position({b.x,b.y}).width(b.width).height(b.height); }
}
WorkspacePanels::WorkspacePanels(ui::Screen& screen,const ViewportState& view_state) {
    scene_panel = screen.column().position({16, 80}).width(232).height(580).padding(12).gap(6)
                    .scrollbar(ui::ScrollBar::always);
    constexpr std::array divider_names{"Resize scene and region lists", "Resize region and blueprint lists",
        "Resize blueprint list and tools", "Resize tools and region creation"};
    for (std::size_t i=0; i<sidebar_sections.size(); ++i) {
        sidebar_sections[i] = scene_panel.column().padding(0).gap(4).scrollbar(ui::ScrollBar::automatic);
        if(i<section_splitters.size()) section_splitters[i]=scene_panel.splitter(divider_names[i]);
    }
    sidebar_sections[0].label("SCENE INSTANCES").height(26);
    scene_list = sidebar_sections[0].column().padding(0).gap(4).scrollbar(ui::ScrollBar::always);
    sidebar_sections[1].label("REGION INSTANCES").height(26);
    region_list = sidebar_sections[1].column().padding(0).gap(4).scrollbar(ui::ScrollBar::always);
    sidebar_sections[2].label("BLUEPRINTS").height(26);
    blueprint_list = sidebar_sections[2].column().padding(0).gap(4).scrollbar(ui::ScrollBar::always);
    manipulation_panel = sidebar_sections[3];
    manipulation_panel.label("MANIPULATION").height(26);
    sidebar_sections[4].label("CREATE REGION").height(26);
    interaction.emplace(
        manipulation_panel.dropdown<InteractionMode>("Mode", {{InteractionMode::objects, "Instances"},
                                                {InteractionMode::vertices, "Mesh edit"}})
            .value(view_state.mode == ViewMode::mesh ? InteractionMode::vertices
                                                : InteractionMode::objects));
    gizmo_host=manipulation_panel.column().height(36).padding(0).gap(0);
    pivot_host=manipulation_panel.column().padding(0).gap(0);
    properties_panel = screen.column().position({1032, 80}).width(312).height(580).padding(10).gap(8).scrollbar(ui::ScrollBar::automatic);
    region_inspector = screen.column().padding(10).gap(8).scrollbar(ui::ScrollBar::automatic).visible(false);
    object_title = properties_panel.label("INSTANCE / worker controls").height(26);
    blueprint_title = properties_panel.label("").height(24);
    transform_hint = properties_panel.label("").height(24);
    blueprint_panel_host=properties_panel.column().padding(0).gap(6);
    vertex_tools = properties_panel.column().padding(0).gap(6).visible(false);
    vertex_tools.label("BLUEPRINT / local XYZ");
    selected = vertex_tools.label("Click a preview vertex");
    xyz = vertex_tools.row().padding(0).gap(4);
    x = xyz.text_input().width(90);
    y = xyz.text_input().width(90);
    z = xyz.text_input().width(90);
    weld = vertex_tools.checkbox("Weld coincident").value(view_state.weld);
    apply_vertex = vertex_tools.button("Apply vertex position");
    publish_mesh = vertex_tools.button("Apply mesh to scene");
    save_mesh_draft = vertex_tools.button("Save on disk");
    draft_status = vertex_tools.label("No unapplied mesh changes").height(26);
    nudges = vertex_tools.row().padding(0).gap(5);
    minus = nudges.button("X - 0.1").width(130);
    plus = nudges.button("X + 0.1").width(130);
    inspector_tabs = screen.row().padding(0).gap(6);
    show_scene = inspector_tabs.button("Scene");
    show_keyframe = inspector_tabs.button("Keyframe values");
    show_base = inspector_tabs.button("Instance properties");
    sidebar_tab = view_state.mode == ViewMode::mesh ? SidebarTab::properties : SidebarTab::scene;
    keyframe_inspector = screen.column();
    keyframe_list = screen.column();
    panels_splitter = screen.root().splitter("Resize scene and keyframe panels").height(12);
    timeline_host = screen.column();

    inspector.emplace(properties_panel);
}
void WorkspacePanels::layout(EditorLayout& geometry) {
    sidebar_sizing.apply(geometry);
    place(scene_panel,geometry.scene);
    const bool vertices=interaction->value()==InteractionMode::vertices;
    const auto panels = SidebarSizing::panels(geometry);
    place(panels_splitter, {geometry.scene.x, geometry.scene.y + geometry.scene.height, geometry.scene.width, 12});
    panels_splitter.range(panels.minimum, panels.maximum);
    if (!panels_splitter.isPressed()) panels_splitter.value(geometry.scene.height);
    const auto heights = sidebar_sizing.sections(geometry);
    for (std::size_t i=0; i<sidebar_sections.size(); ++i) sidebar_sections[i].height(heights[i]);
    scene_list.height(heights[0]-30); region_list.height(heights[1]-30); blueprint_list.height(heights[2]-30);
    for (std::size_t i=0; i<section_splitters.size(); ++i) {
        const auto limits = sidebar_sizing.divider(i,geometry);
        section_splitters[i].range(limits.minimum,limits.maximum);
        if(!section_splitters[i].isPressed()) section_splitters[i].value(heights[i]);
    }
    vertex_tools.visible(vertices);
    place(keyframe_list, geometry.keyframes);
    place(inspector_tabs, geometry.tabs);
    const auto tab_width = geometry.tabs.width - 12;
    show_scene.width(tab_width * .18F);
    show_keyframe.width(tab_width * .38F);
    show_base.width(tab_width * .44F);
    place(properties_panel, geometry.inspector);
    place(region_inspector, geometry.inspector);
    for (auto* field : {&x, &y, &z})
        field->width(std::max(60.0F, (xyz.bounds().width - 8) / 3));
    place(keyframe_inspector, geometry.inspector);
    place(timeline_host, geometry.timeline);
}
bool WorkspacePanels::resize(const EditorLayout& geometry) {
    if (panels_splitter.editStarted() || std::ranges::any_of(section_splitters,[](auto s){return s.editStarted();}))
        sizing_before_drag = sidebar_sizing;
    const auto panel_height = panels_splitter.changedValue();
    if (panel_height) sidebar_sizing.resize_panels(*panel_height, geometry);
    bool section_changed{};
    for(std::size_t i=0;i<section_splitters.size();++i) if(auto height=section_splitters[i].changedValue()) {
        sidebar_sizing.resize_section(i,*height,geometry); section_changed=true;
    }
    const bool resizing = panels_splitter.isPressed() || std::ranges::any_of(section_splitters,[](auto s){return s.isPressed();});
    const bool resize_committed = panels_splitter.editCommitted() || std::ranges::any_of(section_splitters,[](auto s){return s.editCommitted();});
    // Restore the choice of automatic defaults too, not just today's pixel
    // heights. Disabling/hiding or a changed range can cancel before update.
    const bool resize_cancelled = sizing_before_drag && !resizing && !resize_committed;
    if (resize_cancelled) sidebar_sizing = *sizing_before_drag;
    if (!resizing) sizing_before_drag.reset();
    return panel_height.has_value() || section_changed || resize_cancelled;
}
void WorkspacePanels::show(bool custom) {
    const bool scene = sidebar_tab == SidebarTab::scene;
    const bool properties = sidebar_tab == SidebarTab::properties;
    scene_panel.visible(scene);
    keyframe_list.visible(scene);
    panels_splitter.visible(scene);
    properties_panel.visible(properties && !custom);
    region_inspector.visible(properties && custom);
    keyframe_inspector.visible(sidebar_tab == SidebarTab::keyframe);
    show_scene.selected(scene);
    show_keyframe.selected(sidebar_tab == SidebarTab::keyframe);
    show_base.selected(properties);
}
DebugReport WorkspacePanels::debug_report() const {
    return {.name="sidebar",.role="sidebar widgets, tabs and local resize drafts",
        .situation=sidebar_tab==SidebarTab::scene?"Scene":sidebar_tab==SidebarTab::properties?"Properties":"Keyframe",
        .owned={{"resizing",debug_bool(sizing_before_drag.has_value())}}};
}
}
