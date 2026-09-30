#include "app.hpp"
#include "mesh_tools_ui.hpp"
#include "workspace_ui.hpp"
#include "tool_panel.hpp"
#include "mesh_overlay.hpp"
#include "document_patch.hpp"
#include "project.hpp"
#include "effect_presets.hpp"
#include "blueprint_mesh_panel.hpp"
#include "translation_tool.hpp"
#include "instance_rotation_gizmo.hpp"
#include "editing_session.hpp"
#include "instance_controls.hpp"
#include "scale_tool.hpp"
#include "world_bounds_tool.hpp"
#include "region_editor.hpp"
#include "viewport_tools_ui.hpp"
#include "viewport_input.hpp"
#include "edit_clipboard.hpp"
#include "edit_shortcuts.hpp"
#include "preview_logic.hpp"
#include "viewport_session.hpp"
#include "presented_view.hpp"
#include "timing_panel.hpp"
#include "preview_fps.hpp"
#include "camera_pointer_logic.hpp"
#include "scroll_trace.hpp"
#include "selection.hpp"
#include "vertex_drag.hpp"
#include "viewport_shortcuts.hpp"
#include "file_shortcuts.hpp"
#include "scene_file.hpp"
#include "save_dialog.hpp"
#include "import_dialog.hpp"
#include "open_scene_dialog.hpp"
#include "mesh_import_view.hpp"
#include "animation.hpp"
#include "preview_values.hpp"
#include "preview_mailbox.hpp"
#include "timeline_panel.hpp"
#include "editor_layout.hpp"
#include "preview_viewport.hpp"
#include "settings_panel.hpp"
#include "viewport_window.hpp"
#include "ui_scale.hpp"
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
#include <vng/ui_opengl/ui_renderer.hpp>
#include <chrono>
#include <iostream>
#include <map>
#include <set>
#include <thread>

