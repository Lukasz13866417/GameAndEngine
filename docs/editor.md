# Mesh, effect and scene editor

This application currently targets Linux and OpenGL 4.6, with UI/text enabled.

Build and run:

```sh
cmake -S . -B build
cmake --build build --target vng_editor_demo -j 4
./build/vng_editor_demo
./build/vng_editor_demo --mesh-view
./build/vng_editor_demo --import-dialog
./build/vng_editor_demo --scene examples/assets/editor_timeline.vscene
./build/vng_editor_demo --play --no-debug-link
./build/vng_editor_demo --once --screenshot /tmp/editor-new.png
```

The initial project contains the existing procedural sun and colored cube.
The embedded viewport displays a shared-memory image from a separate worker.
**Play (independent)** shows that worker's own native window and presents directly to it,
rendering through the scene's active **Camera** instance; it refuses to start until the
scene has one. Both paths use the same engine rendering code.

See [state boundaries and interaction timing](editor_boundaries.md) for the
separate document/view request lanes, smooth wheel zoom, actual-frame camera
metadata and **Logs → Interaction timing** measurements/export.

## Using this slice

- **Box selection:** hold LMB in the viewport, drag a rectangle, then release.
  In Scene view it selects instance origins inside the rectangle; selected
  origins have orange markers. In mesh view, start in empty space (dragging an
  existing vertex/edge/face still moves the selected geometry). Vertices inside
  the rectangle are selected; edges/faces require all their vertices inside.
  Mesh visibility follows **X-ray selection**. Shift-drag adds, Ctrl-drag removes,
  and Escape cancels the rectangle. No document edit occurs during selection.
- **Multiple instances:** Shift-click adds in the viewport or selects a range in
  the instance list. Ctrl-click toggles one instance. The last selected item is
  active (`>` in the list); other selected entries show `+`. Its inspector shows
  explicitly active-instance properties. The Move gizmo moves the whole group,
  including the active ship's forward/back handle. Release creates one Undo entry;
  Escape restores every selected position/animation track. Rotation/scale gizmos
  currently require a single instance; inspector edits affect only the active one.
- **Multiple keyframes:** Shift-click selects a timestamp range, Ctrl-click
  toggles individual timestamps, in either the list or timeline markers. Selected
  markers turn gold. The inspector edits the active timestamp; **Delete selected
  keys** or Delete removes all selected timestamps as one undoable operation.
  Keyframes do not use rectangle selection.
- **Batch clipboard:** Ctrl+C/Ctrl+V copy all selected instances (shared blueprints,
  independent transforms/tracks), or all selected keyframes. Keyframe paste places
  the earliest copied timestamp at the playhead and preserves relative timing.
  Conflicting/out-of-duration destinations reject the entire paste. Instance paste
  selects all new copies. Delete removes all selected instances but keeps blueprints.

- Click **Import...** in the top toolbar. Browse folders, select a
  `.vmesh` mesh or `.veffect` effect preset and click **Import** (or press Enter). You can also paste a full
  file path. The picker initially opens the source `examples/assets` directory,
  not the build's partial demo copies. New assets such as `earth_savannah.vmesh`
  appear here without a rebuild; use **Refresh** if the dialog was already open.
  **Up** navigates to other folders. `.vscene` files are complete scenes: use
  **Open scene...** for those (the same browser, limited to `.vscene`), or import
  their standalone `.vmesh` asset into the current scene.
  Import adds a reusable mesh blueprint and one selected instance, leaving the
  existing scene objects and cameras untouched. The new mesh is framed
  in its own **Mesh: [name]** view; choose **Scene** to place its instance.
  Vertex tools open in the right-hand **Blueprint geometry** panel. Import is undoable, and invalid files leave
  the scene unchanged. OBJ/FBX/glTF are not supported.
  Effect presets create their own reusable blueprint and a selected instance,
  opening effect inspection. Try [`quiet_sun.veffect`](../examples/assets/quiet_sun.veffect).
  The current preset type is **Sun**; this loads saved settings, not new C++ code.
  Each new instance copies its blueprint defaults, so changing one does not
  change other instances. Blueprints are embedded in saved scenes: deleting the
  original preset file does not break reopening. Deleting an instance retains
  its blueprint in the library.
- In **Instances** mode, left-click a visible mesh or sun body in the viewport to
  select it and display its C++-described controls. The nearest surface wins;
  clicking empty space clears selection. The scene-list buttons also select
  objects, including ones currently hidden. Selected entries are marked in the
  list, and their translation gizmo appears in paused Scene view, including
  objects whose position is animated. The gizmo follows the evaluated position
  at the playhead, not the object's unanimated base position.
  Selection shows the gizmo in the same UI frame; it does not wait for the
  worker's inspector response and does not dirty the scene.
  Effect sliders, toggles and XYZ fields are staged until **Apply**. Instance
  scale is live while dragging; typed scale commits with Enter or **Apply**.
- In **Scene** view, drag a selected object's colored XYZ gizmo to translate it.
  Both the handle and the model move while dragging. Position updates are sent
  independently of mesh geometry; releasing commits one undo entry for the
  whole gesture. Animated positions update/create a position key at the
  playhead. Existing keys retain their incoming interpolation; new keys use
  linear interpolation. Escape restores the original position and its exact
  animation track, without creating an undo entry.
- Ships also show a longer cyan **Forward / back** pair of arrows in **Gizmo → Move**.
  Drag either arrow along the ship's nose/tail direction. It follows the instance's
  rotation (including animation at the playhead), not the camera or world Z.
  The outer arrows remain selectable when aligned with an ordinary XYZ handle.
  They use the same immediate preview, position-key editing, Escape and Undo/Redo
  as ordinary translation; shared blueprint geometry is never modified.
  Blueprints with `coordinates/forward` metadata get this handle automatically
  (`+X`, `-X`, `+Y`, `-Y`, `+Z`, `-Z`). All bundled ships already declare it, including
  copies embedded in saved scenes. Cubes/asteroids have no forward declaration.
  Set `editor/gizmos/forward = "off"` in the mesh's `info` to opt out. A handle
  pointing directly into the camera is hidden; orbit slightly to use it.
- The **Gizmo** menu is the intersection of all selected instances' capabilities.
  A ship-only selection offers **Forward / back** (just the cyan arrows) and
  **Yaw/pitch/roll**, alongside Move/Rotate/Scale. Adding an asteroid, cube,
  or other unsupported blueprint removes those custom entries and handles.
  Compatible modes are retained when selection changes; otherwise it returns to Move.
- Every entry shows its shortcut. With the pointer over the viewport, **G**, **R**,
  **S** select Move, Rotate, Scale and start mouse-following transforms. The gizmo
  stays visible; **X/Y/Z** show only the constrained feature (press the same axis
  again to restore the unconstrained gizmo). Click/Enter confirms; Escape/RMB
  restores the original values. This also works on mesh and region components.
  Instance scale constraints use local axes; move/rotate and component constraints
  use world axes.
- **Free rotate (MMB drag)** is available wherever ordinary rotation is available:
  scene instances (including mixed selections), mesh components, and region
  components. Select it from **Gizmo**, **Mesh gizmo**, or **Boundary gizmo**, then
  hold **MMB** and drag anywhere in the viewport. A cyan halo marks the rotation
  center. Horizontal/vertical motion composes camera-relative turns, including
  on already rotated objects; it does not edit individual Euler components.
  The default pivot is the selection's center; scene instances also retain the
  existing individual/custom pivot controls. Release MMB to commit one Undo step;
  **Escape**, **RMB**, or loss of focus cancels. Plain MMB belongs to this tool
  while selected; **Shift+MMB** still pans, **Ctrl+MMB** still dollies, and
  **Alt+MMB** temporarily orbits the camera. Other gizmos restore normal MMB orbit.
- **F** starts **Forward / back** for a compatible ship selection: mouse movement
  follows the active ship's forward axis, moving the whole group together. It
  shows only the cyan arrows; X/Y/Z do not replace that blueprint-defined axis.
  **T** selects the ship's yaw/pitch/roll rings for dragging. **1/2/3** select region
  vertices/edges/faces when those capabilities are common to the selection.
  Custom shortcuts are unavailable for unsupported/mixed selections. In mesh
  editing, **F** retains its existing Make edge/face action. Text fields, camera
  walk mode and active gestures keep their keyboard ownership.
- **Yaw/pitch/roll** uses blueprint `coordinates/up` and `coordinates/forward`
  metadata, not the numeric XYZ Euler coordinates: yaw turns around up (green),
  pitch around right (red), roll around forward (blue). These axes follow the
  evaluated ship orientation. Both declarations must be valid perpendicular signed
  axes. Set `editor/gizmos/attitude = "off"` to opt out. All bundled ship blueprints
  already supply the metadata, including copies in scene files.
  Rotation is composed about a captured local axis, then converted back to the
  instance's existing `Rz * Ry * Rx` representation. No blueprint geometry changes.
  By default the whole selection turns as one formation about its shared center,
  using the active ship's body axis. **Pivot → Individual centers**
  instead turns each ship about its own geometric center and declared axes.
  A drag is one atomic undo entry; Escape restores position and rotation keys.
  Forward/back instead moves the whole formation by one shared displacement
  along the active ship's forward axis, preserving formation spacing.
