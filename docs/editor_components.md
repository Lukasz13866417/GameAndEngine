# Editor components: downward control and parent-polled results

Start with `examples/editor/workspace_ui.hpp`. A **situation** describes one of a
component's fixed contexts, not an input event. The second argument carries
named data and input for that call. There is no global `TreeInfo`, component
registry, service locator, Visitor class or required base class.

Typed situations organize **editor internals**, not every user-authored
component. Dispatch is optional: a parent that knows the concrete situation
calls its child's private overload directly. Runtime alternatives can use the
dispatch helper. Gizmos have ordinary public methods; their parents choose which
one to run. See "Gizmos and typed movement bindings" below.

The coordination rule is **parents initiate actions; children expose facts or
owned proposals that their parents inspect**. A proposal returned from `handle`
or collected by `take_edit()` is a response to a parent call, not an upward
callback. The parent applies domain changes and supplies acceptance/failure back
down. A child does not reach through a sibling or an ancestor to change it.

## Naming

Names distinguish control-only behavior from components responsible for UI:

| Role | Naming | Examples |
| --- | --- | --- |
| Control/input/delivery behavior without UI ownership | `Logic` suffix | `CameraPointerLogic`, `CameraWalkLogic`, `SelectionInputLogic`, `PreviewLogic`, `PreviewDeliveryLogic` |
| An owning UI branch whose role would otherwise be ambiguous | `UI` suffix | `EditingWorkspaceUI`, `EditingViewportUI`, `MeshEditingUI`, `MeshToolsUI`, `TimelineEditingUI`, `ViewportToolsUI` |
| Visible interaction or controls | A specific UI noun | `SceneMoveGizmo`, `InstanceTransformGizmo`, `InstanceRotationGizmo`, `MeshTransformGizmo`, `MeshOperationControls`, `GizmoControls` |

UI ownership includes child panels, menus, tool-option declarations and gizmo
graphics, not just directly stored buttons. A `UI` component can coordinate its
children and own local behavior; it is not restricted to drawing. Existing clear
UI names such as `TimelinePanel`, `MeshMenu`, `RegionEditor` and `SceneLists`
stay unchanged. Domain/data/resource names such as `EditingSession`,
`WorkspaceSelection`, `PreviewMailbox` and `CameraPose` retain their concrete
nouns: having methods does not by itself make something a control component.

Headers and implementation filenames follow the component names. These are
ordinary C++ names, not new marker interfaces or runtime layers. Diagnostic
paths and serialized scene/protocol identifiers remain stable.

## Ownership

```text
Application run() [host controller]
├── EditingWorkspaceUI [UI owner + coordination]
│   ├── EditingSession [domain]    document, drafts, transactions, history, files
│   ├── WorkspaceSelection [state] shared scene-instance selection authority
│   ├── WorkspacePanels [UI]       sidebar widgets, splitters, tabs, inspector
│   ├── WorldBoundsPanel [UI]      toolbar toggle and numeric boundary controls
│   ├── EditingViewportUI
│   │   │                         [UI owner: viewport authoring branch]
│   │   ├── MeshEditingUI [UI owner + local edit coordination]
│   │   │   ├── MeshToolsUI [UI + local component-selection/cache state]
│   │   │   │   └── MeshMenu [UI]  vertices / edges / faces / inactive
│   │   │   └── MeshOperationControls [UI + local operation-options state]
│   │   ├── MeshNavigationControls [UI] centered-camera / bake controls
│   │   ├── ViewportCameraUI [UI]   camera menu, camera actions and private visit
│   │   │   ├── CameraPreferences [UI] editable settings draft
│   │   │   └── CameraPanel [UI]   Enter / Inspect / Back / Save / Set active
│   │   ├── ToolPanel [UI]         retained active-tool options
│   │   ├── GizmoSelector / RotationPivotControls [UI + local tool choices]
│   │   ├── BlueprintMeshPanel [controller + UI + owned CPU job]
│   │   └── ViewportToolsUI [UI tools + input arbitration, incl. camera vs object tools]
│   │       ├── CameraGizmo [UI + camera-child input arbitration]
│   │       │   ├── CameraPointerGizmo × 5 [Orbit / Look / Pan / Forward / Zoom]
│   │       │   │   └── CameraPointerLogic [gesture math + capture]
│   │       │   └── CameraWalkGizmo [Walk controls + CameraWalkLogic]
│   │       ├── GizmoControls [UI + sensitivity / held-key state]
│   │       ├── SelectionInputLogic [logic: deferred clicks]
│   │       ├── SceneMoveGizmo [interaction + gizmo graphics]
│   │       ├── InstanceTransformGizmo / InstanceRotationGizmo / MeshTransformGizmo
│   │       │                     [controller + gizmo graphics]
│   │       ├── RegionEditor [controller + UI + boundary tools]
│   │       ├── SurfacePartTool [controller + gizmo graphics]
│   │       └── bounds / pivot / box selection / picking helpers
│   ├── SceneLists [UI + catalog presentation]
│   └── TimelineEditingUI [UI owner + local inspector/focus state]
│       └── TimelinePanel [UI]     strip, key list, field drafts, range menu
├── PreviewLogic [logic]
│   ├── PreviewSession [platform] build and worker processes / transport
│   ├── PreviewDeliveryLogic [logic] ordered revisions, narrow coalescing
│   ├── PreviewMailbox [presentation state] completed-frame acceptance
│   └── candidate workers / schemas / submitted views / extents [local state]
└── windows, Screens, toolbars/dialogs and DisplaySurface [UI / platform]
```