namespace editor_example {
using namespace vng;
namespace {
int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
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
    EditingWorkspaceUI workspace{std::move(initial), std::move(scene_file)};
    auto& editing = workspace;
    if (auto configured = editing.timeline_track_limit(settings.timeline_track_limit); !configured)
        return fail(configured.error().message);
    if (auto configured = editing.instance_limit(settings.instance_limit); !configured)
        return fail(configured.error().message);
    const auto& state = editing.state();
    auto& view_state = editing.viewport();
    auto font = text::Font::load(VNG_EXAMPLE_FONT_PATH);
    if (!font)
        return fail(font.error().message);
    const auto& selected_instances=workspace.selected_instances();
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
    input::EventSequence input_sequence;
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
    auto session = PreviewLogic::create(
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
    workspace.create_panels(screen);
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
    auto bounds_controls=scene_bar.item(396);
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
    auto components_tab = diagnostic_tabs.button("Component diagnostics").width(216);
    auto log_text = log_panel.text_area().height(302);
    auto component_text = log_panel.text_area().height(302).visible(false);
    bool components_visible{};
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
    workspace.initialize_panels(screen,viewport_popups,keyframe_actions,region_controls,bounds_controls);
    const auto& mesh_tools = workspace.mesh_components();
    const auto mesh_input = [&](MeshInput input = {}) {
        return dispatch(workspace, workspace_situation(view_state), WorkspaceContext{input}).mesh;
    };
    const auto list_input = [&](SceneListsContext context) {
        return dispatch(workspace, workspace_situation(view_state), WorkspaceContext{.lists=context}).lists;
    };
    const auto timeline_input = [&](TimelineContext context) {
        return dispatch(workspace, workspace_situation(view_state), WorkspaceContext{.timeline=context}).timeline;
    };
    const auto tool_input_request = [&](ToolPanelInput input) {
        return dispatch(workspace,workspace_situation(view_state),WorkspaceContext{.tools=input}).mesh;
    };
    auto& blueprint_mesh_panel=workspace.blueprint_panel();
    auto& viewport_interaction=workspace.interaction();
    auto& camera_gizmo=viewport_interaction.camera_gizmo();
    auto& gizmo_mode=workspace.gizmo_selector();
    auto& rotation_pivot=workspace.rotation_pivot();
    auto& regions = viewport_interaction.regions;
    const auto& tool_panel=workspace.tools();
    workspace.initialize_camera(screen.column(),viewport_popups.column(),settings);
    const auto& camera_ui=workspace.camera_ui();
    const auto inspecting=[&]{return camera_ui.inspecting();};
    const auto close_list_flyouts = [&] { list_input({.input={.close=true}}); };
    const auto& timeline=workspace.timeline_view();
    auto show_timeline = [&] {
        timeline_input({.input={.synchronize=true}});
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
    auto& delivery = *session;
    editor::InteractionTimings timings;
    PreviewFps preview_fps;
    ScrollTrace scroll_trace{ScrollTrace::requested() ? &std::cerr : nullptr};
    if (scroll_trace.enabled()) std::cerr << "[scroll/ui] trace enabled; input -> target -> queued -> presented\n";
    editor::InteractionOrigin document_origin, view_origin;
    u64 traced_revision{}, traced_view_sequence{};
    editor::preview::FrameInfo presented_info;
    gfx::Camera image_camera;
    auto& translation = viewport_interaction.translation;
    auto& box_tool = viewport_interaction.selection_box;
    auto& rotation = viewport_interaction.rotation;
    auto& scaling = viewport_interaction.scale;
    auto& instance_transform = viewport_interaction.instances;
    auto& bounds_tool = viewport_interaction.bounds;
    std::shared_ptr<const gfx::ImageData> preview_pixels;
    u64 ui_frame{};
    std::string automation_clipboard;
    const auto& navigation = camera_gizmo;
    const auto& walk = camera_gizmo.walking();
    const auto& candidates=session->candidates();
    const auto& schemas=session->schemas();
    u64 pending_generation{}, image_generation{}, image_id{}, image_revision{},
        minimum_frame_revision = state.document.revision, minimum_overlay_revision = state.document.revision;
    bool captured{}, playing = options.play, wanted_playing = options.play, mode_pending{};
    std::string info;
    Extent2D image_extent = default_preview_extent, desired_preview_extent = default_preview_extent;
    auto started = std::chrono::steady_clock::now(), previous = started, pending_started = started,
         mode_started = started, last_ui_present = started, next_ui_frame = started;
    // Previous-frame UI ownership: a field, popup or capture that holds the
    // keyboard when the next input frame arrives. popup_* is the popup layer.
    bool unresponsive{}, stop_refresh{}, ui_keyboard_capture{}, ui_shortcut_capture{}, popup_shortcut_capture{};
    const auto component_report = [&] {
        return DebugReport{.name="editor", .role="application coordination",
            .situation=playing ? "IndependentPlay" : "Authoring",
            .owned={{"detached viewport",debug_bool(viewport_window.opened())},
                {"image generation",std::to_string(image_generation)},
                {"image document revision",std::to_string(image_revision)},
                {"requested view sequence",std::to_string(view_state.sequence)},
                {"presented view sequence",std::to_string(presented_info.view_sequence)},
                {"pending playback transition",debug_bool(mode_pending)}},
            .children={workspace.debug_report(),delivery.debug_report(),main_bar.debug_report(),scene_bar.debug_report(),
                save_dialog.debug_report(),open_dialog.debug_report(),import_dialog.debug_report()}};
    };
    auto refresh_selection = [&](bool synchronize_document = true) {
        workspace.refresh_selection(synchronize_document);
        if(synchronize_document)view.show(state);
    };
    auto refresh_vertex = [&] { workspace.refresh_vertex(); };
    auto refresh = [&](bool reset_camera = false) {
        refresh_selection();
        refresh_vertex();
        pause.text(view_state.paused ? "Play (in editor)" : "Pause (in editor)");
        workspace.sync_camera_controls(reset_camera);
    };
    const auto place = [](auto& widget, ui::Rect bounds) {
        widget.position({bounds.x, bounds.y}).width(bounds.width).height(bounds.height);
    };
    const auto sync_sidebar = [&] { workspace.sync_sidebar(); };
    const auto layout = [&](const input::Frame& input) {
        const auto size = input.logical_size;
        const bool vertices = workspace.interaction_mode() == InteractionMode::vertices;
        const auto aspect = playing ? static_cast<f32>(image_extent.width) / static_cast<f32>(image_extent.height)
                                    : 0.F;
        geometry = viewport_window.opened() ? editor_controls_layout(size, vertices) : editor_layout(size, vertices, aspect);
        workspace.layout_panels(geometry,size);
        place(toolbar, geometry.toolbar);
        main_bar.layout(size);
        // Keep commit actions pinned right, independently of view-tool overflow.
        scene_controls.width(std::max(82.F, secondary.bounds().width - keyframe_actions.bounds().width - 6.F));
        scene_bar.layout(size);
        workspace.camera_menu_layout(show_camera.bounds(),size);
        list_input({.presentation=SceneListPresentation{size,scene_bar.popup_anchor(show_instances),scene_bar.popup_anchor(show_blueprints),
            show_instances.bounds(),show_blueprints.bounds()}});
        place(caption, viewport_window.opened() ? ui::Rect{0,0,viewport_raw.logical_size.x,54} : geometry.caption);
        frame_label.width(std::max(120.F, (viewport_window.opened() ? viewport_raw.logical_size.x : geometry.caption.width) - 458));
        place(files, geometry.files);
        files.padding(4);
        path.width(std::max(100.0F, geometry.files.width - 406));
        place(status_host, geometry.status);
        place(log_panel, geometry.logs);
        log_text.height(std::max(60.0F, geometry.logs.height - 102));
        component_text.height(std::max(60.0F, geometry.logs.height - 102));
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
        if(auto error=delivery.resize(generation,desired_preview_extent);!error.empty()) status.text(error);
    };
    auto consume_edits = [&] {
        if (auto notice = editing.take_changes()) {
            document_origin = timings.interaction();
            delivery.changed(notice->changes);
            if(notice->changes.full)view.show(state);
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
        const auto reply=dispatch(delivery,PreviewLogic::LiveLink{},DeliveryContext{
            generation,state,document_origin,view_origin,true});
        if(!reply.error.empty()) status.text(reply.error);
    };
    auto flush_view = [&](u64 generation) {
        // Workspace-owned selection/view commands can advance the sequence
        // without calling a host callback. Attribute their delivery here.
        if(traced_view_sequence!=view_state.sequence) {
            traced_view_sequence=view_state.sequence;
            view_origin=timings.interaction();
        }
        // Visibility is a reliable, change-only packet, never repeated in
        // camera navigation's latest-value mailbox.
        mesh_input();
        const auto reply=dispatch(delivery,PreviewLogic::LiveLink{},DeliveryContext{
            generation,state,document_origin,view_origin,false,VisibilityDelivery{mesh_tools.visibility(),mesh_tools.visibility_revision()}});
        if(reply.view_submission) scroll_trace.submitted(generation,view_state.sequence,*reply.view_submission);
        if(!reply.error.empty()) status.text(reply.error);
    };
    auto viewport_changed = [&] {
        ++view_state.sequence;
        view_origin = timings.interaction();
    };
    auto send_snapshot = [&](u64 generation) {
        send_viewport(generation);
        delivery.reset_document(generation);
        flush_update(generation);
        delivery.reset_view(generation);
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
    // Worker schemas arriving during a capture are not shown (the inspector is
    // disabled then). A gesture's terminal event shows the latest one instead.
    const auto show_current_inspector = [&] {
        if (const auto schema = schemas.find(session->active_generation());
            schema != schemas.end() && schema->second.stamp.revision == state.document.revision)
            workspace.show_inspector(schema->second);
    };
    const auto feedback = [&](const WorkspaceFeedback& reply) {
        if(reply.camera_changed){viewport_changed();workspace.sync_camera_controls();}
        if(reply.pose_finished)show_current_inspector();
        if(!reply.message.empty())status.text(reply.message);
    };
    auto camera_changed = [&] {
        viewport_changed();
        workspace.sync_camera_controls();
    };
    auto end_camera_visit = [&](bool restore) {if(workspace.end_camera_visit(restore))camera_changed();};
    auto playback_changed = [&] {
        viewport_changed();
        workspace.sync_camera_controls();
        workspace.invalidate_inspector();
        pause.text(view_state.paused ? "Play (in editor)" : "Pause (in editor)");
    };
    auto commit_selection = [&](bool different) {
        status.text(workspace.selection_message());
        if(different)view_origin=timings.interaction();
    };
    auto select_object = [&](u32 object,editor::SelectionMode mode=editor::SelectionMode::replace) {
        const auto change=workspace.select_instance(object,mode);
        commit_selection(change.active_changed);
    };
    auto view_changed = [&] {workspace.view_changed();view.show(state);view_origin=timings.interaction();};
    auto open_mesh = [&](BlueprintId blueprint) {
        status.text(workspace.open_mesh(blueprint));view.show(state);view_origin=timings.interaction();
    };
    const auto mesh_reply = [&](const MeshEditingReply& reply) {
        if (reply.visibility_changed) viewport_changed();
        if (reply.authored) refresh_vertex();
        if (reply.camera_changed) camera_changed();
        if (reply.operation_options) tool_input_request({.show=ToolPanel::Show{*reply.operation_options,true}});
        if (!reply.message.empty()) status.text(reply.message);
        if (reply.clear_part_selection) {
            workspace.clear_mesh_part();
        }
    };
    const auto cancel_viewport = [&] {feedback(workspace.cancel_interaction());};
    auto launch = [&](u64 generation) {
        if (!has_camera(state)) {
            status.text("Add a camera first (Blueprints > Camera +): independent Play renders through the scene's active camera");
            return;
        }
        consume_edits();
        // Explicit Play initializes from the current document even with live
        // debugging disconnected. It does not enable the debug data stream.
        const auto reply=dispatch(delivery,PreviewLogic::StartingIndependentPlay{},DeliveryContext{generation,state});
        if(!reply.submitted) {status.text(reply.error);return;}
        diagnostic.value(false);
        playing = true;
        wanted_playing = true;
        mode_pending = true;
        mode_started = std::chrono::steady_clock::now();
        play.text("Stop (independent)");
    };
    auto send_event = [&](editor::Event event) {
        if (!playing) {
            auto local = workspace.apply_instance_control(event, session->active_generation(), workspace.minimum_inspector_sequence());
            if (!local) { status.text(local.error().message); return; }
            if (local->has_value()) { show_timeline(); return; }
        }
        if (!debug_link.value() || editing.awaiting_remote() || session->busy() || unresponsive ||
            !session->active_generation() ||
            !delivery.ready(session->active_generation(), state.document.revision)) {
            status.text("Preview is catching up; Apply again once its controls are ready.");
            return;
        }
        auto it = schemas.find(session->active_generation());
        if (it == schemas.end() || it->second.stamp.revision != state.document.revision ||
            it->second.stamp.object != view_state.selected_object ||
            it->second.stamp.context < workspace.minimum_inspector_sequence())
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
    const auto update_camera_panel = [&] {
        workspace.present_camera_actions(image.bounds(),image_camera,image_extent,
            image_id&&presented_info.has_view?static_cast<f32>(presented_info.time):view_state.time,
            playing,mode_pending,!logs_visible,modal_visible());
    };
    const auto present_camera_gizmo = [&] {
        workspace.present_camera_gizmo({.viewport=image.bounds(),
            .visible=!playing&&!mode_pending&&!logs_visible&&!modal_visible()&&image_id!=0&&
                (viewport_window.opened()||(!camera_ui.opened()&&!main_bar.opened()&&!scene_bar.opened())),
            .enabled=!inspecting()&&!editing.awaiting_remote()&&!viewport_interaction.transforming()},settings);
    };
    // The playhead may start on time zero; this is not a keyframe selection.
    timeline_input({.input={.clear_selection=true}});
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
            cancel_viewport(); timeline_input({.input={.cancel=true}}); mesh_input({.close_menu=true});
            workspace.close_camera_menu();
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
        input_sequence.identify(raw.events);
        if (viewport_window.opened() || !raw.framebuffer.empty()) controls_size.retain(raw);
        viewport_raw=viewport_window.opened() ? viewport_window.take_input() : raw;
        if (viewport_window.opened()) {
            scale_ui_input(viewport_raw,settings.ui_scale_percent);
            if (options.automation) {
                viewport_raw.events.clear(); viewport_raw.focused=false; viewport_raw.overflow=false;
                options.automation->viewport_input(viewport_raw);
            }
            input_sequence.identify(viewport_raw.events);
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
        session->poll();
        while(auto pending=session->take_event()) {
            const auto& event=pending->event;
            using K = editor::preview::EventKind;
            if (event.kind == K::candidate_started) {
                send_settings(event.generation);
                send_snapshot(event.generation);
                session->send(diagnostic.value() ? "diagnostic\n1" : "diagnostic\n0",
                              event.generation);
            } else if (event.kind == K::activated) {
                editing.abandon_remote();
                unresponsive = false;
                status.text("Preview generation " + std::to_string(event.generation) +
                            " ready / scene retained");
                session->send(debug_link.value() ? "link\n1" : "link\n0", event.generation);
                if (playing)
                    launch(event.generation);
                if (auto it = schemas.find(event.generation);
                    it != schemas.end() && it->second.stamp.revision == state.document.revision &&
                    !viewport_interaction.busy())
                    workspace.show_inspector(it->second);
                else if (debug_link.value())
                    flush_update(event.generation);
            } else if (event.kind == K::worker_failed || event.kind == K::build_failed) {
                if (event.kind == K::worker_failed &&
                    event.generation == image_generation && editing.active(EditGesture::move))
                    feedback(workspace.finish_interaction(ViewportTool::translation,true));
                if (event.kind == K::worker_failed && event.generation == image_generation)
                    feedback(workspace.finish_interaction(ViewportTool::rotation,true));
                if (event.kind == K::worker_failed && event.generation == image_generation)
                    feedback(workspace.finish_interaction(ViewportTool::scale,true));
                status.text(event.message);
                if (pending_generation == event.generation) {
                    editing.abandon_remote();
                    session->discard_frames();
                }
                std::cerr << event.message << '\n';
                std::cerr << session->logs() << '\n';
            } else if (event.kind == K::message) {
                const std::string_view message = event.message;
                if (message.starts_with("revision\n")) {
                    // PreviewLogic owns revision acknowledgement.
                } else if (message.starts_with("resync\n")) {
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
                    if (const auto schema=schemas.find(event.generation);
                        pending->schema_updated && schema!=schemas.end() &&
                        event.generation==session->active_generation() &&
                        schema->second.stamp.revision==state.document.revision && !viewport_interaction.busy())
                        workspace.show_inspector(schema->second);
                } else if (message.starts_with("state_patch\n") &&
                           event.generation == session->active_generation()) {
                    auto patch = decode_patch(message.substr(12));
                    if (patch && editing.awaiting_remote() && pending_generation == event.generation) {
                        if (auto applied = editing.accept_remote(event.generation, *patch); applied) {
                            consume_edits();
                            delivery.accepted(event.generation, state.document.revision);
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
                        session->discard_frames();
                    } else
                        delivery.reject(event.generation);
                } else if (message.starts_with("info\n") &&
                           event.generation == session->active_generation()) {
                    info = message.substr(5);
                    analysis_label.text(info);
                }
            }
        }
        if (auto latest = session->take_frame(minimum_frame_revision,image_revision,state.document.revision);
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
                    workspace.sync_camera_controls(false,true);
                }
                frame_label.text("Preview " + std::to_string(image_extent.width) + " x " +
                                 std::to_string(image_extent.height) +
                                 (playing ? " / debug preview (" + (settings.debug_fps
                                     ? std::to_string(settings.debug_fps) + " Hz" : std::string{"uncapped"}) + ")" : "") + " / gen " +
                                 std::to_string(image_generation) + " / update " +
                                 std::to_string(image_revision) +
                                 " / view " + std::to_string(presented_info.view_sequence) +
                                 (playing ? " / independent view"
                                  : inspecting() ? " / inspecting camera" : camera_ui.visiting() ? " / through camera" : " / editor camera") +
                                 (editing.dirty() ? " / unsaved" : ""));
                if (!debug_link.value())
                    frame_label.text(
                        "Debug link OFF / preview frozen; Play renders directly in worker window");
            }
        }
        if (editing.awaiting_remote() && now - pending_started > std::chrono::seconds(30)) {
            editing.abandon_remote();
            session->discard_frames();
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
        workspace.begin_viewport_frame();
        const bool viewport_gesture = viewport_interaction.busy() || timeline.dragging();
        const bool camera_numeric_active = editing.active(EditGesture::camera) && !navigation.dragging() && !walk.moving();
        const bool settings_was_open = settings_panel.visible();
        const bool import_was_open = import_dialog.visible();
        const bool open_was_open = open_dialog.visible();
        if(workspace.reconcile_camera_visit())camera_changed();
        camera_label.text(workspace.camera_label(playing));
        const bool dialog_was_open = modal_visible();
        const bool timeline_enabled = !dialog_was_open && !editing.awaiting_remote() && !playing &&
                                      !mode_pending && !viewport_interaction.busy();
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
        workspace.enable_camera_menu(!dialog_was_open && !editing.awaiting_remote());
        gizmo_only.enabled(!dialog_was_open && view_state.mode==ViewMode::scene &&
            find_instance(state,view_state.selected_object)!=nullptr);
        if(!gizmo_only.changedValue())gizmo_only.value(view_state.gizmo_only);
        const bool lists_enabled = !dialog_was_open && !editing.awaiting_remote() && !viewport_gesture;
        show_instances.enabled(lists_enabled); show_blueprints.enabled(lists_enabled);
        show_camera.enabled(!editing.awaiting_remote() && (!viewport_gesture || camera_numeric_active));
        files.enabled(!dialog_was_open);
        log_panel.enabled(!dialog_was_open);
        // Panels follow the gesture state at the start of this input frame; a
        // gesture that begins below changes them from the next frame.
        const auto panel_frame = [&] {
            return WorkspacePanelFrame{.modal=dialog_was_open,.mode_pending=mode_pending,.playing=playing,
                .inspector_ready=delivery.ready(session->active_generation(),state.document.revision) && debug_link.value() &&
                    !unresponsive && !session->busy() && current_schema!=schemas.end() &&
                    current_schema->second.stamp.revision==state.document.revision &&
                    current_schema->second.stamp.object==view_state.selected_object &&
                    current_schema->second.stamp.context>=workspace.minimum_inspector_sequence(),
                .gesture=viewport_gesture};
        };
        const auto update_pose_controls = [&] { workspace.present_pose_controls(panel_frame()); };
        workspace.present_panels(panel_frame());
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
        if (raw.overflow || !raw.focused)
            timeline_input({.input={.cancel=true}});
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
        // A selected blueprint-local part owns its gizmo list, including in
        // Surface mode. Whole-mesh mode still owns its own transform tools.
        const bool part_gizmo_target=view_state.mode==ViewMode::mesh &&
            mesh_tools.mode()!=MeshSelectMode::whole && blueprint_mesh_panel.has_gizmo();
        const int gizmo_cycle = take_gizmo_cycle(shortcut_input,
            !dialog_was_open && (viewport_keyboard || !ui_shortcut_capture) && !editing.awaiting_remote() && !viewport_gesture &&
            !playing && !mode_pending && !logs_visible && !viewport_interaction.object_tools_suspended() &&
            view_state.paused && ((view_state.mode == ViewMode::scene && selected_instances.size() != 0 &&
            workspace.interaction_mode() == InteractionMode::objects) || (view_state.mode==ViewMode::mesh &&
            (part_gizmo_target || mesh_tools.mode()!=MeshSelectMode::surface))));
        const auto file_shortcut = take_file_shortcut(shortcut_input, !dialog_was_open && !mode_pending &&
                                                                    !editing.awaiting_remote() && !viewport_gesture);
        const bool viewport_tab = take_viewport_tab(
            shortcut_input, !dialog_was_open && (viewport_keyboard || !ui_keyboard_capture) && !editing.awaiting_remote() && !viewport_gesture &&
                          !playing && !mode_pending && !logs_visible && debug_link.value() &&
                          image_id && editable_mesh(state) &&
                          image.bounds().contains(viewport_raw.pointer));
        // Follow accepted preview frames before hit testing, even while MMB
        // remains captured. The authoring-action block intentionally skips drags.
        update_camera_panel();
        present_camera_gizmo();
        tool_input_request({.context={.viewport=image.bounds(),.validate=true}});
        (void)dispatch(workspace,workspace_situation(view_state),WorkspaceContext{
            .presentation=ViewportPresentation{image.bounds(),show_fps.value(),
                !viewport_interaction.busy()&&!editing.busy()&&!walk.active()&&!playing&&!modal_visible(),
                camera_ui.opened()&&!viewport_window.opened()}});
        const bool viewport_popup=mesh_tools.menu_open() || regions.menu_open() || camera_gizmo.contains(viewport_raw.pointer) || tool_panel.contains(viewport_raw.pointer) || workspace.viewport_controls_contain(viewport_raw.pointer);
        // Escape first closes an open flyout or menu, or ends UI editing. Only
        // an unclaimed Escape leaves an explicitly chosen camera mode.
        const bool escape_claimed = popup_shortcut_capture || (!viewport_window.opened() &&
            (ui_shortcut_capture || camera_ui.opened() || workspace.list_flyout_open() || timeline.menu_open()));
        // A docked popup under the pointer gets first refusal for pointer input.
        // A text field, popup or capture in the controls keeps its keystrokes:
        // neither the popup layer nor a shortcut (Delete, Ctrl+Z) sees them.
        // A press in the same batch may move focus, so the popup then gets all.
        const auto pointer_event = [](const input::Event& event) {
            using K = input::EventKind;
            return event.kind==K::pointer_move || event.kind==K::pointer_down || event.kind==K::pointer_up || event.kind==K::scroll;
        };
        const bool docked_popup = viewport_popup && !viewport_window.opened();
        const bool controls_keyboard = docked_popup && ui_shortcut_capture && !popup_shortcut_capture &&
            std::ranges::none_of(ui_input.events, [](const input::Event& event) { return event.kind==input::EventKind::pointer_down; });
        auto popup_events = docked_popup ? ui_input.events : std::vector<input::Event>{};
        if (docked_popup)
            std::erase_if(ui_input.events, [&](const input::Event& event) { return !controls_keyboard || pointer_event(event); });
        auto input = screen.update(ui_input, dt, &clipboard);
        if (!input && raw.overflow) {
            // Screen discarded the incomplete input burst and released capture.
            // The authoring gestures above have already rolled back; publish
            // that compensation and resume with the next complete input frame.
            ui_keyboard_capture = false;
            ui_shortcut_capture = false;
            popup_shortcut_capture = false;
            status.text(input.error().message);
            broadcast();
            continue;
        }
        if (!input)
            return fail(input.error().message);
        if(workspace.resize_panels(geometry))layout(raw);
        // Panels get first refusal when docked (except for pointer input over
        // a viewport popup). Detached menus consume only their own window's input.
        if (!viewport_window.opened() && !viewport_popup) {
            viewport_ui_input=ui_input;
            viewport_ui_input.events=input->events;
        } else if (docked_popup) {
            if (controls_keyboard) {
                const input::AvailableEvents unused{input->unhandled()};
                std::erase_if(popup_events, [&](const input::Event& event) { return !pointer_event(event) && !unused.contains(event); });
            }
            viewport_ui_input.events = std::move(popup_events);
        }
        auto viewport_input=viewport_popups.update(viewport_ui_input,dt,&clipboard);
        if (!viewport_input) {
            if (!viewport_raw.overflow) return fail(viewport_input.error().message);
            popup_shortcut_capture = false;
            status.text(viewport_input.error().message); broadcast(); continue;
        }
        popup_shortcut_capture = viewport_input->capturesShortcuts;
        const auto camera_preferences_edit=workspace.poll_camera_gizmo(settings);
        if(camera_preferences_edit.changed)settings=camera_preferences_edit.value;
        if(!camera_preferences_edit.error.empty())status.text(camera_preferences_edit.error);
        if(camera_preferences_edit.committed) {
            if(auto saved=save_settings(preferences_path,settings);!saved)status.text(saved.error().message);
            else {
                send_settings(session->active_generation());
                for(auto generation:candidates)send_settings(generation);
            }
        }
        mesh_reply(dispatch(workspace,workspace_situation(view_state),WorkspaceContext{.poll_navigation=true}).mesh);
        const bool tool_menu_input=camera_gizmo.contains(viewport_raw.pointer) || workspace.viewport_controls_contain(viewport_raw.pointer) ||
            (tool_panel.opened() && (tool_panel.contains(viewport_raw.pointer) ||
            viewport_input->capturesPointer || viewport_input->capturesShortcuts));
        mesh_reply(tool_input_request({.context={.poll=true}}));
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
        if(scale_cancelled) feedback(workspace.finish_interaction(ViewportTool::scale,true));
        if(auto error=workspace.poll_camera_menu(raw.events,dialog_was_open);!error.empty())status.text(error);
        const bool camera_cancelled = editing.active(EditGesture::camera) &&
            (gesture_raw.overflow || !gesture_raw.focused || std::ranges::any_of(gesture_raw.events, [](const auto& event) {
                return event.kind == input::EventKind::focus_lost ||
                    (event.kind == input::EventKind::key_down && event.key == input::Key::escape);
            }));
        if (camera_cancelled) feedback(workspace.finish_interaction(ViewportTool::navigation,true));
        ui_keyboard_capture = input->capturesKeyboard;
        ui_shortcut_capture = input->capturesShortcuts;
        const auto previous_mesh_mode=mesh_tools.mode();
        mesh_input();
        if(mesh_tools.mode()!=previous_mesh_mode && mesh_tools.mode()!=MeshSelectMode::surface) {
            workspace.clear_mesh_part();
        }
        if(dialog_was_open && mesh_tools.menu_open()) {
            mesh_reply(mesh_input({.menu_events=viewport_raw.events, .poll_menu=true}));
        }
        if(dialog_was_open && regions.menu_open()) {
            if(auto result=workspace.poll_region_menu(viewport_raw.events); !result) status.text(result.error().message);
        }
        if(pop_viewport.clicked() && !dialog_was_open) toggle_viewport=true;
        const bool toolbar_menu_was_open = main_bar.opened() || scene_bar.opened();
        main_bar.poll(raw.events, !dialog_was_open, input->capturesPointer);
        scene_bar.poll(raw.events, !dialog_was_open, input->capturesPointer);
        if (main_bar.opened() || scene_bar.opened()) { workspace.close_camera_menu(); close_list_flyouts(); timeline_input({.input={.close_menu=true}}); }
        if(show_camera.clicked() && !dialog_was_open) {
            timeline_input({.input={.close_menu=true}});
            close_list_flyouts();
            main_bar.close(); scene_bar.close();
            workspace.toggle_camera_menu(settings);
        }
        list_input({.input={.events=raw.events,.poll_flyouts=true},.enabled=lists_enabled});
        if (show_instances.clicked() && lists_enabled) {
            timeline_input({.input={.close_menu=true}});
            main_bar.close(); scene_bar.close();
            workspace.close_camera_menu();
            const auto catalog=blueprint_catalog(state);
            list_input({.catalog=SceneListCatalog{scene_instances(state),catalog,selected_instances,false},
                .input={.toggle=SceneListInput::Toggle::instances}});
        }
        if (show_blueprints.clicked() && lists_enabled) {
            timeline_input({.input={.close_menu=true}});
            main_bar.close(); scene_bar.close();
            workspace.close_camera_menu();
            const auto catalog=blueprint_catalog(state);
            list_input({.catalog=SceneListCatalog{scene_instances(state),catalog,selected_instances,false},
                .input={.toggle=SceneListInput::Toggle::blueprints}});
        }
        if(auto proposed=workspace.camera_preferences(settings)) {
            auto& next=*proposed;
            if(!next)status.text(next.error().message);
            else if(auto saved=save_settings(preferences_path,*next);!saved)status.text(saved.error().message);
            else {
                settings=*next;workspace.camera_settings_changed(settings);
                send_settings(session->active_generation());
                for(auto generation:candidates)send_settings(generation);
                status.text("Camera preferences saved / scene unchanged");
            }
        }
        if (show_settings.clicked() && !dialog_was_open) {
            workspace.close_camera_menu();
            cancel_viewport(); timeline_input({.input={.cancel=true}});
            settings_panel.open(settings, state.document.timeline.tracks().size(), state.document.instances.size());
        }
        if(!dialog_was_open && !viewport_gesture && (smaller_ui.clicked() || larger_ui.clicked())) {
            auto next=settings;
            next.ui_scale_percent=static_cast<unsigned>(std::clamp(static_cast<int>(settings.ui_scale_percent)+(larger_ui.clicked()?5:-5),75,150));
            if(auto saved=save_settings(preferences_path,next);!saved) status.text(saved.error().message);
            else {
                cancel_viewport(); workspace.close_camera_menu();
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
                    // The panel has already validated the preference. This is
                    // session policy, not a scene edit or a worker allocation.
                    if (auto configured = editing.timeline_track_limit(settings.timeline_track_limit); !configured)
                        return fail(configured.error().message);
                    if (auto configured = editing.instance_limit(settings.instance_limit); !configured)
                        return fail(configured.error().message);
                    workspace.camera_settings_changed(settings);
                    next_ui_frame = {};
                    send_settings(session->active_generation());
                    for (auto generation : candidates) send_settings(generation);
                    settings_panel.close();
                    status.text("Editor settings saved / scene unchanged");
                }
            }
        }
        if (show_import.clicked() && !dialog_was_open) {
            cancel_viewport(); timeline_input({.input={.cancel=true}});
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
                    workspace.clear_inspector(); workspace.reset_move_tool();
                    workspace.show_tab(SidebarTab::properties);
                    workspace.interaction_mode(mesh?InteractionMode::vertices:InteractionMode::objects);
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
        workspace.poll_tabs();
        if (worker_log_tab.clicked()) { components_visible=false; component_text.visible(false); timing_panel.visible(false); log_text.visible(true); }
        if (timing_tab.clicked()) { components_visible=false; component_text.visible(false); timing_panel.visible(true); log_text.visible(false); }
        if (components_tab.clicked()) {
            components_visible=true; timing_panel.visible(false); log_text.visible(false); component_text.visible(true);
            component_text.value("Snapshot / click Component diagnostics to refresh\n\n" + component_report().string());
        }
        if (logs_visible)
            if (auto message = timing_panel.update(timings)) status.text(*message);
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
                delivery.reset_document(gen);
                if (*linked) {
                    send_snapshot(gen);
                    session->send(diagnostic.value() ? "diagnostic\n1" : "diagnostic\n0", gen);
                    status.text("Debug link ON / synchronizing the latest authored scene");
                } else {
                    workspace.reset_move_tool();
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
            workspace.reset_move_tool();
            if (logs_visible && components_visible)
                component_text.value("Snapshot / click Component diagnostics to refresh\n\n" + component_report().string());
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
            feedback(workspace.poll_gizmos(gizmo_cycle));
            if (pause.clicked()) {
                view_state.paused = !view_state.paused;
                view_state.time = std::min(view_state.time, state.document.timeline_duration);
                playback_changed();
            }
            if (undo.clicked()) shortcuts.insert(shortcuts.begin(), EditShortcut::undo);
            else if (redo.clicked()) shortcuts.insert(shortcuts.begin(), EditShortcut::redo);
            for(const auto shortcut:shortcuts) {
                const auto result=workspace.shortcut(shortcut);
                if(!result.message.empty())status.text(result.message);
                if(result.changed){view.show(state);pause.text(view_state.paused?"Play (in editor)":"Pause (in editor)");}
                if(result.discontinuous){session->discard_frames();broadcast();}
            }
            const auto lists_reply=workspace.poll_scene_lists(raw.events);
            feedback(lists_reply);
            if(lists_reply.mode_changed)view.show(state);
            feedback(workspace.poll_camera_actions());
            if(auto message=workspace.poll_interaction_mode();!message.empty()){status.text(message);view.show(state);}
            if (auto value = view.changedValue()) {
                if (value->mode == ViewMode::mesh) open_mesh(value->blueprint);
                else {
                    view_state.mode = value->mode;
                    view_state.selected_vertex = 0;
                    end_camera_visit(false);
                    view_state.editor_camera.target = {};
                    workspace.interaction_mode(InteractionMode::objects);
                    if (value->mode == ViewMode::sun) {
                        const auto* effect = view_instance(state, BlueprintKind::sun);
                        select_object(effect ? effect->id : 0);
                    }
                    view_changed();
                }
            }
            mesh_reply(workspace.poll_vertex_controls(true));
        }
        if (!dialog_was_open && !modal_visible() && !editing.awaiting_remote() && !camera_cancelled &&
            !inspecting() && (!viewport_gesture || camera_numeric_active))
            feedback(workspace.poll_camera_pose(settings));
        mesh_reply(workspace.poll_blueprint_controls(!dialog_was_open&&!modal_visible()&&!viewport_gesture&&!mode_pending&&!editing.busy()));
        feedback(workspace.poll_animation_controls(!dialog_was_open&&!modal_visible()&&!playing&&!mode_pending&&!editing.awaiting_remote(),input->unhandled()));
        if(!dialog_was_open && !modal_visible() && !editing.awaiting_remote() &&
           (!viewport_gesture || (editing.active(EditGesture::scale) && !scaling.dragging() && !instance_transform.active()))) {
            auto result=workspace.poll_inspector(!playing&&!scale_cancelled);
            feedback(result.feedback);
            if(!result.events.empty())send_event(std::move(result.events.front()));
        }
        if (timeline_enabled && !modal_visible() && !editing.awaiting_remote() && !playing && !mode_pending && raw.focused &&
            !raw.overflow) {
            const auto reply=timeline_input({.input={.poll=true,.unhandled=input->unhandled(),.raw=raw.events},.enabled=true});
            if(reply.select_instance){view.show(state);view_origin=timings.interaction();}
            if(reply.playback_changed) playback_changed();
            if(!reply.message.empty()) status.text(reply.message);
            update_pose_controls();
        }
        if (timeline.menu_open()) {
            workspace.close_camera_menu();
            close_list_flyouts(); main_bar.close(); scene_bar.close();
        }
        const auto viewport = image.bounds();
        if (!dialog_was_open && !modal_visible() && !viewport_gesture && !editing.busy() && !playing) {
            feedback(workspace.poll_world_bounds(settings.orbit_distance));
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
            (workspace.list_flyout_contains(viewport_raw.pointer) ||
             timeline.menu_contains(viewport_raw.pointer));
        // The camera overlay swallows presses that start on it, never a drag in
        // progress nor a press that began elsewhere in this input frame.
        const bool camera_overlay_blocks = camera_ui.overlay_contains(viewport_raw.pointer) && !navigation.dragging() &&
            !viewport_interaction.busy() && std::ranges::none_of(viewport_raw.events, [&](const input::Event& event) {
                return event.kind == input::EventKind::pointer_down && !camera_ui.overlay_contains(event.position);
            });
        const bool viewport_enabled = viewport_ready && !toolbar_blocks_viewport && !list_blocks_viewport && !camera_overlay_blocks;
        if (viewport_tab && viewport_enabled) {
            const auto message=workspace.change_interaction_mode(workspace.interaction_mode()==InteractionMode::objects
                ?InteractionMode::vertices:InteractionMode::objects);
            if(!message.empty())status.text(message);
            view.show(state);
        }
        // Priority applies to one occurrence, never to every event collected
        // since the previous render. Capture lives across these steps; elapsed
        // time is advanced once on the final tick, regardless of event count.
        ViewportInputSteps steps{viewport_raw,viewport_input->unhandled(),dt};
        while(auto step=steps.next()) {
        const auto& step_raw=step->frame;
        const auto step_input=step->unhandled;
        const ViewportInputContext viewport_context{
            .input={step_raw,step_input,step->seconds,dt,step->tick,
                !viewport_input->capturesShortcuts && (viewport_keyboard || !input->capturesShortcuts),
                viewport_keyboard || !ui_shortcut_capture,tool_menu_input},
            .presented={image_camera,image_extent,image.bounds(),session->active_generation(),image_revision,
                minimum_overlay_revision,image_time_current,delivery.ready(session->active_generation(),state.document.revision),
                current_schema!=schemas.end()?&current_schema->second:nullptr},
            .navigation={.drag_speeds=settings.camera_drag,.walk_speeds=settings.walk,.move_forward=settings.scroll_moves_camera,
                .enabled=viewport_enabled&&!inspecting()&&!camera_cancelled&&!camera_numeric_active,
                .escape_claimed=escape_claimed,.numeric_active=camera_numeric_active},
            .tools={viewport_enabled&&!modal_visible(),viewport_ready&&!modal_visible(),workspace.interaction_mode()==InteractionMode::vertices,
                diagnostic.value(),session->busy(),toolbar_blocks_viewport,
                view_state.mode==ViewMode::scene&&(!modal_visible()||regions.menu_open())&&!logs_visible&&!playing&&!diagnostic.value(),
                workspace.bounds_visible()}};
        auto routed=workspace.interact_viewport(viewport_context);
        if(routed.navigation.changed) camera_changed();
        if(routed.navigation.cancelled) feedback(workspace.finish_interaction(ViewportTool::navigation,true));
        if(routed.pose_finished) show_current_inspector();
        if(routed.view_changed) viewport_changed();
        if(routed.selection) commit_selection(routed.selection->active_changed);
        mesh_reply(routed.mesh);
        if(routed.native) send_event(std::move(*routed.native));
        if(!routed.status.empty()) status.text(routed.status);
        sync_sidebar();
        if(scroll_trace.enabled())
            scroll_trace.input(step_raw.events,step_input,viewport,routed.navigation_was_dragging,
                routed.previous_camera,preview_camera_pose(state,view_state.time),view_state.sequence,
                {{"modal",dialog_was_open||modal_visible()},{"independent-play",playing},
                 {"mode-transition",mode_pending},{"debug-link-off",!debug_link.value()},
                 {"callback-pending",editing.awaiting_remote()},{"worker-unresponsive",unresponsive},
                 {"logs-panel",logs_visible},{"no-image",!image_id},{"unfocused",!step_raw.focused},
                 {"input-overflow",step_raw.overflow},{"wrong-view",!matches_view(presented_info,state)}});
        const auto picked=workspace.pick_viewport({viewport_context,routed.selected_gizmo_handle});
        if(picked.selection) commit_selection(picked.selection->active_changed);
        mesh_reply(picked.mesh);
        if(picked.viewport_changed) view_origin=timings.interaction();
        if(!picked.status.empty()) status.text(picked.status);
        if(workspace.gizmos_dirty()) {
            workspace.acknowledge_gizmos();
            workspace.refresh_viewport_gizmos(viewport_context);
        }
        } // ordered viewport input steps
        if (delete_pressed(shortcut_events, shortcut_raw.events,
                           shortcut_raw.focused && !shortcut_raw.overflow && !dialog_was_open && !modal_visible() &&
                           !save_dialog.visible() && !editing.awaiting_remote() && !playing && !mode_pending &&
                           !viewport_interaction.busy() && !timeline.dragging() && !logs_visible)) {
            const auto result=workspace.delete_selection();
            if(!result.message.empty())status.text(result.message);
            if(result.changed)view.show(state);
        }
        if(workspace.publish_mesh_requested() && !editing.awaiting_remote() && !viewport_gesture && !playing && !modal_visible() && view_state.mode==ViewMode::mesh) {
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
            save.clicked() || workspace.save_mesh_requested() || save_as.clicked() ||
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
                timeline_input({.input={.cancel=true}});
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
            timeline_input({.input={.cancel=true}});
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
                    (void)workspace.end_camera_visit(false);
                    session->discard_frames();
                    workspace.reset_selection();
                    mesh_input({.reset=true});
                    timeline_input({.input={.reset=true}});
                    workspace.reset_panel_scroll();
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
        if (modal_visible()) { main_bar.close(); scene_bar.close(); close_list_flyouts(); timeline_input({.input={.close_menu=true}}); }
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
        workspace.present_tool_options(image.bounds(),viewport_enabled,diagnostic.value());
        (void)dispatch(workspace,workspace_situation(view_state),WorkspaceContext{
            .presentation=ViewportPresentation{image.bounds(),show_fps.value(),
                !viewport_interaction.busy()&&!editing.busy()&&!walk.active()&&!playing&&!modal_visible(),
                camera_ui.opened()&&!viewport_window.opened()}});
        // Selection, camera visits and layout may also change during this tick.
        update_camera_panel();
        present_camera_gizmo();
        auto popup_list=viewport_popups.draw_list();
        if (!popup_list) return fail(popup_list.error().message);
        camera_gizmo.append(*popup_list);
        const bool viewport_uncovered=(!camera_ui.opened() && !main_bar.opened() && !scene_bar.opened() && !timeline.menu_open()) || viewport_window.opened();
        const bool blocking_dialog=save_dialog.visible() || settings_panel.visible() || import_dialog.visible() || open_dialog.visible();
        const bool show_mesh_overlay = viewport_uncovered && !blocking_dialog && workspace.interaction_mode() == InteractionMode::vertices &&
            mesh_tools.component_mode() &&
            !playing && !mode_pending && !logs_visible && !diagnostic.value() &&
            image_time_current && matches_view(presented_info,state) && image_revision >= minimum_overlay_revision;
        if (viewport_uncovered && (!modal_visible() || regions.menu_open())) {
            // Compose annotation geometry immediately above its viewport image,
            // below inspector widgets and popups, including its own RMB menu.
            ui::DrawList annotations;
            workspace.append_region_overlays(annotations,*font);
            const auto viewport_draw=std::ranges::find_if(viewport_list->commands,[&](const auto& command) {
                const auto* draw=std::get_if<ui::ImageDraw>(&command);
                return draw && draw->pixels==preview_pixels;
            });
            if(viewport_draw!=viewport_list->commands.end())
                viewport_list->commands.insert(std::next(viewport_draw),std::make_move_iterator(annotations.commands.begin()),
                    std::make_move_iterator(annotations.commands.end()));
        }
        if (viewport_uncovered && !modal_visible()) {
            workspace.append_tool_overlays(*viewport_list,*font,image_camera,image_extent,image.bounds(),viewport_enabled,
                image_revision,std::chrono::duration<double>(now.time_since_epoch()).count());
        }
        // The range menu covers the inspector, not the ruler. Keep destination
        // markers visible while choosing a range; insertion retains draw order.
        if (!modal_visible() && !camera_ui.opened() && !main_bar.opened() && !scene_bar.opened() &&
            !workspace.list_flyout_open()) timeline.append(*list);
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
                    (translation.visible() || rotation.visible() || scaling.visible() || viewport_interaction.mesh_part.visible())) &&
                    !viewport_interaction.object_tools_suspended() && viewport_uncovered && !modal_visible(),
                .dragging = instance_transform.active() || translation.dragging() || rotation.dragging() || editing.active(EditGesture::scale) || regions.dragging() ||
                    viewport_interaction.mesh.active() || viewport_interaction.pivot.dragging() || viewport_interaction.mesh_part.dragging(),
                .inspector_ready = delivery.ready(active, state.document.revision) && schema != schemas.end() &&
                    schema->second.stamp.revision == state.document.revision &&
                    schema->second.stamp.object == view_state.selected_object &&
                    schema->second.stamp.context >= workspace.minimum_inspector_sequence(),
                .mesh_overlay_visible = show_mesh_overlay,
                .rotation_gizmo = instance_transform.active() ? instance_transform.gesture().rotation_gizmo() :
                    rotation_mode(gizmo_mode.value()) ? &rotation.tool() : nullptr,
                .scale_gizmo = instance_transform.active() ? instance_transform.gesture().scale_gizmo() :
                    gizmo_mode.value() == GizmoMode::scale ? &scaling : nullptr,
                .translation_gizmo = instance_transform.active() ? instance_transform.gesture().translation_gizmo() :
                    translation_mode(gizmo_mode.value()) ? &translation.tool() : nullptr,
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
                .rotation_origin_gizmo = rotation_pivot.moving() ? &viewport_interaction.pivot.tool() : nullptr,
                .rotation_origin = rotation_pivot.value().point,
                .mesh_part_gizmo = &viewport_interaction.mesh_part,
                .blueprint_pending = blueprint_mesh_panel.pending(image_revision),
                .mesh_socket_gizmo = &viewport_interaction.mesh_sockets,
                .component_diagnostics = component_report,
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