- Choose **Gizmo → Rotate** in the sidebar, then drag a colored ring around the
  selected scene instance. Red/green/blue edit its X/Y/Z degree fields; rings
  follow the engine's `Rz * Ry * Rx` Euler convention, including already rotated
  objects. The model updates while dragging. Release commits one Undo entry;
  Escape restores the exact rotation and animation track. Existing rotation
  animation edits a key at the paused playhead. Blueprint geometry is untouched.
  Choose **Gizmo → Move** to return to translation. Rotation currently uses the
  same ±360° limits as the numeric inspector.
  **Pivot**, just below the rotation gizmo menu, defaults to **Selection center**:
  instance centers orbit that point and orientations turn by the same world-space
  rotation. **Individual centers** rotates each in place. **Custom point** uses a
  private world-space origin; enable **Move rotation origin** to move it with normal
  XYZ handles, then disable that checkbox to use the rotation rings again.
  **Reset origin to center** resets the point. Moving it does not dirty the
  scene or create animation keys. Centers are geometric (mean stored vertices for
  a mesh, boundary vertices for a region); each instance contributes equally to
  the group center. These are not physics/density-weighted centers of mass.
- Choose **Gizmo → Scale** to drag the gold square. It scales the selected
  instance uniformly, without editing its shared blueprint. The model changes
  during the drag; release creates one Undo entry, and Escape cancels. Animated
  scale edits the key at the paused playhead, with its interpolation preserved.
  With multiple instances selected, the gizmo applies a shared scale factor about
  each instance's own origin; the inspector still edits only the active instance.
- With the pointer over the viewport, **G / R / S** starts moving, rotating or
  scaling the selected scene instances without holding a mouse button. Move the
  mouse, then **LMB / Enter** confirms; **RMB / Escape** cancels. **X / Y / Z**
  constrains movement or rotation to a world axis, or instance scale to a local
  axis; press the same axis again to release it. Unconstrained movement is in the
  viewing plane, and rotation is around the viewing direction. Rotation respects **Pivot** (selection center,
  individual centers or custom point). Plain **S** scales uniformly; **S X/Y/Z**
  changes only that local dimension on every selected instance. The **Scale**
  slider controls overall size; **Axis scale (local)** exposes these independent
  proportions as XYZ fields. Scaling uses a shared factor for the selection and
  stops when any selected instance reaches its limit. Each completed
  gesture is one Undo entry, obeys the paused-keyframe editing rules and never
  changes a blueprint. The same shortcuts transform whole regions in normal
  Move/Rotate/Scale modes; in region component modes they transform the selected
  boundary points instead. Focused text fields and camera Walk mode keep their keys.
- **Ctrl+C / Ctrl+V** copy/paste a selected scene instance (shared blueprint,
  independent transform and animation tracks), or a selected keyframe at the
  playhead. Instance paste is in place; it selects the duplicate, switches to
  Move mode and immediately draws its move gizmo from the local transform,
  without waiting for a worker frame/schema. Dragging still waits for the worker
  to accept the new instance. Move it using that gizmo.
  Keyframes paste only at empty timestamps, preserving keyed values, incoming
  interpolation and the custom name. This authoring clipboard is session-local
  and clears on Load. **Ctrl+Z** undoes; **Ctrl+Y / Ctrl+Shift+Z** redo. Focused
  text fields retain their own copy/paste and text undo/redo shortcuts.
  The bottom status bar reports **Selected N instances**, **Copied N instances**,
  and **Pasted N instances in place** after successful operations (with matching
  counts for keyframes). Failed copy/paste reports its reason instead. Rapid
  shortcuts received in one input batch execute in order, so Ctrl+C followed
  immediately by Ctrl+V does not lose the paste.
- Drag the slim scrollbar beside **Scene instances** or **Region instances** to browse that list, or
  the outer sidebar scrollbar to reach lower controls. Blueprint and inspector
  lists also show scrollbars when they overflow. Wheel scrolling stays in the
  inner list until it reaches an edge, then moves the outer panel. Scrolling is
  UI state only: it does not move the camera, select an instance, or add a scene revision.
- Drag the thin grip between the **Scene** area and **Keyframes** to divide their
  height. Grips between **Scene instances**, **Region instances**, **Blueprints**,
  **Manipulation** and **Create region** trade height between adjacent sections.
  Both sections retain a usable minimum; Escape cancels an
  active resize. Proportions stay with this editor session across tab switches,
  window resizing, UI scaling and viewport detachment, without touching the scene
  or undo history. Lists start at the top when a scene opens; selecting an
  instance explicitly can still scroll to its entry.
- **Instance list** and **Blueprint list** in the second toolbar (or its **Tools...**
  overflow) open larger, independently scrollable flyouts, like Camera settings.
  The lists remain in the right-hand **Scene** tab too; both views share the same
  selection and blueprint actions. Close with the close button, Escape, or an
  outside click. The sidebar gives these lists more height and keeps the keyframe
  list compact.
- **Delete** removes the selected keyframe or scene instance, according to the
  last selection/inspector tab. It never deletes a scene item while editing text,
  using a popup, dragging, or showing a modal dialog. Repeated key events are
  ignored. Select **Instances** mode to delete an instance; vertex deletion is
  intentionally not bound to Delete. Both deletions support Undo/Redo.
- The **Scene instances** list contains live instances and their blueprint names. **Blueprints**
  retains the mesh and sun definitions even when their last instance is deleted.
  Click the **+** beside a blueprint to create
  independent instances with new IDs, initially using identity placement for
  imported meshes (the built-in examples use their initial scene placements,
  so a new copy can overlap an existing
  one until you move it). Deletion removes the instance's animation tracks;
  recreating from the blueprint does not revive those tracks.
- The top **View** dropdown directly lists the original cube and every imported
  mesh as **Mesh: [name]**, alongside **Scene** and **Sun effect**. Pick the cube
  there even while a ship instance is selected in Scene; there is no second,
  right-panel asset selector. **Edit Mesh** and **Edit [imported name]** in the
  blueprint library remain shortcuts to those same views. Mesh entries include
  blueprints with no remaining instances; identical names get an ID suffix.
  Import, Undo/Redo and loading a scene refresh this menu automatically.
  Opening a blueprint frames its own bounds and enables vertex tools.
  The inspected asset is remembered separately from the selected scene instance:
  importing a ship cannot redirect the cube's editing target. Selecting an entry
  in **Scene instances** returns to Scene view without changing that remembered asset.
- Choose **Scene**, a named **Mesh: [name]**, or **Sun effect** to view the project or isolate an asset.
  **Middle-mouse drag** orbits, **Shift + middle drag** pans, and the **wheel** or
  **Ctrl + middle drag** moves the camera forward/backward by default: eye and
  pivot translate together, preserving zoom. Turn off **Scroll moves camera**
  in Camera settings for optical zoom instead (1 = the original 43-degree lens).
  Hold **Left Alt** for **4×** faster panning and forward/backward movement or
  optical zoom (wheel and Ctrl+middle-drag). Rotation is unaffected; Right Alt
  does not enable the boost.
  This choice is saved as an editor preference. Orbit retains the pan pivot. Opening a
  mesh blueprint centers it and resets optical zoom to 1. Gestures
  start only over the viewport and stay captured across panels until release;
  Escape or loss of focus ends them. Camera sliders remain available.
  In Scene view, **Selected: gizmo only** in the second toolbar hides the active
  instance's surface but keeps its gizmos visible—useful when moving a huge Sun
  or planet. It affects only the editor preview, not saved visibility or independent
  Play. Disable it to restore the surface; selecting another instance transfers
  the temporary hiding to that instance. Other selected instances stay visible.
  **Settings → Minimum/Maximum orbit distance** controls the Orbit distance field
  (defaults **0.01–10,000** scene units). These are editor preferences, not camera
  keyframes; older settings files automatically use the wider defaults. The camera
  adjusts near/far clipping with orbit distance. These limits do not restrict
  forward travel or optical zoom; the separate **Zoom** field accepts 0.05–1000.
  **Walk camera** enables keyboard navigation: **W/S** forward/back along the
  camera's direction (including pitch), **A/D** sideways relative to the camera,
  **E/Q** world up/down. **Shift** increases speed; **middle-drag** looks around
  in place. **Stop walking** or **Escape** exits. Typing in a field, opening a
  dialog, or losing focus releases held movement keys. Speeds are scene units
  per second, not per frame. Settings has separate forward, sideways, vertical
  speeds and a Shift multiplier. Walking the private camera never edits the scene,
  even inside an entered scene camera; only **Save this camera** authors the view.
  **Settings → Maximum viewing distance** sets the far clipping plane in the
  embedded preview and independent Play, including diagnostics. It does not
  restrict camera travel or change the world bounds. Default: 10,000 units.
  Very large distances can reduce depth-buffer precision.
  Open **Camera settings** in the second toolbar for a flyout below the button,
  available regardless of the selected sidebar tab. It groups pose, zoom, animation-camera mode, walking, viewing
  distance, orbit limits and explicitly labeled **Walk** speeds. It also provides
  independent **Shift + middle drag** pan and **Ctrl + middle drag** speed
  multipliers (default 1), plus rotation in degrees per logical pixel (default
  0.3). Ctrl-drag sensitivity also applies when optical zoom is selected.
  Pose controls remain live.
  Camera settings stay open while navigating the viewport; use **Close camera settings**
  (or Escape) to dismiss them. Navigation preferences have sliders plus typed fields;
  sliders cover everyday speeds, while fields accept larger supported values.
  **Save camera preferences** persists navigation preferences without changing
  the scene. Close with the same toolbar button, **Close camera settings**,
  or Escape. The flyout scrolls on smaller windows.
  **Play (in editor) / Pause (in editor)** controls preview animation;
  **Play (independent) / Stop (independent)** controls the native playback window.
