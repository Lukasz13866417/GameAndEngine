# Editing-session API

`editor_example::EditingSession` is the editor application's authoring boundary.
It belongs to `vng_editor_project`, not the OpenGL backend or the reusable engine
UI library. Its public declaration is in
[`editing_session.hpp`](../examples/editor/editing_session.hpp).

## Ownership and dependencies

Scene-pose authoring requires explicit intent: `select_keyframe(time)` chooses
the active key and `select_keyframe(std::nullopt)` clears it. The application
synchronizes this with the timeline panel's active selection; the session's
`can_edit_scene_pose()` also checks that playback is paused at that existing key.
Construction and successful scene loading start with no selection, including at
time zero. `add_keyframe(time)` selects the inserted (or already existing) key.
This intent is local to the UI process, not saved content or a worker request.
Editor-camera navigation, blueprint geometry and document-wide tools do not
require a selected animation key.

```text
Editor application
├── EditingSession
│   ├── State
│   │   ├── Document: blueprints, drafts, instances, animation
│   │   └── ViewportState: private navigation and inspection
│   ├── undo/redo history and saved-content identity
│   ├── one active gesture or native-edit reservation
│   ├── clipboard values
│   └── SceneFile: path and persistence
├── UI panels and interaction tools
├── PreviewUpdates and worker transport
└── presented image and its camera/identity metadata
```

This is an **ownership tree**, not a promise that all C++ includes form a tree.
Tools explicitly borrow the session when they need to edit. The session emits
value descriptions of changes; the parent application supplies them to preview
synchronization. There are no widget, IPC, OpenGL, worker-process or presentation
dependencies inside the session.

`state()` returns `const State&`. Authored data cannot be mutated through it.
`viewport()` returns `ViewportState&` for local navigation, selection and
scrubbing. Private navigation does not modify document revision, history or
saved status. The viewport delivery layer owns its independent sequence.

## Gestures

One captured interaction consists of begin, zero or more updates, and either
commit or cancel:

```cpp
EditingSession editing{std::move(initial_state)};

// On press; include selected IDs for group translation.
auto started = editing.begin_move(ship_id, selected_ids);

// On motion: absolute position of the primary instance.
auto moved = editing.move(next_position);

// On release.
auto committed = editing.commit();

// Or, instead of commit, on Escape/lost capture:
// auto cancelled = editing.cancel();
```

These operations return `content::Result<T>`, the existing
`std::expected<T, content::Diagnostic>` alias. Check each result before continuing;
`Result<bool>` distinguishes failure, successful no-op, and successful change.

The other gesture pairs are:

| Begin | Update |
| --- | --- |
| `begin_rotation(object, selection, pivot)` | `rotate(primary_euler_degrees)` |
| `begin_scale(object)` | `scale(uniform_scale)` |
| `begin_vertices(blueprint, vertex_ids)` | `vertices(absolute_positions)` or `move_vertices(delta_from_start)` |
| `begin_world_bounds()` | `world_bounds({minimum, maximum})` |

Rotation defaults to a shared geometric selection center. `TransformPivot`
chooses `PivotMode::selection`, `individual`, or `custom` with a world-space point.
Shared/custom rotation changes positions and orientations in one sparse property
transaction, preserving the group's shape. Individual mode holds each geometric
center fixed. `begin_attitude` takes the same pivot; a group uses the active ship's
body axis, whereas individual mode uses each ship's own declared axis.

`vertices` accepts `{index, position}` entries for exactly the captured selection;
duplicate or unrelated IDs are rejected. The shared `ComponentTransform` viewport
tool computes G/R/S and gizmo results from frozen selected points. `MeshTransform`
adapts these to blueprint-local vertices and `RegionEditor` adapts them to
instance-local boundary points. Neither tool owns document history or IPC.
Whole-blueprint rotation/scaling instead uses `begin_mesh_transform(blueprint)`
and `mesh_transform(matrix)`: its scope captures only placement matrices, never
vertex arrays. It shares commit/cancel/undo with the other gestures.

Only one gesture or native edit may be active. Updates are immediate CPU edits,
not requests waiting for the worker. Each changed update publishes a revision
with the exact affected properties/vertices. Commit creates one undo entry for
the whole gesture. Cancel restores original values, tracks and mesh-draft
presence and publishes a compensating update without creating an undo entry.
Returning to the starting value restores the original track, including absence
of a key at the playhead; it does not leave a redundant animation key behind.

Gesture snapshots retain only touched values/tracks/vertices. They do not copy
scene geometry for transform/camera motion. Beginning a vertex gesture does not
create a draft until actual movement. The first edit creates a private mesh
draft; subsequent movements patch selected vertices in that draft.

`Document::world_bounds` is a scene-owned axis-aligned extent. Its explicit change
flag and optional patch value carry six coordinates through previews and history;
it is not encoded as a fake instance or an animated property. UI-side cuboid
projection/hit-testing owns no scene state and depends only on a camera snapshot
and these bounds. Visibility is an editor-only overlay choice. Saving persists
the authored corners; neither framing the box nor hiding it changes content.

