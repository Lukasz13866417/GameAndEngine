/* Curated runtime ownership, not a generated include graph or live inspector.
 * `declarations` ties every editor/worker node to source; tests check it.
 * Module links reuse the guide catalog. */
(() => {
  "use strict";
  const node = (id, title, symbol, role, summary, detail, source, options = {}) => ({
    id, title, symbol, role, summary, detail, sources: [source], children: [], ...options
  });
  const ui = (id, title, symbol, summary, detail, file, options) =>
    node(id, title, symbol, "UI", summary, detail, `examples/editor/${file}`, options);
  const logic = (id, title, symbol, summary, detail, file, options) =>
    node(id, title, symbol, "Logic", summary, detail, `examples/editor/${file}`, options);
  const tool = (id, title, symbol, summary, file) => ui(id, title, symbol, summary,
    "Keeps its own interaction state. The workspace coordinates authoring; a tool does not get arbitrary access to sibling components.", file);
  const domain = (id, title, symbol, summary, detail, file, options) =>
    node(id, title, symbol, "Data", summary, detail, `examples/editor/${file}`, options);

  const editor = node("editor-host", "Editor application", "editor_example::run", "Host",
    "Windows, input routing and the two main owners.",
    "The UI-process entry point assembles the workspace and preview coordinator. It owns windows, application bars/dialogs and preview/GPU scheduling. Sidebar construction, selection consequences, gesture cleanup and camera visits belong to the workspace. Live tools are exposed to this host only as const observations.",
    "examples/editor/app.cpp", { children: [
      ui("workspace", "Editing workspace", "EditingWorkspaceUI", "Authoring authority and the UI branches that use it.",
        "The lowest shared owner coordinates scene selection, panels and edit proposals. Children borrow read-only session observations; the workspace executes edits, then acknowledges the outcome. UI ownership means panel objects and widget handles, not ownership of Screen's widget storage.", "workspace_ui.hpp", {
          children: [
            domain("session", "Editing session", "EditingSession", "One authority for edits, history and persistence.",
              "Transactions group a gesture into one undoable change. The session publishes explicit DocumentChanges; the caller drains those changes for preview delivery. It owns the scene-file identity and distinguishes saving from publishing blueprint drafts.", "editing_session.hpp", {
                children: [
                  domain("state", "Document + private view", "State", "Authored content and editor-only viewing state.",
                    "These two values are siblings inside State. Navigating the editor camera must not become a document edit or rebuild the timeline.", "project.hpp", { children: [
                      domain("document", "Authored scene", "Document", "Blueprints, instances, mesh drafts and timeline.",
                        "Instances refer to blueprint identities and have their own transforms. Mesh drafts are separate from published geometry. The animation camera, environment, world bounds and timeline are authored values.", "project.hpp"),
                      domain("private-view", "Private viewport state", "ViewportState", "Editor camera, inspection mode and viewing intent.",
                        "This is not the rectangle containing the image. It is the editor's private choice of camera, inspected mesh, time and decorations. Absolute, independently sequenced viewing requests can replace older viewing requests without losing authored edits.", "project.hpp", { references: [{ scope: "editor", id: "preview", label: "Delivered through PreviewLogic" }] })
                    ] }),
                  domain("history", "Transactions & undo", "Gesture / Checkpoint", "Gesture baseline, undo/redo and saved-content identity.",
                    "These are session-owned records, not another service or manager. A committed gesture produces a history entry; cancelling restores its baseline.", "editing_session.hpp"),
                  domain("clipboard", "Clipboard", "EditClipboard", "Copies that preserve blueprint/instance distinctions.",
                    "A session-owned clipboard captures reusable edit data. Paste is performed through the session so history, selection outcomes and change delivery stay coordinated.", "edit_clipboard.hpp"),
                  domain("scene-file", "Scene persistence", "SceneFile", "Load/save paths and saved scene contents.",
                    "Disk persistence belongs to the editing session, not individual panels. A successful save updates the saved-content identity used for the unsaved indicator.", "scene_file.hpp")
                ] }),
            domain("selection", "Workspace selection", "WorkspaceSelection", "One scene selection observed by lists and viewport.",
              "Selection reconciliation and publication happen at the workspace. Selecting a list row or picking the scene does not create independent, competing selections in each panel.", "workspace_selection.hpp"),
            ui("viewport", "Editing viewport", "EditingViewportUI", "Mesh editing, overlays, picking and interaction tools.",
              "This is the editing branch surrounding the displayed worker image. It routes context and input into tools, polls local proposals and leaves document mutation to its workspace parent.", "workspace_ui.hpp", {
                borrows: ["const EditingSession& from its workspace", "Container handles backed by the UI Screen"], children: [
                  ui("mesh-editing", "Mesh editing", "MeshEditingUI", "Editing modes, selection and mesh-operation proposals.",
                    "InspectMesh, InspectScene and InspectEffect are distinct local situations. The parent calls the applicable private handler; returned proposals carry owned selections so topology changes cannot invalidate borrowed input.", "mesh_editing_ui.hpp", { children: [
                      ui("mesh-tools", "Mesh modes & menu", "MeshToolsUI", "Vertex, edge, face, clean and whole-mesh modes.",
                        "Owns component selection and the mesh context menu. The scene mesh remains a blueprint draft until explicitly applied.", "mesh_tools_ui.hpp", { children: [
                          ui("mesh-menu", "Mesh context menu", "MeshMenu", "Local options such as subdivide and align to line.",
                            "Presents operations suitable for the current component selection. It reports a local menu result; it does not reach into unrelated UI branches.", "mesh_menu.hpp")
                        ] }),
                      ui("mesh-operation", "Operation options", "MeshOperationControls", "Adjust the active mesh operation.",
                        "Owns the operation's options and pending adjustments. The workspace applies an adjustment and returns its success or error.", "mesh_operation_controls.hpp")
                    ] }),
                  ui("viewport-camera-ui", "Camera controls and visits", "ViewportCameraUI", "Camera settings menu and Enter / Inspect / Back state.",
                    "Owns pose widgets, the settings draft and camera-action overlay. The workspace polls local intent, updates the private view or explicitly saves a scene camera. Returning from a visit restores the captured view. No mutable camera widgets escape to the app.", "viewport_camera_ui.hpp", { children: [
                      ui("camera-actions", "Camera action overlay", "CameraPanel", "Actions anchored to a selected camera glyph.",
                        "Projects the selected camera using the currently displayed image camera. Enter, Inspect, Save and Set active are local facts polled by the workspace.", "camera_panel.hpp"),
                      ui("camera-preferences", "Camera preference draft", "CameraPreferences", "Numeric viewing and orbit limits.",
                        "A UI-only draft that validates a proposed Settings value. The application persists accepted preferences; the panel cannot write files or mutate the document.", "camera_preferences.hpp")
                    ] }),
                  ui("mesh-navigation", "Mesh-view controls", "MeshNavigationControls", "Mesh-centered camera and camera-transform baking.",
                    "Owns the small mesh-view controls, not the camera gesture logic. It describes local choices and exposes bake requests to its parent.", "mesh_navigation.hpp"),
                  ui("blueprint-panel", "Blueprint geometry panel", "BlueprintMeshPanel", "Draft controls, cloud formations and Earth addons.",
                    "Owns blueprint-specific widgets and local editing work. Pending results are drained by the workspace, which applies them to the authoritative session.", "blueprint_mesh_panel.hpp"),
                  ui("interaction", "Viewport interaction", "ViewportToolsUI", "Owns tools and arbitrates who receives input.",
                    "One tool owns document editing at a time. Camera navigation may temporarily borrow pointer input during a transform. Handled input does not fall through to selection; lack of input does not hide a passive gizmo.", "viewport_tools_ui.hpp", { borrows: ["const EditingSession& (observations, not mutation)"], children: [
                      ui("navigation", "Camera gizmo", "CameraGizmo", "A camera gizmo that owns navigation gizmos and a corner menu.",
                        "A gizmo is a target-specific editing interaction, including presentation and controls, not just an arrow or ring. It can own other gizmos or menus. This one is the fallback when no object gizmo is visible. Explicit mode selection gives its children LMB; familiar MMB/modifier/wheel shortcuts can borrow input during object edits. NavigationFrame supplies a target pose, input and settings for editor or entered scene cameras. A simple update returns a proposed pose; poll returns preference edits. No document mutation or sibling lookup. The linked definition explains blueprint ownership, local bindings and parent-controlled activation.", "camera_gizmo.hpp", { guide: "../editor_components.md#gizmos-and-typed-movement-bindings", children: [
                          ui("camera-pointer", "Pointer gizmos × 5", "CameraPointerGizmo", "Orbit, Look, Pan, Forward/back and Optical zoom children.",
                            "Five separately owned children share one implementation. Each owns a CameraPointerLogic (camera_pointer_logic.hpp) capture/math helper and a mode-local sensitivity control. The parent routes each input occurrence to its captured child or the selected shortcut, so menu input cannot leak into camera motion.", "camera_gizmo.hpp"),
                          ui("camera-walk", "Walk gizmo", "CameraWalkGizmo", "Held-key walking with its own speed sliders.",
                            "Owns CameraWalkLogic and forward, sideways, vertical and Shift-speed controls, shown only in Walk. Elapsed-time movement is independent of OS key repeat. Typing or controls-window focus clears held keys without leaving Walk; Escape returns to object tools.", "camera_gizmo.hpp")
                        ] }),
                      tool("gizmo-controls", "Gizmo input & options", "GizmoControls", "Sensitivity, scale limits and shared gizmo input.", "gizmo_controls.hpp"),
                      tool("move", "Move gizmo", "SceneMoveGizmo", "Selection movement, including blueprint-defined axes.", "scene_move_gizmo.hpp"),
                      tool("rotate", "Rotation gizmo", "InstanceRotationGizmo", "Rings, arrow control and selection pivots.", "instance_rotation_gizmo.hpp"),
                      tool("scale", "Scale gizmo", "ScaleTool", "Interactive instance scale and axis scale.", "scale_tool.hpp"),
                      tool("instance-transform", "Instance transforms", "InstanceTransformGizmo", "Keyboard-driven transforms on scene instances.", "instance_transform_gizmo.hpp"),
                      tool("mesh-transform", "Mesh transforms", "MeshTransformGizmo", "Component and whole-mesh transformations.", "mesh_transform_gizmo.hpp"),
                      tool("regions", "Region editing", "RegionEditor", "Per-instance boundaries, vertex tools and region UI.", "region_editor.hpp"),
                      tool("bounds", "World bounds", "WorldBoundsTool", "Manipulate the scene's world-boundary cuboid.", "world_bounds_tool.hpp"),
                      tool("surface-parts", "Surface-part gizmos", "SurfacePartTool", "Earth clouds, addons and tunnel controls.", "surface_part_tool.hpp"),
                      tool("sockets", "Socket picking", "SocketPickTool", "Choose attachment sockets on eligible parts.", "socket_pick_tool.hpp"),
                      tool("rotation-origin", "Custom pivot", "MoveGizmo<RotationOriginMovement>", "A reusable move gizmo with an explicit binding.", "rotation_origin_movement.hpp"),
                      logic("selection-input", "Selection input", "SelectionInputLogic", "Interpret selection input after higher-priority tools.",
                        "Keeps selection gesture logic separate from tool arbitration and authoritative workspace selection.", "selection_input_logic.hpp"),
                      tool("box-select", "Box selection", "BoxSelection", "Rectangle capture and its visual overlay.", "box_selection.hpp"),
                      domain("picking", "Picking projection", "InstanceProjection", "Cached projection and accelerated instance queries.",
                        "Picking uses evidence about the displayed view. It is derived state owned by the interaction branch, not a second scene document.", "selection.hpp")
                    ] }),
                  ui("tool-panel", "Tool options panel", "ToolPanel", "The active tool's menu near the viewport corner.",
                    "Presents options supplied by the current tool. A gizmo can supply unusual controls; it does not have to be only a set of axis arrows.", "tool_panel.hpp"),
                  ui("gizmo-selector", "Gizmo chooser", "GizmoSelector", "Available gizmos and their shortcuts.",
                    "Displays the common editing capabilities of the selected targets. It does not own the scene instances being edited.", "gizmo_selector.hpp"),
                  ui("pivot-controls", "Rotation pivot controls", "RotationPivotControls", "Selection center, individual centers or custom point.",
                    "A UI choice supplies pivot policy to the transform path. The actual movement of the custom origin uses a bound move gizmo.", "rotation_pivot_controls.hpp")
                ] }),
            ui("workspace-panels", "Sidebar panels", "WorkspacePanels", "Sidebar widgets, tab presentation and draggable splitters.",
              "Owns the actual widget handles and reversible resize drafts. EditingWorkspaceUI coordinates selection labels, inspector eligibility and commands. The application only supplies layout and host availability facts.", "workspace_panels.hpp"),
            ui("world-bounds-panel", "World bounds controls", "WorldBoundsPanel", "Visibility, framing and numeric bounds.",
              "A workspace-owned UI child. The workspace applies numeric edits as transactions and frames bounds through its private camera; the application never reads the individual fields.", "world_bounds_panel.hpp"),
            ui("scene-lists", "Scene lists", "SceneLists", "Instance, region and blueprint lists, including flyouts.",
              "Keeps list presentation and local interaction outcomes. The workspace coordinates list/viewport selection and executes instance or blueprint operations.", "scene_lists.hpp"),
            ui("timeline", "Timeline editing", "TimelineEditingUI", "Keyframe selection, presentation and edit proposals.",
              "Owns the timeline panel. It observes the session and reports local actions; the workspace executes the authoring operation and coordinates other panels.", "timeline_editing_ui.hpp", { children: [
                ui("timeline-panel", "Keyframe panel", "TimelinePanel", "Keyframe rows, instance sections and editable fields.",
                  "Widget state lives here; authored tracks live in Document. This separation keeps a panel refresh from becoming a timeline reconstruction.", "timeline_panel.hpp")
              ] })
          ] }),
      logic("preview", "Preview coordination", "PreviewLogic", "Worker lifetime, delivery and completed-frame adoption.",
        "Owns the process/transport facade, authored-update delivery state and completed-frame mailbox. It receives immutable observations from the host. The worker is a separate process, not another object holding a reference to the UI's Document.", "preview_logic.hpp", { children: [
          node("transport", "Processes & transport", "preview::PreviewSession", "Platform", "Launch workers, reload and exchange packets/images.",
            "Manages worker generations and transport. Authored deltas are ordered; replaceable viewing intent uses a latest-value lane. Independent Play may own another native window.", "include/vng/editor/preview.hpp", { references: [{ scope: "worker", id: "worker-host", label: "Open the separate worker process" }] }),
          logic("delivery", "Document delivery", "PreviewDeliveryLogic", "Coalesce changes and wait for revision acknowledgments.",
            "Preserves explicit changes to properties or vertices. Structural changes and recovery can require snapshots; camera-only navigation does not use this authored lane.", "preview_delivery_logic.hpp"),
          logic("mailbox", "Completed-frame mailbox", "PreviewMailbox", "Adopt only a coherent completed image.",
            "Tracks the completed frame against worker generation and authored revisions. Displayed-camera metadata belongs to the image actually shown, not merely to the latest requested view.", "preview_mailbox.hpp")
        ] }),
      node("screen", "Widget storage", "vng::ui::Screen", "UI", "Retained widgets, input state and draw data.",
        "Screen owns widget storage. Panels retain lightweight container/widget handles into it. Rendering the Screen is separate from modifying the scene document.", "include/vng/ui/ui.hpp"),
      node("ui-draw", "UI drawing", "vng::opengl::UiRenderer", "GPU", "Concrete rendering of widget shapes and text.",
        "The OpenGL UI renderer consumes Screen draw data. It does not decide editing policy, scene selection or undo semantics.", "include/vng/ui_opengl/ui_renderer.hpp"),
      node("shell-source", "Toolbars & dialogs", "app.cpp / dialog components", "UI", "Top bars, settings, import/save and window layout.",
        "The application still composes these UI elements and polls their outcomes. They are shown as a source grouping rather than a fictional single class.", "examples/editor/app.cpp", { edge: "group", sources: ["examples/editor/app.cpp", "examples/editor/save_dialog.hpp", "examples/editor/editor_layout.hpp"] })
    ] });

  const worker = node("worker-host", "Preview worker process", "main() → run()", "Host",
    "A separate process, context and render loop.",
    "main owns the WorkerEndpoint; run creates a window/device session and a Worker. The loop receives packets, processes window events, advances playback, renders and transfers completed images. Nothing here aliases the UI-process Document.", "examples/editor_worker.cpp", { children: [
      node("worker-endpoint", "Transport endpoint", "preview::WorkerEndpoint", "Platform", "Control packets and image publication.",
        "Owned in main and borrowed by Worker. The UI sends edits and viewing intent; this endpoint returns revisions, schemas, status and completed pixels.", "include/vng/editor/preview.hpp", { references: [{ scope: "editor", id: "transport", label: "UI-side PreviewSession" }] }),
      node("gl-session", "Native window + device", "example::GlfwOpenGLSession", "Platform", "Keeps the graphics context alive around Worker.",
        "The host owns the window/device session; Worker borrows both. Runtime resources are destroyed while this context is still alive. The same worker can present an independent Play window.", "examples/support/glfw_opengl_session.hpp"),
      node("worker", "Worker", "Worker", "Logic", "Request handling, playback and render scheduling.",
        "Owns its state replica and GPU runtime. Window, Device and WorkerEndpoint are explicit borrowed references. Playback and rendering are driven by this loop, not by requests for every simulation step from the UI.", "examples/editor_worker.cpp", { borrows: ["WorkerEndpoint& from main", "Device& and Window& from the host's GlfwOpenGLSession"], children: [
          node("worker-state", "Worker-side state", "State", "Data", "A replica, not shared mutable editor state.",
            "Document patches update this replica. Absolute viewport requests have their own sequence. ProjectControls and Inspector expose editing schemas around this worker state.", "examples/editor/project.hpp"),
          node("inbox", "Latest viewing intent", "ViewportInbox", "Logic", "Accept independently sequenced view requests.",
            "Replaces obsolete private viewing intent while keeping authored document updates ordered. Camera navigation should not require a scene-file round trip.", "examples/editor/viewport_session.hpp"),
          node("runtime", "Rendering runtime", "Runtime → Runtime::Impl", "GPU", "Realizations per blueprint, effects and frame targets.",
            "Borrows Device and current scene/view inputs for operations. It owns GPU realizations and derived scene samples, not the authored document. Ordinary transform changes become draw-ticket values rather than mesh rebuilds.", "examples/editor/runtime.cpp", { children: [
              node("programs", "Shared mesh programs", "MeshPrograms", "GPU", "Stable program lifetimes enclosing mesh renderers.",
                "Programs are declared before the per-blueprint map and therefore destroyed after it. Child renderers explicitly borrow the appropriate program.", "examples/editor/mesh_programs.hpp"),
              node("mesh-resources", "Per-blueprint meshes", "MeshResource / BlueprintMeshRenderer", "GPU", "One resident realization shared by many instance tickets.",
                "The map is keyed by BlueprintId. Mesh resources own concrete renderers; compatible instances are submitted together. Solid and wireframe raster states use separate internal batches, not a renderer per scene instance.", "examples/editor/runtime.cpp", { children: [
                  node("blueprint-renderer", "Mesh renderer", "BlueprintMeshRenderer", "GPU", "GPU geometry and reusable instancing buffers.",
                    "A concrete opengl::Renderer<mesh_shading::MeshDraw>. It owns GpuMesh and two Batch records; it borrows the Runtime-owned Program. This is a representative map entry, not an extra singleton renderer.", "examples/editor/blueprint_mesh_renderer.hpp", { borrows: ["const opengl::Program* from Runtime::Impl"], references: [{ scope: "worker", id: "programs", label: "Borrowed program owner" }] })
                ] }),
              node("effects", "Effects & annotations", "SunRenderer / Starfield / PortalHaze", "Group", "Sun, space backdrop, tunnel haze and editor decorations.",
                "A grouping of Runtime::Impl members. Effect-specific renderers own their resources and policies; the Runtime coordinates them with scene geometry.", "examples/editor/runtime.cpp", { edge: "group" }),
              node("postprocess", "Frame & postprocess resources", "RenderTarget / Bloom / DisplaySurface", "Group", "HDR targets, softening, bloom and display conversion.",
                "These resources are owned directly by Runtime::Impl. They are grouped here for readability, not to imply a separate postprocess manager.", "examples/editor/runtime.cpp", { edge: "group" }),
              node("readback", "GPU image transfer", "opengl::Rgba8ReadbackQueue", "GPU", "Queue transfers and poll completed images.",
                "Runtime owns the readback queue; Worker keeps matching frame metadata and the latest completed image awaiting publication. The UI never borrows a GPU texture from this process.", "include/vng/opengl/readback.hpp"),
              node("samples", "Sampled scene values", "SceneSamples", "Data", "Derived values at the requested playback time.",
                "Cached sampled instances supply renderer tickets. These values are derived from the worker's document and timeline, not another authoring authority.", "examples/editor/scene_samples.hpp")
            ] })
        ] })
    ] });

  // Where each node is declared. `witness` is the definition head of the
  // node's class (or host function) in its first source. `member` is its
  // declaration inside the owner's body: the parent's witness, or `memberOf`
  // when the owner is another function in the parent's file. A dashed group
  // lists the `members` it groups. Tests check all of them, so moving a member
  // to another owner needs only this table and the node's place in the tree.
  const declarations = {
    editor: {
      "editor-host": { witness: "int run(const Options& options)" },
      workspace: { witness: "class EditingWorkspaceUI final", member: "EditingWorkspaceUI workspace{std::move(initial), std::move(scene_file)};" },
      session: { witness: "class EditingSession final", member: "EditingSession editing_;" },
      state: { witness: "struct State", member: "State state_;" },
      document: { witness: "struct Document", member: "Document document;" },
      "private-view": { witness: "struct ViewportState", member: "ViewportState viewport{};" },
      history: { witness: "struct Checkpoint", member: "std::deque<Checkpoint> undo_, redo_;" },
      clipboard: { witness: "class EditClipboard", member: "EditClipboard clipboard_;" },
      "scene-file": { witness: "class SceneFile final", member: "SceneFile file_;" },
      selection: { witness: "class WorkspaceSelection final", member: "WorkspaceSelection selection_;" },
      viewport: { witness: "class EditingViewportUI final", member: "std::optional<EditingViewportUI> viewport_;" },
      "mesh-editing": { witness: "class MeshEditingUI final", member: "MeshEditingUI mesh_;" },
      "mesh-tools": { witness: "class MeshToolsUI", member: "MeshToolsUI components_;" },
      "mesh-menu": { witness: "class MeshMenu final", member: "MeshMenu menu_;" },
      "mesh-operation": { witness: "class MeshOperationControls final", member: "MeshOperationControls operation_;" },
      "viewport-camera-ui": { witness: "class ViewportCameraUI final", member: "std::optional<ViewportCameraUI> camera_;" },
      "camera-actions": { witness: "class CameraPanel final", member: "CameraPanel overlay_;" },
      "camera-preferences": { witness: "class CameraPreferences", member: "CameraPreferences preferences_;" },
      "mesh-navigation": { witness: "class MeshNavigationControls", member: "MeshNavigationControls navigation_;" },
      "blueprint-panel": { witness: "class BlueprintMeshPanel final", member: "std::optional<BlueprintMeshPanel> blueprint_panel_;" },
      interaction: { witness: "class ViewportToolsUI", member: "std::optional<ViewportToolsUI> interaction_;" },
      navigation: { witness: "class CameraGizmo", member: "CameraGizmo navigation_;" },
      "camera-pointer": { witness: "class CameraPointerGizmo", member: "std::array<CameraPointerGizmo,5> pointer_" },
      "camera-walk": { witness: "class CameraWalkGizmo", member: "CameraWalkGizmo walk_;" },
      "gizmo-controls": { witness: "class GizmoControls final", member: "GizmoControls gizmo_input;" },
      move: { witness: "class SceneMoveGizmo final", member: "SceneMoveGizmo translation;" },
      rotate: { witness: "class InstanceRotationGizmo", member: "InstanceRotationGizmo rotation;" },
      scale: { witness: "class ScaleTool", member: "ScaleTool scale;" },
      "instance-transform": { witness: "class InstanceTransformGizmo", member: "InstanceTransformGizmo instances;" },
      "mesh-transform": { witness: "class MeshTransformGizmo", member: "MeshTransformGizmo mesh;" },
      regions: { witness: "class RegionEditor", member: "RegionEditor regions;" },
      bounds: { witness: "class WorldBoundsTool", member: "WorldBoundsTool bounds;" },
      "surface-parts": { witness: "class SurfacePartTool", member: "SurfacePartTool mesh_part;" },
      sockets: { witness: "class SocketPickTool", member: "SocketPickTool mesh_sockets;" },
      "rotation-origin": { witness: "struct RotationOriginMovement", member: "MoveGizmo<RotationOriginMovement> pivot;" },
      "selection-input": { witness: "class SelectionInputLogic", member: "SelectionInputLogic selection_input;" },
      "box-select": { witness: "class BoxSelection", member: "BoxSelection selection_box;" },
      picking: { witness: "class InstanceProjection", member: "InstanceProjection instance_projection;" },
      "tool-panel": { witness: "class ToolPanel", member: "ToolPanel tools_;" },
      "gizmo-selector": { witness: "class GizmoSelector", member: "std::optional<GizmoSelector> gizmo_selector_;" },
      "pivot-controls": { witness: "class RotationPivotControls", member: "std::optional<RotationPivotControls> rotation_pivot_;" },
      "workspace-panels": { witness: "class WorkspacePanels final", member: "std::optional<WorkspacePanels> panels_;" },
      "world-bounds-panel": { witness: "class WorldBoundsPanel", member: "std::optional<WorldBoundsPanel> bounds_panel_;" },
      "scene-lists": { witness: "class SceneLists final", member: "std::optional<SceneLists> lists_;" },
      timeline: { witness: "class TimelineEditingUI final", member: "std::optional<TimelineEditingUI> timeline_;" },
      "timeline-panel": { witness: "class TimelinePanel final", member: "TimelinePanel panel_;" },
      preview: { witness: "class PreviewLogic final", member: "auto session = PreviewLogic::create(" },
      transport: { witness: "class PreviewSession", member: "vng::editor::preview::PreviewSession transport_;" },
      delivery: { witness: "class PreviewDeliveryLogic", member: "PreviewDeliveryLogic updates_;" },
      mailbox: { witness: "class PreviewMailbox", member: "PreviewMailbox frames_;" },
      screen: { witness: "class Screen", member: "ui::Screen screen{theme};" },
      "ui-draw": { witness: "class UiRenderer final", member: "auto renderer = render::make_ui_renderer(device);" },
      "shell-source": { members: ["ToolbarRow main_bar{", "ToolbarRow scene_bar{", "SettingsPanel settings_panel{settings_host};",
        "SaveSceneDialog save_dialog{dialog_host};", "ImportDialog import_dialog{import_host};", "OpenSceneDialog open_dialog{open_host};"] }
    },
    worker: {
      "worker-host": { witness: "int main(int argc, char** argv)" },
      "worker-endpoint": { witness: "class WorkerEndpoint", member: "auto endpoint = preview::WorkerEndpoint::attach(" },
      "gl-session": { witness: "class GlfwOpenGLSession final", member: "auto session = example::GlfwOpenGLSession::create(",
        memberOf: "int run(preview::WorkerEndpoint& endpoint, int control_fd, vng::u64 generation)" },
      worker: { witness: "class Worker final", member: "Worker worker{endpoint, session->device(), session->window(), generation};",
        memberOf: "int run(preview::WorkerEndpoint& endpoint, int control_fd, vng::u64 generation)" },
      "worker-state": { witness: "struct State", member: "std::optional<project::State> state_;" },
      inbox: { witness: "class ViewportInbox", member: "project::ViewportInbox view_inbox_;" },
      runtime: { witness: "struct Runtime::Impl", member: "std::optional<project::Runtime> runtime_;" },
      programs: { witness: "class MeshPrograms", member: "MeshPrograms mesh_programs;" },
      "mesh-resources": { witness: "struct MeshResource", member: "std::map<BlueprintId, MeshResource> meshes;" },
      "blueprint-renderer": { witness: "class BlueprintMeshRenderer final", member: "BlueprintMeshRenderer renderer;" },
      effects: { members: ["example::sun::SunRenderer sun;", "std::optional<Starfield> stars;",
        "std::optional<PortalHaze> portal_haze;", "std::optional<SceneAnnotationRenderer> annotations;"] },
      postprocess: { members: ["opengl::RenderTarget hdr, soft;", "example::sun::SunSoftening softening;",
        "opengl::Bloom bloom;", "example::DisplaySurface display;"] },
      readback: { witness: "class Rgba8ReadbackQueue final", member: "std::optional<opengl::Rgba8ReadbackQueue> readback_queue;" },
      samples: { witness: "class SceneSamples", member: "SceneSamples samples;" }
    }
  };
  const flatten = nodes => nodes.flatMap(n => [n, ...flatten(n.children || [])]);
  for (const [scope, root] of [["editor", editor], ["worker", worker]]) {
    for (const item of flatten([root])) Object.assign(item, declarations[scope][item.id]);
  }
  const guide = flatten(window.VNG_GUIDE);
  const catalog = guide.find(n => n.id === "code-components");
  const targets = new Map(guide.filter(n => n.component).map(n => [n.component.target, n.id]));
  const modules = node("modules-root", "Engine & application modules", "CMake targets", "Group",
    "A module catalog; links are dependencies, not ownership.",
    "Branches organize real build targets by responsibility. They do not imply C++ object ownership or that dependencies form a tree. Select a target to follow its exact direct dependencies and reverse users, with PUBLIC / PRIVATE / INTERFACE scopes.", "CMakeLists.txt", { children: catalog.children.map(group => ({
      id: `modules-${group.id}`, title: group.title, symbol: "Module family", role: "Group", edge: "group",
      summary: group.summary, detail: group.description.join(" "), sources: ["CMakeLists.txt"],
      children: group.children.map(component => ({
        id: component.id, title: component.title, symbol: component.component.target,
        role: component.kind === "Backend" || component.kind === "Integration" ? "Platform" : "Module",
        edge: "group", summary: component.summary, detail: component.description.join(" "),
        sources: component.links.map(link => link.href.replace(/^\.\.\/\.\.\//, "")),
        component: component.component, children: [], guide: `codebase.html#${component.id}`,
        references: component.component.dependencies.map(dep => ({
          scope: "modules", id: targets.get(dep.target), label: `${dep.visibility} · ${dep.target}`
        }))
      }))
    })) });
  window.VNG_ARCHITECTURE_DECLARATIONS = declarations;
  window.VNG_ARCHITECTURE = [
    { id: "editor", title: "Editor", subtitle: "UI-process ownership", root: editor, expanded: ["editor-host", "workspace"], guide: "../editor_components.md" },
    { id: "worker", title: "Preview worker", subtitle: "Worker-process ownership", root: worker, expanded: ["worker-host", "worker"], guide: "../editor_boundaries.md" },
    { id: "modules", title: "Engine modules", subtitle: "Grouped modules · not ownership", root: modules, expanded: ["modules-root"], guide: "codebase.html" }
  ];
})();
