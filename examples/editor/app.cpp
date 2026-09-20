#include "app.hpp"
#include "mesh_tools.hpp"
#include "mesh_operation_tool.hpp"
#include "tool_panel.hpp"
#include "mesh_overlay.hpp"
#include "document_patch.hpp"
#include "project.hpp"
#include "effect_presets.hpp"
#include "inspector_panel.hpp"
#include "blueprint_mesh_panel.hpp"
#include "translation_tool.hpp"
#include "rotation_interaction.hpp"
#include "editing_session.hpp"
#include "instance_controls.hpp"
#include "instance_list.hpp"
#include "blueprint_list.hpp"
#include "panel_flyout.hpp"
#include "scale_tool.hpp"
#include "world_bounds_tool.hpp"
#include "world_bounds_panel.hpp"
#include "region_editor.hpp"
#include "viewport_interaction.hpp"
#include <numbers>
#include "edit_clipboard.hpp"
#include "edit_shortcuts.hpp"
#include "preview_updates.hpp"
#include "viewport_session.hpp"
#include "presented_view.hpp"
#include "timing_panel.hpp"
#include "preview_fps.hpp"
#include "navigation.hpp"
#include "mesh_navigation.hpp"
#include "scroll_trace.hpp"
#include "selection.hpp"
#include "vertex_drag.hpp"
#include "viewport_shortcuts.hpp"
#include "file_shortcuts.hpp"
#include "scene_file.hpp"
#include "save_dialog.hpp"
#include "import_dialog.hpp"
#include "open_scene_dialog.hpp"
#include "camera_panel.hpp"
#include "camera_glyph.hpp"
#include "mesh_import_view.hpp"
#include "animation.hpp"
#include "preview_values.hpp"
#include "preview_mailbox.hpp"
#include "timeline_panel.hpp"
#include "editor_layout.hpp"
#include "sidebar_sizing.hpp"
#include "preview_viewport.hpp"
#include "settings_panel.hpp"
#include "camera_preferences.hpp"
#include "viewport_window.hpp"
#include "ui_scale.hpp"
#include "number_control.hpp"
#include "view_selector.hpp"
#include "gizmo_selector.hpp"
#include "rotation_pivot_controls.hpp"
#include "toolbar_row.hpp"
#include "delete_shortcut.hpp"
#include "box_selection.hpp"
#include "automation.hpp"
#include "../support/glfw_opengl_session.hpp"
#include "../support/presentation.hpp"
#include <vng/editor/preview.hpp>
#include <vng/opengl/ui_renderer.hpp>
#include <charconv>
#include <chrono>
#include <iostream>
#include <map>
#include <set>
#include <thread>