These are responsibility and ownership edges, **not screen rectangles**.
A controller is invisible coordination; UI means visible presentation plus its
local interaction; domain means authoring rules/data; platform means windows,
workers or rendering realization. A gizmo naturally combines behavior and
graphics and need not be split into artificial controller/view classes.

`EditingWorkspaceUI` constructs its session before the UI children.
`create_panels` constructs the sidebar; `initialize_panels` wires its children
and toolbar slots once those slots exist. `initialize_camera` creates the
viewport-owned menu and camera overlay in their respective screens. The
lower-level attachment methods also allow isolated component tests.
Child tools receive `const EditingSession&`
for explicit observation; they do not receive a mutable session or a generic
mutation callback. Workspace operations and parent-side proposal executors are
the authoring boundary. The host schedules ordered input steps and supplies
window/preview facts; the workspace coordinates navigation, tools and picking.
This is not a hidden node registry.

### What the application calls

The host supplies window/preview facts, not per-widget instructions:

```cpp
workspace.layout_panels(layout, screen_size);
// Once per frame, from frame-start facts: a gesture that begins during this
// input frame changes panel availability from the next frame.
workspace.present_panels({modal, pending_play, playing, inspector_ready, gesture});
workspace.present_pose_controls(frame); // later in the frame, after selection/schema changes

// These calls also coordinate the dependent panels and tools.
workspace.select_scene_instance(id, SelectionMode::replace);
auto result = workspace.shortcut(EditShortcut::paste);
auto cancelled = workspace.cancel_interaction();

// Read-only observations for rendering and automation, not mutation handles.
const auto& tools = workspace.interaction();
```

The workspace owns selection consequences: labels, timeline focus, keyboard
action target, stale-inspector invalidation and gizmo refresh. It also owns
copy/paste/delete/undo consequences and gesture completion/rollback. Its replies
flag a finished gesture (`pose_finished`); the host then shows the worker's latest
schema, since schemas that arrive during a capture are deliberately not shown. Camera
visits, menu drafts and restoring the pre-visit view live below the viewport.
Saving preferences to disk and forwarding accepted worker schemas remain
application responsibilities.

Implementation is split by responsibility, not by fragments of the host loop:

- `workspace_panels.*`: widget construction, layout, tabs and reversible splitter drafts.
- `viewport_camera_ui.*`: camera widgets, preference drafts and local visit state.
- `workspace_ui_panels.cpp`: panel eligibility, selection presentation and inspector polling.
- `workspace_ui_camera.cpp`: camera visits and authoring camera-panel actions.
- `workspace_ui_tools.cpp`: tool lifecycle, options and overlay composition.
- `workspace_ui_commands.cpp`: selection/view commands, clipboard, deletion and history consequences.
- `workspace_ui_viewport.cpp` / `workspace_ui_picking.cpp`: ordered input arbitration and picking.

`app.cpp` still schedules windows, UI input frames, preview delivery, file
dialogs and GPU presentation. It can inspect live children through **const**
accessors, but cannot cancel a child tool, change its gizmo selection or modify
its widgets directly. There is no parallel mutable ownership tree.

