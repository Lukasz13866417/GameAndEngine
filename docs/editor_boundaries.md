# Editor state and interaction timing

The editor still has two processes. UI/document authoring runs in the editor;
effect callbacks, playback sampling and OpenGL rendering run on the worker's
context-owning thread. This refactor does not add a render graph or simulation thread.

## Ownership

| Owner | Data | Changes when |
| --- | --- | --- |
| `Document` | Applied blueprints, separate mesh drafts, instances (including scene cameras), timeline, document revision | Authored content is edited |
| `ViewportState` | Private camera target, selected object/vertex, inspected blueprint, playhead/play state, view sequence | Navigation, selection, scrubbing, or inspection changes |
| `Runtime` | Per-blueprint GPU resources, render targets, shader realizations | Explicit resource invalidation or target resize |
| Presented frame | Pixels, generation/frame/document/view IDs, actual camera pose, sampled time, interaction trace | A completed worker frame is accepted |

`State` assembles `document` and `viewport`; they do not inherit from each other.
`EditingSession` owns that state, edit transactions, undo/redo, clipboard, file
identity and saved status. Tools read a const state and submit typed edits. Its
`EditNotice` values feed preview delivery without introducing a transport or
rendering dependency. See the [editing-session API](editing_session.md).
Undo stores scoped values or structural document snapshots and a small inspection
bookmark, not historical navigation requests. The current private camera, any
camera visit and the view mode survive Undo.
Existing saved scenes still load. Their `view` section is workspace metadata;
the private camera and transport sequence are excluded from scene-file output.

`keyframes.hpp` defines the pose-editing rule: paused at a named or property-key
timestamp, with zero always present as the initial pose. `EditingSession` checks
it before property/camera gestures and native callbacks; the UI uses the same
predicate for controls and gizmos. Loading a sparse timeline never materializes
its implicit zero properties. Pose insertion samples current values before
changing any track. Blueprint/global tools do not acquire a timeline dependency.

The keyframe panel owns difference filtering and unsent drafts. It compares
sampled values with the preceding global keyframe, adds currently selected
instances, and retains groups with drafts. Selection reaches it as object IDs,
not a document mutation or worker request. Apply submits only changed fields.

Mesh drafts are working copies keyed by blueprint ID, created on the first edit.
Scene instances resolve only applied geometry. Isolated mesh inspection resolves
the draft if present. Apply publishes it in memory; Save persists both versions
separately without publishing. Draft creation records original touched vertices
and draft presence; Apply uses a structural snapshot. Creation/removal of a draft
requires a full preview update, while subsequent edits retain compact patches.
A patch targets its blueprint's
working copy even if the viewport has since switched to Scene; the worker skips
GPU updates for hidden drafts. Switching between applied/draft inspection refreshes
the affected realization, while ordinary camera navigation uploads nothing.

## Blueprint mesh authoring

`BlueprintMeshPanel` owns a local `InspectorPanel`, the blueprint's control
declaration, part selection, and at most one background CPU edit. `describe_blueprint_mesh()`
routes explicit asset identity to a typed blueprint declaration; the app contains
no Earth-specific widgets or parameter handling. Scene instance controls remain
on the existing worker inspector path; mesh recipe controls don't require a scene
keyframe or the worker to be available.

A declaration submits `MeshDraftEdit{label, apply}`. The job sees an owned mesh
snapshot, never an `EditingSession`, widget, graphics object or mutable shared
document. Completion calls `EditingSession::replace_mesh_draft(blueprint,
expected_revision, mesh)`. An intervening edit or inspection change rejects the
old result; a successful replacement is one undoable draft-only edit. Existing
Apply and Save operations retain their separate meanings. There is no work per
slider tick and no application-wide recipe cache.

Draft completion compares only that blueprint, producing `editor::MeshChanges`:
changed vertex IDs per attribute, changed metadata keys, or a whole-mesh marker
when topology/schema changed. The same scope drives undo, preview coalescing and
`DocumentPatch::MeshDraft`. Its bounded binary `editor::MeshPatch` carries values
and draft membership, so even the first edit and undo back to no draft avoid a
whole-scene snapshot or text serialization. These transport types do not change
the `.vmesh` or `.vscene` file formats.

The worker applies the patch atomically to that blueprint. Hidden drafts need no
GPU update. For a visible draft with unchanged topology, the runtime updates only
the changed ranges in the vertex and surface-attribute streams; normals do not
force a full upload. Topology changes replace that blueprint's realization.
Mesh picking indices are built lazily when actually queried, not for every
temporary history/transport mesh. Blueprint controls do not reset component
selection or topology caches after a position-only edit.

