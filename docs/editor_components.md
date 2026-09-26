# Editor components: situations, ownership, dispatch

Start with `examples/editor/workspace.hpp`. A **situation** describes one of a
component's fixed contexts, not an input event. The second argument carries
named data and input for that call. There is no global `TreeInfo`, component
registry, service locator, Visitor class or required base class.

## Ownership

```text
Editor application
├── EditingSession                 document, drafts, transactions, history, files
├── EditingWorkspace
│   ├── EditingViewport
│   │   ├── MeshEditing
│   │   │   ├── MeshTools           selection, topology cache, visibility masks
│   │   │   │   └── MeshMenu        vertices / edges / faces / inactive
│   │   │   └── MeshOperationTool   current operation's adjustable options
│   │   ├── MeshNavigationControls  centered camera and bake controls
│   │   └── ToolPanel               retained active-tool options UI
│   ├── SceneLists                 instance/region/blueprint lists and flyouts
│   └── TimelineEditing
│       └── TimelinePanel          strip, key list, inspector drafts, range menu
├── ViewportInteraction            gesture arbitration and interaction tools
│   └── CameraNavigation
│       ├── NavigationTool         pointer gestures
│       └── CameraWalk             held keys / walk input
├── PreviewSession                 build and worker processes / transport
└── PreviewDelivery
    └── PreviewUpdates             ordered revisions and narrow change coalescing
```

These are ownership edges, not a ban on borrowing. Authoring owners explicitly
receive `EditingSession&`; `PreviewDelivery` borrows `PreviewSession&`. Owners of
these dependencies outlive their borrowers. No component discovers siblings or
walks back up a parent chain.

Workspace children are private. Its mesh, timeline and options views are const,
for drawing/inspection, not alternate mutation APIs. SceneLists owns both docked
lists and flyouts: a different presentation does not create a second selection
authority. Low-level retained UI utilities keep their existing APIs.

## A real root-to-leaf path

The app chooses a workspace situation from the existing `ViewMode`; that enum
remains the file/protocol representation. Situations are ephemeral, not a second
stored editor mode.

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
// EditingWorkspace → EditingViewport
dispatch(viewport_, s, ViewportEditingContext{
    c.mesh, c.accept_input, c.presentation, c.poll_navigation, c.tools, transitioned
});

// EditingViewport → MeshEditing
dispatch(mesh_, s, MeshEditingContext{
    c.mesh.value_or(MeshInput{}), c.accept_input
});

// MeshEditing uses its private MeshTools. MeshTools chooses its menu situation.
dispatch(menu_, menu_situation(), MeshMenuContext{
    events, !visibility_.hidden_faces.empty(), true
});
```

The leaf declares its allowed contexts and keeps handlers private:

```cpp
class MeshMenu final {
public:
    struct Vertices { std::size_t selected{}; };
    struct Edges    { std::size_t selected{}; };
    struct Faces    { std::size_t selected{}; };
    struct Inactive {};
    using Situation = std::variant<Inactive, Vertices, Edges, Faces>;
    // Construction, open/close, and read-only observations omitted here.
private:
    friend struct Dispatcher;
    std::optional<MeshAction> handle(const Vertices&, const MeshMenuContext&);
    std::optional<MeshAction> handle(const Edges&, const MeshMenuContext&);
    std::optional<MeshAction> handle(const Faces&, const MeshMenuContext&);
    std::optional<MeshAction> handle(const Inactive&, const MeshMenuContext&);
};
```

`menu.handle(...)` is inaccessible to a parent. `dispatch(...)` handles both
concrete situations and runtime alternatives. `component_dispatch.hpp` contains
the single generic `std::visit`; individual components need neither visitors nor
repeated visiting lambdas. Missing handlers are compile-time errors; references
returned by handlers stay references.

The menu returns a **request**, not a success claim. MeshEditing validates and
applies it through EditingSession, then returns precise authored, selection,
visibility and message results. The app reacts; the leaf neither sends worker
messages nor redraws unrelated panels.

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
- TimelineEditing applies keyframe intents and updates authoring permission
  immediately, without waiting for preview completion.
- If its inspector tab is hidden, TimelineEditing retains the latest object
  focus request. The parent supplies visibility when showing the tab; only then
  does the child reveal/focus the row. It never stores obsolete widget geometry.
- Viewport presentation does not refresh list catalogs or timeline rows.
  Catalog synchronization and selection-only changes are explicit separate work.
- CameraNavigation returns a proposed pose. It does not author a simulation
  camera or increment document revisions. Blocking input releases held keys;
  popout focus transfer preserves armed walk mode but not held keys.
- PreviewDelivery keeps reliable document/visibility packets separate from
  replaceable viewport requests. It never waits for a frame or ACK. Failed
  sends preserve/reset the appropriate delivery state for retry.

File formats, draft/Apply/Save semantics and the worker protocol are unchanged.

## Diagnostics

Open **Logs → Component diagnostics**. Clicking it refreshes a snapshot; reports
are not rebuilt every frame. Automation exposes the same on-demand callback.

`DebugReport` owns its strings and separates received facts, owned state,
observed results/counters, and explicitly gathered child reports. `debug_string()`
formats paths such as `editor.workspace.viewport.mesh.components.menu`.
Collection polls no futures, contacts no worker, changes no revisions and triggers
no UI work. Situation labels describe the last dispatch/local state, not a
guessed global mode. Submitted packets are not reported as presented or GPU-done.

## Scope

The new boundary is used by mesh/menu operations, mesh-camera controls, tool
options, scene lists/flyouts, timeline authoring, navigation and preview delivery.
This is **not a claim that every older panel/tool has been converted**.
`app.cpp` still coordinates dialogs, toolbar controls, camera visits, native
inspector callbacks, selection and several scene-gizmo paths. Non-navigation
tools in ViewportInteraction still use their existing integration API.
BlueprintMeshPanel retains its independent asynchronous recipe-edit owner.

Further extraction should move coherent ownership and behavior, not wrap all app
locals in a giant context or route every action back through a generic callback.
Resource/math utilities need no artificial situations merely for uniformity.

Regression coverage includes `component_dispatch_tests.cpp` (private/exhaustive
dispatch), `workspace_tests.cpp` (real menu/keyframe clicks, draft-only edits,
stale/denied input, narrow propagation and focus), and the executable editor E2E
suites for integrated selection, Earth editing and popout behavior.