- **Pop out viewport** opens a separate, fully editable viewport window. Selection,
  gizmos, mesh/region RMB menus, camera navigation and editing shortcuts work there.
  The original window keeps the same tabbed authoring panel, the toolbar, and
  a compact timeline. **Dock viewport**, or closing the viewport window, restores
  the embedded layout without changing the document or camera. UI scaling applies
  to both windows; the detached preview requests native pixels at the same preview
  resolution preference. **Play (independent)** still opens its own worker window;
  stopping Play returns to editing in the detached viewport.
  The pop-out is a presentation child of the UI process, with its own context,
  UI renderer and mesh overlay. It shares the existing editing session and preview
  stream, not a second document or worker. GPU objects never cross its context
  boundary; only one editor window waits for VSync per tick.
- In the second toolbar, enable **World bounds**. **Frame world
  bounds** shows the whole cuboid using the private editor camera, leaving the
  animation shot unchanged. Drag a colored **−X/+X/−Y/+Y/−Z/+Z** face-center
  handle to resize only that border; the opposite border stays fixed. Orbit if
  a face's axis points directly into the camera. Release commits one undo step;
  Escape/focus loss cancels. Exact minimum/maximum coordinates are also editable
  in the sidebar's **Scene** tab with **Apply world bounds**. **Save** persists the corners in the scene, not in
  preferences. Old scenes default to −100…100 on each axis. This is an editor
  extent/guide, not a physics wall or a render-clipping volume, and it is not drawn
  in independent Play. Bounds updates send only six floats, never mesh geometry.
  Moving an instance or a formation is not limited by this guide or by the old
  ±100-unit boundary. Position gizmos, numeric edits, animation keys, preview
  packets and scene save/load all use the same ±1,000,000 coordinate safety
  envelope. A group move preserves offsets and remains one undo step.
- Camera controls and bounded numeric inspector settings have ordinary text
  fields alongside their sliders. Type a precise value and press Enter;
  Escape discards that field's draft. Inspector values remain staged until
  **Apply** unless the C++ control explicitly requests live edits. Non-finite
  or out-of-range values show an error instead of silently being clamped.
  Timeline times/durations, XYZ components and keyframe values are also text-editable.
- **Scene cameras** are the simulation's view, separate from the private editor
  camera you navigate with. **Blueprints > Camera +** adds a camera instance at
  the current editor view; the first camera becomes the active one. A camera has
  a position and rotation like any instance (its heading is the look direction),
  plus a **Camera lens** with optical zoom and a focus distance that doubles as
  its orbit pivot and far-plane scale. Several cameras can exist, but only one is
  **active** at any timestamp; **Set active here** keys the switch at the selected
  keyframe and clears the others there, so the simulation cuts between cameras.
  Independent Play and the demos render through the active camera.
  Cameras use the Move, Rotate, Free rotate and **Forward / back** gizmos and draw
  a camera glyph in the preview (body, lens, film reels and the view frustum):
  bright for the active camera, orange when selected. Clicking the body selects
  the camera like any other instance. The **CAMERA** panel above the instance properties offers **Inspect**
  (look through the camera read-only; playback and scrubbing follow it, navigation
  is blocked), **Enter** (start from the camera and roam freely; nothing is authored
  until **Save this camera** writes the editor view into the camera at the selected
  keyframe) and **Back**, which restores the editor view from before the visit.
  The preview header always says which camera the mouse moves: **EDITOR CAMERA**
  for the private view, **SIM CAMERA (inspecting)** or **SIM CAMERA (entered)**
  with the camera's name during a visit, and **SIM CAMERA / independent Play**.
  Camera placement, lens and activeness are ordinary keyframe properties: **Blend**
  interpolates the segment arriving at a key; clear it for a held shot followed by
  a cut. Scenes without cameras keep evaluating the older saved animation shot,
  but adding a camera takes over.
- Choose **Vertices** mode for the mesh. Clicking selects; dragging a vertex does
  not move it. **G/R/S** starts move/rotate/scale following the mouse without a
  held button. **X/Y/Z** toggles a world-axis constraint. **Enter or LMB** confirms;
  **Escape, RMB or losing focus** restores the exact starting geometry. The
  **Mesh gizmo** menu also offers ordinary draggable Move/Rotate/Scale handles
  and the MMB-driven **Free rotate** halo.
  Rotation gizmos also accept **arrow keys**: Left/Up turn positively,
  Right/Down negatively. Hold an arrow for continuous rotation (60 degrees/second
  at sensitivity 1; Shift is ten times finer). With Move selected,
  Left/Right and Up/Down move in camera-relative orthogonal directions; a
  constrained drag keeps its axis. Arrows also work during G/R, including R+X/Y/Z,
  independently of the OS keyboard-repeat delay/rate. Enter/LMB confirms a keyboard gesture; Esc/RMB
  cancels. Ctrl+arrows remain reserved for gizmo cycling; text fields retain
  their normal arrow-key editing.
  Camera navigation remains available during a transform: **Ctrl+MMB drag**
  moves forward/back, **Shift+MMB drag** pans, and MMB orbits (except when MMB
  belongs to Free rotate). Camera movement pauses the gizmo's mouse input;
  resuming does not jump or create a second transform/undo entry.
  The active gizmo opens a bottom-left **Sensitivity** slider and numeric field
  (0.05–4, default 1). It scales subsequent mouse movement and held-arrow speed;
  camera speeds remain separate. Visiting this panel does not move
  or confirm a modal transform. The sensitivity is shared by viewport gizmos
  for the current editor session, not saved in the scene.
  With **Scale** selected, this panel also exposes **Maximum instance scale**
  (default 3), **Maximum axis scale** (20), and **Maximum gesture factor** (1000).
  Enter new limits and click **Apply scale limits**; typing alone changes nothing.
  These session-local limits apply to viewport tools, not saved asset validity.
  Lowering a limit never resizes an existing object. Values up to 1,000,000 are
  supported; a mesh/region gesture's factor is relative to its starting shape,
  while instance limits bound the transform's absolute scale.
  **5 — Whole mesh** needs no component selection and offers Rotate, Scale,
  and Free rotate (no Move). **Ctrl+Left/Right**, with either Ctrl key, cycles
  the available mesh gizmos. **R/S** also starts a mouse-following transform.
  In every mesh-edit mode the top-left **Mesh-centered camera** toggle makes **MMB drag** orbit
  around the displayed mesh center without snapping away existing panning, **Ctrl+MMB drag**
  approach/recede along the line between the camera and displayed mesh center,
  and **Shift+MMB drag** pan at the mesh's depth rather than the camera's orbit
  distance. The reference includes the draft placement and uses the same vertex
  centroid as whole-mesh gizmos. Toggling does not snap the view or edit geometry.
  This is an opt-in session preference for mesh editing (not Walk mode).
  Wheel scrolling keeps its usual behavior. With the
  toggle on, Ctrl-drag is an object-centered dolly even if wheel zoom is enabled.
  Mode 5 also offers **Bake camera transforms...**, with independent **Rotation**
  and **Scale** checkboxes and a **Bake to mesh draft** confirmation. Rotation
  transfers the inverse viewing change relative to the standard mesh view into
  the mesh, then returns camera yaw/pitch to that view. Scale transfers optical
  zoom into uniform mesh scale, then returns zoom to 1. Panning and moving the
  camera closer/farther are never baked. Both transforms keep the mesh centroid
  fixed. Rotation preserves framing exactly; optical zoom versus uniform scale
  preserves center-plane framing but can change perspective on deep objects.
  Baking is one undoable placement edit, without rebuilding vertex buffers or
  writing files. Undo restores mesh placement, not private camera navigation.
  Use **Apply mesh to scene** to publish and **Save on disk** to persist it.
  Whole-mesh gestures update a small blueprint placement matrix, not vertex
  buffers. Undo/cancel restores that matrix. **Apply mesh to scene** publishes
  it to instances; **Save on disk**/mesh export bakes a copy of the positions,
  normals and procedural authoring frame, leaving live geometry unchanged.
  Components rotate/scale around their selected vertex centroid. You can also edit local
  XYZ values. **Weld coincident** moves duplicated corners together, preserving
  per-face colors and every other vertex field. X nudges provide exact small edits.
  Authored normals are retained too: vertex deformation does not currently
  recalculate lighting normals. Instance rotation and scale transform
  those normals correctly without modifying the blueprint.
  Picking is an X-ray vertex tool, not an occlusion-aware surface picker.
  The **Mode** dropdown switches between Objects and Vertices; **Tab** does the
  same after clicking the viewport with the mesh selected (text inputs retain
  normal keyboard focus). Mesh-isolation view starts in Vertices mode.
  Object picking uses authored triangles and a conservative displaced sun-body
  proxy, not its transparent bloom or prominence strands. Neither object nor
  vertex selection requires an extra GPU readback.