SceneLists owns both docked lists and flyouts. `WorkspaceSelection` is the shared
scene-instance selection authority, not a second selection inside each list.
Mesh component selection and timeline-key selection remain separate local
concepts. A pop-out changes presentation, not document ownership. Low-level
retained UI utilities keep their ordinary APIs.

## A real root-to-leaf path

The app chooses a workspace situation from the existing `ViewMode`; that enum
remains the file/protocol representation. This shared entry-point mapping still
uses dispatch to resolve the runtime alternative once. Situations are ephemeral,
not a second stored editor mode.

```cpp
auto reply = dispatch(workspace, workspace_situation(view_state),
    WorkspaceContext{.mesh = MeshInput{
        .menu_events = events,
        .poll_menu = true
    }});
```

For `InspectMesh`, the relevant calls descend through these owners (excerpts;
the actual methods also check which context sections were supplied):

```cpp
// EditingWorkspaceUI → EditingViewportUI
viewport_->handle(s, ViewportEditingContext{
    c.mesh, c.accept_input, c.presentation, c.poll_navigation, c.tools, transitioned
});

// EditingViewportUI → MeshEditingUI
mesh_.handle(s, MeshEditingContext{
    c.mesh.value_or(MeshInput{}), c.accept_input
});

// MeshEditingUI uses its private MeshToolsUI. That owner knows the selection mode.
// Both opening and polling the menu use this one local decision:
if (mode_ == MeshSelectMode::vertex)
    return menu_.handle(MeshMenu::Vertices{selected_.size()}, context);
if (mode_ == MeshSelectMode::edge)
    return menu_.handle(MeshMenu::Edges{selected_.size()}, context);
if (mode_ == MeshSelectMode::face)
    return menu_.handle(MeshMenu::Faces{selected_.size()}, context);
return menu_.handle(MeshMenu::Inactive{}, context);
```

The leaf declares its allowed contexts and keeps handlers private:

```cpp
class MeshMenu final {
public:
    struct Vertices { std::size_t selected{}; };
    struct Edges    { std::size_t selected{}; };
    struct Faces    { std::size_t selected{}; };
    struct Inactive {};
    // Construction, open/close, and read-only observations omitted here.
private:
    friend class MeshToolsUI;
    std::optional<MeshAction> handle(const Vertices&, const MeshMenuContext&);
    std::optional<MeshAction> handle(const Edges&, const MeshMenuContext&);
    std::optional<MeshAction> handle(const Faces&, const MeshMenuContext&);
    std::optional<MeshAction> handle(const Inactive&, const MeshMenuContext&);
};
```

Only `MeshToolsUI` can call `menu.handle(...)`. It does not construct a menu variant
just to unpack it again. Other internal children likewise friend their immediate
owner: EditingViewportUI friends EditingWorkspaceUI; MeshEditingUI, camera controls
and tool options friend EditingViewportUI; lists and timeline friend
EditingWorkspaceUI. They neither
include their parents nor retain parent pointers. Their private handlers cannot
be reached via the generic dispatcher either.

Already-resolved situations flow downward by ordinary overload resolution;
descendants do not repeat the root's mode switch. Parents branch only on facts
they own or receive, including narrow read-only observations such as whether
walk navigation is armed. The workspace chooses timeline availability; the viewport chooses
new tool options versus updating the current options.

Gizmos do not need that internal-UI handler boilerplate. `CameraGizmo` exposes
`present`, `poll` and `update`. Its parent, `ViewportToolsUI`, supplies the
target pose, input and eligibility (`Navigate`) and the presentation, to which it
adds whether one of its own object gizmos is shown. The gizmo returns a proposed
pose and preference edits. `EditingWorkspaceUI` never holds the gizmo: it passes
facts it owns (target label, camera-action overlay, settings) through that
parent's narrow API — `present_camera`, `poll_camera`, `select_camera`,
`resume_object_tools`, `object_tools_suspended` and a const `camera_gizmo()`
observation. It owns five
`CameraPointerGizmo` children and one `CameraWalkGizmo`. Children own their
capture/motion helpers and mode-local widgets; they do not find or message other
components. The gizmo routes a captured occurrence to its owner, or chooses a
child from the active mode/shortcut. Menu input never reaches navigation.