Declarations may return named `MeshPart`s: stable, blueprint-local IDs, not scene
instances. Selecting one re-declares its inspector; it does not edit the document.
Earth uses this for surface-constrained movement of whole cloud formations.
Declarations can name a UInt32 per-vertex `part_field` for picking. In Surface
mode, `BlueprintMeshPanel` queries the mesh's shared triangle BVH and maps the
nearest visible triangle to a part. List and viewport selection share one local
ID; neither changes the document, rebuilds geometry, nor contacts the worker.

A selected part can additionally declare `MeshPartGizmo{surface, move}`.
`SurfaceMove` is a spherical constraint, anchor, label and optional affine frame; `move(position)`
produces the same CPU-only `MeshDraftEdit` used by inspector controls.
`ViewportInteraction` owns the reusable `SurfaceMoveTool` (hit testing, pointer
capture, immediate handle geometry), not the blueprint recipe. The application
only routes its actions to `BlueprintMeshPanel`.

The panel owns the drag-start snapshot and a bounded async queue: one running
job plus one latest target. Every job uses the same starting geometry, preventing
sampling-rate-dependent placement drift. `EditingSession::begin_mesh_draft_edit`
and `preview_mesh_draft` publish narrow preview patches; commit compacts the
original snapshot to touched fields for one undo entry. Cancel restores those
fields, and late asynchronous results are drained without being adopted or
blocking the UI. The loading indicator also tracks the target image revision,
so it lasts through worker rendering/transfer, not only CPU preparation.

Whole mesh mode uses the same `ComponentTransform` gizmos with a single pivot,
not a giant synthetic vertex selection. `MeshTransform` composes the gesture's
affine matrix with a captured placement through `begin_mesh_transform` and
`mesh_transform`. `Document::mesh_placements` stores applied and optional draft
matrices. Its explicit change scope and versioned patch carry only those matrices
through history and IPC; the worker does not rebuild/upload geometry for them.
Rendering, picking, component coordinates and cloud handles share the placement.
Apply publishes the draft; file save/export bakes a copy, including inverse-
transpose normals and the procedural `editor/mesh-frame`. The live mesh and
its shared picking index stay unchanged. Cloud formations are not scene instances.

Earth declares coverage, puff/spiral size, altitude, relief and visibility. Its
CPU-only asset library replaces tagged cloud geometry while preserving terrain.
Formation movement and height-only edits retain the existing topology, while
footprint changes sample each formation's bounded support rather than repeatedly
sampling the entire globe. Explicit mixed-layer topology is rejected. Recipe values live in `.vmesh`
metadata, including embedded drafts. Existing assets have a generator-provenance
migration path. See [Earth](earth.md) for user controls and limits.

## Effect preset ownership

`ImportDialog` only browses paths; `import_kind()` shares extension recognition
with `EditingSession::import_asset()`. The session atomically installs an imported
blueprint and its first instance, with one undo entry. `EffectBlueprint` owns the
named defaults; each `SceneInstance` owns its independent settings and transform.
`Document::effect_assets` owns the imported blueprints, including unused ones.

`.veffect` uses the existing `content::Document` reader and `vscene 1.0` grammar,
tagged with `effect_preset = 1` and `type = "sun"`. `effect_presets.hpp` handles
validation, decoding, and export. Unknown effect types fail without changing the
scene. Adding a new implementation still requires compiled effect support; preset
import is not a code/plugin loader. Scene files and preview snapshots embed the
defaults rather than retaining a dependency on an external preset file. Rendering
continues through the existing Sun renderer, with no graphics backend dependencies
in the preset model or loader.

## Two delivery policies

### Blueprint rendering batches