- **Undo/Redo** cover mesh edits, accepted inspector actions, and timeline
  key additions/updates/moves/deletions and duration changes in the current
  document. Loading another scene starts a new undo history so an old document
  cannot be restored into the new file's Save target. Animation-camera gestures
  are undoable; inspection-camera navigation is not. Undo/Redo preserve the
  inspection pose, the selected camera mode, and the current isolation view.
- **Face IDs** uses the existing enhanced shader emission/capture path.
  It shows the selected object's source-face identities, isolated before bloom;
  this is deliberately not a diagnostic of the entire composited scene.
- **Save / Ctrl+S** updates the current scene file. An untitled scene first opens
  a filename dialog. **Save As / Ctrl+Shift+S** chooses another file and explicitly
  confirms replacement if it already exists. Cancel or failure retains the
  previous current file and unsaved state. The `.vscene` includes all scene settings,
  cameras, keyframes and embedded `.vmesh`, including edited geometry. The private
  inspection pose and any camera visit are not saved. Opening a scene initializes
  the inspection pose from its saved shot. Older files without camera instances
  still load; their saved animation shot keeps rendering until a camera is added.
- **Open scene...** (Ctrl+O) in the top toolbar browses for a `.vscene` and makes
  the chosen scene the current Save target; `--scene FILE` does the same at
  startup. The browser starts beside the current scene with it preselected, or in
  the source `examples/assets` library for an untitled scene; if the bottom
  filename field names an existing file or folder, it starts there instead.
  Opening replaces the previous file and unsaved state, and the dialog says so
  while the scene is dirty; Escape or **Cancel** keeps everything. Merely editing
  the filename field does not redirect regular Save. **Export new .vmesh** uses that field with a `.vmesh`
  extension, still refuses overwrites, and does not change the scene's Save target.
  Export uses the selected/viewed mesh blueprint, not always the original cube.
  When an effect is selected outside mesh view, the button becomes **Export new
  .veffect**. It saves that instance's evaluated effect settings at the playhead
  (not its placement or animation tracks), using the bottom filename field with
  a `.veffect` extension. It also refuses to overwrite existing files.
  `--mesh FILE` remains a startup option for using a file as the initial mesh of
  an untitled scene; the **Import...** button adds assets to an open scene.

Scene files may also contain an `environment` section with `stars`, `star_seed`,
`exposure`, `bloom_threshold`, and `bloom_strength`. These are authored scene
settings, distinct from the editor's FPS/display preferences. They currently
use file editing rather than the selected-instance inspector. An absent section
preserves the original appearance: no stars, exposure `0.9`, bloom threshold
`6.5`, and no additional global bloom. Visible suns retain their own bloom amount;
the renderer uses the larger of global and visible-sun strengths.

Scene saving stages a complete temporary file in the destination directory,
flushes it, and publishes it atomically. Save As without confirmation cannot
overwrite a file that appears between checking its name and saving. Existing
file permissions are preserved when replacing; final-component symlinks are
rejected. Save is disabled during active drags or a pending worker Apply; unsubmitted
inspector drafts are not silently applied by saving.

Mesh editing changes vertex positions and supports the existing face/edge creation,
subdivision and alignment tools; it does not remesh, edit UVs, or rig characters.
The project retains a built-in mesh blueprint,
one procedural sun blueprint, and imported mesh blueprints, with multiple independent
scene instances. Imported geometry is embedded in `.vscene`, so reopening does
not depend on the original import path still existing.
Deleting an instance removes its scene membership and property tracks, but retains
the blueprint; Undo restores the instance and its animation. Creating from a
blueprint uses a new monotonic ID, not a visibility toggle or an old instance's
tracks. Instance settings are independent; vertex edits intentionally change the
explicitly inspected blueprint (or the selected instance's blueprint in Scene view),
never another mesh. `State::inspected_mesh` identifies the asset;
`mesh_target()` resolves its blueprint ID and optional scene-instance ID.
`editable_mesh()` returns no target in Sun view or Scene without a selected mesh.
Isolated drawing uses blueprint defaults and identity placement, without creating
a placeholder instance; diagnostic provenance names the asset, not a fake entity.
Each mesh blueprint has its own cached GPU storage shared by its instances;
the sun renderer resources are reused.
The format is still a small example scene model, not an arbitrary hierarchy.
Other `.vmesh` fields,
metadata, faces and edges are preserved. Preview shading understands `position`
and optional float RGB/RGBA `color/0`. RGB gets alpha 1 for preview only;
absent or unsupported color encodings display white, with a diagnostic note for
unsupported encodings. The original fields remain intact for export.

## Timeline

Time zero is always the permanent initial keyframe. New projects open there,
but **no keyframe is selected**, so the scene pose starts read-only. Click its
timeline marker or keyframe-list entry to start editing; merely moving the
playhead to zero does not select it.
It cannot be moved, deleted, or unkeyed; batch deletion skips it. Its values
come from explicit zero keys where present, otherwise from the initial scene
properties. This is a logical keyframe: loading an older sparse scene does not
create hundreds of tracks, consume the track budget, or change its playback.

Scene-instance properties, including camera placement, lens and the active
camera, are editable only while
paused **at an explicitly selected keyframe timestamp**. Between keys and after the last key the
pose is read-only: insert a keyframe to edit it. The inspector, transform gizmos,
and editing-session operations enforce the same rule. Editor-camera navigation,
selection, blueprint geometry editing, world bounds and global settings remain
available. Structural operations (such as importing/deleting instances) are
document-wide, not animation keys.

Click/scrub to a timestamp in the bottom timeline, then **Add keyframe**.
This captures the evaluated scene and camera values at the insertion time and
immediately selects the new keyframe, without snapping back to the preceding
keyframe. Splitting a segment retains its linear/hold interpolation, preserving
the existing animation (within floating-point precision). Adding at an existing
timestamp selects it without overwriting anything.
A first insertion after zero also seeds missing tracks with their baseline at
zero. This copies property values, not the clock of time-driven effects.

The **Keyframes** list shows timestamps and custom names. Select there or
click a timeline marker to seek and inspect it. Markers only select: dragging a
marker never moves it. The ruler and empty timeline space are for scrubbing.

In both the list and timeline, **Shift-click** selects the inclusive timestamp
range from the last selection anchor; **Ctrl-click** adds or removes individual
timestamps. A plain click replaces the selection. The list heading shows the
selected count, selected markers are gold, and list entries use `+` for selected
keys and `>` for the active key. The inspector displays the active timestamp;
its pending edits can be applied there or to a range using the toolbar actions.
**Delete** or **Delete selected keys** removes selected nonzero timestamps (across
all properties) in one undoable operation. **Ctrl+Z** restores the entire batch;
**Ctrl+Y** deletes it again. Delete inside a text field still edits its text.

Selection is local UI work, not a scene reload: the panel retains its controls
and refreshes their sampled values. Its snapshot contains only animation and
instance metadata, never blueprint geometry. Repeated selections reuse widget
IDs, including in fleet-sized scenes. UI visibility/enabled changes coalesce
layout work until input, bounds queries or drawing need it; hiding or disabling
a control still cancels focus/capture immediately.

The right **Keyframe values** tab has the selected timestamp, editable name,
and values grouped by object/effect. At nonzero keys it shows only instances
whose evaluated values differ from the preceding keyframe, plus all selected
instances. Each entry initially shows only **blueprint name / instance name**
and a small **+** button; **+ / -** expands or collapses its fields. Selected
identifiers are highlighted. Clicking the **name** selects that instance in the
scene, with Ctrl-click toggling and Shift-click selecting a range, just like the
instance list. Scene selection updates the keyframe entries in the other direction.
The **+ / -** button only changes expansion, not selection. These actions keep
the selected keyframe and any unsent field edits intact. Selected
instances expand automatically and collapse again when deselected. A manual
toggle overrides that default until that instance's selection changes;
manual choices for unrelated entries remain untouched. Collapsing keeps unsent
field edits intact and does not apply them or change the scene. Selecting an
unchanged instance temporarily reveals it; deselecting hides it unless values
differ or an unsaved draft needs attention. Reverting the draft hides an
otherwise unchanged entry. Zero shows the complete initial pose.
**Object** further filters these rows.
**Apply to keyframe** is pinned to the right of the second toolbar. It commits
the inspected keyframe's name, timestamp and pending property edits.
**Apply to keyframes** beside it opens a range menu: enter **From (s)** and
**To (s)**, check the destination count, then **Apply range**. Endpoints are
inclusive and only existing keyframe timestamps are affected. The default range
is the selected keyframe through the last one, or the endpoints of a multi-selection.
Only edited fields are copied, including only the changed components of vectors;
other values and interpolation remain individual to each destination. Changing
only **Blend** preserves each destination's value. Names and timestamps are not
copied; apply pending name/timestamp edits to the single keyframe first.
One **Undo** restores the entire range. Invalid edits change nothing and keep the
menu/draft open; **Cancel**, Escape or an outside click closes the menu without
discarding the draft. Removing keys at time zero is still forbidden.
Once a keyframe is selected, selecting a scene instance keeps the
keyframe and any unsent draft, scrolls to that instance's entry, and focuses its
first Key control. A conflicting object filter is cleared to reveal the entry.
Copy/Delete still follow the last selected kind (instance or keyframe).
Every property has a **Key** switch: checked means a value is stored here
(or belongs to the permanent initial pose at zero);
unchecked shows its evaluated value as read-only context. **Blend** controls
interpolation from that property's previous key to this key (linear for floats
and XYZ; unchecked means hold, then switch). Booleans always switch. At zero
Key and Blend are disabled: the initial pose has no preceding segment.