The camera gizmo is the fallback when no object gizmo is visible. Explicit mode
selection borrows LMB from object tools; `ViewportToolsUI` then suspends them in
`accepts()`, and the workspace skips object input, picking and overlays. It lasts
until an Escape nothing else claimed, **Object tools**, an explicit selection, or
anything hiding the menu. The host decides whether a text field, popup or flyout
claimed this frame's Escape (`escape_claimed`). Ordinary camera shortcuts
can temporarily borrow input during object transforms. Editor and entered scene
cameras share the same `NavigationFrame` pose contract. Camera visits, explicit
Save and document authority remain at their existing parent boundary.

Use `dispatch` where a runtime alternative actually needs unpacking, as at the
workspace entry. Its concrete overload also remains available at the existing
application entry boundaries (interaction, scene movement and preview delivery)
which still friend Dispatcher. That compatibility does **not** require every
descendant to do so. `component_dispatch.hpp` contains the generic `std::visit`;
missing handlers are compile-time errors and reference results stay references.
No Visitor, registration mechanism or new wrapper is needed for direct calls.

The menu returns a **local action**, not a success claim. MeshEditingUI turns it
into an owned `MeshEditProposal` (blueprint, operation, vertex and edge IDs).
EditingViewportUI returns that result to its caller; `EditingWorkspaceUI::execute`
invokes `editing_.mesh_operation(...)`, then calls the viewport's
`accept_operation(request, result)`. MeshEditingUI updates its selection and
operation options only in response to that accepted result.

Thus the full scope belongs to the workspace, but mesh hit-testing, menu options,
selection repair and operation presentation stay below it. The leaf knows
nothing about worker delivery or unrelated panels.

## A concrete asynchronous blueprint edit

Earth clouds and infrastructure use the same parent-polled rule. The host calls
the workspace-owned panel, then asks the workspace to process its outstanding
values:

```cpp
auto& panel = workspace.blueprint_panel();
(void)panel.poll(accept_input);
const auto result = workspace.apply_pending(panel);
// Inspect result before reporting an authored change.
```

Inside `EditingWorkspaceUI::apply_pending(BlueprintMeshPanel&)`, the essential
calls are:

```cpp
while (auto proposal = panel.take_edit()) {
    auto result = apply_blueprint_mesh_edit(editing_, *proposal);
    panel.accept_edit(*proposal, result);
}
```

The actual implementation accumulates changes and preserves the first error,
while draining cancellation/commit proposals generated by acknowledgement.
`BlueprintMeshPanel` owns its CPU future and an immutable source snapshot; the
job returns an `EditableMesh`, never captures a mutable session, and never changes
the document. The completed result becomes a `BlueprintMeshEdit` containing its
blueprint, source revision and owned mesh. Only the workspace adopts it.

During a drag, the panel keeps one running job/result and one newest queued
destination. Parent adoption precedes launching that next destination, so the
next result is stamped against the accepted document revision. Successful
completion produces one history entry for the entire gesture. Cancellation
discards pending computation without waiting for its CPU future. The spinner
also waits for the corresponding rendered revision, not merely job completion.

`RegionEditor` similarly exposes `take_edit()` and `accept_edit(...)`.
`RegionEdit` contains a replacement boundary or sparse point values, target,
revision and gesture phases. Its parent applies it through `apply_region_edit`;
the child performs topology-selection repair and successful-operation messages
only after acknowledgement.

### Transaction identity is not a document revision

`EditingSession::active_transaction()` identifies one successful gesture begin.
It changes for every new capture, even if the new capture edits the same target
and has not changed a single value. `active_blueprint()` identifies mesh-draft,
mesh-transform and vertex transactions' blueprint.

Instance/mesh/rotation and blueprint/region proposals carry the acknowledged
transaction identity for continuations. Parent-side executors reject a proposal
that no longer owns that capture. This prevents a delayed cancel/commit from
ending a newer gesture on the same object. Async mesh adoption additionally
checks blueprint and source revision. Neither an occurrence being consumed nor
a proposal being produced implies a successful authored operation.

## Timing and selective propagation

WorkspaceContext has named optional sections: mesh, lists, timeline, tools, and
viewport presentation/navigation. Absent means **no work for this branch**, not
"clear state". Mode transitions still notify the viewport so mesh menus cannot
remain active in Scene mode. Context spans are synchronous borrows, never stored.
Owners retain widgets/caches and small presentation facts, not input batches or
document snapshots.

- Handlers update retained widgets and poll after `Screen::update()`. They do
  not reconstruct the UI tree.
- Input order is preserved: `2`, `A`, `H` in one batch switches to edges, selects
  them, then hides adjacent faces.
