# Region blueprints and scene manipulation

Regions are ordinary scene instances of the built-in **Region / TODO volume**
blueprint. Each instance owns its own boundary, note, visibility, wall-grid setting and transform.
Editing one boundary never reshapes another instance. Vertices, edges and faces
are internal geometry, not scene entities or additional blueprint instances.

## Using regions

In **View: Scene**:

1. Add **Region / TODO volume** from the blueprint list, or use **Add region**
   to choose a box, tetrahedron, octahedron or triangular prism and radius.
   The latter places the region in front of the viewing camera.
2. Select it in the **Region instances** list or click its outline in the viewport.
   Move, Rotate and Scale operate on the instance transform, including ordinary
   multiple-instance selection. Copy/paste, Delete and undo/redo work normally.
   Hover the viewport and press **G / R / S** to transform the whole selected
   region (or selection of instances) with the mouse. **LMB / Enter** confirms;
   **RMB / Escape** restores the original transforms. **X / Y / Z** constrains
   move/rotation to a world axis, or scale to an instance-local axis. Plain **S**
   scales uniformly. This changes the instance
   transform, not its local boundary. Rotation uses the normal **Pivot** setting.
3. Choose **Region vertices**, **Region edges** or **Region faces** in the normal
   **Gizmo** dropdown. Ctrl+Left/Right cycles these modes too (either Ctrl key).
   When multiple regions are selected, component tools edit the active region;
   whole-instance tools operate on the complete selection. Mixed selections
   offer only common tools.
4. Click a component and drag its XYZ handle. Shift/Ctrl-click extends the
   component selection. **Previous vertex / Next vertex** switch the active
   vertex without creating or selecting another scene instance.
   Drag a rectangle from empty space for box selection (Shift adds, Ctrl removes).
   Empty clicks clear components without deselecting the region. **Boundary gizmo**
   selects Move/Rotate/Scale handles. **G/R/S** starts a mouse-following transform
   without holding a button; **X/Y/Z** constrains it, **Enter/LMB** confirms and
   **Escape/RMB** cancels. Rotation/scaling use the selected component centroid.
5. The region inspector edits its name and TODO note; in component mode its XYZ
   fields edit the selected boundary points in world coordinates.
   **Apply region** applies those edits. **Show walls (grid)** immediately toggles
   an opaque line grid on this instance's polygon faces (off by default).
   This setting supports undo/redo, copy/paste and scene saving. Ctrl+S saves the scene.
6. RMB over the viewport while a region is selected opens its geometry menu.

Boundary edits remain in the scene editor, never the mesh editor. Instance
transforms follow the normal timeline/keyframe editing rules; boundary topology
and notes are static authored data, editable while paused. The visibility
checkbox hides the annotation overlay without deleting data. Regions are editor
annotations, not opaque render meshes, and do not appear in independent Play.

## Geometry tools

The selected-region RMB menu provides:

- Vertex, edge and face selection modes, plus select-all and clear-selection.
- **Add vertex** at the selection center.
- **Make edge / face**: two selected vertices form an edge; three or more form
  a polygon in selection order.
- **Subdivide**: selected edges gain shared midpoints; selected faces split
  into quads while preserving adjacent boundaries.
- **Align to line** through the first two selected vertices, keeping those
  anchors fixed.
- **Delete selected components**, or **Delete entire region**.

Every geometry command or completed drag is one undo entry. Escape/focus loss
cancels a drag. Dents, concave/nonplanar polygons and open intermediate shapes
are allowed. These are authoring cages, not validated watertight collision
volumes; no inside/outside or self-intersection guarantees are provided.

The preview worker draws region edges, optional face grids and world bounds
against the scene's depth buffer: meshes and the sun occlude them. Concave
polygon grids are clipped to each face outline. Grids use opaque lines, not
translucent surfaces. Local editing handles and selected-component highlights
remain X-ray overlays so interaction stays responsive and accessible.

The Scene sidebar has independently scrolling **Scene instances**, **Region
instances**, **Blueprints**, **Manipulation** and **Create region** sections.
Drag the grip between any adjacent sections to resize them; Escape cancels a
resize. These UI proportions do not change the scene or its undo history.