namespace editor_example {
using namespace vng;
namespace {
enum class InteractionMode { objects, vertices };
enum class DeleteTarget { object, keyframe };
enum class SidebarTab { scene, keyframe, properties };
int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}
std::optional<f32> number(std::string_view text) {
    f32 out{};
    const auto [p, e] = std::from_chars(text.data(), text.data() + text.size(), out);
    if (e != std::errc{} || p != text.data() + text.size() || !std::isfinite(out))
        return {};
    return out;
}
Vec3 add(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 subtract(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
} // namespace
int run(const Options& options) {
    if (options.automation && !options.preferences)
        return fail("Automation requires a private preferences path");
    const auto preferences_path = options.preferences.value_or(settings_path());
    const auto preferences = load_settings(preferences_path);
    auto settings = preferences ? *preferences : Settings{};
    if (!preferences) std::cerr << preferences.error().message << '\n';
    SceneFile scene_file;
    auto mesh = editor::EditableMesh::load(options.mesh.value_or(VNG_EDITOR_MESH_PATH));
    if (!mesh)
        return fail(mesh.error().message);
    State initial{.document = {.mesh = std::move(*mesh)}};
    if (options.scene) {
        auto loaded = scene_file.load(*options.scene);
        if (!loaded)
            return fail(loaded.error().message);
        initial = std::move(*loaded);
        // Revisions identify this editor session, not the on-disk file's age.
        initial.document.revision = 1;
    }
    if (options.mesh_view) {
        if (auto inspected = inspect_mesh(initial, initial.viewport.inspected_mesh); !inspected)
            return fail(inspected.error().message);
    }
    EditingSession editing{std::move(initial), std::move(scene_file)};
    if (auto configured = editing.timeline_track_limit(settings.timeline_track_limit); !configured)
        return fail(configured.error().message);
    if (auto configured = editing.instance_limit(settings.instance_limit); !configured)
        return fail(configured.error().message);
    const auto& state = editing.state();
    auto& view_state = editing.viewport();
    auto font = text::Font::load(VNG_EXAMPLE_FONT_PATH);
    if (!font)
        return fail(font.error().message);
    editor::Selection<u32> selected_instances;
    if(view_state.selected_object) selected_instances.select(view_state.selected_object);
    auto app = example::GlfwOpenGLSession::create(
        {.width = 1600,
         .height = 1000,
         .title = "Vibe / Mesh + effect + scene editor",
         .visible = !options.automation,
         .resizable = true,
         .maximized = !options.windowed && !options.automation},
        {.debug = true,
         .samples = 0,
         .default_framebuffer_encoding = render::ColorEncoding::linear},
        {.vsync = options.automation ? window::VSync::off : settings.vsync});
    if (!app) {
        const auto code = example::fail(app.error());
        return options.automation ? 77 : code;
    }
    auto& device = app->device();
    auto& window = app->window();
    ViewportWindow viewport_window{window};
    input::Frame viewport_raw;
    bool toggle_viewport{};
    UiSurfaceSize controls_size, detached_size;
    if (options.fullscreen && !options.automation)
        if (auto mode = window.set_fullscreen(true); !mode)
            return fail(mode.error().message);
    auto geometry = editor_layout({1600, 1000}, options.mesh_view);
    auto preview_bounds = geometry.viewport;
    auto renderer = render::make_ui_renderer(device);
    if (!renderer)
        return fail(renderer.error().message);
    auto overlay = MeshOverlay::create(device);
    if (!overlay) return fail(overlay.error().message);
    auto display = example::DisplaySurface::create(device, window.framebuffer_extent(), true);
    if (!display)
        return fail(display.error().message);
    const auto build_command = options.automation ? std::vector<std::string>{"/usr/bin/true"}
        : std::vector<std::string>{"cmake", "--build", VNG_EDITOR_BUILD_DIR, "--target", "vng_editor_worker", "-j", "2"};
    auto session = editor::preview::PreviewSession::create(
        {.build_command = build_command,
         .build_directory = VNG_EDITOR_SOURCE_DIR,
         .worker_executable = VNG_EDITOR_WORKER_PATH,
         .max_width = preview_capacity.width,
         .max_height = preview_capacity.height,
         .startup_timeout = std::chrono::seconds(120)});
    if (!session)
        return fail(session.error().message);

    auto editor_theme=ui::dark_theme_values(*font);
    editor_theme.panel.w=1; // Overlapping flyouts must not ghost the text underneath.
    auto theme=std::make_shared<ui::BasicTheme>(std::move(editor_theme));
    ui::Screen screen{theme};
    ui::Screen viewport_screen{theme};
    // Popup layer composes above panels when docked, above the image when not.
    ui::Screen viewport_popups{theme};
    auto toolbar = screen.column().position({16,12}).width(1328).height(100).padding(8).gap(6);
    auto top = toolbar.row().height(36).padding(0).gap(6);
    auto secondary = toolbar.row().height(36).padding(0).gap(6);
    auto scene_controls = secondary.row().height(36).padding(0).gap(0);
    auto keyframe_actions = secondary.row().height(36).padding(0).gap(6);
    auto scene_panel = screen.column().position({16, 80}).width(232).height(580).padding(12).gap(6)
                    .scrollbar(ui::ScrollBar::always);
    std::array<ui::Container, SidebarSizing::section_count> sidebar_sections;
    std::array<ui::Splitter, SidebarSizing::section_count-1> section_splitters;
    constexpr std::array divider_names{"Resize scene and region lists", "Resize region and blueprint lists",
        "Resize blueprint list and tools", "Resize tools and region creation"};
    for (std::size_t i=0; i<sidebar_sections.size(); ++i) {
        sidebar_sections[i] = scene_panel.column().padding(0).gap(4).scrollbar(ui::ScrollBar::automatic);
        if(i<section_splitters.size()) section_splitters[i]=scene_panel.splitter(divider_names[i]);
    }
    sidebar_sections[0].label("SCENE INSTANCES").height(26);
    auto scene_list = sidebar_sections[0].column().padding(0).gap(4).scrollbar(ui::ScrollBar::always);
    sidebar_sections[1].label("REGION INSTANCES").height(26);
    auto region_list = sidebar_sections[1].column().padding(0).gap(4).scrollbar(ui::ScrollBar::always);
    sidebar_sections[2].label("BLUEPRINTS").height(26);
    auto blueprint_list = sidebar_sections[2].column().padding(0).gap(4).scrollbar(ui::ScrollBar::always);
    auto manipulation_panel = sidebar_sections[3];
    manipulation_panel.label("MANIPULATION").height(26);
    sidebar_sections[4].label("CREATE REGION").height(26);
    auto interaction =
        manipulation_panel.dropdown<InteractionMode>("Mode", {{InteractionMode::objects, "Instances"},
                                                {InteractionMode::vertices, "Mesh edit"}})
            .value(view_state.mode == ViewMode::mesh ? InteractionMode::vertices
                                                : InteractionMode::objects);
    GizmoSelector gizmo_mode{manipulation_panel.column().height(36).padding(0).gap(0)};
    RotationPivotControls rotation_pivot{manipulation_panel};
    auto image = viewport_screen.root()
                     .image()
                     .position({preview_bounds.x, preview_bounds.y})
                     .width(preview_bounds.width)
                     .height(preview_bounds.height);
    auto caption = viewport_screen.column().position({264, 80}).width(752).height(54).padding(4).gap(2);
    // Which camera the mouse moves is the first thing the header answers.
    auto caption_row = caption.row().height(22).padding(0).gap(10);
    auto camera_label = caption_row.label("EDITOR CAMERA / private view").width(430).height(22);
    auto frame_label = caption_row.label("Starting isolated OpenGL preview...").width(380).height(22);
    auto analysis_label = caption.label("Color buffer arrives through shared memory.").height(22);
    MeshNavigationControls mesh_navigation{viewport_popups.root()};
    auto properties_panel = screen.column().position({1032, 80}).width(312).height(580).padding(10).gap(8).scrollbar(ui::ScrollBar::automatic);
    auto region_inspector = screen.column().padding(10).gap(8).scrollbar(ui::ScrollBar::automatic).visible(false);
    auto object_title = properties_panel.label("INSTANCE / worker controls").height(26);
    auto blueprint_title = properties_panel.label("").height(24);
    auto transform_hint = properties_panel.label("").height(24);
    BlueprintMeshPanel blueprint_mesh_panel{properties_panel.column().padding(0).gap(6),editing};
    std::string last_blueprint_status;
    auto vertex_tools = properties_panel.column().padding(0).gap(6).visible(false);
    vertex_tools.label("BLUEPRINT / local XYZ");
    auto selected = vertex_tools.label("Click a preview vertex");
    auto xyz = vertex_tools.row().padding(0).gap(4);
    auto x = xyz.text_input().width(90);
    auto y = xyz.text_input().width(90);
    auto z = xyz.text_input().width(90);
    auto weld = vertex_tools.checkbox("Weld coincident").value(view_state.weld);
    auto apply_vertex = vertex_tools.button("Apply vertex position");
    auto publish_mesh = vertex_tools.button("Apply mesh to scene");
    auto save_mesh_draft = vertex_tools.button("Save on disk");
    auto draft_status = vertex_tools.label("No unapplied mesh changes").height(26);
    auto nudges = vertex_tools.row().padding(0).gap(5);
    auto minus = nudges.button("X - 0.1").width(130);
    auto plus = nudges.button("X + 0.1").width(130);
    // Camera actions float in the viewport beside the selected camera's glyph,
    // so they stay reachable whatever the sidebar shows.
    CameraPanel camera_panel{viewport_popups.column().padding(8).gap(6)};
    std::optional<CameraVisit> camera_visit;
    const auto inspecting = [&] { return camera_visit && !camera_visit->editable; };
    InspectorPanel inspector{properties_panel};
    auto inspector_tabs = screen.row().padding(0).gap(6);
    auto show_scene = inspector_tabs.button("Scene");
    auto show_keyframe = inspector_tabs.button("Keyframe values");
    auto show_base = inspector_tabs.button("Instance properties");
    auto sidebar_tab = view_state.mode == ViewMode::mesh ? SidebarTab::properties : SidebarTab::scene;
    auto keyframe_inspector = screen.column();
    auto keyframe_list = screen.column();
    auto panels_splitter = screen.root().splitter("Resize scene and keyframe panels").height(12);
    SidebarSizing sidebar_sizing;
    std::optional<SidebarSizing> sizing_before_drag;
    auto timeline_host = screen.column();
    auto files = screen.row().position({16, 928}).width(1328).height(50).padding(6).gap(8);
    auto path = files.text_input().width(784).value(editing.path() ? editing.path()->string()
                                                                      : "editor_scene.vscene");
    auto export_asset = files.button("Export new .vmesh").width(200);
    const auto exporting_effect=[&] {
        const auto* instance=find_instance(state,state.viewport.selected_object);
        return view_state.mode!=ViewMode::mesh && instance && effect_blueprint_settings(state,instance->blueprint);
    };
    auto fullscreen =
        files.button(window.fullscreen() ? "Windowed / F11" : "Fullscreen / F11").width(182);
    auto status_host = screen.column().position({16, 990}).width(1328).height(42).padding(0);
    auto status =
        status_host.label("Save / Ctrl+S: current scene. Save As / Ctrl+Shift+S: choose a file.")
            .height(26);

    // Application/file actions and scene-view controls have separate rows.
    // Each retained group moves into its row's overflow menu when necessary.
    ToolbarRow main_bar{top, screen.column(), "More..."};
    ToolbarRow scene_bar{scene_controls, screen.column(), "Tools..."};
    auto load = main_bar.item(166).button("Open scene...");
    auto save = main_bar.item(64).button("Save");
    auto save_as = main_bar.item(112).button("Save As...");
    auto show_import = main_bar.item(150).button("Import...");
    auto undo = main_bar.item(64).button("Undo");
    auto redo = main_bar.item(64).button("Redo");
    auto show_logs = main_bar.item(64).button("Logs");
    auto show_settings = main_bar.item(100).button("Settings");
    auto reload = main_bar.item(122).button("Reload C++");
    auto pause = main_bar.item(174).button(view_state.paused ? "Play (in editor)" : "Pause (in editor)");
    auto play = main_bar.item(204).button("Play (independent)");
    auto pop_viewport = main_bar.item(188).button("Pop out viewport");
    auto scale_controls = main_bar.item(204).row().height(36).padding(0).gap(4);
    auto smaller_ui = scale_controls.button("UI -").width(62);
    auto scale_label = scale_controls.label(std::to_string(settings.ui_scale_percent)+"%").width(72);
    auto larger_ui = scale_controls.button("UI +").width(62);
    auto view_host = scene_bar.item(246);
    ViewSelector view{view_host, state};
    auto show_camera = scene_bar.item(178).button("Camera settings");
    auto show_fps = scene_bar.item(80).checkbox("FPS").value(false);
    WorldBoundsPanel bounds_panel{scene_bar.item(396), manipulation_panel};
    auto region_controls = scene_bar.item(604);
    auto gizmo_only = scene_bar.item(238).checkbox("Selected: gizmo only").value(view_state.gizmo_only);
    auto debug_link = scene_bar.item(138).checkbox("Debug link").value(options.debug_link);
    auto diagnostic = scene_bar.item(114).checkbox("Face IDs").value(options.diagnostic);
    auto show_instances = scene_bar.item(154).button("Instance list");
    auto show_blueprints = scene_bar.item(162).button("Blueprint list");

    auto log_panel = screen.column()
                         .position({264, 294})
                         .width(752)
                         .height(366)
                         .padding(10)
                         .gap(5)
                         .visible(false);
    log_panel.label("BUILD / WORKER LOG - select text to copy").height(26);
    auto diagnostic_tabs = log_panel.row().padding(0).gap(8);
    auto worker_log_tab = diagnostic_tabs.button("Worker log").width(160);
    auto timing_tab = diagnostic_tabs.button("Interaction timing").width(190);
    auto log_text = log_panel.text_area().height(302);
    auto timing_host = log_panel.column().padding(0).gap(8);
    TimingPanel timing_panel{timing_host};
    auto dialog_host = screen.column().padding(16).gap(10);
    SaveSceneDialog save_dialog{dialog_host};
    auto settings_host = screen.column();
    SettingsPanel settings_panel{settings_host};
    if (options.settings) settings_panel.open(settings, state.document.timeline.tracks().size(), state.document.instances.size());
    auto import_host = screen.column();
    ImportDialog import_dialog{import_host};
    auto open_host = screen.column();
    OpenSceneDialog open_dialog{open_host};
    auto mesh_menu_host = viewport_popups.column();
    MeshTools mesh_tools{vertex_tools, mesh_menu_host};
    ViewportInteraction viewport_interaction{editing, region_controls, sidebar_sections[4], region_inspector, viewport_popups.column()};
    auto& regions = viewport_interaction.regions;
    MeshOperationTool mesh_operation_options{editing};
    ToolPanel tool_panel{viewport_popups.column()};
    auto camera_menu = screen.column().padding(12).gap(6).scrollbar(ui::ScrollBar::automatic).visible(false);
    camera_menu.label("CAMERA SETTINGS").height(28);
    auto close_camera = camera_menu.button("Close camera settings").height(32);
    bool camera_open{};
    NumberControl yaw{camera_menu, "Orbit", -180, 180, preview_camera_pose(state,view_state.time).yaw};
    NumberControl pitch{camera_menu, "Elevation", -camera_max_pitch, camera_max_pitch, preview_camera_pose(state,view_state.time).pitch};
    NumberControl orbit_distance{camera_menu, "Orbit distance", camera_min_distance, camera_max_distance, preview_camera_pose(state,view_state.time).distance};
    orbit_distance.slider_range(settings.orbit_distance.minimum, settings.orbit_distance.maximum);
    NumberControl optical_zoom{camera_menu, "Zoom", camera_min_zoom, camera_max_zoom, preview_camera_pose(state,view_state.time).zoom};
    optical_zoom.slider_range(camera_min_zoom, 10.F);
    auto move_camera_scroll = camera_menu.checkbox("Scroll moves camera").value(settings.scroll_moves_camera);
    auto walk_button = camera_menu.button("Walk camera");
    camera_menu.label("Wheel / Ctrl+MMB").height(26);
    camera_menu.label("Left Alt: 4x pan / zoom speed").height(26);
    CameraPreferences camera_preferences{camera_menu};
    PanelFlyout instance_flyout{screen.column(), "INSTANCE LIST", "Close instance list"};
    PanelFlyout blueprint_flyout{screen.column(), "BLUEPRINT LIST", "Close blueprint list"};
    InstanceList floating_instances{instance_flyout.body(), InstanceList::Filter::scene};
    BlueprintList floating_blueprints{blueprint_flyout.body()};
    const auto close_list_flyouts = [&] { instance_flyout.close(); blueprint_flyout.close(); };
    TimelinePanel timeline{timeline_host, keyframe_list, keyframe_inspector, keyframe_actions, screen.column()};
    auto show_timeline = [&] {
        timeline.show(state);
        editing.select_keyframe(timeline.selected_keyframe());
    };
    // Browse the authoring library, not the build's small set of demo copies.
    // New assets should be discoverable without copying them or reconfiguring.
    auto import_directory = std::filesystem::path{VNG_EDITOR_SOURCE_DIR} / "examples/assets";
    if (options.import_dialog) {
        settings_panel.close();
        import_dialog.open(import_directory);
    }
    const auto modal_visible = [&] {
        return save_dialog.visible() || settings_panel.visible() || import_dialog.visible() || open_dialog.visible() ||
               mesh_tools.menu_open() || regions.menu_open();
    };
    std::string displayed_logs;
    bool logs_visible{};
    PreviewUpdates updates;
    editor::InteractionTimings timings;
    PreviewFps preview_fps;
    ScrollTrace scroll_trace{ScrollTrace::requested() ? &std::cerr : nullptr};
    if (scroll_trace.enabled()) std::cerr << "[scroll/ui] trace enabled; input -> target -> queued -> presented\n";
    editor::InteractionOrigin document_origin, view_origin;
    std::map<u64,u64> sent_views;
    std::map<u64,u64> sent_mesh_visibility;
    u64 traced_revision{};
    editor::preview::FrameInfo presented_info;
    gfx::Camera image_camera;
    u64 minimum_inspector_sequence{};
    auto& translation = viewport_interaction.translation;
    auto& box_tool = viewport_interaction.selection_box;
    editor::Selection<u32> box_instances;
    std::vector<u32> box_mesh;
    gfx::Camera box_camera;
    Extent2D box_extent{};
    ui::Rect box_viewport{};
    auto& rotation = viewport_interaction.rotation;
    auto& scaling = viewport_interaction.scale;
    auto& instance_transform = viewport_interaction.instances;
    auto& bounds_tool = viewport_interaction.bounds;
    std::optional<editor::Schema> position_schema;
    bool selection_overlay_dirty{};
    std::shared_ptr<const gfx::ImageData> preview_pixels;
    u64 ui_frame{};
    std::string automation_clipboard;
    auto& navigation = viewport_interaction.navigation;
    auto& walk = viewport_interaction.walk;
    DeleteTarget delete_target = DeleteTarget::object;
    InstanceList instance_list{scene_list, InstanceList::Filter::scene};
    InstanceList region_instances{region_list, InstanceList::Filter::regions};
    BlueprintList blueprints_view{blueprint_list};
    std::set<u64> candidates;
    std::map<u64, editor::Schema> schemas;
    PreviewMailbox completed_frames;
    u64 pending_generation{}, image_generation{}, image_id{}, image_revision{},
        minimum_frame_revision = state.document.revision, minimum_overlay_revision = state.document.revision;
    bool captured{}, playing = options.play, wanted_playing = options.play, mode_pending{};
    std::string info;
    Extent2D image_extent = default_preview_extent, desired_preview_extent = default_preview_extent;
    std::map<u64, Extent2D> requested_extents;
    auto started = std::chrono::steady_clock::now(), previous = started, pending_started = started,
         mode_started = started, last_ui_present = started, next_ui_frame = started;
    bool unresponsive{}, stop_refresh{}, ui_keyboard_capture{}, ui_shortcut_capture{};
    std::optional<u32> revealed_instance;
    auto refresh_selection = [&](bool synchronize_document = true) {
        const auto blueprints = blueprint_catalog(state);
        if(synchronize_document || (view_state.selected_object&&!instance_list.contains(view_state.selected_object)&&!region_instances.contains(view_state.selected_object))) {
            instance_list.sync(scene_instances(state),blueprints);
            region_instances.sync(scene_instances(state),blueprints);
        }
        if(instance_flyout.opened() && synchronize_document) floating_instances.sync(scene_instances(state),blueprints);
        selected_instances.retain([&](u32 id){return instance_list.contains(id)||region_instances.contains(id);});
        if(selected_instances.active().value_or(0)!=view_state.selected_object) {
            selected_instances.clear();
            if(view_state.selected_object) selected_instances.select(view_state.selected_object);
        }
        gizmo_mode.show(state,selected_instances.items());
        viewport_interaction.selected(view_state.selected_object, gizmo_mode.value());
        std::vector<u64> selected_rows(selected_instances.items().begin(), selected_instances.items().end());
        timeline.selected_objects(selected_rows);
        instance_list.selection(selected_instances);
        region_instances.selection(selected_instances);
        floating_instances.selection(selected_instances);
        if(synchronize_document) {
            blueprints_view.sync(blueprints);
            if (blueprint_flyout.opened()) floating_blueprints.sync(blueprints);
            view.show(state);
        }
        // Initial selection must not scroll either list away from its top.
        // Later explicit selection reveals the actual row, independent of sizes.
        if (revealed_instance && revealed_instance != view_state.selected_object) {
            instance_list.reveal(view_state.selected_object);
            region_instances.reveal(view_state.selected_object);
            if (instance_flyout.opened()) floating_instances.reveal(view_state.selected_object);
        }
        revealed_instance = view_state.selected_object;
        const auto* selected_instance = find_instance(state, view_state.selected_object);
        if (view_state.mode == ViewMode::mesh) {
            const auto blueprint = std::ranges::find(blueprints, view_state.inspected_mesh, &Blueprint::id);
            object_title.text("Editing blueprint: " + std::string(blueprint->name));
            blueprint_title.text(std::to_string(std::ranges::count(state.document.instances, view_state.inspected_mesh,
                                                                  &SceneInstance::blueprint)) +
                                 " scene instance(s) use the applied blueprint");
        } else if (selected_instance) {
            object_title.text((selected_instances.size()>1 ? std::to_string(selected_instances.size())+" selected / active: " : "Instance: ") + selected_instance->name);
            const auto blueprint = std::ranges::find(blueprints, selected_instance->blueprint, &Blueprint::id);
            const auto count = std::ranges::count(state.document.instances, selected_instance->blueprint, &SceneInstance::blueprint);
            blueprint_title.text("Blueprint: " + std::string(blueprint->name) + " / " + std::to_string(count) + " instance(s)");
        } else {
            object_title.text("No instance selected");
            blueprint_title.text("");
        }
        show_base.text(view_state.mode == ViewMode::mesh ? "Blueprint geometry" : "Instance properties");
        transform_hint.text(view_state.mode == ViewMode::mesh
            ? "Mesh draft / Apply publishes to scene; Save writes the project"
            : selected_instances.size()>1 ? "Inspector: active only / gizmos, Copy and Delete: whole selection"
            : "Instance only / select a paused keyframe to edit");
    };
    auto refresh_vertex = [&] {
        mesh_tools.sync(state);
        const auto target=mesh_target(state);
        const bool draft=target && has_mesh_draft(state,target->blueprint);
        draft_status.text(draft ? "Unapplied mesh changes / scene unchanged" : "No unapplied mesh changes");
        const auto* mesh = editable_mesh(state);
        if (!mesh || !mesh->size()) {
            view_state.selected_vertex = 0;
            selected.text("No mesh blueprint selected");
            x.value(""); y.value(""); z.value("");
            return;
        }
        view_state.selected_vertex = std::min(view_state.selected_vertex, static_cast<u32>(mesh->size() - 1));
        const auto p = mesh->position(view_state.selected_vertex);
        selected.text("Vertex " + std::to_string(view_state.selected_vertex) + " / " +
                      std::to_string(mesh->size()));
        x.value(std::to_string(p.x));
        y.value(std::to_string(p.y));
        z.value(std::to_string(p.z));
    };
    auto refresh = [&](bool reset_camera = false) {
        refresh_selection();
        refresh_vertex();
        pause.text(view_state.paused ? "Play (in editor)" : "Pause (in editor)");
        weld.value(view_state.weld);
        const auto pose = preview_camera_pose(state, view_state.time);
        if (reset_camera) {
            yaw.reset(pose.yaw);
            pitch.reset(pose.pitch);
            orbit_distance.reset(pose.distance); optical_zoom.slider_range(camera_min_zoom, std::max(10.F, pose.zoom)).reset(pose.zoom);
        } else {
            yaw.value(pose.yaw);
            pitch.value(pose.pitch);
            orbit_distance.value(pose.distance); optical_zoom.slider_range(camera_min_zoom, std::max(10.F, pose.zoom)).value(pose.zoom);
        }
    };
    const auto place = [](auto& widget, ui::Rect bounds) {
        widget.position({bounds.x, bounds.y}).width(bounds.width).height(bounds.height);
    };
    // Switching pages only changes visibility. Widgets, drafts and scroll
    // offsets belong to their page and survive tab/selection changes.
    const auto sync_sidebar = [&] {
        const bool scene = sidebar_tab == SidebarTab::scene;
        const bool properties = sidebar_tab == SidebarTab::properties;
        const bool custom = viewport_interaction.custom_inspector();
        scene_panel.visible(scene);
        keyframe_list.visible(scene);
        panels_splitter.visible(scene);
        properties_panel.visible(properties && !custom);
        region_inspector.visible(properties && custom);
        keyframe_inspector.visible(sidebar_tab == SidebarTab::keyframe);
        show_scene.selected(scene);
        show_keyframe.selected(sidebar_tab == SidebarTab::keyframe);
        show_base.selected(properties);
    };
    const auto layout = [&](const input::Frame& input) {
        const auto size = input.logical_size;
        const bool vertices = interaction.value() == InteractionMode::vertices;
        const auto aspect = playing ? static_cast<f32>(image_extent.width) / static_cast<f32>(image_extent.height)
                                    : 0.F;
        geometry = viewport_window.opened() ? editor_controls_layout(size, vertices) : editor_layout(size, vertices, aspect);
        sidebar_sizing.apply(geometry);
        place(toolbar, geometry.toolbar);
        main_bar.layout(size);
        // Keep commit actions pinned right, independently of view-tool overflow.
        scene_controls.width(std::max(82.F, secondary.bounds().width - keyframe_actions.bounds().width - 6.F));
        scene_bar.layout(size);
        timeline.layout_menu(size);
        place(scene_panel, geometry.scene);
        const auto camera_button = show_camera.bounds();
        const auto camera_y = camera_button.y + camera_button.height + 4;
        place(camera_menu, {std::clamp(camera_button.x, 8.F, std::max(8.F,size.x-388)),camera_y,380,
            std::max(100.F,std::min(820.F,size.y-camera_y-16))});
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
        instance_flyout.layout(size, scene_bar.popup_anchor(show_instances));
        blueprint_flyout.layout(size, scene_bar.popup_anchor(show_blueprints));
        blueprints_view.layout();
        if (blueprint_flyout.opened()) floating_blueprints.layout();
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
        place(caption, viewport_window.opened() ? ui::Rect{0,0,viewport_raw.logical_size.x,54} : geometry.caption);
        frame_label.width(std::max(120.F, (viewport_window.opened() ? viewport_raw.logical_size.x : geometry.caption.width) - 458));
        place(timeline_host, geometry.timeline);
        timeline.compact(geometry.timeline.height<160);
        place(files, geometry.files);
        files.padding(4);
        path.width(std::max(100.0F, geometry.files.width - 406));
        place(status_host, geometry.status);
        place(log_panel, geometry.logs);
        log_text.height(std::max(60.0F, geometry.logs.height - 102));
        timing_panel.fit_height(geometry.logs.height);
        place(dialog_host, geometry.dialog);
        place(settings_host, {std::max(0.0F, (size.x - 760) * .5F),
                              std::max(0.0F, (size.y - 900) * .5F), 760, std::min(900.F, size.y)});
        place(import_host, {std::max(0.0F, (size.x - 780) * .5F),
                            std::max(0.0F, (size.y - 560) * .5F), 780, 560});
        place(open_host, {std::max(0.0F, (size.x - 780) * .5F),
                          std::max(0.0F, (size.y - 560) * .5F), 780, 560});
        const auto& preview_input=viewport_window.opened() ? viewport_raw : input;
        preview_bounds = viewport_window.opened() ? ui::Rect{0,54,preview_input.logical_size.x,std::max(1.F,preview_input.logical_size.y-54)} : geometry.viewport;
        auto width = aspect > 0 ? std::min(preview_bounds.width, preview_bounds.height * aspect) : preview_bounds.width;
        auto height = aspect > 0 ? width / aspect : preview_bounds.height;
        Vec2 position{preview_bounds.x + (preview_bounds.width - width) * .5F,
                      preview_bounds.y + (preview_bounds.height - height) * .5F};
        if (auto pixels = preview_pixel_extent({width, height}, preview_input.logical_size, preview_input.framebuffer);
            !pixels.empty()) {
            desired_preview_extent = {
                std::max(1U, pixels.width * settings.preview_percent / 100),
                std::max(1U, pixels.height * settings.preview_percent / 100)};
            const auto sx = static_cast<f32>(preview_input.framebuffer.width) / preview_input.logical_size.x;
            const auto sy = static_cast<f32>(preview_input.framebuffer.height) / preview_input.logical_size.y;
            if (std::round(width * sx) <= preview_capacity.width &&
                std::round(height * sy) <= preview_capacity.height) {
                // Pixel-aligned edges give a true 1:1 sampling footprint at
                // native resolution, including fractional logical-pixel DPI.
                width = static_cast<f32>(pixels.width) / sx;
                height = static_cast<f32>(pixels.height) / sy;
                position = {
                    std::round((preview_bounds.x + (preview_bounds.width - width) * .5F) * sx) / sx,
                    std::round((preview_bounds.y + (preview_bounds.height - height) * .5F) * sy) /
                        sy};
            }
        }
        image.position(position).width(width).height(height);
        sync_sidebar();
    };
    refresh();
    auto send_settings = [&](u64 generation) {
        if (generation)
            if (auto sent = session->send("settings\n" + encode_settings(settings), generation); !sent)
                status.text(sent.error().message);
    };
    auto send_viewport = [&](u64 generation) {
        if (!generation || (requested_extents.contains(generation) &&
                            requested_extents.at(generation) == desired_preview_extent))
            return;
        if (auto sent = session->send(encode_viewport_size(desired_preview_extent), generation);
            !sent)
            status.text(sent.error().message);
        else
            requested_extents[generation] = desired_preview_extent;
    };
    auto consume_edits = [&] {
        if (auto notice = editing.take_changes()) {
            document_origin = timings.interaction();
            updates.changed(notice->changes);
            regions.changed(notice->changes);
            if(notice->changes.full)refresh_selection();
            else if(!notice->changes.regions.empty()) {
                bool renamed=false;
                for(const auto& [id, changes] : notice->changes.regions)
                    if(const auto* instance = find_instance(state, id))
                        { renamed |= instance_list.rename(id, instance->name); renamed |= region_instances.rename(id, instance->name); floating_instances.rename(id, instance->name); }
                if(renamed)refresh_selection(false);
            }
            if (notice->discontinuous)
                minimum_frame_revision = minimum_overlay_revision = notice->revision;
        }
    };
    auto flush_update = [&](u64 generation) {
        consume_edits();
        if (traced_revision != state.document.revision) {
            traced_revision = state.document.revision;
            document_origin = timings.interaction();
        }
        auto packet = updates.next(generation, state);
        if (!packet) {
            status.text(packet.error().message);
            return;
        }
        if (!*packet)
            return;
        if (auto sent = session->send(editor::traced_message(**packet, document_origin), generation); !sent) {
            status.text(sent.error().message);
            updates.reset(generation);
        }
    };
    auto flush_view = [&](u64 generation) {
        // Visibility is a reliable, change-only packet, never repeated in
        // camera navigation's latest-value mailbox.
        mesh_tools.sync(state);
        if(generation && sent_mesh_visibility[generation]!=mesh_tools.visibility_revision()) {
            const auto bytes=encode_mesh_visibility({state.document.revision,mesh_tools.visibility()});
            if(auto sent=session->send("mesh_visibility\n"+bytes,generation);!sent) status.text(sent.error().message);
            else sent_mesh_visibility[generation]=mesh_tools.visibility_revision();
        }
        if (!generation || sent_views[generation] == view_state.sequence) return;
        auto bytes = encode_viewport_request({state.document.revision,view_state});
        if (!bytes) { status.text(bytes.error()); return; }
        auto sent = session->send_latest(editor::traced_message("view\n" + *bytes,view_origin),generation);
        scroll_trace.submitted(generation, view_state.sequence, sent.has_value());
        if (!sent) status.text(sent.error().message);
        else sent_views[generation]=view_state.sequence;
    };
    auto viewport_changed = [&] {
        ++view_state.sequence;
        view_origin = timings.interaction();
    };
    auto send_snapshot = [&](u64 generation) {
        send_viewport(generation);
        updates.reset(generation);
        flush_update(generation);
        sent_views.erase(generation);
        sent_mesh_visibility.erase(generation);
        flush_view(generation);
    };
    auto broadcast = [&] {
        consume_edits();
        if (!debug_link.value())
            return;
        if (session->active_generation()) {
            flush_update(session->active_generation());
            flush_view(session->active_generation());
        }
        for (auto gen : candidates) {
            flush_update(gen);
            flush_view(gen);
        }
    };
    auto finish_position_drag = [&](bool cancel) {
        if (!editing.active(EditGesture::move)) return;
        const auto finished = viewport_interaction.finish(ViewportTool::translation, cancel);
        if (!finished) {
            status.text(finished.error().message);
            return;
        }
        if (cancel) translation.cancel();
        position_schema.reset();
        show_timeline();
        if (auto schema = schemas.find(session->active_generation());
            schema != schemas.end() && schema->second.stamp.revision == state.document.revision)
            inspector.show(schema->second);
        status.text(cancel ? "Move cancelled / original position restored"
                           : *finished ? "Moved object / Undo restores the whole drag"
                                       : "Position unchanged");
    };
    auto rotation_changed = [&](content::Result<RotationChange> result) {
        if (!result) { status.text(result.error().message); return; }
        const auto& action = *result;
        if (action.finished) {
            show_timeline();
            if (auto schema = schemas.find(session->active_generation());
                schema != schemas.end() && schema->second.stamp.revision == state.document.revision)
                inspector.show(schema->second);
            status.text(action.cancelled ? "Rotation cancelled / original transform restored"
                : action.committed ? "Rotated selection / Undo restores the whole drag"
                                   : "Rotation unchanged");
        }
    };
    auto cancel_rotation = [&] { rotation_changed(rotation.cancel()); };
    auto finish_scale = [&](bool cancel) {
        if(!editing.active(EditGesture::scale)) return;
        const auto object=editing.active_object();
        const auto finished=viewport_interaction.finish(ViewportTool::scale, cancel);
        if(!finished) { status.text(finished.error().message); return; }
        if(cancel) scaling.cancel();
        if(cancel)
            if(const auto* instance=find_instance(state,object))
                inspector.reset_number("transform","scale",evaluate_instance(state,*instance,view_state.time).transform.scale);
        show_timeline();
        if(auto schema=schemas.find(session->active_generation());
           schema!=schemas.end() && schema->second.stamp.revision==state.document.revision)
            inspector.show(schema->second);
        status.text(cancel ? "Scale cancelled" : "Instance scaled / Ctrl+Z restores the edit");
    };
    auto edit_scale = [&](f32 value, bool finish, bool group = false) {
        if(!editing.active(EditGesture::scale)) {
            auto begun=editing.begin_scale(view_state.selected_object,group ? selected_instances.items() : std::span<const u32>{});
            if(!begun) { status.text(begun.error().message); return; }
        }
        const auto edited=editing.scale(value,group ? std::optional{viewport_interaction.gizmo_input.scale_limits().instance} : std::nullopt);
        if(!edited) { finish_scale(true); status.text(edited.error().message); return; }
        if(scaling.dragging()) {
            const auto* instance=find_instance(state,view_state.selected_object);
            if(instance)inspector.reset_number("transform","scale",evaluate_transform(state,*instance,view_state.time).scale);
        }
        if(finish) finish_scale(false);
    };
    auto camera_changed = [&] {
        viewport_changed();
        const auto pose = preview_camera_pose(state, view_state.time);
        yaw.value(pose.yaw);
        pitch.value(pose.pitch);
        orbit_distance.value(pose.distance); optical_zoom.slider_range(camera_min_zoom, std::max(10.F, pose.zoom)).value(pose.zoom);
    };
    auto finish_bounds = [&](bool cancel) {
        if (!editing.active(EditGesture::world_bounds)) return;
        const auto result = viewport_interaction.finish(ViewportTool::bounds, cancel);
        if (!result) status.text(result.error().message);
        else if (*result) status.text(cancel ? "World bounds edit cancelled" : "World bounds updated");
        if (cancel) bounds_tool.cancel();
        bounds_panel.sync(state.document.world_bounds);
    };
    auto finish_camera = [&](bool cancel) {
        if (!editing.active(EditGesture::camera)) return;
        auto result = viewport_interaction.finish(ViewportTool::navigation, cancel);
        if (!result) { status.text(result.error().message); return; }
        if (cancel) {
            navigation.cancel();
            camera_changed();
            const auto pose = preview_camera_pose(state, view_state.time);
            yaw.reset(pose.yaw); pitch.reset(pose.pitch); orbit_distance.reset(pose.distance); optical_zoom.slider_range(camera_min_zoom, std::max(10.F, pose.zoom)).reset(pose.zoom);
            status.text("Camera edit cancelled");
        }
        show_timeline();
    };
    // Navigation always moves the private editor view. Authoring a camera is
    // explicit: Enter a camera, look around, then Save this camera.
    auto camera_edited = [&](const CameraPose& before, const CameraPose& after) {
        if (before == after) return;
        view_state.editor_camera = after;
        camera_changed();
    };
    auto end_camera_visit = [&](bool restore) {
        if (!camera_visit) return;
        if (restore) view_state.editor_camera = camera_visit->previous;
        camera_visit.reset();
        camera_changed();
    };
    auto visit_camera = [&](const SceneInstance& camera, bool editable) {
        const auto previous = camera_visit ? camera_visit->previous : view_state.editor_camera;
        camera_visit = CameraVisit{camera.id, editable, previous};
        view_state.editor_camera = camera_pose(evaluate_instance(state, camera, view_state.time));
        walk.active(false);
        camera_changed();
        status.text(editable ? "Looking through " + camera.name + " / navigate freely, then Save this camera to author the view"
                             : "Inspecting " + camera.name + " / read-only view; Back returns to the editor view");
    };
    auto playback_changed = [&] {
        viewport_changed();
        const auto pose = preview_camera_pose(state, view_state.time);
        yaw.value(pose.yaw);
        pitch.value(pose.pitch);
        orbit_distance.value(pose.distance); optical_zoom.slider_range(camera_min_zoom, std::max(10.F, pose.zoom)).value(pose.zoom);
        minimum_inspector_sequence = view_state.sequence;
        pause.text(view_state.paused ? "Play (in editor)" : "Pause (in editor)");
    };
    auto commit_selection = [&] {
        const bool editing_keyframe = timeline.selected_keyframe().has_value();
        // Keyboard actions follow the last explicitly selected kind. Keeping
        // the keyframe inspector open must not make Copy/Delete target keys
        // when the user has just selected an instance.
        delete_target = DeleteTarget::object;
        const auto object=selected_instances.active().value_or(0);
        const bool different=view_state.selected_object!=object;
        view_state.selected_object = object;
        view_state.selected_vertex = 0;
        if(different) inspector.clear();
        translation.cancel();
        selection_overlay_dirty = true;
        refresh_selection(false);
        if(view_state.mode==ViewMode::mesh)refresh_vertex();
        if (editing_keyframe) timeline.focus_object(object);
        if(different) {viewport_changed();minimum_inspector_sequence = view_state.sequence;}
        if(selected_instances.size()>1) {
            status.text("Selected " + std::to_string(selected_instances.size()) +
                        " instances / gizmos: whole selection / inspector: active only");
        } else if (state.document.timeline.find({object, "position"}))
            status.text("Selected 1 instance: " + object_name(state, object) +
                        (editing.can_edit_scene_pose() ? " / dragging edits the selected keyframe"
                                                     : " / read-only pose: select a keyframe to edit"));
        else
            status.text(object ? "Selected 1 instance: " + object_name(state, object) + " / Delete removes this instance"
                               : "Selection cleared / 0 instances selected");
    };
    auto select_object = [&](u32 object,editor::SelectionMode mode=editor::SelectionMode::replace) {
        if(object) {
            std::vector<u32> order;for(const auto& i:state.document.instances) order.push_back(i.id);
            selected_instances.select(object,mode,order);
        } else if(mode==editor::SelectionMode::replace) selected_instances.clear();
        commit_selection();
    };
    auto view_changed = [&] {
        view_state.smooth_zoom = false;
        viewport_changed();
        minimum_inspector_sequence = view_state.sequence;
        inspector.clear();
        translation.cancel();
        navigation.cancel();
        refresh(true);
    };
    auto select_scene_instance = [&](u32 object, editor::SelectionMode mode) {
        if (!find_instance(state, object)) return;
        const bool was_isolated = view_state.mode != ViewMode::scene;
        view_state.mode = ViewMode::scene;
        select_object(object, mode);
        interaction.value(InteractionMode::objects);
        if (was_isolated) view_changed();
    };
    auto open_mesh = [&](BlueprintId blueprint) {
        if (auto inspected = inspect_mesh(state, view_state, blueprint); !inspected) {
            status.text(inspected.error().message);
            return;
        }
        interaction.value(InteractionMode::vertices);
        sidebar_tab = SidebarTab::properties;
        view_changed();
        status.text("Editing mesh draft / Apply mesh to scene publishes changes; Save on disk preserves the project");
    };
    auto interaction_changed = [&] {
        if (interaction.value() == InteractionMode::vertices) {
            const auto target=mesh_target(state);
            if(view_state.mode!=ViewMode::mesh) open_mesh(target?target->blueprint:view_state.inspected_mesh);
            sidebar_tab = SidebarTab::properties;
            refresh_vertex();
        } else if (view_state.mode == ViewMode::mesh) {
            // Returning to instance tools also returns to the scene: never
            // delete or select a hidden scene instance while inspecting an asset.
            view_state.mode = ViewMode::scene;
            view_state.selected_vertex = 0;
            view_changed();
        }
        translation.cancel();
    };
    auto mesh_action = [&](MeshAction action) {
        auto* mesh = editable_mesh(state);
        const auto target = mesh_target(state);
        if(!mesh || !target || view_state.mode!=ViewMode::mesh) return;
        if(action==MeshAction::hide || action==MeshAction::reveal) {
            const auto count=action==MeshAction::hide?mesh_tools.hide_selected():mesh_tools.reveal_hidden();
            if(count) viewport_changed();
            status.text(count?(action==MeshAction::hide?"Hidden ":"Revealed ")+std::to_string(count)+
                " faces / Alt+H reveals all":"No faces to "+std::string(action==MeshAction::hide?"hide":"reveal"));
            return;
        }
        const auto ids=mesh_tools.vertices(*mesh);
        auto result = editing.mesh_operation(target->blueprint,
            action == MeshAction::fill ? MeshOperation::fill : action == MeshAction::subdivide ? MeshOperation::subdivide : MeshOperation::align,
            ids, mesh_tools.edges(*mesh));
        if (!result) { status.text(result.error().message); return; }
        if (!*result) { status.text("Mesh unchanged"); return; }
        refresh_vertex();
        mesh_operation_options.observe(target->blueprint,action==MeshAction::fill?MeshOperation::fill:
            action==MeshAction::subdivide?MeshOperation::subdivide:MeshOperation::align);
        tool_panel.show(mesh_operation_options,true);
        status.text(action==MeshAction::fill?"Created edge/face / Undo restores topology":
            action==MeshAction::subdivide?"Subdivided selected edges / shared midpoints / Undo restores topology":
            "Aligned to line / first two vertices unchanged");
    };
    auto selected_mesh_vertices = [&] {
        auto* mesh=editable_mesh(state);
        return mesh_tools.vertices(*mesh,view_state.weld);
    };
    const auto cancel_viewport = [&] {
        if(viewport_interaction.pivot.dragging())rotation_pivot.cancel();
        const bool camera = editing.active(EditGesture::camera);
        const auto object = editing.active_object();
        auto cancelled = viewport_interaction.cancel();
        if (!cancelled) { status.text(cancelled.error().message); return; }
        position_schema.reset();
        bounds_panel.sync(state.document.world_bounds);
        if (const auto* instance = find_instance(state, object))
            inspector.reset_number("transform", "scale", evaluate_instance(state, *instance, view_state.time).transform.scale);
        if (camera) {
            camera_changed();
            const auto pose = preview_camera_pose(state, view_state.time);
            yaw.reset(pose.yaw); pitch.reset(pose.pitch); orbit_distance.reset(pose.distance); optical_zoom.slider_range(camera_min_zoom, std::max(10.F, pose.zoom)).reset(pose.zoom);
        }
        if (*cancelled) { show_timeline(); refresh_vertex(); }
    };
    auto launch = [&](u64 generation) {
        if (!has_camera(state)) {
            status.text("Add a camera first (Blueprints > Camera +): independent Play renders through the scene's active camera");
            return;
        }
        consume_edits();
        // Explicit Play initializes from the current document even with live
        // debugging disconnected. It does not enable the debug data stream.
        updates.reset(generation);
        auto packet = updates.next(generation, state);
        if (!packet || !*packet) {
            status.text(packet ? "Cannot initialize Play" : packet.error().message);
            return;
        }
        (**packet).replace(0, 9, "launch\n");
        if (auto sent = session->send(**packet, generation); !sent) {
            status.text(sent.error().message);
            updates.reset(generation);
            return;
        }
        diagnostic.value(false);
        playing = true;
        wanted_playing = true;
        mode_pending = true;
        mode_started = std::chrono::steady_clock::now();
        play.text("Stop (independent)");
    };
    auto send_event = [&](editor::Event event) {
        if (!playing) {
            auto local = apply_instance_control(editing, event, session->active_generation(), minimum_inspector_sequence);
            if (!local) { status.text(local.error().message); return; }
            if (local->has_value()) { show_timeline(); return; }
        }
        if (!debug_link.value() || editing.awaiting_remote() || session->busy() || unresponsive ||
            !session->active_generation() ||
            !updates.ready(session->active_generation(), state.document.revision)) {
            status.text("Preview is catching up; Apply again once its controls are ready.");
            return;
        }
        auto it = schemas.find(session->active_generation());
        if (it == schemas.end() || it->second.stamp.revision != state.document.revision ||
            it->second.stamp.object != view_state.selected_object ||
            it->second.stamp.context < minimum_inspector_sequence)
            return;
        event.stamp = it->second.stamp;
        auto payload = editor::encode_event(event);
        if (!payload) {
            status.text(payload.error().message);
            return;
        }
        if (auto begun = editing.begin_remote(event.stamp.generation); !begun) {
            status.text(begun.error().message); return;
        }
        if (auto sent = session->send(editor::traced_message("event\n" + *payload,timings.interaction()), event.stamp.generation); !sent) {
            editing.abandon_remote();
            status.text(sent.error().message);
            return;
        }
        pending_generation = event.stamp.generation;
        pending_started = std::chrono::steady_clock::now();
    };
    // The playhead may start on time zero; this is not a keyframe selection.
    timeline.clear_selection();
    while (!window.should_close()) {
        window.poll_events();
        if (const auto now = std::chrono::steady_clock::now(); now < next_ui_frame) {
            std::this_thread::sleep_for(std::min(next_ui_frame - now,
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(4))));
            continue;
        }
        next_ui_frame = std::chrono::steady_clock::now() + frame_interval(settings.ui_fps);
        if (toggle_viewport || viewport_window.closing()) {
            toggle_viewport=false;
            cancel_viewport(); timeline.cancel(); mesh_tools.close(); regions.cancel();
            camera_open=false; camera_menu.visible(false);
            close_list_flyouts();
            if (viewport_window.opened()) viewport_window.close();
            else if (auto opened=viewport_window.open(!options.automation);!opened) status.text(opened.error());
            pop_viewport.text(viewport_window.opened() ? "Dock viewport" : "Pop out viewport");
        }
        // One swap interval per editor tick, on the window showing the scene.
        const auto controls_vsync=options.automation || viewport_window.opened() ? window::VSync::off : settings.vsync;
        if (window.vsync()!=controls_vsync)
            if (auto applied=window.set_vsync(controls_vsync);!applied) return fail(applied.error().message);
        auto raw = window.take_input();
        scale_ui_input(raw, settings.ui_scale_percent);
        if (options.automation) {
            raw.events.clear();
            raw.focused = true;
            raw.overflow = false;
            options.automation->input(raw);
        }
        if (viewport_window.opened() || !raw.framebuffer.empty()) controls_size.retain(raw);
        viewport_raw=viewport_window.opened() ? viewport_window.take_input() : raw;
        if (viewport_window.opened()) {
            scale_ui_input(viewport_raw,settings.ui_scale_percent);
            if (options.automation) {
                viewport_raw.events.clear(); viewport_raw.focused=false; viewport_raw.overflow=false;
                options.automation->viewport_input(viewport_raw);
            }
            detached_size.retain(viewport_raw);
        }
        timings.begin_tick(viewport_window.opened() && viewport_raw.focused ? viewport_raw.events : raw.events);
        layout(raw);
        send_viewport(session->active_generation());
        for (auto generation : candidates)
            send_viewport(generation);
        auto now = std::chrono::steady_clock::now();
        const auto dt = std::chrono::duration<f32>(now - previous).count();
        previous = now;
        for (const auto& event : session->poll()) {
            using K = editor::preview::EventKind;
            if (event.kind == K::candidate_started) {
                candidates.insert(event.generation);
                updates.add(event.generation);
                send_settings(event.generation);
                send_snapshot(event.generation);
                session->send(diagnostic.value() ? "diagnostic\n1" : "diagnostic\n0",
                              event.generation);
            } else if (event.kind == K::activated) {
                completed_frames.clear();
                candidates.erase(event.generation);
                std::erase_if(requested_extents, [&](const auto& entry) {
                    return entry.first != event.generation && !candidates.contains(entry.first);
                });
                editing.abandon_remote();
                unresponsive = false;
                std::erase_if(schemas, [&](const auto& entry) {
                    const bool remove =
                        entry.first != event.generation && !candidates.contains(entry.first);
                    if (remove)
                        updates.remove(entry.first);
                    return remove;
                });
                status.text("Preview generation " + std::to_string(event.generation) +
                            " ready / scene retained");
                session->send(debug_link.value() ? "link\n1" : "link\n0", event.generation);
                if (playing)
                    launch(event.generation);
                if (auto it = schemas.find(event.generation);
                    it != schemas.end() && it->second.stamp.revision == state.document.revision &&
                    !viewport_interaction.busy())
                    inspector.show(it->second);
                else if (debug_link.value())
                    flush_update(event.generation);
            } else if (event.kind == K::worker_failed || event.kind == K::build_failed) {
                if (event.kind == K::worker_failed &&
                    event.generation == image_generation && editing.active(EditGesture::move))
                    finish_position_drag(true);
                if (event.kind == K::worker_failed && event.generation == image_generation)
                    cancel_rotation();
                if (event.kind == K::worker_failed && event.generation == image_generation)
                    finish_scale(true);
                candidates.erase(event.generation);
                requested_extents.erase(event.generation);
                if (event.kind == K::worker_failed)
                    schemas.erase(event.generation);
                if (event.kind == K::worker_failed)
                    updates.remove(event.generation);
                status.text(event.message);
                if (pending_generation == event.generation) {
                    editing.abandon_remote();
                    completed_frames.clear();
                }
                std::cerr << event.message << '\n';
                std::cerr << session->logs() << '\n';
            } else if (event.kind == K::message) {
                const std::string_view message = event.message;
                if (message.starts_with("revision\n")) {
                    u64 revision{};
                    const auto text = message.substr(9);
                    const auto [end, error] =
                        std::from_chars(text.data(), text.data() + text.size(), revision);
                    if (error == std::errc{} && end == text.data() + text.size())
                        updates.acknowledge(event.generation, revision);
                } else if (message.starts_with("resync\n")) {
                    updates.reset(event.generation);
                    status.text(message.substr(7));
                } else if (message.starts_with("status\n") &&
                           event.generation == session->active_generation()) {
                    const bool acknowledged_play = message.find("play=1") != std::string_view::npos;
                    if (!mode_pending || acknowledged_play == wanted_playing) {
                        const bool stopped = playing && !acknowledged_play;
                        playing = acknowledged_play;
                        wanted_playing = playing;
                        mode_pending = false;
                        play.text(playing ? "Stop (independent)" : "Play (independent)");
                        if (stopped)
                            stop_refresh =
                                true; // Defer a revision bump until any native callback ACK.
                    }
                    if (!debug_link.value())
                        frame_label.text("Debug link OFF / preview frozen; Play renders directly "
                                         "in worker window");
                } else if (message.starts_with("schema\n")) {
                    auto schema = editor::decode_schema(message.substr(7));
                    if (schema) {
                        schemas.insert_or_assign(event.generation, *schema);
                        if (event.generation == session->active_generation() &&
                            schema->stamp.revision == state.document.revision && !viewport_interaction.busy())
                            inspector.show(*schema);
                    }
                } else if (message.starts_with("state_patch\n") &&
                           event.generation == session->active_generation()) {
                    auto patch = decode_patch(message.substr(12));
                    if (patch && editing.awaiting_remote() && pending_generation == event.generation) {
                        if (auto applied = editing.accept_remote(event.generation, *patch); applied) {
                            consume_edits();
                            updates.accepted(event.generation, state.document.revision);
                            refresh();
                        } else {
                            status.text(applied.error().message);
                            send_snapshot(event.generation);
                        }
                        editing.abandon_remote();
                    } else if (!patch) {
                        status.text(patch.error().message);
                        editing.abandon_remote();
                        send_snapshot(event.generation);
                    }
                } else if (message.starts_with("error\n")) {
                    status.text(message.substr(6));
                    std::cerr << message.substr(6) << '\n';
                    if (editing.awaiting_remote() && event.generation == pending_generation) {
                        editing.abandon_remote();
                        completed_frames.clear();
                    } else
                        updates.reject(event.generation);
                } else if (message.starts_with("info\n") &&
                           event.generation == session->active_generation()) {
                    info = message.substr(5);
                    analysis_label.text(info);
                }
            }
        }
        if (auto arrived = session->take_latest_frame())
            completed_frames.offer(std::move(*arrived), session->active_generation());
        if (auto latest = completed_frames.take(session->active_generation(), minimum_frame_revision,
                                                image_revision, state.document.revision);
            latest &&
            (latest->info.generation != image_generation || latest->info.frame_id != image_id)) {
            // Keep showing completed geometry while dragging. A newer camera,
            // scene, or diagnostic mode still establishes a strict image floor.
            if ((debug_link.value() || !image_id) &&
                latest->info.generation == session->active_generation() &&
                accepts_completed_revision(latest->info.scene_revision, minimum_frame_revision,
                                           image_revision, state.document.revision)) {
                image_generation = latest->info.generation;
                image_id = latest->info.frame_id;
                image_revision = latest->info.scene_revision;
                presented_info = latest->info;
                if (presented_info.has_view) image_camera = presented_camera(presented_info);
                image_extent = {latest->info.width, latest->info.height};
                preview_pixels = std::make_shared<gfx::ImageData>(image_extent, std::move(latest->rgba));
                image.image(preview_pixels);
                layout(raw);
                if (!playing && !view_state.paused && image_revision == state.document.revision &&
                    presented_info.view_sequence == view_state.sequence) {
                    view_state.time = static_cast<f32>(latest->info.time);
                    const auto pose = preview_camera_pose(state, view_state.time);
                    if (!yaw.editing()) yaw.value(pose.yaw);
                    if (!pitch.editing()) pitch.value(pose.pitch);
                    if (!orbit_distance.editing()) orbit_distance.value(pose.distance);
                    if (!optical_zoom.editing()) optical_zoom.slider_range(camera_min_zoom, std::max(10.F, pose.zoom)).value(pose.zoom);
                }
                frame_label.text("Preview " + std::to_string(image_extent.width) + " x " +
                                 std::to_string(image_extent.height) +
                                 (playing ? " / debug preview (" + (settings.debug_fps
                                     ? std::to_string(settings.debug_fps) + " Hz" : std::string{"uncapped"}) + ")" : "") + " / gen " +
                                 std::to_string(image_generation) + " / update " +
                                 std::to_string(image_revision) +
                                 " / view " + std::to_string(presented_info.view_sequence) +
                                 (playing ? " / independent view"
                                  : inspecting() ? " / inspecting camera" : camera_visit ? " / through camera" : " / editor camera") +
                                 (editing.dirty() ? " / unsaved" : ""));
                if (!debug_link.value())
                    frame_label.text(
                        "Debug link OFF / preview frozen; Play renders directly in worker window");
            }
        }
        if (editing.awaiting_remote() && now - pending_started > std::chrono::seconds(30)) {
            editing.abandon_remote();
            completed_frames.clear();
            unresponsive = true;
            status.text(
                "Preview callback timed out. Fix C++ and Reload; scene + last image are retained.");
        }
        if (mode_pending && now - mode_started > std::chrono::seconds(5)) {
            mode_pending = false;
            unresponsive = true;
            status.text("Play control timed out. Reload C++ can replace the worker.");
        }
        const auto current_schema = schemas.find(session->active_generation());
        if (stop_refresh && !editing.awaiting_remote()) {
            viewport_changed();
            stop_refresh = false;
        }
        viewport_interaction.begin_frame();
        const bool viewport_gesture = viewport_interaction.busy() || timeline.dragging();
        const bool camera_numeric_active = editing.active(EditGesture::camera) && !navigation.dragging() && !walk.moving();
        const bool settings_was_open = settings_panel.visible();
        const bool import_was_open = import_dialog.visible();
        const bool open_was_open = open_dialog.visible();
        if (camera_visit) {
            const auto* visited = find_instance(state, camera_visit->camera);
            if (!visited || !is_camera_instance(state, camera_visit->camera) || view_state.mode != ViewMode::scene)
                end_camera_visit(true);
            else if (!camera_visit->editable) {
                const auto pose = camera_pose(evaluate_instance(state, *visited, view_state.time));
                if (pose != view_state.editor_camera) { view_state.editor_camera = pose; camera_changed(); }
            }
        }
        camera_label.text(playing ? "SIM CAMERA / independent Play"
            : !camera_visit ? (view_state.mode == ViewMode::scene ? "EDITOR CAMERA / private view" : "EDITOR CAMERA / blueprint view")
            : camera_visit->editable ? "SIM CAMERA (entered) / " + object_name(state, camera_visit->camera)
            : "SIM CAMERA (inspecting) / " + object_name(state, camera_visit->camera));
        const bool dialog_was_open = modal_visible();
        const bool timeline_enabled = !dialog_was_open && !editing.awaiting_remote() && !playing &&
                                      !mode_pending && !viewport_interaction.busy();
        timeline_host.enabled(timeline_enabled);
        keyframe_list.enabled(timeline_enabled);
        keyframe_inspector.enabled(timeline_enabled);
        timeline.actions_enabled(timeline_enabled);
        inspector_tabs.enabled(!dialog_was_open && !editing.busy());
        if (!timeline_enabled)
            timeline.cancel();
        // Streamed object positions can advance every UI tick. The inspector
        // is disabled during capture; rebuild its keyframe rows once at the
        // terminal event, not once per intermediate revision.
        if (!editing.busy()) show_timeline();
        toolbar.enabled(!dialog_was_open);
        main_bar.enabled(!dialog_was_open);
        scene_bar.enabled(!dialog_was_open);
        smaller_ui.enabled(!viewport_gesture && settings.ui_scale_percent>75);
        pop_viewport.enabled(!viewport_gesture && !mode_pending && !editing.awaiting_remote());
        larger_ui.enabled(!viewport_gesture && settings.ui_scale_percent<150);
        scale_label.text(std::to_string(settings.ui_scale_percent)+"%");
        camera_menu.enabled(!dialog_was_open && !editing.awaiting_remote());
        gizmo_only.enabled(!dialog_was_open && view_state.mode==ViewMode::scene &&
            find_instance(state,view_state.selected_object)!=nullptr);
        if(!gizmo_only.changedValue())gizmo_only.value(view_state.gizmo_only);
        const bool lists_enabled = !dialog_was_open && !editing.awaiting_remote() && !viewport_gesture;
        panels_splitter.enabled(lists_enabled);
        for(auto splitter:section_splitters) splitter.enabled(lists_enabled);
        instance_flyout.enabled(lists_enabled); blueprint_flyout.enabled(lists_enabled);
        show_instances.enabled(lists_enabled); show_blueprints.enabled(lists_enabled);
        show_camera.enabled(!editing.awaiting_remote() && (!viewport_gesture || camera_numeric_active));
        files.enabled(!dialog_was_open);
        log_panel.enabled(!dialog_was_open);
        blueprint_mesh_panel.sync();
        const auto update_pose_controls = [&] {
            properties_panel.enabled(view_state.mode==ViewMode::mesh ? (!dialog_was_open && !mode_pending && !editing.busy() && !viewport_gesture) : editing.can_edit_scene_pose() && ((editing.active(EditGesture::scale) && !scaling.dragging() && !instance_transform.active() && !dialog_was_open && !mode_pending && !editing.awaiting_remote()) ||
                      (!dialog_was_open && !mode_pending &&
                      updates.ready(session->active_generation(), state.document.revision) &&
                      debug_link.value() && !editing.awaiting_remote() && !unresponsive && !viewport_gesture &&
                      !session->busy() && current_schema != schemas.end() &&
                      current_schema->second.stamp.revision == state.document.revision &&
                      current_schema->second.stamp.object == view_state.selected_object &&
                      current_schema->second.stamp.context >= minimum_inspector_sequence)));
            const bool camera_editable = !inspecting();
            yaw.enabled(camera_editable); pitch.enabled(camera_editable); orbit_distance.enabled(camera_editable); optical_zoom.enabled(camera_editable);
            move_camera_scroll.enabled(camera_editable && !viewport_gesture);
            gizmo_mode.enabled(editing.can_edit_scene_pose() && !playing && !mode_pending && !viewport_gesture &&
                           selected_instances.size()!=0 && view_state.mode == ViewMode::scene && interaction.value() == InteractionMode::objects);
            blueprint_mesh_panel.enabled(!dialog_was_open && !viewport_gesture && !mode_pending && !editing.busy());
        };
        update_pose_controls();
        scene_panel.enabled(!dialog_was_open && !editing.awaiting_remote() && (!viewport_gesture || camera_numeric_active));
        bounds_panel.sync(state.document.world_bounds);
        bounds_panel.available(view_state.mode == ViewMode::scene, !viewport_gesture && !playing && !mode_pending && view_state.paused);
        scene_list.enabled(!viewport_gesture);
        region_list.enabled(!viewport_gesture);
        blueprint_list.enabled(!viewport_gesture);
        interaction.enabled(!viewport_gesture);
        view.enabled(!editing.awaiting_remote() && !viewport_gesture);
        pause.enabled(debug_link.value() && !playing && !editing.awaiting_remote() && !viewport_gesture);
        play.enabled(!mode_pending && !editing.awaiting_remote() && !viewport_gesture && !session->busy() &&
                     session->active_generation() != 0);
        debug_link.enabled(!mode_pending && !editing.awaiting_remote() && !viewport_gesture && !session->busy() &&
                           session->active_generation() != 0);
        undo.enabled(!editing.awaiting_remote() && !viewport_gesture && editing.can_undo());
        redo.enabled(!editing.awaiting_remote() && !viewport_gesture && editing.can_redo());
        reload.enabled(!mode_pending && !editing.awaiting_remote() && !viewport_gesture && !session->busy());
        show_settings.enabled(!editing.awaiting_remote() && !viewport_gesture && !mode_pending);
        show_import.enabled(!editing.awaiting_remote() && !viewport_gesture && !playing && !mode_pending);
        load.enabled(!editing.awaiting_remote() && !viewport_gesture);
        save.enabled(!mode_pending && !editing.awaiting_remote() && !viewport_gesture);
        save_as.enabled(!mode_pending && !editing.awaiting_remote() && !viewport_gesture);
        export_asset.text(exporting_effect()?"Export new .veffect":"Export new .vmesh");
        export_asset.enabled(!editing.awaiting_remote() && !viewport_gesture && (exporting_effect() || editable_mesh(state)));
        diagnostic.enabled(debug_link.value() && !playing && !editing.awaiting_remote() && !viewport_gesture &&
                           (view_state.mode == ViewMode::mesh || view_state.selected_object != 0));
        const bool editing_mesh = view_state.paused && editable_mesh(state) &&
                                  interaction.value() == InteractionMode::vertices && !mesh_tools.selected().empty();
        x.enabled(editing_mesh);
        y.enabled(editing_mesh);
        z.enabled(editing_mesh);
        weld.enabled(editing_mesh);
        apply_vertex.enabled(editing_mesh);
        publish_mesh.enabled(!editing.awaiting_remote() && !viewport_gesture && !playing && view_state.mode==ViewMode::mesh &&
                             has_mesh_draft(state,view_state.inspected_mesh));
        save_mesh_draft.enabled(!mode_pending && !editing.awaiting_remote() && !viewport_gesture);
        nudges.enabled(editing_mesh);
        if (raw.overflow || !raw.focused)
            timeline.cancel();
        if(viewport_raw.overflow || !viewport_raw.focused || (box_tool.active() &&
            (image.bounds().width!=box_viewport.width || image.bounds().height!=box_viewport.height ||
             image.bounds().x!=box_viewport.x || image.bounds().y!=box_viewport.y))) box_tool.cancel();
        if (raw.overflow || viewport_raw.overflow) {
            cancel_viewport();
        }
        if (!raw.framebuffer.width || !raw.framebuffer.height) {
            cancel_viewport();
            broadcast();
            auto suspended = raw;
            suspended.framebuffer = {1, 1};
            suspended.logical_size = {1360, 900};
            suspended.focused = false;
            suspended.events.push_back({.kind = input::EventKind::focus_lost});
            (void)screen.update(suspended, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        struct Clipboard final : input::Clipboard {
            decltype(window)& w;
            std::string* local;
            Clipboard(decltype(window)& value, std::string* isolated) : w(value), local(isolated) {}
            std::string read() override { return local ? *local : w.clipboard_text(); }
            void write(std::string_view value) override {
                if (local) *local = value;
                else w.set_clipboard_text(value);
            }
        } clipboard{window, options.automation ? &automation_clipboard : nullptr};
        // Tab normally traverses UI focus. Give it to the viewport only when
        // no widget owns the keyboard; never also move focus with the same key.
        auto ui_input = raw;
        auto viewport_ui_input=viewport_raw;
        const bool viewport_keyboard=viewport_window.opened() && viewport_raw.focused;
        auto& shortcut_input=viewport_keyboard ? viewport_ui_input : ui_input;
        const int gizmo_cycle = take_gizmo_cycle(shortcut_input,
            !dialog_was_open && (viewport_keyboard || !ui_shortcut_capture) && !editing.awaiting_remote() && !viewport_gesture &&
            !playing && !mode_pending && !logs_visible &&
            view_state.paused && ((view_state.mode == ViewMode::scene && selected_instances.size() != 0 &&
            interaction.value() == InteractionMode::objects) || (view_state.mode==ViewMode::mesh && mesh_tools.mode()!=MeshSelectMode::surface)));
        const auto file_shortcut = take_file_shortcut(shortcut_input, !dialog_was_open && !mode_pending &&
                                                                    !editing.awaiting_remote() && !viewport_gesture);
        const bool viewport_tab = take_viewport_tab(
            shortcut_input, !dialog_was_open && (viewport_keyboard || !ui_keyboard_capture) && !editing.awaiting_remote() && !viewport_gesture &&
                          !playing && !mode_pending && !logs_visible && debug_link.value() &&
                          image_id && editable_mesh(state) &&
                          image.bounds().contains(viewport_raw.pointer));
        tool_panel.validate();tool_panel.layout(image.bounds());
        mesh_navigation.layout(image.bounds(),view_state.mode==ViewMode::mesh,mesh_tools.mode()==MeshSelectMode::whole,
            show_fps.value(),!viewport_interaction.busy()&&!editing.busy()&&!walk.active()&&!playing&&!modal_visible());
        const bool viewport_popup=mesh_tools.menu_open() || regions.menu_open() || tool_panel.contains(viewport_raw.pointer) || mesh_navigation.contains(viewport_raw.pointer);
        if (viewport_popup && !viewport_window.opened()) ui_input.events.clear();
        auto input = screen.update(ui_input, dt, &clipboard);
        if (!input && raw.overflow) {
            // Screen discarded the incomplete input burst and released capture.
            // The authoring gestures above have already rolled back; publish
            // that compensation and resume with the next complete input frame.
            ui_keyboard_capture = false;
            ui_shortcut_capture = false;
            status.text(input.error().message);
            broadcast();
            continue;
        }
        if (!input)
            return fail(input.error().message);
        // Apply captured divider movement before presenting this input frame.
        // Neither path touches document revisions, history, nor preview IPC.
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
        if (panel_height || section_changed || resize_cancelled) layout(raw);
        // Panels get first refusal when docked (except an open viewport popup).
        // Detached menus consume only their own window's input.
        if (!viewport_window.opened() && !viewport_popup) {
            viewport_ui_input=ui_input;
            viewport_ui_input.events=input->events;
        }
        auto viewport_input=viewport_popups.update(viewport_ui_input,dt,&clipboard);
        if (!viewport_input) {
            if (!viewport_raw.overflow) return fail(viewport_input.error().message);
            status.text(viewport_input.error().message); broadcast(); continue;
        }
        if(auto options=mesh_navigation.poll();options && view_state.mode==ViewMode::mesh &&
           mesh_tools.mode()==MeshSelectMode::whole && !viewport_interaction.busy() && !editing.busy() && !modal_visible()) {
            auto baked=bake_mesh_camera(editing,*options);
            if(!baked)status.text(baked.error().message);
            else {
                if(*baked){refresh_vertex();camera_changed();}
                status.text(*baked?"Camera rotation/scale baked to mesh draft / Apply to publish, Save on disk to persist":"No selected camera transform to bake");
            }
        }
        const bool tool_menu_input=mesh_navigation.contains(viewport_raw.pointer) ||
            (tool_panel.opened() && (tool_panel.contains(viewport_raw.pointer) ||
            viewport_input->capturesPointer || viewport_input->capturesShortcuts));
        tool_panel.poll();
        if(!tool_panel.status().empty())status.text(tool_panel.status());
        input::Frame viewport_background{.logical_size=viewport_ui_input.logical_size,
            .framebuffer=viewport_ui_input.framebuffer,.pointer=viewport_ui_input.pointer,
            .focused=viewport_ui_input.focused};
        if (auto updated=viewport_screen.update(viewport_background,dt);!updated) return fail(updated.error().message);
        const auto& shortcut_raw=viewport_keyboard ? viewport_raw : raw;
        // Docked viewport popups also get first refusal. Forward their unused
        // shortcuts rather than querying the controls screen we just bypassed.
        const auto shortcut_events=viewport_keyboard || viewport_popup ? viewport_input->unhandled() : input->unhandled();
        auto shortcuts=edit_shortcuts(shortcut_events,shortcut_raw.events,
            shortcut_raw.focused && !shortcut_raw.overflow && !dialog_was_open && !mode_pending && !editing.awaiting_remote() &&
            !viewport_gesture && !playing && !logs_visible);
        const bool viewport_drag=viewport_interaction.busy() && !camera_numeric_active &&
            !(editing.active(EditGesture::scale) && !scaling.dragging() && !instance_transform.active());
        const auto& gesture_raw=viewport_drag ? viewport_raw : raw;
        const bool scale_cancelled=editing.active(EditGesture::scale) && !instance_transform.active() && (gesture_raw.overflow || !gesture_raw.focused ||
            std::ranges::any_of(gesture_raw.events,[](const auto& event) {
                return event.kind==input::EventKind::focus_lost ||
                    (event.kind==input::EventKind::key_down && event.key==input::Key::escape);
            }));
        if(scale_cancelled) finish_scale(true);
        yaw.poll(); pitch.poll(); orbit_distance.poll(); optical_zoom.poll();
        const bool camera_cancelled = editing.active(EditGesture::camera) &&
            (gesture_raw.overflow || !gesture_raw.focused || std::ranges::any_of(gesture_raw.events, [](const auto& event) {
                return event.kind == input::EventKind::focus_lost ||
                    (event.kind == input::EventKind::key_down && event.key == input::Key::escape);
            }));
        if (camera_cancelled) finish_camera(true);
        for (const auto* control : {&yaw, &pitch, &orbit_distance, &optical_zoom})
            if (!control->status().empty()) status.text(control->status());
        ui_keyboard_capture = input->capturesKeyboard;
        ui_shortcut_capture = input->capturesShortcuts;
        const auto previous_mesh_mode=mesh_tools.mode();
        mesh_tools.sync(state);
        if(mesh_tools.mode()!=previous_mesh_mode && mesh_tools.mode()!=MeshSelectMode::surface) {
            (void)blueprint_mesh_panel.select_part(0);
            viewport_interaction.mesh_part.cancel();
        }
        if(dialog_was_open && mesh_tools.menu_open()) {
            if(auto action=mesh_tools.poll(viewport_raw.events)) mesh_action(*action);
        }
        if(dialog_was_open && regions.menu_open())regions.poll_menu(editing,viewport_raw.events);
        if(pop_viewport.clicked() && !dialog_was_open) toggle_viewport=true;
        const bool toolbar_menu_was_open = main_bar.opened() || scene_bar.opened();
        main_bar.poll(raw.events, !dialog_was_open, input->capturesPointer);
        scene_bar.poll(raw.events, !dialog_was_open, input->capturesPointer);
        if (main_bar.opened() || scene_bar.opened()) { camera_open = false; close_list_flyouts(); timeline.close_menu(); }
        if(show_camera.clicked() && !dialog_was_open) {
            timeline.close_menu();
            close_list_flyouts();
            main_bar.close(); scene_bar.close();
            camera_open=!camera_open;
            if(camera_open) { camera_preferences.show(settings); camera_menu.scroll(0); }
        }
        if(camera_open && (close_camera.clicked() || dialog_was_open ||
            std::ranges::any_of(raw.events,[&](const auto& event) {
                return event.kind==input::EventKind::key_down && event.key==input::Key::escape;
            }))) camera_open=false;
        camera_menu.visible(camera_open);
        instance_flyout.poll(raw.events, show_instances.bounds(), lists_enabled);
        blueprint_flyout.poll(raw.events, show_blueprints.bounds(), lists_enabled);
        if (show_instances.clicked() && lists_enabled) {
            timeline.close_menu();
            const bool opening = !instance_flyout.opened();
            close_list_flyouts(); main_bar.close(); scene_bar.close();
            camera_open = false; camera_menu.visible(false);
            if (opening) {
                floating_instances.sync(scene_instances(state), blueprint_catalog(state));
                floating_instances.selection(selected_instances);
                instance_flyout.layout(raw.logical_size, scene_bar.popup_anchor(show_instances));
                instance_flyout.open();
            }
        }
        if (show_blueprints.clicked() && lists_enabled) {
            timeline.close_menu();
            const bool opening = !blueprint_flyout.opened();
            close_list_flyouts(); main_bar.close(); scene_bar.close();
            camera_open = false; camera_menu.visible(false);
            if (opening) {
                floating_blueprints.sync(blueprint_catalog(state));
                blueprint_flyout.layout(raw.logical_size, scene_bar.popup_anchor(show_blueprints));
                blueprint_flyout.open();
                floating_blueprints.layout();
            }
        }
        if(camera_open) camera_preferences.poll();
        if(camera_open && camera_preferences.applied()) {
            auto next=camera_preferences.read(settings);
            if(!next) status.text(next.error().message);
            else if(auto saved=save_settings(preferences_path,*next);!saved) status.text(saved.error().message);
            else {
                settings=*next;
                orbit_distance.slider_range(settings.orbit_distance.minimum,settings.orbit_distance.maximum);
                send_settings(session->active_generation());
                for(auto generation:candidates) send_settings(generation);
                status.text("Camera preferences saved / scene unchanged");
            }
        }
        if (auto moves = move_camera_scroll.changedValue()) {
            auto next = settings;
            next.scroll_moves_camera = *moves;
            if (auto saved = save_settings(preferences_path, next); !saved) {
                status.text(saved.error().message);
                move_camera_scroll.value(settings.scroll_moves_camera);
            } else settings = next;
        }
        if (show_settings.clicked() && !dialog_was_open) {
            camera_open=false; camera_menu.visible(false);
            cancel_viewport(); timeline.cancel();
            settings_panel.open(settings, state.document.timeline.tracks().size(), state.document.instances.size());
        }
        if(!dialog_was_open && !viewport_gesture && (smaller_ui.clicked() || larger_ui.clicked())) {
            auto next=settings;
            next.ui_scale_percent=static_cast<unsigned>(std::clamp(static_cast<int>(settings.ui_scale_percent)+(larger_ui.clicked()?5:-5),75,150));
            if(auto saved=save_settings(preferences_path,next);!saved) status.text(saved.error().message);
            else {
                cancel_viewport(); camera_open=false; camera_menu.visible(false);
                settings=next;
                status.text("UI scale saved: "+std::to_string(settings.ui_scale_percent)+"%");
            }
        }
        if (settings_was_open) {
            if (auto next = settings_panel.poll(raw.events)) {
                const auto previous_vsync = app->window().vsync();
                if (auto applied = app->window().set_vsync(options.automation || viewport_window.opened() ? window::VSync::off : next->vsync); !applied)
                    settings_panel.error(applied.error().message);
                else if (auto saved = save_settings(preferences_path, *next); !saved) {
                    if (auto restored = app->window().set_vsync(previous_vsync); !restored)
                        return fail(restored.error().message);
                    settings_panel.error(saved.error().message);
                }
                else {
                    settings = *next;
                    move_camera_scroll.value(settings.scroll_moves_camera);
                    // The panel has already validated the preference. This is
                    // session policy, not a scene edit or a worker allocation.
                    if (auto configured = editing.timeline_track_limit(settings.timeline_track_limit); !configured)
                        return fail(configured.error().message);
                    if (auto configured = editing.instance_limit(settings.instance_limit); !configured)
                        return fail(configured.error().message);
                    orbit_distance.slider_range(settings.orbit_distance.minimum, settings.orbit_distance.maximum);
                    next_ui_frame = {};
                    send_settings(session->active_generation());
                    for (auto generation : candidates) send_settings(generation);
                    settings_panel.close();
                    status.text("Editor settings saved / scene unchanged");
                }
            }
        }
        if (show_import.clicked() && !dialog_was_open) {
            cancel_viewport(); timeline.cancel();
            import_dialog.open(import_directory);
        }
        if (import_was_open) {
            if (const auto file = import_dialog.poll(raw.events)) {
                const auto imported = editing.import_asset(*file);
                if (!imported) {
                    import_dialog.error(imported.error().message);
                } else {
                    const auto blueprint=find_instance(state,*imported)->blueprint;
                    const bool mesh=is_mesh_blueprint(state,blueprint);
                    if(mesh)open_mesh(blueprint);
                    else {
                        view_state.mode=ViewMode::sun;
                        view_state.paused=true;
                        end_camera_visit(false);
                        view_state.selected_vertex=0;
                        // Isolated views use 0.52 of the orbit distance. Leave
                        // space beyond the body for prominences and the corona.
                        view_state.editor_camera={.distance=effect_blueprint_settings(state,blueprint)->radius*9.F};
                        select_object(*imported);
                        view_changed();
                    }
                    inspector.clear(); translation.cancel();
                    sidebar_tab = SidebarTab::properties;
                    interaction.value(mesh?InteractionMode::vertices:InteractionMode::objects);
                    refresh(true);
                    import_directory = file->parent_path();
                    import_dialog.close();
                    status.text("Imported " + file->filename().string() +
                                " / new blueprint and instance / choose View > Scene for placement");
                }
            }
        }
        const bool fullscreen_key = std::ranges::any_of(raw.events, [](const auto& event) {
            return event.kind == input::EventKind::key_down && event.key == input::Key::f11 &&
                   !event.repeat;
        });
        if (fullscreen.clicked() || fullscreen_key) {
            if (auto mode = window.set_fullscreen(!window.fullscreen()); !mode)
                status.text(mode.error().message);
            fullscreen.text(window.fullscreen() ? "Windowed / F11" : "Fullscreen / F11");
        }
        if (show_scene.clicked()) sidebar_tab = SidebarTab::scene;
        if (show_keyframe.clicked()) {
            sidebar_tab = SidebarTab::keyframe;
            delete_target = DeleteTarget::keyframe;
        }
        if (worker_log_tab.clicked()) { timing_panel.visible(false); log_text.visible(true); }
        if (timing_tab.clicked()) { timing_panel.visible(true); log_text.visible(false); }
        if (logs_visible)
            if (auto message = timing_panel.update(timings)) status.text(*message);
        if (show_base.clicked()) {
            sidebar_tab = SidebarTab::properties;
            delete_target = DeleteTarget::object;
        }
        sync_sidebar();
        if (play.clicked()) {
            if (playing) {
                if (auto sent = session->send("play\n0"); !sent)
                    status.text(sent.error().message);
                else {
                    wanted_playing = false;
                    mode_pending = true;
                    mode_started = now;
                    play.text("Stopping...");
                }
            } else
                launch(session->active_generation());
        }
        if (auto linked = debug_link.changedValue()) {
            const auto gen = session->active_generation();
            const auto sent = session->send(*linked ? "link\n1" : "link\n0", gen);
            if (!sent) {
                debug_link.value(!*linked);
                status.text(sent.error().message);
            } else {
                updates.reset(gen);
                if (*linked) {
                    send_snapshot(gen);
                    session->send(diagnostic.value() ? "diagnostic\n1" : "diagnostic\n0", gen);
                    status.text("Debug link ON / synchronizing the latest authored scene");
                } else {
                    translation.cancel();
                    frame_label.text(
                        "Debug link OFF / frozen preview; no image readback or live edits");
                    status.text("Disconnected. Local edits are retained. Play boots from this "
                                "document; reconnect applies changes.");
                }
            }
        }
        if (show_logs.clicked()) {
            logs_visible = !logs_visible;
            log_panel.visible(logs_visible);
            translation.cancel();
        }
        if (logs_visible) {
            const auto logs = session->logs();
            const auto tail = logs.substr(logs.size() > 32768 ? logs.size() - 32768 : 0);
            if (displayed_logs != tail) {
                displayed_logs = tail;
                log_text.value(displayed_logs.empty() ? "No build/worker output yet."
                                                      : displayed_logs);
            }
        }
        if (reload.clicked()) {
            if (auto r = session->request_reload(); !r)
                status.text(r.error().message);
            else
                status.text("Compiling C++ asynchronously; current preview remains usable...");
        }
        if (auto value = gizmo_only.changedValue()) {
            view_state.gizmo_only=*value;
            scene_bar.close();
            viewport_changed();
            status.text(*value ? "Active instance surface hidden in preview / gizmo remains / scene unchanged" :
                "Active instance surface restored / scene unchanged");
        }
        if (auto value = diagnostic.changedValue()) {
            const std::string message = *value ? "diagnostic\n1" : "diagnostic\n0";
            session->send(message);
            for (auto gen : candidates)
                session->send(message, gen);
            viewport_changed();
        }
        if (!dialog_was_open && !modal_visible() && !editing.awaiting_remote() && !viewport_gesture) {
            bool cycled{};
            for (int i = 0; i < std::abs(gizmo_cycle); ++i)
                if(view_state.mode==ViewMode::mesh) {
                    if(mesh_tools.cycle(gizmo_cycle>0?1:-1))selection_overlay_dirty=true;
                } else cycled |= gizmo_mode.cycle(gizmo_cycle > 0 ? 1 : -1);
            if (gizmo_mode.changedValue() || cycled) {
                cancel_viewport();
                selection_overlay_dirty = true;
                status.text(gizmo_mode.value() == GizmoMode::scale
                    ? "Scale: drag the gold square / each selected instance scales about its own origin / Escape cancels"
                    : gizmo_mode.value() == GizmoMode::attitude
                    ? "Yaw / pitch / roll: turn each selected ship about its own up / right / forward axis / Escape cancels"
                    : gizmo_mode.value() == GizmoMode::forward
                    ? "Forward / back: move the selection along the active ship's forward axis / Escape cancels"
                    : gizmo_mode.value() == GizmoMode::rotate
                    ? "Rotate: X/Y/Z Euler rings / each selected instance turns in place / Escape cancels"
                    : "Move: drag a colored axis / Escape cancels");
            }
            if (pause.clicked()) {
                view_state.paused = !view_state.paused;
                view_state.time = std::min(view_state.time, state.document.timeline_duration);
                playback_changed();
            }
            if (undo.clicked()) shortcuts.insert(shortcuts.begin(), EditShortcut::undo);
            else if (redo.clicked()) shortcuts.insert(shortcuts.begin(), EditShortcut::redo);
            for (const auto shortcut : shortcuts) {
                if (shortcut == EditShortcut::undo || shortcut == EditShortcut::redo) {
                    const auto result = shortcut == EditShortcut::undo ? editing.undo() : editing.redo();
                    if (!result) status.text(result.error().message);
                    else if (*result) {
                        completed_frames.clear();
                        refresh(true);
                        broadcast();
                    }
                }
                if(shortcut==EditShortcut::copy) {
                    const bool keyframes = delete_target == DeleteTarget::keyframe;
                    const auto count = keyframes ? timeline.selected_keyframes().size() : selected_instances.size();
                    auto copied=keyframes
                        ? editing.copy_keyframes(timeline.selected_keyframes())
                        : editing.copy_instances(
                            view_state.mode!=ViewMode::mesh && interaction.value()==InteractionMode::objects
                                ? selected_instances.items() : std::span<const u32>{});
                    if (!copied) status.text("Copy failed: " + copied.error().message);
                    else status.text("Copied " + std::to_string(count) +
                        (keyframes ? (count == 1 ? " keyframe" : " keyframes") : (count == 1 ? " instance" : " instances")) +
                        (keyframes ? " / Ctrl+V preserves relative timing" : " / Ctrl+V pastes in place"));
                }
                if(shortcut==EditShortcut::paste) {
                    auto pasted=editing.paste();
                    if(!pasted) status.text("Paste failed: " + pasted.error().message);
                    else {
                        inspector.clear(); translation.cancel(); scaling.cancel();
                        refresh();
                        if(pasted->object) {
                            selected_instances.select(pasted->objects);
                            refresh_selection();
                            delete_target=DeleteTarget::object;
                            if (sidebar_tab == SidebarTab::keyframe) sidebar_tab=SidebarTab::properties;
                            interaction.value(InteractionMode::objects);
                            gizmo_mode.value(GizmoMode::move);
                            selection_overlay_dirty=true;
                        } else {
                            timeline.select_keyframes(pasted->keyframes);
                            show_timeline();
                            delete_target=DeleteTarget::keyframe;
                            sidebar_tab=SidebarTab::keyframe;
                        }
                        const auto count = pasted->object ? pasted->objects.size() : pasted->keyframes.size();
                        status.text("Pasted " + std::to_string(count) +
                            (pasted->object ? (count == 1 ? " instance" : " instances") : (count == 1 ? " keyframe" : " keyframes")) +
                            (pasted->object ? " in place / copies overlap originals / Move to separate them"
                                            : " at playhead / Ctrl+Z undoes paste"));
                    }
                }
            }
            auto picked_instance = instance_list.poll(raw.events);
            if (const auto picked = region_instances.poll(raw.events)) picked_instance = picked;
            if (instance_flyout.opened())
                if (const auto picked = floating_instances.poll(raw.events)) picked_instance = picked;
            if (picked_instance) select_scene_instance(picked_instance->id, picked_instance->mode);
            std::optional<BlueprintId> add_blueprint;
            std::optional<BlueprintId> edit_blueprint;
            auto blueprint_action = blueprints_view.poll();
            if (blueprint_flyout.opened())
                if (const auto action = floating_blueprints.poll()) blueprint_action = action;
            if (blueprint_action) {
                if (blueprint_action->create_instance) add_blueprint = blueprint_action->id;
                else edit_blueprint = blueprint_action->id;
            }
            if (edit_blueprint) {
                if (mesh_geometry(state, *edit_blueprint)) open_mesh(*edit_blueprint);
                else {
                    const auto found = std::ranges::find(state.document.instances, *edit_blueprint, &SceneInstance::blueprint);
                    if (found == state.document.instances.end())
                        status.text("Add an instance (+) to inspect this effect's controls");
                    else {
                        view_state.mode = (*edit_blueprint==BlueprintId::region || *edit_blueprint==BlueprintId::camera)
                            ? ViewMode::scene : ViewMode::sun;
                        if (*edit_blueprint != BlueprintId::camera) end_camera_visit(false);
                        select_object(found->id);
                        view_state.selected_vertex = 0;
                        interaction.value(InteractionMode::objects);
                        view_changed();
                    }
                }
            }
            if (add_blueprint) {
                auto created = editing.instantiate(*add_blueprint);
                if (!created) status.text(created.error().message);
                else {
                    view_state.mode = ViewMode::scene;
                    inspector.clear(); translation.cancel();
                    select_object(*created);
                    refresh_selection();
                    refresh_vertex();
                    interaction.value(InteractionMode::objects);
                    status.text("Added " + object_name(state, *created) + " / blueprint retained for reuse");
                }
            }
            {
                const auto* selected_camera = is_camera_instance(state, view_state.selected_object)
                    ? find_instance(state, view_state.selected_object) : nullptr;
                const auto* active_now = active_camera(state, view_state.time);
                const bool can_author = editing.can_edit_scene_pose() && !playing && !mode_pending && !editing.awaiting_remote();
                camera_panel.sync(view_state.mode == ViewMode::scene ? selected_camera : nullptr, camera_visit,
                                  selected_camera && active_now == selected_camera, can_author,
                                  view_state.mode == ViewMode::scene && !playing && !logs_visible);
                camera_panel.enabled(!dialog_was_open && !mode_pending && !editing.busy());
                if (camera_panel.shown()) {
                    // Beside the glyph body when it is on screen, else the viewport's top-right corner.
                    const auto bounds = image.bounds();
                    constexpr f32 panel_width = 324, panel_height = 236;
                    ui::Rect at{bounds.x + bounds.width - panel_width - 12, bounds.y + 12, panel_width, panel_height};
                    if (selected_camera)
                        if (const auto snapshot = image_camera.snapshot(image_extent)) {
                            const auto body = camera_glyph(evaluate_instance(state, *selected_camera, view_state.time)).body_center();
                            Vec4 clip{};
                            for (std::size_t row = 0; row < 4; ++row) {
                                clip[row] = snapshot->view_projection[3][row];
                                for (std::size_t column = 0; column < 3; ++column)
                                    clip[row] += snapshot->view_projection[column][row] * body[column];
                            }
                            if (clip.w > 0) {
                                const auto x = bounds.x + (clip.x / clip.w * .5F + .5F) * bounds.width;
                                const auto y = bounds.y + (.5F - clip.y / clip.w * .5F) * bounds.height;
                                if (bounds.contains({x, y})) {
                                    at.x = std::clamp(x + 36, bounds.x, bounds.x + bounds.width - panel_width);
                                    at.y = std::clamp(y - panel_height * .5F, bounds.y, bounds.y + bounds.height - panel_height);
                                }
                            }
                        }
                    camera_panel.place(at);
                }
                if (camera_panel.enter_clicked() && selected_camera) visit_camera(*selected_camera, true);
                if (camera_panel.inspect_clicked() && selected_camera) visit_camera(*selected_camera, false);
                if (camera_panel.back_clicked()) { end_camera_visit(true); status.text("Editor view restored"); }
                if (camera_panel.save_clicked() && camera_visit && camera_visit->editable) {
                    const auto name = object_name(state, camera_visit->camera);
                    auto saved = editing.set_camera(camera_visit->camera, view_state.editor_camera);
                    if (!saved) status.text(saved.error().message);
                    else {
                        status.text(*saved ? "Saved the editor view into " + name + " at this keyframe"
                                           : name + " already matches the editor view");
                        if (*saved) { show_timeline(); refresh_selection(); }
                    }
                }
                if (camera_panel.activate_clicked() && selected_camera) {
                    const auto name = selected_camera->name;
                    auto activated = editing.set_active_camera(selected_camera->id);
                    if (!activated) status.text(activated.error().message);
                    else {
                        status.text(*activated ? name + " is the active camera from this keyframe on"
                                               : name + " is already the active camera here");
                        if (*activated) { show_timeline(); refresh_selection(); }
                    }
                }
            }
            if (auto mode = interaction.changedValue()) {
                interaction_changed();
            }
            if (auto value = view.changedValue()) {
                if (value->mode == ViewMode::mesh) open_mesh(value->blueprint);
                else {
                    view_state.mode = value->mode;
                    view_state.selected_vertex = 0;
                    end_camera_visit(false);
                    view_state.editor_camera.target = {};
                    interaction.value(InteractionMode::objects);
                    if (value->mode == ViewMode::sun) {
                        const auto* effect = view_instance(state, BlueprintKind::sun);
                        select_object(effect ? effect->id : 0);
                    }
                    view_changed();
                }
            }
            if (auto value = weld.changedValue()) {
                view_state.weld = *value;
                viewport_changed();
            }
            if (editing_mesh && (apply_vertex.clicked() || minus.clicked() || plus.clicked())) {
                auto px = number(x.getText()), py = number(y.getText()), pz = number(z.getText());
                if (!px || !py || !pz)
                    status.text("Vertex components must be finite numbers.");
                else {
                    Vec3 position{*px, *py, *pz};
                    if (minus.clicked() || plus.clicked())
                        position = add(editable_mesh(state)->position(view_state.selected_vertex),
                                       {minus.clicked() ? -.1F : .1F, 0, 0});
                    const auto ids = selected_mesh_vertices();
                    if(position==editable_mesh(state)->position(view_state.selected_vertex)) continue;
                    const auto target=mesh_target(state);
                    if (auto moved = editing.translate_vertices(target->blueprint,
                            ids, subtract(position, editable_mesh(state)->position(view_state.selected_vertex)));
                        !moved)
                    {
                        status.text(moved.error().message);
                    }
                    else {
                        refresh_vertex();
                    }
                }
            }
        }
        if (!dialog_was_open && !modal_visible() && !editing.awaiting_remote() && !camera_cancelled &&
            !inspecting() && (!viewport_gesture || camera_numeric_active) &&
            (yaw.changedValue() || pitch.changedValue() || orbit_distance.changedValue() || optical_zoom.changedValue())) {
            const auto before = preview_camera_pose(state, view_state.time);
            auto after = before;
            after.yaw = yaw.value(); after.pitch = pitch.value();
            after.distance = before.distance;
            if (optical_zoom.changedValue()) after.zoom = optical_zoom.value();
            if (orbit_distance.changedValue()) {
                if (orbit_distance.value() < settings.orbit_distance.minimum || orbit_distance.value() > settings.orbit_distance.maximum) {
                    orbit_distance.reset(before.distance);
                    status.text("Orbit distance is outside its range; adjust its limits in Settings");
                } else after.distance = orbit_distance.value();
            }
            view_state.smooth_zoom = false;
            camera_edited(before, after);
        }
        const auto previous_mesh_part=blueprint_mesh_panel.selected_part();
        if(blueprint_mesh_panel.poll(!dialog_was_open && !modal_visible() && !viewport_gesture && !mode_pending && !editing.busy())) {
            // Attribute-only blueprint edits keep component selection, masks,
            // edge caches and unrelated scene/timeline widgets intact.
            mesh_tools.sync(state);refresh_vertex();
        }
        if(blueprint_mesh_panel.selected_part()!=previous_mesh_part && blueprint_mesh_panel.selected_part())
            mesh_tools.mode(MeshSelectMode::surface);
        if(!blueprint_mesh_panel.status().empty() && blueprint_mesh_panel.status()!=last_blueprint_status) {
            last_blueprint_status=blueprint_mesh_panel.status();status.text(last_blueprint_status);
        }
        if(!dialog_was_open && !modal_visible() && !editing.awaiting_remote() &&
           (!viewport_gesture || (editing.active(EditGesture::scale) && !scaling.dragging() && !instance_transform.active()))) {
            auto events = inspector.poll();
            if(const auto edit=inspector.number_edit("transform","scale"); edit &&
               view_state.mode==ViewMode::scene && !playing && editing.can_edit_scene_pose() && !scale_cancelled) {
                if(edit->changed || edit->pressed) edit_scale(edit->value,!edit->pressed);
                else if(editing.active(EditGesture::scale) && !edit->pressed) finish_scale(false);
            }
            if (!inspector.status().empty())
                status.text(inspector.status());
            if (!events.empty())
                send_event(std::move(events.front()));
        }
        if (timeline_enabled && !modal_visible() && !editing.awaiting_remote() && !playing && !mode_pending && raw.focused &&
            !raw.overflow) {
            if (auto action = timeline.poll(input->unhandled(), raw.events)) {
                if (action->kind == TimelineAction::Kind::select_object) {
                    if (action->object == camera_animation_object) {
                        // The legacy shot row stands in for the scene camera.
                        if (const auto* camera = active_camera(state, view_state.time))
                            select_scene_instance(camera->id, action->selection_mode);
                        else timeline.focus_object(camera_animation_object);
                    } else select_scene_instance(static_cast<u32>(action->object), action->selection_mode);
                } else {
                    regions.deselect();
                    delete_target = DeleteTarget::keyframe;
                    if (action->kind == TimelineAction::Kind::add || timeline.selected_keyframe()) {
                        sidebar_tab = SidebarTab::keyframe;
                    }
                    if (action->kind == TimelineAction::Kind::seek) {
                        if (view_state.time != action->time || !view_state.paused) {
                            view_state.time = action->time;
                            view_state.paused = true;
                            playback_changed();
                        }
                        show_timeline();
                    } else {
                        content::Result<bool> result{false};
                        const auto previous_view = view_state;
                        switch (action->kind) {
                        case TimelineAction::Kind::add:
                            result = editing.add_keyframe(action->time);
                            break;
                        case TimelineAction::Kind::edit:
                            result = editing.update_keyframe(action->time, action->destination,
                                                             action->name, action->values);
                            break;
                        case TimelineAction::Kind::apply_range:
                            result = editing.apply_keyframe_range(action->time, action->destination, action->changes);
                            break;
                        case TimelineAction::Kind::erase:
                            result = editing.erase_keyframes(action->selection.empty()
                                ? std::span{&action->time, 1} : std::span{action->selection});
                            break;
                        case TimelineAction::Kind::duration:
                            result = editing.duration(action->time);
                            break;
                        case TimelineAction::Kind::seek:
                        case TimelineAction::Kind::select_object:
                            break;
                        }
                        if (!result)
                            timeline.error(result.error().message);
                        else {
                            if (action->kind == TimelineAction::Kind::apply_range)
                                status.text("Applied edited fields to keyframes from " + std::to_string(action->time) +
                                            " to " + std::to_string(action->destination) + " seconds / Undo restores the whole range");
                            if (view_state.time != previous_view.time || view_state.paused != previous_view.paused)
                                playback_changed();
                            // Selection/inspector follow the local transaction in
                            // this input frame; never wait for preview completion.
                            show_timeline();
                        }
                    }
                }
            }
            // A scrub or Ctrl-click may clear the selection without changing
            // the playhead. Update authoring permission before viewport tools.
            editing.select_keyframe(timeline.selected_keyframe());
            update_pose_controls();
        }
        if (timeline.menu_open()) {
            camera_open = false; camera_menu.visible(false);
            close_list_flyouts(); main_bar.close(); scene_bar.close();
        }
        const auto viewport = image.bounds();
        if (!dialog_was_open && !modal_visible() && !viewport_gesture && !editing.busy() && !playing) {
            if (bounds_panel.apply_requested()) {
                auto value = bounds_panel.read();
                if (!value) status.text(value.error().message);
                else if (auto begun = editing.begin_world_bounds(); !begun) status.text(begun.error().message);
                else if (auto changed = editing.world_bounds(*value); !changed) {
                    finish_bounds(true); status.text(changed.error().message);
                } else finish_bounds(false);
            }
            if (bounds_panel.frame_requested()) {
                // Framing an editor aid is private navigation; leave any camera visit.
                end_camera_visit(false);
                const auto& bounds = state.document.world_bounds;
                auto& pose = view_state.editor_camera;
                float radius{};
                for (unsigned c = 0; c < 3; ++c) {
                    pose.target[c] = (bounds.minimum[c]+bounds.maximum[c])*.5F;
                    radius = std::hypot(radius,(bounds.maximum[c]-bounds.minimum[c])*.5F);
                }
                pose.distance = std::clamp(radius / std::sin(camera_vertical_fov * std::numbers::pi_v<float>/360) * 1.15F,
                    settings.orbit_distance.minimum, settings.orbit_distance.maximum);
                pose.yaw = 30; pose.pitch = 20; pose.zoom = 1; view_state.smooth_zoom = false;
                bounds_panel.show(); camera_changed();
                status.text("World bounds / drag a face handle to resize / Escape cancels");
            }
        }
        const bool image_time_current = presented_info.time == static_cast<double>(view_state.time);
        const bool viewport_ready =
            !dialog_was_open && !modal_visible() && !playing && !mode_pending && debug_link.value() && !editing.awaiting_remote() &&
            !unresponsive && !logs_visible && !timeline.dragging() && !timeline.handledPointer() &&
            image_id && (viewport_raw.focused || raw.focused) && !viewport_raw.overflow &&
            image_generation == session->active_generation() && matches_view(presented_info,state);
        // An overflow menu blocks viewport input, not its own authoring
        // controls (notably the region shape selector inside Tools...).
        const bool toolbar_blocks_viewport = !viewport_window.opened() &&
            (toolbar_menu_was_open || main_bar.opened() || scene_bar.opened());
        const bool list_blocks_viewport = !viewport_window.opened() &&
            (instance_flyout.contains(viewport_raw.pointer) || blueprint_flyout.contains(viewport_raw.pointer) ||
             timeline.menu_contains(viewport_raw.pointer));
        // The camera overlay swallows presses that start on it, never a drag in
        // progress nor a press that began elsewhere in this input frame.
        const bool camera_overlay_blocks = camera_panel.contains(viewport_raw.pointer) && !navigation.dragging() &&
            !viewport_interaction.busy() && std::ranges::none_of(viewport_raw.events, [&](const input::Event& event) {
                return event.kind == input::EventKind::pointer_down && !camera_panel.contains(event.position);
            });
        const bool viewport_enabled = viewport_ready && !toolbar_blocks_viewport && !list_blocks_viewport && !camera_overlay_blocks;
        const auto previous_camera = preview_camera_pose(state, view_state.time);
        if (walk_button.clicked() && viewport_enabled && !inspecting()) {
            finish_camera(false);
            walk.active(!walk.active());
            status.text(walk.active() ? "Walk: WASD / Q down / E up / Shift fast / middle drag turns / Escape exits" : "Walk mode off");
        }
        auto navigated_camera = previous_camera;
        const bool navigation_was_dragging = navigation.dragging();
        navigation.scroll_mode(move_camera_scroll.value() ? NavigationTool::ScrollMode::move_forward : NavigationTool::ScrollMode::zoom);
        navigation.look_in_place(walk.active());
        navigation.speeds(settings.camera_drag);
        navigation.drag_origin(walk.active()?std::nullopt:mesh_navigation.origin(state));
        const auto* rotation_target=find_instance(state,view_state.selected_object);
        const bool free_object_rotation=view_state.mode==ViewMode::scene && editing.can_edit_scene_pose() &&
            interaction.value()==InteractionMode::objects && gizmo_mode.value()==GizmoMode::free_rotate &&
            rotation_target && evaluate_visibility(state,*rotation_target,view_state.time);
        const bool free_mesh_rotation=view_state.mode==ViewMode::mesh && interaction.value()==InteractionMode::vertices &&
            mesh_tools.transform_mode()==GizmoMode::free_rotate &&
            (mesh_tools.mode()==MeshSelectMode::whole || !mesh_tools.selected().empty());
        navigation.orbit_enabled(walk.active() || !(free_object_rotation || free_mesh_rotation || regions.free_rotation_selected()));
        const bool navigated = viewport_interaction.update(ViewportTool::navigation, [&](bool available) {
            const bool enabled=available && viewport_enabled && !inspecting() &&
                !camera_cancelled && !camera_numeric_active;
            const bool pointer_moved=navigation.update(navigated_camera, view_state.mode, view_state.smooth_zoom,
                              {viewport.x, viewport.y}, {viewport.width, viewport.height},
                              viewport_input->unhandled(), viewport_raw.events,
                              enabled && viewport_raw.focused && !viewport_raw.overflow);
            // Arming walk from the controls window must survive the subsequent
            // focus transfer. While controls own focus, release held keys only.
            bool walked{};
            if (viewport_window.opened() && raw.focused && !viewport_raw.focused) walk.stop();
            else walked=walk.update(navigated_camera,dt,viewport_raw,settings.walk,
                enabled && !viewport_input->capturesShortcuts && (viewport_keyboard || !input->capturesShortcuts));
            if(walked) view_state.smooth_zoom=false;
            return pointer_moved || walked;
        });
        walk_button.text(walk.active() ? "Stop walking" : "Walk camera");
        if (navigation.cancelled()) finish_camera(true);
        else if (navigated) {
            camera_edited(previous_camera, navigated_camera);
        }
        viewport_interaction.gizmo_input.route(viewport_interaction.transforming(),
            navigation_was_dragging || navigation.handledPointer(),tool_menu_input,viewport_raw.pointer,
            viewport_raw.events,viewport_input->unhandled(),dt,
            viewport_enabled && viewport_raw.focused && !walk.active() && !viewport_input->capturesShortcuts &&
            (viewport_interaction.transforming() || image.bounds().contains(viewport_raw.pointer)) &&
            (viewport_keyboard || !ui_shortcut_capture));
        const auto tool_raw=viewport_interaction.gizmo_input.events();
        const auto tool_input=viewport_interaction.gizmo_input.unhandled();
        const auto arrow_step=viewport_interaction.gizmo_input.arrow_step();
        if (scroll_trace.enabled())
            scroll_trace.input(viewport_raw.events, viewport_input->unhandled(), viewport, navigation_was_dragging,
                previous_camera, preview_camera_pose(state, view_state.time), view_state.sequence,
                {{"modal", dialog_was_open || modal_visible()}, {"independent-play", playing},
                 {"mode-transition", mode_pending}, {"debug-link-off", !debug_link.value()},
                 {"callback-pending", editing.awaiting_remote()}, {"worker-unresponsive", unresponsive},
                 {"logs-panel", logs_visible}, {"timeline-drag", timeline.dragging() || timeline.handledPointer()},
                 {"no-image", !image_id}, {"unfocused", !viewport_raw.focused}, {"input-overflow", viewport_raw.overflow},
                 {"stale-generation", image_generation != session->active_generation()},
                 {"wrong-view", !matches_view(presented_info, state)},
                 {"vertex-drag", editing.active(EditGesture::vertices)}, {"translation-drag", translation.dragging() || editing.active(EditGesture::move)},
                 {"rotation-drag", rotation.active()}});
        if (editing.active(EditGesture::camera) && !navigation.dragging() && !walk.moving() && !yaw.isPressed() && !pitch.isPressed() &&
            !orbit_distance.isPressed() && !optical_zoom.isPressed()) {
            finish_camera(false);
        }
        if (viewport_tab && viewport_enabled && !navigation.handledPointer()) {
            interaction.value(interaction.value() == InteractionMode::objects
                                  ? InteractionMode::vertices
                                  : InteractionMode::objects);
            interaction_changed();
        }
        if(auto snapshot=image_camera.snapshot(image_extent)) {
            const auto part_action=viewport_interaction.update(ViewportTool::mesh_part,[&](bool allowed) {
                return viewport_interaction.mesh_part.update(blueprint_mesh_panel.gizmo(),*snapshot,image.bounds(),
                    allowed ? tool_input : std::span<const input::Event>{},
                    allowed ? tool_raw : std::span<const input::Event>{},
                    viewport_enabled && view_state.mode==ViewMode::mesh && mesh_tools.mode()!=MeshSelectMode::whole && !diagnostic.value() && !walk.active(),
                    allowed && !editing.busy() && !blueprint_mesh_panel.busy(),arrow_step);
            });
            if(part_action.began || part_action.changed || part_action.finished || part_action.cancelled) {
                auto applied=blueprint_mesh_panel.edit_part(part_action);
                if(!applied){status.text(applied.error().message);viewport_interaction.mesh_part.cancel();}
                else if(*applied)refresh_vertex();
            }
            // Non-modal shortcuts select the same capability-listed gizmos as
            // the dropdown. G/R/S/F start a gesture through the adapter below.
            if(viewport_enabled && !viewport_interaction.busy() && !walk.active() &&
               (viewport_keyboard || !ui_shortcut_capture) && editing.can_edit_scene_pose() &&
               view_state.mode==ViewMode::scene && interaction.value()==InteractionMode::objects) {
                for(const auto& event:viewport_input->unhandled()) {
                    if(!image.bounds().contains(event.position))continue;
                    if(auto mode=gizmo_shortcut(event,gizmo_mode.common());mode && !gizmo_description(*mode).gesture) {
                        gizmo_mode.value(*mode);
                        selection_overlay_dirty=true;
                    }
                }
            }
            auto action=viewport_interaction.update(ViewportTool::instances,[&](bool allowed) {
                return instance_transform.update(session->active_generation(),selected_instances.items(),rotation_pivot.value(),
                    *snapshot,image.bounds(),
                    allowed && (viewport_keyboard || !ui_shortcut_capture) ? tool_input : std::span<const input::Event>{},
                    allowed ? tool_raw : std::span<const input::Event>{},
                    allowed && viewport_enabled && !walk.active() && !regions.component_editing() &&
                    interaction.value()==InteractionMode::objects && view_state.mode==ViewMode::scene &&
                    image_time_current && !diagnostic.value() && !session->busy(),arrow_step);
            });
            if(!action) status.text(action.error().message);
            else {
                if(action->began) {
                    gizmo_mode.value(instance_transform.gizmo());
                    status.text("Move mouse / Click or Enter confirms / Escape or RMB cancels");
                }
                if(action->finished) {
                    selection_overlay_dirty=true; // Restore passive handles in this frame, with no replayed click.
                    show_timeline();
                    status.text(action->cancelled ? "Transform cancelled / original values restored" :
                        action->committed ? "Transformed selection / Ctrl+Z undoes the whole gesture" : "Transform unchanged");
                }
            }
        }
        {
            const auto snapshot=image_camera.snapshot(image_extent);
            if(auto mode=regions.take_mode()){gizmo_mode.value(*mode);selection_overlay_dirty=true;}
            viewport_interaction.selected(view_state.selected_object, gizmo_mode.value());
            viewport_interaction.update(ViewportTool::boundary, [&](bool available) {
                regions.update(editing,snapshot?*snapshot:gfx::CameraSnapshot{},image.bounds(),
                    toolbar_blocks_viewport || (!viewport_keyboard && ui_shortcut_capture) ? std::span<const input::Event>{} : tool_input,
                    toolbar_blocks_viewport ? std::span<const input::Event>{} : tool_raw,
                    snapshot && view_state.mode==ViewMode::scene && (!modal_visible()||regions.menu_open()) && !logs_visible && !playing && !diagnostic.value(),
                    available && viewport_ready && !walk.active() && view_state.paused &&
                    (!editing.busy() || editing.active(EditGesture::region)),arrow_step);
            });
            if(viewport_enabled && view_state.paused && !regions.handled() && viewport_interaction.accepts(ViewportTool::boundary))
                for(const auto& event:viewport_input->unhandled())
                    if(event.kind==input::EventKind::pointer_down&&event.button==1&&image.bounds().contains(event.position)&&
                       !event.modifiers.alt&&!event.modifiers.control)
                        {tool_panel.close();regions.open_menu(event.position,viewport_raw.logical_size,image.bounds());}
            if(auto selected=regions.take_selection()) {
                const auto mode=viewport_interaction.picked_selection(click_modifiers(viewport_input->unhandled(),image.bounds()));
                select_object(*selected,mode);
            }
            if(auto message=regions.take_message())status.text(*message);
            sync_sidebar();
        }
        const bool show_regions = regions.boundaries_visible(), show_bounds = bounds_panel.visible();
        if (view_state.show_regions != show_regions || view_state.show_world_bounds != show_bounds) {
            view_state.show_regions = show_regions; view_state.show_world_bounds = show_bounds;
            viewport_changed();
        }
        if (const auto snapshot = image_camera.snapshot(image_extent)) {
            auto action = viewport_interaction.update(ViewportTool::bounds, [&](bool available) {
                return bounds_tool.update(state.document.world_bounds, *snapshot, image.bounds(), tool_input, tool_raw,
                bounds_panel.visible() && view_state.mode == ViewMode::scene && !modal_visible() && !logs_visible && !playing,
                available && viewport_enabled && view_state.paused &&
                (!editing.busy() || editing.active(EditGesture::world_bounds)));
            });
            if (action.cancelled) finish_bounds(true);
            else {
                if (action.began) {
                    if (auto begun = editing.begin_world_bounds(); !begun) {
                        bounds_tool.cancel(); status.text(begun.error().message);
                    }
                }
                if (editing.active(EditGesture::world_bounds) && (action.changed || action.finished)) {
                    if (auto changed = editing.world_bounds(action.value); !changed) {
                        finish_bounds(true); status.text(changed.error().message);
                    } else if (action.finished) finish_bounds(false);
                }
            }
        } else { finish_bounds(true); bounds_tool.cancel(); }
        if(viewport_enabled && !walk.active() && !editing.active(EditGesture::mesh_draft) && !editing.active(EditGesture::mesh_transform) && !editing.active(EditGesture::vertices) && interaction.value()==InteractionMode::vertices &&
            view_state.mode==ViewMode::mesh && (viewport_keyboard || !ui_shortcut_capture) && image.bounds().contains(viewport_raw.pointer)) {
            for(const auto& e:viewport_input->unhandled()) if(e.kind==input::EventKind::key_down && !e.repeat &&
                !e.modifiers.control && !e.modifiers.super) {
                if(e.key==input::Key::h && !e.modifiers.shift && mesh_tools.component_mode())
                    mesh_action(e.modifiers.alt?MeshAction::reveal:MeshAction::hide);
                if(e.modifiers.alt) continue;
                if(e.key==input::Key::one) mesh_tools.mode(MeshSelectMode::vertex);
                if(e.key==input::Key::two) mesh_tools.mode(MeshSelectMode::edge);
                if(e.key==input::Key::three) mesh_tools.mode(MeshSelectMode::face);
                if(e.key==input::Key::four) mesh_tools.mode(MeshSelectMode::surface);
                if(e.key==input::Key::five) mesh_tools.mode(MeshSelectMode::whole);
                if(e.key==input::Key::five || e.key==input::Key::one || e.key==input::Key::two || e.key==input::Key::three) {
                    (void)blueprint_mesh_panel.select_part(0);
                    viewport_interaction.mesh_part.cancel();
                }
                if(e.key==input::Key::a) mesh_tools.select_all(*editable_mesh(state),e.modifiers.shift);
                if(e.key==input::Key::f && mesh_tools.component_mode()) mesh_action(MeshAction::fill);
            }
        }
        // The modal adapter owns its session transaction until confirmation.
        // Ring/arrow/scale-handle adapters must not finish or cancel that edit.
        if (!instance_transform.active() && !instance_transform.handled()) {
        if (current_schema != schemas.end() || position_schema || session->active_generation()) {
            auto snapshot = image_camera.snapshot(image_extent);
            if (snapshot) {
                // Freeze the drag's original schema while our own position
                // revisions are in flight. Outside capture, draw the gizmo at
                // the current local position; worker readiness gates only a
                // new drag, never the feedback for an existing one.
                auto local_schema = local_position_gizmo(state, session->active_generation());
                if (current_schema != schemas.end() &&
                    current_schema->second.stamp.object == view_state.selected_object) {
                    local_schema = current_schema->second;
                    local_schema.stamp.revision = state.document.revision;
                }
                auto gizmo_schema = position_schema ? *position_schema
                                                          : selection_gizmo_schema(state, local_schema);
                filter_translation_gizmos(gizmo_schema,gizmo_mode.common());
                const auto control = std::ranges::find_if(gizmo_schema.controls, [](const auto& c) {
                    return c.kind == editor::Kind::translation_gizmo;
                });
                const bool scene_position = control != gizmo_schema.controls.end() && control->key == "position";
                const bool can_begin = image_time_current && current_schema != schemas.end() &&
                                       image_revision == state.document.revision &&
                                       current_schema->second.stamp.object == view_state.selected_object &&
                                       current_schema->second.stamp.revision == state.document.revision &&
                                       updates.ready(session->active_generation(), state.document.revision);
                const auto presses = can_begin ? tool_input
                                               : std::span<const vng::input::Event>{};
                auto event = viewport_interaction.update(ViewportTool::translation, [&](bool available) {
                    // Input ownership is not visibility: e.g. a held selection
                    // click must not erase the freshly selected object's gizmo.
                    return translation.update(
                    gizmo_schema, *snapshot, image.bounds(), available ? presses : std::span<const input::Event>{},
                    available ? tool_raw : std::span<const input::Event>{},
                    (available || !translation.dragging()) && translation_mode(gizmo_mode.value()) &&
                        interaction.value() == InteractionMode::objects && viewport_enabled &&
                        editing.can_edit_scene_pose() && image_time_current &&
                        (scene_position || image_revision >= minimum_overlay_revision) &&
                        view_state.mode == ViewMode::scene && !logs_visible && !diagnostic.value() &&
                        !playing && !mode_pending && debug_link.value() && !editing.awaiting_remote() &&
                        !unresponsive && !session->busy() &&
                        gizmo_schema.stamp.generation == session->active_generation(), gizmo_mode.value()!=GizmoMode::forward,arrow_step);
                });
                // Only the built-in instance position has this compact model
                // contract. Custom effect gizmos keep their native callbacks.
                if (!scene_position && event) {
                    send_event(std::move(*event));
                    event.reset();
                }
                if (scene_position && !editing.active(EditGesture::move) && (translation.dragging() || event)) {
                    auto begun = editing.begin_move(view_state.selected_object,selected_instances.items());
                    if (!begun) {
                        status.text(begun.error().message);
                        translation.cancel();
                        event.reset();
                    } else {
                        position_schema = gizmo_schema;
                    }
                }
                auto preview = translation.preview_position();
                if (event) preview = std::get<Vec3>(event->values.front().value);
                if (editing.active(EditGesture::move) && preview) {
                    const auto moved = editing.move(*preview);
                    if (!moved) {
                        finish_position_drag(true);
                        status.text(moved.error().message);
                    }
                }
                if (editing.active(EditGesture::move) && !translation.dragging())
                    finish_position_drag(!event.has_value());
            } else
                translation.cancel();
        } else
            translation.cancel();
        if (editing.active(EditGesture::move) && !translation.dragging())
            finish_position_drag(true);
        const bool show_rotation_origin=rotation_mode(gizmo_mode.value()) && view_state.mode==ViewMode::scene &&
            interaction.value()==InteractionMode::objects && view_state.selected_object!=0;
        rotation_pivot.update(show_rotation_origin ? rotation.center(selected_instances.items()) : Vec3{},
            show_rotation_origin,viewport_interaction.busy());
        if(auto snapshot=image_camera.snapshot(image_extent);snapshot) {
            viewport_interaction.update(ViewportTool::pivot,[&](bool allowed) {
                rotation_pivot.update_tool(viewport_interaction.pivot,*snapshot,image.bounds(),
                    allowed ? tool_input : std::span<const input::Event>{},
                    allowed ? tool_raw : std::span<const input::Event>{},
                    allowed && viewport_enabled && show_rotation_origin,arrow_step);
            });
        }
        if (auto snapshot = image_camera.snapshot(image_extent); snapshot) {
            const bool can_begin = image_time_current && image_revision == state.document.revision &&
                updates.ready(session->active_generation(), state.document.revision);
            auto action = viewport_interaction.update(ViewportTool::rotation, [&](bool available) {
                return rotation.update(session->active_generation(), *snapshot,
                image.bounds(), available && can_begin ? tool_input : std::span<const input::Event>{},
                available ? tool_raw : std::span<const input::Event>{},
                (available || !rotation.active()) && !rotation_pivot.moving() && rotation_mode(gizmo_mode.value()) &&
                interaction.value() == InteractionMode::objects && viewport_enabled &&
                view_state.mode == ViewMode::scene && editing.can_edit_scene_pose() && image_time_current &&
                image_revision >= minimum_overlay_revision && !diagnostic.value() && !session->busy(),
                selected_instances.items(),gizmo_mode.value()==GizmoMode::attitude,rotation_pivot.value(),
                gizmo_mode.value()==GizmoMode::free_rotate,arrow_step);
            });
            if (!action) cancel_rotation();
            rotation_changed(std::move(action));
        } else cancel_rotation();
        if(const auto* instance=find_instance(state,view_state.selected_object)) {
            const auto snapshot=image_camera.snapshot(image_extent);
            if(snapshot) {
                const auto value=evaluate_instance(state,*instance,view_state.time);
                const bool available=image_time_current && image_revision==state.document.revision &&
                    updates.ready(session->active_generation(),state.document.revision);
                auto action=viewport_interaction.update(ViewportTool::scale, [&](bool allowed) {
                    return scaling.update({instance->id,session->active_generation(),state.document.revision},
                    value.transform.position,value.transform.scale,*snapshot,image.bounds(),
                    allowed && available ? tool_input : std::span<const input::Event>{},
                    allowed ? tool_raw : std::span<const input::Event>{},
                    (allowed || !scaling.dragging()) && gizmo_mode.value()==GizmoMode::scale && interaction.value()==InteractionMode::objects &&
                    viewport_enabled && view_state.mode==ViewMode::scene && editing.can_edit_scene_pose() &&
                    image_time_current && !diagnostic.value() &&
                    (!editing.active(EditGesture::scale) || scaling.dragging()) &&
                    std::visit([](const auto& settings) { return settings.visible; },value.settings));
                });
                if(action.cancelled) finish_scale(true);
                else if(action.began || action.changed || action.finished)
                    edit_scale(action.value,action.finished,true);
            } else { finish_scale(true); scaling.cancel(); }
        } else { finish_scale(true); scaling.cancel(); }
        }
        if(auto snapshot=image_camera.snapshot(image_extent);snapshot) {
            auto changed=viewport_interaction.update(ViewportTool::components,[&](bool allowed) {
                return viewport_interaction.mesh.update(mesh_tools,*snapshot,image.bounds(),
                    allowed && (viewport_keyboard || !ui_shortcut_capture) ? tool_input : std::span<const input::Event>{},
                    allowed ? tool_raw : std::span<const input::Event>{},
                    allowed && viewport_enabled && !walk.active() && interaction.value()==InteractionMode::vertices &&
                    view_state.mode==ViewMode::mesh && view_state.paused && !editing.awaiting_remote() &&
                    (mesh_tools.mode()==MeshSelectMode::whole || !blueprint_mesh_panel.gizmo()),arrow_step);
            });
            if(!changed)status.text(changed.error().message);else if(*changed)refresh_vertex();
        }
        for (std::size_t event_index = 0; event_index < viewport_raw.events.size(); ++event_index) {
            const auto& event = viewport_raw.events[event_index];
            if(box_tool.active()) {
                if(auto result=box_tool.update(event)) {
                    const ui::Rect normalized{(result->rect.x-box_viewport.x)/box_viewport.width,
                        (result->rect.y-box_viewport.y)/box_viewport.height,
                        result->rect.width/box_viewport.width,result->rect.height/box_viewport.height};
                    if(interaction.value()==InteractionMode::objects) {
                        std::vector<u32> hits;
                        for(const auto& point:viewport_interaction.instance_projection.get(state,box_extent,box_camera))
                            if(point.point.x>=normalized.x && point.point.x<=normalized.x+normalized.width &&
                               point.point.y>=normalized.y && point.point.y<=normalized.y+normalized.height) hits.push_back(point.object);
                        selected_instances=box_instances;
                        selected_instances.select(hits,result->mode);
                        commit_selection();
                    } else if(editable_mesh(state)) {
                        const auto hits=mesh_tools.box(state,normalized,box_extent,box_camera);
                        mesh_tools.select(box_mesh,editor::SelectionMode::replace);
                        mesh_tools.select(hits,result->mode);
                        const auto vertices=mesh_tools.vertices(*editable_mesh(state));
                        if(!vertices.empty()) view_state.selected_vertex=vertices.back();
                        refresh_vertex();viewport_changed();
                        status.text(std::to_string(mesh_tools.selected().size())+" mesh elements selected");
                    }
                }
                continue;
            }
            if (!viewport_interaction.accepts(ViewportTool::selection))
                break;
            const bool available_press =
                event.kind == input::EventKind::pointer_down && event.button == 0 &&
                image.bounds().contains(event.position) && viewport_enabled &&
                image_time_current && image_revision == state.document.revision && !diagnostic.value() &&
                std::ranges::any_of(viewport_input->unhandled(), [&](const auto& available) {
                    return available.kind == event.kind && available.button == event.button &&
                           available.position == event.position;
                });
            if (available_press && view_state.mode != ViewMode::mesh &&
                interaction.value() == InteractionMode::objects) {
                const auto b = image.bounds();
                box_instances=selected_instances;
                box_camera=image_camera;box_extent=image_extent;box_viewport=b;
                box_tool.begin(event.position,b,event.modifiers);
                viewport_interaction.selection_handled();
                select_object(pick_object(state,
                                          {(event.position.x - b.x) / b.width,
                                           (event.position.y - b.y) / b.height},
                                          image_extent, &image_camera)
                                  .value_or(0),click_selection(event.modifiers));
                continue;
            }
            if(available_press && view_state.mode==ViewMode::mesh && mesh_tools.mode()==MeshSelectMode::surface &&
               !blueprint_mesh_panel.busy()) {
                const auto b=image.bounds();
                if(auto snapshot=image_camera.snapshot(image_extent)) {
                    const auto part=blueprint_mesh_panel.pick_part(
                        {(event.position.x-b.x)/b.width,(event.position.y-b.y)/b.height},*snapshot);
                    (void)blueprint_mesh_panel.select_part(part);
                    // Present the selected handle in the clicking frame, without
                    // replaying the click as the start of a drag.
                    (void)viewport_interaction.mesh_part.update(blueprint_mesh_panel.gizmo(),*snapshot,b,{},{},true,true);
                    status.text(part?"Blueprint part selected / drag its surface handle":"Whole blueprint / 5: transform mesh");
                }
                viewport_interaction.selection_handled();
                continue;
            }
            if (event.kind == input::EventKind::pointer_down && event.button == 1 &&
                interaction.value()==InteractionMode::vertices && mesh_tools.component_mode() && viewport_enabled && !editing.active(EditGesture::vertices) &&
                image.bounds().contains(event.position) && !navigation.handledPointer() &&
                !event.modifiers.alt && !event.modifiers.control) {
                tool_panel.close();mesh_tools.open(event.position,viewport_raw.logical_size,image.bounds());
                continue;
            }
            if (event.kind == input::EventKind::pointer_down && event.button == 0 &&
                interaction.value() == InteractionMode::vertices && viewport_enabled &&
                mesh_tools.component_mode() &&
                view_state.paused && image.bounds().contains(event.position) && !playing &&
                !mode_pending && !editing.awaiting_remote() && !logs_visible && !diagnostic.value() &&
                editable_mesh(state) && image_time_current && image_revision == state.document.revision &&
                std::ranges::any_of(viewport_input->unhandled(), [&](const auto& available) {
                    return available.kind == event.kind && available.button == event.button &&
                           available.position == event.position;
                })) {
                const auto b = image.bounds();
                auto id = mesh_tools.pick(
                    state,
                    {(event.position.x - b.x) / b.width, (event.position.y - b.y) / b.height},
                    image_extent, image_camera, b);
                if (id) {
                    mesh_tools.select(*id,event.modifiers.shift || event.modifiers.control);
                    const auto chosen=mesh_tools.vertices(*editable_mesh(state));
                    if(chosen.empty()) { refresh_vertex(); continue; }
                    view_state.selected_vertex = chosen.back();
                    viewport_changed();
                    minimum_inspector_sequence = view_state.sequence;
                    refresh_vertex();
                    viewport_interaction.selection_handled();
                } else {
                    box_mesh.assign(mesh_tools.selected().begin(),mesh_tools.selected().end());
                    box_camera=image_camera;box_extent=image_extent;box_viewport=b;
                    box_tool.begin(event.position,b,event.modifiers);
                    viewport_interaction.selection_handled();
                    if(!event.modifiers.shift && !event.modifiers.control) mesh_tools.select_all(*editable_mesh(state),true);
                }
            }
        }
        // Picking runs after gizmo hit-testing so handles win overlapping
        // clicks. Refresh its passive geometry now, in that same input frame,
        // without waiting for a new worker schema or replaying the click.
        // A pasted instance already has a local transform: its passive handle
        // must not wait for the worker to accept/render the new document.
        if (selection_overlay_dirty) {
            selection_overlay_dirty = false;
            if (auto snapshot = image_camera.snapshot(image_extent); snapshot) {
                auto schema=local_position_gizmo(state,session->active_generation());
                filter_translation_gizmos(schema,gizmo_mode.common());
                (void)translation.update(schema,
                    *snapshot, image.bounds(), {}, {},
                    translation_mode(gizmo_mode.value()) &&
                    viewport_enabled && interaction.value() == InteractionMode::objects &&
                    editing.can_edit_scene_pose() && image_time_current &&
                    view_state.mode == ViewMode::scene && !diagnostic.value(),gizmo_mode.value()!=GizmoMode::forward);
                rotation_changed(rotation.update(session->active_generation(), *snapshot,
                    image.bounds(), {}, {}, !rotation_pivot.moving() && rotation_mode(gizmo_mode.value()) &&
                    viewport_enabled && interaction.value() == InteractionMode::objects &&
                    editing.can_edit_scene_pose() && view_state.mode == ViewMode::scene && !diagnostic.value() &&
                    image_revision >= minimum_overlay_revision,selected_instances.items(),gizmo_mode.value()==GizmoMode::attitude,rotation_pivot.value()));
                if (const auto* instance=find_instance(state,view_state.selected_object)) {
                    const auto value=evaluate_instance(state,*instance,view_state.time);
                    (void)scaling.update({instance->id,session->active_generation(),state.document.revision},
                        value.transform.position,value.transform.scale,*snapshot,image.bounds(),{}, {},
                        gizmo_mode.value()==GizmoMode::scale && viewport_enabled &&
                        interaction.value()==InteractionMode::objects && view_state.mode==ViewMode::scene &&
                        editing.can_edit_scene_pose() && image_time_current && !diagnostic.value() &&
                        std::visit([](const auto& settings) { return settings.visible; },value.settings));
                } else scaling.cancel();
            }
        }
        if (delete_pressed(shortcut_events, shortcut_raw.events,
                           shortcut_raw.focused && !shortcut_raw.overflow && !dialog_was_open && !modal_visible() &&
                           !save_dialog.visible() && !editing.awaiting_remote() && !playing && !mode_pending &&
                           !viewport_interaction.busy() && !timeline.dragging() && !logs_visible)) {
            content::Result<bool> result{false};
            bool remove{};
            std::string removed;
            if (delete_target == DeleteTarget::keyframe) {
                if (!timeline.selected_keyframes().empty()) {
                    result = editing.erase_keyframes(timeline.selected_keyframes());
                    const auto count = std::ranges::count_if(timeline.selected_keyframes(), [](f32 time){ return time != 0; });
                    removed = std::to_string(count)+" keyframe(s)";
                    remove = count != 0;
                    if (!remove) status.text("Time zero is the permanent initial keyframe");
                }
            } else if (view_state.mode != ViewMode::mesh && view_state.selected_object &&
                       interaction.value() == InteractionMode::objects) {
                removed = std::to_string(selected_instances.size())+" instance(s) (blueprints kept)";
                result = editing.erase_instances(selected_instances.items());
                remove = true;
            } else if (interaction.value() == InteractionMode::vertices)
                status.text("Switch to Objects mode to delete an instance; vertex deletion is not supported.");
            if (!result) status.text(result.error().message);
            else if (remove) {
                if (delete_target == DeleteTarget::keyframe) timeline.clear_selection();
                editing.select_keyframe(timeline.selected_keyframe());
                inspector.clear(); translation.cancel();
                refresh();
                status.text("Deleted " + removed + " / Undo restores it");
            }
        }
        if(publish_mesh.clicked() && !editing.awaiting_remote() && !viewport_gesture && !playing && !modal_visible() && view_state.mode==ViewMode::mesh) {
            auto applied=editing.apply_mesh(view_state.inspected_mesh);
            if(!applied) status.text(applied.error().message);
            else if(*applied) {
                refresh_vertex();
                status.text("Mesh applied to scene instances / not saved on disk yet");
            }
        }
        broadcast();
        const auto saved = [&] {
            path.value(editing.path()->string());
            status.text("Saved scene and mesh drafts: " + editing.path()->string() + " / unapplied drafts remain separate");
            if (debug_link.value() && image_id)
                frame_label.text("Embedded preview / gen " + std::to_string(image_generation) +
                                 " / update " + std::to_string(image_revision) + " / saved");
        };
        const bool save_action =
            save.clicked() || save_mesh_draft.clicked() || save_as.clicked() ||
            file_shortcut == FileShortcut::save || file_shortcut == FileShortcut::save_as;
        if (dialog_was_open) {
            if (auto request = save_dialog.poll(raw.events)) {
                auto result = editing.save_as(request->path, request->replace_existing);
                if (!result) {
                    save_dialog.error(result.error().message);
                    status.text(result.error().message);
                } else {
                    save_dialog.close();
                    saved();
                }
            }
        } else if (save_action && !modal_visible()) {
            if (editing.awaiting_remote() || mode_pending || viewport_interaction.busy() || timeline.dragging()) {
                status.text("Finish the current edit before saving.");
            } else if (save_as.clicked() || file_shortcut == FileShortcut::save_as ||
                       !editing.path()) {
                cancel_viewport();
                timeline.cancel();
                save_dialog.open(std::filesystem::path{path.getText()});
            } else {
                auto result = editing.save();
                if (result)
                    saved();
                else
                    status.text(result.error().message);
            }
        }
        if (!dialog_was_open && !modal_visible() && !save_action && export_asset.clicked() && (exporting_effect() || editable_mesh(state))) {
            auto destination = std::filesystem::path{path.getText()};
            const bool effect=exporting_effect();
            destination.replace_extension(effect?".veffect":".vmesh");
            auto data = [&]() -> content::Result<std::string> {
                if(!effect) {
                    auto baked=bake_mesh_placements(state);
                    if(!baked)return std::unexpected(baked.error());
                    return content::vmesh::write_vmesh(editable_mesh(*baked)->document());
                }
                auto preset=effect_preset(state,view_state.selected_object);
                if(!preset)return std::unexpected(preset.error());
                return encode_effect_preset(*preset);
            }();
            if (!data)
                status.text(data.error().message);
            else {
                auto result = save_new(destination, *data);
                status.text(result ? std::string(effect?"Exported new effect preset: ":"Exported new mesh: ") + destination.string()
                                   : result.error().message);
            }
        }
        // Open scene browses for a .vscene. A bottom-field path that exists points
        // the browser there; otherwise it starts beside the current scene, or in
        // the authoring asset library for an untitled scene.
        if (!dialog_was_open && !modal_visible() && !save_action && !export_asset.clicked() &&
            (load.clicked() || file_shortcut == FileShortcut::open) && !editing.awaiting_remote()) {
            cancel_viewport();
            timeline.cancel();
            std::error_code ignored;
            const std::filesystem::path typed{path.getText()};
            open_dialog.open(!typed.empty() && std::filesystem::exists(typed, ignored) ? typed
                             : editing.path() ? *editing.path() : import_directory);
            if (editing.dirty())
                open_dialog.error("The current scene has unsaved changes. Opening another scene discards them.");
        }
        if (open_was_open) {
            if (const auto file = open_dialog.poll(raw.events)) {
                auto loaded = editing.load(*file);
                if (!loaded)
                    open_dialog.error(loaded.error().message);
                else {
                    open_dialog.close();
                    camera_visit.reset();
                    completed_frames.clear();
                    selected_instances.clear();
                    mesh_tools.reset();
                    timeline.reset();
                    revealed_instance.reset();
                    scene_panel.scroll(0); scene_list.scroll(0); region_list.scroll(0); blueprint_list.scroll(0);
                    for(auto section:sidebar_sections) section.scroll(0);
                    instance_flyout.body().scroll(0); blueprint_flyout.body().scroll(0);
                    minimum_frame_revision = minimum_overlay_revision = state.document.revision;
                    path.value(editing.path()->string());
                    status.text("Loaded scene: " + editing.path()->string());
                    refresh(true);
                    broadcast();
                }
            }
        }
        // Input handlers coalesce the latest absolute view throughout the tick.
        // Deliver it before UI drawing/vsync, not at next tick's receive/poll.
        update_pose_controls();
        if (auto sent = session->flush_requests(); !sent) status.text(sent.error().message);
        if (!debug_link.value() && raw.events.empty() &&
            now - last_ui_present < std::chrono::milliseconds(100)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            continue;
        }
        const auto controls_extent = window.framebuffer_extent();
        if (controls_extent.empty() && !viewport_window.opened())
            continue;
        const auto extent=controls_extent.empty() ? raw.framebuffer : controls_extent;
        // The window manager can finish maximizing while this frame is being
        // assembled (especially while a file browser is listing a directory).
        // Re-layout on the next input snapshot instead of submitting stale UI
        // coordinates to a differently sized render target.
        if (extent != raw.framebuffer)
            continue;
        if (auto r = display->resize(device, extent); !r)
            return fail(r.error().message);
        auto frame = render::begin_frame(device, display->target(),
                                         {.extent = extent,
                                          .color_encoding = render::ColorEncoding::srgb,
                                          .clear_color = std::array<f32, 4>{.007F, .01F, .019F, 1},
                                          .clear_depth = {}});
        if (!frame)
            return fail(frame.error().message);
        if (modal_visible()) { main_bar.close(); scene_bar.close(); close_list_flyouts(); timeline.close_menu(); }
        const bool preview_ready = image_id && image_revision == state.document.revision &&
                                   matches_view(presented_info,state) && presented_info.settled &&
                                   presented_info.view_sequence == view_state.sequence &&
                                   (playing || image_extent == desired_preview_extent);
        const auto preview_activity = !debug_link.value() ? PreviewFps::Activity::frozen
            : !image_id ? PreviewFps::Activity::waiting
            : preview_ready && view_state.paused && !playing ? PreviewFps::Activity::idle
            : PreviewFps::Activity::updating;
        preview_fps.update(std::chrono::steady_clock::now(), preview_activity);
        auto list = screen.draw_list();
        if (!list)
            return fail(list.error().message);
        auto viewport_list=viewport_screen.draw_list();
        if (!viewport_list) return fail(viewport_list.error().message);
        tool_panel.validate();
        auto* active_tool_options=instance_transform.tool_options();
        if(!active_tool_options)active_tool_options=viewport_interaction.mesh.tool_options();
        if(!active_tool_options)active_tool_options=regions.tool_options();
        const bool captured_gizmo=viewport_interaction.transforming();
        const bool visible_gizmo=viewport_enabled && !diagnostic.value() &&
            (view_state.mode==ViewMode::mesh ? viewport_interaction.mesh.visible()||viewport_interaction.mesh_part.visible() :
             translation.visible()||rotation.visible()||scaling.visible()||regions.tool().gizmo_visible()||
             viewport_interaction.pivot.visible()||bounds_tool.handle(0)||bounds_tool.handle(1));
        const bool scale_options=instance_transform.active() ? instance_transform.gizmo()==GizmoMode::scale :
            view_state.mode==ViewMode::mesh ? mesh_tools.transform_mode()==GizmoMode::scale :
            regions.selected() ? regions.tool().transform_mode()==GizmoMode::scale : gizmo_mode.value()==GizmoMode::scale;
        const bool new_gizmo_options=viewport_interaction.gizmo_input.show(captured_gizmo||visible_gizmo,active_tool_options,captured_gizmo,scale_options);
        if(viewport_interaction.gizmo_input.options_available() &&
           (captured_gizmo || !tool_panel.opened() || tool_panel.showing(viewport_interaction.gizmo_input)))
            tool_panel.show(viewport_interaction.gizmo_input,new_gizmo_options);
        else if(active_tool_options)tool_panel.show(*active_tool_options);
        tool_panel.layout(image.bounds());
        mesh_navigation.layout(image.bounds(),view_state.mode==ViewMode::mesh,mesh_tools.mode()==MeshSelectMode::whole,
            show_fps.value(),!viewport_interaction.busy()&&!editing.busy()&&!walk.active()&&!playing&&!modal_visible());
        auto popup_list=viewport_popups.draw_list();
        if (!popup_list) return fail(popup_list.error().message);
        const bool viewport_uncovered=(!camera_open && !main_bar.opened() && !scene_bar.opened() && !timeline.menu_open()) || viewport_window.opened();
        const bool blocking_dialog=save_dialog.visible() || settings_panel.visible() || import_dialog.visible() || open_dialog.visible();
        const bool show_mesh_overlay = viewport_uncovered && !blocking_dialog && interaction.value() == InteractionMode::vertices &&
            mesh_tools.component_mode() &&
            !playing && !mode_pending && !logs_visible && !diagnostic.value() &&
            image_time_current && matches_view(presented_info,state) && image_revision >= minimum_overlay_revision;
        if (viewport_uncovered && (!modal_visible() || regions.menu_open())) {
            // Compose annotation geometry immediately above its viewport image,
            // below inspector widgets and popups, including its own RMB menu.
            ui::DrawList annotations;
            regions.append(annotations,*font);
            const auto viewport_draw=std::ranges::find_if(viewport_list->commands,[&](const auto& command) {
                const auto* draw=std::get_if<ui::ImageDraw>(&command);
                return draw && draw->pixels==preview_pixels;
            });
            if(viewport_draw!=viewport_list->commands.end())
                viewport_list->commands.insert(std::next(viewport_draw),std::make_move_iterator(annotations.commands.begin()),
                    std::make_move_iterator(annotations.commands.end()));
        }
        if (viewport_uncovered && !modal_visible()) {
            if(view_state.mode==ViewMode::scene && selected_instances.size()>1 && viewport_enabled) {
                for(const auto& p:viewport_interaction.instance_projection.get(state,image_extent,image_camera)) {
                    if(!selected_instances.contains(p.object) || p.object==view_state.selected_object) continue;
                    const auto b=image.bounds();
                    const Vec2 point{b.x+p.point.x*b.width,b.y+p.point.y*b.height};
                    viewport_list->commands.emplace_back(ui::BoxDraw{{point.x-5,point.y-5,10,10},b,
                        {1,.7F,.12F,1},{.04F,.04F,.04F,1},4,1});
                }
            }
            if (!instance_transform.active()) {
            translation.append(*viewport_list, *font);
            bounds_tool.append(*viewport_list, *font, false);
            rotation.append(*viewport_list,*font);
            viewport_interaction.pivot.append(*viewport_list,*font);
            scaling.append(*viewport_list);
            }
            instance_transform.append(*viewport_list,*font);
            viewport_interaction.mesh.append(*viewport_list,*font);
            viewport_interaction.mesh_part.append(*viewport_list,*font,
                view_state.mode==ViewMode::mesh && blueprint_mesh_panel.pending(image_revision),
                std::chrono::duration<double>(now.time_since_epoch()).count());
            box_tool.append(*viewport_list);
        }
        // The range menu covers the inspector, not the ruler. Keep destination
        // markers visible while choosing a range; insertion retains draw order.
        if (!modal_visible() && !camera_open && !main_bar.opened() && !scene_bar.opened() &&
            !instance_flyout.opened() && !blueprint_flyout.opened()) timeline.append(*list);
        if (show_fps.value()) preview_fps.append(*viewport_list, *font, image.bounds());
        if (!viewport_window.opened()) {
            list->commands.insert(list->commands.begin(),viewport_list->commands.begin(),viewport_list->commands.end());
        }
        if (auto r = renderer->render(*frame, *list); !r)
            return fail(r.error().message);
        if (show_mesh_overlay && !viewport_window.opened())
            if (auto r = overlay->render(*frame,state,mesh_tools,image.bounds(),list->logical_size,image_extent,image_camera); !r)
                return fail(r.error().message);
        if(!viewport_window.opened() && !popup_list->commands.empty())
            if(auto r=renderer->render(*frame,*popup_list);!r)return fail(r.error().message);
        if (auto r = frame->end(); !r)
            return fail(r.error().message);
        if (!controls_extent.empty())
            if (auto r = display->copy_to_window(device); !r) return fail(r.error().message);
        if (viewport_window.opened()) {
            auto presented = viewport_window.present(*viewport_list,*popup_list,options.automation ? window::VSync::off : settings.vsync,
                state,mesh_tools,image.bounds(),image_extent,image_camera,show_mesh_overlay);
            if (!presented) return fail(presented.error());
            if (*presented) preview_fps.presented(presented_info, std::chrono::steady_clock::now());
        }
        if (options.automation) {
            auto& composed=viewport_window.opened()?*viewport_list:*list;
            composed.commands.insert(composed.commands.end(),popup_list->commands.begin(),popup_list->commands.end());
            const auto active = session->active_generation();
            const auto schema = schemas.find(active);
            options.automation->observe({
                .state = state, .screen = screen, .draw_list = *list,
                .frame = ++ui_frame, .worker_generation = active, .image_revision = image_revision,
                .extent = extent, .preview_extent = image_extent, .viewport = image.bounds(),
                .preview_ready = preview_ready, .dirty = editing.dirty(),
                .gizmo_visible = (instance_transform.active() ? instance_transform.visible() :
                    (translation.visible() || rotation.visible() || scaling.visible() || viewport_interaction.mesh_part.visible())) && viewport_uncovered && !modal_visible(),
                .dragging = instance_transform.active() || translation.dragging() || rotation.dragging() || editing.active(EditGesture::scale) || regions.dragging() ||
                    viewport_interaction.mesh.active() || viewport_interaction.pivot.dragging() || viewport_interaction.mesh_part.dragging(),
                .inspector_ready = updates.ready(active, state.document.revision) && schema != schemas.end() &&
                    schema->second.stamp.revision == state.document.revision &&
                    schema->second.stamp.object == view_state.selected_object &&
                    schema->second.stamp.context >= minimum_inspector_sequence,
                .mesh_overlay_visible = show_mesh_overlay,
                .rotation_gizmo = instance_transform.active() ? instance_transform.gesture().rotation_gizmo() :
                    rotation_mode(gizmo_mode.value()) ? &rotation.tool() : nullptr,
                .scale_gizmo = instance_transform.active() ? instance_transform.gesture().scale_gizmo() :
                    gizmo_mode.value() == GizmoMode::scale ? &scaling : nullptr,
                .translation_gizmo = instance_transform.active() ? instance_transform.gesture().translation_gizmo() :
                    translation_mode(gizmo_mode.value()) ? &translation : nullptr,
                .gizmo_axis = instance_transform.active() ? instance_transform.gesture().axis() : -1,
                .bounds_gizmo = &bounds_tool,
                .region_gizmo = &regions.tool(),
                .mesh_tools = &mesh_tools,
                .playing = playing, .debug_link = debug_link.value(), .modal = modal_visible(),
                .selected_keyframe = timeline.selected_keyframe(),
                .selected_instances = selected_instances.items(),
                .selected_keyframes = timeline.selected_keyframes(),
                .box_selecting = box_tool.dragging(),
                .status = status.text(), .worker_logs = session->logs(),
                .preview_pixels = preview_pixels.get(),
                .capture = [&] { return example::capture_screenshot(device, extent); },
                .presented = &presented_info, .timings = &timings,
                .viewport_detached = viewport_window.opened(),
                .viewport_screen = &viewport_screen, .viewport_draw_list = &*viewport_list,
                .viewport_popups = &viewport_popups,
                .capture_viewport = [&]() -> resources::Result<gfx::ImageData> {
                    return viewport_window.opened() ? viewport_window.capture() : example::capture_screenshot(device,extent);
                },
                .rotation_origin_gizmo = rotation_pivot.moving() ? &viewport_interaction.pivot : nullptr,
                .rotation_origin = rotation_pivot.value().point,
                .mesh_part_gizmo = &viewport_interaction.mesh_part,
                .blueprint_pending = blueprint_mesh_panel.pending(image_revision),
            });
        }
        if (!mode_pending && preview_ready && options.screenshot && !captured) {
            auto r = example::save_screenshot(device, extent, *options.screenshot);
            if (!r)
                return fail(r.error().message);
            captured = true;
        }
        const auto submit = monotonic_ns();
        if (!controls_extent.empty()) {
            if (auto r = window.present(); !r) return fail(r.error().message);
            if (!viewport_window.opened())
                preview_fps.presented(presented_info, std::chrono::steady_clock::now());
        }
        timings.presented(image_generation,image_revision,presented_info.view_sequence,image_id,
                          presented_info.interaction,presented_info.settled,submit,monotonic_ns());
        scroll_trace.presented(presented_info);
        last_ui_present = now;
        if (options.automation && options.automation->finished()) break;
        if (options.once && !mode_pending && preview_ready)
            break;
        if (options.once && now - started > std::chrono::seconds(120))
            return fail("Preview startup timed out: " + std::string(session->logs()));
    }
    return 0;
}
} // namespace editor_example