Every visible mesh instance produces a backend-neutral `mesh_shading::MeshDraw`
ticket (transform, lighting values, wireframe choice). The runtime groups these by
blueprint and calls that blueprint's `BlueprintMeshRenderer::render(frame, view,
tickets)` once. Each renderer derives from `opengl::Renderer<MeshDraw>` and owns
one GPU mesh plus reusable instance-stream buffers. It explicitly borrows the
runtime-owned shared shader program; the program has a stable address and outlives
all blueprint renderers. Geometry edits still patch the one shared mesh.

Transform columns, brightness, eye and light values are semantic per-instance
records, not uniforms changed between individual draws. One solid batch is one
`glDrawElementsInstanced`, regardless of ticket count. The renderer partitions
solid and wireframe tickets because polygon mode is fixed-function state; mixed
groups use two draws, still through one renderer call. This opaque-mesh policy is
not a promise that arbitrary transparent effects can be reordered the same way.

The reusable backend API is `opengl::InstanceBuffer<Record>::update(device, records)`
and `commands.draw(mesh, instances)`. `gfx::make_instance_buffer(device, records)`
selects the realization from its device. Shader semantics infer vertex bindings
and divisor-one delivery; callers provide neither locations nor VAOs. Buffers grow
geometrically, and VAOs survive buffer growth/rebinding without duplicating geometry.
Instance data updates are transient draw preparation, not document mutations.

`RuntimeStats::last_mesh_renderer_calls`, `last_mesh_draw_calls`, and
`last_mesh_instances` expose the distinction (also included in worker statistics).
The isolated diagnostic capture uses
the same shading helper but remains a separate selected-object pass. Suns receive
one ticket-span submission to their effect renderer; their existing multi-pass
surface/corona/strand implementation still renders each sun separately internally.

Selection sets are local UI state (`editor::Selection<Id>`), with stable identities,
an active item and a range anchor. The worker still receives only the active
inspection target; changing the set does not serialize meshes or advance the
document revision. Keyframe and component selections stay in their panels/tools.
Box capture records the presented camera and rectangle, then picks only on
release. Scene selection projects instance origins (O(instances)); mesh selection
bins projected triangles spatially for visibility tests, avoiding an all-pairs
vertex/face scan. Group translation stages only position properties/tracks and
sends the existing per-property patches. A commit, batch delete or batch paste
creates one history transaction; blueprint data is never duplicated by selection.

`MeshTools` also owns temporary hidden source-face IDs (H / Alt+H), scoped to
the inspected topology. A change-only `mesh_visibility` packet carries this mask
and its topology fingerprint, independently of document patches and the small
camera mailbox. The worker waits for its required document revision; a mismatched
topology cannot mask new faces. `Runtime` applies it only to mesh inspection,
updating resident element indices without reallocating vertices or VAOs.
Degenerate hidden triangles preserve source primitive IDs in diagnostic captures.
The local overlay and CPU picking use the same visibility rules. This state never
enters scene persistence, mesh drafts, or undo history.

### Accelerated picking and selection updates

`vng_spatial` depends only on `vng_core`. Its immutable `TriangleBvh` owns local-space
positions, triangles, and a median-split bounds tree. `EditableMesh` owns its picking
index; snapshots and instances share that immutable index. Loading prepares it once.
Changing positions invalidates only that mesh's index (rebuilt on its next query);
topology replacement constructs a new one. Camera, selection, and instance-transform
changes never rebuild geometry indices. Cache access follows the mesh's owning thread.

Scene picking transforms the ray by each instance's inverse render matrix, rejects
missed bounds, and tests only intersected tree leaves. Keeping the ray direction
unnormalized preserves camera near/far depth, including scaled instances. Exact
two-sided triangle tests retain holes, nearest-hit ordering, and back-side picking.
`PickStats` / `spatial::QueryStats` expose bounds and triangle work without a global
profiler. The remaining broad phase is a linear instance walk, not a scene-wide BVH.

The viewport also owns `InstanceProjection`: box selection and multi-selection
markers share projected origins keyed by document revision, playback time, view,
and the presented camera/extent. Idle redraws and scene-selection changes reuse
them. Projection samples only visibility and transforms; it never copies names,
appearance settings, or potentially large region boundaries. Picking likewise
does not clone the entire instance list before testing geometry.

The keyframe panel updates row visibility only after selection/filter/value
changes and row widths only after width/schema changes. Its statistics count
these passes, allowing tests to prove that idle and camera-only frames do not
refilter or resize thousands of retained fields.

`editor::Selection<Id>` combines ordered IDs with hash membership and exposes
`changes_from(previous)` (added/removed IDs and active-item changes). `InstanceList`
owns rows and an ID lookup: document synchronization updates names/additions/removals;
selection updates touch only changed membership and old/new active rows. They do
not resynchronize the instance/blueprint catalogs. Clearing an already-empty
keyframe selection does nothing. Mesh overlays retain static topology indices and
upload selected indices separately, never the entire topology just to highlight it.

Ctrl+Right / Ctrl+Left cycle the selection's available gizmos, wrapping at both
ends and including shared blueprint-specific tools. Either physical Ctrl key works.
The shortcut respects text input, open popups, modal dialogs, and active drags.
`ui::UpdateResult::capturesShortcuts` distinguishes those exclusive interactions
from ordinary button/dropdown focus; the latter does not block gizmo cycling.

### Blueprint-specific gizmos

The core inspector can augment any position gizmo with named world-space axes:

```cpp
ui.translation_gizmo("position", position, [&](vng::Vec3 next) { position = next; })
    .axis("Forward / back", world_forward)
    .axis("Lift", world_up);