Limits remain 128 region instances, 1–1024 vertices per region, 4096 faces /
16384 face corners and 4096-byte notes. Invalid topology, nonfinite coordinates
and oversized payloads are rejected before commit.

## Ownership and API

```cpp
auto id = session.instantiate(BlueprintId::region);
// Or choose a primitive and placement:
auto other = session.add_region(RegionShape::prism, {0, 0, -8}, 2);

// Read this instance's local geometry:
const RegionSettings* settings = region_settings(session.state(), *id);
const RegionGeometry& boundary = settings->boundary;
```

```text
EditingSession
└── Document.instances
    └── SceneInstance (blueprint = region)
        ├── identity + ordinary InstanceTransform
        └── RegionSettings
            ├── local boundary: points / faces / loose edges
            ├── note
            ├── visible
            └── show_walls

Editor application
├── ordinary instance selection + GizmoSelector
└── ViewportInteraction (capture, priority, cancellation)
    └── RegionEditor (boundary inspector and RMB actions)
        └── CageTool (cached display cages and component selection)
            └── ComponentTransform (shared with mesh editing)
                ├── TranslationTool
                ├── RotationTool
                └── ScaleTool
```

There is no second persistent region registry. The blueprint supplies an initial
shape; instantiation and copy/paste own independent geometry by value.
`region_snapshot(state)` projects authored local boundaries into inspection
values. `region_world_snapshot(state)` evaluates instance transforms for the
cages. `region_to_local` converts a world-space manipulation back into the
selected instance's local boundary. These snapshots do not own scene identity.

Point gestures explicitly select their scope:

```cpp
session.begin_region_points(instance_id, selected_point_ids);
session.region_points(new_local_positions); // span<RegionPointEdit>, {index, position}
session.commit();                           // or cancel()
```

Undo/redo, preview coalescing and wire patches retain the instance and point IDs.
Point updates leave topology and unrelated boundaries untouched. Topology or
name/note/wall-style editing uses `begin_region(id)` and `region(replacement)`, storing only
that instance's boundary. Adding/removing instances remains a structural edit.
Patch format v7 adds the wall flag to whole-region replacements; versions 1–6
remain readable, with walls disabled for old data.

The display follows the same scopes: point edits update cached world positions
and the center handle; topology changes replace one cage. Input-only frames do
not resupply/copy cages. Structural changes and playback time changes refresh the
visible set. Blueprint manipulation capabilities choose boundary tools through
the same adapter used by the ordinary Gizmo selector.

`Region::scene_cage()` supplies the backend-neutral `vng::editor::SceneCage`
description: object identity, labeled control points, edges and polygon faces.
`CageTool` knows nothing about regions, documents, blueprints, IPC or OpenGL.
It only picks/draws cages and reports gestures. `RegionEditor` interprets them
through `EditingSession::begin_region / region / commit / cancel`.
The session owns validation, history and persistence.

`scene_annotation_lines(state, time)` produces neutral `SceneLine` values from
sampled instance transforms and the world bounds. The worker's `Runtime` owns
`SceneAnnotationRenderer` (explicitly an `opengl::Renderer<SceneLine>`), which
batches these lines and retains its GPU buffer while geometry is unchanged.
It draws after bloom/tone mapping into the final color target, borrowing the
original scene depth attachment. Camera navigation only changes the shader's
view-projection argument. Editing handles remain local UI geometry; no depth
image is sent over IPC. `RenderRequest::annotations` enables this for debug
preview only, and private viewport flags control region/world-bound visibility.

Creation/deletion uses ordinary structural scene updates. Boundary changes use
the bounded region payload in document patches; transform edits use normal
property patches. Camera navigation does not rebuild boundary snapshots.
New native scenes use project version 3. Old separate-region collections migrate
to fresh scene instance IDs, preserving world geometry without colliding with
existing mesh IDs. Point-only legacy boundaries acquire explicit faces once on
load; authored open topology never gets re-hulled.

Tests cover independent instance boundaries, copy/paste, transforms and inverse
mapping, ordinary selection capabilities, compact boundary patches, undo/redo,
migration, save/reload, component picking and the real editor RMB workflow.