Instance/property and scene-camera gestures require an explicitly selected,
paused keyframe; `EditingSession::can_edit_scene_pose()` exposes that rule to UI
tools. Native callbacks have the same gate. Time zero is a permanent initial
pose, even with no explicit tracks, but must still be selected. Between keys
and after the last key, insert a pose before
editing. Blueprint geometry, world bounds and private navigation are independent.

The playhead and paused state must stay fixed during a gesture. The session
rejects updates if they change; cancellation remains available. The UI disables
conflicting controls while captured. The current private camera, any camera
visit (Inspect/Enter) and the view mode remain independent of Undo/Redo.
`set_camera(id, pose)` writes an editor pose into a scene camera and
`set_active_camera(id)` makes it the simulation's camera from the selected
keyframe on, clearing the others there; both are single undoable edits and,
like gestures, `set_transform` and keyframe edits, respect the timeline track
budget. Scene cameras are the only simulation camera: there is no separate
camera gesture.

## Discrete edits

All these methods provide their own validation, history and change notices:

```cpp
auto imported = editing.import_mesh(path);       // new instance ID
auto instance = editing.instantiate(blueprint);  // shared blueprint, new instance
auto erased = editing.erase_instances(ids);
auto transformed = editing.set_transform(id, rotation, scale);

auto moved = editing.translate_vertices(blueprint, vertices, delta);
auto divided = editing.mesh_operation(blueprint, MeshOperation::subdivide,
                                     vertices, edges);
auto applied = editing.apply_mesh(blueprint);

auto added = editing.add_keyframe(time);
auto changed = editing.update_keyframe(from, to, name, values);
auto removed = editing.erase_keyframes(times);
auto resized = editing.duration(seconds);
```

`add_keyframe` captures the evaluated pose without changing the animation at
that instant. Zero cannot be retimed/unkeyed; `erase_keyframes` skips zero and
returns an unchanged result if it is the only selection. `update_keyframe`
accepts a field delta: the keyframe panel need not resend unchanged properties.

Mesh operations are `fill`, `subdivide` and `align`. Apply publishes a mesh draft
to scene instances without saving it to disk. Save persists the published mesh
and the draft separately, without applying the draft. Structural operations
(import, topology, instance creation/deletion, Apply) may use document snapshots.
One-shot timeline operations currently stage a candidate document; history and
delivery still retain only their changed animation values/markers. Continuous
gesture updates do not use this candidate-copy path.

`copy_instances(ids)` / `copy_keyframes(times)` store clipboard values locally.
`paste()` returns the pasted kind and identities and creates one undo entry.

## History, persistence and delivery

```cpp
auto undone = editing.undo();
auto redone = editing.redo();
auto saved = editing.save();
auto saved_elsewhere = editing.save_as(path); // no replacement by default
auto loaded = editing.load(path);

if (auto notice = editing.take_changes()) {
    preview_updates.changed(notice->changes);
    // notice->revision identifies the resulting document.
    // notice->discontinuous requests presentation resynchronization if needed.
}
```

`can_undo()`, `can_redo()`, `busy()`, `active(kind)`, `active_object()`, `dirty()`
and `path()` are queries. `take_changes()` drains the accumulated notice; it
preserves exact property/vertex identities across multiple edits before draining.
Structural changes explicitly request full synchronization. Preview transport
chooses wire encoding/coalescing; the session never sends messages itself.

History retains at most 64 entries. Document revisions only increase, including
undo, redo and compensating cancellation. Dirty status instead follows a separate
content identity, so undoing to the saved version becomes clean even though its
transport revision is newer. No-op/cancelled gestures preserve redo. Save marks
the content saved only after successful persistence; Load starts a fresh history
and clipboard. Instance/blueprint identity allocation does not rewind on Undo.

## Native inspector callbacks

`instance_controls.hpp` translates stamped inspector events into typed local
transform operations. The session itself does not know inspector widgets/events.

Custom effect controls still execute in the isolated worker:

1. `begin_remote(generation)` reserves the current document revision.
2. The application sends the callback event. The reservation leaves private
   viewport state independent, although the UI currently pauses interactions
   while waiting for acknowledgement.
3. `accept_remote(generation, patch)` validates and atomically adopts its result.
4. On transport failure, timeout or worker replacement, call `abandon_remote()`.

Acceptance requires the reserved generation, matching base revision, the exact
next revision, valid property targets/types/ranges and valid animation tracks.
Stale or malformed proposals do not mutate the document/history. Native callback
proposals currently support property edits only; mesh/topology/publication and
timeline metadata use the explicit session methods above. A no-op callback can
advance the protocol acknowledgement revision without making the document dirty.

## Verification

Session tests exercise saved identity, no-op/return-to-start gestures, cancellation,
sparse undo, draft creation/publication, animation tracks, viewport independence,
native callback rejection, compound-transform rollback, monotonic IDs and bounded
history. Gesture, timeline, file and UI tests use the same session boundary.
The real two-process walkthrough additionally covers import, gizmos, mesh drafts,
scene-camera visits and saves, keyframe/inspector refresh, Save and Load.