Edit the name, timestamp or values, then **Apply to keyframe**. Name/timestamp Enter
also applies the complete draft. Retiming moves the entire group, and collisions
are rejected without partial edits. Only changed fields are submitted, so an
initial-pose Apply does not materialize every implicit property. **Delete
keyframe** removes the entire nonzero group.
An intentionally empty group remains selectable until deleted. Names, times,
values and interpolation are saved in the scene and covered by Undo/Redo.

Dragging the ruler or **Playhead** slider scrubs and pauses playback. **Time**
and **Duration** also accept numeric entry followed by Enter. Both Play modes
loop over the duration; scrubbing can inspect the exact endpoint. Reducing
duration past an existing key is rejected, not silently destructive. Timeline
editing is disabled during independent Play and pending inspector callbacks.
With Debug link off, keys can still be authored/saved locally; the image remains
frozen until reconnection.

**Layer** filters timeline markers and list entries to timestamps with keys in
that layer; it never mutes playback. **All layers** also shows empty keyframes.
Both the list and inspector scroll when they run out of room. Music/subtitle
clip playback is not implemented yet; the timeline core remains separate from
these scene-property bindings.

Property edits are applied as one validated timeline transaction, including all
four scene-camera channels. Inserting/applying a keyframe does not copy and
validate the whole timeline once per property. Validation indexes property
identities instead of scanning the full property list for every track.

The UI maintains a clipped paint/hit-test list, caches measurements within each
layout pass, and avoids relayout when fixed/column-width text changes. Offscreen
controls retain editing state and accessibility data, but produce no draw
commands. Visible frames are still composited normally; this is not a dirty-tile
renderer or a change to preview resolution.

The separate **Instance properties** tab displays the evaluated instance at the
playhead. **Apply instance transform** edits overall scale, local-axis proportions and XYZ rotation in
degrees; the separate **Position** Apply/gizmo edits translation. **Apply
appearance** edits brightness, visibility and wireframe. Only changed fields are
applied. At zero an unanimated field updates that instance's initial value. At
any later keyframe it creates/updates a property key, preserving an existing
key's incoming interpolation. Pause playback and select a keyframe first. This
prevents a timeline key from silently masking an accepted scale Apply.
Completed preview images that arrive before their scene acknowledgement are
retained in a one-frame mailbox, then displayed once that revision is accepted.
This avoids losing the only updated image from a paused worker after Apply.

Instance placement is explicitly separate from reusable content:

```cpp
SceneInstance instance;             // identity + blueprint reference
instance.transform.position = {1, 0, 0};
instance.transform.rotation = {0, 45, 0}; // degrees; Rz * Ry * Rx
instance.transform.scale = 0.7F;
// MeshBlueprint owns geometry and appearance defaults, not this transform.
```

Scene rendering uses `T * Rz * Ry * Rx * S` on blueprint-local vertices. The
same transform drives picking, diagnostics and vertex-drag conversion. **Blueprint
mesh** view ignores scene placement and edits the shared local geometry; scene
placement never rewrites vertex bytes or triggers a geometry upload. The **SCENE
INSTANCES** list shows each instance and its blueprint name. Adding another
imported-blueprint instance starts with identity placement, not the selected
instance's transform or animation. Saved editor projects now use version 2 with
separate `transform` and `settings` sections; version 1 files migrate on load.
Animated overrides do not overwrite those base values. Vertices can be edited while paused; their
projection uses the animated transforms at the current playhead. Playhead changes
are not undo entries. See [timeline API and file format](timeline.md).

## Editor settings

Scene annotations are ordinary **Region / TODO volume** blueprint instances,
each with its own boundary. Add them from Blueprints or **Regions / TODO volumes**.
The normal **Gizmo** list offers Move/Rotate/Scale and region vertex/edge/face
tools; the inspector also has Previous/Next vertex controls. Copy/paste, selection
and deletion use the normal instance workflow. See [regions](regions.md).

Open **Settings** in the top toolbar (or start with `--settings`). **Apply & save**
validates all values and writes preferences independently of the scene;
**Cancel** discards changes, while **Defaults** only stages the defaults.

| Setting | Default | Range |
| --- | --- | --- |
| UI scale | 100% | Compact desktop-tool baseline; 75–150% scales text, widgets and hit targets together |
| UI FPS cap | 60 | 0–240; 0 = uncapped |
| Embedded preview FPS cap | 60 | 1–240 |
| Independent Play FPS cap | 0 | 0–240; 0 = uncapped |
| Play debug-preview FPS cap | 10 | Any unsigned whole number; 0 = uncapped |
| Preview resolution | 100% | 25–100%; 100% = native pixels |
| Timeline track limit | 256 | 1–16384; limits new animated object/property tracks |
| Instance limit | 4096 | 1–65536; limits new scene instances |
| VSync (editor and independent Play) | On | On/off; presentation synchronization, independent of FPS caps |

Use **UI − / UI +** in the toolbar for immediate, saved 5% adjustments.
**UI scale %** is also the first field in Settings. Apply & save changes it without
restarting, and restores it on the next launch. It multiplies the window's DPI
scaling: the UI uses scaled logical coordinates, while glyphs and the viewport
remain rendered to native framebuffer pixels. It is independent of preview
resolution and does not modify camera zoom or scene content.
At 100%, the editor's compact density maps 18-unit text to approximately 13
logical pixels and 36-unit controls to 26 pixels. This is a common layout/input
scale, not image downsampling; popup hit targets and detached windows follow it.

Raise **Timeline track limit** (for example to 1024) to copy/paste larger animated
formations or key more properties. It applies immediately after **Apply & save**;
the dialog reports the current track count. A rejected edit reports the required
count and changes nothing. This is an authoring preference, not part of a scene:
lowering it never removes animation, and existing scenes, playback and undo/redo
remain usable even when their track count exceeds the preference. Existing-track
edits and track removal remain available. The engine's separate key budgets
(4096 keys per track, 16384 keys total) and scene/transport byte budgets still apply;
16384 tracks is the structural maximum because each track needs at least one key.
Old preferences files keep the 256-track default.

**Instance limit** applies to import, instantiate and paste. The settings dialog
shows the current instance count, and a rejected addition leaves the scene and
history unchanged. Lowering the preference does not delete objects or block
loading, editing, deletion, or undo/redo in a larger scene. Old settings files
receive the 4096-instance default; version 5 persists both authoring limits and VSync.
The 65536-instance format ceiling is a safety bound, not a performance promise;
the scene/transport byte budgets and timeline budgets still apply independently.