- TimelineEditingUI proposes keyframe actions. The workspace applies them,
  acknowledges the result to the timeline child and updates authoring permission
  without waiting for preview completion.
- If its inspector tab is hidden, TimelineEditingUI retains the latest object
  focus request. The parent supplies visibility when showing the tab; only then
  does the child reveal/focus the row. It never stores obsolete widget geometry.
- Viewport presentation does not refresh list catalogs or timeline rows.
  Catalog synchronization and selection-only changes are explicit separate work.
- CameraGizmo returns a proposed pose. It does not author a simulation
  camera or increment document revisions. Blocking input, focus loss and any
  Escape release held keys and drags. Of these, only an unclaimed Escape also
  ends an explicitly chosen mode.
- PreviewLogic keeps reliable document/visibility packets separate from
  replaceable viewport requests. It never waits for a frame or ACK. Failed
  sends preserve/reset the appropriate delivery state for retry.

File formats, draft/Apply/Save semantics and the worker protocol are unchanged.

### Input occurrences and priority

The window coordinator calls `input::EventSequence::identify` before passing
events to UI surfaces. `Event::routing_id` identifies the **occurrence**, not its
contents. Copies and coordinate conversions retain this ID. Two identical wheel
events are still distinct; one being consumed must not consume or resurrect the
other. `input::AvailableEvents` indexes the occurrences left unhandled by the
previous UI/tool stage. Tools build one lookup per update instead of searching
the remaining vector by partial event equality for each event.

Standalone callers and internal held-key ticks may supply zero-ID events; those
use complete event equality as a fallback and cannot distinguish completely
identical occurrences. Native window input is identified by its owner first.

`ViewportInputSteps` (`viewport_input.hpp`) gives the viewport one received
occurrence at a time, with the pointer/key/focus snapshot for that step. Each
occurrence finishes its navigation → tools → picking descent before the next
starts. `ViewportToolsUI::begin_step()` resets routing consumption without
discarding capture. The final empty tick advances held controls exactly once;
`GizmoControls` event/tick phases keep arrow motion independent of batch size.

For each step, the host calls `EditingWorkspaceUI::interact_viewport` with a
`ViewportInputContext`: borrowed input, presented camera/image metadata,
navigation preferences and eligibility facts. `workspace_ui_viewport.cpp` owns
navigation/tool priority, proposals, transactions and immediate gizmo state.
The returned `ViewportInputReply` tells the host what to present or send through
the native preview path; it contains no callable mutation capability.

`EditingWorkspaceUI::pick_viewport(ViewportSelectionContext)` similarly owns
scene/mesh/part picking, rectangle selection, socket/placement interactions and
mesh-context-menu opening. Region menus are handled by `interact_viewport`.
`ViewportPickCapture` is stored under the viewport,
not in the host loop. `refresh_viewport_gizmos` updates passive handles after
selection changes. The host does not decide how to pick a tunnel socket or finish
a mesh/instance transform.

UI occurrence flags belong to `Screen::update`, not each raw event step.
Region controls and pivot controls poll those flags on the final tick; their
pointer tools still run once per occurrence. This prevents one Add region click
from becoming several creations when a frame contains multiple input events.

Priority remains local to viewport arbitration. New gestures require available
input; captured gestures continue observing release/focus-loss from the ordered
raw stream. Pointer capture, selected handle, keyboard focus and document
transaction ownership remain separate. Camera navigation can borrow input while
a gizmo stays displayed. Consuming one event does not suppress drawing or discard
the rest of a frame. Mesh shortcut routing executes each proposed operation
before delivering the next shortcut, so subdivision followed by select-all sees
the new topology.

Worker observations preserve order too. `PreviewLogic::poll()` collects a
batch without applying its entire lifecycle at once. Each `take_event()` applies
one event's delivery/candidate/schema consequences and returns an `Observation`
containing the event and whether its schema was successfully updated. The host
uses that validated schema without decoding it again and handles the observation
before the next is taken. A startup failure cannot be processed
before the host has handled its preceding candidate-start event; a later resync
cannot accidentally be overwritten by an earlier native-edit acknowledgement.

## Gizmos and typed movement bindings

### Definition and ownership

A **gizmo is a target-specific editing interaction, including its presentation
and controls**. It is not just an arrow or ring. It may draw handles, vertex
markers, an anchored UI menu, a corner panel, or a composition of these. The
intent is that scene content and editor-only targets can expose appropriate
editing interactions through this same idea.