```

An axis is just `{label, direction}`; it is bidirectional and need not be normalized.
There is one position field/callback, not duplicate editable position controls.
Descriptions contain no UI/backend objects; schema wire version 3 bounds and
validates axis counts, labels and finite nonzero directions. Event payloads are
unchanged. Existing `translation_gizmo` calls keep the ordinary XYZ handles.

`blueprint_gizmos.cpp` is the example's policy adapter: applied mesh orientation
metadata describes a forward axis, and the evaluated instance rotation maps it
to world space with the same `Rz * Ry * Rx` convention as rendering. Both the
worker inspector and local selection feedback call this adapter. This makes
the extra handle available on the selection/paste frame and refreshes it after
rotation or timeline scrubbing without round-trip-dependent geometry.

`TranslationTool` projects arbitrary axes through the presented camera and
inverts perspective along the captured axis during a drag. The axis is frozen
for the gesture; cancellation handles changed descriptions/context. Scene handles
submit `EditingSession::begin_move/move/commit` (or `cancel`), retaining precise
position-change tracking and compact patch delivery.
Neither motion nor navigation reuploads geometry or serializes the mesh. Scale
has no effect on the unit movement direction; bounds clip distance along the
line, so hitting a limit does not bend the drag onto another axis.

`selection_gizmos(state, selected_ids)` intersects stable gizmo capabilities over
the complete selection. `GizmoSelector` owns the sidebar menu and rebuilds it only
when this intersection changes. A supported current mode survives selection changes;
unsupported modes fall back to Move. The same capabilities filter viewport handles,
so a stale worker description cannot restore a ship-only handle on a mixed selection.

Oriented ship blueprints with perpendicular `coordinates/forward` and `coordinates/up`
also provide local yaw/pitch/roll axes. `RotationTool` owns projection and pointer
capture; `RotationInteraction` joins its captured angular delta to
`EditingSession::begin_attitude/attitude/commit` (or `cancel`). The session captures
each selected instance's evaluated orientation and blueprint basis once, then
composes each update from those originals. Rotation never accumulates Euler deltas
in this mode. The stored transform still uses the existing Euler representation,
and the renderer/worker needs no new gizmo protocol or backend objects.

Group Euler rotation, body-relative attitude, and uniform scaling edit only their
selected properties/tracks, keep instance origins fixed, and commit or cancel
atomically. Group forward translation instead preserves formation spacing with
one displacement along the active instance's forward axis. Inspector fields remain
active-instance-only. All group gestures use compact multi-property patches, not
mesh serialization or vertex uploads.

### Document and viewport transport

Document edits use `PreviewUpdates`: an ordered revision/acknowledgement lane.
`DocumentChanges` carries explicit blueprint/vertex IDs, object/property targets,
and changed timeline markers/duration through edits, history, coalescing and
transport. Single vertex/instance-transform edits retain their tiny codecs;
mixed changes use one versioned `DocumentPatch`, not a full scene snapshot.
Dirty targets accumulate while an update is in flight, so an ACK cannot discard
a later edit. Blueprint identity is independent of the currently inspected view.
Structural edits (import, instance creation/deletion, load), reconnects and
unknown changes still require snapshots.
Scene cameras are ordinary instances: their placement, lens and active flag
travel as instance property patches like any other keyed value, never embedded
meshes, and cannot overwrite the independently moving private camera. There is
no other simulation camera and no camera-specific packet. History entries also
retain change identities: undo/redo of known vertex, property and keyframe
changes sends patches, not serialized meshes.

Scene files before `editor_project = 5` also stored a document-level orbit shot
with yaw/pitch/distance/target/zoom tracks, which was the simulation camera
whenever a scene had no camera instances. `legacy_camera.hpp` is the only code
that knows that shape: the decoder asks it to read the shot and route its
tracks, and it turns a shot that was in use into an active **Animation camera**
instance that orbits, as the old camera did: between position keys its focus
point moves in a straight line and the eye turns and dollies around it. So the
old tracks carry over key for key and the path is unchanged: focus and zoom
copy distance and zoom, position has the target's keys (each eye placed around
the target), and rotation takes yaw's and pitch's key times. A cut in yaw or
pitch while the other moves keeps both via a key one millisecond before the
cut; only if those rotation keys would exceed a timeline limit are they thinned.
A scene that already had camera instances drops the unused shot, as does a
scene already at the instance limit. An eye beyond the scene's coordinate
range is clamped into it. Saving writes version 5.

Native `ProjectControls` callbacks report the properties they committed. The
worker replies with `state_patch`, updating those properties/tracks in the UI
without sending a scene or running whole-mesh reconciliation. A property patch
contains its base value and optional replacement track (absence removes that
target's track); unrelated tracks and geometry are not transmitted. Keyframe
editing identifies affected tracks and markers from the small animation tables,
never by comparing mesh data. Explicit marker removals preserve named/empty keys.
Packets validate revision, targets, types, ranges and animation constraints before
CPU mutation. GPU writes target only touched vertex ranges and roll back on error.

`ViewportRequest` is a 76-byte absolute value with its own sequence and a minimum
required document revision. `PreviewSession::send_latest()` retains one pending
replaceable value per generation. After processing input, the application calls
`flush_requests()` before drawing/presenting the UI, delivering that tick's latest
value without an extra UI-frame delay. This nonblocking operation preserves the
ordered-command lane; `poll()` retries delivery under backpressure. The worker
coalesces incoming targets and never
waits for a document ACK to apply a view whose dependencies already exist. If an
imported blueprint is not present yet, it retains just the latest request until
the corresponding document update arrives. Commands/deltas must not use this lane.

Camera-only requests do not serialize meshes, advance document revisions, mark
the document dirty, copy Undo snapshots, upload geometry, or regenerate inspector
schemas. Selection/playhead changes refresh only the relevant inspection context.
The timeline panel is keyed by document revision and inspection context, not
private camera/view sequence: navigating causes neither metadata reconstruction
nor value resampling. Saving the editor view into a **scene camera** is an authored
edit and follows the document lane instead.
Inspector event stamps include that context identity, so an old Apply cannot act
on a different selection or newly sampled value at the same document revision.

The legacy selection/playback packet codecs remain available for existing
protocol tests/tools; the editor's private interactions use the new viewport lane.
`ViewportRequest` version 5 retired the pilot-camera flag bit; it must be zero.

## Rendering and zoom

`RenderRequest` borrows the scene and explicitly supplies a camera, extent, time
and diagnostic mode. Drawing never temporarily installs its camera into authored
state. `Runtime::update_positions()` consumes an explicit blueprint + touched
vertex set, updates only contiguous dirty ranges, and rolls attempted writes back
if uploading fails. Full-snapshot reconciliation remains available for structural
edits, structural Undo, load and explicit resynchronization.

`CameraPose::zoom` is optical magnification (1 = the original 43-degree lens).
It changes projection, not camera position. `distance` is the eye-to-orbit-pivot
distance, and `target` locates that pivot. **Scroll moves camera** switches the
wheel and Ctrl+middle-drag from optical zoom to translation of eye and pivot
along the viewing direction, without changing either lens or orbit distance.
This routing toggle is UI-only; it is not scene data or another worker message.
Panning accounts for optical magnification to retain screen-space sensitivity.
Holding **Left Alt** boosts pan, Ctrl+middle-drag and wheel movement/zoom by
4×. Orbit rotation is unchanged; Right Alt/AltGr does not activate the boost.
The modifier is sampled with each input event, including pointer and wheel events.

**Selected: gizmo only**, in the second toolbar, hides the active scene instance's
surface while retaining its selection and gizmos. Other instances remain visible;
with multiple selections this applies to the active instance, not the entire group.
The private viewport flag travels in the lightweight view packet, without editing
visibility tracks, rebuilding assets, or creating Undo entries. Hidden surfaces
also stop intercepting scene ray picks. Independent Play and scene-file output
ignore this option; hiding a Sun's surface still preserves its light on other objects.

`ViewportInteraction` owns `CameraWalk` alongside pointer navigation. The tool
owns only walk mode and held keys; it receives a pose, input eligibility, elapsed
time and `WalkSpeeds`. It yields pose changes through the same session boundary
as other camera navigation, so a held walk gesture is one authored Undo step.
Typing/modal input clears held keys; focus loss and Escape exit. Integration
caps a stalled update at 100 ms and normalizes simultaneous directional input.
During walk mode, pointer navigation turns in place instead of orbiting.

Maximum viewing distance and walk speeds live in `Settings`, never the document.
The existing settings channel supplies the far plane to the worker. Completed
frames carry that plane with their actual pose, so picking and gizmos reconstruct
the same projection as the rendered image. Both preview and independent Play
use the setting without mesh reloads or authored revisions.

Wheel input immediately updates the requested pose. Worker-local `ZoomMotion`
approaches optical magnification and distance exponentially in log-space, and
position linearly, with a 55 ms time constant and a
small finite settling tolerance. This is independent of frame rate: paused
previews continue drawing until the target settles. Orbit, pan, direct numeric
edits and view switches snap rather than acquiring smoothing lag. Smoothing
does not create document edits or additional input samples.

Input precision and visual smoothing are separate. The Linux GLFW adapter reads
XInput 2.1 scroll valuators, normalizes their fractional units, and removes the
duplicated legacy wheel events. Navigation consumes every nonzero fraction; it
does not wait for a whole wheel step. See the [backend note](../cmake/glfw/README.md).

The preview runtime owns a `SceneSamples` cache. Camera, selection, viewport
extent and geometry are not animation-sampling dependencies. Unchanged authored
instance records reuse their sampled values at the same playback time; changed
instances alone are resampled. A changed time or timeline content token resamples
the scene. `Timeline::version()` is a process-local content token that survives
copies and changes on mutation, not a serialized scene revision. This also handles
direct edits in demos and tools without relying on callers to bump a revision.
Lighting first collects visible suns, then searches only those per mesh; a scene
without suns performs no per-mesh light-candidate scans.

GPU meshes cache vertex-input configuration. Warm draws reuse it; instanced draws
rebind just the instance storage, preserving formats and divisors across buffer
growth, moves, and alternating solid/wireframe batches.

Every completed image contains the camera actually used, not a later target.
UI picking, vertex projections and gizmos use that camera. Document/view identity
checks prevent an old blueprint image from being used to edit a different asset.
Live color readback uses a context-owned `opengl::Rgba8ReadbackQueue`: three
persistently mapped pixel-pack buffers and zero-timeout fence polling. Rendering
submits a transfer and returns; the worker drains completed transfers separately,
including while paused. Editable preview permits one in-flight frame so another
expensive CPU render cannot delay delivery of an already-rendered view; input
continues to coalesce meanwhile. Independent Play can pipeline the debug queue.
A full queue skips submission rather than growing or waiting. Only the newest
completed image is copied to CPU memory. Its document,
camera, view sequence, time and diagnostics were captured at submission, never
reconstructed from later state. Resize, debug disconnect and Play-mode switches
invalidate pending results without waiting for their GPU work.

Both sides try-lock the shared image mutex. `WorkerEndpoint::try_publish()`
returns false on contention; the worker retains one latest completed image and
retries it, so an isolated final edit is not lost. CPU copies and UI texture
uploads remain; this is not zero-copy GPU sharing or a hard real-time guarantee
for driver calls. Diagnostic/evidence captures and explicit screenshot readbacks
remain synchronous. Independent Play with Debug link off avoids preview transfer.

## Viewport interaction ownership

Presentation may be docked or detached without changing this ownership tree.
The application has a controls `ui::Screen` and viewport base/popup layers
(image/caption, then mesh/region context menus). When docked, panel-unhandled input feeds
the viewport; an open viewport menu gets priority. The image composes below panels
and the popup layer above them. When detached, each surface consumes its own window's coordinates,
focus, and releases. Only the focused surface contributes document shortcuts.
There is no synthetic enlarged desktop canvas and no second editing session.

`ViewportWindow` is an optional presentation child. It owns its window/context,
UI renderer, mesh overlay and display target, and explicitly restores the controls
context after creation, drawing, capture and destruction. CPU preview snapshots
can be consumed by either renderer; GL resources never cross contexts. Closing
this window docks the viewport. Independent Play remains a separate worker-owned
window and does not replace this presentation child.

`ViewportInteraction` is the application's single owner of navigation, boundary,
world-bounds, translation, rotation, scale, and selection-box tools. Its explicit
`EditingSession&` is the only document/history dependency. Tools do not query or
cancel their siblings.

At each UI tick, `begin_frame()` clears consumption from the previous input
batch. `update(tool, callback)` supplies whether that tool may interact: an
existing capture wins over other authoring tools, otherwise the viewport visits
navigation, instance keyboard transforms, blueprint boundary tools, bounds,
transform handles, then selection.
Camera navigation may borrow pointer input while an authoring tool retains its
transaction. `GizmoInput`, owned by `ViewportInteraction`, strips camera and
options-panel motion and accumulates a sensitivity-scaled virtual pointer.
It also tracks held arrows, discards OS repeat events, and emits one normalized
arrow per held direction per tick. The explicit `arrow_step` argument carries
elapsed time × sensitivity into clock-free math tools; the backend input event
type remains unchanged. Focus loss, UI ownership, navigation and gesture end
clear held input, and a bounded time step prevents jumps after stalls.
Its tool-options description adds the shared mouse/arrow sensitivity to the active
tool's own controls. On camera changes the math tools rebase their screen-space
reference at the current transform, preserving the original document baseline
and undo entry. No tool calls a sibling or starts a second editing transaction.
Concurrent navigation uses the private editor view; while inspecting a scene
camera, navigation is blocked instead of silently editing that camera.
`MeshNavigationControls` hosts a private navigation preference for all mesh modes
and emits explicit `CameraBakeOptions` requests only in whole-mesh mode.
It supplies an optional displayed mesh center to `NavigationTool::drag_origin`;
the navigation tool freezes that reference for an MMB gesture and knows
nothing about blueprint or mesh ownership. `EditableMesh::center()` lazily caches
the vertex centroid with the geometry snapshot, shared by whole-mesh gizmos and
navigation. Placement/camera changes do not rescan vertices; geometry edits
invalidate the cache along with picking data.
`mesh_camera_bake` computes a centroid-preserving placement and compensating
private view; `bake_mesh_camera` submits the placement through one existing
`EditingSession` transaction. Rotation and optical scale are opt-in components;
camera translation is never authored into mesh data. Neither the controls nor
the math own GPU objects, file persistence, or scene publishing.
A press and release within one tick remains consumed, so it cannot accidentally
select an object underneath a gizmo. Input ownership is separate from gizmo
visibility: a possible selection rectangle can hold LMB while move/rotation/scale
geometry remains visible. Non-owning transform tools receive empty input, never
the selection press; the owning tool continues receiving raw releases even over
UI panels. Invalid context still cancels its capture. `finish()` ends the owning authoring
transaction; `cancel()` rolls it back and clears all child captures. App-level
checks now describe context eligibility (paused scene, current preview, modal
state), not combinations of sibling tools that must be idle.

`TransformGesture` owns the shared G/R/S input and camera-relative math, not
document data or history. `InstanceTransformInteraction` translates its deltas
into one `EditingSession` move/rotation/scale transaction. `ComponentTransform`
applies the same gesture to selected mesh or region points, through their existing
editing adapters. Each captures a baseline once and previews from that baseline;
mouse moves never need a worker round trip. World-space rotation deltas are
composed with each instance's original orientation before applying the selected
pivot policy. Keyboard and draggable-handle transforms share input ownership, so
confirmation cannot also click through to selection, and an idle handle cannot
commit or cancel another tool's transaction. The overall `scale` is independent
of local `axis_scale` proportions; both belong to the instance and its timeline,
not its blueprint. Axis scaling travels through ordinary property patches. Old
scenes default to `{1,1,1}` proportions, and Save persists them explicitly. Render
matrices, inverse picking/component coordinates, geometric pivots and shading
normals all use the combined scale. Instance S-axis constraints are local (no
shear); G/R and mesh/region component constraints are world-space.

`gizmo_mode.hpp` is the shared capability/shortcut description used by gizmo
menus and viewport input. Each mode specifies its label, key and optional modal
transform kind. G/R/S start ordinary transforms; F starts blueprint-axis movement;
T selects attitude rings; region 1/2/3 select a boundary component mode. Instance
shortcuts are filtered by the selection's common capabilities, resolved once on
gesture start rather than scanning the scene during pointer motion. F captures
the primary instance's evaluated forward direction, keeping group offsets intact.
The modal gesture uses the existing TranslationTool/RotationTool/ScaleTool visuals
with empty input. Only the modal adapter owns the transaction. Axis constraints
filter those visuals to one feature; scale visuals use the same local orientation
as the actual instance scaling. Component transforms reuse this path, so mesh and
region editing do not maintain a separate keyboard-only overlay implementation.

`GizmoMode::free_rotate` is a shared rotation capability, not a ship-specific
tool. `RotationTool` accepts a `RotationGizmo` with `free_rotation=true`: a
camera-facing halo and an MMB capture replace its Euler-axis ring picking.
The mouse delta composes camera-up/right turns with the captured orientation.
`RotationInteraction` passes the resulting world-space delta to the existing
`EditingSession::rotate_by` transaction; `ComponentTransform` uses the same tool
for selected mesh and region points. Both preserve the selected center and the
existing one-gesture/one-undo rule. No renderer or worker callback is involved.
The viewport disables only **starting bare-MMB camera orbit** when this capability
is selected. Modified pan/dolly, wheel navigation, and Alt+MMB camera orbit retain
their normal input path. Tools do not inspect or cancel their siblings.

### Shared tool options, separate editing responsibilities

`ToolOptions` (`examples/editor/tool_options.hpp`) is the optional menu capability
shared by gizmo gestures, mesh operations, and custom blueprint tools:

```cpp
class ToolOptions {
public:
    virtual ~ToolOptions() = default;
    virtual std::string_view title() const = 0;
    virtual bool options_available() const = 0;
    virtual void describe_options(vng::editor::Inspector&) = 0;
};
```

`ToolPanel` owns the viewport's bottom-left UI host and the temporary inspector
description. It borrows a tool owned by the editor's interaction tree; tools
must outlive the panel. The tool describes its controls and receives ordinary
inspector actions. It does not position widgets, know an OpenGL context, send
preview messages, or gain ownership of another tool's state.

For example, a blueprint-specific tool can describe its own settings using the
existing inspector API:

```cpp
void ExtrudeTool::describe_options(vng::editor::Inspector& ui) {
    ui.edit("extrusion", settings_, "Extrusion")
        .field("distance", &Settings::distance, "Distance")
        .apply("Update extrusion", [this](const Settings& next) {
            return update_extrusion(next); // Tool's own validated editing adapter.
        });
}
// The interaction owner explicitly presents its currently active tool:
tool_panel.show(extrude_tool);
```

`ExtrudeTool` above illustrates an extension, not an implemented mesh operation.
Implemented providers are `TransformGesture` and `MeshOperationTool`. G/R/S and
blueprint-forward gestures queue menu axis/confirm/cancel requests into their own
input state machine; their existing adapter remains the sole transaction owner.
`MeshOperationTool` exposes subdivision levels, alignment strength, and undo.
It delegates to `EditingSession`, which owns the captured input mesh, original
selection, revision guard, and undo checkpoint. Adjustments recompute that input
and replace the last result without pushing another checkpoint. Each accepted
adjustment still gets a fresh content identity and normal preview change notice.
An unrelated edit or history traversal invalidates the operation token, so old
options cannot overwrite newer work. No second undo stack or replayed UI events
are needed.

Menu input ownership is independent of overlay visibility. The viewport draws
its image and normal UI, then the local depth-tested mesh overlay, then tool
menus. Both docked and detached presentation use this ordering. This avoids
hiding vertices during menu interaction or painting vertices over the menu.

`blueprint_manipulation(state, blueprint)` is the one authoring adapter returning
the supported gizmos, local forward/attitude axes, and editing surface. The
gizmo selector intersects these capabilities across selected instances. The
viewport uses the same description to route internal boundary editing; the app
does not identify regions by blueprint ID. Regions remain one scene instance,
with internal component selection and independently owned instance geometry.

## Timing

Open **Logs → Interaction timing**. Capture is enabled by default; the panel
provides Clear and Export JSON. It keeps at most 256 observed interactions and
256 intervals between newly presented preview frames. Merely redrawing the same
preview image is not another interaction response.

```text
input callback received → UI mutation → send queued → worker receives
  → worker applies → render starts → render/readback finishes
  → shared pixels published → UI copies pixels → presentation call → call returns