VSync applies immediately to the editor UI and any running independent Play
window. Hidden worker previews stay unsynchronized. It does not affect animation
time, scene revision or resource ownership. Old preferences migrate with VSync
enabled. The engine API and driver limitations are documented in
[window and context](window_and_context.md#vsync).

Display synchronization can additionally limit UI/Play FPS. Independent Play remains
native-resolution even when embedded preview resolution is reduced. Worker caps
apply with Debug link off too; changing settings never rebuilds mesh geometry or
marks the scene unsaved. Settings are resent when the worker is reloaded.

The Play debug-preview rate has no artificial ceiling: 144, 360, and higher
rates are accepted. Zero requests a capture on every independent Play frame;
the actual delivery rate still depends on rendering and communication throughput.
The default remains 10 FPS to keep debug readback overhead modest.

The playback clock ticks between render deadlines, so an intentional low cap
(even 1 FPS) does not slow the animation down. The existing 0.25-second clamp
still protects against actual long stalls; there is no gameplay simulation yet.

Preferences live at `$XDG_CONFIG_HOME/vng/editor.settings`, falling back to
`$HOME/.config/vng/editor.settings`. Invalid files are reported; applying settings
uses a temporary file and atomic replacement. No scene Save is needed.

## Window space

The editor starts maximized, retaining normal desktop borders and window controls.
Its viewport and panels resize with the window. Use **Fullscreen / F11** or F11
for true monitor fullscreen; toggle again to return to the previous windowed mode.
One right sidebar has **Scene**, **Keyframe values**, and **Instance properties**
tabs (**Blueprint geometry** while editing a mesh). Scene holds separate scene-instance,
region-instance and blueprint lists, object tools, region creation, numerical world
bounds and the keyframe list. Every adjacent Scene section has a draggable divider,
including the region-creation controls; each list has independent scrolling.
Each page retains its widgets and scroll position; switching tabs does not edit
the document or clear object selection. Region properties use the same Properties
page, so its tabs remain accessible. Selecting a timeline key opens Keyframe values;
opening a mesh opens Blueprint geometry. Ordinary instance selection stays on the
current page, allowing uninterrupted Shift/Ctrl list selection.

The first toolbar holds **Open scene...**, **Save**, **Save As**, **Import...**,
**Undo/Redo**, **Logs**, **Settings**, reload/playback, pop-out and UI-scale actions.
**Open scene...** browses for a `.vscene` project. The second toolbar holds
**View**, **Camera settings**, **FPS**, **World bounds**, **Frame world bounds**,
**Regions / TODO volumes**, **Region shape**, **Debug link**, **Face IDs**,
**Instance list** and **Blueprint list**.
At narrow widths or larger UI scales, trailing controls move into **More...**
or **Tools...** in their own row, preserving values and widget identities.

**FPS** toggles a small two-line overlay, off initially. **UI FPS** measures
viewport-window presentations, including repaints of the same image. **Preview
ms** is the moving average of the last **20 presented worker frames' durations**,
from worker render start through observed readback completion. It updates from the first
valid frame, using however many samples exist so far. Repainting an old image,
frame-cap waits, and time between editing bursts do not contribute samples.
The last average stays visible with **(idle)** or **(frozen)** when appropriate;
new bursts continue the rolling window. Replacing the worker resets its history.
The overlay follows the popped-out viewport. Preview duration includes CPU work
and asynchronous GPU transfer completion/polling (also native presentation when using independent Play),
not IPC/UI delivery; it is neither a GPU timer nor delivered FPS. The toggle is
local UI state, not scene content.

The editable viewport fills the remaining width, using a matching camera aspect
ratio rather than stretching the image. Independent Play keeps the worker window's
aspect ratio; the sidebar absorbs unused horizontal space. With the viewport popped
out, the tabbed panel fills the controls window above a compact timeline.
The same native window and graphics context survive the transition.
`--windowed` starts at 1600×1000 instead; `--fullscreen` starts in true fullscreen.

## Performance and native Play

**Play (independent)** boots from the current document, including unsent local edits. Its
window belongs to the worker, reuses its context/resources, and renders at its
own framebuffer resolution without the embedded-preview cap (native swap
interval is zero). **Stop (independent)**, or closing that window, returns to the editor
without destroying the worker. A private Play clock leaves authored time intact.
Viewport picking/gizmos are disabled during Play; numeric editing and inspector
controls remain usable with the debug link on.
The native window supports the same orbit/pan/zoom controls, even with the debug
link off. By default it follows the authored camera timeline at the independent
playback time. Navigating in that window switches to a private inspection pose
and temporarily stops following the shot; restarting Play resumes the camera
timeline. Stop restores the editor's chosen view. Connected edits to the base
animation-camera pose also clear the private override. Inspection-camera edits
and toggling the editor's camera mode do not disturb native Play.

**Debug link** is independent of Play:

- On: authored edits and inspector traffic are enabled. Native Play streams an
  aspect-preserving production preview at 10 Hz by default (configurable). Face IDs are an
  embedded-editor mode, not an additional render during Play.
- Off: no periodic image readback, shared-image publication, schemas or telemetry;
  ordinary authored updates are not applied. The UI retains its document and last
  image. Offline **Play (independent)** still sends one explicit initialization snapshot so it
  launches the current document, rather than the last connected version.
- Reconnect: the latest full document synchronizes before live updates resume.
  A small lifecycle channel remains available for Stop, reconnect, errors and
  requested counters. Initial worker promotion still requires one completed image.

Disconnected, idle UI rendering is also throttled to avoid competing with Play;
actual input is processed promptly.

Vertex dragging no longer serializes/parses a whole scene or recreates GPU objects:

1. Absolute position patches use 28 bytes plus 16 per touched vertex. A sample
   one-vertex payload was 44 bytes versus 17,596 for its full 256-vertex scene.
2. Each worker generation has its own acknowledged revision and at most one
   update in flight. New positions coalesce, including return-to-start/final
   edits. Only structural/unknown changes and resynchronization use full snapshots;
   mixed known edits retain their targets in a single document patch.
3. CPU edits touch only selected positions, and projection builds camera/model
   transforms once per pass. Dynamic GPU record ranges retain buffers and VAOs;
   topology/vertex-count changes still re-upload.
4. Completed images advance while authoring is ahead of rendering. Current
   vertex markers remain responsive; camera-lagged overlays and stale picking
   are withheld. The old exact-revision display rule could freeze the image
   throughout a drag. Embedded rendering defaults to a configurable 60 Hz cap.

Object translation uses its own `position` message: an unanimated position is
48 bytes plus the 9-byte message prefix, regardless of mesh size. For an
animated position, only that property's track accompanies the position, so
Escape can restore the exact keys and interpolation. The worker updates the
instance/track and acknowledges the revision; it neither serializes the scene
back to the UI nor uploads mesh geometry. Ordinary native inspector **Apply**
callbacks remain native C++, but return the properties/tracks they changed as a
compact `state_patch`, with no whole-scene serialization or mesh reconciliation.

Pending pointer samples coalesce to the latest position instead of forming a
backlog. An active gesture keeps its original gizmo basis as worker revisions
arrive, and completed images keep advancing even when authoring is ahead.
The keyframe panel is refreshed once the gesture ends, not on every movement.
Undo snapshots are made only on commit, never on pointer movement. Rendering,
readback and IPC still introduce normal frame latency; these changes remove
the mesh-size-dependent encode/decode work from dragging. Worker `stats`
responses expose `position_edits`, `scene_encode_calls`, `scene_decode_calls`
and `mesh_update_calls` to verify this path.

Selection, private camera movement and scrubbing now share a separately sequenced
76-byte absolute `ViewportRequest`, delivered through a latest-value lane without
document acknowledgements. They neither advance the document revision nor force
mixed camera/vertex changes into full snapshots. The locally known instance
position supplies the gizmo immediately; native inspector callbacks require a
matching document and inspection-context stamp. A frame carries its actual camera
and sampled time, so picking does not use the next requested camera target.

Wheel zoom is smoothed on the worker using a time-based log-magnification response;
forward/backward wheel translation uses the same settling time in position space;
orbit, pan and numeric camera edits remain direct. Editing the authored animation
camera uses an independent ordered patch that cannot overwrite private
viewport state. Pose-only packets are 52 bytes; animated edits send only the
five camera tracks, never embedded meshes. Legacy packets/scenes default to zoom 1.
Camera Undo entries are lightweight
too. Normal/diagnostic rendering still evaluate the same timeline,
including the independent window's private clock. See
[editor boundaries](editor_boundaries.md) for the ownership and timing details.

Rotation uses a distinct 48-byte `VNGROT` property packet (plus track keys when
animated), sharing the bounded vector-property codec with position. Repeated
moves coalesce behind one in-flight update; mixed properties/objects retain their
identities in one atomic patch. Worker rotation updates do not parse the whole scene,
rebuild GPU meshes, or upload vertex data. The UI tool and interaction helper
are backend-independent; the existing instance transform drives the renderer.

Scale shares the bounded instance-property codec: a 40-byte `VNGSCL` packet
plus optional scale-track metadata and keys. Both the slider/gizmo and a
scale-only **Apply instance transform** use this path, not the whole-scene
worker callback round trip. Scale history entries contain only the scale
property; undo/redo also stream compact updates. No mesh upload or scene
serialization is needed. A combined scale-and-rotation Apply remains one
transaction and sends only those two properties and their associated tracks.
Known vertex/property/keyframe undo/redo retains the same target metadata;
structural undo still synchronizes the changed document structure.

Runtime `render_frame()` / `present()` are GPU-only; `readback()` is separate and
optional, with explicit counters. A tiny-mesh Debug benchmark measured position
updates improving from roughly 0.0126 ms to 0.0013 ms. GPU-only render/present
averaged about 0.075 ms while render/readback remained around 1.2 ms. These are
local microbenchmarks, not a promised full-scene frame rate. Use a separate
`RelWithDebInfo` or `Release` build for representative CPU performance; Play does
not change compiler optimization settings.

The runtime now retains sampled instance values across camera-only redraws and
searches a prefiltered sun list for lighting. Warm instanced mesh draws reuse VAO
formats and only rebind the instance stream. On the same 911-instance asteroid
scene, the Debug CPU render cost during camera navigation dropped from about
6.3 ms to 0.61 ms (1500×900, seven mesh draws). A matched editor-navigation run
at 1184×676 and a 180 FPS cap improved preview delivery from about 126 to 163
updates/s, with median input-to-presentation submission dropping from 18.5 to
7.5 ms. These are local measurements, not GPU time or monitor scanout latency.
Worker statistics expose `sampled_instances` and `light_candidates` so redundant
CPU work can be tested without relying on timing thresholds.

The maximized editor no longer enlarges a fixed 640×480 image: viewport size and
framebuffer DPI determine the requested render extent. The request is a tiny
`viewport\nWIDTH HEIGHT` packet, independent of scene revisions and mesh uploads.
The caption reports the delivered pixel size. Resize and fullscreen changes
renegotiate without recompiling shaders or restarting the worker.

Repeated unchanged UI setters are no-ops; natural label widths are cached.
UI text geometry uploads once per submission while keeping painter order.
The keyframe list reuses its buttons across ordinary scene revisions.
A focused Debug 160-control update test dropped from ~2779 ms to ~0.037 ms;
this is not an editor-FPS measurement. Byte readback now writes directly into its
returned allocation instead of constructing a temporary pixel array and copying
the full image. The 1500×1100 Debug microbenchmark's readback dropped from about
5.5 ms to 1.0 ms on an RTX 5060. Camera requests are flushed after input handling,
before UI presentation, rather than waiting for the next tick. In the paused
911-instance asteroid scene, 1500×900 preview delivery improved from about 85 to
123 updates/s, with seven mesh draw calls and no camera-triggered geometry uploads.
These are local measurements before asynchronous transfer, not frame-rate guarantees.
Live color preview now uses a bounded three-buffer asynchronous readback queue;
independent Play with Debug link off avoids it entirely.
The sun's intentional softening is
applied before the mesh, with shared depth, so it no longer blurs mesh detail.

## Mesh components and topology tools

Open a mesh blueprint from **View**, then use **Select** in the blueprint tools:

- **1 / Vertices**, **2 / Edges**, **3 / Faces** (click the viewport to focus it before using keyboard shortcuts).
- **4 / Surface** previews the mesh without editor vertex dots, edges, face
  wireframes or selection highlights. The mesh's own rendering is unchanged,
  including any wireframe it deliberately draws. Component selection is inactive;
  use **1/2/3** to edit again. Like the other mode switches, this clears component
  selection but retains temporarily hidden faces and never changes the mesh.
- Click selects; **Shift-click** adds/removes components in selection order.
  Clicking an already selected component keeps the group for dragging.
- **A** selects all in the current mode; **Shift+A** clears the selection.
- **G / R / S** starts a mouse-driven move, rotation or scale; **X / Y / Z**
  constrains the axis. **LMB / Enter** confirms, **RMB / Escape** cancels.
  **Weld coincident** also moves coincident records, preserving split color/normal seams.
- **H** temporarily hides selected faces. In edge mode it hides all faces
  incident to selected edges; in vertex mode, all faces using selected vertices.
  **Alt+H** reveals all hidden faces. Both actions are also in the mesh RMB menu.
  Hidden faces no longer occlude selection; vertices/edges used only by hidden
  faces disappear too, even with X-ray. Loose geometry stays visible.
  This is private mesh-view visibility, not deletion: Apply, Save, scene instances
  and Undo history are unaffected. Changing blueprint or topology clears the mask.
- **F** creates an explicit edge from two vertices, or triangulates the ordered
  polygon boundary from three or more. Select around the perimeter; order controls winding.
- **RMB → Subdivide** inserts one midpoint per selected edge. In vertex mode an
  edge is selected when both endpoints are selected; face mode splits all edges
  of selected faces. Incident unselected faces also split shared edges to avoid
  T-junctions. One fully selected triangle becomes four triangles.
  Its **Subdivide** options panel opens at the viewport's bottom-left. Set
  **Subdivision levels** (1–4), then **Update operation** to refine the same
  original selection; lowering the level removes the extra subdivisions.
  This is one adjustable operation, not another subdivision on each click.
- **RMB → Align to line** projects the remaining vertices onto
  the infinite line through the first two selected vertices. Anchors are not moved.
  Their vertex IDs appear beside the selection count. Coincident anchors produce
  an error, not an arbitrary direction. Its bottom-left options panel offers
  **Strength** from 0 (original positions) to 1 (fully aligned).

Mesh/region right-click menus stay at the viewport's bottom-left. They capture
input without hiding mesh edges or vertex markers; the menu is drawn above those
overlays. The same placement works in the popped-out viewport.

Active **G/R/S** transforms for instances, mesh components, and region components
use the same tool-options host, offering axis constraints and **Confirm transform** /
**Cancel transform**. The ship's **F** forward/back tool offers confirmation and
cancellation without unrelated XYZ controls. Keyboard shortcuts still work;
clicking a menu control does not move or accidentally confirm the selection.

Completed mesh topology operations have **Undo operation** in their options.
Subdivision/alignment changes always recalculate from the captured original mesh
and remain a single Undo entry. A later document edit, Undo/Redo, or leaving the
mesh's editing context closes stale options. **Close tool options** only dismisses
the panel; it does not undo the result. These operations still edit the mesh
draft, not the published scene blueprint.

Selection is UI-local and ordered, not an authored property. Mode changes clear
selection. Vertex/edge picking and overlays hide occluded components by default;
**X-ray selection** explicitly enables through-surface picking and overlays.
Vertex mode shows screen-sized dark dots with a contrasting rim; selected
vertices are larger and orange. Dot size follows UI scaling, not camera distance.
Face picking chooses the nearest projected triangle. Faces remain triangles in `.vmesh`, not
persistent n-gons. Topology changes clear stale component IDs; Undo/Redo restore
geometry and all fields, not historical multi-selection.

Mesh operations edit an undoable **draft**, not the blueprint used by scene
instances. Switching back to Scene does not publish it. Returning to the mesh
view restores the draft, including topology and all vertex fields.

- **Apply mesh to scene** publishes the draft to its blueprint, updating every
  instance of that blueprint in memory. It does not write a file. Undo restores
  both the previous published mesh and the unapplied draft.
- **Save on disk** uses the project Save/Save As workflow. `.vscene` stores the
  applied meshes and `mesh_drafts` separately, so saving and reopening preserves
  work without publishing it. Apply followed by Save persists the published
  result. Existing **Save / Ctrl+S** has the same disk-only meaning.
- **Export new .vmesh** exports the inspected draft without applying it. It
  remains an explicit new-file export, not an overwrite of the imported source.

`Document::mesh_drafts` owns working copies by blueprint ID. `mesh_geometry`
and `instance_mesh` always return published geometry; `mesh_edit_geometry`
resolves the working copy, and `mesh_view_geometry` chooses it only in its
isolated mesh view. Draft creation is lazy on the first actual modification.
`begin_mesh_draft` / `apply_mesh_draft` / `discard_mesh_draft` do not increment
revisions or own history; their callers publish a structural change explicitly.
View-only camera updates still never serialize or copy mesh data. Switching
between an existing draft and its published mesh refreshes the worker's GPU
realization once; hidden draft patches never touch scene GPU buffers.

Align/move stream touched vertex patches; fill/subdivide use structural updates.
The first draft edit and Apply also use snapshots to establish/change ownership;
subsequent position edits retain the compact patch path.
Save and mesh export retain new faces and explicit loose edges. Subdivision
averages floating-point vertex fields and copies integral/categorical fields
from the lower-ID endpoint. It does not merge seam vertices or smooth the surface.
Malformed selections/boundaries and edits exceeding mesh limits fail atomically.

The backend-neutral API is `EditableMesh::edges()`, `fill(ordered_vertices)`,
`subdivide(edges)`, and `align_to_line(ordered_vertices)` in
`include/vng/editor/mesh.hpp`. UI selection/picking/menu lives separately
in `examples/editor/mesh_tools.*`; no widget or backend types enter the mesh API.
`mesh_overlay.*` is its OpenGL realization: resident position/index buffers,
a local depth-only pass, then batched edges/markers/highlights. Camera navigation
uploads no geometry, and position changes leave topology resident. It uses the
current local mesh rather than waiting for worker depth readback. No per-edge UI
widgets/draw commands are built, and no topology is silently dropped to satisfy
the UI command budget. Blueprint inspection uses a camera-following light so
underside colors remain readable; scene lighting is unchanged.

## Effect authors write ordinary C++

`examples/editor/effects.cpp` owns descriptions and native callbacks:

```cpp
void ProjectControls::describe_editor(vng::editor::Inspector& ui) {
    const auto id = state_.viewport.selected_object;
    const auto* instance = find_instance(state_, id);
    if (!instance) return;
    const auto evaluated = evaluate_instance(state_, *instance, state_.viewport.time);
    const auto* sun = std::get_if<SunSettings>(&evaluated.settings);
    if (!sun) return;
    auto edit = ui.edit("surface", *sun);
    edit.slider("radius", &SunSettings::radius, .1F, 4.F);
    edit.slider("bloom", &SunSettings::bloom, 0.F, 1.F);
    edit.toggle("white_spots", &SunSettings::white_spots);
    edit.apply("Apply surface", [this, id, before = *sun](const SunSettings& next) mutable {
        auto result = apply_appearance(state_, changes_, id, before, next);
        if (result) before = next;
        return result;
    });
}
```

Only owning field descriptions, values, stable keys, object identity, generation
and revision cross the process boundary. Callbacks, object pointers, C++ layouts,
GPU handles and lambdas never cross it. An inspector copies the baseline settings;
rebuild it when external state changes. Keep it alive through a live gizmo gesture.
Successful dispatch advances its revision. Callbacks may return `void` or an
`std::expected<void, Error>`; user callbacks must apply their own changes
transactionally when they can fail. The example's `apply_appearance` helper stages
one instance and its animation values, validates the changes, and reports exact
property targets to `ProjectControls::take_changes()`. The worker packages only
those targets into its reply; it does not infer a geometry change because a
callback ran. New project callbacks must report their committed changes too.

The generic adapter in `examples/editor/inspector_panel.cpp` knows only schemas,
not effect types. Backend-neutral descriptions support bool, signed/unsigned int,
float, Vec3, text, groups, actions, live edits and translation-gizmo events.
Additional custom widgets require extending the description/adapter vocabulary;
arbitrary UI-side C++ widgets are not dynamically injected by a worker.

Change `effects.cpp`, `runtime.cpp`, or the existing sun C++ sources in VS Code,
then press **Reload C++**. The UI asynchronously builds `vng_editor_worker`, copies
the resulting executable into a unique generation directory and starts it with
the latest authored snapshot. No dynamic-library unloading or ABI compatibility
between old and new C++ objects is needed. The new worker replaces the old one
only after readiness and a completed image. Changing the project's serialized
state schema still requires a compatible decoder/migration and rebuilding the UI.

Compile errors, failed startup and crashes leave the UI, undo history and last
completed image intact. **Build log** opens the bounded build/worker output in
the UI; it is also available from `PreviewSession::logs()`.
Worker errors are displayed and printed to stderr. A hung inspector callback
times out in the UI so it can be replaced through Reload. This executes trusted
project code: process isolation is crash containment, not a security sandbox.

## Ownership and dependencies

```text
vng_editor ──────────────> vng_content / vng_gfx / vng_core
  Inspector, schema/event codecs, EditableMesh

vng_editor_preview ──────> vng_core + Linux/POSIX threads
  process supervision, control channel, shared image transport

vng_timeline ───────────> vng_core
  typed keyframes, incoming interpolation, validated sampling

vng_editor_project ─────> vng_editor + vng_timeline
  EditingSession, authored State, scene animation bridge, persistence, CPU picking

vng_editor_demo ────────> project + preview + UI OpenGL + GLFW
  owns EditingSession, inspector widgets, preview synchronization, embedded image

vng_editor_worker ──────> project + preview + editor runtime
  native callbacks, existing sun renderer, DSL mesh shader, OpenGL
```

The [editing-session API](editing_session.md) is the authoring entry point. It
owns history/revisions/savepoints and emits precise changes; UI tools and preview
transport do not own those policies.

`vng_ui` owns no GPU objects. Its `ImageView` accepts an immutable, top-down RGBA8
sRGB snapshot; the OpenGL UI renderer handles texture caching and color conversion.
The worker sends viewport-sized frames through a bounded latest-frame shared-memory
slot. The editor reserves capacity up to 4096×4096 (lazily backed); capacity is
not the rendering resolution. Larger viewports fit inside that limit.
Neither process waits for the other's robust process-shared mutex. The worker's
`try_publish()` returns false if the UI is reading; its one-image mailbox keeps
the latest completed result for retry, even when no more edits arrive.
It takes ownership of the completed pixel vector instead of copying it a second
time into the UI image. `latest_frame()` remains available for inspecting clients;
`take_latest_frame()` consumes that snapshot, leaving display retention to its caller.
Live color rendering queues GPU readback into persistently mapped staging buffers.
The worker polls fences with zero timeout and copies only the newest completed
frame. Each queued frame retains the scene/view/camera metadata from its own
render; late completion cannot relabel old pixels as a newer edit. A full queue
skips work, and resize/disconnect/Play switches invalidate pending results.
Diagnostic/evidence captures and explicit screenshot readback remain synchronous.
This transport still performs CPU image copies and a UI texture upload; it is
not zero-copy GPU sharing. A later GPU-sharing transport can replace that boundary.

IPC uses bounded, nonblocking Unix messages (32 MiB logical message, fragmented;
64 MiB outgoing queue). Schema/event codecs are deterministic and versioned.
The project reserves envelope space within that scene limit. Individual meshes
accept at most 65,536 vertices, 16 MiB source text and 16 MiB decoded data;
the bundled spaceship fits these limits. Imported-asset vertex patches carry
their blueprint identity, so updates and rollback address the correct GPU buffer.
Scene revisions reject stale edits; worker generations reject events
from obsolete code. Rendering and callbacks happen between frames. Dirty edits
coalesce for current and initializing workers with separate baselines. Displayed
document revisions never regress; view identity is checked separately, while
continuous vertex edits allow lagged completed frames. Paused clean workers
redraw only while a wheel-zoom target is settling; disconnected non-playing
workers do not redraw.

## Validation

```sh
ctest --test-dir build -R 'vng_editor_' --output-on-failure
```

Core tests cover schema/event validation, local callbacks, geometry preservation,
roundtrips, editing, history, atomic replacement and exclusive saving. Save-dialog
tests cover first save, replacement confirmation, cancellation, and failed writes;
shortcut tests protect Ctrl+S/Ctrl+Shift+S from repeat and focus problems. Transport tests use self-exec
workers for failure, timeout and handover paths. UI tests cover generic control
mapping, invalid input and draft preservation. The real worker test renders the
sun and mesh, applies controls, uploads edited vertices, captures diagnostics and
reloads a generation without losing authored state. GL-unavailable environments
skip the real rendering test explicitly. Additional regressions cover compact
patches, coalescing/replay, buffer/VAO reuse, actual native backbuffer pixels,
offline Play with zero readbacks/streaming, reconnect, and native close/reopen.
Navigation tests cover captured gestures, UI focus/Tab routing, camera limits,
saved pivots, exact pixel-aligned vertex dragging, overlapping/hidden object
picking, and selection clearing. The real worker additionally verifies compact
camera changes alter rendered pixels without touching GPU mesh storage.

### Native end-to-end UI walkthrough

```sh
cmake --build build --target vng_editor_e2e_tests -j4
ctest --test-dir build -R '^vng_editor_e2e_tests$' --output-on-failure
# Actual 105-instance asteroid fleet: rapid, frame-exact keyframe selections:
ctest --test-dir build -R '^vng_editor_fleet_e2e_tests$' --output-on-failure
# Choose where a new, uniquely named run directory is written:
./build/vng_editor_e2e_tests --artifacts /tmp/vng-editor-e2e
```

This runs the same editor application loop and separate OpenGL worker as the
interactive executable. A test-only input driver finds controls by semantic
role/label and sends pointer, key and text events through normal UI handling.
It never invokes import, save, selection, or widget callbacks directly to make
an action pass. Input is injected at the engine's window-input boundary, not
through operating-system mouse events: this exercises the editor but does not
test GLFW's native event translation. No browser or Playwright dependency is
needed for this C++ application.

The UI window is hidden and fixed-size. Each run uses private scene files and
preferences, an in-memory clipboard, and already-built worker binaries. It
does not operate on the user's running editor, move the desktop pointer, or
overwrite authored assets. A missing graphics context is an explicit CTest
skip, not a reported visual pass.

`ui::Screen::inspect()` supplies owned, backend-neutral widget snapshots:
roles, authored labels, current values, hierarchy, visibility, enabled state,
logical bounds/clips, and expanded dropdown options. It exposes no mutation
backdoor. The walkthrough combines this layout evidence with actual composed
RGBA screenshots, worker preview pixels and read-only document observations.
Screenshots are review artifacts, not proof by themselves; assertions must also
check the intended state transition and relevant rendered output. Default
artifacts are retained under `build/editor-e2e/`, including failed runs. Open a
run's `index.html` for the screenshot gallery, or inspect `report.json`, the
per-checkpoint `.ui.txt` snapshots and `worker.log` directly.

The walkthrough covers viewport selection, invalid and successful spaceship
import, nested scrollbar dragging/wheel bubbling, live gizmo movement and rotation,
cancellation, Undo/Redo, Save As/current-file Save,
Load, independent editor-camera changes, scene scale/rotation Apply, animated
scale Apply, two instances sharing one unchanged blueprint, keyframe addition/deletion, enhanced
Face IDs and restoration of normal output, settings, and deleting an instance
without deleting its blueprint. Import regressions open the top View menu from
Scene with the ship selected, pick the cube directly, edit its vertices, and
verify the ship's geometry remains unchanged. They capture the menu with both
named meshes, verify import Undo/Redo removes/restores its entry, and check
the menu after loading. Retained
blueprints are rendered and edited again after their last instance is deleted.
The walkthrough also exercises debug-preview rates of 144 and 0 (unlimited),
and checks that blueprint tools cannot delete a hidden scene selection.
Same-input-frame assertions catch selection
and drag feedback delays; image assertions check visible geometry, gizmo
pixels, movement and restoration. The smaller unit/worker suites still cover
protocol faults and other edge cases that are impractical to drive visually.
This is not a claim of exhaustive visual correctness: screenshot galleries
remain useful for manual review, and new editor features need new scenarios.

`ctest --test-dir build -R vng_editor_multiselect_e2e_tests --output-on-failure`
runs the dedicated multi-selection walkthrough: full/partial/add/subtract boxes,
instance range/toggle selection, live group translation, group clipboard/delete
and Undo, keyframe range/toggle/batch delete, and mesh vertex selection. It emits
the same screenshot gallery and semantic UI snapshots as the main walkthrough.