A blueprint defines how its content can be edited, including custom behavior
for its instances and subparts. This is not restricted to an editor-defined
list of position/rotation/scale capabilities. Earth cloud formations, tunnel
endpoints and tunnel sockets can have their own interactions and menus. A
subpart or vertex marker does not need to become a separate blueprint instance
just to be editable. Editor-only targets such as the editor camera and world
bounds likewise do not need fake persisted scene instances.

The editing owner binds the live interaction to the appropriate target or
compatible selection and applies its edit proposals. Selecting several objects
does not require running several competing gizmos: one interaction can operate
on their selection. Blueprint-defined editing behavior and ownership of the
live editor interaction are distinct responsibilities.

**A gizmo can own other gizmos.** `CameraGizmo` is a concrete example: it owns
Orbit, Look, Pan, Forward/back, Optical zoom and Walk children, plus a small
mode-selection menu. Each child owns its local controls; walk-speed controls
appear while walking. The same navigation interface operates on the editor
camera or an entered scene camera. The camera gizmo is the fallback when no
object gizmo is visible, and familiar camera shortcuts can temporarily borrow
input while another gizmo remains displayed. This composition does not require
a universal gizmo base class or a generic gizmo registry.

The parent chooses which gizmo is active and supplies its target, context and
input. Ordinary gizmo authors implement the active interaction through normal
methods, not mandatory `Active`/`Inactive` situations, visitors or dispatcher
friends. When switching targets or tools, the parent cancels any unfinished
gesture. Local idle/dragging state still belongs to the gizmo. A child exposes
results for its parent to inspect; it does not reach into siblings to mutate
the document or other UI.

### Reuse through a local binding

The editor can supply drawing/input helpers or whole reusable gizmos. A typed
binding between a particular gizmo and target defines what a property such as
"position" means for that pairing. Custom menu-only or surface-editing gizmos
are not required to invent a position property merely to be hosted.

The reusable axis gizmo is `MoveGizmo<Movement>`, in
`examples/editor/move_gizmo.hpp`. It is a plain `template<class Movement>`;
the binding is an ordinary type with the following members, not a concept or
a required base class:

```cpp
struct InstanceMovement {
    struct Target { vng::u32 object; vng::Vec3 position; };
    struct Edit   { vng::u32 object; vng::Vec3 position; };

    static vng::Vec3 world_position(const Target& target) {
        return target.position;
    }
    static Edit move_to(const Target& target, vng::Vec3 destination) {
        return {target.object, destination};
    }
};
```

These are static operations on a synchronously borrowed, const target. `Edit`
is an owned value, not a reference. Neither operation mutates the target or
starts a transaction. For private model data, friend the **binding type**, not
the generic gizmo; no global `getPosition<T>` customization is involved. Multiple
bindings can interpret the same target differently, such as a tunnel's two ends.

The semantic contract is explicit: `world_position` returns the movement anchor
in world coordinates; `move_to` proposes an absolute world-space destination for
that anchor. The binding converts coordinates/constraints and defines what the
edit means. These meanings live in the implementation and documentation, not a
semantic concept hierarchy. Missing/incompatible methods produce ordinary
template compilation errors where they are used.

Actual bindings:

- `InstanceMovement`: evaluated instance origin → an identified position edit.
  The session applies the primary position and preserves selected-instance offsets.
- `RotationOriginMovement`: custom rotation cursor → a private viewport point.
  It never authors scene data or creates history entries.
- `SurfaceAltitudeMovement`: world-space arrow → clamped local radius, through
  the blueprint frame. Earth endpoints and elevated infrastructure use this path.

Gizmo authors and callers use a direct API:

```cpp
MoveGizmo<InstanceMovement> gizmo;
auto reply = gizmo.update(target, context);
gizmo.append(draw_list);

// When the parent switches tools or targets:
auto stopped = gizmo.cancel();
// If stopped.cancelled, the owner rolls back the outstanding edit.
```

There are no `Active`/`Inactive` situation types or dispatcher friends on
`MoveGizmo`. The parent calls `update`/`append` only for its selected gizmo.
`cancel()` clears handles and capture without requiring a target, camera or
input batch. It returns a cancellation only for an unfinished gesture; repeated
cleanup is safe. The parent owns activation, not a flag duplicated in each gizmo.

Inside the editor, `SceneMoveGizmo` still has meaningful instance/native/hidden
contexts. It routes these internally, then calls its gizmo's public methods.
Blueprint authors do not implement this host's situation protocol. The scene
path is:

```cpp
// Workspace → dispatch(SceneMoveGizmo) → gizmo.update(target, context).
auto reply = dispatch(movement,
    SceneMoveGizmo::Instance{{instance_id, evaluated_position}, axes},
    MoveGizmoContext{stamp, camera, viewport, input, raw, can_begin});

// The existing transaction owner performs these at the gesture boundaries.
// Production code checks every result and rolls back on failure.
if (reply.began) editing.begin_move(instance_id, selected_instances);
if (reply.edit) editing.apply(*reply.edit);
if (reply.cancelled) editing.cancel();
else if (reply.finished) editing.commit();
```

`EditingSession::apply(InstanceMovement::Edit)` rejects mismatched active objects.
The session retains validation, keyframe permissions, one-gesture undo and narrow
preview changes. A proposal alone does not change the document or publish a draft.
`SceneMoveGizmo` captures only the evaluated position, stamp and axes during a drag;
its own preview revisions cannot change that baseline. Ordinary instance movement
does not wait for worker schemas/ACKs, although viewport validity and edit
permissions still apply.

`can_begin` controls new gestures, not activation. An active gizmo can remain
visible while the camera borrows input. Existing capture can receive release
events over UI, and camera changes rebase the drag without changing its target.
Context/target references are never retained. The owner supplies a stable baseline
while a gesture is open; a target/generation change cancels that gesture.
Gizmo diagnostics report local idle/interacting state, not a guessed activation
decision belonging to the parent.

The binding adds no stored provider, vtable, type-erased callback or runtime
lookup. Its calls are ordinary static calls that can inline. Input, rendering,
diagnostics and authored edits still have their normal costs. Runtime-native
inspector callbacks remain an explicitly separate compatibility path, not a
claim that dynamic/custom tools have no dispatch overhead. The raw TranslationTool
accepts a typed `TranslationTarget`; only its native adapter parses a schema.

This implements the movement contract, not a mandatory universal gizmo base.
Rotation/scale, cloud surface dragging, menu-only tools and other custom behaviors
retain their existing implementations; they can reuse components without being
forced through a fictitious position property.

## Diagnostics

Open **Logs → Component diagnostics**. Clicking it refreshes a snapshot; reports
are not rebuilt every frame. Automation exposes the same on-demand callback.

`DebugReport` owns its strings and separates received facts, owned state,
observed results/counters, and explicitly gathered child reports. `debug_string()`
formats paths such as `editor.workspace.viewport.mesh.components.menu`.
Collection polls no futures, contacts no worker, changes no revisions and triggers
no UI work. Situation labels describe the last dispatch/local state, not a
guessed global mode. Submitted packets are not reported as presented or GPU-done.

## Integration boundaries

`app.cpp` assembles window/UI layout and toolbar/dialog behavior, presents worker
frames, schedules input steps, and supplies host facts to the workspace. The
workspace owns navigation/manipulation/picking behavior and invokes its domain
session; the host handles returned presentation and IPC observations without
lending a mutable session to panel children. Runtime-native inspector edits
still cross the existing stamped worker
protocol; they are not C++ callbacks to another process's widgets.

The ownership tree is not a universal `Node` framework. Concrete types, ordinary
methods, polled values and optional typed situations remain the API. Further
extraction should move coherent state and behavior together, not wrap all app
locals in a giant context or route every action through a generic callback.
Resource/math utilities need no artificial situations for uniformity.

Regression coverage includes `component_dispatch_tests.cpp` (private/exhaustive
dispatch), `movement_binding_tests.cpp` (friend access, coordinate semantics,
capture, typed proposals, native compatibility and transaction boundaries),
`workspace_tests.cpp` (real menu/keyframe clicks, draft-only edits,
stale/denied input, narrow propagation and focus), and the executable editor E2E
suites for integrated selection, Earth editing and popout behavior.
`input_routing_tests.cpp` distinguishes consumed lookalike events and converted
coordinates. `workspace_picking_tests.cpp` exercises scene and mesh selection
through the workspace owner. `viewport_input_tests.cpp` covers owned
navigation/tool integration. `blueprint_mesh_panel_tests.cpp` and `region_ui_tests.cpp` verify
that polling alone does not author, stale completions are rejected and a parent
acknowledges successful edits. Transform tests cover replaced-transaction cleanup.