```

All timestamps use Linux `CLOCK_MONOTONIC`, shared across these processes. An
interaction has a local ID scoped by worker generation; a completed frame carries
the originating trace. Superseded targets are not assigned fabricated latency
samples. First presentation of a frame incorporating the request is recorded
separately from the first observed frame that reaches the smoothed target.
Input events handled in one UI tick form a burst with oldest/newest timestamps
and a count. Untimed/synthetic events are explicitly marked as estimated.

The panel displays median/p95 input-to-presentation-submit latency, a breakdown
for the latest sample, target settling time, and p95 new-preview gaps (including
deliberately idle time; this is not an active-render FPS estimate). Percentiles
use nearest rank; an even-sized median averages its two middle samples. Export
contains raw integer nanosecond timestamps and all document/view/frame identities.
It creates a new `interaction-timing-<monotonic timestamp>.json` in the working
directory and reports its absolute path.

These are **CPU** timings: callback receipt is not the hardware mouse timestamp;
render/readback includes GPU waiting but is not a GPU timer query; presentation
submission/return is not compositor scheduling or monitor scanout. Screenshot
automation can itself delay presentation and is included in its measured samples.
No `glFinish`, per-frame disk logging, unbounded history or extra diagnostic render
is introduced by timing capture.

## Checks

- CPU tests cover packet validation, future-document dependencies, newest-value
  coalescing, frame-rate-independent settling, actual-camera metadata, trace
  ordering, generation isolation, bounds and synthetic timestamps.
- Worker integration covers a burst of viewport requests, settling without more
  input, unchanged document/mesh/schema counters and ordered cross-process stamps.
- Mixed-patch tests check atomic rejection, truncation, cross-blueprint coalescing,
  undo/redo target preservation and exact GPU upload byte counts. Callback tests
  assert zero scene encode/decode and zero mesh updates for appearance changes.
- Timeline UI tests repeatedly move only the private camera and verify unchanged
  `document_refreshes` and `value_refreshes`. Worker `stats` additionally exposes
  `document_patches`, `inspector_rebuilds` and `schema_sends` for regression checks.
- The actual UI walkthrough exercises wheel navigation and records timing JSON,
  alongside import, blueprint/instance editing, gizmos, timeline, save and reload.

Relevant code: `examples/editor/editing_session.hpp`, `session_operations.cpp`,
`project.hpp`, `viewport_session.hpp`,
`document_changes.hpp`, `document_patch.hpp`, `preview_updates.hpp`,
`legacy_camera.hpp`, `presented_view.hpp`, `runtime.hpp`, `timing_panel.hpp`,
`include/vng/editor/interaction_timing.hpp`, and `src/editor/preview.cpp`.
